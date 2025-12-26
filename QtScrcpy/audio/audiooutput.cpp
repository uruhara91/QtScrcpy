#include "audiooutput.h"
#include <QCoreApplication>
#include <QDebug>
#include <QAudioFormat>
#include <QThread>
#include <QFileInfo>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QMediaDevices>
#include <QAudioDevice>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif

// Konfigurasi Package (Sesuaikan dengan AndroidManifest.xml)
static const QString APP_PACKAGE = "com.android.sound.helper";
static const QString APP_ACTIVITY = ".MainActivity";
static const QString APK_NAME = "soundhelper.apk"; 

// ================= WORKER IMPLEMENTATION =================

AudioServerWorker::AudioServerWorker(int port, QObject *parent)
    : QObject(parent), m_port(port) {}

AudioServerWorker::~AudioServerWorker() {
    stopServer();
}

void AudioServerWorker::startServer() {
    if (m_server && m_server->isListening()) return;

    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::AnyIPv4, m_port)) {
        qCritical() << "AudioServerWorker::Failed to listen on port" << m_port;
        emit serverReady(false);
        return;
    }
    
    connect(m_server, &QTcpServer::newConnection, this, [this]() {
        QTcpSocket *next = m_server->nextPendingConnection();
        if (m_client) {
            m_client->close();
            m_client->deleteLater();
        }
        m_client = next;
        
        // OPTIMASI TCP: LOW LATENCY UNTUK FPS GAMING
        m_client->setSocketOption(QAbstractSocket::LowDelayOption, 1); // No Nagle
        m_client->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        m_client->setReadBufferSize(16 * 1024); // Buffer secukupnya

        emit clientConnected(m_client->peerAddress().toString());

        connect(m_client, &QTcpSocket::readyRead, this, [this]() {
            if (m_client) {
                emit dataReceived(m_client->readAll());
            }
        });

        connect(m_client, &QTcpSocket::disconnected, this, &AudioServerWorker::clientDisconnected);
    });

    qInfo() << "AudioServerWorker::Listening on port" << m_port;
    emit serverReady(true);
}

