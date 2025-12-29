#include <QCoreApplication>
#include <QDebug>
#include "qyuvopenglwidget.h"
#include "../../QtScrcpyCore/src/device/decoder/videobuffer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
}

// Vertices & Texture Coords (Full Quad)
static const GLfloat coordinate[] = {
    // Vertex XYZ (Position)    // Texture XY (UV)
    -1.0f, -1.0f, 0.0f,         0.0f, 1.0f,
     1.0f, -1.0f, 0.0f,         1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f,         0.0f, 0.0f,
     1.0f,  1.0f, 0.0f,         1.0f, 0.0f
};

// --- SHADER SOFTWARE (Legacy YUV) ---
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

// --- SHADER HARDWARE (External OES / ARB) ---
static const char *vertShaderHW = R"(
    attribute vec3 vertexIn;
    attribute vec2 textureIn;
    varying vec2 textureOut;
    void main(void) {
        gl_Position = vec4(vertexIn, 1.0);
        textureOut = textureIn;
    }
)";

// Perbaikan: Gunakan GL_ARB_texture_external untuk Desktop Linux
static const char *fragShaderHW = R"(
    // Deteksi apakah kita di Desktop (ARB) atau Mobile (OES)
    #ifdef GL_ES
        #extension GL_OES_EGL_image_external : require
    #else
        #extension GL_ARB_texture_external : require
    #endif

    varying vec2 textureOut;
    uniform samplerExternalOES tex_external;

    void main(void) {
        // samplerExternalOES otomatis convert YUV(NV12) ke RGB!
        gl_FragColor = texture2D(tex_external, textureOut);
    }
)";

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
}

QYuvOpenGLWidget::~QYuvOpenGLWidget()
{
    makeCurrent();
    releaseHWFrame();
    deInitTextures();
    m_vbo.destroy();
    doneCurrent();
}

QSize QYuvOpenGLWidget::minimumSizeHint() const
{
    return QSize(50, 50);
}

QSize QYuvOpenGLWidget::sizeHint() const
{
    return m_frameSize;
}

void QYuvOpenGLWidget::setFrameSize(const QSize &frameSize)
{
    if (m_frameSize != frameSize) {
        m_frameSize = frameSize;
        updateGeometry();
    }
}

const QSize &QYuvOpenGLWidget::frameSize()
{
    return m_frameSize;
}

void QYuvOpenGLWidget::setVideoBuffer(VideoBuffer *vb)
{
    m_vb = vb;
}

void QYuvOpenGLWidget::initializeGL()
{
    initializeOpenGLFunctions();

    // 1. Load EGL Extensions
    m_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    m_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    m_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!m_eglCreateImageKHR || !m_eglDestroyImageKHR || !m_glEGLImageTargetTexture2DOES) {
        qWarning() << "Failed to load EGL extensions! Zero-Copy might fail.";
    } else {
        qInfo() << "EGL extensions loaded successfully.";
    }

    // 2. Init VBO
    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(coordinate, sizeof(coordinate));

    // 3. Init Shaders
    initShader();

    // 4. Init Textures
    initTextures();

    // Background Hitam Pekat (Alpha 1.0)
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader()
{
    // --- Compile Software Shader ---
    if (!m_programSW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShaderSW)) {
        qCritical() << "SW Vertex Shader compile error:" << m_programSW.log();
    }
    if (!m_programSW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderSW)) {
        qCritical() << "SW Fragment Shader compile error:" << m_programSW.log();
    }
    m_programSW.link();

    // --- Compile Hardware Shader ---
    if (!m_programHW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShaderHW)) {
        qCritical() << "HW Vertex Shader compile error:" << m_programHW.log();
    }
    if (!m_programHW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderHW)) {
        qCritical() << "HW Fragment Shader compile error:" << m_programHW.log();
    }
    m_programHW.link();
}

