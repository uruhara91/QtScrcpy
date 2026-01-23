#include "qyuvopenglwidget.h"
#include <QSurfaceFormat>
#include <cstring>

// Shader Vertex
static const char *vertShader = R"(#version 450 core
layout(location = 0) in vec3 vertexIn;
layout(location = 1) in vec2 textureIn;
out vec2 textureOut;
void main(void) {
    gl_Position = vec4(vertexIn, 1.0);
    textureOut = textureIn;
}
)";

// Shader Fragment
static const char *fragShader = R"(#version 450 core
in vec2 textureOut;
out vec4 FragColor;

layout(binding = 0) uniform sampler2D tex_y;
layout(binding = 1) uniform sampler2D tex_u;
layout(binding = 2) uniform sampler2D tex_v;

const vec3 offset = vec3(16.0/255.0, 128.0/255.0, 128.0/255.0);
const mat3 yuv2rgb = mat3(
    1.164,  1.164,  1.164,
    0.0,   -0.391,  2.018,
    1.596, -0.813,  0.0
);

void main(void) {
    vec3 yuv;
    yuv.x = texture(tex_y, textureOut).r;
    yuv.y = texture(tex_u, textureOut).r;
    yuv.z = texture(tex_v, textureOut).r;

    yuv -= offset;
    FragColor = vec4(yuv2rgb * yuv, 1.0);
}
)";

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent) {
    QSurfaceFormat format;
    format.setVersion(4, 5);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(0);
    format.setRedBufferSize(8);
    format.setGreenBufferSize(8);
    format.setBlueBufferSize(8);
    format.setAlphaBufferSize(0);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(0);
    
    setFormat(format);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

    connect(this, &QYuvOpenGLWidget::requestUpdateTextures, this, 
        [this](int w, int h, int strideY, int strideU, int strideV){
        if (isValid()) {
            makeCurrent();
            setFrameSize(QSize(w, h));
            initPBOs(h, strideY, strideU, strideV); 
            initTextures(w, h);
            m_textureSizeMismatch = false;
            doneCurrent();
            update();
        } else {
            m_textureSizeMismatch = false; 
        }
    }, Qt::QueuedConnection);
}

QYuvOpenGLWidget::~QYuvOpenGLWidget() {
    if (isValid()) {
        makeCurrent();
        deInitTextures();
        deInitPBOs();
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        doneCurrent();
    }
}

QSize QYuvOpenGLWidget::minimumSizeHint() const { return QSize(640, 360); }
QSize QYuvOpenGLWidget::sizeHint() const { return m_frameSize.isValid() ? m_frameSize : QSize(1280, 720); }

void QYuvOpenGLWidget::setFrameSize(const QSize &frameSize) {
    if (m_frameSize != frameSize) {
        m_frameSize = frameSize;
        updateGeometry();
        m_pboSizeValid = false;
    }
}

void QYuvOpenGLWidget::setFrameData(int width, int height, 
                                   std::span<const uint8_t> dataY, 
                                   std::span<const uint8_t> dataU, 
                                   std::span<const uint8_t> dataV, 
                                   int linesizeY, int linesizeU, int linesizeV)
{
    bool sizeChanged = (width != m_frameSize.width() || height != m_frameSize.height());

    if (sizeChanged || 
        linesizeY != m_pboStrides[0] || 
        linesizeU != m_pboStrides[1] || 
        linesizeV != m_pboStrides[2] || 
        !m_pboSizeValid) [[unlikely]] 
    {
        if (!m_textureSizeMismatch.exchange(true)) {
            emit requestUpdateTextures(width, height, linesizeY, linesizeU, linesizeV);
        }
        return;
    }

    if (m_textureSizeMismatch) return;

    int nextWriteIndex = (m_writeIndex.load(std::memory_order_relaxed) + 1) % PBO_COUNT;
    
    {
        std::lock_guard<std::mutex> lock(m_pboLock);
        if (!m_pboSizeValid) return;

        const int heights[3] = { height, (height + 1) / 2, (height + 1) / 2 };
        const std::span<const uint8_t> srcData[3] = { dataY, dataU, dataV };

        for (int i = 0; i < 3; i++) {
            auto dstPtr = m_pboMappedPtrs[nextWriteIndex][i];
            std::memcpy(dstPtr, srcData[i].data(), static_cast<size_t>(m_pboStrides[i]) * heights[i]);
        }
    }

    m_writeIndex.store(nextWriteIndex, std::memory_order_release);

    if (!m_updatePending.test_and_set(std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

void QYuvOpenGLWidget::initializeGL() {
    if (!initializeOpenGLFunctions()) {
        qWarning() << "Failed to initialize OpenGL Functions";
        return;
    }

    m_isInitialized = true;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);

    initShader();

    static const float coordinate[] = {
        -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
         1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,
         1.0f,  1.0f, 0.0f,   1.0f, 0.0f
    };
    
    glCreateVertexArrays(1, &m_vao);
    glCreateBuffers(1, &m_vbo);
    glNamedBufferStorage(m_vbo, sizeof(coordinate), coordinate, 0);

    glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, 5 * sizeof(float));

    glEnableVertexArrayAttrib(m_vao, 0);
    glVertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_vao, 0, 0);

    glEnableVertexArrayAttrib(m_vao, 1);
    glVertexArrayAttribFormat(m_vao, 1, 2, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
    glVertexArrayAttribBinding(m_vao, 1, 0);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader() {
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader))
        qWarning() << "Vertex shader compile error";
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShader))
        qWarning() << "Fragment shader compile error";
        
    m_program.link();
    
    GLuint progId = m_program.programId();
    glProgramUniform1i(progId, glGetUniformLocation(progId, "tex_y"), 0);
    glProgramUniform1i(progId, glGetUniformLocation(progId, "tex_u"), 1);
    glProgramUniform1i(progId, glGetUniformLocation(progId, "tex_v"), 2);
}

