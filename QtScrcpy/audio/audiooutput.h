#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QObject>
#include <QThread>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
class QAudioSink;
#else
class QAudioOutput;
#endif
class QIODevice;

// Worker class untuk menangani Server di thread terpisah
class AudioServerWorker : public QObject {
    Q_OBJECT
public:
    explicit AudioServerWorker(int port, QObject *parent = nullptr);
    ~AudioServerWorker();

public slots:
    void startServer();
    void stopServer();

signals:
    void dataReceived(const QByteArray &data);
    void serverReady(bool success);
    void clientConnected(const QString &addr);
    void clientDisconnected();

private:
    int m_port;
    QTcpServer *m_server = nullptr;
    QTcpSocket *m_client = nullptr;
};

class AudioOutput : public QObject
{
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput();

    // START: Hanya menjalankan forwarding (Instant)
    bool start(const QString& serial, int port);
    
    // INSTALL: Install APK + Inject Permission (One time setup)
    bool install(const QString& serial, int port);
    
    void stop();

private:
    // Helper untuk menjalankan raw adb command
    bool runAdbCommand(const QString& serial, const QStringList& args);
    
    // Internal process logic
    bool runAppProcess(const QString& serial, int port);
    void setupAudioDevice();
    void cleanupAudioDevice();

private slots:
    void onDataReceived(const QByteArray &data);

private:
    QThread m_workerThread;
    AudioServerWorker *m_serverWorker = nullptr;
    QProcess m_appProcess; 
    
    // Audio Components
    QPointer<QIODevice> m_audioIO;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioOutput* m_audioOutput = nullptr;
#else
    QAudioSink *m_audioSink = nullptr;
#endif
    
    bool m_running = false;
    QString m_currentSerial;
};

#endif // AUDIOOUTPUT_H