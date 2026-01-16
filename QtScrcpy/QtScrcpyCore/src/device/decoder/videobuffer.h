#ifndef VIDEOBUFFER_H
#define VIDEOBUFFER_H

#include <QObject>
#include <functional>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>

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

    void offerDecodedFrame(bool &previousFrameSkipped);

    const AVFrame *consumeRenderedFrame();

    AVFrame *decodingFrame();

    void peekFrameInfo(int &width, int &height, int &format);

    void peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

    void setRenderExpiredFrames(bool renderExpiredFrames);

    // Spinlock control
    void lock();
    void unLock();
    void interrupt();

signals:
    void updateFPS(int fps);

private:
    void swap();

private:
    AVFrame *m_decodingFrame = nullptr;
    AVFrame *m_renderingframe = nullptr;
    
    bool m_renderExpiredFrames = false;
    bool m_renderingFrameConsumed = true;
    
    FpsCounter m_fpsCounter;
    std::atomic_flag m_spinLock = ATOMIC_FLAG_INIT;
    std::atomic<bool> m_interrupted = false;

    std::shared_ptr<std::vector<uint8_t>> m_cachedFrame; 
    
    uint64_t m_frameGen = 0;
    uint64_t m_cacheGen = 0;
    
    int m_cachedWidth = 0;
    int m_cachedHeight = 0;
    int m_cachedFormat = -1;
};

#endif // VIDEOBUFFER_H