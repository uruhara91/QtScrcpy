#include "videobuffer.h"
#include "avframeconvert.h"
#include <QDebug>

extern "C"
{
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
}

VideoBuffer::VideoBuffer(QObject *parent) : QObject(parent) {
    connect(&m_fpsCounter, &FpsCounter::updateFPS, this, &VideoBuffer::updateFPS);
}

VideoBuffer::~VideoBuffer() {
    deInit();
}

bool VideoBuffer::init()
{
    // Alokasi Frame
    m_decodingFrame = av_frame_alloc();
    if (!m_decodingFrame) {
        goto error;
    }

    m_renderingframe = av_frame_alloc();
    if (!m_renderingframe) {
        goto error;
    }

    m_renderingFrameConsumed = true;
    m_fpsCounter.start();
    return true;

error:
    deInit();
    return false;
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

void VideoBuffer::lock()
{
    m_mutex.lock();
}

void VideoBuffer::unLock()
{
    m_mutex.unlock();
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
    QMutexLocker lock(&m_mutex);
    
    if (!m_renderingFrameConsumed) {
        previousFrameSkipped = true;
        if (m_fpsCounter.isStarted()) {
            m_fpsCounter.addSkippedFrame();
        }
        av_frame_unref(m_renderingframe);
    } else {
        previousFrameSkipped = false;
    }

    swap();
    m_renderingFrameConsumed = false;
}

const AVFrame *VideoBuffer::consumeRenderedFrame()
{
    m_renderingFrameConsumed = true;
    
    if (m_fpsCounter.isStarted()) {
        m_fpsCounter.addRenderedFrame();
    }
    
    if (m_renderExpiredFrames) {
        m_renderingFrameConsumedCond.wakeOne();
    }
    return m_renderingframe;
}

void VideoBuffer::peekFrameInfo(int &width, int &height, int &format)
{
    QMutexLocker lock(&m_mutex);
    if (m_renderingframe) {
        width = m_renderingframe->width;
        height = m_renderingframe->height;
        format = m_renderingframe->format;
    } else {
        width = 0;
        height = 0;
        format = -1;
    }
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

    AVFrame *rgbFrame = av_frame_alloc();
    if (!rgbFrame) {
        unLock();
        return;
    }

    int size = av_image_get_buffer_size(AV_PIX_FMT_RGB32, width, height, 4);
    uint8_t* rgbBuffer = new uint8_t[size];

    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer, AV_PIX_FMT_RGB32, width, height, 4);

    AVFrameConvert convert;
    convert.setSrcFrameInfo(width, height, (AVPixelFormat)frame->format);
    convert.setDstFrameInfo(width, height, AV_PIX_FMT_RGB32);
    
    if (convert.init() && convert.convert(frame, rgbFrame)) {
        convert.deInit();
        av_free(rgbFrame);
        unLock();
        
        onFrame(width, height, rgbBuffer);
    } else {
        // Gagal
        convert.deInit();
        av_free(rgbFrame);
        unLock();
    }

    delete [] rgbBuffer;
}

void VideoBuffer::interrupt()
{
    if (m_renderExpiredFrames) {
        QMutexLocker lock(&m_mutex);
        m_interrupted = true;
        m_renderingFrameConsumedCond.wakeOne();
    }
}

void VideoBuffer::swap()
{
    AVFrame *tmp = m_decodingFrame;
    m_decodingFrame = m_renderingframe;
    m_renderingframe = tmp;
}