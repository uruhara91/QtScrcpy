#include <QAudioOutput>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTime>
#include <QThread>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QAudioDevice>
#include <QMediaDevices>
#endif

#include "audiooutput.h"

AudioOutput::AudioOutput(QObject *parent)
    : QObject(parent)
{
    m_running = false;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    m_audioOutput = nullptr;
#else
    m_audioSink = nullptr;
#endif

    // Pindahkan object ini ke worker thread agar UI tidak freeze saat proses audio berat
    // Note: Logika threading di QtScrcpy asli agak unik, kita ikuti pattern mereka
    // tapi server kita taruh di m_workerThread.
}

AudioOutput::~AudioOutput()
{
    stop();
}

bool AudioOutput::start(const QString& serial, int port)
{
    if (m_running) {
        stop();
    }

    // 1. Start Server DULUAN sebelum jalankan script
    // Agar saat aplikasi Android launch, port PC sudah terbuka (listening)
    startServer(port);

    QElapsedTimer timeConsumeCount;
    timeConsumeCount.start();

    // 2. Jalankan Script (Launch App Android)
    bool ret = runSndcpyProcess(serial, port);
    qInfo() << "AudioOutput::run sndcpy cost:" << timeConsumeCount.elapsed() << "milliseconds";
    if (!ret) {
        stopServer();
        return ret;
    }

    // 3. Siapkan Audio Device (Speaker)
    startAudioOutput();

    m_running = true;
    return true;
}

void AudioOutput::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;

    // Matikan proses di Android/Script
    if (QProcess::NotRunning != m_sndcpy.state()) {
        m_sndcpy.kill();
    }
    
    stopServer();
    stopAudioOutput();
}

void AudioOutput::installonly(const QString &serial, int port)
{
    runSndcpyProcess(serial, port, false);
}

bool AudioOutput::runSndcpyProcess(const QString &serial, int port, bool wait)
{
    if (QProcess::NotRunning != m_sndcpy.state()) {
        m_sndcpy.kill();
    }

#ifdef Q_OS_WIN32
    QStringList params{serial, QString::number(port)};
    m_sndcpy.start("sndcpy.bat", params);
#else
    // Kita asumsikan script wrapper kamu bernama sndcpy.sh
    QStringList params{"sndcpy.sh", serial, QString::number(port)};
    m_sndcpy.setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_sndcpy.start("bash", params);
#endif

    if (!wait) {
        return true;
    }
    
    // Kita tunggu sebentar untuk memastikan script jalan, 
    // tapi jangan terlalu lama blocking karena kita butuh event loop jalan
    // untuk terima koneksi di thread sebelah.
    if (!m_sndcpy.waitForStarted(2000)) {
        qWarning() << "AudioOutput::start sndcpy process failed";
        return false;
    }

    return true;
}

void AudioOutput::startAudioOutput()
{
    // FORMAT AUDIO: Sesuaikan dengan RECORD_AUDIO di Android (48000, Stereo, 16bit)
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
        qWarning() << "AudioOutput::audio format not supported.";
        return;
    }

    m_audioOutput = new QAudioOutput(format, this);
    // OPTIMASI BUFFER: Kecilkan buffer size untuk low latency!
    // Asumsi: 48kHz * 2 ch * 2 bytes = 192000 bytes/sec.
    // 20ms buffer = ~3840 bytes.
    m_audioOutput->setBufferSize(192000 * 0.050); // Set buffer ~50ms
    m_outputDevice = m_audioOutput->start();
#else
    format.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice defaultDevice = QMediaDevices::defaultAudioOutput();
    if (!defaultDevice.isFormatSupported(format)) {
        qWarning() << "AudioOutput::audio format not supported.";
        return;
    }

    m_audioSink = new QAudioSink(defaultDevice, format, this);
    // OPTIMASI BUFFER QT6
    m_audioSink->setBufferSize(192000 * 0.050); // Set buffer ~50ms
    m_outputDevice = m_audioSink->start();
#endif

    if (!m_outputDevice) {
        qWarning() << "AudioOutput::Failed to start audio device";
    }
}

void AudioOutput::stopAudioOutput()
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    if (m_audioOutput) {
        m_audioOutput->stop();
        delete m_audioOutput;
        m_audioOutput = nullptr;
    }
#else
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
#endif
    m_outputDevice = nullptr;
}

// === BAGIAN SERVER (YANG BARU) ===

void AudioOutput::startServer(int port)
{
    if (m_workerThread.isRunning()) {
        stopServer();
    }

    // Logic server harus jalan di worker thread biar ga nge-freeze GUI
    // Kita manfaatkan mekanisme moveToThread yang sudah ada
    m_server = new QTcpServer(); // Parent null dulu, akan dipindah ke thread
    m_server->moveToThread(&m_workerThread);

    connect(&m_workerThread, &QThread::started, m_server, [this, port]() {
        if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
             qCritical() << "AudioOutput::Server failed to listen on port" << port;
             return;
        }
        qInfo() << "AudioOutput::Server Listening on port" << port;
    }, Qt::DirectConnection);

    connect(&m_workerThread, &QThread::finished, m_server, &QObject::deleteLater);

    // Handle New Connection
    connect(m_server, &QTcpServer::newConnection, m_server, [this]() {
        if (!m_server) return;
        QTcpSocket *nextPending = m_server->nextPendingConnection();
        if (!nextPending) return;

        if (m_clientSocket) {
            // Kita cuma terima 1 koneksi (HP), reject yang lain atau replace?
            // Replace lebih aman buat reconnect scenario.
            m_clientSocket->close();
            m_clientSocket->deleteLater();
        }

        m_clientSocket = nextPending;
        qInfo() << "AudioOutput::Client Connected from" << m_clientSocket->peerAddress();

        // OPTIMASI SOCKET
        m_clientSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1); // TCP_NODELAY
        m_clientSocket->setReadBufferSize(4096 * 4); // Kecilkan read buffer

        connect(m_clientSocket, &QTcpSocket::readyRead, this, [this]() {
            if (!m_clientSocket || !m_outputDevice) return;
            
            // BACA DARI SOCKET -> TULIS KE SPEAKER (Zero Copy logic simulation)
            // Di Qt kita harus baca ke QByteArray dulu
            QByteArray data = m_clientSocket->readAll();
            if (data.isEmpty()) return;

            // Langsung tulis ke audio device
            m_outputDevice->write(data);
        }, Qt::DirectConnection); // DirectConnection penting agar jalan di thread yang sama

        connect(m_clientSocket, &QTcpSocket::disconnected, this, [this]() {
            qInfo() << "AudioOutput::Client Disconnected";
            if (m_clientSocket) m_clientSocket->deleteLater();
            m_clientSocket = nullptr;
        });
    });

    m_workerThread.start();
}

void AudioOutput::stopServer()
{
    if (m_workerThread.isRunning()) {
        m_workerThread.quit();
        m_workerThread.wait();
    }
    // Cleanup objek yang mungkin tertinggal (dihandle deleteLater via signal finished)
    m_clientSocket = nullptr;
    m_server = nullptr;
}