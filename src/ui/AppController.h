#pragma once

#include "core/ImportManager.h"
#include "ui/SystemTheme.h"
#include "ui/VideoModel.h"

#include <QAbstractListModel>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include <memory>

// Import jobs shown in the activity panel: running ones plus recent results.
class JobModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { TitleRole = Qt::UserRole + 1, ArtistRole, StageRole, ProgressRole, FinishedRole, OutcomeRole, DetailRole };

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void update(const JobStatus &s);
    void clear();

private:
    QVector<JobStatus> m_jobs;
};

struct AppOptions {
    QString configFile; // empty: the user's normal settings
    QStringList musicDirs; // command line overrides, not persisted
    QString mvDir;
    bool mute = false;  // force silence for this run (testing); never saved
};

// The object QML talks to: settings, library models and import activity.
class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(VideoFilterModel *videos READ videos CONSTANT)
    Q_PROPERTY(JobModel *jobs READ jobs CONSTANT)
    Q_PROPERTY(QVariantList facets READ facets NOTIFY facetsChanged)
    Q_PROPERTY(int videoCount READ videoCount NOTIFY facetsChanged)
    // Videos that wait for the user to accept or reject them.
    Q_PROPERTY(int reviewCount READ reviewCount NOTIFY facetsChanged)
    // Library videos whose tracks are gone, for the user to keep or delete.
    Q_PROPERTY(int orphanCount READ orphanCount NOTIFY facetsChanged)

    Q_PROPERTY(QStringList musicDirs READ musicDirs NOTIFY settingsChanged)
    Q_PROPERTY(QString mvDir READ mvDir WRITE setMvDir NOTIFY settingsChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY settingsChanged)
    Q_PROPERTY(bool replaceAudio READ replaceAudio WRITE setReplaceAudio NOTIFY settingsChanged)
    Q_PROPERTY(bool allowUnofficial READ allowUnofficial WRITE setAllowUnofficial NOTIFY settingsChanged)
    Q_PROPERTY(bool skipStillImages READ skipStillImages WRITE setSkipStillImages NOTIFY settingsChanged)
    // Subtitles: the languages fetched with each video ("en, ja"; empty for
    // none), and whether the player shows them.
    Q_PROPERTY(QString subtitleLangs READ subtitleLangs WRITE setSubtitleLangs NOTIFY settingsChanged)
    Q_PROPERTY(bool subtitlesOn READ subtitlesOn WRITE setSubtitlesOn NOTIFY settingsChanged)
    Q_PROPERTY(bool fetchingSubtitles READ fetchingSubtitles NOTIFY activityChanged)
    // How plain subtitles are drawn: outline thickness and drop shadow
    // offset, in mpv's units. Styled subtitles keep their own look.
    Q_PROPERTY(double subtitleOutline READ subtitleOutline WRITE setSubtitleOutline NOTIFY settingsChanged)
    Q_PROPERTY(double subtitleShadow READ subtitleShadow WRITE setSubtitleShadow NOTIFY settingsChanged)
    // Whether a video grows to the full view when it starts, or plays in its thumbnail.
    Q_PROPERTY(bool autoExpand READ autoExpand WRITE setAutoExpand NOTIFY settingsChanged)
    // Videos that no track in the music folders has.
    Q_PROPERTY(int untrackedCount READ untrackedCount NOTIFY activityChanged)
    // A YouTube Premium account's cookies.txt, kept beside the settings.
    Q_PROPERTY(bool hasCookies READ hasCookies NOTIFY settingsChanged)
    Q_PROPERTY(QString cookiesAdded READ cookiesAdded NOTIFY settingsChanged)
    Q_PROPERTY(bool checkingQuality READ checkingQuality NOTIFY activityChanged)
    // What asking YouTube about the cookies said: "" (not asked), "premium",
    // "ordinary", "expired" or "unknown", and the sentence that goes with it.
    Q_PROPERTY(bool checkingCookies READ checkingCookies NOTIFY cookiesCheckChanged)
    Q_PROPERTY(QString cookiesState READ cookiesState NOTIFY cookiesCheckChanged)
    Q_PROPERTY(QString cookiesStatus READ cookiesStatus NOTIFY cookiesCheckChanged)
    // Whether the account is used where YouTube refuses a guest with its
    // bot check ("Sign in to confirm you're not a bot"): for the user to say.
    Q_PROPERTY(bool accountOnBotCheck READ accountOnBotCheck WRITE setAccountOnBotCheck NOTIFY settingsChanged)

    // Replacing a video by hand: whether a search, check or replacement
    // runs, what it is doing, and for which video.
    Q_PROPERTY(bool replacing READ replacing NOTIFY replaceChanged)
    Q_PROPERTY(QString replaceStage READ replaceStage NOTIFY replaceChanged)
    Q_PROPERTY(qint64 replaceVideoId READ replaceVideoId NOTIFY replaceChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY activityChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY activityChanged)
    Q_PROPERTY(int remaining READ remaining NOTIFY activityChanged)
    Q_PROPERTY(int sessionDone READ sessionDone NOTIFY activityChanged)
    Q_PROPERTY(QVariantMap trackCounts READ trackCounts NOTIFY activityChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY activityChanged)
    // YouTube is refusing requests; the queue waits and resumes by itself.
    Q_PROPERTY(bool importPaused READ importPaused NOTIFY activityChanged)
    Q_PROPERTY(QString pauseReason READ pauseReason NOTIFY activityChanged)
    // What the pause wants of the user: "" (nothing: requests are limited
    // and it passes), "signin" (YouTube asks for a sign-in, and the account
    // can give it if the user agrees) or "cookies" (it asks for one, and
    // there is no account whose cookies work).
    Q_PROPERTY(QString pauseKind READ pauseKind NOTIFY activityChanged)

    // "auto" (follow the playing video) or a colour such as "#8b7dff"
    Q_PROPERTY(QString accent READ accent WRITE setAccent NOTIFY appearanceChanged)
    // "auto" (follow the operating system), "dark" or "light"
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY appearanceChanged)
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)
    // Which tag the sidebar lists: albumArtist | artist | genre | album | year
    Q_PROPERTY(QString sidebarFacet READ sidebarFacet WRITE setSidebarFacet NOTIFY appearanceChanged)

    // Scroll feel; set in the config file (ui/touchpadGain, ui/wheelStep,
    // ui/flickDeceleration).
    Q_PROPERTY(double flickDeceleration READ flickDeceleration CONSTANT)
    Q_PROPERTY(double touchpadGain READ touchpadGain CONSTANT)
    Q_PROPERTY(double wheelStep READ wheelStep CONSTANT)

    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY volumeChanged)

