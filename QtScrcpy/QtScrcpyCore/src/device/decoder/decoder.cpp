#include <QDebug>
#include <QThread>
#include "compat.h"
#include "decoder.h"
#include "videobuffer.h"

Decoder::Decoder(std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> onFrame, QObject *parent)
    : QObject(parent)
    , m_vb(new VideoBuffer())
    , m_onFrame(onFrame)
{
    m_vb->init();
    connect(this, &Decoder::newFrame, this, &Decoder::onNewFrame, Qt::QueuedConnection);
    connect(m_vb, &VideoBuffer::updateFPS, this, &Decoder::updateFPS);
}

Decoder::~Decoder() {
    close();
    if (m_vb) {
        m_vb->deInit();
        delete m_vb;
        m_vb = Q_NULLPTR;
    }
}

bool Decoder::open()
{
    QThread::currentThread()->setPriority(QThread::TimeCriticalPriority);

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qCritical("H.264 decoder not found");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        qCritical("Could not allocate decoder context");
        return false;
    }

    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->thread_type = FF_THREAD_SLICE;
    m_codecCtx->thread_count = 0; 

    // Open Codec
    if (avcodec_open2(m_codecCtx, codec, NULL) < 0) {
        qCritical("Could not open H.264 codec");
        return false;
    }
    
    m_isCodecCtxOpen = true;
    qInfo("SW Decoder initialized. Threads: %d, Type: Slice", m_codecCtx->thread_count);
    return true;
}

void Decoder::close()
{
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = Q_NULLPTR;
    }
    m_isCodecCtxOpen = false;
}

bool Decoder::push(const AVPacket *packet)
{
    if (!m_codecCtx || !m_isCodecCtxOpen) {
        return false;
    }
    
    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0) {
        qCritical("Could not send video packet: %d", ret);
        return false;
    }

    AVFrame *decodingFrame = m_vb->decodingFrame();
    
    ret = avcodec_receive_frame(m_codecCtx, decodingFrame);
    if (ret == 0) {
        pushFrame();
    } else if (ret != AVERROR(EAGAIN)) {
        // Error serius (bukan sekadar butuh data lagi)
        qWarning("Decoder receive error: %d", ret);
        return false;
    }
    
    return true;
}

void Decoder::peekFrame(std::function<void (int, int, uint8_t *)> onFrame)
{
    if (!m_vb) {
        return;
    }
    m_vb->peekRenderedFrame(onFrame);
}

void Decoder::pushFrame()
{
    if (!m_vb) {
        return;
    }
    bool previousFrameSkipped = true;
    m_vb->offerDecodedFrame(previousFrameSkipped);
    if (previousFrameSkipped) {
        return;
    }
    emit newFrame();
}

void Decoder::onNewFrame()
{
    if (!m_vb) return;

    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    
    if (m_onFrame && frame) {
         // SW Decode = YUV420P (3 Planes: Y, U, V)
         m_onFrame(frame->width, frame->height,
                   frame->data[0], frame->data[1], frame->data[2],
                   frame->linesize[0], frame->linesize[1], frame->linesize[2]);
    }
    m_vb->unLock();
}