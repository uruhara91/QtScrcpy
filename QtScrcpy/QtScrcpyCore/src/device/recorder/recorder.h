#ifndef RECORDER_H
#define RECORDER_H

#include <QMutex>
#include <QQueue>
#include <QSize>
#include <QString>
#include <QThread>
#include <QWaitCondition>

extern "C"
{
#include "libavformat/avformat.h"
}

class Recorder : public QThread
{
    Q_OBJECT
public:
    enum RecorderFormat
    {
        RECORDER_FORMAT_NULL = 0,
        RECORDER_FORMAT_MP4,
        RECORDER_FORMAT_MKV,
    };

    explicit Recorder(const QString &fileName, QObject *parent = nullptr);
    virtual ~Recorder() override;

    void setFrameSize(const QSize &declaredFrameSize);
    void setFormat(Recorder::RecorderFormat format);
    
    bool open();
    void close();
    bool startRecorder();
    void stopRecorder();
    
    bool push(AVPacket *packet);

private:
    const AVOutputFormat *findMuxer(const char *name);
    bool recorderWriteHeader(const AVPacket *packet);
    void recorderRescalePacket(AVPacket *packet);
    QString recorderGetFormatName(Recorder::RecorderFormat format);
    RecorderFormat guessRecordFormat(const QString &fileName);
    bool write(AVPacket *packet);

private:
    void packetDelete(AVPacket *packet);
    void queueClear();

protected:
    void run() override;

private:
    QString m_fileName = "";
    AVFormatContext *m_formatCtx = nullptr;
    QSize m_declaredFrameSize;
    
    bool m_headerWritten = false;
    RecorderFormat m_format = RECORDER_FORMAT_NULL;
    
    QMutex m_mutex;
    QWaitCondition m_recvDataCond;
    
    bool m_stopped = false; 
    bool m_failed = false;
    
    QQueue<AVPacket *> m_queue;
};

#endif // RECORDER_H