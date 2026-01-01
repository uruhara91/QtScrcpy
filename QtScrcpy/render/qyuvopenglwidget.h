#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMutex>
#include <QMap>
#include <QPair>
#include <QList>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> 
#include <libdrm/drm_fourcc.h>

// Forward Declaration
class VideoBuffer;
struct AVFrame;
struct AVDRMPlaneDescriptor;
struct AVDRMObjectDescriptor;

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

    // Legacy method
    void updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV);

    // Zero Copy Interface
    void setVideoBuffer(VideoBuffer *vb);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void initShader();
    void initTextures();
    void deInitTextures();
    
    // Logic render Software
    void renderSoftwareFrame();
    // Logic render Hardware
    void renderHardwareFrame(const AVFrame *frame);

    // Helper cleanup
    // void releaseHWFrame();

private:
    QSize m_frameSize = { -1, -1 };
    VideoBuffer *m_vb = nullptr;
    
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;
    
    QOpenGLShaderProgram m_programSW;
    QOpenGLShaderProgram m_programHW;

    // index 0-2: Y, U, V textures (SW)
    // HW Mode will reuse index 0 (Y) and 1 (UV)
    GLuint m_textures[4] = {0, 0, 0, 0};

    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;

    // --- EGL Zero-Copy Resources ---
    EGLImageKHR m_eglImageY = EGL_NO_IMAGE_KHR;
    EGLImageKHR m_eglImageUV = EGL_NO_IMAGE_KHR;
    
    const AVFrame *m_currentHWFrame = nullptr;

    // EGL Image Cache Structure
    struct EGLImageCacheEntry {
        EGLImageKHR image;
        int width;
        int height;
    };
    
    QMap<QPair<int, int>, EGLImageCacheEntry> m_eglImageCache;
    
    QList<QPair<int, int>> m_cacheRecentUse; 

    void flushEGLCache(); 
    EGLImageKHR getCachedEGLImage(int fd, int offset, int pitch, int width, int height, uint64_t modifier);
};

#endif // QYUVOPENGLWIDGET_H