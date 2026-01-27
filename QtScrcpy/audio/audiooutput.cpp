#include "audiooutput.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QMediaDevices>
#include <QAudioDevice>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif

// Konfigurasi sesuai Audio-Forwarder (Kotlin)
static const QString APP_PACKAGE = "com.android.sound.helper";
static const QString APP_ACTIVITY = ".MainActivity";
static const QString APK_NAME = "soundservice.apk";
// Sample Rate HARUS sama dengan di Kotlin (48000)
static const int SAMPLE_RATE = 48000; 

// ================= WORKER IMPLEMENTATION =================

AudioServerWorker::AudioServerWorker(int port, QObject *parent)
    : QObject(parent), m_port(port) {}

AudioServerWorker::~AudioServerWorker() {
    stopServer();
}

void AudioServerWorker::startServer() {
    // Hindari double start
    if (m_server && m_server->isListening()) return;
    
    // Bersihkan sisa-sisa lama
    stopServer();

    m_server = new QTcpServer(this);

    // Listen Only on IPv4 Localhost/Remote (Any)
    if (!m_server->listen(QHostAddress::AnyIPv4, m_port)) {
        qCritical() << "[Audio] Failed to listen on port" << m_port;
        emit serverReady(false);
        return;
    }

    connect(m_server, &QTcpServer::newConnection, this, [this]() {
        if (!m_server) return;
        
        QTcpSocket *next = m_server->nextPendingConnection();
        
        // Hanya izinkan 1 koneksi audio aktif (Single Stream)
        if (m_client) {
            m_client->close();
            m_client->deleteLater();
        }
        m_client = next;

        // === OPTIMASI NETWORK (MATCHING C++ CLIENT) ===
        // Matikan Nagle Algorithm (Wajib untuk Realtime Audio)
        m_client->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        // KeepAlive agar tahu jika kabel putus
        m_client->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        // Buffer Read Socket: Jangan terlalu besar (bikin delay), jangan terlalu kecil (bikin stutter)
        // 16KB cukup untuk menampung ~80ms audio worst case, tapi kita baca secepatnya.
        m_client->setReadBufferSize(16 * 1024); 

        QString peerAddr = m_client->peerAddress().toString();
        qInfo() << "[Audio] Client connected from:" << peerAddr;
        emit clientConnected(peerAddr);

        // === STREAMING LOOP ===
        connect(m_client, &QTcpSocket::readyRead, this, [this]() {
            if (m_client) {
                // Langsung kirim raw bytes ke Audio Thread.
                // QByteArray menggunakan Implicit Sharing (COW), jadi ini efisien.
                emit dataReceived(m_client->readAll());
            }
        });

        connect(m_client, &QTcpSocket::disconnected, this, [this]() {
            qInfo() << "[Audio] Client disconnected";
            emit clientDisconnected();
            if (m_client) m_client->deleteLater();
        });
    });

    qInfo() << "[Audio] Server listening on port" << m_port;
    emit serverReady(true);
}

void AudioServerWorker::stopServer() {
    if (m_client) {
        m_client->disconnectFromHost();
        m_client->deleteLater();
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
    }
}

// ================= AUDIO OUTPUT CLASS =================

AudioOutput::AudioOutput(QObject *parent) : QObject(parent) {
    // Thread worker di-start SEKALI saja seumur hidup objek ini.
    // Ini mencegah crash akibat access violation pada thread yang sudah mati.
    m_workerThread.start();
    
    // Setup Audio Format (Fixed 16-bit PCM Stereo 48kHz)
    m_format.setSampleRate(SAMPLE_RATE);
    m_format.setChannelCount(2);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    m_format.setSampleSize(16);
    m_format.setCodec("audio/pcm");
    m_format.setByteOrder(QAudioFormat::LittleEndian);
    m_format.setSampleType(QAudioFormat::SignedInt);
#else
    m_format.setSampleFormat(QAudioFormat::Int16);
#endif
}

AudioOutput::~AudioOutput() {
    stop();
    m_workerThread.quit();
    m_workerThread.wait();
}

