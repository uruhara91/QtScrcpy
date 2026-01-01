#include <QDebug>
#include "avframeconvert.h"

AVFrameConvert::AVFrameConvert() {}

AVFrameConvert::~AVFrameConvert() {
    deInit();
}

void AVFrameConvert::setSrcFrameInfo(int srcWidth, int srcHeight, AVPixelFormat srcFormat)
{
    m_srcWidth = srcWidth;
    m_srcHeight = srcHeight;
    m_srcFormat = srcFormat;
}

void AVFrameConvert::getSrcFrameInfo(int &srcWidth, int &srcHeight, AVPixelFormat &srcFormat)
{
    srcWidth = m_srcWidth;
    srcHeight = m_srcHeight;
    srcFormat = m_srcFormat;
}

void AVFrameConvert::setDstFrameInfo(int dstWidth, int dstHeight, AVPixelFormat dstFormat)
{
    m_dstWidth = dstWidth;
    m_dstHeight = dstHeight;
    m_dstFormat = dstFormat;
}

void AVFrameConvert::getDstFrameInfo(int &dstWidth, int &dstHeight, AVPixelFormat &dstFormat)
{
    dstWidth = m_dstWidth;
    dstHeight = m_dstHeight;
    dstFormat = m_dstFormat;
}

bool AVFrameConvert::init()
{
    if (m_convertCtx) {
        return true;
    }

    AVPixelFormat realSrcFormat = m_srcFormat;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(m_srcFormat);
    if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        realSrcFormat = AV_PIX_FMT_NV12;
    }

    m_convertCtx = sws_getContext(m_srcWidth, m_srcHeight, realSrcFormat,
                                  m_dstWidth, m_dstHeight, m_dstFormat,
                                  SWS_BICUBIC, Q_NULLPTR, Q_NULLPTR, Q_NULLPTR);
    if (!m_convertCtx) {
        qCritical("AVFrameConvert: Failed to initialize sws_context");
        return false;
    }
    return true;
}

bool AVFrameConvert::isInit()
{
    return m_convertCtx ? true : false;
}

void AVFrameConvert::deInit()
{
    if (m_convertCtx) {
        sws_freeContext(m_convertCtx);
        m_convertCtx = Q_NULLPTR;
    }
}

bool AVFrameConvert::convert(const AVFrame *srcFrame, AVFrame *dstFrame)
{
    if (!m_convertCtx || !srcFrame || !dstFrame) {
        return false;
    }

    const uint8_t *const *srcData = srcFrame->data;
    const int *srcLinesize = srcFrame->linesize;

    AVFrame *swFrame = Q_NULLPTR;
    bool isHwFrame = (srcFrame->format == AV_PIX_FMT_VAAPI || 
                      srcFrame->format == AV_PIX_FMT_DRM_PRIME);

    if (isHwFrame) {
        swFrame = av_frame_alloc();
        if (!swFrame) return false;

        int ret = av_hwframe_transfer_data(swFrame, srcFrame, 0);
        if (ret < 0) {
            qCritical("AVFrameConvert: Failed to transfer data from GPU to CPU: %d", ret);
            av_frame_free(&swFrame);
            return false;
        }

        srcData = swFrame->data;
        srcLinesize = swFrame->linesize;
    }

    int ret = sws_scale(m_convertCtx,
                        srcData, srcLinesize,
                        0, m_srcHeight,
                        dstFrame->data, dstFrame->linesize);

    if (swFrame) {
        av_frame_free(&swFrame);
    }

    return (ret > 0);
}