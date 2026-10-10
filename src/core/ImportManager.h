#pragma once

#include "core/AudioAlign.h"
#include "core/Database.h"
#include "core/LibraryScanner.h"

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QVariant>

#include <atomic>

struct ImportSettings {
    QStringList musicDirs;
    QString mvDir;
    bool replaceAudio = true;      // mux in library audio when it beats YouTube's
    bool allowUnofficial = false;  // accept uploads that do not look official
    bool skipStillImages = true;   // reject videos whose picture never changes
    int concurrency = 2;
    QStringList ytdlpArgs;         // extra yt-dlp arguments (cookies, proxies, ...)
    QString cookiesFile;           // cookies.txt of a YouTube Premium account; empty: none
    bool accountOnBotCheck = false; // sign in with it where YouTube refuses a guest with its bot check
    QStringList subtitleLangs;     // subtitles to fetch with a video ("en"); empty: none
    int retryNotFoundDays = 14;
    int pauseBaseSecs = 600;       // first wait after YouTube blocks requests; doubles on repeats
};

struct JobStatus {
    qint64 trackId = 0;
    QString title;
    QString artist;
    QString stage;        // human readable
    double progress = -1; // 0..1, or -1 when indeterminate
    bool finished = false;
    QString outcome;      // done | not_found | failed | skipped | postponed (when finished)
    QString detail;
    bool upgrade = false; // part of a quality check of existing videos, not an import
};
Q_DECLARE_METATYPE(JobStatus)

// Keeps the MV library in step with the audio library: scans and watches the
// music folder and runs the find / verify / download / mux pipeline for every
// track that does not have a video yet.
class ImportManager : public QObject
{
    Q_OBJECT
public:
    explicit ImportManager(Database *db, QObject *parent = nullptr);
    ~ImportManager() override;

    void setSettings(const ImportSettings &s);
    ImportSettings settings() const;

    void start();
    void stop(); // cancels running work and waits for it
    void rescan();
    void retryUnmatched();
    // Looks these tracks up again as if for the first time, the video each
    // has judged by today's rules as one candidate among the search results:
    // kept where it still fits outright (and the download skipped), sent to
    // review where it no longer does, replaced where another fits better,
    // let go where the rules turn it down. Tracks without a video are simply
    // looked up. Returns how many recordings were queued.
    int reimport(const QVector<qint64> &trackIds);
    int reimportVideo(qint64 videoId);
    // A file of the music folders, or a folder: every track under it.
    int reimportPath(const QString &path);

    // Replacing a video by hand. replaceSearch looks for uploads that are
    // the track by ear — any channel, not only the artist's own — and
    // answers with up to three (replaceOptions). replaceCheck listens to one
    // upload the user named (replaceChecked). replaceWith brings that upload
    // in as the track's video, accepted, in place of the one it has.
    // One of these runs at a time (replacing()): false when another does.
    bool replaceSearch(qint64 videoId);
    bool replaceCheck(qint64 videoId, const QString &ytId);
    bool replaceWith(qint64 videoId, const QString &ytId);
    bool replacing() const { return m_replacing; }

    // The user's verdict on a video that waits for review: it joins the
    // library, or its tracks have no video and it is deleted.
    void approveVideo(qint64 videoId);
    void rejectVideo(qint64 videoId);

    // Looks at every video in the library again with the account's cookies
    // and rebuilds those YouTube now offers in better quality.
    void checkQuality();
    bool checkingQuality() const { return m_upgrading && !m_subtitling; }
    int qualityChecked() const { return m_upgradeDone; }
    int qualityTotal() const { return m_upgradeTotal; }
    // Fetches the subtitles that the videos already imported lack. Shares the
    // quality check's progress: one pass over the library at a time.
    // Videos that no track in the music folders has (see
    // Database::untrackedVideos), and their deletion: files, subtitles and
    // what was remembered of their tracks. Returns how many were deleted.
    QSet<qint64> untrackedIds() const;
    int deleteUntracked();
    // The user's verdict on one of them, outside review: it stays in the
    // library for good, or it is deleted. Deleting waits for the folders to
    // be read (false until then): that settles whether its tracks are gone.
    void keepUntracked(qint64 videoId);
    bool deleteUntracked(qint64 videoId);
    void fetchSubtitles();
    bool fetchingSubtitles() const { return m_upgrading && m_subtitling; }

    // Circuit breaker: when YouTube starts refusing requests the whole queue
    // waits instead of failing track after track.
    bool paused() const { return m_blocked; }
    QDateTime resumeAt() const { return m_resumeAt; }
    QString pauseReason() const { return m_pauseReason; }
    void resumeNow();

