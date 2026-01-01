#ifndef VIDEO_BUFFER_H
#define VIDEO_BUFFER_H

#include <QMutex>
#include <QWaitCondition>
#include <QObject>

#include <functional>
#include "fpscounter.h"

// forward declarations
typedef struct AVFrame AVFrame;

class VideoBuffer : public QObject
{
    Q_OBJECT
public:
    VideoBuffer(QObject *parent = Q_NULLPTR);
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
    AVFrame *m_decodingFrame = Q_NULLPTR;
    AVFrame *m_renderingframe = Q_NULLPTR;
    QMutex m_mutex;
    bool m_renderingFrameConsumed = true;
    FpsCounter m_fpsCounter;

    bool m_renderExpiredFrames = false;
    QWaitCondition m_renderingFrameConsumedCond;

    bool m_interrupted = false;
};

#endif // VIDEO_BUFFER_H
