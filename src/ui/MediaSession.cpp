#include "ui/MediaSession.h"

#ifdef MV_HAVE_DBUS
#include "ui/Mpris.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QMetaClassInfo>
#include <QTimer>

#include <utility>

namespace {

const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kService = QStringLiteral("org.mpris.MediaPlayer2.mvplayer");

} // namespace
#endif

MediaSession::MediaSession(QObject *parent)
    : QObject(parent)
{
#ifdef MV_HAVE_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    // The object stays registered; only the well-known name, which is how
    // desktops find players, comes and goes with the video.
    m_object = new QObject(this);
    m_root = new MprisRoot(this, m_object);
    m_player = new MprisPlayer(this, m_object);
    if (!bus.registerObject(kPath, m_object, QDBusConnection::ExportAdaptors))
        m_object = nullptr;
#endif
}

MediaSession::~MediaSession()
{
    publish(false);
}

void MediaSession::setTrack(const QVariantMap &track)
{
    if (track == m_track)
        return;
    m_track = track;
    emit trackChanged();
    changed(QStringLiteral("Metadata"));
    // Starting and stopping are told by the bus name coming and going.
    publish(hasTrack());
}

void MediaSession::setPlaying(bool playing)
{
    if (playing == m_playing)
        return;
    m_playing = playing;
    emit playingChanged();
    changed(QStringLiteral("PlaybackStatus"));
}

void MediaSession::setPosition(double seconds)
{
    // Not announced: listeners extrapolate it, told only of jumps (seeked()).
    if (seconds == m_position)
        return;
    m_position = seconds;
    emit positionChanged();
}

void MediaSession::setVolume(double volume)
{
    if (volume == m_volume)
        return;
    m_volume = volume;
    emit volumeChanged();
    changed(QStringLiteral("Volume"));
}

void MediaSession::setShuffle(bool shuffle)
{
    if (shuffle == m_shuffle)
        return;
    m_shuffle = shuffle;
    emit shuffleChanged();
    changed(QStringLiteral("Shuffle"));
}

void MediaSession::setRepeatMode(int mode)
{
    if (mode == m_repeatMode)
        return;
    m_repeatMode = mode;
    emit repeatModeChanged();
    changed(QStringLiteral("LoopStatus"));
}

void MediaSession::setCanStep(bool canStep)
{
    if (canStep == m_canStep)
        return;
    m_canStep = canStep;
    emit canStepChanged();
    changed(QStringLiteral("CanGoNext"));
    changed(QStringLiteral("CanGoPrevious"));
}

void MediaSession::setFullscreen(bool fullscreen)
{
    if (fullscreen == m_fullscreen)
        return;
    m_fullscreen = fullscreen;
    emit fullscreenChanged();
    changed(QStringLiteral("Fullscreen"));
}

double MediaSession::length() const
{
    return m_track.value(QStringLiteral("duration")).toDouble();
}

void MediaSession::seeked()
{
#ifdef MV_HAVE_DBUS
    if (!m_service.isEmpty())
        emit m_player->Seeked(qlonglong(m_position * 1e6));
#endif
}

// Changes are gathered and announced together once control returns to the
// event loop: one PropertiesChanged per interface, not one per property.
// Without the bus name nobody listens; a desktop reads everything afresh
// when the name appears.
void MediaSession::changed(const QString &property)
{
#ifdef MV_HAVE_DBUS
    if (m_service.isEmpty())
        return;
    if (m_changed.isEmpty())
        QTimer::singleShot(0, this, &MediaSession::flush);
    m_changed.insert(property);
#else
    Q_UNUSED(property)
#endif
}

void MediaSession::flush()
{
#ifdef MV_HAVE_DBUS
    const QSet<QString> names = std::exchange(m_changed, {});
    if (m_service.isEmpty())
        return;
    for (const QDBusAbstractAdaptor *adaptor : QList<QDBusAbstractAdaptor *>{m_root, m_player}) {
        const QMetaObject *meta = adaptor->metaObject();
        QVariantMap values;
        for (const QString &name : names) {
            const QByteArray key = name.toLatin1();
            if (meta->indexOfProperty(key.constData()) >= 0)
                values.insert(name, adaptor->property(key.constData()));
        }
        if (values.isEmpty())
            continue;
        const char *iface = meta->classInfo(meta->indexOfClassInfo("D-Bus Interface")).value();
        QDBusMessage signal = QDBusMessage::createSignal(kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                         QStringLiteral("PropertiesChanged"));
        signal << QString::fromLatin1(iface) << values << QStringList();
        QDBusConnection::sessionBus().send(signal);
    }
#endif
}

void MediaSession::publish(bool on)
{
#ifdef MV_HAVE_DBUS
    if (!m_object || on == !m_service.isEmpty())
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!on) {
        bus.unregisterService(m_service);
        m_service.clear();
        return;
    }
    // A second instance takes a name of its own, as the specification asks.
    const QString pid = QString::number(QCoreApplication::applicationPid());
    for (const QString &name : {kService, kService + QStringLiteral(".instance") + pid}) {
        if (bus.registerService(name)) {
            m_service = name;
            return;
        }
    }
    qWarning("media controls: no bus name could be taken");
#else
    Q_UNUSED(on)
#endif
}
