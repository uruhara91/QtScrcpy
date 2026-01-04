#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMutex>

// --- System Headers for Zero Copy ---
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

    // EGL Helper
    EGLImageKHR createEGLImage(int fd, int offset, int pitch, int width, int height, uint64_t modifier);

    // --- EGL Extension Definitions & Members ---
    // Defined explicitly for robustness across different Mesa versions
    
    // 1. Image Management (DMABUF Import)
    typedef EGLImageKHR (EGLAPIENTRYP PFNEGLCREATEIMAGEKHRPROC) (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
    typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYIMAGEKHRPROC) (EGLDisplay dpy, EGLImageKHR image);
    typedef void (EGLAPIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, GLeglImageOES image);

    // 2. Explicit Synchronization (The Fence)
    typedef void *EGLSyncKHR;
    typedef EGLSyncKHR (EGLAPIENTRYP PFNEGLCREATESYNCKHRPROC) (EGLDisplay dpy, EGLenum type, const EGLint *attrib_list);
    typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYSYNCKHRPROC) (EGLDisplay dpy, EGLSyncKHR sync);
    
    // CPU Wait (Client-Side) - Used for cleanup check
    typedef EGLint (EGLAPIENTRYP PFNEGLCLIENTWAITSYNCKHRPROC) (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout);
    
    // GPU Wait (Server-Side) - NEW: The secret sauce for non-blocking sync
    typedef EGLint (EGLAPIENTRYP PFNEGLWAITSYNCKHRPROC) (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags);

    // Constants Guards (Safety First)
    #ifndef EGL_SYNC_FENCE_KHR
    #define EGL_SYNC_FENCE_KHR 0x30F9
    #endif
    #ifndef EGL_SYNC_FLUSH_COMMANDS_BIT_KHR
    #define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
    #endif
    #ifndef EGL_NO_SYNC_KHR
    #define EGL_NO_SYNC_KHR ((EGLSyncKHR)0)
    #endif

    // Function Pointers (Loaded at runtime)
    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;

    PFNEGLCREATESYNCKHRPROC m_eglCreateSyncKHR = nullptr;
    PFNEGLDESTROYSYNCKHRPROC m_eglDestroySyncKHR = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC m_eglClientWaitSyncKHR = nullptr;
    PFNEGLWAITSYNCKHRPROC m_eglWaitSyncKHR = nullptr; // New member

private:
    // State Management
    QSize m_frameSize = { -1, -1 };
    VideoBuffer *m_vb = nullptr;
    int m_lastWidth = -1; // Optimization for uniform update
    
    // OpenGL Resources
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;
    
    // Shader Programs
    QOpenGLShaderProgram m_programSW;
    QOpenGLShaderProgram m_programHW;

    // Textures (0-2: SW YUV, 0-1: HW Y/UV reused)
    GLuint m_textures[4] = {0, 0, 0, 0}; 
};

#endif // QYUVOPENGLWIDGET_H