void QYuvOpenGLWidget::initTextures(int width, int height) {
    if (!m_isInitialized) return;

    if (m_textures[0] != 0) {
        glDeleteTextures(3, m_textures.data());
    }

    glCreateTextures(GL_TEXTURE_2D, 3, m_textures.data());

    int widths[3] = {width, width / 2, width / 2};
    int heights[3] = {height, height / 2, height / 2};

    for (int i = 0; i < 3; i++) {
        glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        glTextureParameteri(m_textures[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_textures[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        
        glTextureStorage2D(m_textures[i], 1, GL_R8, widths[i], heights[i]);
    }
}

void QYuvOpenGLWidget::initPBOs(int height, int strideY, int strideU, int strideV) {
    deInitPBOs();
    std::lock_guard<std::mutex> lock(m_pboLock);

    m_pboStrides = {strideY, strideU, strideV};

    std::array<int, 3> sizes = {
        strideY * height,           
        strideU * ((height + 1) / 2),
        strideV * ((height + 1) / 2)
    };

    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    for (int set = 0; set < PBO_COUNT; set++) {
        glCreateBuffers(3, m_pbos[set].data());

        for (int plane = 0; plane < 3; plane++) {
            glNamedBufferStorage(m_pbos[set][plane], sizes[plane], nullptr, flags);
            m_pboMappedPtrs[set][plane] = static_cast<std::byte*>(
                glMapNamedBufferRange(m_pbos[set][plane], 0, sizes[plane], flags)
            );
        }
    }
    m_pboSizeValid = true;
    
    m_writeIndex = 0;
    m_readIndex = 0;
}

void QYuvOpenGLWidget::deInitPBOs() {
    if (!m_isInitialized) return;
    std::lock_guard<std::mutex> lock(m_pboLock);
    m_pboSizeValid = false;

    for (int set = 0; set < PBO_COUNT; set++) {
        for (int plane = 0; plane < 3; plane++) {
            if (m_pbos[set][plane] != 0) {
                glUnmapNamedBuffer(m_pbos[set][plane]);
                glDeleteBuffers(1, &m_pbos[set][plane]);
                m_pbos[set][plane] = 0;
                m_pboMappedPtrs[set][plane] = nullptr;
            }
        }
    }
}

void QYuvOpenGLWidget::deInitTextures() {
    if (!m_isInitialized) return;
    if (m_textures[0] != 0) {
        glDeleteTextures(3, m_textures.data());
        m_textures.fill(0);
    }
}

void QYuvOpenGLWidget::resizeGL(int width, int height) {
    glViewport(0, 0, width, height);
}

void QYuvOpenGLWidget::paintGL() {
    m_updatePending.clear(std::memory_order_release);
    
    if (!m_pboSizeValid) return;

    int currentWriteIndex = m_writeIndex.load(std::memory_order_acquire);
    
    m_readIndex.store(currentWriteIndex, std::memory_order_relaxed);

    int drawIndex = currentWriteIndex;

    m_program.bind();
    glBindVertexArray(m_vao);

    for (int i = 0; i < 3; i++) {
        glBindTextureUnit(i, m_textures[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[drawIndex][i]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, m_pboStrides[i]); 
        
        int w = (i == 0) ? m_frameSize.width() : m_frameSize.width() / 2;
        int h = (i == 0) ? m_frameSize.height() : m_frameSize.height() / 2;
        
        glTextureSubImage2D(m_textures[i], 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }
    
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    m_program.release();
}