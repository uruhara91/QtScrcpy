#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <span>
#include <array>
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

    // Fungsi utama penerima frame dari decoder
    void setFrameData(int width, int height, 
                      std::span<const uint8_t> dataY, 
                      std::span<const uint8_t> dataU, 
                      std::span<const uint8_t> dataV, 
                      int linesizeY, int linesizeU, int linesizeV);
    
    const QSize &frameSize();
    void setVideoBuffer(VideoBuffer *vb);
    void updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV);

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

    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    QOpenGLShaderProgram m_program;

    std::array<GLuint, 3> m_textures = {0, 0, 0}; 
    std::array<std::array<GLuint, 3>, 2> m_pbos = {{ {0,0,0}, {0,0,0} }};
    
    std::array<std::array<void*, 3>, 2> m_pboMappedPtrs = {{ {nullptr}, {nullptr} }};
    std::array<int, 3> m_pboStrides = {0, 0, 0};
    
    bool m_pboSizeValid = false;
    bool m_isInitialized = false;

    std::atomic<int> m_pboIndex = 0;
    std::atomic<bool> m_textureSizeMismatch = false;
    std::atomic<bool> m_updatePending = false;

    std::atomic_flag m_pboLock = ATOMIC_FLAG_INIT;
};

#endif // QYUVOPENGLWIDGET_H