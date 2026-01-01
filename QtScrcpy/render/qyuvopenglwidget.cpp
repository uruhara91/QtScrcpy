#include <QCoreApplication>
#include <QDebug>
#include "qyuvopenglwidget.h"
#include "../../QtScrcpyCore/src/device/decoder/videobuffer.h"
#include <QSurfaceFormat>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
}

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 fourcc_code('R', '8', ' ', ' ')
#endif

// Vertex data
static const GLfloat coordinate[] = {
    // X, Y, Z,           U, V
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f
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
        // BT.601 conversion (Standard for SW decode)
        rgb = mat3(1.0, 1.0, 1.0,
                   0.0, -0.39465, 2.03211,
                   1.13983, -0.58060, 0.0) * yuv;
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

// --- SHADER HARDWARE ---
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
    // Precision Hint
    #ifdef GL_ES
    precision mediump float;
    #endif

    varying vec2 textureOut;
    uniform sampler2D tex_y;      // Y Plane (R8)
    uniform sampler2D tex_uv_raw; // UV Plane (R8 Raw)
    uniform float width;          // Texture Width

    // Pre-calculated vectors for BT.709 (HDTV Standard)
    const vec3 coeff_r = vec3(1.0, 0.0, 1.7927);
    const vec3 coeff_g = vec3(1.0, -0.2132, -0.5329);
    const vec3 coeff_b = vec3(1.0, 2.1124, 0.0);

    void main(void) {
        float y, u, v;

        // 1. Fetch Y (Luminance)
        y = texture2D(tex_y, textureOut).r;

        // 2. Fetch UV (Chroma) with Pixel Center Sampling (Anti-Artifacts)
        // Optimization: Simplify math for coordinate calculation
        float texelSize = 1.0 / width;
        float pixelPos = textureOut.x * width;
        
        // Logic: floor(pos / 2) * 2 + 0.5 -> Mencari tengah-tengah dari blok 2x2
        float u_x = (floor(pixelPos * 0.5) * 2.0 + 0.5) * texelSize;
        float v_x = u_x + texelSize; 
        
        // Reading UV (And centering it by subtracting 0.5)
        u = texture2D(tex_uv_raw, vec2(u_x, textureOut.y)).r - 0.5; 
        v = texture2D(tex_uv_raw, vec2(v_x, textureOut.y)).r - 0.5; 

        // 3. COLOR CORRECTION: Limited Range Fix + BT.709
        // Vectorized Math for GPU Efficiency
        
        // Expand Limited Range Y (16-235) to Full Range (0-255)
        // Original Formula: y = 1.1643 * (y - 0.0627);
        y = (y - 0.0627) * 1.1643;

        vec3 yuv = vec3(y, u, v);
        vec3 rgb;

        // Dot Product is native GPU instruction (1 cycle usually)
        rgb.r = dot(yuv, coeff_r);
        rgb.g = dot(yuv, coeff_g);
        rgb.b = dot(yuv, coeff_b);

        gl_FragColor = vec4(rgb, 1.0);
    }
)";

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent) {}

QYuvOpenGLWidget::~QYuvOpenGLWidget() {
    makeCurrent();
    
    flushEGLCache(); 
    deInitTextures();
    m_vao.destroy();
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

    QSurfaceFormat format = this->format();
    format.setSwapInterval(0); 
    context()->setFormat(format);

    initShader();
    initTextures();

    // --- VAO SETUP ---
    m_vao.create();
    m_vao.bind();

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(coordinate, sizeof(coordinate));
    
    // Vertex Pos (vec3) - Location 0/vertexIn
    m_programHW.enableAttributeArray("vertexIn");
    m_programHW.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));
    
    // Texture Coord (vec2) - Location 1/textureIn
    m_programHW.enableAttributeArray("textureIn");
    m_programHW.setAttributeBuffer("textureIn", GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));

    m_vbo.release();
    m_vao.release();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader() {
    // Compile shaders
    if (!m_programSW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShaderSW))
        qCritical() << "SW Vertex Shader Error:" << m_programSW.log();
    if (!m_programSW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderSW))
        qCritical() << "SW Frag Shader Error:" << m_programSW.log();
    m_programSW.link();

    if (!m_programHW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShaderHW))
        qCritical() << "HW Vertex Shader Error:" << m_programHW.log();
    if (!m_programHW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderHW))
        qCritical() << "HW Frag Shader Error:" << m_programHW.log();
    m_programHW.link();
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
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_vb) return;

    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    m_vb->unLock();

    // --- BIND VAO ---
    QOpenGLVertexArrayObject::Binder vaoBinder(&m_vao);

    if (!frame) return;

    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        renderHardwareFrame(frame);
    } else {
        
        updateTextures(frame->data[0], frame->data[1], frame->data[2], 
                       frame->linesize[0], frame->linesize[1], frame->linesize[2]);
    }
}

