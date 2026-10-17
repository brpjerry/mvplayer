#include "ui/AppController.h"
#include "ui/FrameStats.h"
#include "ui/IdleInhibitor.h"
#include "ui/IpcServer.h"
#include "ui/MediaSession.h"
#include "ui/MpvItem.h"
#include "ui/PointerPacer.h"
#include "ui/SingleInstance.h"
#include "ui/WindowFrame.h"
#include "ui/YtDlpUpdater.h"

#include "core/Util.h"

#include <QCommandLineParser>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QMouseEvent>
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

    // Started again for the same library: the running instance shows its
    // window instead, and this one is done.
    const QString instanceName = SingleInstance::nameFor(parser.value(QStringLiteral("config")), parser.value(QStringLiteral("mv-dir")));
    if (SingleInstance::handOver(instanceName))
        return 0;

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
    MediaSession mediaSession;
    YtDlpUpdater ytDlpUpdater;
    WindowFrame windowFrame;
    PointerPacer pointerPacer;

    const char *uri = "MvPlayer.Core";
    qmlRegisterType<MpvItem>(uri, 1, 0, "MpvItem");
    qmlRegisterSingletonInstance(uri, 1, 0, "App", &controller);
    qmlRegisterSingletonInstance(uri, 1, 0, "FrameStats", &frameStats);
    qmlRegisterSingletonInstance(uri, 1, 0, "IdleInhibitor", &idleInhibitor);
    qmlRegisterSingletonInstance(uri, 1, 0, "MediaSession", &mediaSession);
    qmlRegisterSingletonInstance(uri, 1, 0, "YtDlpUpdater", &ytDlpUpdater);
    qmlRegisterSingletonInstance(uri, 1, 0, "WindowFrame", &windowFrame);
    qmlRegisterAnonymousType<VideoFilterModel>(uri, 1);
    qmlRegisterAnonymousType<AlbumFilterModel>(uri, 1);
    qmlRegisterAnonymousType<JobModel>(uri, 1);
    qmlRegisterUncreatableType<FolderTreeModel>(uri, 1, 0, "FolderTreeModel", QStringLiteral("App.folderTree is the one there is"));

    int rc = 1;
    {
        QQmlApplicationEngine engine;
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                         [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
        engine.loadFromModule("MvPlayer", "Main");
        if (engine.rootObjects().isEmpty())
            return 1;

        QObject *root = engine.rootObjects().constFirst();
        if (auto *window = qobject_cast<QQuickWindow *>(root)) {
            frameStats.attach(window);
            windowFrame.attach(window);
            pointerPacer.attach(window);
        }

        SingleInstance instance(instanceName);
        QObject::connect(&instance, &SingleInstance::raiseRequested, root, [root](const QString &token) {
            // The token lets the window take focus on Wayland; Qt's
            // requestActivate() spends it from the environment.
            if (!token.isEmpty())
                qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
            QMetaObject::invokeMethod(root, "bringUp");
        });

        std::unique_ptr<IpcServer> ipc;
        if (parser.isSet(QStringLiteral("ipc"))) {
            ipc = std::make_unique<IpcServer>(parser.value(QStringLiteral("ipc")), [root](const QString &line) {
                // grab-window <file>: the whole window, popups included,
                // which QML (it grabs items) cannot picture.
                static const QString grabWindow = QStringLiteral("grab-window ");
                if (line.startsWith(grabWindow)) {
                    auto *window = qobject_cast<QQuickWindow *>(root);
                    return window && window->grabWindow().save(line.mid(grabWindow.size())) ? QStringLiteral("ok")
                                                                                             : QStringLiteral("failed");
                }
                // pointer <x> <y>: the pointer is moved there, in the
                // window's own coordinates; click <x> <y>: and the left
                // button pressed and released. What hovering brings up
                // cannot be tried otherwise without a pointer of the desk's.
                const bool click = line.startsWith(QLatin1String("click "));
                if (click || line.startsWith(QLatin1String("pointer "))) {
                    auto *window = qobject_cast<QQuickWindow *>(root);
                    const QStringList at = line.section(QLatin1Char(' '), 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
                    if (!window || at.size() != 2)
                        return QStringLiteral("failed");
                    const QPointF local(at[0].toDouble(), at[1].toDouble());
                    const QPointF global = window->mapToGlobal(local);
                    const auto send = [&](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons held) {
                        QMouseEvent e(type, local, local, global, button, held, Qt::NoModifier);
                        QCoreApplication::sendEvent(window, &e);
                    };
                    send(QEvent::MouseMove, Qt::NoButton, Qt::NoButton);
                    if (click) {
                        send(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
                        send(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
                    }
                    return QStringLiteral("ok");
                }
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
