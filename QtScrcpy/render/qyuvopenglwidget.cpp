#include "qyuvopenglwidget.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>

// Shader Vertex & Fragment
static const char *vertShader = R"(#version 450 core
layout(location = 0) in vec3 vertexIn;
layout(location = 1) in vec2 textureIn;
out vec2 textureOut;
void main(void) {
    gl_Position = vec4(vertexIn, 1.0);
    textureOut = textureIn;
}
)";

static const char *fragShader = R"(#version 450 core
in vec2 textureOut;
out vec4 FragColor;
layout(binding = 0) uniform sampler2D tex_y;
layout(binding = 1) uniform sampler2D tex_u;
layout(binding = 2) uniform sampler2D tex_v;
const mat3 yuv2rgb = mat3(1.164, 1.164, 1.164, 0.0, -0.213, 2.112, 1.793, -0.533, 0.0);
const vec3 rgbOffset = vec3(0.9729, -0.30148, 1.1334);
void main(void) {
    vec3 yuv;
    yuv.x = texture(tex_y, textureOut).r;
    yuv.y = texture(tex_u, textureOut).r;
    yuv.z = texture(tex_v, textureOut).r;
    FragColor = vec4(yuv2rgb * yuv - rgbOffset, 1.0);
}
)";

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent) {
    QSurfaceFormat format;
    format.setVersion(4, 5);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(0); 
    setFormat(format);
    
    connect(this, &QYuvOpenGLWidget::requestUpdateTextures, this, [this](int w, int h, int sY, int sU, int sV){
        if (isValid()) {
            makeCurrent();
            initPBOs(h, sY, sU, sV);
            
            if (m_textures[0] != 0) glDeleteTextures(3, m_textures.data());
            glCreateTextures(GL_TEXTURE_2D, 3, m_textures.data());
            
            int widths[3] = {w, w/2, w/2};
            int heights[3] = {h, h/2, h/2};
            
            for(int i=0; i<3; i++) {
                glTextureParameteri(m_textures[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTextureParameteri(m_textures[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTextureStorage2D(m_textures[i], 1, GL_R8, widths[i], heights[i]);
            }

            setFrameSize(QSize(w, h));
            m_needInit = false; 
            update();
        }
    }, Qt::QueuedConnection);
}

QYuvOpenGLWidget::~QYuvOpenGLWidget() {
    makeCurrent();
    freePBOs();
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_textures[0]) glDeleteTextures(3, m_textures.data());
    doneCurrent();
}

QSize QYuvOpenGLWidget::minimumSizeHint() const { return QSize(50, 50); }
QSize QYuvOpenGLWidget::sizeHint() const { return m_frameSize; }
const QSize &QYuvOpenGLWidget::frameSize() { return m_frameSize; }
void QYuvOpenGLWidget::setFrameSize(const QSize &frameSize) {
    if (m_frameSize != frameSize) {
        m_frameSize = frameSize;
        updateGeometry();
    }
}

void QYuvOpenGLWidget::initPBOs(int height, int sY, int sU, int sV) {
    std::lock_guard<std::mutex> lock(m_initMutex);
    freePBOs();

    m_strides[0] = sY; m_strides[1] = sU; m_strides[2] = sV;
    int dataSizes[3] = { sY * height, sU * ((height+1)/2), sV * ((height+1)/2) };
    
    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    for(int i=0; i<PBO_COUNT; i++) {
        glCreateBuffers(3, m_pbos[i].data());
        for(int plane=0; plane<3; plane++) {
            glNamedBufferStorage(m_pbos[i][plane], dataSizes[plane], nullptr, flags);
            m_pboMapped[i][plane] = glMapNamedBufferRange(m_pbos[i][plane], 0, dataSizes[plane], flags);
        }
    }
    m_pboReady = true;
    m_renderIndex = 0;
    m_readyIndex = -1;
    qInfo() << "[QYUV] PBO Mailbox Initialized (3 Buffers).";
}

void QYuvOpenGLWidget::freePBOs() {
    m_pboReady = false;
    for(int i=0; i<PBO_COUNT; i++) {
        for(int plane=0; plane<3; plane++) {
            if(m_pbos[i][plane]) {
                glUnmapNamedBuffer(m_pbos[i][plane]);
                glDeleteBuffers(1, &m_pbos[i][plane]);
                m_pbos[i][plane] = 0;
                m_pboMapped[i][plane] = nullptr;
            }
        }
    }
}

void QYuvOpenGLWidget::setFrameData(int width, int height, 
                                   std::span<const uint8_t> dataY,
                                   std::span<const uint8_t> dataU,
                                   std::span<const uint8_t> dataV,
                                   int linesizeY, int linesizeU, int linesizeV) 
{
    if (width != m_frameSize.width() || height != m_frameSize.height() || 
        linesizeY != m_strides[0] || m_needInit) {
        if (!m_needInit) {
            m_needInit = true;
            emit requestUpdateTextures(width, height, linesizeY, linesizeU, linesizeV);
        }
        return;
    }

    if (!m_pboReady) return;

    static QElapsedTimer stallTimer;
    if (!stallTimer.isValid()) stallTimer.start();
    qint64 delta = stallTimer.restart();
    if (delta > 35) {
        qWarning() << "[Decoder] Inter-frame gap:" << delta << "ms";
    }

    int currentRender = m_renderIndex.load(std::memory_order_acquire);
    int currentReady = m_readyIndex.load(std::memory_order_acquire);
    int writeTarget = -1;

    for (int i = 0; i < PBO_COUNT; i++) {
        if (i != currentRender && i != currentReady) {
            writeTarget = i;
            break;
        }
    }

    if (writeTarget == -1) {
        writeTarget = (currentRender + 1) % PBO_COUNT;
    }

    {
        std::lock_guard<std::mutex> lock(m_initMutex); 
        if (!m_pboReady) return;

        const uint8_t* srcs[3] = {dataY.data(), dataU.data(), dataV.data()};
        int h_plane[3] = {height, (height+1)/2, (height+1)/2};
        
        for(int i=0; i<3; i++) {
            if (m_pboMapped[writeTarget][i]) {
                memcpy(m_pboMapped[writeTarget][i], srcs[i], m_strides[i] * h_plane[i]);
            }
        }
    }
    
    m_readyIndex.store(writeTarget, std::memory_order_release);

    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void QYuvOpenGLWidget::initializeGL() {
    initializeOpenGLFunctions();
    
    static const float coords[] = { 
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 0.0f, 1.0f, 0.0f 
    };
    
    glCreateVertexArrays(1, &m_vao);
    glCreateBuffers(1, &m_vbo);
    glNamedBufferStorage(m_vbo, sizeof(coords), coords, 0);
    
    glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, 5 * sizeof(float));
    glEnableVertexArrayAttrib(m_vao, 0);
    glVertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_vao, 0, 0);
    glEnableVertexArrayAttrib(m_vao, 1);
    glVertexArrayAttribFormat(m_vao, 1, 2, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
    glVertexArrayAttribBinding(m_vao, 1, 0);

    m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader);
    m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShader);
    m_program.link();
    
    m_program.bind();
    m_program.setUniformValue("tex_y", 0);
    m_program.setUniformValue("tex_u", 1);
    m_program.setUniformValue("tex_v", 2);
    m_program.release();
}

void QYuvOpenGLWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void QYuvOpenGLWidget::paintGL() {
    if (!m_pboReady) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    int freshFrame = m_readyIndex.exchange(-1, std::memory_order_acq_rel);

    if (freshFrame != -1) {
        m_renderIndex.store(freshFrame, std::memory_order_release);
    }

    int drawIdx = m_renderIndex.load(std::memory_order_acquire);

    m_program.bind();
    glBindVertexArray(m_vao);

    int w = m_frameSize.width();
    int h = m_frameSize.height();
    int widths[3] = {w, w/2, w/2};
    int heights[3] = {h, h/2, h/2};

    for(int i=0; i<3; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[drawIdx][i]);
        
        glPixelStorei(GL_UNPACK_ROW_LENGTH, m_strides[i]);
        glTextureSubImage2D(m_textures[i], 0, 0, 0, widths[i], heights[i], GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }
    
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    m_program.release();
}