void QYuvOpenGLWidget::initTextures()
{
    glGenTextures(4, m_textures);

    // Init SW Textures (0, 1, 2)
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // --- Init HW Texture (External OES) ---
    // Gunakan target 0x8D65 (GL_TEXTURE_EXTERNAL_OES) agar driver melakukan konversi YUV->RGB
    glBindTexture(0x8D65, m_textures[3]); 
    glTexParameteri(0x8D65, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(0x8D65, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(0x8D65, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(0x8D65, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void QYuvOpenGLWidget::deInitTextures()
{
    if (QOpenGLFunctions::isInitialized(QOpenGLFunctions::d_ptr)) {
        glDeleteTextures(4, m_textures);
    }
}

void QYuvOpenGLWidget::resizeGL(int width, int height)
{
    glViewport(0, 0, width, height);
}

// --- MAIN RENDER LOOP ---
void QYuvOpenGLWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_vb) return;

    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    m_vb->unLock();

    // Repaint frame terakhir jika frame baru kosong
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

void QYuvOpenGLWidget::releaseHWFrame()
{
    if (m_eglImage != EGL_NO_IMAGE_KHR) {
        m_eglDestroyImageKHR(eglGetCurrentDisplay(), m_eglImage);
        m_eglImage = EGL_NO_IMAGE_KHR;
    }
    m_currentHWFrame = nullptr;
}

void QYuvOpenGLWidget::renderHardwareFrame(const AVFrame *frame)
{
    if (frame) {
        releaseHWFrame();
        m_currentHWFrame = frame;

        const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
        if (!desc) return;

        // Create EGL Image
        EGLint attribs[50];
        int i = 0;
        attribs[i++] = EGL_WIDTH;
        attribs[i++] = frame->width;
        attribs[i++] = EGL_HEIGHT;
        attribs[i++] = frame->height;
        attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        attribs[i++] = desc->layers[0].format;

        // Plane 0 (Y)
        attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attribs[i++] = desc->objects[desc->layers[0].planes[0].object_index].fd;
        attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attribs[i++] = desc->layers[0].planes[0].offset;
        attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attribs[i++] = desc->layers[0].planes[0].pitch;
        
        if (desc->objects[desc->layers[0].planes[0].object_index].format_modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attribs[i++] = desc->objects[desc->layers[0].planes[0].object_index].format_modifier & 0xFFFFFFFF;
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attribs[i++] = desc->objects[desc->layers[0].planes[0].object_index].format_modifier >> 32;
        }

        // Plane 1 (UV)
        if (desc->layers[0].nb_planes > 1) {
            attribs[i++] = EGL_DMA_BUF_PLANE1_FD_EXT;
            attribs[i++] = desc->objects[desc->layers[0].planes[1].object_index].fd;
            attribs[i++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
            attribs[i++] = desc->layers[0].planes[1].offset;
            attribs[i++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
            attribs[i++] = desc->layers[0].planes[1].pitch;

            if (desc->objects[desc->layers[0].planes[1].object_index].format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
                attribs[i++] = desc->objects[desc->layers[0].planes[1].object_index].format_modifier & 0xFFFFFFFF;
                attribs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
                attribs[i++] = desc->objects[desc->layers[0].planes[1].object_index].format_modifier >> 32;
            }
        }
        
        attribs[i++] = EGL_NONE;

        m_eglImage = m_eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        if (m_eglImage == EGL_NO_IMAGE_KHR) {
            qWarning() << "[HW] Failed to create EGLImage";
            return;
        }
    }

    if (m_eglImage == EGL_NO_IMAGE_KHR) return;

    // 5. Render
    m_programHW.bind();
    m_vbo.bind();

    int vertexLocation = m_programHW.attributeLocation("vertexIn");
    m_programHW.enableAttributeArray(vertexLocation);
    m_programHW.setAttributeBuffer(vertexLocation, GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));

    int textureLocation = m_programHW.attributeLocation("textureIn");
    m_programHW.enableAttributeArray(textureLocation);
    m_programHW.setAttributeBuffer(textureLocation, GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));

    // --- BIND KE TEXTURE EXTERNAL OES (0x8D65) ---
    // Ini kuncinya! Jangan pakai GL_TEXTURE_2D
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(0x8D65, m_textures[3]); 
    
    // Bind EGL Image ke target EXTERNAL OES
    m_glEGLImageTargetTexture2DOES(0x8D65, m_eglImage);
    
    // Cek error untuk debugging
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        qWarning() << "[HW] GL Error:" << Qt::hex << err;
    }
    
    m_programHW.setUniformValue("tex_external", 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_programHW.disableAttributeArray(vertexLocation);
    m_programHW.disableAttributeArray(textureLocation);
    m_programHW.release();
}

void QYuvOpenGLWidget::renderSoftwareFrame()
{
    if (!m_programSW.bind()) return;
    if (!m_vbo.bind()) return;

    int vertexLocation = m_programSW.attributeLocation("vertexIn");
    if (vertexLocation != -1) {
        m_programSW.enableAttributeArray(vertexLocation);
        m_programSW.setAttributeBuffer(vertexLocation, GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));
    }

    int textureLocation = m_programSW.attributeLocation("textureIn");
    if (textureLocation != -1) {
        m_programSW.enableAttributeArray(textureLocation);
        m_programSW.setAttributeBuffer(textureLocation, GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));
    }

    m_programSW.setUniformValue("tex_y", 0);
    m_programSW.setUniformValue("tex_u", 1);
    m_programSW.setUniformValue("tex_v", 2);
}

void QYuvOpenGLWidget::updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV)
{
    releaseHWFrame();
    renderSoftwareFrame();
    
    m_programSW.bind();
    m_vbo.bind();
    
    // Upload Y
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeY, m_frameSize.height(), 0, GL_RED, GL_UNSIGNED_BYTE, dataY);

    // Upload U
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeU, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataU);

    // Upload V
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeV, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataV);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_programSW.release();
}