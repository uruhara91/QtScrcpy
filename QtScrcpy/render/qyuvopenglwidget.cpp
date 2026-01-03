#include "qyuvopenglwidget.h"
#include "../../QtScrcpyCore/src/device/decoder/videobuffer.h"
#include <QDebug>
#include <QThread>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
}

// --- CONSTANTS ---
#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 fourcc_code('R', '8', ' ', ' ')
#endif

// Quad Vertices (Full Screen Strip)
static const GLfloat coordinate[] = {
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f
};

// --- SHADERS ---
// Vertex Shader (Shared)
static const char *vertShader = R"(
    attribute vec3 vertexIn;
    attribute vec2 textureIn;
    varying vec2 textureOut;
    void main(void) {
        gl_Position = vec4(vertexIn, 1.0);
        textureOut = textureIn;
    }
)";

// Fragment Shader HW (MANUAL UV SAMPLING - PRESERVED)
static const char *fragShaderHW = R"(
    #ifdef GL_ES
    precision mediump float;
    #endif
    varying vec2 textureOut;
    uniform sampler2D tex_y;
    uniform sampler2D tex_uv_raw;
    uniform float width;
    const vec3 offset = vec3(0.0627, 0.5, 0.5);
    const mat3 yuv2rgb = mat3(1.164, 1.164, 1.164, 0.000, -0.392, 2.017, 1.596, -0.813, 0.000);

    void main(void) {
        float y = texture2D(tex_y, textureOut).r - offset.x;
        // MANUAL NEAREST SAMPLING (Anti-Bleeding Workaround)
        float texelSize = 1.0 / width;
        float u_x = (floor(textureOut.x * width * 0.5) * 2.0 + 0.5) * texelSize;
        float v_x = u_x + texelSize;
        float u = texture2D(tex_uv_raw, vec2(u_x, textureOut.y)).r - offset.y;
        float v = texture2D(tex_uv_raw, vec2(v_x, textureOut.y)).r - offset.z;
        vec3 rgb = yuv2rgb * vec3(y, u, v);
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

// Fragment Shader SW (Fallback)
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
        rgb = mat3(1.0, 1.0, 1.0, 0.0, -0.39465, 2.03211, 1.13983, -0.58060, 0.0) * yuv;
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

// --- IMPLEMENTATION ---

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent) {}

QYuvOpenGLWidget::~QYuvOpenGLWidget() {
    makeCurrent();
    cleanCache(); // Clean EGL resources
    deInitTextures();
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
    }
}

const QSize &QYuvOpenGLWidget::frameSize() { return m_frameSize; }
void QYuvOpenGLWidget::setVideoBuffer(VideoBuffer *vb) { m_vb = vb; }

void QYuvOpenGLWidget::initializeGL() {
    initializeOpenGLFunctions();

    // 1. Dynamic Load EGL Functions
    m_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    m_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    m_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    m_eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    m_eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    m_eglClientWaitSyncKHR = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");

    if (!m_eglCreateImageKHR || !m_eglCreateSyncKHR) {
        qWarning() << "Critical EGL extensions missing! Optimization disabled.";
    }

    // 2. Disable VSync
    QSurfaceFormat format = this->format();
    format.setSwapInterval(0); 
    context()->setFormat(format);

    // 3. Init Resources
    initShader();
    initTextures();

    m_vao.create();
    m_vao.bind();
    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(coordinate, sizeof(coordinate));
    
    m_programHW.bind();
    m_programHW.enableAttributeArray("vertexIn");
    m_programHW.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));
    m_programHW.enableAttributeArray("textureIn");
    m_programHW.setAttributeBuffer("textureIn", GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));
    m_programHW.release();

    m_vbo.release();
    m_vao.release();
    
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader() {
    // SW Shader
    if (!m_programSW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader))
        qWarning() << "SW Vertex:" << m_programSW.log();
    if (!m_programSW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderSW))
        qWarning() << "SW Fragment:" << m_programSW.log();
    if (!m_programSW.link())
        qWarning() << "SW Link:" << m_programSW.log();

    // HW Shader
    if (!m_programHW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader))
        qWarning() << "HW Vertex:" << m_programHW.log();
    if (!m_programHW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderHW))
        qWarning() << "HW Fragment:" << m_programHW.log();
    if (!m_programHW.link())
        qWarning() << "HW Link:" << m_programHW.log();

    m_programHW.bind();
    m_programHW.setUniformValue("tex_y", 0);
    m_programHW.setUniformValue("tex_uv_raw", 1);
    m_programHW.release();
}

void QYuvOpenGLWidget::initTextures() {
    glGenTextures(4, m_textures);
    for (int i = 0; i < 4; i++) {
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
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
    if (!m_vb) return;

    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    m_vb->unLock();

    if (!frame) return;

    if (frame->width != m_frameSize.width() || frame->height != m_frameSize.height()) {
        m_frameSize.setWidth(frame->width);
        m_frameSize.setHeight(frame->height);
        updateGeometry(); 
        
        // Size changed, cache invalid (safe to clear)
        cleanCache();
    }

    QOpenGLVertexArrayObject::Binder vaoBinder(&m_vao);

    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        renderHardwareFrame(frame);
    } else {
        renderSoftwareFrame(frame);
    }
}

