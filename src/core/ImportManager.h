#pragma once

#include "core/Database.h"
#include "core/LibraryScanner.h"

#include <QFileSystemWatcher>
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
    int retryNotFoundDays = 14;
};

struct JobStatus {
    qint64 trackId = 0;
    QString title;
    QString artist;
    QString stage;        // human readable
    double progress = -1; // 0..1, or -1 when indeterminate
    bool finished = false;
    QString outcome;      // done | not_found | failed | skipped (when finished)
    QString detail;
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

    bool scanning() const { return m_scanning; }
    int queuedCount() const { return m_queue.size(); }
    int activeCount() const { return m_active; }
    bool busy() const { return m_scanning || m_auditing || m_active > 0 || !m_queue.isEmpty(); }

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
    bool m_rescanWanted = false;
    bool m_started = false;

    QSet<QString> m_claimed; // YouTube ids being imported right now (guarded by m_mutex)

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_periodic;
};
