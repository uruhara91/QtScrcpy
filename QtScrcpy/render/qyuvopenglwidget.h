#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <mutex>
#include <atomic>
#include <array>
#include <span>

class QYuvOpenGLWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core {
    Q_OBJECT
public:
    explicit QYuvOpenGLWidget(QWidget *parent = nullptr);
    ~QYuvOpenGLWidget();

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;
    
    void setFrameSize(const QSize &frameSize);
    const QSize &frameSize();

    void setFrameData(int width, int height,
                      std::span<const uint8_t> dataY,
                      std::span<const uint8_t> dataU,
                      std::span<const uint8_t> dataV,
                      int linesizeY, int linesizeU, int linesizeV);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

signals:
    void requestUpdateTextures(int w, int h, int strideY, int strideU, int strideV);

private:
    void initPBOs(int height, int strideY, int strideU, int strideV);
    void freePBOs();

private:
    // OpenGL Resources
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    QOpenGLShaderProgram m_program;
    std::array<GLuint, 3> m_textures = {0, 0, 0};

    static const int PBO_COUNT = 3;
    std::array<std::array<GLuint, 3>, PBO_COUNT> m_pbos;
    std::array<std::array<void*, 3>, PBO_COUNT> m_pboMapped = {{{nullptr}}};
    
    std::atomic<int> m_renderIndex = 0;
    std::atomic<int> m_readyIndex = -1;
    
    std::mutex m_initMutex; 
    std::atomic<bool> m_pboReady = false;
    
    QSize m_frameSize;
    int m_strides[3] = {0,0,0};
    bool m_needInit = true;
};

#endif // QYUVOPENGLWIDGET_H