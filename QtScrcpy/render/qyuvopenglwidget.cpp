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

// --- SHADER HARDWARE (External OES) ---
// Perhatikan extension GL_OES_EGL_image_external
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
    // Tidak perlu extension OES/ARB lagi karena kita pakai sampler2D
    varying vec2 textureOut;
    uniform sampler2D tex_external; // Ganti samplerExternalOES jadi sampler2D

    void main(void) {
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

    // 1. Load EGL Extensions (Manual Load)
    // Fungsi ini wajib ada untuk Zero-Copy
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

    // 4. Init Textures (Gen IDs)
    initTextures();

    // Default clear color (Black)
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
    // Generate 4 textures
    glGenTextures(4, m_textures);

    // Init SW Textures (0, 1, 2) - Biarkan sama
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // --- UBAH BAGIAN INI ---
    // Init HW Texture (index 3) sebagai GL_TEXTURE_2D juga!
    glBindTexture(GL_TEXTURE_2D, m_textures[3]); 
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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

    // Ambil frame dari VideoBuffer
    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    m_vb->unLock();

    // Jika tidak ada frame baru, mungkin kita perlu repaint frame HW terakhir?
    // Untuk simplifikasi, kita asumsikan render loop dipanggil saat ada frame baru.
    // Tapi jika frame == null, kita bisa cek apakah kita masih punya m_currentHWFrame.
    
    if (!frame && m_currentHWFrame) {
        // Repaint frame HW terakhir (misal saat window resize)
        renderHardwareFrame(nullptr); // nullptr trigers repaint logic
        return;
    }

    if (!frame) return;

    // Cek tipe Frame
    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        // --- HARDWARE PATH ---
        renderHardwareFrame(frame);
    } else {
        // --- SOFTWARE PATH (Legacy) ---
        // Jika karena suatu alasan decoder fallback ke SW
        // Kita butuh data YUV manual. Di struktur baru, ini mungkin perlu penyesuaian
        // tapi untuk sekarang kita fokus ke HW.
        
        // Release HW frame jika sebelumnya kita render HW
        releaseHWFrame();
        
        // Render SW logic (simplified call)
        // Disini kita harusnya memanggil updateTextures dengan data dari frame->data
        updateTextures(frame->data[0], frame->data[1], frame->data[2], 
                       frame->linesize[0], frame->linesize[1], frame->linesize[2]);
        
        // Software frame biasanya di-copy datanya oleh updateTextures, jadi AVFrame bisa dilepas
        // Tapi hati-hati: di VideoBuffer, pointer frame ini milik VideoBuffer.
        // Jangan unref frame di sini kecuali kita yakin.
    }
}

void QYuvOpenGLWidget::releaseHWFrame()
{
    if (m_eglImage != EGL_NO_IMAGE_KHR) {
        m_eglDestroyImageKHR(eglGetCurrentDisplay(), m_eglImage);
        m_eglImage = EGL_NO_IMAGE_KHR;
    }
    
    // Kita tidak meng-unref frame di sini karena kepemilikan frame ada di VideoBuffer
    // VideoBuffer akan menggunakan frame ini lagi (double buffering).
    // Tapi kita perlu menullkan pointer render kita.
    m_currentHWFrame = nullptr;
}

void QYuvOpenGLWidget::renderHardwareFrame(const AVFrame *frame)
{
    if (frame) {
        // 1. Clean up old image
        releaseHWFrame();
        m_currentHWFrame = frame;

        const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
        if (!desc) {
            qWarning() << "[HW] Error: DRM Descriptor is NULL";
            return;
        }

        // 3. Create EGL Image Attribute List
        EGLint attribs[50];
        int i = 0;
        attribs[i++] = EGL_WIDTH;
        attribs[i++] = frame->width;
        attribs[i++] = EGL_HEIGHT;
        attribs[i++] = frame->height;
        attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        attribs[i++] = desc->layers[0].format;

        // Plane 0
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

        // Plane 1 (UV) - Biasanya NV12 butuh ini
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

        // 4. Create EGL Image
        m_eglImage = m_eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        
        if (m_eglImage == EGL_NO_IMAGE_KHR) {
            // Log Error EGL jika gagal
            EGLint error = eglGetError();
            qWarning() << "[HW] Failed to create EGLImage! Error Code:" << Qt::hex << error;
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

    // --- UBAH BAGIAN BINDING INI ---
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[3]); // Gunakan GL_TEXTURE_2D
    
    // Bind EGL Image ke Texture 2D
    // Driver Intel di Linux biasanya support target GL_TEXTURE_2D
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_eglImage);
    
    // Cek error lagi untuk memastikan
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        qWarning() << "[HW] GL Error after EGLImageTargetTexture2DOES:" << Qt::hex << err;
    }
    
    m_programHW.setUniformValue("tex_external", 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_programHW.disableAttributeArray(vertexLocation);
    m_programHW.disableAttributeArray(textureLocation);
    m_programHW.release();
}

void QYuvOpenGLWidget::renderSoftwareFrame()
{
    // 1. Bind Shader Software (YUV -> RGB)
    if (!m_programSW.bind()) {
        return;
    }

    // 2. Bind VBO (Vertex Data)
    if (!m_vbo.bind()) {
        return;
    }

    // 3. Setup Attributes (Vertex & Texture Coords)
    // Pastikan nama attribute sesuai dengan s_vertShaderSW ("vertexIn", "textureIn")
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

    // 4. Set Uniform Samplers (texture unit 0, 1, 2)
    m_programSW.setUniformValue("tex_y", 0);
    m_programSW.setUniformValue("tex_u", 1);
    m_programSW.setUniformValue("tex_v", 2);
    
    // Note: Kita tidak melakukan glDrawArrays di sini.
    // Draw dilakukan di updateTextures() setelah glTexImage2D upload selesai.
}

// --- LEGACY SUPPORT ---
void QYuvOpenGLWidget::updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV)
{
    // Jika kita masuk sini, berarti mode SW.
    renderSoftwareFrame(); // Setup shader SW
    
    // ... Logic upload texture lama (glTexSubImage2D) ...
    // Karena kode ini panjang dan kamu fokus ke HW, saya singkat bagian ini.
    // Intinya: Logic lama yang ada di file aslimu tetap valid untuk texture[0], [1], [2].
    // Yang penting jangan lupa bind m_programSW sebelum draw.
    
    // Implementasi singkat untuk kelengkapan:
    m_programSW.bind();
    m_vbo.bind();
    
    // Setup Attributes (sama seperti HW)
    int vertexLocation = m_programSW.attributeLocation("vertexIn");
    m_programSW.enableAttributeArray(vertexLocation);
    m_programSW.setAttributeBuffer(vertexLocation, GL_FLOAT, 0, 3, 5 * sizeof(GLfloat));
    int textureLocation = m_programSW.attributeLocation("textureIn");
    m_programSW.enableAttributeArray(textureLocation);
    m_programSW.setAttributeBuffer(textureLocation, GL_FLOAT, 3 * sizeof(GLfloat), 2, 5 * sizeof(GLfloat));

    // Upload Y
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeY, m_frameSize.height(), 0, GL_RED, GL_UNSIGNED_BYTE, dataY);
    m_programSW.setUniformValue("tex_y", 0);

    // Upload U
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeU, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataU);
    m_programSW.setUniformValue("tex_u", 1);

    // Upload V
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, linesizeV, m_frameSize.height()/2, 0, GL_RED, GL_UNSIGNED_BYTE, dataV);
    m_programSW.setUniformValue("tex_v", 2);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    m_programSW.release();
}