void QYuvOpenGLWidget::flushEGLCache() {
    if (!m_eglDestroyImageKHR) return;

    auto it = m_eglImageCache.begin();
    while (it != m_eglImageCache.end()) {
        m_eglDestroyImageKHR(eglGetCurrentDisplay(), it.value().image);
        ++it;
    }
    m_eglImageCache.clear();
    m_cacheRecentUse.clear();
}

EGLImageKHR QYuvOpenGLWidget::getCachedEGLImage(int fd, int offset, int pitch, int width, int height, uint64_t modifier) {
    QPair<int, int> key = qMakePair(fd, offset);

    // 1. Cek Cache
    if (m_eglImageCache.contains(key)) {
        m_cacheRecentUse.removeOne(key); 
        m_cacheRecentUse.append(key);
        
        EGLImageCacheEntry entry = m_eglImageCache[key];
        if (entry.width == width && entry.height == height) {
            return entry.image;
        } else {
            m_eglDestroyImageKHR(eglGetCurrentDisplay(), entry.image);
            m_eglImageCache.remove(key);
            m_cacheRecentUse.removeOne(key);
        }
    }

    // 2. LRU
    while (m_eglImageCache.size() >= 5) {
        if (m_cacheRecentUse.isEmpty()) break;
        
        // Ambil key yang paling jarang dipake (paling depan)
        QPair<int, int> oldKey = m_cacheRecentUse.takeFirst();
        
        if (m_eglImageCache.contains(oldKey)) {
            // Destroy Image biar Buffer-nya balik ke Pool FFmpeg
            m_eglDestroyImageKHR(eglGetCurrentDisplay(), m_eglImageCache[oldKey].image);
            m_eglImageCache.remove(oldKey);
        }
    }

    // 3. Create
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

    // 4. Caching
    if (img != EGL_NO_IMAGE_KHR) {
        m_eglImageCache.insert(key, {img, width, height});
        m_cacheRecentUse.append(key);
    }

    return img;
}

void QYuvOpenGLWidget::renderHardwareFrame(const AVFrame *frame) {
    if (frame) {
        
        static int lastW = 0, lastH = 0;
        if (frame->width != lastW || frame->height != lastH) {
            flushEGLCache();
            lastW = frame->width;
            lastH = frame->height;
        }

        const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
        if (!desc) return;

        const AVDRMPlaneDescriptor *planeY = nullptr;
        const AVDRMPlaneDescriptor *planeUV = nullptr;

        if (desc->nb_layers > 0) {
            planeY = &desc->layers[0].planes[0];
        }

        if (desc->nb_layers > 1) {
            planeUV = &desc->layers[1].planes[0];
        } else if (desc->layers[0].nb_planes > 1) {
            planeUV = &desc->layers[0].planes[1];
        }

        if (planeY) {
            const AVDRMObjectDescriptor &obj = desc->objects[planeY->object_index];
            m_eglImageY = getCachedEGLImage(obj.fd, planeY->offset, planeY->pitch, 
                                            frame->width, frame->height, obj.format_modifier);
        }

        if (planeUV) {
            const AVDRMObjectDescriptor &obj = desc->objects[planeUV->object_index];
            m_eglImageUV = getCachedEGLImage(obj.fd, planeUV->offset, planeUV->pitch, 
                                             frame->width, frame->height / 2, obj.format_modifier);
        }
    }

    if (m_eglImageY == EGL_NO_IMAGE_KHR) return;

    m_programHW.bind();

    // Bind Texture Units
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImageY);
    m_programHW.setUniformValue("tex_y", 0);

    if (m_eglImageUV != EGL_NO_IMAGE_KHR) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_textures[1]);
        m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImageUV);
        m_programHW.setUniformValue("tex_uv_raw", 1);
    }

    m_programHW.setUniformValue("width", (float)m_frameSize.width());

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    m_programHW.release();
}

void QYuvOpenGLWidget::renderSoftwareFrame() {
    m_programSW.bind();
    m_programSW.setUniformValue("tex_y", 0);
    m_programSW.setUniformValue("tex_u", 1);
    m_programSW.setUniformValue("tex_v", 2);
}

void QYuvOpenGLWidget::updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV) {

    renderSoftwareFrame();
    
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeY, m_frameSize.height(), 0, GL_RED, GL_UNSIGNED_BYTE, dataY);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeU, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataU);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeV, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataV);
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_programSW.release();
}