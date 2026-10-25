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
#include <QKeySequence>
#include <QMouseEvent>
#include <QQuickItem>
#include <QPainter>
#include <QQuickWindow>
#include <QQuickItemGrabResult>
#include <QTimer>
#include <QEventLoop>
#include "ui/CoverProvider.h"
#include <QDateTime>
#include <qpa/qwindowsysteminterface.h>
#include <qpa/qwindowsysteminterface.h>

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
        engine.addImageProvider(QStringLiteral("cover"), new CoverProvider); // the engine owns it
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
            mediaSession.attach(window);
        }

        // A press outside the text field that has the keyboard takes the
        // keyboard away from it, before any item gets the press: with the
        // search box focused, Space typed a space and the shortcuts were
        // dead until the box was clicked away by chance.
        struct FocusRelease : QObject {
            bool eventFilter(QObject *o, QEvent *e) override
            {
                if (e->type() != QEvent::MouseButtonPress)
                    return false;
                auto *window = qobject_cast<QQuickWindow *>(o);
                QQuickItem *field = window ? window->activeFocusItem() : nullptr;
                if (!field || !(field->inherits("QQuickTextInput") || field->inherits("QQuickTextEdit")))
                    return false;
                const QPointF local = field->mapFromScene(static_cast<QMouseEvent *>(e)->scenePosition());
                if (!field->contains(local))
                    field->setFocus(false);
                return false;
            }
        } focusRelease;
        if (auto *window = qobject_cast<QQuickWindow *>(root))
            window->installEventFilter(&focusRelease);

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
                // grab-window <file>: the whole window, popups included
                // (the overlay is a child of the content item). Grabbed as
                // an item: QQuickWindow::grabWindow() on an offscreen window
                // paints stray copies of the Shape icons over the picture.
                static const QString grabWindow = QStringLiteral("grab-window ");
                if (line.startsWith(grabWindow)) {
                    auto *window = qobject_cast<QQuickWindow *>(root);
                    if (!window)
                        return QStringLiteral("failed");
                    const QString file = line.mid(grabWindow.size());
                    auto result = window->contentItem()->grabToImage();
                    if (!result)
                        return QStringLiteral("failed");
                    QEventLoop wait;
                    QObject::connect(result.data(), &QQuickItemGrabResult::ready, &wait, &QEventLoop::quit);
                    QTimer::singleShot(5000, &wait, &QEventLoop::quit);
                    wait.exec(QEventLoop::ExcludeSocketNotifiers);
                    if (result->image().isNull())
                        return QStringLiteral("failed");
                    // On the window's own colour: the content item alone
                    // leaves the background clear wherever nothing is drawn.
                    QImage flat(result->image().size(), QImage::Format_ARGB32_Premultiplied);
                    flat.fill(window->color());
                    QPainter painter(&flat);
                    painter.drawImage(0, 0, result->image());
                    painter.end();
                    return flat.save(file) ? QStringLiteral("ok") : QStringLiteral("failed");
                }
                // pointer <x> <y>: the pointer is moved there, in the
                // window's own coordinates; click <x> <y>: and the left
                // button pressed and released; dblclick <x> <y>: twice, as
                // a double click arrives. What hovering brings up cannot
                // be tried otherwise without a pointer of the desk's.
                const bool click = line.startsWith(QLatin1String("click "));
                const bool rclick = line.startsWith(QLatin1String("rclick "));
                const bool dblclick = line.startsWith(QLatin1String("dblclick "));
                if (click || rclick || dblclick || line.startsWith(QLatin1String("pointer "))) {
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
                    } else if (rclick) { // rclick <x> <y>: the right button, for the menus
                        send(QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton);
                        send(QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton);
                    } else if (dblclick) {
                        // Through the window-system layer, which makes the
                        // second press a double click as it does for a real
                        // one; a double click handed to the window directly
                        // is taken for a plain press.
                        const ulong now = ulong(QDateTime::currentMSecsSinceEpoch());
                        const auto press = [&](ulong when, QEvent::Type type, Qt::MouseButtons held) {
                            QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
                                window, when, local, global, held, Qt::LeftButton, type);
                        };
                        // The second press a little later: at the same
                        // instant it would not count as a double click.
                        for (int i = 0; i < 2; ++i) {
                            press(now + ulong(i) * 50, QEvent::MouseButtonPress, Qt::LeftButton);
                            press(now + ulong(i) * 50, QEvent::MouseButtonRelease, Qt::NoButton);
                        }
                    }
                    return QStringLiteral("ok");
                }
                // key <sequence>: pressed and released as the keyboard
                // would, shortcuts included ("Space", "Ctrl+M", "Escape",
                // "F11"); type <text>: typed into what has the focus.
                const bool key = line.startsWith(QLatin1String("key "));
                if (key || line.startsWith(QLatin1String("type "))) {
                    auto *window = qobject_cast<QQuickWindow *>(root);
                    if (!window)
                        return QStringLiteral("failed");
                    const QString arg = line.section(QLatin1Char(' '), 1);
                    // Keys go to the active window; offscreen none is until asked.
                    if (!window->isActive())
                        window->requestActivate();
                    const auto press = [&](int k, Qt::KeyboardModifiers mods, const QString &text) {
                        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyPress, k, mods, text);
                        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(window, QEvent::KeyRelease, k, mods, text);
                    };
                    if (key) {
                        const QKeySequence seq(arg);
                        if (seq.isEmpty())
                            return QStringLiteral("failed");
                        const QKeyCombination combo = seq[0];
                        const int k = combo.key();
                        const QString text = k >= Qt::Key_Space && k <= Qt::Key_ydiaeresis && !(combo.keyboardModifiers() & ~Qt::ShiftModifier)
                            ? QString(QChar(k)).toLower() : QString();
                        press(k, combo.keyboardModifiers(), combo.keyboardModifiers() & Qt::ShiftModifier ? text.toUpper() : text);
                        return QStringLiteral("ok key=0x") + QString::number(k, 16) + QStringLiteral(" mods=0x") + QString::number(int(combo.keyboardModifiers()), 16);
                    } else {
                        for (const QChar c : arg)
                            press(0, Qt::NoModifier, QString(c));
                    }
                    return QStringLiteral("ok");
                }
                // focus: what has the keyboard focus (its type), as a key
                // would reach it first.
                if (line == QLatin1String("focus")) {
                    auto *window = qobject_cast<QQuickWindow *>(root);
                    QQuickItem *item = window ? window->activeFocusItem() : nullptr;
                    return item ? QString::fromLatin1(item->metaObject()->className()).section(QLatin1Char('_'), 0, 0) : QStringLiteral("none");
                }
                // resize <w> <h>: the window, within its minimum.
                if (line.startsWith(QLatin1String("resize "))) {
                    auto *window = qobject_cast<QQuickWindow *>(root);
                    const QStringList at = line.section(QLatin1Char(' '), 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
                    if (!window || at.size() != 2)
                        return QStringLiteral("failed");
                    window->resize(at[0].toInt(), at[1].toInt());
                    return QStringLiteral("ok");
                }
                // drag <x1> <y1> <x2> <y2>: the left button pressed at the
                // first point, moved to the second in steps, released.
                if (line.startsWith(QLatin1String("drag "))) {
                    auto *window = qobject_cast<QQuickWindow *>(root);
                    const QStringList at = line.section(QLatin1Char(' '), 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
                    if (!window || at.size() != 4)
                        return QStringLiteral("failed");
                    const QPointF from(at[0].toDouble(), at[1].toDouble()), to(at[2].toDouble(), at[3].toDouble());
                    const auto send = [&](QEvent::Type type, const QPointF &p, Qt::MouseButton button, Qt::MouseButtons held) {
                        QMouseEvent e(type, p, p, window->mapToGlobal(p), button, held, Qt::NoModifier);
                        QCoreApplication::sendEvent(window, &e);
                    };
                    send(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
                    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
                    const int steps = 12;
                    for (int i = 1; i <= steps; ++i)
                        send(QEvent::MouseMove, from + (to - from) * (double(i) / steps), Qt::NoButton, Qt::LeftButton);
                    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
                    return QStringLiteral("ok");
                }
                QVariant result;
                QMetaObject::invokeMethod(root, "ipc", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, line));
                return result.toString();
            });
        }

        // Logging out or `kill` should end imports as cleanly as closing the window.
        quitOnTerminationSignals([&controller] { controller.quit(); });

        rc = app.exec();
    }
    controller.shutdown();
    return rc;
}
