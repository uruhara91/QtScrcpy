#include "qyuvopenglwidget.h"
#include <QDebug>
#include <QOpenGLFunctions>
#include <algorithm>
#include <bit>

extern "C" {
#include <libavutil/imgutils.h>
}

// Constexpr forces compile-time evaluation where possible
constexpr int align32(int width) {
    return (width + 31) & ~31;
}

// Shader
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
void main(void) {
    vec3 yuv;
    yuv.x = texture(tex_y, textureOut).r;
    yuv.y = texture(tex_u, textureOut).r;
    yuv.z = texture(tex_v, textureOut).r;
    vec3 rgb = mat3(
        1.164,  1.164,  1.164,
        0.0,   -0.391,  2.018,
        1.596, -0.813,  0.0
    ) * yuv - vec3(0.871, -0.529, 1.082);
    FragColor = vec4(rgb, 1.0);
}
)";

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent) {
    // Lambda
    connect(this, &QYuvOpenGLWidget::requestUpdateTextures, this, [this](int w, int h){
        if (isValid()) {
            makeCurrent();
            setFrameSize(QSize(w, h));
            initPBOs(w, h);
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
    makeCurrent();
    deInitTextures();
    deInitPBOs();
    
    // Cleanup Raw Handles
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    
    doneCurrent();
}   

QSize QYuvOpenGLWidget::minimumSizeHint() const { return QSize(50, 50); }
QSize QYuvOpenGLWidget::sizeHint() const { return m_frameSize; }
const QSize &QYuvOpenGLWidget::frameSize() { return m_frameSize; }

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
    // Cek Resize
    if (width != m_frameSize.width() || height != m_frameSize.height()) {
        if (!m_textureSizeMismatch) {
            m_textureSizeMismatch = true;
            emit requestUpdateTextures(width, height);
        }
        return;
    }

    if (!m_pboSizeValid || m_textureSizeMismatch) return;

    // Atomic Index Swap & Copy
    int uploadIndex = (m_pboIndex + 1) % 2;

    const uint8_t* srcData[3] = { dataY.data(), dataU.data(), dataV.data() };
    int srcLinesizes[3] = { linesizeY, linesizeU, linesizeV };
    int widths[3] = { width, width / 2, width / 2 };
    int heights[3] = { height, height / 2, height / 2 };

    // Direct Memory Copy to Mapped GPU Memory
    for (int i = 0; i < 3; i++) {
        if (auto dstPtr = static_cast<uint8_t*>(m_pboMappedPtrs[uploadIndex][i]); dstPtr && srcData[i]) {
             av_image_copy_plane(dstPtr, m_pboStrides[i], srcData[i], srcLinesizes[i], widths[i], heights[i]);
        }
    }

    m_pboIndex = uploadIndex;

    // Prevent flooding event loop
    bool expected = false;
    if (m_updatePending.compare_exchange_strong(expected, true)) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

void QYuvOpenGLWidget::initializeGL() {
    if (initializeOpenGLFunctions()) {
        m_isInitialized = true;
    } else {
        qCritical() << "Initialize OpenGL Functions failed!";
        return;
    }
    
    // Opt
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);

    initShader();

    // Data Vertex Quad
    static const float coordinate[] = {
        -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
         1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,
         1.0f,  1.0f, 0.0f,   1.0f, 0.0f
    };
    
    // 1. Create VAO & VBO
    glCreateVertexArrays(1, &m_vao);
    glCreateBuffers(1, &m_vbo);

    // 2. Upload Data to VBO
    glNamedBufferStorage(m_vbo, sizeof(coordinate), coordinate, 0);

    // 3. Link VBO to VAO Binding Point 0
    glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, 5 * sizeof(float));

    // 4. Setup Attribute Layout
    // Attribute 0: Posisi (3 floats)
    glEnableVertexArrayAttrib(m_vao, 0);
    glVertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_vao, 0, 0);

    // Attribute 1: Texture Coords (2 floats, offset 3*float)
    glEnableVertexArrayAttrib(m_vao, 1);
    glVertexArrayAttribFormat(m_vao, 1, 2, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
    glVertexArrayAttribBinding(m_vao, 1, 0);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader() {
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader)) return;
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShader)) return;
    if (!m_program.link()) return;
    
    // Set uniforms once
    m_program.bind();
    m_program.setUniformValue("tex_y", 0);
    m_program.setUniformValue("tex_u", 1);
    m_program.setUniformValue("tex_v", 2);
    m_program.release();
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
        // Setup texture parameters
        glTextureParameteri(m_textures[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_textures[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Allocate immutable storage
        glTextureStorage2D(m_textures[i], 1, GL_R8, widths[i], heights[i]);
    }
}

void QYuvOpenGLWidget::initPBOs(int width, int height) {
    deInitPBOs();

    // Alignment calculation
    m_pboStrides[0] = align32(width);
    m_pboStrides[1] = align32(width / 2);
    m_pboStrides[2] = align32(width / 2);

    int sizes[3] = {
        m_pboStrides[0] * height,           
        m_pboStrides[1] * (height / 2),
        m_pboStrides[2] * (height / 2)
    };

    // Persistent Mapping
    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    for (int set = 0; set < 2; set++) {
        glCreateBuffers(3, m_pbos[set].data());
        for (int plane = 0; plane < 3; plane++) {
            // Allocate Storage & Map
            glNamedBufferStorage(m_pbos[set][plane], sizes[plane], nullptr, flags);
            m_pboMappedPtrs[set][plane] = glMapNamedBufferRange(m_pbos[set][plane], 0, sizes[plane], flags);
        }
    }
    m_pboSizeValid = true;
}

void QYuvOpenGLWidget::deInitTextures() {
    if (!m_isInitialized) return;
    if (m_textures[0] != 0) {
        glDeleteTextures(3, m_textures.data());
        std::ranges::fill(m_textures, 0);
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

    // Index buffer
    int drawIndex = m_pboIndex; 

    int widths[3] = {m_frameSize.width(), m_frameSize.width() / 2, m_frameSize.width() / 2};
    int heights[3] = {m_frameSize.height(), m_frameSize.height() / 2, m_frameSize.height() / 2};

    m_program.bind();
    
    // Bind VAO
    glBindVertexArray(m_vao);

    // Texture Upload
    for (int i = 0; i < 3; i++) {
        glBindTextureUnit(i, m_textures[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[drawIndex][i]);
        
        glPixelStorei(GL_UNPACK_ROW_LENGTH, m_pboStrides[i]);
        // Update texture content
        glTextureSubImage2D(m_textures[i], 0, 0, 0, widths[i], heights[i], GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    m_program.release();

    m_updatePending = false;
}

void QYuvOpenGLWidget::setVideoBuffer(VideoBuffer *vb) { m_vb = vb; }
void QYuvOpenGLWidget::updateTextures(quint8*, quint8*, quint8*, quint32, quint32, quint32) {}