    bool scanning() const { return m_scanning; }
    int queuedCount() const { return m_queue.size(); }
    int activeCount() const { return m_active; }
    bool busy() const { return m_scanning || m_upgrading || m_reviewJobs > 0 || m_blocked || m_active > 0 || !m_queue.isEmpty() || m_replacing; }

    static QString dataDir(const QString &mvDir);
    // What is remembered about candidate videos between tracks.
    static QString cacheDir(const QString &mvDir);

signals:
    void jobChanged(const JobStatus &status);
    // What a replace job is doing (empty when none runs); its answers.
    // Each option: {id, title, channel, duration, thumbnail, waveform,
    // fingerprint, artistChannel, summary}. The check: {id, title, channel,
    // matches, reason}.
    void replaceStageChanged(qint64 videoId, const QString &stage);
    void replaceOptions(qint64 videoId, const QVariantList &options, const QString &error);
    void replaceChecked(qint64 videoId, const QVariantMap &result);
    void videoAdded(qint64 videoId);
    void videoChanged(qint64 videoId);
    void videoRemoved(qint64 videoId);
    void activityChanged();
    void idle();

private:
    void enqueue(qint64 trackId);
    // Deletes an untracked video with what is kept beside it.
    void removeUntracked(const VideoInfo &v);
    // Re-import: deletes the track's videos (its own, or the options it has
    // waiting) whose file is empty or missing. True when there were any.
    bool dropBrokenVideos(const TrackInfo &track);
    // Re-import: deletes the options the track has waiting for review that
    // today's rules rule out by their length for every track waiting on
    // them. Needs no request. True when there were any.
    bool dropOutdatedOptions(const TrackInfo &track);
    void pump();
    void onScanFinished(const LibraryScanner::Result &r);
    void onJobFinished(qint64 trackId);
    void runJob(qint64 trackId, const ImportSettings &cfg);
    // Rebuilds a video's file with the track's audio as its main audio.
    bool putLibraryAudioIn(const VideoInfo &video, const TrackInfo &track, const AudioAlign::Result &align,
                           const ImportSettings &cfg, QString *error, bool review = false,
                           int ytAudioStream = 0);
    void upgradeVideos(const ImportSettings &cfg);
    void subtitleVideos(const ImportSettings &cfg);
    // "done" when the video was rebuilt, "skipped" when it is as good as it gets.
    QString upgradeVideo(const VideoInfo &video, const ImportSettings &cfg, QString *detail);
    void requeueRetryable();
    void noteSuccess();                      // any thread
    bool noteFailure(const QString &error);  // any thread; true when the queue is (now) paused
    void tripBreaker(const QString &reason);
    void appendLog(const QJsonObject &entry); // any thread
    bool claimVideo(const QString &ytId);
    void releaseVideo(const QString &ytId);

    Database *m_db;
    mutable QMutex m_mutex;
    ImportSettings m_settings;

    QThreadPool m_jobPool;
    QThreadPool m_scanPool;
    QThreadPool m_auditPool;
    QThreadPool m_replacePool;
    std::atomic<bool> m_replacing{false};
    // The track a replace job works for: the best file of the video's.
    std::optional<TrackInfo> replaceTrack(const VideoInfo &video);
    void replaceSearchJob(const VideoInfo &video, const TrackInfo &track, const ImportSettings &cfg);
    void replaceCheckJob(const VideoInfo &video, const TrackInfo &track, const QString &ytId, const ImportSettings &cfg);
    void replaceWithJob(const VideoInfo &video, const TrackInfo &track, const QString &ytId, const ImportSettings &cfg);
    std::atomic<bool> m_cancel{false};

    QQueue<qint64> m_queue;
    QSet<qint64> m_pending; // queued or running
    int m_active = 0;
    bool m_scanning = false;
    std::atomic<int> m_reviewJobs{0};
    std::atomic<bool> m_upgrading{false};
    std::atomic<bool> m_subtitling{false}; // the pass that is running fetches subtitles
    std::atomic<int> m_upgradeDone{0};
    std::atomic<int> m_upgradeTotal{0};
    bool m_rescanWanted = false;
    bool m_started = false;

    QSet<QString> m_claimed; // YouTube ids being imported right now (guarded by m_mutex)

    std::atomic<bool> m_blocked{false};
    std::atomic<int> m_failStreak{0}; // network failures in a row, across jobs
    std::atomic<int> m_tripCount{0};  // pauses in a row without a success in between
    std::atomic<bool> m_probing{false}; // just resumed: one job at a time until a request succeeds
    QTimer m_resumeTimer;
    QDateTime m_resumeAt;
    QString m_pauseReason;
    QMutex m_logMutex;

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_periodic;
};
