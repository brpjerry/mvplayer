#include "ui/Mpris.h"
#include "ui/MediaSession.h"

#include <QGuiApplication>
#include <QUrl>

namespace {

// Multi-valued tags are joined with "; " in the library.
QStringList tagValues(const QVariant &tag)
{
    return tag.toString().split(QStringLiteral("; "), Qt::SkipEmptyParts);
}

} // namespace

// ---- org.mpris.MediaPlayer2 ------------------------------------------------

MprisRoot::MprisRoot(MediaSession *session, QObject *parent)
    : QDBusAbstractAdaptor(parent)
    , m_session(session)
{
}

bool MprisRoot::fullscreen() const
{
    return m_session->fullscreen();
}

void MprisRoot::setFullscreen(bool on)
{
    if (m_session->hasTrack())
        emit m_session->fullscreenRequested(on);
}

bool MprisRoot::canSetFullscreen() const
{
    return m_session->hasTrack();
}

QString MprisRoot::identity() const
{
    return QGuiApplication::applicationDisplayName();
}

QString MprisRoot::desktopEntry() const
{
    return QGuiApplication::desktopFileName();
}

void MprisRoot::Raise()
{
    emit m_session->raiseRequested();
}

void MprisRoot::Quit()
{
    QCoreApplication::quit();
}

// ---- org.mpris.MediaPlayer2.Player -----------------------------------------

MprisPlayer::MprisPlayer(MediaSession *session, QObject *parent)
    : QDBusAbstractAdaptor(parent)
    , m_session(session)
{
}

QString MprisPlayer::playbackStatus() const
{
    if (!hasTrack())
        return QStringLiteral("Stopped");
    return m_session->playing() ? QStringLiteral("Playing") : QStringLiteral("Paused");
}

QString MprisPlayer::loopStatus() const
{
    switch (m_session->repeatMode()) {
    case 1: return QStringLiteral("Playlist");
    case 2: return QStringLiteral("Track");
    default: return QStringLiteral("None");
    }
}

void MprisPlayer::setLoopStatus(const QString &status)
{
    const int mode = QStringList{QStringLiteral("None"), QStringLiteral("Playlist"), QStringLiteral("Track")}.indexOf(status);
    if (mode >= 0)
        emit m_session->repeatModeRequested(mode);
}

void MprisPlayer::setRate(double rate)
{
    // Only normal speed; a rate of 0 means pause.
    if (rate == 0)
        Pause();
}

bool MprisPlayer::shuffle() const
{
    return m_session->shuffle();
}

void MprisPlayer::setShuffle(bool on)
{
    emit m_session->shuffleRequested(on);
}

QDBusObjectPath MprisPlayer::trackId() const
{
    if (!hasTrack())
        return QDBusObjectPath(QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack"));
    return QDBusObjectPath(QStringLiteral("/mvplayer/video/%1").arg(m_session->track().value(QStringLiteral("videoId")).toLongLong()));
}

QVariantMap MprisPlayer::metadata() const
{
    QVariantMap m;
    m.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(trackId()));
    if (!hasTrack())
        return m;
    const QVariantMap &t = m_session->track();
    if (m_session->length() > 0)
        m.insert(QStringLiteral("mpris:length"), qlonglong(m_session->length() * 1e6));
    const QString thumb = t.value(QStringLiteral("thumb")).toString();
    if (!thumb.isEmpty())
        m.insert(QStringLiteral("mpris:artUrl"), thumb);
    m.insert(QStringLiteral("xesam:url"), QUrl::fromLocalFile(t.value(QStringLiteral("path")).toString()).toString());
    m.insert(QStringLiteral("xesam:title"), t.value(QStringLiteral("title")).toString());
    m.insert(QStringLiteral("xesam:artist"), tagValues(t.value(QStringLiteral("artist"))));
    m.insert(QStringLiteral("xesam:albumArtist"), tagValues(t.value(QStringLiteral("albumArtist"))));
    m.insert(QStringLiteral("xesam:album"), t.value(QStringLiteral("album")).toString());
    m.insert(QStringLiteral("xesam:genre"), tagValues(t.value(QStringLiteral("genre"))));
    return m;
}

double MprisPlayer::volume() const
{
    return m_session->volume();
}

void MprisPlayer::setVolume(double volume)
{
    emit m_session->volumeRequested(qBound(0.0, volume, 1.0));
}

qlonglong MprisPlayer::position() const
{
    return qlonglong(m_session->position() * 1e6);
}

bool MprisPlayer::canStep() const
{
    return hasTrack() && m_session->canStep();
}

bool MprisPlayer::hasTrack() const
{
    return m_session->hasTrack();
}

void MprisPlayer::Next()
{
    if (canStep())
        emit m_session->nextRequested();
}

void MprisPlayer::Previous()
{
    if (canStep())
        emit m_session->previousRequested();
}

void MprisPlayer::Pause()
{
    if (hasTrack())
        emit m_session->pauseRequested();
}

void MprisPlayer::PlayPause()
{
    if (hasTrack())
        emit m_session->playPauseRequested();
}

void MprisPlayer::Stop()
{
    if (hasTrack())
        emit m_session->stopRequested();
}

void MprisPlayer::Play()
{
    if (hasTrack())
        emit m_session->playRequested();
}

void MprisPlayer::Seek(qlonglong offset)
{
    if (!hasTrack())
        return;
    const double target = m_session->position() + offset / 1e6;
    // Past the end is the next video, as the specification has it.
    if (m_session->length() > 0 && target > m_session->length())
        Next();
    else
        emit m_session->seekRequested(qMax(0.0, target));
}

void MprisPlayer::SetPosition(const QDBusObjectPath &id, qlonglong position)
{
    // Only for the video that is playing, and only within it.
    if (!hasTrack() || id != trackId() || position < 0
        || (m_session->length() > 0 && position / 1e6 > m_session->length()))
        return;
    emit m_session->seekRequested(position / 1e6);
}

void MprisPlayer::OpenUri(const QString &)
{
    // SupportedUriSchemes is empty: nothing to open.
}
