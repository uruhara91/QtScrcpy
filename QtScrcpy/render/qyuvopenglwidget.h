#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

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

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void initShader();
    void initTextures();
    void initPBOs(int width, int height);
    void deInitTextures();
    void deInitPBOs();
    
    // Core Rendering Logic
    void renderFrame(const AVFrame *frame);

private:
    // State Management
    QSize m_frameSize = { -1, -1 };
    VideoBuffer *m_vb = nullptr;
    
    // OpenGL Resources
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLShaderProgram m_program;

    // Textures: 3 (Y, U, V)
    GLuint m_textures[3] = {0, 0, 0}; 

    // PBOs: Double Buffer (2 Sets) x 3 Planes (Y, U, V)
    // Index 0: Y-Plane, 1: U-Plane, 2: V-Plane
    GLuint m_pbos[2][3] = {{0,0,0}, {0,0,0}};
    int m_pboIndex = 0;
    bool m_pboSizeValid = false;
};

#endif // QYUVOPENGLWIDGET_H