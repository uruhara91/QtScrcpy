#include "qyuvopenglwidget.h"
#include "../../QtScrcpyCore/src/device/decoder/videobuffer.h"
#include <QDebug>

extern "C" {
#include <libavutil/frame.h>
}

// --- CONSTANTS ---
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif

// Quad Vertices
static const GLfloat coordinate[] = {
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f
};

// --- SHADERS ---
static const char *vertShader = R"(
    attribute vec3 vertexIn;
    attribute vec2 textureIn;
    varying vec2 textureOut;
    void main(void) {
        gl_Position = vec4(vertexIn, 1.0);
        textureOut = textureIn;
    }
)";

static const char *fragShader = R"(
    varying vec2 textureOut;
    uniform sampler2D tex_y;
    uniform sampler2D tex_u;
    uniform sampler2D tex_v;
    void main(void) {
        vec3 yuv;
        vec3 rgb;
        yuv.x = texture2D(tex_y, textureOut).r;
        yuv.y = texture2D(tex_u, textureOut).r - 0.5;
        yuv.z = texture2D(tex_v, textureOut).r - 0.5;
        rgb = mat3(1.0, 1.0, 1.0, 0.0, -0.39465, 2.03211, 1.13983, -0.58060, 0.0) * yuv;
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

// --- IMPLEMENTATION ---

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

void QYuvOpenGLWidget::setFrameSize(const QSize &frameSize) {
    if (m_frameSize != frameSize) {
        m_frameSize = frameSize;
        updateGeometry();
        m_pboSizeValid = false;
    }
}

const QSize &QYuvOpenGLWidget::frameSize() { return m_frameSize; }
void QYuvOpenGLWidget::setVideoBuffer(VideoBuffer *vb) { m_vb = vb; }

void QYuvOpenGLWidget::initializeGL() {
    initializeOpenGLFunctions();

    // 1. Disable VSync
    QSurfaceFormat format = this->format();
    format.setSwapInterval(0); 
    context()->setFormat(format);

    // 2. Init Resources
    initShader();
    initTextures();

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
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader))
        qWarning() << "Vertex Shader failed:" << m_program.log();
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShader))
        qWarning() << "Fragment Shader failed:" << m_program.log();
    if (!m_program.link())
        qWarning() << "Program Link failed:" << m_program.log();

    m_program.bind();
    m_program.setUniformValue("tex_y", 0);
    m_program.setUniformValue("tex_u", 1);
    m_program.setUniformValue("tex_v", 2);
    m_program.release();
}

void QYuvOpenGLWidget::initTextures() {
    glGenTextures(3, m_textures);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

void QYuvOpenGLWidget::initPBOs(int width, int height) {
    deInitPBOs();

    // Calculate Sizes for YUV420P
    int sizeY = width * height;
    int sizeU = (width / 2) * (height / 2);
    int sizeV = sizeU;
    int sizes[3] = {sizeY, sizeU, sizeV};

    glGenBuffers(6, &m_pbos[0][0]);

    for (int set = 0; set < 2; set++) {
        for (int plane = 0; plane < 3; plane++) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[set][plane]);
            // Allocate "STREAM_DRAW" memory (CPU Write, GPU Read)
            glBufferData(GL_PIXEL_UNPACK_BUFFER, sizes[plane], NULL, GL_STREAM_DRAW);
        }
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    m_pboSizeValid = true;
    qInfo() << "PBO Initialized for resolution:" << width << "x" << height;
}

void QYuvOpenGLWidget::deInitTextures() {
    if (QOpenGLFunctions::isInitialized(QOpenGLFunctions::d_ptr)) {
        glDeleteTextures(3, m_textures);
    }
}

void QYuvOpenGLWidget::deInitPBOs() {
    if (QOpenGLFunctions::isInitialized(QOpenGLFunctions::d_ptr)) {
        // Delete all 6 buffers safely
        if (m_pbos[0][0] != 0) glDeleteBuffers(6, &m_pbos[0][0]);
        memset(m_pbos, 0, sizeof(m_pbos));
    }
    m_pboSizeValid = false;
}

void QYuvOpenGLWidget::resizeGL(int width, int height) {
    glViewport(0, 0, width, height);
}

void QYuvOpenGLWidget::paintGL() {
    if (!m_vb) return;

    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    m_vb->unLock();

    if (!frame) return;

    // Ensure Size & PBOs are correct
    if (frame->width != m_frameSize.width() || frame->height != m_frameSize.height() || !m_pboSizeValid) {
        m_frameSize.setWidth(frame->width);
        m_frameSize.setHeight(frame->height);
        updateGeometry();
        initPBOs(frame->width, frame->height);
    }

    QOpenGLVertexArrayObject::Binder vaoBinder(&m_vao);
    renderFrame(frame);
}

// --- SW RENDERER ---
void QYuvOpenGLWidget::renderFrame(const AVFrame *frame) {
    // 1. Swap Index
    int nextIndex = (m_pboIndex + 1) % 2;
    int index = nextIndex;
    m_pboIndex = nextIndex;

    int widths[3] = {frame->width, frame->width / 2, frame->width / 2};
    int heights[3] = {frame->height, frame->height / 2, frame->height / 2};

    // 2. Upload Data to PBOs
    for (int i = 0; i < 3; i++) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[index][i]);
        
        GLubyte* ptr = (GLubyte*)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, widths[i] * heights[i], 
                                                  GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        
        if (ptr) {
            // Copy line by line
            uint8_t* src = frame->data[i];
            int linesize = frame->linesize[i];
            int widthBytes = widths[i];
            
            if (linesize == widthBytes) {
                memcpy(ptr, src, widthBytes * heights[i]);
            } else {
                for (int h = 0; h < heights[i]; h++) {
                    memcpy(ptr + h * widthBytes, src + h * linesize, widthBytes);
                }
            }
            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        }
    }

    // 3. Update Textures from PBOs
    m_program.bind();
    
    // Bind Textures & Update
    // Y Plane
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[index][0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, widths[0], heights[0], 0, GL_RED, GL_UNSIGNED_BYTE, 0); // 0 offset = PBO

    // U Plane
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[index][1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, widths[1], heights[1], 0, GL_RED, GL_UNSIGNED_BYTE, 0);

    // V Plane
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[index][2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, widths[2], heights[2], 0, GL_RED, GL_UNSIGNED_BYTE, 0);

    // Unbind PBO
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // 4. Draw
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    m_program.release();
}

// Legacy stub
void QYuvOpenGLWidget::updateTextures(quint8*, quint8*, quint8*, quint32, quint32, quint32) {}