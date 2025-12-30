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

// --- KONFIGURASI APLIKASI ANDROID ---
static const QString APP_PACKAGE = "com.android.sound.helper";
static const QString APP_ACTIVITY = ".MainActivity";
static const QString APK_NAME = "soundservice.apk";

// ================= WORKER IMPLEMENTATION (Server Thread) =================

AudioServerWorker::AudioServerWorker(int port, QObject *parent)
: QObject(parent), m_port(port) {}

AudioServerWorker::~AudioServerWorker() {
    stopServer();
}

void AudioServerWorker::startServer() {
    if (m_server && m_server->isListening()) return;

    m_server = new QTcpServer(this);

    // Listen di semua interface (AnyIPv4) pada port yang ditentukan
    if (!m_server->listen(QHostAddress::AnyIPv4, m_port)) {
        qCritical() << "[Audio] Failed to listen on port" << m_port;
        emit serverReady(false);
        return;
    }

    // Handle koneksi masuk dari HP
    connect(m_server, &QTcpServer::newConnection, this, [this]() {
        // Ambil koneksi
        QTcpSocket *next = m_server->nextPendingConnection();

        if (m_client) {
            // Jika ada koneksi lama nyangkut, putus dulu
            m_client->close();
            m_client->deleteLater();
        }
        m_client = next;

        // --- OPTIMIZATION: LOW LATENCY FOR FPS GAMING ---
        // Matikan Nagle's Algorithm di sisi PC juga untuk ACK yang cepat
        m_client->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_client->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        // Set buffer socket secukupnya, jangan terlalu besar biar realtime
        m_client->setReadBufferSize(4096 * 4);
        // ------------------------------------------------

        QString peerAddr = m_client->peerAddress().toString();
        qInfo() << "[Audio] Client connected from:" << peerAddr;
        emit clientConnected(peerAddr);

        // Handle data audio masuk (PCM Stream)
        connect(m_client, &QTcpSocket::readyRead, this, [this]() {
            if (m_client) {
                // Emit data mentah ke AudioOutput untuk diputar
                emit dataReceived(m_client->readAll());
            }
        });

        connect(m_client, &QTcpSocket::disconnected, this, [this]() {
            qInfo() << "[Audio] Client disconnected";
            emit clientDisconnected();
            if (m_client) {
                m_client->deleteLater();
                m_client = nullptr;
            }
        });
    });

    qInfo() << "[Audio] Server listening on port" << m_port;
    emit serverReady(true);
}

void AudioServerWorker::stopServer() {
    if (m_client) {
        m_client->disconnectFromHost();
        m_client->deleteLater();
        m_client = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

// ================= AUDIO OUTPUT CLASS (Main Controller) =================

AudioOutput::AudioOutput(QObject *parent) : QObject(parent) {
    // Port 0 = Auto (tapi nanti akan di-override logic start)
    // Di sini kita inisialisasi worker
    m_serverWorker = new AudioServerWorker(0);

    // Pindahkan worker ke thread terpisah agar tidak laggy (UI freeze)
    m_serverWorker->moveToThread(&m_workerThread);

    connect(&m_workerThread, &QThread::finished, m_serverWorker, &QObject::deleteLater);
    connect(this, &AudioOutput::stopRequested, m_serverWorker, &AudioServerWorker::stopServer);

    // Sambungkan data dari Worker (Thread B) ke AudioOutput (Thread A)
    // Menggunakan QueuedConnection otomatis karena beda thread
    connect(m_serverWorker, &AudioServerWorker::dataReceived, this, &AudioOutput::onDataReceived);

    m_workerThread.start();
}

AudioOutput::~AudioOutput() {
    stop();
    m_workerThread.quit();
    m_workerThread.wait();
}

bool AudioOutput::start(const QString& serial, int port) {
    // 1. Matikan proses lama jika ada
    stop();

    // 2. Restart Worker dengan Port baru
    // Kita harus set property port di worker secara thread-safe atau create ulang
    // Untuk simplifikasi, kita asumsikan port fixed sesuai parameter user (default 28200)
    // Jika perlu dinamis, Worker perlu metode setPort.
    // Tapi di sini kita hack sedikit: worker sudah created, kita re-init servernya.
    // (Note: Idealnya worker dibuat ulang kalau port berubah, tapi ini cukup)

    // Matikan server lama di thread worker
    emit stopRequested();

    // Kita perlu mekanisme untuk set port baru ke worker jika berubah
    // Tapi karena logic AudioServerWorker di header kamu (sebelumnya) fix di constructor,
    // kita delete dan buat baru worker-nya agar bersih
    m_workerThread.quit();
    m_workerThread.wait();

    m_serverWorker = new AudioServerWorker(port);
    m_serverWorker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_serverWorker, &QObject::deleteLater);
    connect(this, &AudioOutput::stopRequested, m_serverWorker, &AudioServerWorker::stopServer);
    connect(m_serverWorker, &AudioServerWorker::dataReceived, this, &AudioOutput::onDataReceived);
    m_workerThread.start();

    // Start Server di thread worker
    QMetaObject::invokeMethod(m_serverWorker, "startServer", Qt::BlockingQueuedConnection);

    // 3. Setup Audio Device (Speaker PC)
    setupAudioDevice();

    // 4. SETUP KONEKSI & PERMISSION (PENTING!)

    // A. ADB REVERSE (Agar HP bisa connect ke localhost PC)
    QString portStr = QString::number(port);
    QString remote = QString("tcp:%1").arg(portStr);
    QString local = QString("tcp:%1").arg(portStr);
    if (!runAdbCommand(serial, QStringList() << "reverse" << remote << local)) {
        qWarning() << "[Audio] ADB Reverse failed. Audio streaming might fail.";
    } else {
        qInfo() << "[Audio] ADB Reverse success:" << port;
    }

    // B. AUTO GRANT PERMISSION (Agar tidak perlu klik Allow manual di HP)
    // Izin: Microphone (RECORD_AUDIO)
    bool permGranted = runAdbCommand(serial, QStringList()
    << "shell" << "pm" << "grant" << APP_PACKAGE << "android.permission.RECORD_AUDIO");
    << "shell" << "appops" << "set" << APP_PACKAGE << "PROJECT_MEDIA" << "allow");

    if (permGranted) {
        qInfo() << "[Audio] Auto-grant RECORD_AUDIO permission success";
    } else {
        qWarning() << "[Audio] Failed to auto-grant permission. You might need to allow manually.";
    }

    // 5. Jalankan Aplikasi di Android
    return runAppProcess(serial, port);
}