public:
    explicit AppController(const AppOptions &options, QObject *parent = nullptr);
    ~AppController() override;

    VideoFilterModel *videos() const { return m_filter; }
    JobModel *jobs() const { return m_jobs; }
    QVariantList facets() const { return m_facets; }
    int videoCount() const { return m_model->rowCount() - m_reviewVideos; }
    int reviewCount() const { return m_reviewCount; }
    Q_INVOKABLE void approveVideo(qint64 videoId);
    Q_INVOKABLE void rejectVideo(qint64 videoId);
    int orphanCount() const { return m_orphanCount; }
    Q_INVOKABLE void keepOrphan(qint64 videoId);
    // False while the music folders are being read.
    Q_INVOKABLE bool deleteOrphan(qint64 videoId);

    QStringList musicDirs() const { return m_cfg.musicDirs; }
    Q_INVOKABLE void addMusicDir(const QString &dir);
    Q_INVOKABLE void removeMusicDir(const QString &dir);
    QString mvDir() const { return m_cfg.mvDir; }
    void setMvDir(const QString &dir);
    bool configured() const;
    bool replaceAudio() const { return m_cfg.replaceAudio; }
    void setReplaceAudio(bool v);
    bool allowUnofficial() const { return m_cfg.allowUnofficial; }
    void setAllowUnofficial(bool v);
    bool skipStillImages() const { return m_cfg.skipStillImages; }
    QString subtitleLangs() const { return m_cfg.subtitleLangs.join(QStringLiteral(", ")); }
    void setSubtitleLangs(const QString &langs);
    bool subtitlesOn() const { return m_subtitlesOn; }
    void setSubtitlesOn(bool on);
    bool fetchingSubtitles() const;
    Q_INVOKABLE void fetchSubtitles();
    double subtitleOutline() const { return m_subtitleOutline; }
    void setSubtitleOutline(double v);
    double subtitleShadow() const { return m_subtitleShadow; }
    void setSubtitleShadow(double v);
    bool autoExpand() const { return m_autoExpand; }
    void setAutoExpand(bool v);
    int untrackedCount() const { return m_untrackedCount; }
    Q_INVOKABLE int deleteUntracked();
    void setSkipStillImages(bool v);
    bool hasCookies() const { return !m_cfg.cookiesFile.isEmpty(); }
    QString cookiesAdded() const;
    // Stores a copy of `file`. Returns what is wrong with it, or nothing.
    Q_INVOKABLE QString importCookies(const QString &file);
    Q_INVOKABLE void removeCookies();
    bool checkingCookies() const { return m_checkingCookies; }
    QString cookiesState() const { return m_cookiesState; }
    QString cookiesStatus() const { return m_cookiesStatus; }
    Q_INVOKABLE void checkCookies();
    bool accountOnBotCheck() const { return m_cfg.accountOnBotCheck; }
    void setAccountOnBotCheck(bool v);
    bool checkingQuality() const;
    Q_INVOKABLE void checkQuality();

    bool busy() const;
    bool scanning() const;
    int remaining() const;
    int sessionDone() const { return m_sessionDone; }
    QVariantMap trackCounts() const { return m_trackCounts; }
    QString statusText() const;
    bool importPaused() const;
    QString pauseReason() const;
    QString pauseKind() const;
    Q_INVOKABLE void resumeImport();

    QString accent() const { return m_accent; }
    void setAccent(const QString &accent);
    QString themeMode() const { return m_themeMode; }
    void setThemeMode(const QString &mode);
    bool systemDark() const { return m_systemTheme.dark(); }
    QString sidebarFacet() const { return m_sidebarFacet; }
    void setSidebarFacet(const QString &key);

    double flickDeceleration() const { return m_flickDeceleration; }
    double touchpadGain() const { return m_touchpadGain; }
    double wheelStep() const { return m_wheelStep; }

    double volume() const { return m_volume; }
    void setVolume(double v);
    bool muted() const { return m_muted || m_options.mute; }
    void setMuted(bool m);

    Q_INVOKABLE void rescan();
    Q_INVOKABLE void retryUnmatched();
    // Looks tracks up again, their videos judged by today's rules (see
    // ImportManager::reimport): those of one video, of every video of the
    // section on show with a tag, or of the music files at a path (a file
    // or a folder). Each returns how many lookups were queued.
    Q_INVOKABLE int reimportVideo(qint64 videoId);
    Q_INVOKABLE int reimportFacet(const QString &type, const QString &value);
    Q_INVOKABLE int reimportPath(const QString &path);
    // Replacing a video by hand (ImportManager::replaceSearch and friends).
    // replaceCheck takes a YouTube link or id; it returns what is wrong with
    // it, or nothing and the answer comes as replaceCheckDone.
    bool replacing() const { return m_manager && m_manager->replacing(); }
    QString replaceStage() const { return m_replaceStage; }
    qint64 replaceVideoId() const { return m_replaceVideoId; }
    Q_INVOKABLE bool replaceSearch(qint64 videoId);
    Q_INVOKABLE QString replaceCheck(qint64 videoId, const QString &text);
    Q_INVOKABLE bool replaceWith(qint64 videoId, const QString &ytId);
    static QString youtubeId(const QString &text);
    Q_INVOKABLE QVariantList unmatchedTracks() const;
    // What a video stands for: {ytUrl, ytTitle, ytChannel, videoLength,
    // files}. files: the music files that have it, the one whose tags it
    // carries first, each {path, absent, length, format, tags: [{key, value}]}.
    Q_INVOKABLE QVariantMap videoSources(qint64 videoId) const;
    // How long the first of those files plays ("4:59"); empty with none.
    Q_INVOKABLE QString trackLength(qint64 videoId) const;
    Q_INVOKABLE QString urlToPath(const QUrl &url) const;
    Q_INVOKABLE QUrl pathToUrl(const QString &path) const;
    Q_INVOKABLE QString displayPath(const QString &path) const;

    void shutdown();

