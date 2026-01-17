#ifndef VIDEOBUFFER_H
#define VIDEOBUFFER_H

#include <QObject>
#include <mutex>
#include <vector>
#include <memory>
#include <functional>

#include "fpscounter.h"

struct AVFrame;

class VideoBuffer : public QObject
{
    Q_OBJECT
public:
    explicit VideoBuffer(QObject *parent = nullptr);
    ~VideoBuffer();

    bool init();
    void deInit();

    void setRenderExpiredFrames(bool renderExpiredFrames);
    AVFrame *decodingFrame();
    void offerDecodedFrame(bool &previousFrameSkipped);
    const AVFrame *consumeRenderedFrame();
    void peekFrameInfo(int &width, int &height, int &format);
    void peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);
    void interrupt();

private:
    void swap();

private:
    std::mutex m_mutex; 

    AVFrame *m_decodingFrame = nullptr;
    AVFrame *m_renderingframe = nullptr;
    bool m_renderingFrameConsumed = true;
    bool m_renderExpiredFrames = false;
    FpsCounter m_fpsCounter;

    bool m_interrupted = false;

    int m_frameGen = 0;
    int m_cacheGen = 0;
    std::shared_ptr<std::vector<uint8_t>> m_cachedFrame;
    int m_cachedWidth = 0;
    int m_cachedHeight = 0;
    int m_cachedFormat = -1;
};

#endif // VIDEOBUFFER_H