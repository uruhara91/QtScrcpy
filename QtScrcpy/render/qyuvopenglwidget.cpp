#include "qyuvopenglwidget.h"
#include <QDebug>
#include <QOpenGLFunctions>

extern "C" {
#include <libavutil/imgutils.h>
}

// Vertex shader
static const char *vertShader = R"(#version 450 core
layout(location = 0) in vec3 vertexIn;
layout(location = 1) in vec2 textureIn;
out vec2 textureOut;
void main(void) {
    gl_Position = vec4(vertexIn, 1.0);
    textureOut = textureIn;
}
)";

// Fragment Shader
static const char *fragShader = R"(#version 450 core
in vec2 textureOut;
out vec4 FragColor;

// Binding: 0 = Y Plane (R8), 1 = UV Plane (RG8)
layout(binding = 0) uniform sampler2D tex_y;
layout(binding = 1) uniform sampler2D tex_uv;

void main(void) {
    vec3 yuv;
    
    // Sample YUV from NV12 Textures
    yuv.x = texture(tex_y, textureOut).r;
    vec2 uv = texture(tex_uv, textureOut).rg;
    yuv.y = uv.r;
    yuv.z = uv.g;
    
    // Baked YUV to RGB Conversion
    vec3 rgb = mat3(
        1.164,  1.164,  1.164,
        0.0,   -0.391,  2.018,
        1.596, -0.813,  0.0
    ) * yuv - vec3(0.8709, -0.529, 1.082);
    
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

void QYuvOpenGLWidget::setFrameData(int width, int height, 
                                   std::span<const uint8_t> dataY, 
                                   std::span<const uint8_t> dataUV, 
                                   int linesizeY, int linesizeUV)
{
    if (width != m_frameSize.width() || height != m_frameSize.height()) {
        if (!m_textureSizeMismatch) {
            m_textureSizeMismatch = true;
            emit requestUpdateTextures(width, height);
        }
        return;
    }

    if (!m_pboSizeValid || m_textureSizeMismatch) return;

    int uploadIndex = (m_pboIndex + 1) % 2;

    // 0: Y Plane, 1: UV Plane
    const uint8_t* srcData[2] = { dataY.data(), dataUV.data() };
    int srcLinesizes[2] = { linesizeY, linesizeUV };
    
    int copyWidths[2] = { width, width }; 
    int heights[2] = { height, height / 2 };

    for (int i = 0; i < 2; i++) {
        uint8_t* dstPtr = static_cast<uint8_t*>(m_pboMappedPtrs[uploadIndex][i]);
        
        if (dstPtr && srcData[i]) {
            av_image_copy_plane(dstPtr, copyWidths[i], srcData[i], srcLinesizes[i], copyWidths[i], heights[i]);
        }
    }

    m_pboIndex = uploadIndex;
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
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
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    connect(this, &QYuvOpenGLWidget::requestUpdateTextures, this, [this](int w, int h){
        makeCurrent();
        setFrameSize(QSize(w, h));
        initPBOs(w, h);
        initTextures(w, h);
        m_textureSizeMismatch = false;
        doneCurrent();
    }, Qt::QueuedConnection);
}

void QYuvOpenGLWidget::initShader() {
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader)) return;
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShader)) return;
    if (!m_program.link()) return;
}

void QYuvOpenGLWidget::initTextures(int width, int height) {
    if (!m_isInitialized) return;

    if (m_textures[0] != 0) {
        glDeleteTextures(2, m_textures);
    }

    glCreateTextures(GL_TEXTURE_2D, 2, m_textures);

    // Texture 0: Y Plane (Luma) -> R8
    glTextureParameteri(m_textures[0], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_textures[0], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_textures[0], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_textures[0], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureStorage2D(m_textures[0], 1, GL_R8, width, height);

    // Texture 1: UV Plane (Chroma) -> RG8 (Interleaved)
    glTextureParameteri(m_textures[1], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_textures[1], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_textures[1], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_textures[1], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureStorage2D(m_textures[1], 1, GL_RG8, width / 2, height / 2);
}

void QYuvOpenGLWidget::initPBOs(int width, int height) {
    deInitPBOs();
    
    // UPDATE PBO Sizes for NV12
    int sizes[2] = {
        width * height,             // Y Plane (1 byte per pixel)
        (width / 2) * (height / 2) * 2 // UV Plane (2 bytes per pixel, 1/4 resolution)
    };

    glCreateBuffers(4, &m_pbos[0][0]); // 2 sets * 2 planes = 4 buffers

    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    for (int set = 0; set < 2; set++) {
        for (int plane = 0; plane < 2; plane++) {
            glNamedBufferStorage(m_pbos[set][plane], sizes[plane], nullptr, flags);
            m_pboMappedPtrs[set][plane] = glMapNamedBufferRange(m_pbos[set][plane], 0, sizes[plane], flags);
        }
    }
    m_pboSizeValid = true;
}

void QYuvOpenGLWidget::deInitTextures() {
    if (!m_isInitialized) return;
    if (m_textures[0] != 0) {
        glDeleteTextures(2, m_textures);
        memset(m_textures, 0, sizeof(m_textures));
    }
}

void QYuvOpenGLWidget::deInitPBOs() {
    if (!m_isInitialized) return;
    for (int set = 0; set < 2; set++) {
        for (int plane = 0; plane < 2; plane++) {
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
    
    // Y: w x h, Format RED
    // UV: w/2 x h/2, Format RG
    int widths[2] = {m_frameSize.width(), m_frameSize.width() / 2};
    int heights[2] = {m_frameSize.height(), m_frameSize.height() / 2};
    GLenum formats[2] = {GL_RED, GL_RG};

    m_program.bind();
    QOpenGLVertexArrayObject::Binder vaoBinder(&m_vao);

    for (int i = 0; i < 2; i++) {
        glBindTextureUnit(i, m_textures[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[drawIndex][i]);
        glTextureSubImage2D(m_textures[i], 0, 0, 0, widths[i], heights[i], formats[i], GL_UNSIGNED_BYTE, nullptr);
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_program.release();
}

void QYuvOpenGLWidget::setVideoBuffer(VideoBuffer *vb) { m_vb = vb; }