#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QMutex>

// --- EGL & DRM Dependencies ---
#include <EGL/egl.h>
#include <EGL/eglext.h>
// Pastikan libdrm terinstall (paket 'libdrm' di Arch/CachyOS)
#include <libdrm/drm_fourcc.h>

// Forward Declaration
class VideoBuffer;
struct AVFrame;

class QYuvOpenGLWidget
    : public QOpenGLWidget
    , protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit QYuvOpenGLWidget(QWidget *parent = nullptr);
    virtual ~QYuvOpenGLWidget() override;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    void setFrameSize(const QSize &frameSize);
    const QSize &frameSize();

    // Legacy method (masih disimpan untuk compatibility fallback)
    void updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV);

    // --- NEW: Zero Copy Interface ---
    // Kita set VideoBuffer agar widget bisa mengambil frame HW langsung
    void setVideoBuffer(VideoBuffer *vb);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void initShader();
    void initTextures();
    void deInitTextures();
    
    // Logic render Software (YUV 3 Planes)
    void renderSoftwareFrame();
    // Logic render Hardware (EGL DMA-BUF)
    void renderHardwareFrame(const AVFrame *frame);

    // Helper untuk membersihkan frame HW sebelumnya
    void releaseHWFrame();

private:
    QSize m_frameSize = { -1, -1 };
    
    // --- Video Source ---
    VideoBuffer *m_vb = nullptr;

    // --- OpenGL Resources ---
    QOpenGLBuffer m_vbo;
    
    // Shader untuk Software Decode (YUV -> RGB conversion manual)
    QOpenGLShaderProgram m_programSW;
    // Shader untuk Hardware Decode (SamplerExternalOES handles conversion)
    QOpenGLShaderProgram m_programHW;

    // Texture IDs
    // index 0-2: Y, U, V textures (SW)
    // index 3: External OES texture (HW)
    GLuint m_textures[4] = {0, 0, 0, 0};

    // --- EGL Zero-Copy Resources ---
    EGLImageKHR m_eglImage = EGL_NO_IMAGE_KHR;
    const AVFrame *m_currentHWFrame = nullptr; // Keep reference to prevent generic release

    // --- Function Pointers untuk Ekstensi EGL ---
    // Qt tidak selalu mengekspos ini secara langsung, jadi kita load manual
    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;
};

#endif // QYUVOPENGLWIDGET_H