#pragma once

#include <QColor>
#include <QQuickFramebufferObject>
#include <QTimer>
#include <QVariantList>

#include <memory>

struct MpvCore;
struct mpv_event_property;

// Video surface backed by libmpv's render API. mpv draws into an FBO owned by
// the Qt Quick scene graph, so the video composites like any other item.
class MpvItem : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(int audioTrack READ audioTrack WRITE setAudioTrack NOTIFY audioTrackChanged)
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY audioTracksChanged)
    // Subtitles kept beside the video: whether it has any, whether they are
    // shown, and the languages preferred ("en,ja").
    Q_PROPERTY(bool hasSubtitles READ hasSubtitles NOTIFY hasSubtitlesChanged)
    Q_PROPERTY(bool subtitlesVisible READ subtitlesVisible WRITE setSubtitlesVisible NOTIFY subtitlesVisibleChanged)
    Q_PROPERTY(QString subtitleLangs READ subtitleLangs WRITE setSubtitleLangs NOTIFY subtitleLangsChanged)
    Q_PROPERTY(double subtitleOutline READ subtitleOutline WRITE setSubtitleOutline NOTIFY subtitleStyleChanged)
    Q_PROPERTY(double subtitleShadow READ subtitleShadow WRITE setSubtitleShadow NOTIFY subtitleStyleChanged)
    Q_PROPERTY(QSize videoSize READ videoSize NOTIFY videoSizeChanged)
    Q_PROPERTY(QString hwdec READ hwdec NOTIFY hwdecChanged)
    // While enabled, `frameColor` follows the picture: an accent colour taken
    // from a frame once a second.
    Q_PROPERTY(bool colorSampling READ colorSampling WRITE setColorSampling NOTIFY colorSamplingChanged)
    Q_PROPERTY(QColor frameColor READ frameColor NOTIFY frameColorChanged)
    Q_PROPERTY(bool frameColorValid READ frameColorValid NOTIFY frameColorChanged)

public:
    explicit MpvItem(QQuickItem *parent = nullptr);
    ~MpvItem() override;

    Renderer *createRenderer() const override;

    double position() const { return m_position; }
    double duration() const { return m_duration; }
    bool paused() const { return m_paused; }
    void setPaused(bool paused);
    bool active() const { return m_active; }
    double volume() const { return m_volume; }
    void setVolume(double v); // 0..1
    bool muted() const { return m_muted; }
    void setMuted(bool m);
    int audioTrack() const { return m_audioTrack; }
    void setAudioTrack(int id);
    QVariantList audioTracks() const { return m_audioTracks; }
    bool hasSubtitles() const { return m_hasSubtitles; }
    bool subtitlesVisible() const { return m_subtitlesVisible; }
    void setSubtitlesVisible(bool v);
    QString subtitleLangs() const { return m_subtitleLangs; }
    void setSubtitleLangs(const QString &langs);
    double subtitleOutline() const { return m_subtitleOutline; }
    void setSubtitleOutline(double v);
    double subtitleShadow() const { return m_subtitleShadow; }
    void setSubtitleShadow(double v);
    QSize videoSize() const { return m_videoSize; }
    QString hwdec() const { return m_hwdec; }
    bool colorSampling() const { return m_colorSampling; }
    void setColorSampling(bool on);
    QColor frameColor() const { return m_frameColor; }
    bool frameColorValid() const { return m_frameColorValid; }

    // Set before any item is created: discard audio entirely (test runs).
    static void setSilent(bool silent);

    Q_INVOKABLE void load(const QString &path);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(double seconds, bool exact = true);
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void togglePause();

signals:
    void positionChanged();
    void durationChanged();
    void pausedChanged();
    void activeChanged();
    void volumeChanged();
    void mutedChanged();
    void audioTrackChanged();
    void audioTracksChanged();
    void hasSubtitlesChanged();
    void subtitlesVisibleChanged();
    void subtitleLangsChanged();
    void subtitleStyleChanged();
    void videoSizeChanged();
    void hwdecChanged();
    void colorSamplingChanged();
    void frameColorChanged();
    void firstFrame();  // the newly loaded file is now on screen
    void endReached();  // playback ran to the end of the file
    void loadFailed(const QString &reason);

private:
    void drainEvents();
    void handleProperty(const mpv_event_property *prop);
    void refreshTracks();
    void updateSampling();
    void requestSample();
    void onFramePixels(const QByteArray &rgba); // 16x9 RGBA from the renderer

    std::shared_ptr<MpvCore> m_core;
    double m_position = 0;
    double m_duration = 0;
    bool m_paused = false;
    bool m_active = false;
    bool m_awaitingFirstFrame = false;
    double m_volume = 1.0;
    bool m_muted = false;
    int m_audioTrack = 0;
    QVariantList m_audioTracks;
    bool m_hasSubtitles = false;
    bool m_subtitlesVisible = true;
    QString m_subtitleLangs;
    void redrawPaused();
    double m_subtitleOutline = 2.2;
    double m_subtitleShadow = 0;
    QSize m_videoSize;
    QString m_hwdec;

    bool m_colorSampling = false;
    QTimer m_sampleTimer;
    QColor m_frameColor;
    bool m_frameColorValid = false;
    bool m_restartColor = true; // next sample replaces the running average
    double m_avgR = 0, m_avgG = 0, m_avgB = 0;

    friend struct MpvCore;
};
