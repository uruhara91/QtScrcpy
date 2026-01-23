#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <atomic>
#include <mutex>
#include <array>
#include <span>

constexpr int PBO_COUNT = 3; 

class QYuvOpenGLWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core
{
    Q_OBJECT
public:
    explicit QYuvOpenGLWidget(QWidget *parent = nullptr);
    ~QYuvOpenGLWidget() override;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    void setFrameData(int width, int height, 
                      std::span<const uint8_t> dataY, 
                      std::span<const uint8_t> dataU, 
                      std::span<const uint8_t> dataV, 
                      int linesizeY, int linesizeU, int linesizeV);

signals:
    void requestUpdateTextures(int width, int height, int strideY, int strideU, int strideV);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void initShader();
    void initTextures(int width, int height);
    void deInitTextures();
    
    void initPBOs(int height, int strideY, int strideU, int strideV);
    void deInitPBOs();
    
    void updateTexture(int plane, int width, int height, int stride);

private:
    QSize m_frameSize = { -1, -1 };

    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    QOpenGLShaderProgram m_program;

    std::array<GLuint, 3> m_textures = {0, 0, 0}; 
    std::array<std::array<GLuint, 3>, PBO_COUNT> m_pbos = {};
    std::array<std::array<std::byte*, 3>, PBO_COUNT> m_pboMappedPtrs = {};
    std::array<int, 3> m_pboStrides = {0, 0, 0};
    
    bool m_pboSizeValid = false;
    bool m_isInitialized = false;

    std::atomic<int> m_writeIndex = 0;
    std::atomic<int> m_readIndex = 0;
    std::atomic<bool> m_textureSizeMismatch = false;
    std::atomic_flag m_updatePending = ATOMIC_FLAG_INIT;
    std::mutex m_pboLock;
};

#endif // QYUVOPENGLWIDGET_H