bool AudioOutput::start(const QString& serial, int port) {
    stop(); // Pastikan bersih

    // --- 1. Init Worker ---
    // Hapus worker lama jika ada (dijadwalkan delete di thread worker)
    if (m_serverWorker) m_serverWorker->deleteLater();

    // Buat worker baru
    AudioServerWorker* worker = new AudioServerWorker(port);
    worker->moveToThread(&m_workerThread);
    m_serverWorker = worker;

    // Wiring Signal/Slot
    connect(this, &AudioOutput::stopRequested, worker, &AudioServerWorker::stopServer);
    connect(worker, &AudioServerWorker::dataReceived, this, &AudioOutput::onDataReceived);
    // Auto-clean worker saat thread mati (safety net)
    connect(&m_workerThread, &QThread::finished, worker, &QObject::deleteLater);

    // Trigger Start (Thread-Safe)
    QMetaObject::invokeMethod(worker, "startServer", Qt::QueuedConnection);

    // --- 2. Setup PC Audio ---
    setupAudioDevice();

    // --- 3. Setup Android Connection (ADB) ---
    // Reverse Forwarding: HP connect ke PC Localhost
    QString portStr = QString::number(port);
    runAdbCommand(serial, {"reverse", "tcp:" + portStr, "tcp:" + portStr});

    // Grant Permissions (Penting untuk Android 13+)
    // 'record_audio' untuk mic/internal, 'project_media' untuk screen cast bypass
    runAdbCommand(serial, {"shell", "pm", "grant", APP_PACKAGE, "android.permission.RECORD_AUDIO"});
    runAdbCommand(serial, {"shell", "appops", "set", APP_PACKAGE, "PROJECT_MEDIA", "allow"}); 
    
    // Launch App
    return runAppProcess(serial, port);
}

void AudioOutput::stop() {
    emit stopRequested(); // Stop Server Socket

    // Clean Audio PC
    cleanupAudioDevice();

    // Clean Worker
    if (m_serverWorker) {
        m_serverWorker->deleteLater();
        m_serverWorker = nullptr;
    }

    // Kill Android Process (Opsional, biar hemat baterai HP)
    if (m_appProcess.state() != QProcess::NotRunning) {
        m_appProcess.kill();
        m_appProcess.waitForFinished(500);
    }
}

bool AudioOutput::install(const QString& serial) {
    // Logic pencarian APK yang lebih robust
    QStringList searchPaths = {
        QCoreApplication::applicationDirPath() + "/sndcpy/" + APK_NAME,
        QCoreApplication::applicationDirPath() + "/" + APK_NAME,
        QDir::currentPath() + "/" + APK_NAME
    };

    QString apkPath;
    for (const auto& path : searchPaths) {
        if (QFileInfo::exists(path)) {
            apkPath = path;
            break;
        }
    }

    if (apkPath.isEmpty()) {
        qWarning() << "[Audio] APK not found!";
        return false;
    }

    qInfo() << "[Audio] Installing:" << apkPath;
    // -g: Grant all runtime permissions
    // -r: Reinstall allowed
    // -t: Allow test packages
    return runAdbCommand(serial, {"install", "-r", "-g", "-t", apkPath});
}

bool AudioOutput::runAdbCommand(const QString& serial, const QStringList& args) {
    QProcess adb;
    QString adbPath = qgetenv("QTSCRCPY_ADB_PATH");
    if (adbPath.isEmpty()) adbPath = "adb";

    QStringList params;
    params << "-s" << serial << args;

    adb.start(adbPath, params);
    if (!adb.waitForStarted()) return false;
    if (!adb.waitForFinished()) return false;
    return (adb.exitCode() == 0);
}

bool AudioOutput::runAppProcess(const QString& serial, int port) {
    // Start Service via Activity (MainActivity menghandle startService)
    QString cmd = QString("am start -n %1/%2 --ei PORT %3")
                      .arg(APP_PACKAGE, APP_ACTIVITY)
                      .arg(port);

    qInfo() << "[Audio] Launching:" << cmd;

    QString adbPath = qgetenv("QTSCRCPY_ADB_PATH");
    if (adbPath.isEmpty()) adbPath = "adb";

    // Jalankan secara detached/background process
    m_appProcess.start(adbPath, {"-s", serial, "shell", cmd});
    return m_appProcess.waitForStarted(1000);
}

void AudioOutput::setupAudioDevice() {
    cleanupAudioDevice(); // Safety

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());
    if (!info.isFormatSupported(m_format)) {
        qWarning() << "[Audio] Raw 48k format not supported, adapting...";
        m_format = info.nearestFormat(m_format);
    }
    m_audioOutput = new QAudioOutput(m_format, this);
    // Buffer: 48kHz * 2ch * 2bytes * 0.04s (40ms) = ~7.6KB
    // Buffer terlalu kecil = Underrun (kresek), Terlalu besar = Latency
    m_audioOutput->setBufferSize(7680); 
    m_audioIO = m_audioOutput->start();
#else
    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    m_audioSink = new QAudioSink(device, m_format, this);
    m_audioSink->setBufferSize(7680); // Target ~40ms Latency
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
        m_audioSink->stop(); // Stop engine sebelum delete! Penting untuk mencegah SEGV
        delete m_audioSink;
        m_audioSink = nullptr;
    }
#endif
    m_audioIO = nullptr;
}

void AudioOutput::onDataReceived(const QByteArray &data) {
    if (m_audioIO) {
        // Tulis data ke Sound Card
        // QAudioSink akan memblokir jika buffer penuh, menjaga sinkronisasi
        m_audioIO->write(data);
    }
}