void AudioOutput::stop() {
    emit stopRequested();

    // Bersihkan Audio Device
    cleanupAudioDevice();

    // Kill proses adb app (logcat/shell process jika ada yg running)
    if (m_appProcess.state() != QProcess::NotRunning) {
        m_appProcess.kill();
        m_appProcess.waitForFinished();
    }
}

bool AudioOutput::install(const QString& serial, int port) {
    Q_UNUSED(port);
    // Cari lokasi APK (asumsi ada di folder yang sama dengan executable / sndcpy)
    QString appPath = qgetenv("QTSCRCPY_ADB_PATH"); // Fallback path logic
    QString apkPath = QCoreApplication::applicationDirPath() + "/sndcpy/" + APK_NAME;

    QFileInfo info(apkPath);
    if (!info.exists()) {
        qWarning() << "[Audio] APK not found at:" << apkPath;
        // Coba fallback ke current dir
        apkPath = QCoreApplication::applicationDirPath() + "/" + APK_NAME;
        if (!QFileInfo::exists(apkPath)) return false;
    }

    qInfo() << "[Audio] Installing audio helper..." << apkPath;
    return runAdbCommand(serial, QStringList() << "install" << "-r" << "-g" << apkPath);
    // Flag -g mencoba grant semua permission, tapi kadang gagal di Android modern,
    // makanya kita tetap butuh 'pm grant' manual di fungsi start().
}

bool AudioOutput::runAdbCommand(const QString& serial, const QStringList& args) {
    QProcess adb;
    QString adbPath = qgetenv("QTSCRCPY_ADB_PATH");
    if (adbPath.isEmpty()) {
        adbPath = "adb"; // System path fallback
    }

    QStringList params;
    params << "-s" << serial << args;

    adb.start(adbPath, params);
    if (!adb.waitForStarted()) return false;
    if (!adb.waitForFinished()) return false;

    return (adb.exitCode() == 0);
}

bool AudioOutput::runAppProcess(const QString& serial, int port) {
    // Format: adb -s serial shell am start -n pkg/activity --ei PORT 28200
    // Menggunakan 'am start-foreground-service' (Android 8+) atau 'am start' biasa
    // Kita pakai 'start' activity biasa karena di MainActivity sudah handle startForegroundService

    QString cmd = QString("am start -n %1/%2 --ei PORT %3")
    .arg(APP_PACKAGE, APP_ACTIVITY)
    .arg(port);

    qInfo() << "[Audio] Launching app:" << cmd;

    // Kita jalankan app process secara non-blocking (fire and forget)
    // Tapi kita simpan QProcess member agar bisa di-kill saat stop()
    QString adbPath = qgetenv("QTSCRCPY_ADB_PATH");
    if (adbPath.isEmpty()) adbPath = "adb";

    QStringList params;
    params << "-s" << serial << "shell" << cmd;

    m_appProcess.start(adbPath, params);
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
        qWarning() << "[Audio] Raw format not supported, trying nearest...";
        format = info.nearestFormat(format);
    }
    m_audioOutput = new QAudioOutput(format, this);

    // --- LOW LATENCY BUFFER ---
    // Buffer audio di PC: 1920 sample * 4 bytes (16bit stereo) = ~7.6KB
    // Semakin kecil semakin realtime, tapi risiko suara "kresek" kalau CPU sibuk.
    // 1920 samples @ 48kHz = 40ms latency buffer. Cukup aman buat FPS.
    m_audioOutput->setBufferSize(1920 * 4);

    m_audioIO = m_audioOutput->start();
    #else
    format.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    m_audioSink = new QAudioSink(device, format, this);

    // Sama, buffer kecil untuk latency rendah
    m_audioSink->setBufferSize(1920 * 4);

    m_audioIO = m_audioSink->start();
    #endif
}

void AudioOutput::cleanupAudioDevice() {
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
    m_audioIO = nullptr;
}

void AudioOutput::onDataReceived(const QByteArray &data) {
    if (m_audioIO) {
        m_audioIO->write(data);
    }
}
