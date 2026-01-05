#include <QApplication>
#include <QDebug>
#include <QFile>
#ifdef Q_OS_LINUX
#include <QFileInfo>
#include <QIcon>
#endif
#include <QSurfaceFormat>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTranslator>
#include <QDateTime>
#include <QStandardPaths>
#include <QDir>

#include "config.h"
#include "dialog.h"
#include "mousetap/mousetap.h"
#include "adbprocess.h"

static Dialog *g_mainDlg = Q_NULLPTR;
static QtMessageHandler g_oldMessageHandler = Q_NULLPTR;
void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg);
void installTranslator();

static QtMsgType g_msgType = QtInfoMsg;
QtMsgType covertLogLevel(const QString &logLevel);

int main(int argc, char *argv[])
{
    // 1. SETUP PATHS (Robust & Relative)
    // Menggunakan path relatif terhadap executable agar portable (sesuai output CMake)
    QString appPath = QCoreApplication::applicationDirPath();

#ifdef Q_OS_WIN32
    // Cek dulu di folder yang sama (Deployment), fallback ke struktur dev jika tidak ada
    QString adbPath = appPath + "/adb.exe";
    if (!QFile::exists(adbPath)) {
        adbPath = "../../../QtScrcpy/QtScrcpyCore/src/third_party/adb/win/adb.exe";
    }
    qputenv("QTSCRCPY_ADB_PATH", adbPath.toLocal8Bit());
    
    // Server logic similar...
    QString serverPath = appPath + "/scrcpy-server";
    if (!QFile::exists(serverPath)) {
        serverPath = "../../../QtScrcpy/QtScrcpyCore/src/third_party/scrcpy-server";
    }
    qputenv("QTSCRCPY_SERVER_PATH", serverPath.toLocal8Bit());
    qputenv("QTSCRCPY_KEYMAP_PATH", (appPath + "/keymap").toLocal8Bit());
    qputenv("QTSCRCPY_CONFIG_PATH", (appPath + "/config").toLocal8Bit());
#endif

#ifdef Q_OS_OSX
    // Mac Bundle Structure handling
    QString contentsPath = appPath + "/../"; 
    qputenv("QTSCRCPY_ADB_PATH", (contentsPath + "MacOS/adb").toLocal8Bit());
    qputenv("QTSCRCPY_SERVER_PATH", (contentsPath + "MacOS/scrcpy-server").toLocal8Bit());
    qputenv("QTSCRCPY_KEYMAP_PATH", (contentsPath + "Resources/keymap").toLocal8Bit());
    qputenv("QTSCRCPY_CONFIG_PATH", (contentsPath + "Resources/config").toLocal8Bit());
#endif

#ifdef Q_OS_LINUX
    // Linux CMake Deployment (Everything is in bin/)
    qputenv("QTSCRCPY_ADB_PATH", QString("/usr/bin/adb").toLocal8Bit());
    qputenv("QTSCRCPY_SERVER_PATH", (appPath + "/scrcpy-server").toLocal8Bit());
    qputenv("QTSCRCPY_KEYMAP_PATH", (appPath + "/keymap").toLocal8Bit());
    qputenv("QTSCRCPY_CONFIG_PATH", (appPath + "/config").toLocal8Bit());
#endif

    g_msgType = covertLogLevel(Config::getInstance().getLogLevel());

    // 2. OPENGL CONFIGURATION (CRITICAL FOR PBO)
    // Setup Global OpenGL Format sebelum QApplication dibuat
    QSurfaceFormat varFormat = QSurfaceFormat::defaultFormat();
    
    // Optimization: Disable Depth & Stencil (We only draw 2D Video)
    varFormat.setDepthBufferSize(0);
    varFormat.setStencilBufferSize(0);
    
    // Modern OpenGL: Request 3.3 Core Profile
    // Ini WAJIB agar QOpenGLExtraFunctions (PBO, glMapBufferRange) bekerja
    varFormat.setVersion(3, 3); 
    varFormat.setProfile(QSurfaceFormat::CoreProfile);
    
    // Performance: Disable VSync (Low Latency)
    varFormat.setSwapInterval(0); 
    
    QSurfaceFormat::setDefaultFormat(varFormat);

    // 3. HIGH DPI SCALING (QT5 COMPAT)
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    #if (QT_VERSION >= QT_VERSION_CHECK(5,14,0))
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    #endif
#endif

    // Setup Logger
    g_oldMessageHandler = qInstallMessageHandler(myMessageOutput);
    
    QApplication a(argc, argv);

    // Linux Icon
#ifdef Q_OS_LINUX
    QIcon appIcon(":/image/tray/logo.png");
    if (!appIcon.isNull()) {
        a.setWindowIcon(appIcon);
    }
#endif

    // Debug Info
    qDebug() << "App Name:" << a.applicationName();
    qDebug() << "App Version:" << a.applicationVersion();
    qDebug() << "OpenGL Context Requested: 3.3 Core Profile";

    // Version String Logic
    QStringList versionList = QCoreApplication::applicationVersion().split(".");
    if (versionList.size() >= 3) {
        QString version = versionList[0] + "." + versionList[1] + "." + versionList[2];
        a.setApplicationVersion(version);
    }

    installTranslator();
    
#if defined(Q_OS_WIN32) || defined(Q_OS_OSX)
    MouseTap::getInstance()->initMouseEventTap();
#endif

    // Load Stylesheet
    QFile file(":/qss/psblack.css");
    if (file.open(QFile::ReadOnly)) {
        QString qss = QLatin1String(file.readAll());
        // Simple validation to avoid crash if css is broken
        if (qss.length() > 30) {
            QString paletteColor = qss.mid(20, 7);
            qApp->setPalette(QPalette(QColor(paletteColor)));
            qApp->setStyleSheet(qss);
        }
        file.close();
    }

    // Set ADB Path in Core
    qsc::AdbProcess::setAdbPath(Config::getInstance().getAdbPath());

    // Show Main UI
    g_mainDlg = new Dialog {};
    g_mainDlg->show();

    int ret = a.exec();
    
    delete g_mainDlg;
    g_mainDlg = Q_NULLPTR; // Safety

#if defined(Q_OS_WIN32) || defined(Q_OS_OSX)
    MouseTap::getInstance()->quitMouseEventTap();
#endif
    return ret;
}

