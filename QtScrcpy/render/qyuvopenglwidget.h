#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
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

    const QSize &frameSize();
    void setFrameSize(const QSize &frameSize);

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
    void initShader();
    void initTextures(int width, int height);
    void initPBOs(int height, int strideY, int strideU, int strideV);
    void deInitTextures();
    void deInitPBOs();

private:
    // Resources
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    QOpenGLShaderProgram m_program;
    std::array<GLuint, 3> m_textures = {0, 0, 0};

    // Double Buffer PBO
    std::array<std::array<GLuint, 3>, 2> m_pbos;
    std::array<std::array<void*, 3>, 2> m_pboMappedPtrs;
    std::array<int, 3> m_pboStrides = {0, 0, 0};

    // Sync
    std::atomic<int> m_pboIndex = 0; // 0 atau 1
    std::atomic_flag m_updatePending = ATOMIC_FLAG_INIT;
    std::mutex m_pboLock;

    // State
    QSize m_frameSize;
    bool m_isInitialized = false;
    bool m_pboSizeValid = false;
    bool m_textureSizeMismatch = false;
};

#endif // QYUVOPENGLWIDGET_H