#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <span>
#include <atomic>

class VideoBuffer;
struct AVFrame;

class QYuvOpenGLWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core
{
    Q_OBJECT
public:
    explicit QYuvOpenGLWidget(QWidget *parent = nullptr);
    virtual ~QYuvOpenGLWidget() override;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    void setFrameData(int width, int height, 
                      std::span<const uint8_t> dataY, 
                      std::span<const uint8_t> dataUV, 
                      int linesizeY, int linesizeUV);
                      
    const QSize &frameSize();
    void setVideoBuffer(VideoBuffer *vb);

signals:
    void requestUpdateTextures(int width, int height);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void initShader();
    void initTextures(int width, int height);
    void deInitTextures();
    
    void initPBOs(int width, int height);
    void deInitPBOs();
    
    void setFrameSize(const QSize &frameSize);

private:
    QSize m_frameSize = { -1, -1 };
    VideoBuffer *m_vb = nullptr;
    
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLShaderProgram m_program;

    GLuint m_textures[2] = {0, 0}; 
    GLuint m_pbos[2][2] = {{0,0}, {0,0}};
    void* m_pboMappedPtrs[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};
    
    bool m_pboSizeValid = false;
    bool m_isInitialized = false;

    std::atomic<int> m_pboIndex = 0;
    std::atomic<bool> m_textureSizeMismatch = false;

    // GLsync m_fences[2] = {nullptr, nullptr};
};

#endif // QYUVOPENGLWIDGET_H