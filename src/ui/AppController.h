#pragma once

#include "core/ImportManager.h"
#include "core/Judge.h"
#include "core/ChatGptAuth.h"
#include "ui/SystemTheme.h"
#include "ui/VideoModel.h"
#include "ui/AlbumModel.h"
#include "ui/CoverCache.h"
#include "ui/FolderTreeModel.h"

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
    Q_PROPERTY(bool undoAvailable READ undoAvailable NOTIFY undoChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY undoChanged)
    // Library videos whose tracks are gone, for the user to keep or delete.
    Q_PROPERTY(int orphanCount READ orphanCount NOTIFY facetsChanged)

    // Which library the window shows: "video" (the music videos) or
    // "music" (the albums of the music library, played from their files).
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(AlbumFilterModel *albums READ albums CONSTANT)
    Q_PROPERTY(int albumCount READ albumCount NOTIFY facetsChanged)
    Q_PROPERTY(int libraryTrackCount READ libraryTrackCount NOTIFY facetsChanged)
    // Albums asked for from the grid ("Find videos for this album"): looked
    // up wherever their files are (docs/import-rules.html, A12). Each
    // {albumArtist, album}.
    Q_PROPERTY(QVariantList lookupAlbums READ lookupAlbums NOTIFY lookupAlbumsChanged)

    // The music library: a base directory, and the folders ticked under it
    // (musicDirs, the ones that are read). The tree shows the base's folders.
    Q_PROPERTY(QString baseDir READ baseDir WRITE setBaseDir NOTIFY settingsChanged)
    Q_PROPERTY(QStringList musicDirs READ musicDirs NOTIFY settingsChanged)
    Q_PROPERTY(FolderTreeModel *folderTree READ folderTree CONSTANT)
    Q_PROPERTY(QString mvDir READ mvDir WRITE setMvDir NOTIFY settingsChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY settingsChanged)
    Q_PROPERTY(bool replaceAudio READ replaceAudio WRITE setReplaceAudio NOTIFY settingsChanged)
    Q_PROPERTY(bool allowUnofficial READ allowUnofficial WRITE setAllowUnofficial NOTIFY settingsChanged)
    Q_PROPERTY(bool skipStillImages READ skipStillImages WRITE setSkipStillImages NOTIFY settingsChanged)
    // Uploads from channels with fewer than minSubscribers subscribers are
    // ignored while ignoreSmallChannels is on; the limit stays when it is off.
    Q_PROPERTY(bool ignoreSmallChannels READ ignoreSmallChannels WRITE setIgnoreSmallChannels NOTIFY settingsChanged)
    Q_PROPERTY(int minSubscribers READ minSubscribers WRITE setMinSubscribers NOTIFY settingsChanged)
    // The channels the user allowed and denied: {id, name, handle, list}
    // each, list "allow" or "deny".
    Q_PROPERTY(QVariantList listedChannels READ listedChannels NOTIFY channelListsChanged)
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
    // Music mode: a track that has its music video plays the video, grown
    // over the albums; the next track without one goes back to the music.
    Q_PROPERTY(bool videoFollows READ videoFollows WRITE setVideoFollows NOTIFY settingsChanged)
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
    Q_PROPERTY(bool keepEverything READ keepEverything WRITE setKeepEverything NOTIFY settingsChanged)
    // Claude, for judging the review queue: whether an API key is stored,
    // how it reads with its middle left out, and what the last check of it
    // found (judgeState: ok | bad | error, empty before a check).
    Q_PROPERTY(bool judgeKeySet READ judgeKeySet NOTIFY judgeChanged)
    Q_PROPERTY(QString judgeKeyHint READ judgeKeyHint NOTIFY judgeChanged)
    Q_PROPERTY(bool judgeChecking READ judgeChecking NOTIFY judgeChanged)
    Q_PROPERTY(QString judgeState READ judgeState NOTIFY judgeChanged)
    Q_PROPERTY(QString judgeStatus READ judgeStatus NOTIFY judgeChanged)
    // What the judge does (docs/import-rules-judge.html, X4): "off",
    // "advice" (a recommendation on cards under review) or "judge" (it runs
    // the lookup); which model; and how many lookups it has settled alone
    // and left for your verdict.
    Q_PROPERTY(QString judgeMode READ judgeMode WRITE setJudgeMode NOTIFY settingsChanged)
    Q_PROPERTY(QString judgeModel READ judgeModel WRITE setJudgeModel NOTIFY settingsChanged)
    Q_PROPERTY(int judgeThreads READ judgeThreads WRITE setJudgeThreads NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList judgeModels READ judgeModels NOTIFY judgeChanged)
    Q_PROPERTY(QString judgeProvider READ judgeProvider WRITE setJudgeProvider NOTIFY judgeChanged)
    Q_PROPERTY(bool judgeReady READ judgeReady NOTIFY judgeChanged)
    Q_PROPERTY(QObject *chatGpt READ chatGpt CONSTANT)
    Q_PROPERTY(int judgeAlone READ judgeAlone NOTIFY judgeChanged)
    Q_PROPERTY(int judgeLeft READ judgeLeft NOTIFY judgeChanged)
    Q_PROPERTY(int adviceLeft READ adviceLeft NOTIFY adviceQueueChanged)

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
    // How many cards across in the video grid and in the album grid
    // (ui/thumbColumns, ui/coverColumns), within each one's range.
    Q_PROPERTY(int thumbColumns READ thumbColumns WRITE setThumbColumns NOTIFY appearanceChanged)
    Q_PROPERTY(int coverColumns READ coverColumns WRITE setCoverColumns NOTIFY appearanceChanged)
    // The panel right of the album grid (queue, track, cover) is on show;
    // off until it is asked for again (ui/musicPanel).
    Q_PROPERTY(bool musicPanel READ musicPanel WRITE setMusicPanel NOTIFY appearanceChanged)
    Q_PROPERTY(int thumbColumnsMin READ thumbColumnsMin CONSTANT)
    Q_PROPERTY(int thumbColumnsMax READ thumbColumnsMax CONSTANT)
    Q_PROPERTY(int coverColumnsMin READ coverColumnsMin CONSTANT)
    Q_PROPERTY(int coverColumnsMax READ coverColumnsMax CONSTANT)
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
    // The verdict on a video under review. Accepted, it is the video of
    // its candidate track on show; rejected, of none of them.
    Q_INVOKABLE void approveVideo(qint64 videoId);
    Q_INVOKABLE void rejectVideo(qint64 videoId);
    // The last verdict can be taken back until the next one (or the app
    // closes); undoLabel names what it would take back.
    bool undoAvailable() const { return m_manager && m_manager->undoAvailable(); }
    QString undoLabel() const { return m_manager ? m_manager->undoLabel() : QString(); }
    Q_INVOKABLE void undoVerdict();
    // Allowing or denying the channel of a video under review. Offered
    // where the channel is not an artist's already for the candidate
    // showing and the library kept its id: {offered, id, name, handle}.
    Q_INVOKABLE QVariantMap channelActions(qint64 videoId) const;
    // What allowing (or denying) that channel would change: {id, name,
    // handle, allow, videos: [{videoId, ytTitle, thumb, review, tracks}]}.
    // Allowing: every video under review from it; which of them would be
    // accepted is found out by listening and arrives with channelJudged,
    // then channelPreviewDone. Denying: every video the library has from it.
    Q_INVOKABLE QVariantMap channelChange(qint64 videoId, bool allow);
    Q_INVOKABLE void cancelChannelChange();
    Q_INVOKABLE void applyChannelChange(const QString &channelId, bool allow);
    Q_INVOKABLE void unlistChannel(const QString &channelId);
    // The candidate tracks of a video under review, the tracks whose video
    // it could be, in the order they are shown: each {trackId, title, artist,
    // album, length, stream, audioDetail, elsewhere}. stream: which audio
    // track of the video is that track's audio, from 1 (0: it has none, and
    // would keep YouTube's). elsewhere: the other videos under review the
    // track is a candidate of, by id.
    Q_INVOKABLE QVariantList candidates(qint64 videoId) const;
    // Which of them the video's card and the panel show, from 0.
    Q_INVOKABLE int shownCandidate(qint64 videoId) const;
    Q_INVOKABLE void showCandidate(qint64 videoId, int index);
    // The audio track a video starts on, from 1: the first, or under review
    // that of its candidate on show.
    Q_INVOKABLE int firstAudioTrack(qint64 videoId) const;
    int orphanCount() const { return m_orphanCount; }
    QString mode() const { return m_mode; }
    void setMode(const QString &mode);
    AlbumFilterModel *albums() const { return m_albums; }
    int albumCount() const { return m_albumModel->rowCount(); }
    int libraryTrackCount() const { return m_libraryTracks; }
    // The files of the album at `row` of the albums on show, in disc and
    // track order, as the queue takes them: each {kind: "track", trackId,
    // videoId, videoPath, ytTitle, path, title, artist, albumArtist, album,
    // albumKey, year, trackNo, discNo, duration, durationText, thumb (the
    // cover), codec, bitrate, lossless, sampleRate, bits, lookup, state,
    // review: false}.
    Q_INVOKABLE QVariantList albumTracks(int row) const;
    QVariantList lookupAlbums() const;
    // Asks for an album (returns how many of its tracks were not looked up
    // until now), takes one off again, or looks an album's tracks up again
    // by today's rules (returns how many lookups were queued, H).
    Q_INVOKABLE int lookupAlbum(const QString &albumArtist, const QString &album);
    Q_INVOKABLE void removeLookupAlbum(const QString &albumArtist, const QString &album);
    Q_INVOKABLE int reimportAlbum(const QString &albumArtist, const QString &album);
    Q_INVOKABLE void keepOrphan(qint64 videoId);
    // False while the music folders are being read.
    Q_INVOKABLE bool deleteOrphan(qint64 videoId);

    QString baseDir() const { return m_baseDir; }
    void setBaseDir(const QString &dir);
    QStringList musicDirs() const { return m_cfg.musicDirs; }
    FolderTreeModel *folderTree() { return &m_folderTree; }
    // Ticks or unticks a folder of the tree (FolderTreeModel::withChecked).
    Q_INVOKABLE void setFolderChecked(const QString &path, bool on);
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
    bool ignoreSmallChannels() const { return m_ignoreSmallChannels; }
    int minSubscribers() const { return m_minSubscribers; }
    QVariantList listedChannels() const;
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
    bool videoFollows() const { return m_videoFollows; }
    void setVideoFollows(bool v);
    int untrackedCount() const { return m_untrackedCount; }
    Q_INVOKABLE int deleteUntracked();
    void setSkipStillImages(bool v);
    void setIgnoreSmallChannels(bool v);
    void setMinSubscribers(int n);
    bool hasCookies() const { return !m_cfg.cookiesFile.isEmpty(); }
    QString cookiesAdded() const;
    // Stores a copy of `file`. Returns what is wrong with it, or nothing.
    Q_INVOKABLE QString importCookies(const QString &file);
    Q_INVOKABLE void removeCookies();
    bool checkingCookies() const { return m_checkingCookies; }
    QString cookiesState() const { return m_cookiesState; }
    QString cookiesStatus() const { return m_cookiesStatus; }
    Q_INVOKABLE void checkCookies();
    bool judgeKeySet() const { return m_judge->hasKey(); }
    QString judgeKeyHint() const { return m_judge->keyHint(); }
    bool judgeChecking() const { return m_judge->checking(); }
    QString judgeState() const { return m_judge->state(); }
    QString judgeStatus() const { return m_judge->status(); }
    // Stores the key (empty: the reason it was not), and checks it at once.
    Q_INVOKABLE QString setJudgeKey(const QString &key);
    Q_INVOKABLE void removeJudgeKey();
    Q_INVOKABLE void checkJudgeKey();
    QString judgeMode() const { return m_cfg.judgeMode; }
    void setJudgeMode(const QString &mode);
    QString judgeModel() const { return m_cfg.judgeModel; }
    int judgeThreads() const { return m_cfg.judgeThreads; }
    void setJudgeThreads(int n);
    void setJudgeModel(const QString &model);
    QVariantList judgeModels() const;
    QString judgeProvider() const { return m_cfg.judgeProvider; }
    void setJudgeProvider(const QString &provider);
    bool judgeReady() const;
    QObject *chatGpt() const { return m_chatGpt.get(); }
    int judgeAlone() const { return m_judgeAlone; }
    int judgeLeft() const { return m_judgeLeft; }
    // The judge's advice on a video under review for one of its candidates
    // (X2): {asked, keep, probability, kind, source, version, whole, note,
    // mode, model, error}. askJudge asks for it (advice mode), or again.
    Q_INVOKABLE QVariantMap judgeAdvice(qint64 videoId, qint64 trackId);
    Q_INVOKABLE void askJudge(qint64 videoId, qint64 trackId, bool again);
    // "Review all": every card without an answer (X2); adviceLeft counts
    // the questions still queued.
    Q_INVOKABLE void askJudgeAll();
    int adviceLeft() const { return m_adviceLeft; }
    // "Accept all" (X4): what the judge's confident answers would decide,
    // {accept, reject} counts; acceptJudged carries them out as verdicts.
    Q_INVOKABLE QVariantMap judgePlan() const;
    Q_INVOKABLE void acceptJudged();
    bool accountOnBotCheck() const { return m_cfg.accountOnBotCheck; }
    // Keeping everything for a replay (README): the capture folder under
    // the videos folder; captureSizeText says how much it holds.
    bool keepEverything() const { return m_keepEverything; }
    void setKeepEverything(bool on);
    Q_INVOKABLE QString captureFolder() const;
    Q_INVOKABLE QString captureSizeText() const;
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
    int thumbColumns() const { return m_thumbColumns; }
    void setThumbColumns(int n);
    int coverColumns() const { return m_coverColumns; }
    void setCoverColumns(int n);
    bool musicPanel() const { return m_musicPanel; }
    void setMusicPanel(bool on);
    static constexpr int kThumbColumnsMin = 2;
    static constexpr int kThumbColumnsMax = 6;
    static constexpr int kCoverColumnsMin = 2;
    static constexpr int kCoverColumnsMax = 8;
    int thumbColumnsMin() const { return kThumbColumnsMin; }
    int thumbColumnsMax() const { return kThumbColumnsMax; }
    int coverColumnsMin() const { return kCoverColumnsMin; }
    int coverColumnsMax() const { return kCoverColumnsMax; }
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
    void channelListsChanged();
    void channelJudged(qint64 videoId, bool accept, const QString &track, const QString &reason);
    void channelPreviewDone();
    // What the videos under review have as candidates, or show of them, changed.
    void candidatesChanged();
    void undoChanged();
    void judgeChanged();
    void adviceChanged(qint64 videoId);
    void adviceQueueChanged();
    void facetsChanged();
    void modeChanged();
    void lookupAlbumsChanged();
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
    void reloadAlbums();
    void refreshCounts();
    void saveSettings();
    QString cookiesPath() const;
    void setCookiesCheck(const QString &state, const QString &status);
    // The music files that have a video, those still in the music folders
    // first, the best of them first. Needs the library open.
    QVector<TrackInfo> sourceTracks(const VideoInfo &v) const;
    void updateShown(const VideoInfo &v);
    QHash<qint64, qint64> m_shownKey; // video under review -> its candidate track on show (Database::trackKey)
    int m_channelTicket = 0;          // the preview of a channel change that is on show; 0: none

    AppOptions m_options;
    std::unique_ptr<QSettings> m_settings;
    ImportSettings m_cfg;
    QString m_baseDir;
    FolderTreeModel m_folderTree;
    void applyMusicDirs(QStringList dirs);
    // Keeps the ticked folders inside the base: outside ones go, an ancestor
    // of the base becomes the base itself.
    void fitDirsToBase();
    void syncLibraryDirs();
    bool m_checkingCookies = false;
    int m_cookiesCheck = 0; // counts checks, so that a late answer about replaced cookies is dropped
    QString m_cookiesState, m_cookiesStatus;
    std::unique_ptr<Judge> m_judge;
    std::unique_ptr<ChatGptAuth> m_chatGpt;

    std::unique_ptr<Database> m_db;
    std::unique_ptr<ImportManager> m_manager;
    VideoModel *m_model;
    VideoFilterModel *m_filter;
    JobModel *m_jobs;
    AlbumModel *m_albumModel;
    AlbumFilterModel *m_albums;
    CoverCache m_covers;
    QTimer m_albumTimer;
    QString m_mode = QStringLiteral("video");
    int m_libraryTracks = 0;

    QVariantList m_facets;
    QTimer m_facetTimer;
    QVariantMap m_trackCounts;
    int m_sessionDone = 0;
    bool m_ignoreSmallChannels = true;
    int m_minSubscribers = 5000;
    int m_judgeAlone = 0, m_judgeLeft = 0, m_adviceLeft = 0;
    bool m_keepEverything = false;
    int m_reviewCount = 0;  // videos waiting for review
    int m_reviewVideos = 0;
    int m_orphanCount = 0;
    QString m_accent = QStringLiteral("auto");
    QString m_sidebarFacet = QStringLiteral("albumArtist");
    SystemTheme m_systemTheme;
    QString m_themeMode = QStringLiteral("auto");
    int m_thumbColumns = 4;
    int m_coverColumns = 3;
    bool m_musicPanel = true;
    double m_flickDeceleration = 4800; // px/s²: how quickly a flick glides to a stop
    double m_touchpadGain = 4.0;
    double m_wheelStep = 170;
    double m_volume = 1.0;
    bool m_muted = false;
    bool m_subtitlesOn = true;
    double m_subtitleOutline = 2.2;
    double m_subtitleShadow = 0;
    bool m_autoExpand = true;
    bool m_videoFollows = false;
    int m_untrackedCount = 0;
    QString m_replaceStage;
    qint64 m_replaceVideoId = 0;
};
