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
#include <memory>

struct ImportSettings {
    QStringList musicDirs;
    QString mvDir;
    bool replaceAudio = true;      // mux in library audio when it beats YouTube's
    bool allowUnofficial = false;  // accept uploads that do not look official
    bool skipStillImages = true;   // reject videos whose picture never changes
    int minSubscribers = 5000;     // ignore uploads from channels with fewer subscribers; 0: none are ignored
    int concurrency = 2;
    QStringList ytdlpArgs;         // extra yt-dlp arguments (cookies, proxies, ...)
    QString cookiesFile;           // cookies.txt of a YouTube Premium account; empty: none
    bool accountOnBotCheck = false; // sign in with it where YouTube refuses a guest with its bot check
    QStringList subtitleLangs;     // subtitles to fetch with a video ("en"); empty: none
    int retryNotFoundDays = 14;
    int pauseBaseSecs = 600;       // first wait after YouTube blocks requests; doubles on repeats
    // The judge (docs/import-rules-judge.html): "off", "advice" (a
    // recommendation on cards under review, X2) or "judge" (it runs the
    // lookup, Q–W); which model ("haiku", "sonnet", "opus"); where the
    // account's key is kept.
    QString judgeMode = QStringLiteral("off");
    QString judgeModel = QStringLiteral("sonnet");
    int judgeThreads = 1; // advice questions at once (X2): 1–8
    QString judgeKeyFile;
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

