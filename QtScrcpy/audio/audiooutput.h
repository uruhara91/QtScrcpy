#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QObject>
#include <QThread>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>
#include <QAudioFormat>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
class QAudioSink;
class QMediaDevices;
#else
class QAudioOutput;
#endif
class QIODevice;

// --- Worker Class (Berjalan di Thread Terpisah) ---
class AudioServerWorker : public QObject {
    Q_OBJECT
public:
    explicit AudioServerWorker(int port, QObject *parent = nullptr);
    ~AudioServerWorker();

public slots:
    void startServer();
    void stopServer();

signals:
    // Menggunakan const reference untuk menghindari copy berlebih
    void dataReceived(const QByteArray &data);
    void serverReady(bool success);
    void clientConnected(const QString &addr);
    void clientDisconnected();

private:
    int m_port;
    // QPointer mencegah Dangling Pointer jika objek dihapus di tempat lain
    QPointer<QTcpServer> m_server;
    QPointer<QTcpSocket> m_client;
};

// --- Main Controller (Berjalan di Main/UI Thread) ---
class AudioOutput : public QObject
{
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput();

    bool start(const QString& serial, int port);
    bool install(const QString& serial);
    void stop();

signals:
    void stopRequested();

private:
    bool runAdbCommand(const QString& serial, const QStringList& args);
    bool runAppProcess(const QString& serial, int port);
    void setupAudioDevice();
    void cleanupAudioDevice();

private slots:
    void onDataReceived(const QByteArray &data);

private:
    // Thread Worker tetap hidup selama aplikasi berjalan (Persistent Thread)
    QThread m_workerThread;
    QPointer<AudioServerWorker> m_serverWorker;
    QProcess m_appProcess;
    
    // Audio Components
    QPointer<QIODevice> m_audioIO;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioOutput* m_audioOutput = nullptr;
#else
    QAudioSink *m_audioSink = nullptr;
#endif
    
    QAudioFormat m_format;
};

#endif // AUDIOOUTPUT_H