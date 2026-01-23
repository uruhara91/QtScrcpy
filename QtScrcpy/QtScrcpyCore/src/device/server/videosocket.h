#ifndef VIDEOSOCKET_H
#define VIDEOSOCKET_H

#include <QTcpSocket>
#include <atomic>

class VideoSocket : public QTcpSocket
{
    Q_OBJECT
public:
    explicit VideoSocket(QObject *parent = nullptr);
    virtual ~VideoSocket();

    qint32 subThreadRecvData(quint8 *buf, qint32 bufSize);
    
    void quitNotify() { m_quit = true; } 

private:
    std::atomic<bool> m_quit{false};
};

#endif // VIDEOSOCKET_H
