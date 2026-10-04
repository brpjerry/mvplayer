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

    // The user's verdict on a video that waits for review: it joins the
    // library, or its tracks have no video and it is deleted.
    void approveVideo(qint64 videoId);
    void rejectVideo(qint64 videoId);

    // Looks at every video in the library again with the account's cookies
    // and rebuilds those YouTube now offers in better quality.
    void checkQuality();
    bool checkingQuality() const { return m_upgrading; }
    int qualityChecked() const { return m_upgradeDone; }
    int qualityTotal() const { return m_upgradeTotal; }

    // Circuit breaker: when YouTube starts refusing requests the whole queue
    // waits instead of failing track after track.
    bool paused() const { return m_blocked; }
    QDateTime resumeAt() const { return m_resumeAt; }
    QString pauseReason() const { return m_pauseReason; }
    void resumeNow();

    bool scanning() const { return m_scanning; }
    int queuedCount() const { return m_queue.size(); }
    int activeCount() const { return m_active; }
    bool busy() const { return m_scanning || m_auditing || m_auditingAudio || m_upgrading || m_reviewJobs > 0 || m_blocked || m_active > 0 || !m_queue.isEmpty(); }

    static QString dataDir(const QString &mvDir);

signals:
    void jobChanged(const JobStatus &status);
    void videoAdded(qint64 videoId);
    void videoChanged(qint64 videoId);
    void videoRemoved(qint64 videoId);
    void activityChanged();
    void idle();

private:
    void enqueue(qint64 trackId);
    void pump();
    void onScanFinished(const LibraryScanner::Result &r);
    void onJobFinished(qint64 trackId);
    void runJob(qint64 trackId, const ImportSettings &cfg);
    void auditStills();
    void auditAudio(const ImportSettings &cfg);
    // Rebuilds a video's file with the track's audio as its main audio.
    bool putLibraryAudioIn(const VideoInfo &video, const TrackInfo &track, const AudioAlign::Result &align,
                           const ImportSettings &cfg, QString *error, bool review = false);
    void upgradeVideos(const ImportSettings &cfg);
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
    std::atomic<bool> m_cancel{false};

    QQueue<qint64> m_queue;
    QSet<qint64> m_pending; // queued or running
    int m_active = 0;
    bool m_scanning = false;
    bool m_auditing = false;
    std::atomic<bool> m_auditingAudio{false};
    bool m_audioAuditDue = false;
    std::atomic<int> m_reviewJobs{0};
    std::atomic<bool> m_upgrading{false};
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