void installTranslator()
{
    static QTranslator translator;
    QLocale locale;
    QLocale::Language language = locale.language();

    QString configLang = Config::getInstance().getLanguage();
    if (configLang == "zh_CN") {
        language = QLocale::Chinese;
    } else if (configLang == "en_US") {
        language = QLocale::English;
    } else if (configLang == "ja_JP") {
        language = QLocale::Japanese;
    }

    QString languagePath = ":/i18n/";
    switch (language) {
    case QLocale::Chinese:
        languagePath += "zh_CN.qm";
        break;
    case QLocale::Japanese:
        languagePath += "ja_JP.qm";
        break;
    case QLocale::English:
    default:
        languagePath += "en_US.qm";
        break;
    }

    if (!translator.load(languagePath)) {
        qWarning() << "Failed to load translation file:" << languagePath;
    } else {
        qApp->installTranslator(&translator);
    }
}

QtMsgType covertLogLevel(const QString &logLevel)
{
    if ("debug" == logLevel) return QtDebugMsg;
    if ("info" == logLevel) return QtInfoMsg;
    if ("warn" == logLevel) return QtWarningMsg;
    if ("error" == logLevel) return QtCriticalMsg;

#ifdef QT_NO_DEBUG
    return QtInfoMsg;
#else
    return QtDebugMsg;
#endif
}

void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QString outputMsg;
    
#ifdef ENABLE_DETAILED_LOGS
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    
    if (context.file && context.line > 0) {
        QString fileName = QString::fromUtf8(context.file);
        int lastSlash = fileName.lastIndexOf('/');
        if (lastSlash >= 0) fileName = fileName.mid(lastSlash + 1);
        lastSlash = fileName.lastIndexOf('\\');
        if (lastSlash >= 0) fileName = fileName.mid(lastSlash + 1);
        
        outputMsg = QString("[ %1 %2: %3 ] %4").arg(timestamp).arg(fileName).arg(context.line).arg(msg);
    } else {
        outputMsg = QString("[%1] %2").arg(timestamp).arg(msg);
    }

    QString prefix;
    switch (type) {
        case QtDebugMsg: prefix = "[debug] "; break;
        case QtInfoMsg: prefix = "[info] "; break;
        case QtWarningMsg: prefix = "[warning] "; break;
        case QtCriticalMsg: prefix = "[critical] "; break;
        case QtFatalMsg: prefix = "[fatal] "; break;
    }
    outputMsg.prepend(prefix);
    fprintf(stderr, "%s\n", outputMsg.toUtf8().constData());
#else
    outputMsg = msg;
    if (g_oldMessageHandler) {
        g_oldMessageHandler(type, context, outputMsg);
    }
#endif

    // Filter Logic untuk GUI Log Window
    float fLogLevel = g_msgType;
    if (QtInfoMsg == g_msgType) fLogLevel = QtDebugMsg + 0.5f;
    
    float fLogLevel2 = type;
    if (QtInfoMsg == type) fLogLevel2 = QtDebugMsg + 0.5f;

    if (fLogLevel <= fLogLevel2) {
        // Safety Check: Pastikan Dialog masih ada sebelum panggil method-nya
        if (g_mainDlg && g_mainDlg->isVisible() && !g_mainDlg->filterLog(outputMsg)) {
            g_mainDlg->outLog(outputMsg);
        }
    }

    if (QtFatalMsg == type) {
        // abort(); // Optional: uncomment if strict crash needed
    }
}