    // The user's verdict on a video that waits for review. Accepted, it joins
    // the library as the video of one of its candidate tracks: `trackId`
    // (any file of it), or the candidate shown first. Rejected, it is deleted
    // and not offered to any of its candidates again. Either way its other
    // candidates wait on what else they are candidates of, or have no video.
    // The verdict is carried out on a thread of its own: videoChanged or
    // videoRemoved says when.
    // `byJudge`: "Accept all" carrying out the judge's confident advice on
    // your behalf (import-rules-judge X4); logged as the judge's.
    void approveVideo(qint64 videoId, qint64 trackId = 0, bool byJudge = false);
    void rejectVideo(qint64 videoId, bool byJudge = false);
    // The last verdict can be taken back while what it deleted is set aside
    // in `.mvplayer/undo` (G10): the app's verdicts, not the headless
    // importer's, and not those a channel gives. One step: the next verdict
    // ends it, as does stopping. undoChanged says when there is one, or
    // none; undoLabel names what it would take back. Carried out on the
    // verdicts' thread: videoAdded and videoChanged say when.
    void setKeepsUndo(bool on) { m_keepsUndo = on; }
    bool undoAvailable() const;
    QString undoLabel() const;
    void undoVerdict();
    // A channel the user allows is an artist's channel for every track from
    // then on, and never ignored as a small one. Every video under review
    // from it is judged again from its file, with nothing asked of YouTube:
    // one that has become a candidate's own recording is accepted for that
    // track, the others stay under review.
    // A channel the user denies: every video the library has from it is
    // deleted (those under review as rejected for all their candidates; the
    // tracks that had the others are looked up again), and its uploads are
    // dropped on sight from then on.
    // Both are carried out on the verdicts' thread; channelListsChanged
    // says when the list itself has changed.
    void allowChannel(const QString &channelId, const QString &name, const QString &handle);
    void denyChannel(const QString &channelId, const QString &name, const QString &handle);
    // Takes a channel off either list. Nothing in the library changes.
    void unlistChannel(const QString &channelId);
    // What allowing a channel would do to each video under review from it,
    // found out by listening: channelJudged for each, then
    // channelPreviewDone. Returns a ticket that tells previews apart; a
    // newer preview, or cancelChannelPreview, ends an older one.
    int previewAllowChannel(const QString &channelId);
    void cancelChannelPreview();
    // The candidate tracks of a video under review in the order they are
    // shown: first the one the upload is titled after (and no other), then
    // by how close each one's waveform is to the upload's.
    static QVector<Database::Candidate> candidatesInOrder(Database *db, const VideoInfo &video);
    // Advice mode (X2): asks the judge whether the video under review is the
    // official video of its candidate `trackId`, from the video's own file
    // and what the library kept of the upload; nothing is fetched. Asked
    // once per video and candidate unless `again`; adviceChanged says when
    // the answer is in. Carried out on a thread of its own.
    void askJudge(qint64 videoId, qint64 trackId, bool again = false);
    // "Review all" (X2): asks about every video under review that has no
    // answer yet, for each of its candidates, in the queue's order.
    void askJudgeAll();
    // Questions queued for the judge and not yet answered (advice mode).
    int adviceLeft() const { return m_adviceLeft.load(); }

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
    // Verdicts and the like that are being carried out.
    bool settling() const { return m_reviewJobs > 0; }

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
    void channelListsChanged();
    // accept: the video would be (or was) accepted for the track named;
    // otherwise `reason` says what else it waits for.
    void channelJudged(int ticket, qint64 videoId, bool accept, qint64 trackId, const QString &track, const QString &reason);
    void channelPreviewDone(int ticket);
    void activityChanged();
    void idle();
    void undoChanged();
    // Judge mode: a lookup the judge settled by itself (W1, W3), or left
    // for your verdict (W2, W4).
    void judgeDecided(qint64 trackId, bool alone);
    // The judge's advice on a video under review changed (X2).
    void adviceChanged(qint64 videoId);
    void adviceQueueChanged(int left);

private:
    // What a verdict changed, as it was before: the video and the videos
    // deleted with it, their candidates, the import state of their tracks,
    // the uploads turned down, and the files it would have deleted (or
    // rewritten), set aside.
    struct Undo {
        QString verdict; // accept | reject
        QString label;
        bool changed = false; // something was written: worth keeping
        QVector<VideoInfo> videos;
        QHash<qint64, QVector<Database::CandidateRow>> candidates;
        QSet<qint64> trackKeys; // whose tracks are recorded
        QVector<Database::TrackState> tracks;
        QVector<QPair<TrackInfo, QString>> rejected; // track, YouTube id
        QHash<QString, QString> files;               // where it was, where it is set aside
    };
    QString undoDir() const;
    // The verdict being carried out keeps a record while it is undoable.
    void beginUndo(const QString &verdict, const QString &label);
    void endUndo();
    void recordVideo(const VideoInfo &v);  // the row, its candidates, its candidates' tracks
    bool recording() const;                // on the verdict's own thread
    // Sets a file aside instead of deleting it; a copy of one about to be rewritten.
    void setAside(const QString &path, bool copy);
    void dropUndo(std::unique_ptr<Undo> undo); // its set-aside files go
    void undoJob(const Undo &undo);
    void enqueue(qint64 trackId);
    // Deletes an untracked video with what is kept beside it.
    void removeUntracked(const VideoInfo &v);
    // Re-import: deletes the track's videos (its own, and those under review
    // it is a candidate of) whose file is empty or missing. True when there
    // were any.
    bool dropBrokenVideos(const TrackInfo &track);
    // Re-import: the track is no longer a candidate of the videos under
    // review that today's rules rule out for it by their length. Needs no
    // request. True when there were any.
    bool dropOutdatedOptions(const TrackInfo &track);
    // Re-import: a video that tracks of several recordings have, from before
    // a video had one track, stays with the one that is the closest fit to
    // its upload; the others have no video and are looked up. Needs no
    // request. True when the video was such a one.
    bool settleShared(qint64 videoId, const ImportSettings &cfg);
    // Deletes a video with what is kept beside it.
    void discardVideo(const VideoInfo &v);
    // The track is no longer a candidate of these videos under review; one
    // left without a candidate is deleted. Claims each video in turn: not
    // to be called with a claim held.
    void withdraw(const TrackInfo &track, const QVector<VideoInfo> &from);
    // A video has left the review queue, accepted for `keeper` or deleted:
    // the tracks that were its candidates wait on what else they are
    // candidates of; one with nothing left has no video.
    void releaseCandidates(qint64 videoId, const QVector<Database::Candidate> &were, const TrackInfo *keeper, const QString &why);
    // The track becomes one more candidate of a video under review; where it
    // is the better audio, its audio is put in the file beside the others'.
    // `align`: the track against the upload's own audio. Call with the
    // video claimed.
    bool addCandidate(const VideoInfo &video, const TrackInfo &track, const AudioAlign::Result &align,
                      const ImportSettings &cfg, QString *error);
    // Measures the candidates that have not been (those of a library from
    // before review went by video), and gives each its audio in the file.
    void prepareCandidates(const ImportSettings &cfg);
    bool measureCandidates(qint64 videoId, const ImportSettings &cfg);
    // `byChannel`: accepted because its channel was allowed, not by the
    // user's verdict on the video itself.
    // `undoable`: the user's own verdict in the app, kept so it can be taken back.
    void approveJob(qint64 videoId, qint64 trackId, const ImportSettings &cfg, bool byChannel = false, bool undoable = false, bool byJudge = false);
    void rejectJob(qint64 videoId, const QString &why = QString(), bool undoable = false, bool byJudge = false);
    // A video under review judged from its file with its channel taken as an
    // artist's: the candidate whose own recording it is (the closest fit of
    // several), or why it still waits.
    struct ChannelVerdict {
        bool accept = false;
        TrackInfo track;
        QString reason;
    };
    ChannelVerdict judgeAsArtists(const VideoInfo &video, const ImportSettings &cfg, const std::atomic<bool> *stop);
    void allowJob(const QString &channelId, const ImportSettings &cfg);
    void adviceJob(qint64 videoId, qint64 trackId, bool again, const ImportSettings &cfg);
    void denyJob(const QString &channelId);
    void requeuePending(); // any thread
    void pump();
    void onScanFinished(const LibraryScanner::Result &r);
    void onJobFinished(qint64 trackId);
    void runJob(qint64 trackId, const ImportSettings &cfg);
    // Rebuilds a video's file with the track's audio as its main audio.
    bool putLibraryAudioIn(const VideoInfo &video, const TrackInfo &track, const AudioAlign::Result &align,
                           const ImportSettings &cfg, QString *error, bool review = false);
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
    QThreadPool m_verdictPool;
    QThreadPool m_previewPool;
    QThreadPool m_judgePool; // advice mode's questions, one at a time
    std::atomic<int> m_adviceLeft{0};
    std::shared_ptr<std::atomic<bool>> m_previewStop;
    int m_previewTicket = 0;
    bool m_settledOnce = false; // what a library from before needs was asked for, this run
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

    bool m_keepsUndo = false;
    mutable QMutex m_undoMutex;
    std::unique_ptr<Undo> m_undo;      // the last verdict's (guarded by m_undoMutex)
    std::unique_ptr<Undo> m_recording; // the verdict being carried out, on its thread
    QThread *m_recordingThread = nullptr;

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
