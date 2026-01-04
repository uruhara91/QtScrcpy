#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLExtraFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

class VideoBuffer;
struct AVFrame;

class QYuvOpenGLWidget
    : public QOpenGLWidget
    , protected QOpenGLExtraFunctions
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

    void updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void initShader();
    void initTextures();
    void deInitTextures();
    
    void initPBOs(int width, int height);
    void deInitPBOs();
    
    void renderFrame(const AVFrame *frame);

private:
    QSize m_frameSize = { -1, -1 };
    VideoBuffer *m_vb = nullptr;
    
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLShaderProgram m_program;

    GLuint m_textures[3] = {0, 0, 0}; 

    GLuint m_pbos[2][3] = {{0,0,0}, {0,0,0}};
    int m_pboIndex = 0;
    bool m_pboSizeValid = false;
};

#endif // QYUVOPENGLWIDGET_H