// --- SMART EGL CACHE ---
EGLImageKHR QYuvOpenGLWidget::getCachedEGLImage(int fd, int offset, int pitch, int width, int height, uint64_t modifier) {
    EGLImageKey key = {fd, offset, pitch, modifier};
    
    if (m_eglImageCache.contains(key)) {
        return m_eglImageCache[key];
    }

    // Cache Miss: Create new EGLImage
    if (!m_eglCreateImageKHR) return EGL_NO_IMAGE_KHR;

    EGLint attribs[50];
    int i = 0;
    attribs[i++] = EGL_WIDTH; attribs[i++] = width;
    attribs[i++] = EGL_HEIGHT; attribs[i++] = height;
    attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT; attribs[i++] = DRM_FORMAT_R8;
    attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT; attribs[i++] = fd;
    attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attribs[i++] = offset;
    attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; attribs[i++] = pitch;
    
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attribs[i++] = modifier & 0xFFFFFFFF;
        attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attribs[i++] = modifier >> 32;
    }
    attribs[i++] = EGL_NONE;

    EGLImageKHR img = m_eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    
    if (img != EGL_NO_IMAGE_KHR) {
        m_eglImageCache.insert(key, img);
    }
    return img;
}

void QYuvOpenGLWidget::cleanCache() {
    if (!m_eglDestroyImageKHR) return;
    
    EGLDisplay dpy = eglGetCurrentDisplay();
    QHashIterator<EGLImageKey, EGLImageKHR> i(m_eglImageCache);
    while (i.hasNext()) {
        i.next();
        m_eglDestroyImageKHR(dpy, i.value());
    }
    m_eglImageCache.clear();
}

// --- OPTIMIZED RENDER LOOP ---
void QYuvOpenGLWidget::renderHardwareFrame(const AVFrame *frame) {
    if (!m_eglCreateImageKHR || !m_glEGLImageTargetTexture2DOES) return;

    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
    if (!desc) return;

    // --- PLANE Y ---
    const AVDRMPlaneDescriptor *planeY = &desc->layers[0].planes[0];
    const AVDRMObjectDescriptor &objY = desc->objects[planeY->object_index];
    
    // --- PLANE UV ---
    const AVDRMPlaneDescriptor *planeUV = nullptr;
    if (desc->nb_layers > 1) planeUV = &desc->layers[1].planes[0];
    else if (desc->layers[0].nb_planes > 1) planeUV = &desc->layers[0].planes[1];
    if (!planeUV) return;
    const AVDRMObjectDescriptor &objUV = desc->objects[planeUV->object_index];

    // 1. GET FROM CACHE (Zero Allocation if cached)
    EGLImageKHR imgY = getCachedEGLImage(objY.fd, planeY->offset, planeY->pitch, 
                                         frame->width, frame->height, objY.format_modifier);
    
    EGLImageKHR imgUV = getCachedEGLImage(objUV.fd, planeUV->offset, planeUV->pitch, 
                                          frame->width, frame->height / 2, objUV.format_modifier);

    if (imgY == EGL_NO_IMAGE_KHR || imgUV == EGL_NO_IMAGE_KHR) {
         qWarning() << "EGL Image Creation Failed";
         return;
    }

    m_programHW.bind();

    // 2. BIND (Fast)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, imgY);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, imgUV);

    if (m_lastWidth != frame->width) {
        m_programHW.setUniformValue("width", (float)frame->width);
        m_lastWidth = frame->width;
    }
    
    // 3. DRAW
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // --- FENCE SYNCHRONIZATION ---
    // Pastikan GPU selesai menggunakan EGLImage ini sebelum decoder menimpanya nanti
    if (m_eglCreateSyncKHR && m_eglClientWaitSyncKHR && m_eglDestroySyncKHR) {
        EGLSyncKHR sync = m_eglCreateSyncKHR(eglGetCurrentDisplay(), EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            // Flush and wait for command submission (NOT completion)
            m_eglClientWaitSyncKHR(eglGetCurrentDisplay(), sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 1000000);
            m_eglDestroySyncKHR(eglGetCurrentDisplay(), sync);
        } else {
            glFinish(); 
        }
    } else {
        glFlush();
    }
    
    // NO DESTROY HERE! Cache holds the image.
    m_programHW.release();
}

void QYuvOpenGLWidget::renderSoftwareFrame(const AVFrame *frame) {
    if (!frame) return;
    updateTextures(frame->data[0], frame->data[1], frame->data[2], 
                   frame->linesize[0], frame->linesize[1], frame->linesize[2]);
}

void QYuvOpenGLWidget::updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV) {
    m_programSW.bind();
    m_programSW.setUniformValue("tex_y", 0);
    m_programSW.setUniformValue("tex_u", 1);
    m_programSW.setUniformValue("tex_v", 2);
    
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeY, m_frameSize.height(), 0, GL_RED, GL_UNSIGNED_BYTE, dataY);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeU, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataU);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeV, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataV);
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_programSW.release();
}