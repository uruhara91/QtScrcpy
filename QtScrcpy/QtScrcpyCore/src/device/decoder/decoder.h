#ifndef DECODER_H
#define DECODER_H
#include <QObject>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavutil/hwcontext.h"
}

#include <functional>

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
    bool initHWDecoder(const AVCodec *codec);

private:
    VideoBuffer *m_vb = Q_NULLPTR;
    AVCodecContext *m_codecCtx = Q_NULLPTR;
    AVBufferRef *m_hwDeviceCtx = Q_NULLPTR;
    
    bool m_isCodecCtxOpen = false;
    std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> m_onFrame = Q_NULLPTR;
};

#endif // DECODER_H