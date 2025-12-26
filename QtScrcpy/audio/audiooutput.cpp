#include "audiooutput.h"
#include <QCoreApplication>
#include <QDebug>
#include <QAudioFormat>
#include <QThread>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QMediaDevices>
#include <QAudioDevice>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif

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
        
        // OPTIMASI TCP: LOW LATENCY
        m_client->setSocketOption(QAbstractSocket::LowDelayOption, 1); // No Nagle
        m_client->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        m_client->setReadBufferSize(4096 * 4); // Kecilkan buffer baca

        emit clientConnected(m_client->peerAddress().toString());

        connect(m_client, &QTcpSocket::readyRead, this, [this]() {
            if (m_client) {
                // Emit raw data langsung ke Main Thread untuk diproses Audio Output
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

bool AudioOutput::start(const QString& serial, int port) {
    if (m_running) stop();
    m_currentSerial = serial;

    // 1. WAJIB: ADB REVERSE (Agar HP bisa connect ke localhost:port PC)
    // Kita jalankan synchronous karena ini syarat mutlak connection.
    qInfo() << "AudioOutput::Setting up adb reverse tcp:" << port;
    QProcess adb;
    QStringList adbArgs;
    adbArgs << "-s" << serial << "reverse" << QString("tcp:%1").arg(port) << QString("tcp:%1").arg(port);
    adb.start("adb", adbArgs);
    adb.waitForFinished(2000); // Tunggu max 2 detik
    if (adb.exitCode() != 0) {
        qWarning() << "AudioOutput::ADB Reverse failed! Audio might not work.";
    }

    // 2. Setup Worker Thread & Server
    m_serverWorker = new AudioServerWorker(port);
    m_serverWorker->moveToThread(&m_workerThread);

    connect(&m_workerThread, &QThread::finished, m_serverWorker, &QObject::deleteLater);
    connect(this, &AudioOutput::stop, m_serverWorker, &AudioServerWorker::stopServer, Qt::QueuedConnection);
    
    // Connect Data Signal: Worker -> AudioOutput (Main Thread) -> Speaker
    // Menggunakan QueuedConnection secara implisit karena beda thread.
    connect(m_serverWorker, &AudioServerWorker::dataReceived, this, &AudioOutput::onDataReceived);

    m_workerThread.start();
    
    // Trigger start server di thread worker
    QMetaObject::invokeMethod(m_serverWorker, "startServer", Qt::QueuedConnection);

    // 3. Setup Audio Device (PC Speaker)
    setupAudioDevice();

    // 4. Launch Android App (via Intent/Activity Manager)
    // Asumsi: Kita pakai command 'am start' via adb shell
    if (!runAppProcess(serial, port)) {
        stop();
        return false;
    }

    m_running = true;
    return true;
}

void AudioOutput::stop() {
    m_running = false;

    // Matikan Android App (Optional, biar rapi)
    QProcess::execute("adb", QStringList() << "-s" << m_currentSerial << "shell" << "am" << "broadcast" << "-a" << "com.aaudio.forwarder.STOP");
    
    // Matikan ADB Reverse
    // QProcess::execute("adb", QStringList() << "-s" << m_currentSerial << "reverse" << "--remove" << QString("tcp:%1").arg(port));

    if (m_workerThread.isRunning()) {
        m_workerThread.quit();
        m_workerThread.wait();
    }
    
    cleanupAudioDevice();
}

bool AudioOutput::runAppProcess(const QString& serial, int port) {
    // Kita kirim Intent untuk start service/activity di Android
    // Pastikan format komponen sesuai dengan package name di AndroidManifest
    QString cmd = QString("am start -n com.aaudio.forwarder/.MainActivity --ei PORT %1").arg(port);
    
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
        qWarning() << "AudioOutput::Format not supported, trying nearest.";
        format = info.nearestFormat(format);
    }
    m_audioOutput = new QAudioOutput(format, this);
    // EXTREME LOW LATENCY: 10ms Buffer
    // 48000 * 2 * 2 = 192,000 bytes/sec -> 10ms = 1920 bytes
    m_audioOutput->setBufferSize(1920); 
    m_audioIO = m_audioOutput->start();
#else
    format.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!device.isFormatSupported(format)) {
        qWarning() << "AudioOutput::Format not supported";
    }

    m_audioSink = new QAudioSink(device, format, this);
    // EXTREME LOW LATENCY: 10ms Buffer
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