void AudioServerWorker::stopServer() {
    if (m_client) {
        m_client->close();
        m_client->deleteLater();
        m_client = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

// ================= AUDIOOUTPUT IMPLEMENTATION =================

AudioOutput::AudioOutput(QObject *parent) : QObject(parent) {
    m_running = false;
}

AudioOutput::~AudioOutput() {
    stop();
}

// Helper: Menjalankan ADB command synchronous
bool AudioOutput::runAdbCommand(const QString& serial, const QStringList& args) {
    QProcess adb;
    QStringList finalArgs;
    if (!serial.isEmpty()) {
        finalArgs << "-s" << serial;
    }
    finalArgs << args;

    adb.start("adb", finalArgs);
    if (!adb.waitForStarted(1000)) return false;
    
    // Tunggu sampai selesai (Timeout 10 detik untuk install, 2 detik untuk command biasa)
    // Install apk bisa lama, kita handle logic timeout di caller atau set cukup lama disini
    int timeout = args.contains("install") ? 30000 : 3000;
    if (!adb.waitForFinished(timeout)) {
        qWarning() << "ADB Command Timed out:" << args;
        return false;
    }
    
    return (adb.exitCode() == 0);
}

// FUNCTION 1: INSTALL (Update APK & Grant Permissions)
// Ini dipanggil saat tombol "Install Sndcpy" ditekan.
bool AudioOutput::install(const QString& serial, int port) {
    Q_UNUSED(port);
    qInfo() << "AudioOutput::Starting Installation Process...";

    QString apkPath = QCoreApplication::applicationDirPath() + "/" + APK_NAME;
    if (!QFileInfo::exists(apkPath)) {
        qCritical() << "AudioOutput::APK Not Found at:" << apkPath;
        qCritical() << "Please place" << APK_NAME << "in the application folder.";
        return false;
    }

    // 1. Install APK (-t: test-only, -r: replace, -g: grant runtime permissions)
    qInfo() << "AudioOutput::Installing APK... (This may take a few seconds)";
    if (!runAdbCommand(serial, QStringList() << "install" << "-t" << "-r" << "-g" << apkPath)) {
        qCritical() << "AudioOutput::Install Failed!";
        return false;
    }

    // 2. Inject Permissions (The "Silent" Magic)
    // Ini agar user tidak perlu klik "Allow" di HP berulang kali.
    qInfo() << "AudioOutput::Granting Special Permissions...";
    
    // Permission Record Audio
    runAdbCommand(serial, QStringList() << "shell" << "pm" << "grant" << APP_PACKAGE << "android.permission.RECORD_AUDIO");
    
    // Permission Notification (Android 13+)
    runAdbCommand(serial, QStringList() << "shell" << "pm" << "grant" << APP_PACKAGE << "android.permission.POST_NOTIFICATIONS");

    // Bypass "Start Casting" Popup (AppOps)
    // Command: cmd appops set <package> PROJECT_MEDIA allow
    runAdbCommand(serial, QStringList() << "shell" << "appops" << "set" << APP_PACKAGE << "PROJECT_MEDIA" << "allow");

    qInfo() << "AudioOutput::Installation & Setup Completed Successfully!";
    return true;
}

// FUNCTION 2: START (Zero Overhead, Instant)
// Ini dipanggil saat tombol "Start Audio" ditekan.
bool AudioOutput::start(const QString& serial, int port) {
    if (m_running) stop();
    m_currentSerial = serial;

    qInfo() << "AudioOutput::Starting Audio Forwarding...";

    // 1. ADB REVERSE (Wajib: Agar Android bisa hit localhost PC)
    if (!runAdbCommand(serial, QStringList() << "reverse" << QString("tcp:%1").arg(port) << QString("tcp:%1").arg(port))) {
        qWarning() << "AudioOutput::ADB Reverse failed! Audio might not connect.";
    }

    // 2. Setup PC Server (Worker Thread)
    m_serverWorker = new AudioServerWorker(port);
    m_serverWorker->moveToThread(&m_workerThread);

    connect(&m_workerThread, &QThread::finished, m_serverWorker, &QObject::deleteLater);
    connect(this, &AudioOutput::stop, m_serverWorker, &AudioServerWorker::stopServer, Qt::QueuedConnection);
    connect(m_serverWorker, &AudioServerWorker::dataReceived, this, &AudioOutput::onDataReceived);

    m_workerThread.start();
    QMetaObject::invokeMethod(m_serverWorker, "startServer", Qt::QueuedConnection);

    // 3. Setup Speaker PC
    setupAudioDevice();

    // 4. Launch Android App (Service Only)
    if (!runAppProcess(serial, port)) {
        stop();
        return false;
    }

    m_running = true;
    return true;
}

void AudioOutput::stop() {
    m_running = false;

    // Graceful Shutdown: Kirim Broadcast ke Android untuk stop service
    if (!m_currentSerial.isEmpty()) {
        runAdbCommand(m_currentSerial, QStringList() << "shell" << "am" << "broadcast" << "-a" << APP_PACKAGE + ".STOP");
        
        // Opsional: Remove reverse rule
        // runAdbCommand(m_currentSerial, QStringList() << "reverse" << "--remove" << QString("tcp:28200"));
    }

    if (m_workerThread.isRunning()) {
        m_workerThread.quit();
        m_workerThread.wait();
    }
    
    cleanupAudioDevice();
}

bool AudioOutput::runAppProcess(const QString& serial, int port) {
    // Command: am start -n com.pkg/.Activity --ei PORT 28200
    QString cmd = QString("am start -n %1/%2 --ei PORT %3")
                      .arg(APP_PACKAGE, APP_ACTIVITY)
                      .arg(port);
    
    QStringList params;
    params << "-s" << serial << "shell" << cmd;
    
    m_appProcess.start("adb", params);
    return m_appProcess.waitForStarted(1000);
}

void AudioOutput::setupAudioDevice() {
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());
    if (!info.isFormatSupported(format)) {
        format = info.nearestFormat(format);
    }
    m_audioOutput = new QAudioOutput(format, this);
    // EXTREME LOW LATENCY: 10ms Buffer (1920 bytes)
    m_audioOutput->setBufferSize(1920); 
    m_audioIO = m_audioOutput->start();
#else
    format.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    m_audioSink = new QAudioSink(device, format, this);
    m_audioSink->setBufferSize(1920); 
    m_audioIO = m_audioSink->start();
#endif
}

void AudioOutput::cleanupAudioDevice() {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    if (m_audioOutput) { m_audioOutput->stop(); delete m_audioOutput; m_audioOutput = nullptr; }
#else
    if (m_audioSink) { m_audioSink->stop(); delete m_audioSink; m_audioSink = nullptr; }
#endif
    m_audioIO = nullptr;
}

void AudioOutput::onDataReceived(const QByteArray &data) {
    if (m_audioIO) {
        m_audioIO->write(data);
    }
}