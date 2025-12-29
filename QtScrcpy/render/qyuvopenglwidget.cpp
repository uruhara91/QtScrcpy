#include <QCoreApplication>
#include <QDebug>
#include "qyuvopenglwidget.h"
#include "../../QtScrcpyCore/src/device/decoder/videobuffer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
}

// Fallback jika header DRM tidak lengkap
#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 fourcc_code('R', '8', ' ', ' ')
#endif
#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88 fourcc_code('G', 'R', '8', '8')
#endif

// Vertices & Texture Coords
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

// --- SHADER HARDWARE (Manual YUV Conversion) ---
// Kita ambil Y dari texture 0, UV dari texture 1, lalu convert manual.
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
    uniform sampler2D tex_y;  // Texture R8 (Y Plane)
    uniform sampler2D tex_uv; // Texture GR88 (UV Plane)

    void main(void) {
        float r, g, b, y, u, v;
        
        // Ambil Y (ada di channel Red texture pertama)
        y = texture2D(tex_y, textureOut).r;
        
        // Ambil UV (ada di channel Red dan Green texture kedua)
        // Note: Tergantung endianness driver, r bisa U atau V. 
        // NV12 standard: Byte 0=U, Byte 1=V.
        // Di Texture GR88: r=Byte0, g=Byte1.
        u = texture2D(tex_uv, textureOut).r - 0.5;
        v = texture2D(tex_uv, textureOut).g - 0.5;

        // Konversi BT.601
        r = y + 1.402 * v;
        g = y - 0.344136 * u - 0.714136 * v;
        b = y + 1.772 * u;

        gl_FragColor = vec4(r, g, b, 1.0);
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

    m_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    m_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    m_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!m_eglCreateImageKHR || !m_eglDestroyImageKHR || !m_glEGLImageTargetTexture2DOES) {
        qWarning() << "Failed to load EGL extensions!";
    }

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(coordinate, sizeof(coordinate));

    initShader();
    initTextures();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader()
{
    // SW Shader
    m_programSW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShaderSW);
    m_programSW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderSW);
    m_programSW.link();

    // HW Shader
    m_programHW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShaderHW);
    if (!m_programHW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderHW)) {
        qCritical() << "HW Shader Error:" << m_programHW.log();
    }
    m_programHW.link();
}

