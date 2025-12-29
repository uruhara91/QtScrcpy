#include <QCoreApplication>
#include <QDebug>
#include "qyuvopenglwidget.h"
#include "../../QtScrcpyCore/src/device/decoder/videobuffer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
}

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 fourcc_code('R', '8', ' ', ' ')
#endif

// Vertices (Full Quad)
static const GLfloat coordinate[] = {
    -1.0f, -1.0f, 0.0f,         0.0f, 1.0f,
     1.0f, -1.0f, 0.0f,         1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f,         0.0f, 0.0f,
     1.0f,  1.0f, 0.0f,         1.0f, 0.0f
};

// --- SHADER SOFTWARE ---
static const char *vertShaderSW = R"(
    attribute vec3 vertexIn;
    attribute vec2 textureIn;
    varying vec2 textureOut;
    void main(void) {
        gl_Position = vec4(vertexIn, 1.0);
        textureOut = textureIn;
    }
)";

static const char *fragShaderSW = R"(
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
        rgb = mat3(1.0, 1.0, 1.0,
                   0.0, -0.39465, 2.03211,
                   1.13983, -0.58060, 0.0) * yuv;
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

// --- SHADER HARDWARE (DEBUG: Y-PLANE ONLY) ---
// Kita hanya mengambil komponen Y (Red Channel dari Texture 0)
// Jika ini berhasil, gambar akan HITAM PUTIH.
static const char *vertShaderHW = R"(
    attribute vec3 vertexIn;
    attribute vec2 textureIn;
    varying vec2 textureOut;
    void main(void) {
        gl_Position = vec4(vertexIn, 1.0);
        textureOut = textureIn;
    }
)";

static const char *fragShaderHW = R"(
    varying vec2 textureOut;
    uniform sampler2D tex_y; // Texture R8

    void main(void) {
        float y = texture2D(tex_y, textureOut).r;
        // Output Grayscale
        gl_FragColor = vec4(y, y, y, 1.0);
    }
)";

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent) {}

QYuvOpenGLWidget::~QYuvOpenGLWidget() {
    makeCurrent();
    releaseHWFrame();
    deInitTextures();
    m_vbo.destroy();
    doneCurrent();
}

QSize QYuvOpenGLWidget::minimumSizeHint() const { return QSize(50, 50); }
QSize QYuvOpenGLWidget::sizeHint() const { return m_frameSize; }

void QYuvOpenGLWidget::setFrameSize(const QSize &frameSize) {
    if (m_frameSize != frameSize) {
        m_frameSize = frameSize;
        updateGeometry();
    }
}

const QSize &QYuvOpenGLWidget::frameSize() { return m_frameSize; }
void QYuvOpenGLWidget::setVideoBuffer(VideoBuffer *vb) { m_vb = vb; }

void QYuvOpenGLWidget::initializeGL() {
    initializeOpenGLFunctions();

    m_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    m_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    m_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!m_eglCreateImageKHR || !m_eglDestroyImageKHR || !m_glEGLImageTargetTexture2DOES) {
        qWarning() << "[HW] Critical: Failed to load EGL extensions!";
    }

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(coordinate, sizeof(coordinate));

    initShader();
    initTextures();

    // DEBUG: Set Clear Color MERAH. 
    // Jika layar Merah -> Berarti renderHardwareFrame tidak menggambar apapun (failed draw)
    // Jika layar Hitam/Gambar -> Berarti draw berhasil.
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader() {
    m_programSW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShaderSW);
    m_programSW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderSW);
    m_programSW.link();

    m_programHW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShaderHW);
    if (!m_programHW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderHW)) {
        qCritical() << "[HW] Shader Error:" << m_programHW.log();
    }
    m_programHW.link();
}

void QYuvOpenGLWidget::initTextures() {
    glGenTextures(4, m_textures);
    for (int i = 0; i < 4; i++) {
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

void QYuvOpenGLWidget::deInitTextures() {
    if (QOpenGLFunctions::isInitialized(QOpenGLFunctions::d_ptr)) {
        glDeleteTextures(4, m_textures);
    }
}

void QYuvOpenGLWidget::resizeGL(int width, int height) {
    glViewport(0, 0, width, height);
}

void QYuvOpenGLWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT); // Clear to Red

    if (!m_vb) return;

    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    m_vb->unLock();

    if (!frame && m_currentHWFrame) {
        renderHardwareFrame(nullptr);
        return;
    }
    if (!frame) return;

    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        renderHardwareFrame(frame);
    } else {
        releaseHWFrame();
        updateTextures(frame->data[0], frame->data[1], frame->data[2], 
                       frame->linesize[0], frame->linesize[1], frame->linesize[2]);
    }
}

