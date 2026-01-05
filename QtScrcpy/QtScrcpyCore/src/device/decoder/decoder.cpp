#include <QDebug>
#include <QThread>
#include "compat.h"
#include "decoder.h"
#include "videobuffer.h"

// Callback static
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;
    enum AVPixelFormat target = *static_cast<enum AVPixelFormat*>(ctx->opaque);

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == target) {
            return *p;
        }
    }
    
    qWarning("Failed to get HW surface format. Fallback to SW.");
    return AV_PIX_FMT_NONE;
}

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
    m_swsCtx = NULL;

    m_cacheSwFrame = av_frame_alloc();
    m_cacheConvFrame = av_frame_alloc();
    if (!m_cacheSwFrame || !m_cacheConvFrame) {
        qCritical("Could not allocate cache frames");
        return false;
    }

    QThread::currentThread()->setPriority(QThread::TimeCriticalPriority);

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qCritical("H.264 decoder not found");
        return false;
    }

    // 1. Hardware Init
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
#if defined(Q_OS_WIN)
    type = AV_HWDEVICE_TYPE_D3D11VA;
    qInfo("Selecting D3D11VA for Windows");
#elif defined(Q_OS_LINUX)
    type = AV_HWDEVICE_TYPE_VAAPI;
    qInfo("Selecting VAAPI for Linux");
#endif

    // 2. Inisialisasi Hardware
    int err = 0;
    if (type != AV_HWDEVICE_TYPE_NONE) {
        err = av_hwdevice_ctx_create(&m_hwDeviceCtx, type, NULL, NULL, 0);
        if (err < 0) {
            qWarning("Failed to create HW device. Error code: %d. Fallback to SW Decode.", err);
        } else {
            if (type == AV_HWDEVICE_TYPE_VAAPI) m_hwFormat = AV_PIX_FMT_VAAPI;
            else if (type == AV_HWDEVICE_TYPE_D3D11VA) m_hwFormat = AV_PIX_FMT_D3D11;
            else if (type == AV_HWDEVICE_TYPE_DXVA2) m_hwFormat = AV_PIX_FMT_DXVA2_VLD;
            
            qInfo("HW Device created successfully.");
        }
    }

    // 3. Alokasi Context
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        qCritical("Could not allocate decoder context");
        return false;
    }

    // 4. Attach HW Context
    if (m_hwDeviceCtx) {
        m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_codecCtx->opaque = &m_hwFormat; 
        m_codecCtx->get_format = get_hw_format;
    }

    // 5. Optimasi
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->skip_loop_filter = AVDISCARD_NONREF;

    // Threading
    if (m_hwDeviceCtx) {
        m_codecCtx->thread_count = 1;
    } else {
        m_codecCtx->thread_type = FF_THREAD_SLICE;
        m_codecCtx->thread_count = 0;
    }

    // Open Codec
    if (avcodec_open2(m_codecCtx, codec, NULL) < 0) {
        qCritical("Could not open H.264 codec");
        return false;
    }
    
    m_isCodecCtxOpen = true;
    qInfo("Decoder initialized. HW Accel: %s", m_hwDeviceCtx ? "ENABLED (Copy Mode)" : "DISABLED (SW Mode)");
    return true;
}

void Decoder::close()
{
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = Q_NULLPTR;
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = Q_NULLPTR;
    }
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = Q_NULLPTR;
    }

    if (m_cacheSwFrame) {
        av_frame_free(&m_cacheSwFrame);
        m_cacheSwFrame = Q_NULLPTR;
    }
    if (m_cacheConvFrame) {
        av_frame_free(&m_cacheConvFrame);
        m_cacheConvFrame = Q_NULLPTR;
    }

    m_isCodecCtxOpen = false;
}

bool Decoder::push(const AVPacket *packet)
{
    if (!m_codecCtx || !m_isCodecCtxOpen) return false;
    
    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0) {
        qCritical("Send packet error: %d", ret);
        return false;
    }

    AVFrame *decodingFrame = m_vb->decodingFrame();
    
    ret = avcodec_receive_frame(m_codecCtx, decodingFrame);
    if (ret == 0) {
        pushFrame();
    } else if (ret != AVERROR(EAGAIN)) {
        qWarning("Receive frame error: %d", ret);
        return false;
    }
    
    return true;
}

void Decoder::peekFrame(std::function<void (int, int, uint8_t *)> onFrame)
{
    if (m_vb) m_vb->peekRenderedFrame(onFrame);
}

void Decoder::pushFrame()
{
    if (!m_vb) return;
    bool previousFrameSkipped = true;
    m_vb->offerDecodedFrame(previousFrameSkipped);
    if (previousFrameSkipped) return;
    emit newFrame();
}

void Decoder::onNewFrame()
{
    if (!m_vb) return;

    m_vb->lock();
    AVFrame *frame = m_vb->consumeRenderedFrame();
    
    if (m_onFrame && frame) {
        AVFrame *final_frame = frame; 

        // --- 1. PHASE COPY ---
        if (frame->format == m_hwFormat && m_hwFormat != AV_PIX_FMT_NONE) {
            av_frame_unref(m_cacheSwFrame); 
            if (av_hwframe_transfer_data(m_cacheSwFrame, frame, 0) == 0) {
                m_cacheSwFrame->width = frame->width;
                m_cacheSwFrame->height = frame->height;
                final_frame = m_cacheSwFrame; 
            }
        }

        // --- 2. PHASE CONVERT & ROTATION ---
        if (final_frame && final_frame->format != AV_PIX_FMT_YUV420P) {
            m_swsCtx = sws_getCachedContext(
                m_swsCtx,
                final_frame->width, final_frame->height, (AVPixelFormat)final_frame->format,
                final_frame->width, final_frame->height, AV_PIX_FMT_YUV420P,
                SWS_FAST_BILINEAR, NULL, NULL, NULL
            );
            
            if (m_swsCtx) {
                if (m_cacheConvFrame->width != final_frame->width ||
                    m_cacheConvFrame->height != final_frame->height ||
                    m_cacheConvFrame->format != AV_PIX_FMT_YUV420P ||
                    !m_cacheConvFrame->data[0]) {
                    
                    av_frame_unref(m_cacheConvFrame);
                    
                    m_cacheConvFrame->width = final_frame->width;
                    m_cacheConvFrame->height = final_frame->height;
                    m_cacheConvFrame->format = AV_PIX_FMT_YUV420P;
                    av_frame_get_buffer(m_cacheConvFrame, 32);
                }

                if (m_cacheConvFrame->data[0]) {
                     sws_scale(m_swsCtx, 
                               final_frame->data, final_frame->linesize, 0, final_frame->height,
                               m_cacheConvFrame->data, m_cacheConvFrame->linesize);
                     
                     final_frame = m_cacheConvFrame;
                }
            }
        }

        // --- 3. PHASE RENDER ---
        if (final_frame && final_frame->data[0]) {
             m_onFrame(final_frame->width, final_frame->height,
                       final_frame->data[0], final_frame->data[1], final_frame->data[2],
                       final_frame->linesize[0], final_frame->linesize[1], final_frame->linesize[2]);
        }
    }

    if (frame) {
        av_frame_free(&frame);
    }

    m_vb->unLock();
}