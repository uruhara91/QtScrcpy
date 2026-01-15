#ifndef VIDEOBUFFER_H
#define VIDEOBUFFER_H

#include <QObject>
#include <atomic>
#include <QWaitCondition>
#include <functional>
#include <span>

#include "fpscounter.h"

struct AVFrame;

class VideoBuffer : public QObject
{
    Q_OBJECT
public:
    explicit VideoBuffer(QObject *parent = nullptr);
    virtual ~VideoBuffer();

    bool init();
    void deInit();

    // Spinlock API
    void lock();
    void unLock();

    AVFrame *decodingFrame();
    void offerDecodedFrame(bool &previousFrameSkipped);
    const AVFrame *consumeRenderedFrame();

    void peekFrameInfo(int &width, int &height, int &format);
    void peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

    void setRenderExpiredFrames(bool renderExpiredFrames);
    void interrupt();

signals:
    void updateFPS(int fps);

private:
    void swap();

private:
    AVFrame *m_decodingFrame = nullptr;
    AVFrame *m_renderingframe = nullptr;

    std::atomic_flag m_spinLock = ATOMIC_FLAG_INIT;

    bool m_renderExpiredFrames = false;
    bool m_renderingFrameConsumed = true;
    bool m_interrupted = false;

    FpsCounter m_fpsCounter;
};

#endif // VIDEOBUFFER_H