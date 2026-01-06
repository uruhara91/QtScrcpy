#include "qyuvopenglwidget.h"
#include <QDebug>
#include <QOpenGLFunctions>

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

// Binding
layout(binding = 0) uniform sampler2D tex_y;
layout(binding = 1) uniform sampler2D tex_u;
layout(binding = 2) uniform sampler2D tex_v;

void main(void) {
    vec3 yuv;
    vec3 rgb;
    
    yuv.x = texture(tex_y, textureOut).r - 0.0627;
    yuv.y = texture(tex_u, textureOut).r - 0.5;
    yuv.z = texture(tex_v, textureOut).r - 0.5;
    
    rgb = mat3(
        1.164,  1.164,  1.164,
        0.0,   -0.391,  2.018,
        1.596, -0.813,  0.0
    ) * yuv;
    
    FragColor = vec4(rgb, 1.0);
}
)";

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent) {}

QYuvOpenGLWidget::~QYuvOpenGLWidget() {
    makeCurrent();
    deInitTextures();
    deInitPBOs();
    if (m_vao.isCreated()) m_vao.destroy();
    if (m_vbo.isCreated()) m_vbo.destroy();
    doneCurrent();
}   

QSize QYuvOpenGLWidget::minimumSizeHint() const { return QSize(50, 50); }
QSize QYuvOpenGLWidget::sizeHint() const { return m_frameSize; }

const QSize &QYuvOpenGLWidget::frameSize() { 
    return m_frameSize; 
}

void QYuvOpenGLWidget::setFrameSize(const QSize &frameSize) {
    if (m_frameSize != frameSize) {
        m_frameSize = frameSize;
        updateGeometry();
        m_pboSizeValid = false;
    }
}

void QYuvOpenGLWidget::setFrameData(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV, int linesizeY, int linesizeU, int linesizeV)
{
    if (width != m_frameSize.width() || height != m_frameSize.height() || !m_pboSizeValid) {
        setFrameSize(QSize(width, height));
        initPBOs(width, height);
        initTextures(width, height);
    }

    int uploadIndex = (m_pboIndex + 1) % 2;
    m_pboIndex = uploadIndex; 

    uint8_t* srcData[3] = {dataY, dataU, dataV};
    int srcLinesizes[3] = {linesizeY, linesizeU, linesizeV};
    int widths[3] = {width, width / 2, width / 2};
    int heights[3] = {height, height / 2, height / 2};

    for (int i = 0; i < 3; i++) {
        uint8_t* dstPtr = (uint8_t*)m_pboMappedPtrs[uploadIndex][i];
        if (dstPtr) {
            av_image_copy_plane(dstPtr, widths[i], srcData[i], srcLinesizes[i], widths[i], heights[i]);
        }
    }
    update();
}

void QYuvOpenGLWidget::initializeGL() {
    if (initializeOpenGLFunctions()) {
        m_isInitialized = true;
    } else {
        qCritical() << "Initialize OpenGL Functions failed!";
        return;
    }
    
    QSurfaceFormat format = this->format();
    format.setSwapInterval(0);
    context()->setFormat(format);

    initShader();
    // initTextures();

    static const GLfloat coordinate[] = {
        -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
         1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,
         1.0f,  1.0f, 0.0f,   1.0f, 0.0f
    };

    m_vao.create();
    m_vao.bind();
    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(coordinate, sizeof(coordinate));
    
    m_program.bind();
    m_program.enableAttributeArray("vertexIn");
    m_program.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));
    m_program.enableAttributeArray("textureIn");
    m_program.setAttributeBuffer("textureIn", GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));
    m_program.release();

    m_vbo.release();
    m_vao.release();
    
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader() {
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader)) return;
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShader)) return;
    if (!m_program.link()) return;

    m_program.bind();
    m_program.setUniformValue("tex_y", 0);
    m_program.setUniformValue("tex_u", 1);
    m_program.setUniformValue("tex_v", 2);
    m_program.release();
}

void QYuvOpenGLWidget::initTextures(int width, int height) {
    if (!m_isInitialized) return;

    if (m_textures[0] != 0) {
        glDeleteTextures(3, m_textures);
    }

    glCreateTextures(GL_TEXTURE_2D, 3, m_textures);

    int widths[3] = {width, width / 2, width / 2};
    int heights[3] = {height, height / 2, height / 2};

    for (int i = 0; i < 3; i++) {
        glTextureParameteri(m_textures[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_textures[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureStorage2D(m_textures[i], 1, GL_R8, widths[i], heights[i]);
    }
}

void QYuvOpenGLWidget::initPBOs(int width, int height) {
    deInitPBOs();
    
    int sizes[3] = {
        width * height,             // Y
        (width / 2) * (height / 2), // U
        (width / 2) * (height / 2)  // V
    };

    glCreateBuffers(6, &m_pbos[0][0]); 

    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    for (int set = 0; set < 2; set++) {
        for (int plane = 0; plane < 3; plane++) {
            glNamedBufferStorage(m_pbos[set][plane], sizes[plane], nullptr, flags);
            m_pboMappedPtrs[set][plane] = glMapNamedBufferRange(m_pbos[set][plane], 0, sizes[plane], flags);
        }
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    m_pboSizeValid = true;
}

void QYuvOpenGLWidget::deInitTextures() {
    if (!m_isInitialized) return;

    if (m_textures[0] != 0) {
        glDeleteTextures(3, m_textures);
        memset(m_textures, 0, sizeof(m_textures));
    }
}

void QYuvOpenGLWidget::deInitPBOs() {
    if (!m_isInitialized) return;
    for (int set = 0; set < 2; set++) {
        for (int plane = 0; plane < 3; plane++) {
            if (m_pbos[set][plane] != 0) {
                glUnmapNamedBuffer(m_pbos[set][plane]); 
                glDeleteBuffers(1, &m_pbos[set][plane]);
                m_pbos[set][plane] = 0;
                m_pboMappedPtrs[set][plane] = nullptr;
            }
        }
    }
    m_pboSizeValid = false;
}

void QYuvOpenGLWidget::resizeGL(int width, int height) {
    glViewport(0, 0, width, height);
}

void QYuvOpenGLWidget::paintGL() {
    if (!m_pboSizeValid) return;

    int drawIndex = m_pboIndex;
    int widths[3] = {m_frameSize.width(), m_frameSize.width() / 2, m_frameSize.width() / 2};
    int heights[3] = {m_frameSize.height(), m_frameSize.height() / 2, m_frameSize.height() / 2};

    m_program.bind();
    QOpenGLVertexArrayObject::Binder vaoBinder(&m_vao);

    for (int i = 0; i < 3; i++) {
        // Bind Unit Tekstur
        glBindTextureUnit(i, m_textures[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[drawIndex][i]);
        glTextureSubImage2D(m_textures[i], 0, 0, 0, widths[i], heights[i], GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_program.release();
}

void QYuvOpenGLWidget::setVideoBuffer(VideoBuffer *vb) { m_vb = vb; }
void QYuvOpenGLWidget::updateTextures(quint8*, quint8*, quint8*, quint32, quint32, quint32) {}