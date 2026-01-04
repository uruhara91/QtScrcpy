#ifndef VIDEO_BUFFER_H
#define VIDEO_BUFFER_H

#include <QMutex>
#include <QWaitCondition>
#include <QObject>
#include <functional>

#include "fpscounter.h"

// Forward declarations
struct AVFrame;

class VideoBuffer : public QObject
{
    Q_OBJECT
public:
    explicit VideoBuffer(QObject *parent = nullptr);
    virtual ~VideoBuffer();

    bool init();
    void deInit();

    void lock();
    void unLock();
    
    void setRenderExpiredFrames(bool renderExpiredFrames);

    AVFrame *decodingFrame();
    void offerDecodedFrame(bool &previousFrameSkipped);

    const AVFrame *consumeRenderedFrame();

    void peekFrameInfo(int &width, int &height, int &format);
    void peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

    void interrupt();

signals:
    void updateFPS(quint32 fps);

private:
    void swap();

private:
    // Double Buffer
    AVFrame *m_decodingFrame = nullptr;
    AVFrame *m_renderingframe = nullptr;
    
    QMutex m_mutex;
    bool m_renderingFrameConsumed = true;
    FpsCounter m_fpsCounter;

    bool m_renderExpiredFrames = false;
    QWaitCondition m_renderingFrameConsumedCond;

    bool m_interrupted = false;
};

#endif // VIDEO_BUFFER_H