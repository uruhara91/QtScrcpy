#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include "qyuvopenglwidget.h"
#include "../../QtScrcpyCore/src/device/decoder/videobuffer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
}

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 fourcc_code('R', '8', ' ', ' ')
#endif

// Vertices
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

// --- SHADER HARDWARE (SINGLE GIANT TEXTURE) ---
// Kita membaca Y dan UV dari satu texture R8 besar.
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
    uniform sampler2D tex_all;    // Satu texture R8 berisi Y dan UV
    uniform float uv_y_start;     // Posisi normalisasi (0.0-1.0) dimana UV mulai
    uniform float y_height_norm;  // Tinggi normalisasi area Y
    uniform float width;          // Lebar texture (untuk de-interleave UV)

    void main(void) {
        float y, u, v, r, g, b;

        // 1. Ambil Y
        // Y ada di bagian atas texture
        // Koordinat Y shader (0-1) harus dipetakan ke area Y di texture (0 - y_height_norm)
        vec2 coordY = vec2(textureOut.x, textureOut.y * y_height_norm);
        y = texture2D(tex_all, coordY).r;

        // 2. Ambil UV
        // UV ada di bagian bawah texture (mulai dari uv_y_start)
        // Kita harus de-interleave U dan V (U=genap, V=ganjil)
        
        float texelSize = 1.0 / width;
        
        // Cari koordinat X untuk U (genap terdekat)
        float u_x = (floor(textureOut.x * width / 2.0) * 2.0) * texelSize;
        float v_x = u_x + texelSize;
        
        // Koordinat Y untuk UV: dipetakan dari (0-1) ke area UV di texture
        // Area UV tingginya setengah Y.
        float uv_tex_y = uv_y_start + (textureOut.y * (y_height_norm / 2.0));
        
        u = texture2D(tex_all, vec2(u_x, uv_tex_y)).r - 0.5;
        v = texture2D(tex_all, vec2(v_x, uv_tex_y)).r - 0.5;

        // 3. Konversi
        r = y + 1.402 * v;
        g = y - 0.344136 * u - 0.714136 * v;
        b = y + 1.772 * u;

        gl_FragColor = vec4(r, g, b, 1.0);
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

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
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
    // Init semua sebagai TEXTURE_2D dengan NEAREST filter (penting untuk manual de-interleave)
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
    m_currentHWFrame = nullptr;
}

// --- DEBUGGER OTOMATIS ---
// Fungsi ini akan menulis info frame ke /tmp/qtscrcpy_debug.txt sekali saja
void dumpFrameInfo(const AVFrame *frame, const AVDRMFrameDescriptor *desc) {
    static bool dumped = false;
    if (dumped) return;
    
    QFile f("/tmp/qtscrcpy_debug.txt");
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&f);
        out << "=== QtScrcpy HW Debug Info ===\n";
        out << "Resolution: " << frame->width << "x" << frame->height << "\n";
        out << "Format: " << frame->format << "\n";
        out << "DRM Layers: " << desc->nb_layers << "\n";
        
        if (desc->nb_layers > 0) {
            out << "Layer 0 Format: " << Qt::hex << desc->layers[0].format << "\n";
            out << "Layer 0 Planes: " << desc->layers[0].nb_planes << "\n";
            for (int i = 0; i < desc->layers[0].nb_planes; ++i) {
                out << "  Plane " << i << ":\n";
                out << "    Object Index: " << desc->layers[0].planes[i].object_index << "\n";
                out << "    Offset: " << desc->layers[0].planes[i].offset << "\n";
                out << "    Pitch: " << desc->layers[0].planes[i].pitch << "\n";
            }
            // Check objects
            out << "DRM Objects:\n";
            for (int i = 0; i < desc->nb_objects; ++i) {
                out << "  Object " << i << ": FD=" << desc->objects[i].fd 
                    << " Size=" << desc->objects[i].size 
                    << " Mod=" << Qt::hex << desc->objects[i].format_modifier << "\n";
            }
        }
        f.close();
        qWarning() << "[HW] Debug info dumped to /tmp/qtscrcpy_debug.txt";
    }
    dumped = true;
}

void QYuvOpenGLWidget::renderHardwareFrame(const AVFrame *frame) {
    if (frame) {
        releaseHWFrame();
        m_currentHWFrame = frame;

        const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
        if (!desc || desc->nb_layers < 1) return;

        // DUMP DEBUG INFO (Tanpa Recompile lagi!)
        dumpFrameInfo(frame, desc);

        // --- STRATEGI: SINGLE TEXTURE ---
        // Kita hitung total tinggi texture yang dibutuhkan untuk menampung Y dan UV
        // Asumsi: UV plane ada setelah Y plane di memori yang sama
        // Offset Plane 1 = Lokasi mulai UV
        // Pitch Plane 0 = Stride
        
        uint32_t stride = desc->layers[0].planes[0].pitch;
        uint32_t uv_offset = desc->layers[0].planes[1].offset;
        uint32_t uv_size = desc->layers[0].planes[1].pitch * (frame->height / 2); // Estimasi size UV
        
        // Total bytes yang harus dicover texture
        uint32_t total_bytes = uv_offset + uv_size;
        
        // Hitung tinggi texture R8 yang dibutuhkan: (Total Bytes / Stride)
        int texture_height = (total_bytes + stride - 1) / stride;

        // Buat EGL Image Raksasa (R8)
        EGLint attribs[50];
        int i = 0;
        attribs[i++] = EGL_WIDTH;
        attribs[i++] = frame->width; // Asumsi Stride ~= Width
        attribs[i++] = EGL_HEIGHT;
        attribs[i++] = texture_height; // Tinggi yang disesuaikan
        attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        attribs[i++] = DRM_FORMAT_R8;
        
        attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attribs[i++] = desc->objects[0].fd; // Asumsi Plane 0 dan 1 di FD yang sama (Object 0)
        attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attribs[i++] = 0; // Mulai dari 0 (Awal Y)
        attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attribs[i++] = stride;
        
        if (desc->objects[0].format_modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attribs[i++] = desc->objects[0].format_modifier & 0xFFFFFFFF;
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attribs[i++] = desc->objects[0].format_modifier >> 32;
        }
        attribs[i++] = EGL_NONE;
        
        m_eglImageY = m_eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        
        if (m_eglImageY == EGL_NO_IMAGE_KHR) {
             qWarning() << "[HW] Failed to create Single Giant Texture!";
             return;
        }
        
        // Setup Uniforms untuk Shader
        // Hitung posisi normalisasi Y dan UV di dalam texture raksasa
        float y_h_norm = (float)frame->height / texture_height;
        float uv_start_norm = (float)uv_offset / stride / texture_height; // Baris ke berapa UV mulai
        
        m_programHW.bind();
        m_programHW.setUniformValue("y_height_norm", y_h_norm);
        m_programHW.setUniformValue("uv_y_start", uv_start_norm);
        m_programHW.setUniformValue("width", (float)frame->width);
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

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImageY);
    m_programHW.setUniformValue("tex_all", 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_programHW.disableAttributeArray(vertexLoc);
    m_programHW.disableAttributeArray(textureLoc);
    m_programHW.release();
}

void QYuvOpenGLWidget::renderSoftwareFrame() {
    if (!m_programSW.bind()) return;
    if (!m_vbo.bind()) return;
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