void QYuvOpenGLWidget::initTextures()
{
    glGenTextures(4, m_textures);
    for (int i = 0; i < 4; i++) {
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
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

void QYuvOpenGLWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);
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

void QYuvOpenGLWidget::releaseHWFrame()
{
    if (m_eglImageY != EGL_NO_IMAGE_KHR) {
        m_eglDestroyImageKHR(eglGetCurrentDisplay(), m_eglImageY);
        m_eglImageY = EGL_NO_IMAGE_KHR;
    }
    if (m_eglImageUV != EGL_NO_IMAGE_KHR) {
        m_eglDestroyImageKHR(eglGetCurrentDisplay(), m_eglImageUV);
        m_eglImageUV = EGL_NO_IMAGE_KHR;
    }
    m_currentHWFrame = nullptr;
}

void QYuvOpenGLWidget::renderHardwareFrame(const AVFrame *frame)
{
    if (frame) {
        releaseHWFrame();
        m_currentHWFrame = frame;

        const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
        if (!desc || desc->nb_layers < 1) return;

        // Kita asumsikan NV12: Layer 0 punya 2 planes.
        // Plane 0: Y (Format R8)
        // Plane 1: UV (Format GR88)
        
        // --- 1. Buat EGL Image untuk Y (Plane 0) ---
        EGLint attribsY[50];
        int i = 0;
        attribsY[i++] = EGL_WIDTH;
        attribsY[i++] = frame->width;
        attribsY[i++] = EGL_HEIGHT;
        attribsY[i++] = frame->height;
        attribsY[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        attribsY[i++] = DRM_FORMAT_R8; // Force format R8 untuk Y
        attribsY[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attribsY[i++] = desc->objects[desc->layers[0].planes[0].object_index].fd;
        attribsY[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attribsY[i++] = desc->layers[0].planes[0].offset;
        attribsY[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attribsY[i++] = desc->layers[0].planes[0].pitch;
        
        if (desc->objects[desc->layers[0].planes[0].object_index].format_modifier != DRM_FORMAT_MOD_INVALID) {
            attribsY[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attribsY[i++] = desc->objects[desc->layers[0].planes[0].object_index].format_modifier & 0xFFFFFFFF;
            attribsY[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attribsY[i++] = desc->objects[desc->layers[0].planes[0].object_index].format_modifier >> 32;
        }
        attribsY[i++] = EGL_NONE;
        
        m_eglImageY = m_eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribsY);

        // --- 2. Buat EGL Image untuk UV (Plane 1) ---
        if (desc->layers[0].nb_planes > 1) {
            EGLint attribsUV[50];
            i = 0;
            attribsUV[i++] = EGL_WIDTH;
            attribsUV[i++] = frame->width / 2; // UV width setengah Y
            attribsUV[i++] = EGL_HEIGHT;
            attribsUV[i++] = frame->height / 2; // UV height setengah Y
            attribsUV[i++] = EGL_LINUX_DRM_FOURCC_EXT;
            attribsUV[i++] = DRM_FORMAT_GR88; // Force format GR88 untuk UV (16 bit/pixel)
            attribsUV[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
            attribsUV[i++] = desc->objects[desc->layers[0].planes[1].object_index].fd;
            attribsUV[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
            attribsUV[i++] = desc->layers[0].planes[1].offset;
            attribsUV[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
            attribsUV[i++] = desc->layers[0].planes[1].pitch;

            if (desc->objects[desc->layers[0].planes[1].object_index].format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribsUV[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
                attribsUV[i++] = desc->objects[desc->layers[0].planes[1].object_index].format_modifier & 0xFFFFFFFF;
                attribsUV[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
                attribsUV[i++] = desc->objects[desc->layers[0].planes[1].object_index].format_modifier >> 32;
            }
            attribsUV[i++] = EGL_NONE;

            m_eglImageUV = m_eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribsUV);
        }
    }

    if (m_eglImageY == EGL_NO_IMAGE_KHR || m_eglImageUV == EGL_NO_IMAGE_KHR) {
        // qWarning() << "EGL Image creation failed";
        return;
    }

    m_programHW.bind();
    m_vbo.bind();

    int vertexLoc = m_programHW.attributeLocation("vertexIn");
    m_programHW.enableAttributeArray(vertexLoc);
    m_programHW.setAttributeBuffer(vertexLoc, GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));

    int textureLoc = m_programHW.attributeLocation("textureIn");
    m_programHW.enableAttributeArray(textureLoc);
    m_programHW.setAttributeBuffer(textureLoc, GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));

    // Bind Y to Texture Unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImageY);
    m_programHW.setUniformValue("tex_y", 0);

    // Bind UV to Texture Unit 1
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImageUV);
    m_programHW.setUniformValue("tex_uv", 1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_programHW.disableAttributeArray(vertexLoc);
    m_programHW.disableAttributeArray(textureLoc);
    m_programHW.release();
}

void QYuvOpenGLWidget::renderSoftwareFrame()
{
    if (!m_programSW.bind()) return;
    if (!m_vbo.bind()) return;

    int vertexLocation = m_programSW.attributeLocation("vertexIn");
    m_programSW.enableAttributeArray(vertexLocation);
    m_programSW.setAttributeBuffer(vertexLocation, GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));

    int textureLocation = m_programSW.attributeLocation("textureIn");
    m_programSW.enableAttributeArray(textureLocation);
    m_programSW.setAttributeBuffer(textureLocation, GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));

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