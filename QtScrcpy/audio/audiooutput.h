#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QThread>
#include <QProcess>
#include <QPointer>
#include <QVector>
#include <QTcpServer>
#include <QTcpSocket>

class QAudioSink;
class QAudioOutput;
class QIODevice;

class AudioOutput : public QObject
{
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput();

    bool start(const QString& serial, int port);
    void stop();
    void installonly(const QString& serial, int port);

private:
    bool runSndcpyProcess(const QString& serial, int port, bool wait = true);
    void startAudioOutput();
    void stopAudioOutput();
    // Berubah fungsi dari startRecvData menjadi startServer
    void startServer(int port);
    void stopServer();

signals:
    // Signal internal untuk memulai server di thread worker
    void startServerSignal(int port);

private:
    QPointer<QIODevice> m_outputDevice;
    QThread m_workerThread;
    QProcess m_sndcpy;
    // Buffer raw untuk menampung data sementara jika audio device belum siap
    QVector<char> m_buffer;
    bool m_running = false;
    
    // Server components
    QTcpServer* m_server = nullptr;
    QTcpSocket* m_clientSocket = nullptr;

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioOutput* m_audioOutput = nullptr;
#else
    QAudioSink *m_audioSink = nullptr;
#endif
};

#endif // AUDIOOUTPUT_H