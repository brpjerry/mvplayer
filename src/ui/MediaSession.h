#pragma once

#include <QObject>
#include <QSet>
#include <QVariantMap>

class MprisRoot;
class MprisPlayer;

// What is playing, offered to the desktop's media controls (media keys,
// panel widgets, lock screen). QML binds the state in and acts on the
// requests; the queue stays in QML.
//
// Linux: MPRIS on the session bus, under org.mpris.MediaPlayer2.mvplayer
// while a video is loaded (idle, the name is left to other players, so media
// keys reach them). Other platforms need their own here (SMTC on Windows).
class MediaSession : public QObject
{
    Q_OBJECT
    // The playing video's map from the library model; empty when stopped.
    Q_PROPERTY(QVariantMap track READ track WRITE setTrack NOTIFY trackChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(double position READ position WRITE setPosition NOTIFY positionChanged) // seconds
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)         // 0..1
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY shuffleChanged)
    Q_PROPERTY(int repeatMode READ repeatMode WRITE setRepeatMode NOTIFY repeatModeChanged) // 0 off, 1 all, 2 one
    Q_PROPERTY(bool canStep READ canStep WRITE setCanStep NOTIFY canStepChanged)        // next / previous change the video
    Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY fullscreenChanged)

public:
    explicit MediaSession(QObject *parent = nullptr);
    ~MediaSession() override;

    QVariantMap track() const { return m_track; }
    void setTrack(const QVariantMap &track);
    bool playing() const { return m_playing; }
    void setPlaying(bool playing);
    double position() const { return m_position; }
    void setPosition(double seconds);
    double volume() const { return m_volume; }
    void setVolume(double volume);
    bool shuffle() const { return m_shuffle; }
    void setShuffle(bool shuffle);
    int repeatMode() const { return m_repeatMode; }
    void setRepeatMode(int mode);
    bool canStep() const { return m_canStep; }
    void setCanStep(bool canStep);
    bool fullscreen() const { return m_fullscreen; }
    void setFullscreen(bool fullscreen);

    bool hasTrack() const { return !m_track.isEmpty(); }
    double length() const; // seconds, 0 if unknown

    // The position jumped (a seek, or a video started over) rather than
    // moving on with playback.
    Q_INVOKABLE void seeked();

signals:
    void trackChanged();
    void playingChanged();
    void positionChanged();
    void volumeChanged();
    void shuffleChanged();
    void repeatModeChanged();
    void canStepChanged();
    void fullscreenChanged();

    // Asked for by the desktop.
    void playRequested();
    void pauseRequested();
    void playPauseRequested();
    void stopRequested();
    void nextRequested();
    void previousRequested();
    void seekRequested(double seconds); // absolute
    void volumeRequested(double volume);
    void shuffleRequested(bool shuffle);
    void repeatModeRequested(int mode);
    void fullscreenRequested(bool fullscreen);
    void raiseRequested();

private:
    void changed(const QString &property);
    void flush();
    void publish(bool on);

    QVariantMap m_track;
    bool m_playing = false;
    double m_position = 0;
    double m_volume = 1;
    bool m_shuffle = false;
    int m_repeatMode = 0;
    bool m_canStep = false;
    bool m_fullscreen = false;

#ifdef MV_HAVE_DBUS
    QObject *m_object = nullptr; // carries the adaptors at /org/mpris/MediaPlayer2
    MprisRoot *m_root = nullptr;
    MprisPlayer *m_player = nullptr;
    QString m_service;           // the bus name held, if any
    QSet<QString> m_changed;     // MPRIS properties to announce
#endif
};
