#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMutex>

// --- System Headers for Zero Copy ---
// Menggunakan header standar Linux/Mesa
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> 
#include <libdrm/drm_fourcc.h>

// Forward declarations
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
    void setVideoBuffer(VideoBuffer *vb);

    // Legacy support (SW Decode Path)
    void updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    // --- Internal Helpers ---
    void initShader();
    void initTextures();
    void deInitTextures();
    
    // Core Rendering Logic
    void renderSoftwareFrame(const AVFrame *frame);
    void renderHardwareFrame(const AVFrame *frame);

    // EGL Helper Wrapper
    EGLImageKHR createEGLImage(int fd, int offset, int pitch, int width, int height, uint64_t modifier);

    // --- EGL Extension Definitions & Members ---
    // Kita definisikan manual typedef-nya agar portable dan robust
    // meskipun header sistem kadang tidak lengkap.
    
    // Image Imports (DMABUF)
    typedef EGLImageKHR (EGLAPIENTRYP PFNEGLCREATEIMAGEKHRPROC) (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
    typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYIMAGEKHRPROC) (EGLDisplay dpy, EGLImageKHR image);
    typedef void (EGLAPIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, GLeglImageOES image);

    // Explicit Synchronization (Fences)
    typedef void *EGLSyncKHR;
    typedef EGLSyncKHR (EGLAPIENTRYP PFNEGLCREATESYNCKHRPROC) (EGLDisplay dpy, EGLenum type, const EGLint *attrib_list);
    typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYSYNCKHRPROC) (EGLDisplay dpy, EGLSyncKHR sync);
    typedef EGLint (EGLAPIENTRYP PFNEGLCLIENTWAITSYNCKHRPROC) (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout);

    // Constants Guards (Safety First)
    #ifndef EGL_SYNC_FENCE_KHR
    #define EGL_SYNC_FENCE_KHR 0x30F9
    #endif
    #ifndef EGL_SYNC_FLUSH_COMMANDS_BIT_KHR
    #define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
    #endif

    // Function Pointers (Loaded at runtime via eglGetProcAddress)
    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;

    PFNEGLCREATESYNCKHRPROC m_eglCreateSyncKHR = nullptr;
    PFNEGLDESTROYSYNCKHRPROC m_eglDestroySyncKHR = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC m_eglClientWaitSyncKHR = nullptr;

private:
    // State Management
    QSize m_frameSize = { -1, -1 };
    VideoBuffer *m_vb = nullptr;
    int m_lastWidth = -1; // Untuk optimasi uniform update
    
    // OpenGL Resources
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLShaderProgram m_programSW;
    QOpenGLShaderProgram m_programHW;
    GLuint m_textures[4] = {0, 0, 0, 0}; // 0-2: SW YUV, 0-1: HW Y/UV (Reuse index 0 & 1)
};

#endif // QYUVOPENGLWIDGET_H