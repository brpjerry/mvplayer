#include "core/ImportManager.h"

#include "core/AudioAlign.h"
#include "core/Matcher.h"
#include "core/Muxer.h"
#include "core/Util.h"
#include "core/YtDlp.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QThread>

namespace {

double trackQuality(const TrackInfo &t)
{
    if (t.lossless)
        return 1e6;
    double eff = 1.0;
    if (t.codec == QLatin1String("opus"))
        eff = 1.5;
    else if (t.codec == QLatin1String("aac") || t.codec == QLatin1String("vorbis"))
        eff = 1.2;
    return t.bitrate * eff;
}

double youtubeQuality(const QJsonObject &info)
{
    double abr = info.value(QLatin1String("abr")).toDouble();
    if (abr <= 0)
        abr = 130;
    const QString codec = info.value(QLatin1String("acodec")).toString();
    double eff = 1.0;
    if (codec.startsWith(QLatin1String("opus")))
        eff = 1.5;
    else if (codec.startsWith(QLatin1String("mp4a")))
        eff = 1.2;
    return abr * eff;
}

// Does the video's soundtrack contain this track?
bool audioMatches(const AudioAlign::Result &r)
{
    const double m = r.fpMatchedSec;
    if (m < 0.3 * r.videoSec)
        return false; // the song is only a small part of something longer
    return m >= 0.5 * r.trackSec || (m >= 0.8 * r.videoSec && m >= 45);
}

// Is the waveform match complete enough to swap the audio without audible seams?
bool audioReplaceable(const AudioAlign::Result &r)
{
    return !r.segments.isEmpty() && r.pcmMatchedSec >= 20 && r.pcmMatchedSec >= 0.85 * r.fpMatchedSec;
}

} // namespace

ImportManager::ImportManager(Database *db, QObject *parent)
    : QObject(parent)
    , m_db(db)
{
    qRegisterMetaType<JobStatus>();
    m_scanPool.setMaxThreadCount(1);
    m_jobPool.setMaxThreadCount(2);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(2500);
    connect(&m_debounce, &QTimer::timeout, this, &ImportManager::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });

    // Catches what directory watching cannot see, such as tags edited in place.
    m_periodic.setInterval(10 * 60 * 1000);
    connect(&m_periodic, &QTimer::timeout, this, &ImportManager::rescan);
}

ImportManager::~ImportManager()
{
    stop();
}

QString ImportManager::dataDir(const QString &mvDir)
{
    return QDir(mvDir).filePath(QStringLiteral(".mvplayer"));
}

void ImportManager::setSettings(const ImportSettings &s)
{
    QMutexLocker lock(&m_mutex);
    m_settings = s;
    m_jobPool.setMaxThreadCount(qBound(1, s.concurrency, 8));
}

ImportSettings ImportManager::settings() const
{
    QMutexLocker lock(&m_mutex);
    return m_settings;
}

