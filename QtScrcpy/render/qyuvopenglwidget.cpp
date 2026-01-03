#include "qyuvopenglwidget.h"
#include "../../QtScrcpyCore/src/device/decoder/videobuffer.h"
#include <QDebug>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
}

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 fourcc_code('R', '8', ' ', ' ')
#endif

// Quad Vertex
static const GLfloat coordinate[] = {
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f
};

// --- SHADER ---
static const char *vertShader = R"(
    attribute vec3 vertexIn;
    attribute vec2 textureIn;
    varying vec2 textureOut;
    void main(void) {
        gl_Position = vec4(vertexIn, 1.0);
        textureOut = textureIn;
    }
)";

// Fragment Shader HW
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
        // NEAREST SAMPLING IS KEY HERE
        float texelSize = 1.0 / width;
        float u_x = (floor(textureOut.x * width * 0.5) * 2.0 + 0.5) * texelSize;
        float v_x = u_x + texelSize;
        float u = texture2D(tex_uv_raw, vec2(u_x, textureOut.y)).r - offset.y;
        float v = texture2D(tex_uv_raw, vec2(v_x, textureOut.y)).r - offset.z;
        vec3 rgb = yuv2rgb * vec3(y, u, v);
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

// Fragment Shader SW
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

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent) {}

QYuvOpenGLWidget::~QYuvOpenGLWidget() {
    makeCurrent();
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

    // DISABLE VSYNC
    QSurfaceFormat format = this->format();
    format.setSwapInterval(0); 
    context()->setFormat(format);

    initShader();
    initTextures();

    m_vao.create();
    m_vao.bind();
    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(coordinate, sizeof(coordinate));
    
    m_programHW.enableAttributeArray("vertexIn");
    m_programHW.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));
    m_programHW.enableAttributeArray("textureIn");
    m_programHW.setAttributeBuffer("textureIn", GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));

    m_vbo.release();
    m_vao.release();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader() {
    m_programSW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader);
    m_programSW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderSW);
    m_programSW.link();

    m_programHW.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader);
    m_programHW.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShaderHW);
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

    if (!m_vb) return;

    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    m_vb->unLock();

    if (!frame) return;

    if (frame->width != m_frameSize.width() || frame->height != m_frameSize.height()) {
        m_frameSize.setWidth(frame->width);
        m_frameSize.setHeight(frame->height);
    }

    QOpenGLVertexArrayObject::Binder vaoBinder(&m_vao);

    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        renderHardwareFrame(frame);
    } else {
        renderSoftwareFrame(frame);
    }
}

EGLImageKHR QYuvOpenGLWidget::createEGLImage(int fd, int offset, int pitch, int width, int height, uint64_t modifier) {
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

    return m_eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
}

void QYuvOpenGLWidget::renderHardwareFrame(const AVFrame *frame) {
    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
    if (!desc) return;
    const AVDRMPlaneDescriptor *planeY = &desc->layers[0].planes[0];
    const AVDRMObjectDescriptor &objY = desc->objects[planeY->object_index];
    
    const AVDRMPlaneDescriptor *planeUV = nullptr;
    if (desc->nb_layers > 1) planeUV = &desc->layers[1].planes[0];
    else if (desc->layers[0].nb_planes > 1) planeUV = &desc->layers[0].planes[1];
    if (!planeUV) return;
    const AVDRMObjectDescriptor &objUV = desc->objects[planeUV->object_index];

    // CREATE
    EGLImageKHR imgY = createEGLImage(objY.fd, planeY->offset, planeY->pitch, 
                                      frame->width, frame->height, objY.format_modifier);
    
    EGLImageKHR imgUV = createEGLImage(objUV.fd, planeUV->offset, planeUV->pitch, 
                                       frame->width, frame->height / 2, objUV.format_modifier);

    if (imgY == EGL_NO_IMAGE_KHR || imgUV == EGL_NO_IMAGE_KHR) {
         if (imgY) m_eglDestroyImageKHR(eglGetCurrentDisplay(), imgY);
         if (imgUV) m_eglDestroyImageKHR(eglGetCurrentDisplay(), imgUV);
         return;
    }

    m_programHW.bind();

    // BIND & DRAW
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, imgY);
    m_programHW.setUniformValue("tex_y", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, imgUV);
    m_programHW.setUniformValue("tex_uv_raw", 1);

    m_programHW.setUniformValue("width", (float)frame->width);
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    // DESTROY
    m_eglDestroyImageKHR(eglGetCurrentDisplay(), imgY);
    m_eglDestroyImageKHR(eglGetCurrentDisplay(), imgUV);
    
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