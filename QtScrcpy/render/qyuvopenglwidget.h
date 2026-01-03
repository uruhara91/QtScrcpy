#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMutex>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> 
#include <libdrm/drm_fourcc.h>

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
    void setVideoBuffer(VideoBuffer *vb);

    // Legacy support
    void updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void initShader();
    void initTextures();
    void deInitTextures();
    
    void renderSoftwareFrame(const AVFrame *frame);
    void renderHardwareFrame(const AVFrame *frame);

    // Helper EGL
    EGLImageKHR createEGLImage(int fd, int offset, int pitch, int width, int height, uint64_t modifier);

private:
    QSize m_frameSize = { -1, -1 };
    VideoBuffer *m_vb = nullptr;

    int m_lastWidth = -1;
    
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;
    
    QOpenGLShaderProgram m_programSW;
    QOpenGLShaderProgram m_programHW;

    GLuint m_textures[4] = {0, 0, 0, 0};

    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;
};

#endif // QYUVOPENGLWIDGET_H