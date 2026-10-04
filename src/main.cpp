#include "ui/AppController.h"
#include "ui/FrameStats.h"
#include "ui/IdleInhibitor.h"
#include "ui/IpcServer.h"
#include "ui/MpvItem.h"
#include "ui/YtDlpUpdater.h"

#include "core/Util.h"

#include <QCommandLineParser>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>

#include <cstdio>

static void logToStderr(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *tag = type == QtWarningMsg ? "warning: "
                    : type == QtCriticalMsg || type == QtFatalMsg ? "error: " : "";
    std::fprintf(stderr, "%s%s\n", tag, consoleText(msg).constData());
}

int main(int argc, char **argv)
{
    initConsole();
    qInstallMessageHandler(logToStderr);

    // libmpv renders through OpenGL, so the scene graph has to as well.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("mvplayer"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("MV Player"));
    QGuiApplication::setOrganizationName(QStringLiteral("mvplayer"));
    QGuiApplication::setDesktopFileName(QStringLiteral("mvplayer"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A music player for music videos."));
    parser.addHelpOption();
    parser.addOptions({
        {QStringLiteral("music-dir"), QStringLiteral("Use this audio library folder for this run (repeatable)."), QStringLiteral("dir")},
        {QStringLiteral("mv-dir"), QStringLiteral("Use this music video library for this run."), QStringLiteral("dir")},
        {QStringLiteral("config"), QStringLiteral("Read and write settings in this file."), QStringLiteral("file")},
        {QStringLiteral("fps"), QStringLiteral("Show the frame rate counter (also F12).")},
        {QStringLiteral("mute"), QStringLiteral("Keep audio muted for this run.")},
        {QStringLiteral("ipc"), QStringLiteral("Listen for automation commands on this local socket."), QStringLiteral("name")},
    });
    parser.process(app);

    // Prefer a clean UI face when one is installed; CJK falls back via the
    // platform's font matching.
    const QStringList families = QFontDatabase::families();
    for (const QString &family : {QStringLiteral("Inter"), QStringLiteral("Inter Variable"), QStringLiteral("Adwaita Sans"),
                                  QStringLiteral("Segoe UI Variable Text")}) {
        if (families.contains(family)) {
            QFont font(family);
            font.setPixelSize(14);
            QGuiApplication::setFont(font);
            break;
        }
    }

    AppOptions options;
    options.configFile = parser.value(QStringLiteral("config"));
    options.musicDirs = parser.values(QStringLiteral("music-dir"));
    options.mvDir = parser.value(QStringLiteral("mv-dir"));
    options.mute = parser.isSet(QStringLiteral("mute"));
    MpvItem::setSilent(options.mute);

    AppController controller(options);
    FrameStats frameStats;
    frameStats.setVisible(parser.isSet(QStringLiteral("fps")));
    IdleInhibitor idleInhibitor;
    YtDlpUpdater ytDlpUpdater;

    const char *uri = "MvPlayer.Core";
    qmlRegisterType<MpvItem>(uri, 1, 0, "MpvItem");
    qmlRegisterSingletonInstance(uri, 1, 0, "App", &controller);
    qmlRegisterSingletonInstance(uri, 1, 0, "FrameStats", &frameStats);
    qmlRegisterSingletonInstance(uri, 1, 0, "IdleInhibitor", &idleInhibitor);
    qmlRegisterSingletonInstance(uri, 1, 0, "YtDlpUpdater", &ytDlpUpdater);
    qmlRegisterAnonymousType<VideoFilterModel>(uri, 1);
    qmlRegisterAnonymousType<JobModel>(uri, 1);

    int rc = 1;
    {
        QQmlApplicationEngine engine;
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                         [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
        engine.loadFromModule("MvPlayer", "Main");
        if (engine.rootObjects().isEmpty())
            return 1;

        QObject *root = engine.rootObjects().constFirst();
        if (auto *window = qobject_cast<QQuickWindow *>(root))
            frameStats.attach(window);

        std::unique_ptr<IpcServer> ipc;
        if (parser.isSet(QStringLiteral("ipc"))) {
            ipc = std::make_unique<IpcServer>(parser.value(QStringLiteral("ipc")), [root](const QString &line) {
                QVariant result;
                QMetaObject::invokeMethod(root, "ipc", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, line));
                return result.toString();
            });
        }

        // Logging out or `kill` should end imports as cleanly as closing the window.
        quitOnTerminationSignals([] { QCoreApplication::quit(); });

        rc = app.exec();
    }
    controller.shutdown();
    return rc;
}
