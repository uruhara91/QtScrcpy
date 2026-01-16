#include "videobuffer.h"
#include "avframeconvert.h"
#include <QDebug>
#include <thread>

extern "C"
{
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
}

#if defined(__GNUC__) || defined(__clang__)
    #include <immintrin.h>
    #define CPU_RELAX() _mm_pause()
#else
    #define CPU_RELAX() std::this_thread::yield()
#endif

VideoBuffer::VideoBuffer(QObject *parent) : QObject(parent) {
    connect(&m_fpsCounter, &FpsCounter::updateFPS, this, &VideoBuffer::updateFPS);
}

VideoBuffer::~VideoBuffer() {
    deInit();
}

bool VideoBuffer::init()
{
    m_decodingFrame = av_frame_alloc();
    if (!m_decodingFrame) return false;

    m_renderingframe = av_frame_alloc();
    if (!m_renderingframe) {
        av_frame_free(&m_decodingFrame);
        return false;
    }

    m_renderingFrameConsumed = true;
    m_fpsCounter.start();
    return true;
}

void VideoBuffer::deInit()
{
    if (m_decodingFrame) {
        av_frame_free(&m_decodingFrame);
        m_decodingFrame = nullptr;
    }
    if (m_renderingframe) {
        av_frame_free(&m_renderingframe);
        m_renderingframe = nullptr;
    }
    m_fpsCounter.stop();
}

// SPINLOCK
void VideoBuffer::lock()
{
    while (m_spinLock.test_and_set(std::memory_order_acquire)) {
        CPU_RELAX();
    }
}

void VideoBuffer::unLock()
{
    m_spinLock.clear(std::memory_order_release);
}

void VideoBuffer::setRenderExpiredFrames(bool renderExpiredFrames)
{
    m_renderExpiredFrames = renderExpiredFrames;
}

AVFrame *VideoBuffer::decodingFrame()
{
    return m_decodingFrame;
}

void VideoBuffer::offerDecodedFrame(bool &previousFrameSkipped)
{
    lock();
    
    if (!m_renderingFrameConsumed) {
        previousFrameSkipped = true;
        if (m_fpsCounter.isStarted()) {
            m_fpsCounter.addSkippedFrame();
        }
    } else {
        previousFrameSkipped = false;
    }

    swap();

    m_frameGen++; 
    
    m_renderingFrameConsumed = false;
    
    unLock();
}

const AVFrame *VideoBuffer::consumeRenderedFrame()
{    
    m_renderingFrameConsumed = true;
    
    if (m_fpsCounter.isStarted()) {
        m_fpsCounter.addRenderedFrame();
    }
    
    return m_renderingframe;
}

void VideoBuffer::peekFrameInfo(int &width, int &height, int &format)
{
    lock();
    if (m_renderingframe) {
        width = m_renderingframe->width;
        height = m_renderingframe->height;
        format = m_renderingframe->format;
    } else {
        width = 0; height = 0; format = -1;
    }
    unLock();
}

void VideoBuffer::peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame)
{
    if (!onFrame) {
        return;
    }

    lock();
    
    AVFrame* frame = m_renderingframe;

    if (!frame || frame->width <= 0 || frame->height <= 0) {
        unLock();
        return; 
    }

    int width = frame->width;
    int height = frame->height;
    int format = frame->format;

    bool cacheValid = (m_frameGen == m_cacheGen) &&
                      (m_cachedFrame != nullptr) &&
                      (width == m_cachedWidth) &&
                      (height == m_cachedHeight) &&
                      (format == m_cachedFormat);

    std::shared_ptr<std::vector<uint8_t>> targetBuffer;

    if (!cacheValid) {
        
        if (m_cachedFrame && m_cachedFrame.use_count() == 1) {
            targetBuffer = m_cachedFrame;
        } else {
            // Buffer
            targetBuffer = std::make_shared<std::vector<uint8_t>>();
        }

        int size = av_image_get_buffer_size(AV_PIX_FMT_RGB32, width, height, 4);
        if (targetBuffer->size() != (size_t)size) {
            targetBuffer->resize(size);
        }

        // Conversion
        AVFrame *rgbFrame = av_frame_alloc();
        if (rgbFrame) {
            av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, targetBuffer->data(), AV_PIX_FMT_RGB32, width, height, 4);

            AVFrameConvert convert;
            convert.setSrcFrameInfo(width, height, (AVPixelFormat)format);
            convert.setDstFrameInfo(width, height, AV_PIX_FMT_RGB32);
            
            if (convert.init()) {
                convert.convert(frame, rgbFrame);
                
                // Update State Cache
                m_cachedFrame = targetBuffer;
                m_cacheGen = m_frameGen;
                m_cachedWidth = width;
                m_cachedHeight = height;
                m_cachedFormat = format;
            } else {
                 qWarning() << "VideoBuffer::peekRenderedFrame convert init failed";
                 // Reset target buffer on failure to prevent using garbage data
                 targetBuffer.reset();
            }
            convert.deInit();
            av_frame_free(&rgbFrame);
        } else {
            targetBuffer.reset();
        }
    } else {
        targetBuffer = m_cachedFrame;
    }

    unLock();

    if (targetBuffer && !targetBuffer->empty()) {
        onFrame(m_cachedWidth, m_cachedHeight, targetBuffer->data());
    }
}

void VideoBuffer::interrupt()
{
    m_interrupted = true;
}

void VideoBuffer::swap()
{
    AVFrame *tmp = m_decodingFrame;
    m_decodingFrame = m_renderingframe;
    m_renderingframe = tmp;
}