signals:
    void facetsChanged();
    void settingsChanged();
    void activityChanged();
    void cookiesCheckChanged();
    void volumeChanged();
    void appearanceChanged();
    void systemDarkChanged();
    void videoImported(const QVariantMap &video);
    void replaceChanged();
    // Up to three uploads that are the track by ear, or why there are none.
    void replaceOptionsReady(qint64 videoId, const QVariantList &options, const QString &error);
    // {id, title, channel, duration, thumbnail, matches, reason, error?}
    void replaceCheckDone(qint64 videoId, const QVariantMap &result);

private:
    void openLibrary();
    void closeLibrary();
    void rebuildFacets();
    void refreshCounts();
    void saveSettings();
    QString cookiesPath() const;
    void setCookiesCheck(const QString &state, const QString &status);
    // The music files that have a video, those still in the music folders
    // first, the best of them first. Needs the library open.
    QVector<TrackInfo> sourceTracks(const VideoInfo &v) const;

    AppOptions m_options;
    std::unique_ptr<QSettings> m_settings;
    ImportSettings m_cfg;
    bool m_checkingCookies = false;
    int m_cookiesCheck = 0; // counts checks, so that a late answer about replaced cookies is dropped
    QString m_cookiesState, m_cookiesStatus;

    std::unique_ptr<Database> m_db;
    std::unique_ptr<ImportManager> m_manager;
    VideoModel *m_model;
    VideoFilterModel *m_filter;
    JobModel *m_jobs;

    QVariantList m_facets;
    QTimer m_facetTimer;
    QVariantMap m_trackCounts;
    int m_sessionDone = 0;
    int m_reviewCount = 0;  // tracks with videos waiting for review
    int m_reviewVideos = 0; // those videos, counting every option
    int m_orphanCount = 0;
    QString m_accent = QStringLiteral("auto");
    QString m_sidebarFacet = QStringLiteral("albumArtist");
    SystemTheme m_systemTheme;
    QString m_themeMode = QStringLiteral("auto");
    double m_flickDeceleration = 4800; // px/s²: how quickly a flick glides to a stop
    double m_touchpadGain = 4.0;
    double m_wheelStep = 170;
    double m_volume = 1.0;
    bool m_muted = false;
    bool m_subtitlesOn = true;
    double m_subtitleOutline = 2.2;
    double m_subtitleShadow = 0;
    bool m_autoExpand = true;
    int m_untrackedCount = 0;
    QString m_replaceStage;
    qint64 m_replaceVideoId = 0;
};
