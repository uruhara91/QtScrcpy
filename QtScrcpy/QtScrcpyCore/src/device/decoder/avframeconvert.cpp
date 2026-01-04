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
    // Validasi Parameter Dasar
    if (m_srcWidth <= 0 || m_srcHeight <= 0 || m_srcFormat == AV_PIX_FMT_NONE ||
        m_dstWidth <= 0 || m_dstHeight <= 0 || m_dstFormat == AV_PIX_FMT_NONE) {
        return false;
    }

    // Inisialisasi Konversi
    m_convertCtx = sws_getCachedContext(m_convertCtx,
                                        m_srcWidth, m_srcHeight, m_srcFormat,
                                        m_dstWidth, m_dstHeight, m_dstFormat,
                                        SWS_FAST_BILINEAR, NULL, NULL, NULL);

    if (!m_convertCtx) {
        qCritical("AVFrameConvert: Failed to initialize/cache sws_context");
        return false;
    }
    return true;
}

bool AVFrameConvert::isInit()
{
    return m_convertCtx != Q_NULLPTR;
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
    
    int ret = sws_scale(m_convertCtx,
                        srcFrame->data, srcFrame->linesize, 0, m_srcHeight,
                        dstFrame->data, dstFrame->linesize);

    if (ret != m_dstHeight) {
        qWarning("AVFrameConvert: sws_scale failed or incomplete. Ret: %d, Expected: %d", ret, m_dstHeight);
        return false;
    }

    return true;
}