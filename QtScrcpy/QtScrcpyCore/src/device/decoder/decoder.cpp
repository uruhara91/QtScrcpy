#include <QDebug>
#include <QThread>
#include "compat.h"
#include "decoder.h"
#include "videobuffer.h"

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    (void)ctx;
    
    const enum AVPixelFormat *p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_VAAPI) {
            return *p;
        }
    }

    qWarning("VAAPI hardware format not found, falling back to software.");
    return pix_fmts[0];
}

Decoder::Decoder(std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> onFrame, QObject *parent)
    : QObject(parent)
    , m_vb(new VideoBuffer())
    , m_onFrame(onFrame)
{
    m_vb->init();
    connect(this, &Decoder::newFrame, this, &Decoder::onNewFrame, Qt::QueuedConnection);
    connect(m_vb, &VideoBuffer::updateFPS, this, &Decoder::updateFPS);

    m_tempFrame = av_frame_alloc();
}

Decoder::~Decoder() {
    close();
    if (m_vb) {
        m_vb->deInit();
        delete m_vb;
        m_vb = Q_NULLPTR;
    }

        if (m_tempFrame) {
        av_frame_free(&m_tempFrame);
        m_tempFrame = nullptr;
    }
}

bool Decoder::initHWDecoder(const AVCodec *codec)
{   
    (void)codec;
    
    int ret = 0;
    ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);
    if (ret < 0) {
        qCritical("Failed to create VAAPI device context. Error: %d", ret);
        ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
        return false;
    }
    
    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    m_codecCtx->get_format = get_hw_format;
    
    qInfo("VAAPI initialized successfully.");
    return true;
}

bool Decoder::open()
{
    QThread::currentThread()->setPriority(QThread::TimeCriticalPriority);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qCritical("H.264 decoder not found");
        return false;
    }

    // Alokasi context
    m_codecCtx = avcodec_alloc_context3(codec);

    if (!m_codecCtx) {
        qCritical("Could not allocate decoder context");
        return false;
    }

    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->skip_loop_filter = AVDISCARD_NONREF;
    m_codecCtx->extra_hw_frames = 1;
    m_codecCtx->thread_type = FF_THREAD_SLICE;
    m_codecCtx->thread_count = 1;
    m_codecCtx->delay = 0;

    if (!initHWDecoder(codec)) {
        qWarning("VAAPI init failed, falling back to software decoding.");
    }

    if (avcodec_open2(m_codecCtx, codec, NULL) < 0) {
        qCritical("Could not open H.264 codec");
        return false;
    }
    m_isCodecCtxOpen = true;
    return true;
}

void Decoder::close()
{
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = Q_NULLPTR;
    }
    // Release HW Context
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = Q_NULLPTR;
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
        if (decodingFrame->format == AV_PIX_FMT_VAAPI) {

            // 1. Reset
            av_frame_unref(m_tempFrame);

            // 2. Set format
            m_tempFrame->format = AV_PIX_FMT_DRM_PRIME;

            // 3. Mapping (Zero Copy Magic)
            int mapRet = av_hwframe_map(m_tempFrame, decodingFrame, AV_HWFRAME_MAP_READ);
            if (mapRet < 0) {
                mapRet = av_hwframe_map(m_tempFrame, decodingFrame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_WRITE);
            }

            if (mapRet == 0) {
                m_tempFrame->pts = decodingFrame->pts;
                m_tempFrame->pkt_dts = decodingFrame->pkt_dts;
                m_tempFrame->width = decodingFrame->width;
                m_tempFrame->height = decodingFrame->height;

                // 4. SWAP FRAME
                av_frame_unref(decodingFrame);
                av_frame_move_ref(decodingFrame, m_tempFrame);

            } else {
                qWarning("Failed to map VAAPI frame: %d", mapRet);
            }
        }
        pushFrame();
    } else if (ret != AVERROR(EAGAIN)) {
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

    int width = 0, height = 0, format = -1;
    m_vb->peekFrameInfo(width, height, format);

    if (format == AV_PIX_FMT_DRM_PRIME) {
        if (m_onFrame) {
            m_onFrame(width, height, nullptr, nullptr, nullptr, 0, 0, 0);
        }
    } 
    else {
        m_vb->lock();
        const AVFrame *frame = m_vb->consumeRenderedFrame();
        
        if (m_onFrame && frame) {
             m_onFrame(frame->width, frame->height,
                       frame->data[0], frame->data[1], frame->data[2],
                       frame->linesize[0], frame->linesize[1], frame->linesize[2]);
        }
        m_vb->unLock();
    }
}