void QYuvOpenGLWidget::releaseHWFrame() {
    if (m_eglImageY != EGL_NO_IMAGE_KHR) {
        m_eglDestroyImageKHR(eglGetCurrentDisplay(), m_eglImageY);
        m_eglImageY = EGL_NO_IMAGE_KHR;
    }
    // Cleanup UV image jika ada nanti
    m_currentHWFrame = nullptr;
}

void QYuvOpenGLWidget::renderHardwareFrame(const AVFrame *frame) {
    if (frame) {
        releaseHWFrame();
        m_currentHWFrame = frame;

        const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
        if (!desc || desc->nb_layers < 1) {
            qWarning() << "[HW] Invalid DRM Descriptor";
            return;
        }

        // --- DEBUG: IMPORT Y PLANE ONLY (R8) ---
        EGLint attribs[50];
        int i = 0;
        attribs[i++] = EGL_WIDTH;
        attribs[i++] = frame->width;
        attribs[i++] = EGL_HEIGHT;
        attribs[i++] = frame->height;
        attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        attribs[i++] = DRM_FORMAT_R8; // Force R8
        attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attribs[i++] = desc->objects[desc->layers[0].planes[0].object_index].fd;
        attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attribs[i++] = desc->layers[0].planes[0].offset;
        attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attribs[i++] = desc->layers[0].planes[0].pitch;
        
        // Penting: Modifiers
        if (desc->objects[desc->layers[0].planes[0].object_index].format_modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attribs[i++] = desc->objects[desc->layers[0].planes[0].object_index].format_modifier & 0xFFFFFFFF;
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attribs[i++] = desc->objects[desc->layers[0].planes[0].object_index].format_modifier >> 32;
        }
        attribs[i++] = EGL_NONE;

        m_eglImageY = m_eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        
        if (m_eglImageY == EGL_NO_IMAGE_KHR) {
            EGLint err = eglGetError();
            qWarning() << "[HW] Failed to create Y-Plane Image! Err:" << Qt::hex << err;
            return;
        }
    }

    if (m_eglImageY == EGL_NO_IMAGE_KHR) return;

    m_programHW.bind();
    m_vbo.bind();

    int vertexLoc = m_programHW.attributeLocation("vertexIn");
    m_programHW.enableAttributeArray(vertexLoc);
    m_programHW.setAttributeBuffer(vertexLoc, GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));

    int textureLoc = m_programHW.attributeLocation("textureIn");
    m_programHW.enableAttributeArray(textureLoc);
    m_programHW.setAttributeBuffer(textureLoc, GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));

    // Bind Y Texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImageY);
    m_programHW.setUniformValue("tex_y", 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_programHW.disableAttributeArray(vertexLoc);
    m_programHW.disableAttributeArray(textureLoc);
    m_programHW.release();
}

void QYuvOpenGLWidget::renderSoftwareFrame() {
    if (!m_programSW.bind()) return;
    if (!m_vbo.bind()) return;
    // ... setup attribute pointers ...
    int vertexLoc = m_programSW.attributeLocation("vertexIn");
    m_programSW.enableAttributeArray(vertexLoc);
    m_programSW.setAttributeBuffer(vertexLoc, GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));
    int texLoc = m_programSW.attributeLocation("textureIn");
    m_programSW.enableAttributeArray(texLoc);
    m_programSW.setAttributeBuffer(texLoc, GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));
    
    m_programSW.setUniformValue("tex_y", 0);
    m_programSW.setUniformValue("tex_u", 1);
    m_programSW.setUniformValue("tex_v", 2);
}

void QYuvOpenGLWidget::updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV) {
    releaseHWFrame();
    renderSoftwareFrame();
    m_programSW.bind();
    m_vbo.bind();
    
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeY, m_frameSize.height(), 0, GL_RED, GL_UNSIGNED_BYTE, dataY);

    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeU, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataU);

    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeV, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataV);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_programSW.release();
}