void ImportManager::start()
{
    const ImportSettings cfg = settings();
    m_cancel = false;
    m_started = true;

    // Leftovers from a previous run that was interrupted.
    QDir(QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp"))).removeRecursively();

    m_db->requeueStale(30 * 60, qint64(cfg.retryNotFoundDays) * 86400);
    for (const TrackInfo &t : m_db->tracksInState({QStringLiteral("pending")}))
        enqueue(t.id);

    m_periodic.start();
    rescan();
    pump();
}

void ImportManager::stop()
{
    if (!m_started)
        return;
    m_started = false;
    m_cancel = true;
    m_debounce.stop();
    m_periodic.stop();
    m_queue.clear();
    m_scanPool.waitForDone();
    m_jobPool.waitForDone();
    m_pending.clear();
    m_active = 0;
    m_scanning = false;
}

void ImportManager::rescan()
{
    if (!m_started)
        return;
    if (m_scanning) {
        m_rescanWanted = true;
        return;
    }
    const QStringList roots = settings().musicDirs;
    m_scanning = true;
    emit activityChanged();
    m_scanPool.start([this, roots] {
        const LibraryScanner::Result r = LibraryScanner::scan(roots, *m_db, &m_cancel);
        QMetaObject::invokeMethod(this, [this, r] { onScanFinished(r); }, Qt::QueuedConnection);
    });
}

void ImportManager::retryUnmatched()
{
    m_db->resetTracks({QStringLiteral("not_found"), QStringLiteral("failed")});
    for (const TrackInfo &t : m_db->tracksInState({QStringLiteral("pending")}))
        enqueue(t.id);
    emit activityChanged();
    pump();
}

void ImportManager::onScanFinished(const LibraryScanner::Result &r)
{
    m_scanning = false;
    if (!m_started)
        return;
    if (r.ok) {
        if (r.added || r.changed || r.removed)
            qInfo("[scan] %d tracks: %d new, %d changed, %d removed", r.total, r.added, r.changed, r.removed);
        const QStringList watched = m_watcher.directories();
        const QSet<QString> want(r.directories.begin(), r.directories.end());
        const QSet<QString> have(watched.begin(), watched.end());
        const QStringList drop = (have - want).values();
        const QStringList add = (want - have).values();
        if (!drop.isEmpty())
            m_watcher.removePaths(drop);
        if (!add.isEmpty())
            m_watcher.addPaths(add);

        for (qint64 id : r.queued)
            enqueue(id);
        for (qint64 id : r.videosChanged)
            emit videoChanged(id);
    }
    if (r.unsettled)
        m_debounce.start();
    if (m_rescanWanted) {
        m_rescanWanted = false;
        m_debounce.start();
    }
    emit activityChanged();
    pump();
}

void ImportManager::enqueue(qint64 trackId)
{
    if (m_pending.contains(trackId))
        return;
    m_pending.insert(trackId);
    m_queue.enqueue(trackId);
}

void ImportManager::pump()
{
    if (!m_started)
        return;
    const ImportSettings cfg = settings();
    while (m_active < m_jobPool.maxThreadCount() && !m_queue.isEmpty()) {
        const qint64 id = m_queue.dequeue();
        ++m_active;
        m_jobPool.start([this, id, cfg] {
            runJob(id, cfg);
            QMetaObject::invokeMethod(this, [this, id] { onJobFinished(id); }, Qt::QueuedConnection);
        });
    }
    emit activityChanged();
    if (!busy() && !m_debounce.isActive())
        emit idle();
}

void ImportManager::onJobFinished(qint64 trackId)
{
    m_pending.remove(trackId);
    m_active = qMax(0, m_active - 1);
    pump();
}

bool ImportManager::claimVideo(const QString &ytId)
{
    // Two tracks can resolve to the same video (a single and its album cut);
    // the second waits for the first, then links to its result.
    for (;;) {
        {
            QMutexLocker lock(&m_mutex);
            if (!m_claimed.contains(ytId)) {
                m_claimed.insert(ytId);
                return true;
            }
        }
        if (m_cancel)
            return false;
        QThread::msleep(250);
    }
}

void ImportManager::releaseVideo(const QString &ytId)
{
    QMutexLocker lock(&m_mutex);
    m_claimed.remove(ytId);
}

void ImportManager::runJob(qint64 trackId, const ImportSettings &cfg)
{
    const std::optional<TrackInfo> maybe = m_db->track(trackId);
    if (!maybe || maybe->state != QLatin1String("pending"))
        return;
    const TrackInfo track = *maybe;

    JobStatus st;
    st.trackId = trackId;
    st.title = track.title;
    st.artist = splitMulti(track.albumArtist).value(0, track.artist);

    auto report = [&](const QString &stage, double progress = -1, const QString &detail = QString()) {
        st.stage = stage;
        st.progress = progress;
        st.detail = detail;
        emit jobChanged(st);
    };

    const QString workDir = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/job-%1").arg(trackId));
    auto cleanup = [&] { QDir(workDir).removeRecursively(); };

    auto finish = [&](const QString &outcome, qint64 videoId, const QString &message) {
        cleanup();
        if (m_cancel)
            return; // leave the track pending for the next run
        m_db->setTrackResult(trackId, outcome, videoId, message);
        st.finished = true;
        st.outcome = outcome;
        st.progress = -1;
        st.detail = message;
        st.stage = outcome == QLatin1String("done") ? QStringLiteral("Imported")
            : outcome == QLatin1String("not_found") ? QStringLiteral("No music video found")
            : outcome == QLatin1String("skipped") ? QStringLiteral("Skipped")
            : QStringLiteral("Failed");
        qInfo().noquote() << QStringLiteral("[import] %1 — %2: %3%4")
                                 .arg(st.artist, st.title, st.stage,
                                      message.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(message));
        emit jobChanged(st);
    };

    QString why;
    if (Matcher::isNonMvTrack(track, &why)) {
        finish(QStringLiteral("skipped"), 0, why);
        return;
    }

    // ---- 1. Search ---------------------------------------------------------
    report(QStringLiteral("Searching"));
    YtDlp yt(QStringLiteral("yt-dlp"), cfg.ytdlpArgs, &m_cancel);
    QVector<YtCandidate> candidates;
    QSet<QString> seen;
    bool searched = false;
    QString error;
    auto usable = [&](const YtCandidate &c) {
        return c.rejectReason.isEmpty() && (c.trusted || cfg.allowUnofficial);
    };
    for (const QString &query : Matcher::searchQueries(track)) {
        QVector<YtCandidate> found;
        if (!yt.search(query, 10, &found, &error)) {
            if (m_cancel)
                return cleanup();
            continue;
        }
        searched = true;
        for (YtCandidate &c : found) {
            if (seen.contains(c.id))
                continue;
            seen.insert(c.id);
            if (!candidates.isEmpty())
                c.rank += 2; // results of fallback queries rank slightly lower
            candidates.append(c);
        }
        Matcher::rank(track, candidates);
        if (std::any_of(candidates.begin(), candidates.end(), usable))
            break;
    }
    if (!searched) {
        finish(QStringLiteral("failed"), 0, QStringLiteral("search failed: %1").arg(error));
        return;
    }
    QVector<YtCandidate> shortlist;
    for (const YtCandidate &c : std::as_const(candidates)) {
        if (usable(c) && shortlist.size() < 4)
            shortlist.append(c);
    }
    if (shortlist.isEmpty()) {
        finish(QStringLiteral("not_found"), 0,
               QStringLiteral("none of %1 search results looked like an official video").arg(candidates.size()));
        return;
    }

    // ---- 2. Verify by listening -------------------------------------------
    std::vector<int16_t> trackPcm;
    if (!AudioAlign::decodeMono(track.path, &trackPcm, &m_cancel, &error)) {
        if (m_cancel)
            return cleanup();
        finish(QStringLiteral("failed"), 0, QStringLiteral("cannot decode track: %1").arg(error));
        return;
    }

    YtCandidate chosen;
    QString chosenDir, ytAudio;
    QJsonObject ytInfo;
    AudioAlign::Result alignment;
    QStringList reasons;
    bool anyChecked = false;

    for (const YtCandidate &c : std::as_const(shortlist)) {
        if (m_cancel)
            return cleanup();
        if (!claimVideo(c.id))
            return cleanup();
        bool keepClaim = false;
        auto unclaim = qScopeGuard([&] {
            if (!keepClaim)
                releaseVideo(c.id);
        });

        if (const auto existing = m_db->videoByYtId(c.id)) {
            // Already in the MV library through another track (a single and
            // its album cut, say). It still has to be this recording.
            std::vector<int16_t> pcm;
            if (QFile::exists(existing->path) && AudioAlign::decodeMono(existing->path, &pcm, &m_cancel, &error)) {
                anyChecked = true;
                const AudioAlign::Result ar = AudioAlign::align(trackPcm, pcm);
                if (audioMatches(ar)) {
                    finish(QStringLiteral("done"), existing->id,
                           QStringLiteral("shares the video of “%1”").arg(existing->title));
                    return;
                }
            }
            if (m_cancel)
                return cleanup();
            reasons << QStringLiteral("“%1” is a different recording").arg(c.title);
            continue;
        }

        const QString dir = QDir(workDir).filePath(c.id);
        QDir().mkpath(dir);
        report(QStringLiteral("Checking audio"), -1, c.title);

        QString audioFile;
        QJsonObject info;
        if (!yt.downloadAudio(c.id, dir, &audioFile, &info, &error)) {
            if (m_cancel)
                return cleanup();
            reasons << QStringLiteral("%1: %2").arg(c.id, error);
            continue;
        }
        std::vector<int16_t> mvPcm;
        if (!AudioAlign::decodeMono(audioFile, &mvPcm, &m_cancel, &error)) {
            if (m_cancel)
                return cleanup();
            reasons << QStringLiteral("%1: %2").arg(c.id, error);
            continue;
        }
        anyChecked = true;
        const AudioAlign::Result ar = AudioAlign::align(trackPcm, mvPcm);
        qInfo().noquote() << QStringLiteral("[align] %1 ~ “%2” [%3]: %4").arg(track.title, c.title, c.id, ar.summary());
        if (!audioMatches(ar)) {
            reasons << QStringLiteral("“%1” is a different recording").arg(c.title);
            QDir(dir).removeRecursively();
            continue;
        }

        if (cfg.skipStillImages) {
            report(QStringLiteral("Checking video"), -1, c.title);
            QString preview;
            if (yt.downloadPreview(c.id, dir, &preview, &error)) {
                const bool still = Muxer::isStaticVideo(preview, &m_cancel);
                QFile::remove(preview);
                if (still) {
                    reasons << QStringLiteral("“%1” is a still image").arg(c.title);
                    QDir(dir).removeRecursively();
                    continue;
                }
            } else if (m_cancel) {
                return cleanup();
            }
        }

        chosen = c;
        chosenDir = dir;
        ytAudio = audioFile;
        ytInfo = info;
        alignment = ar;
        keepClaim = true;
        break;
    }

    if (chosen.id.isEmpty()) {
        if (!anyChecked)
            finish(QStringLiteral("failed"), 0, reasons.join(QStringLiteral("; ")));
        else
            finish(QStringLiteral("not_found"), 0, reasons.join(QStringLiteral("; ")));
        return;
    }
    auto releaseChosen = qScopeGuard([&] { releaseVideo(chosen.id); });

    // ---- 3. Download -------------------------------------------------------
    report(QStringLiteral("Downloading"), 0, chosen.title);
    QString videoFile, thumbFile;
    double lastShown = -1;
    const bool got = yt.downloadVideo(chosen.id, chosenDir, [&](double p) {
        if (p - lastShown >= 0.01 || p >= 1.0) {
            lastShown = p;
            report(QStringLiteral("Downloading"), p, chosen.title);
        }
    }, &videoFile, &thumbFile, &error);
    if (!got) {
        if (m_cancel)
            return cleanup();
        finish(QStringLiteral("failed"), 0, QStringLiteral("download failed: %1").arg(error));
        return;
    }

    // ---- 4. Audio + mux ----------------------------------------------------
    const QString artistDir = sanitizeFileName(splitMulti(track.albumArtist).value(0, QStringLiteral("Unknown Artist")));
    const QString baseName = QStringLiteral("%1 [%2]").arg(sanitizeFileName(track.title), chosen.id);
    const QString outDir = QDir(cfg.mvDir).filePath(artistDir);
    const QString outFile = QDir(outDir).filePath(baseName + QStringLiteral(".mkv"));
    const QString outThumb = QDir(outDir).filePath(baseName + QStringLiteral(".jpg"));

    Muxer::Plan plan;
    plan.videoFile = videoFile;
    plan.ytAudioFile = ytAudio;
    plan.workDir = chosenDir;
    plan.outFile = outFile;
    plan.track = track;
    plan.ytId = chosen.id;
    plan.align = alignment;
    plan.replaceAudio = cfg.replaceAudio && audioReplaceable(alignment)
        && trackQuality(track) > youtubeQuality(ytInfo) * 1.15;

    report(plan.replaceAudio ? QStringLiteral("Muxing library audio") : QStringLiteral("Muxing"), -1, chosen.title);
    QString audioDetail;
    bool muxed = Muxer::mux(plan, &m_cancel, &audioDetail, &error);
    if (!muxed && !m_cancel && plan.replaceAudio) {
        qWarning().noquote() << "[import] audio replacement failed, keeping YouTube audio:" << error;
        plan.replaceAudio = false;
        muxed = Muxer::mux(plan, &m_cancel, &audioDetail, &error);
    }
    if (!muxed) {
        if (m_cancel)
            return cleanup();
        finish(QStringLiteral("failed"), 0, error);
        return;
    }

    Muxer::Probe pr;
    Muxer::probe(outFile, &pr);

    QFile::remove(outThumb);
    if (thumbFile.isEmpty() || !QFile::rename(thumbFile, outThumb)) {
        // No YouTube thumbnail: grab a frame instead.
        ProcOptions o;
        o.timeoutMs = 60000;
        runProcess(QStringLiteral("ffmpeg"),
                   {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
                    QStringLiteral("-ss"), QString::number(pr.duration * 0.25, 'f', 2), QStringLiteral("-i"), outFile,
                    QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-vf"),
                    QStringLiteral("scale=1280:-2"), outThumb}, o);
    }

    VideoInfo v;
    v.ytId = chosen.id;
    v.path = outFile;
    v.thumb = QFile::exists(outThumb) ? outThumb : QString();
    v.title = track.title;
    v.artist = track.artist;
    v.albumArtist = track.albumArtist;
    v.album = track.album;
    v.genre = track.genre;
    v.year = track.year;
    v.trackNo = track.trackNo;
    v.duration = pr.duration;
    v.width = pr.width;
    v.height = pr.height;
    v.fps = pr.fps;
    v.vcodec = pr.vcodec;
    v.audioSource = plan.replaceAudio ? QStringLiteral("library") : QStringLiteral("youtube");
    v.audioDetail = audioDetail;
    v.ytTitle = chosen.title;
    v.ytChannel = chosen.channel;
    v.tags = track.tags;
    v.addedAt = QDateTime::currentSecsSinceEpoch();

    if (m_cancel) {
        QFile::remove(outFile);
        QFile::remove(outThumb);
        return cleanup();
    }
    const qint64 videoId = m_db->insertVideo(v);
    if (videoId <= 0) {
        QFile::remove(outFile);
        QFile::remove(outThumb);
        finish(QStringLiteral("failed"), 0, QStringLiteral("could not record the video in the library"));
        return;
    }
    finish(QStringLiteral("done"), videoId,
           QStringLiteral("%1 · %2×%3 · %4").arg(chosen.title).arg(pr.width).arg(pr.height).arg(audioDetail));
    emit videoAdded(videoId);
}
