#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMutex>
#include <QHash> // Wajib untuk Caching

// --- System Headers for Zero Copy ---
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> 
#include <libdrm/drm_fourcc.h>

// Forward declarations
class VideoBuffer;
struct AVFrame;

// --- CACHE KEY DEFINITION ---
// Identitas unik untuk setiap DMA-BUF plane
struct EGLImageKey {
    int fd;
    int offset;
    int pitch;
    uint64_t modifier;

    bool operator==(const EGLImageKey &other) const {
        return fd == other.fd && 
               offset == other.offset && 
               pitch == other.pitch && 
               modifier == other.modifier;
    }
};

// Global Hash Function agar bisa masuk QHash
inline uint qHash(const EGLImageKey &key, uint seed = 0) {
    // XOR hash sederhana tapi efektif untuk kombinasi unik ini
    return qHash(key.fd, seed) ^ qHash(key.offset, seed) ^ qHash(key.pitch, seed);
}

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

    // --- EGL CACHING SYSTEM ---
    // Mengambil Image dari cache atau membuat baru jika belum ada
    EGLImageKHR getCachedEGLImage(int fd, int offset, int pitch, int width, int height, uint64_t modifier);
    
    // Membersihkan seluruh cache (Hapus EGLImage dari GPU)
    void cleanCache(); 

    // --- EGL Extension Definitions & Members ---
    typedef EGLImageKHR (EGLAPIENTRYP PFNEGLCREATEIMAGEKHRPROC) (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
    typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYIMAGEKHRPROC) (EGLDisplay dpy, EGLImageKHR image);
    typedef void (EGLAPIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, GLeglImageOES image);

    // Explicit Synchronization (Fences)
    typedef void *EGLSyncKHR;
    typedef EGLSyncKHR (EGLAPIENTRYP PFNEGLCREATESYNCKHRPROC) (EGLDisplay dpy, EGLenum type, const EGLint *attrib_list);
    typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYSYNCKHRPROC) (EGLDisplay dpy, EGLSyncKHR sync);
    typedef EGLint (EGLAPIENTRYP PFNEGLCLIENTWAITSYNCKHRPROC) (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, uint64_t timeout);

    // Constants Guards
    #ifndef EGL_SYNC_FENCE_KHR
    #define EGL_SYNC_FENCE_KHR 0x30F9
    #endif
    #ifndef EGL_SYNC_FLUSH_COMMANDS_BIT_KHR
    #define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
    #endif

    // Function Pointers
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
    int m_lastWidth = -1;
    
    // OpenGL Resources
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLShaderProgram m_programSW;
    QOpenGLShaderProgram m_programHW;
    GLuint m_textures[4] = {0, 0, 0, 0}; 

    // --- CACHE STORAGE ---
    // Map: [Identitas Unik Buffer] -> [Handle Gambar di GPU]
    // Ini mencegah Create/Destroy berulang untuk buffer yang sama (Pool Recycling)
    QHash<EGLImageKey, EGLImageKHR> m_eglImageCache;
};

#endif // QYUVOPENGLWIDGET_H