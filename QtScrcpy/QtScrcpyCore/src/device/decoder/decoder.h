#ifndef DECODER_H
#define DECODER_H

#include <QObject>
#include <functional>
#include <memory>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavutil/pixfmt.h"
}

struct AVCodecContext;
struct AVPacket;
struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const;
};

class VideoBuffer;

class Decoder : public QObject
{
    Q_OBJECT
public:
    Decoder(std::function<void(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV)> onFrame, QObject *parent = Q_NULLPTR);
    virtual ~Decoder();

    bool open();
    void close();
    bool push(const AVPacket *packet);
 
    void peekFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

    VideoBuffer* videoBuffer() const { return m_vb; }

signals:
    void updateFPS(quint32 fps);
    void newFrame();

private slots:
    void onNewFrame();

private:
    void pushFrame();

private:
    VideoBuffer *m_vb = Q_NULLPTR;
    
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> m_codecCtx;
    
    bool m_isCodecCtxOpen = false;
    
    // Callback UI/Render
    std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> m_onFrame;
};

#endif // DECODER_H