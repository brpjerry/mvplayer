#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QStringList>
#include <QVariantMap>

class MediaSession;

// The two MPRIS interfaces on /org/mpris/MediaPlayer2, reading from and
// asking through a MediaSession. Names and types follow the specification:
// https://specifications.freedesktop.org/mpris-spec/latest/

// org.mpris.MediaPlayer2: the application.
class MprisRoot : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool Fullscreen READ fullscreen WRITE setFullscreen)
    Q_PROPERTY(bool CanSetFullscreen READ canSetFullscreen)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    MprisRoot(MediaSession *session, QObject *parent);

    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool fullscreen() const;
    void setFullscreen(bool on);
    bool canSetFullscreen() const;
    bool hasTrackList() const { return false; }
    QString identity() const;
    QString desktopEntry() const;
    // Only the library's own videos play; nothing is opened from outside.
    QStringList supportedUriSchemes() const { return {}; }
    QStringList supportedMimeTypes() const { return {}; }

public slots:
    void Raise();
    void Quit();

private:
    MediaSession *m_session;
};

// org.mpris.MediaPlayer2.Player: the playing video.
class MprisPlayer : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(bool CanGoNext READ canStep)
    Q_PROPERTY(bool CanGoPrevious READ canStep)
    Q_PROPERTY(bool CanPlay READ hasTrack)
    Q_PROPERTY(bool CanPause READ hasTrack)
    Q_PROPERTY(bool CanSeek READ hasTrack)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    MprisPlayer(MediaSession *session, QObject *parent);

    QString playbackStatus() const;
    QString loopStatus() const;
    void setLoopStatus(const QString &status);
    double rate() const { return 1.0; }
    void setRate(double rate);
    bool shuffle() const;
    void setShuffle(bool on);
    QVariantMap metadata() const;
    double volume() const;
    void setVolume(double volume);
    qlonglong position() const; // microseconds, as are all MPRIS times
    bool canStep() const;
    bool hasTrack() const;
    bool canControl() const { return true; }

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offset);
    void SetPosition(const QDBusObjectPath &trackId, qlonglong position);
    void OpenUri(const QString &uri);

signals:
    void Seeked(qlonglong position);

private:
    QDBusObjectPath trackId() const;

    MediaSession *m_session;
};
