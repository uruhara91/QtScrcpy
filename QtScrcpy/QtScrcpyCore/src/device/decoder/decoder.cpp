#include <QDebug>
#include "compat.h"
#include "decoder.h"
#include "videobuffer.h"

// --- Callback Static untuk FFmpeg ---
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    (void)ctx;
    
    const enum AVPixelFormat *p;
    // Cari format VAAPI dalam daftar yang didukung codec
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_VAAPI) {
            return *p;
        }
    }
    
    // Fallback: Jika VAAPI tidak ada, gunakan format software pertama yg tersedia (biasanya YUV420P)
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
}

Decoder::~Decoder() {
    close(); // Pastikan close dipanggil
    if (m_vb) {
        m_vb->deInit();
        delete m_vb;
        m_vb = Q_NULLPTR;
    }
}

bool Decoder::initHWDecoder(const AVCodec *codec)
{   
    (void)codec;
    
    int ret = 0;
    // Mencoba membuat context HW Device untuk VAAPI
    // Untuk Intel Gen 11 di Linux, VAAPI adalah jalur terbaik.
    ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
    if (ret < 0) {
        qCritical("Failed to create VAAPI device context. Error: %d", ret);
        return false;
    }
    
    // Attach context ke codec context
    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    
    // Set callback get_format agar FFmpeg tahu kita mau pakai HW
    m_codecCtx->get_format = get_hw_format;
    
    qInfo("Hardware decoding (VAAPI) initialized successfully.");
    return true;
}

bool Decoder::open()
{
    // Cari decoder H.264
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

    // --- COBA INIT HARDWARE ---
    if (!initHWDecoder(codec)) {
        qWarning("Hardware decoder init failed, falling back to software decoding.");
        // Jangan return false, biarkan jalan software decode biasa
    }
    // --------------------------

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

    // Ambil frame dari decoder (bisa berkali-kali loop)
    AVFrame *decodingFrame = m_vb->decodingFrame();
    
    ret = avcodec_receive_frame(m_codecCtx, decodingFrame);
    if (ret == 0) {
        // --- LOGIKA MAPPING HW -> DRM PRIME ---
        if (decodingFrame->format == AV_PIX_FMT_VAAPI) {
            // Frame ada di GPU (VAAPI Surface).
            // Renderer kita butuh File Descriptor (DRM PRIME).
            // Kita lakukan mapping: VAAPI -> DRM PRIME.
            
            AVFrame *mappedFrame = av_frame_alloc();
            if (!mappedFrame) {
                 qCritical("Failed to allocate mapped frame");
                 return false;
            }

            // Flag AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_WRITE kadang diperlukan, atau 0.
            // Map ke DRM_PRIME
            mappedFrame->format = AV_PIX_FMT_DRM_PRIME;
            int mapRet = av_hwframe_map(mappedFrame, decodingFrame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_WRITE);
            
            if (mapRet < 0) {
                // Jika gagal map read/write, coba read only
                mapRet = av_hwframe_map(mappedFrame, decodingFrame, AV_HWFRAME_MAP_READ);
            }

            if (mapRet == 0) {
                // Sukses map!
                // Copy metadata waktu (pts) agar sinkronisasi AV tetap jalan
                mappedFrame->pts = decodingFrame->pts;
                mappedFrame->pkt_dts = decodingFrame->pkt_dts;
                mappedFrame->width = decodingFrame->width;
                mappedFrame->height = decodingFrame->height;

                // Sekarang mappedFrame berisi descriptor DRM (fd, stride, offset).
                // decodingFrame (VAAPI) bisa kita unref karena mappedFrame memegang referensi ke data aslinya.
                av_frame_unref(decodingFrame);
                
                // Pindahkan isi mappedFrame kembali ke decodingFrame (yang dipakai VideoBuffer)
                av_frame_move_ref(decodingFrame, mappedFrame);
                av_frame_free(&mappedFrame); // Hapus shell mappedFrame
            } else {
                qWarning("Failed to map VAAPI frame to DRM_PRIME: %d. Rendering might fail.", mapRet);
                av_frame_free(&mappedFrame);
                // Jika gagal map, biarkan frame apa adanya (VAAPI), 
                // tapi kemungkinan renderer akan error atau hitam.
            }
        }
        // --------------------------------------

        pushFrame();
    } else if (ret != AVERROR(EAGAIN)) {
        // Error decoding
        // qCritical("Could not receive video frame: %d", ret);
        // Terkadang error kecil wajar saat stream baru mulai
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
        // Frame sebelumnya dibuang (skip) karena decoding terlalu cepat dibanding render
        return;
    }
    emit newFrame();
}

void Decoder::onNewFrame()
{
    if (!m_vb) return;

    // 1. Intip dulu format dan ukurannya
    int width = 0, height = 0, format = -1;
    m_vb->peekFrameInfo(width, height, format);

    if (format == AV_PIX_FMT_DRM_PRIME) {
        // --- JALUR ZERO COPY (HARDWARE) ---
        // JANGAN panggil consumeRenderedFrame()! Biarkan QYuvOpenGLWidget yang mengambilnya.
        // Kita hanya perlu memberitahu UI bahwa ada frame baru dengan ukuran sekian.
        
        if (m_onFrame) {
            // Kirim nullptr sebagai data agar VideoForm tahu ini mode HW
            m_onFrame(width, height, nullptr, nullptr, nullptr, 0, 0, 0);
        }
    } 
    else {
        // --- JALUR LEGACY (SOFTWARE) ---
        // Lakukan seperti biasa
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