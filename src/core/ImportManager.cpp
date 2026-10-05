#include "core/ImportManager.h"

#include "core/TalkCheck.h"

#include "core/Subtitles.h"

#include "core/AudioAlign.h"
#include "core/Matcher.h"
#include "core/Muxer.h"
#include "core/Util.h"
#include "core/YtDlp.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDirIterator>
#include <QFile>
#include <QThread>

#include <functional>

namespace {

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
    // The song is only part of something longer. That includes a short edit
    // of a song (the cut used as a show's opening, say) against the video of
    // the full version: it only gets a video of about its own length.
    if (m < 0.5 * r.videoSec)
        return false;
    return m >= 0.5 * r.trackSec || (m >= 0.8 * r.videoSec && m >= 45);
}

constexpr int kFailStreakLimit = 8;         // consecutive network failures that also trip the breaker
constexpr int kMaxPauseSecs = 2 * 60 * 60;
constexpr qint64 kLogRotateBytes = 8 * 1024 * 1024;

// Why a video's soundtrack does not count as containing the track.
QString mismatchReason(const AudioAlign::Result &r)
{
    const double m = r.fpMatchedSec;
    if (m < 0.5 * r.videoSec && m < 0.5 * r.trackSec)
        return QStringLiteral("not this song: fingerprints match %1 s of a %2 s track").arg(qRound(m)).arg(qRound(r.trackSec));
    if (m < 0.5 * r.videoSec)
        return QStringLiteral("the track is only part of this video: %1 s of %2 s").arg(qRound(m)).arg(qRound(r.videoSec));
    return QStringLiteral("only part of the track is in this video: fingerprints match %1 s of %2 s").arg(qRound(m)).arg(qRound(r.trackSec));
}

// A video leaves the library with what is kept beside it.
void removeVideoFiles(const VideoInfo &v)
{
    for (const QString &sub : Subtitles::sidecars(v.path))
        QFile::remove(sub);
    QFile::remove(v.path);
    if (!v.thumb.isEmpty())
        QFile::remove(v.thumb);
}

// Fetches the subtitles a video lacks, of the languages wanted, and puts them
// beside it. Returns how many were written; -1 when YouTube could not be asked.
int fetchSubtitlesFor(YtDlp &yt, const QString &ytId, const QString &videoPath, const QStringList &languages,
                      const QString &workDir, QString *error)
{
    QStringList missing;
    for (const QString &l : languages) {
        if (!Subtitles::hasSidecar(videoPath, l))
            missing << l;
    }
    if (missing.isEmpty())
        return 0;
    const QString dir = QDir(workDir).filePath(QStringLiteral("subs"));
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const auto cleanup = qScopeGuard([&] { QDir(dir).removeRecursively(); });
    QHash<QString, QString> files;
    if (!yt.downloadSubtitles(ytId, dir, missing, &files, error))
        return -1;
    int written = 0;
    for (auto it = files.begin(); it != files.end(); ++it) {
        QString why;
        if (!Subtitles::convertSrv3(it.value(), Subtitles::sidecarBase(videoPath, it.key()), &why).isEmpty())
            ++written;
        else if (!why.isEmpty())
            qWarning().noquote() << "[subtitles]" << ytId << it.key() << why;
    }
    return written;
}

// The measurements a decision about a candidate rests on, for the import log.
QJsonObject measurements(const AudioAlign::Result &r)
{
    const double whole = std::min(r.trackSec, r.videoSec);
    const auto round = [](double v, int digits) { const double k = std::pow(10.0, digits); return std::round(v * k) / k; };
    return {
        {QStringLiteral("trackSec"), round(r.trackSec, 1)},
        {QStringLiteral("videoSec"), round(r.videoSec, 1)},
        // fingerprints: how much of the two is recognisably the same song
        {QStringLiteral("fingerprintSec"), round(r.fpMatchedSec, 1)},
        {QStringLiteral("fingerprintCoverage"), whole > 0 ? round(r.fpMatchedSec / whole, 3) : 0},
        // waveform: how much is demonstrably the same recording
        {QStringLiteral("sameWaveformSec"), round(r.goodSec, 1)},
        {QStringLiteral("sameWaveform"), whole > 0 ? round(r.goodSec / whole, 3) : 0},
        {QStringLiteral("segments"), r.segments.size()},
        {QStringLiteral("segmentSec"), round(r.pcmMatchedSec, 1)},
        // loudness: where the two rise and fall together, and how well
        {QStringLiteral("loudnessOffsetSec"), round(double(r.contourLag) / AudioAlign::kRate, 3)},
        {QStringLiteral("loudnessCorrelation"), round(r.contourCorr, 3)},
        {QStringLiteral("gainDb"), r.segments.isEmpty() ? 0 : round(20 * std::log10(r.gain), 2)},
        {QStringLiteral("polarityInverted"), r.inverted},
        {QStringLiteral("placedByOffset"), r.byOffset},
        {QStringLiteral("interruptedSec"), round(r.interruptedSec(), 1)},
    };
}

// Is this the track's own recording, so that its audio can take the video's
// place? Most of the track (or of a shorter video) has to be demonstrably
// the same waveform. Measured on a real library: another master of the
// recording (quieter, re-equalised, drifting, inverted) shows 77% and more;
// another singer over the same backing 67%, a remix over the same vocal 59%,
// live takes and covers 25–51%.
constexpr double kOwnWaveform = 0.72;

bool audioReplaceable(const AudioAlign::Result &r)
{
    if (r.byOffset)
        return true;
    const double whole = std::min(r.trackSec, r.videoSec);
    return !r.segments.isEmpty() && r.goodSec >= kOwnWaveform * whole && r.goodSec >= 0.6 * r.fpMatchedSec;
}

// Where track and video name different versions ("prompt αU ver." against
// none), the waveform has to overrule the names: a version made over the
// original keeps its backing and some of its lines, and showed 90%; the
// same recording under another label 99.8% and more.
constexpr double kOwnWaveformOtherVersion = 0.97;

// A video that stops the song to carry on with it later is not the song's
// video as it stands, however much of the waveform is the track's: a
// reaction video had all of a track, in 19 pieces with talk in between.
// (An interlude cut into a real music video looks the same; the user says.)
constexpr double kMaxInterruptedSec = 15;

bool interrupted(const AudioAlign::Result &r)
{
    return r.interruptedSec() > kMaxInterruptedSec;
}

QString interruptedReason(const AudioAlign::Result &r)
{
    int places = 0;
    const double other = r.interruptedSec(&places);
    return QStringLiteral("the song is interrupted in this video: %1 s of other audio inside it, in %2 %3")
        .arg(qRound(other)).arg(places).arg(places == 1 ? QStringLiteral("place") : QStringLiteral("places"));
}

bool ownRecording(const AudioAlign::Result &r, const TrackInfo &track, const QString &videoTitle)
{
    if (!audioReplaceable(r) || interrupted(r))
        return false;
    return r.byOffset || Matcher::sameVersion(track, videoTitle)
        || r.goodSec >= kOwnWaveformOtherVersion * std::min(r.trackSec, r.videoSec);
}

// What a candidate that is not taken as the track's recording lacks.
QString unconfirmedReason(const AudioAlign::Result &r, const TrackInfo &track, const QString &videoTitle)
{
    const int share = qRound(100 * r.goodSec / qMax(1.0, std::min(r.trackSec, r.videoSec)));
    if (audioReplaceable(r) && interrupted(r))
        return QStringLiteral("%1% of it is demonstrably the track's waveform, but %2").arg(share).arg(interruptedReason(r));
    if (audioReplaceable(r)) {
        return QStringLiteral("%1% of it is demonstrably the track's waveform, but track and video are marked as different versions (%2% needed then)")
            .arg(share)
            .arg(qRound(100 * kOwnWaveformOtherVersion));
    }
    return QStringLiteral("only %1% of it is demonstrably the track's waveform (%2% needed)%3")
        .arg(share)
        .arg(qRound(100 * kOwnWaveform))
        .arg(Matcher::sameVersion(track, videoTitle) ? QString() : QStringLiteral("; track and video are marked as different versions"));
}

// The whole track at the one offset at which the loudness of the two rises
// and falls together, exact to the sample where a waveform segment sits there.
// A weak waveform match on its own can sit a beat off.
AudioAlign::Result placedWhole(AudioAlign::Result r, bool *exact = nullptr)
{
    AudioAlign::Segment main;
    main.lag = r.contourLag;
    bool found = false;
    for (const AudioAlign::Segment &s : std::as_const(r.segments)) {
        if (std::llabs(s.lag - r.contourLag) <= AudioAlign::kRate / 50
            && (!found || s.mvEnd - s.mvStart > main.mvEnd - main.mvStart)) {
            main = s;
            found = true;
        }
    }
    if (exact)
        *exact = found;
    const qint64 trackSamples = std::llround(r.trackSec * AudioAlign::kRate);
    const qint64 videoSamples = std::llround(r.videoSec * AudioAlign::kRate);
    main.mvStart = std::max<qint64>(main.lag, 0);
    main.mvEnd = std::min(trackSamples + main.lag, videoSamples);
    if (main.mvEnd - main.mvStart < AudioAlign::kRate)
        return r;
    r.segments = {main};
    r.pcmMatchedSec = double(main.mvEnd - main.mvStart) / AudioAlign::kRate;
    r.byOffset = true;
    return r;
}

// The same performance in another mix — reverb added, the voice at another
// level — has the waveform of the track only here and there, like a cover
// over the same backing. The waveforms cannot tell those apart; what the two
// are called can. So when the fingerprints cover the song, the waveforms
// agree in places at the offset the loudness gives, and neither side is
// marked as a version the other is not (live, remix, cover, ...), it is taken
// for the track's recording, and the track is placed whole at that offset.
AudioAlign::Result fitted(AudioAlign::Result r, const TrackInfo &track, const QString &videoTitle)
{
    if (r.segments.isEmpty() || audioReplaceable(r))
        return r;
    const double whole = std::min(r.trackSec, r.videoSec);
    if (r.fpMatchedSec < 0.8 * whole || r.goodSec < 0.2 * whole || r.contourCorr < 0.5
        || !Matcher::sameVersion(track, videoTitle))
        return r;
    bool exact = false;
    const AudioAlign::Result placed = placedWhole(r, &exact);
    return exact ? placed : r;
}

// The library's file is the better audio: it is what the video must play.
// YouTube's is only ever the main audio when the file is the lesser one.
bool libraryIsBetter(const TrackInfo &track, double youtubeQuality)
{
    return trackQuality(track) > youtubeQuality * 1.15;
}

// Quality of the YouTube audio an existing video was built from (Opus).
double storedYoutubeQuality(const VideoInfo &v)
{
    return (v.ytAbr > 0 ? v.ytAbr : 130) * 1.5;
}

} // namespace

ImportManager::ImportManager(Database *db, QObject *parent)
    : QObject(parent)
    , m_db(db)
{
    qRegisterMetaType<JobStatus>();
    m_scanPool.setMaxThreadCount(1);
    m_auditPool.setMaxThreadCount(1);
    m_jobPool.setMaxThreadCount(2);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(2500);
    connect(&m_debounce, &QTimer::timeout, this, &ImportManager::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });

    m_resumeTimer.setSingleShot(true);
    connect(&m_resumeTimer, &QTimer::timeout, this, &ImportManager::resumeNow);

    // Catches what directory watching cannot see, such as tags edited in place.
    m_periodic.setInterval(10 * 60 * 1000);
    connect(&m_periodic, &QTimer::timeout, this, [this] {
        requeueRetryable();
        rescan();
    });
}

ImportManager::~ImportManager()
{
    stop();
}

QString ImportManager::cacheDir(const QString &mvDir)
{
    return QDir(dataDir(mvDir)).filePath(QStringLiteral("cache"));
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

    // Leftovers from a previous run that was interrupted: scratch files, and
    // half-written outputs if the process was killed outright mid-mux.
    QDir(QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp"))).removeRecursively();
    // What was remembered about candidates: not for ever, and not without bound.
    YtDlp::pruneCache(cacheDir(cfg.mvDir), 2LL * 1024 * 1024 * 1024, 14);

    QDirIterator partials(cfg.mvDir, {QStringLiteral("*.mkv.part.mkv")}, QDir::Files, QDirIterator::Subdirectories);
    while (partials.hasNext())
        QFile::remove(partials.next());

    requeueRetryable();

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
    m_resumeTimer.stop();
    m_blocked = false;
    m_queue.clear();
    m_scanPool.waitForDone();
    m_auditPool.waitForDone();
    m_jobPool.waitForDone();
    m_pending.clear();
    m_active = 0;
    m_scanning = false;
    m_upgrading = false;
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

void ImportManager::requeueRetryable()
{
    // Failed lookups come back after 30 minutes, doubling each time; "no
    // video" verdicts after a couple of weeks, in case one has been published.
    m_db->requeueStale(30 * 60, qint64(settings().retryNotFoundDays) * 86400);
    for (const TrackInfo &t : m_db->pendingRecordings())
        enqueue(t.id);
    // The caller starts the work: reporting "idle" from here would end a
    // headless run before its first scan.
}

void ImportManager::noteSuccess()
{
    m_failStreak = 0;
    m_tripCount = 0;
    m_probing = false;
}

bool ImportManager::noteFailure(const QString &error)
{
    const bool refused = YtDlp::looksBlocked(error);
    const int streak = ++m_failStreak;
    if (!refused && streak < kFailStreakLimit)
        return m_blocked;
    if (!m_blocked.exchange(true)) {
        const QString reason = refused ? QStringLiteral("YouTube is limiting requests (%1)").arg(error)
                                       : QStringLiteral("%1 downloads in a row failed (last: %2)").arg(streak).arg(error);
        QMetaObject::invokeMethod(this, [this, reason] { tripBreaker(reason); }, Qt::QueuedConnection);
    }
    return true;
}

void ImportManager::tripBreaker(const QString &reason)
{
    if (!m_started)
        return;
    const int trips = ++m_tripCount;
    const int base = qMax(1, settings().pauseBaseSecs);
    const int wait = int(qMin<qint64>(qint64(base) << qMin(trips - 1, 10), qMax(base, kMaxPauseSecs)));
    m_pauseReason = reason;
    m_resumeAt = QDateTime::currentDateTime().addSecs(wait);
    m_resumeTimer.start(wait * 1000);
    qInfo().noquote() << QStringLiteral("[import] paused for %1: %2").arg(formatDuration(wait), reason);
    appendLog({{QStringLiteral("event"), QStringLiteral("paused")}, {QStringLiteral("seconds"), wait},
               {QStringLiteral("reason"), reason}});
    emit activityChanged();
}

void ImportManager::resumeNow()
{
    if (!m_started || !m_blocked)
        return;
    m_resumeTimer.stop();
    m_blocked = false;
    m_failStreak = 0;
    m_probing = true;
    qInfo("[import] resuming");
    appendLog({{QStringLiteral("event"), QStringLiteral("resumed")}});
    // Tracks set aside while paused are still pending in the database.
    for (const TrackInfo &t : m_db->pendingRecordings())
        enqueue(t.id);
    emit activityChanged();
    pump();
}

void ImportManager::appendLog(const QJsonObject &entry)
{
    // One JSON object per line: what was searched, which candidates were
    // examined and why each was accepted or dropped, and how many requests
    // it took. Meant for tuning the matching rules against real libraries.
    QJsonObject line = entry;
    line.insert(QStringLiteral("t"), QDateTime::currentDateTime().toString(Qt::ISODate));
    const QString path = QDir(dataDir(settings().mvDir)).filePath(QStringLiteral("import-log.jsonl"));
    QMutexLocker lock(&m_logMutex);
    if (QFileInfo(path).size() > kLogRotateBytes) {
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append))
        f.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
}

bool ImportManager::putLibraryAudioIn(const VideoInfo &video, const TrackInfo &track, const AudioAlign::Result &align,
                                      const ImportSettings &cfg, QString *error, bool review, int ytAudioStream)
{
    const QString workDir = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/audio-%1").arg(video.id));
    QDir(workDir).removeRecursively();
    QDir().mkpath(workDir);
    const auto cleanup = qScopeGuard([&] { QDir(workDir).removeRecursively(); });

    // The picture and YouTube's audio come from the file as it is.
    Muxer::Plan plan;
    plan.videoFile = video.path;
    plan.ytAudioFile = video.path;
    plan.ytAudioStream = ytAudioStream;
    plan.workDir = workDir;
    plan.outFile = video.path;
    plan.track = track;
    plan.ytId = video.ytId;
    plan.align = align;
    plan.replaceAudio = true;
    plan.review = review;
    QString audioDetail;
    Muxer::ReviewSpan span;
    if (!Muxer::mux(plan, &m_cancel, &audioDetail, error, &span))
        return false;

    VideoInfo v = video;
    v.audioSource = QStringLiteral("library");
    v.audioDetail = audioDetail;
    v.review = review;
    v.reviewStart = span.start;
    v.reviewEnd = span.end;
    v.reviewGroup = review ? v.id : 0;
    m_db->updateVideoMedia(v);
    qInfo().noquote() << (review ? QStringLiteral("[audio] “%1” (%2) could not be confirmed as the track's recording: it waits for your review")
                                       .arg(video.title, video.ytTitle)
                                 : QStringLiteral("[audio] “%1” now plays the library's audio (%2)").arg(video.title, audioDetail));
    emit videoChanged(v.id);
    return true;
}

void ImportManager::approveVideo(qint64 videoId)
{
    std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v || !v->review)
        return;
    // The other options have served their purpose.
    const qint64 group = v->reviewGroup > 0 ? v->reviewGroup : v->id;
    for (const VideoInfo &other : m_db->reviewOptions(group)) {
        if (other.id == videoId)
            continue;
        m_db->relinkTracks(other.id, videoId);
        m_db->removeVideo(other.id);
        removeVideoFiles(other);
        emit videoRemoved(other.id);
    }
    v->review = false;
    v->reviewGroup = 0;
    m_db->updateVideoMedia(*v);
    // Whatever its tracks were still looking for, they have their video.
    for (const TrackInfo &t : m_db->tracksForVideo(videoId)) {
        if (t.state != QLatin1String("done"))
            m_db->setTrackResult(t.id, QStringLiteral("done"), videoId, QStringLiteral("you accepted “%1”").arg(v->ytTitle));
    }
    qInfo().noquote() << QStringLiteral("[review] accepted “%1” (%2)").arg(v->title, v->ytTitle);
    {
        QJsonArray tracks;
        for (const TrackInfo &t : m_db->tracksForVideo(videoId))
            tracks.append(QJsonObject{{QStringLiteral("trackId"), t.id}, {QStringLiteral("track"), t.title}, {QStringLiteral("album"), t.album}});
        appendLog({{QStringLiteral("event"), QStringLiteral("verdict")}, {QStringLiteral("verdict"), QStringLiteral("accept")},
                   {QStringLiteral("videoId"), videoId}, {QStringLiteral("id"), v->ytId}, {QStringLiteral("title"), v->ytTitle},
                   {QStringLiteral("tracks"), tracks}});
    }
    emit videoChanged(videoId);
    // The review stream has done its job; the track's audio becomes the default.
    ++m_reviewJobs;
    emit activityChanged();
    const QString path = v->path;
    m_auditPool.start([this, path] {
        QString error;
        if (!Muxer::dropReviewStream(path, &m_cancel, &error) && !m_cancel)
            qWarning().noquote() << "[review] could not tidy up" << path << ":" << error;
        --m_reviewJobs;
        QMetaObject::invokeMethod(this, [this] {
            if (!m_started)
                return;
            emit activityChanged();
            pump();
        }, Qt::QueuedConnection);
    });
}

void ImportManager::rejectVideo(qint64 videoId)
{
    const std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v || !v->review)
        return;
    const qint64 group = v->reviewGroup > 0 ? v->reviewGroup : v->id;
    QVector<VideoInfo> others;
    QVector<TrackInfo> tracks;
    for (const VideoInfo &option : m_db->reviewOptions(group)) {
        tracks += m_db->tracksForVideo(option.id);
        if (option.id != videoId)
            others << option;
    }
    // This upload is not offered for these tracks again. While other options
    // remain the tracks keep waiting on those; otherwise they have no video.
    for (const TrackInfo &t : std::as_const(tracks))
        m_db->rejectVideoFor(t, v->ytId);
    if (!others.isEmpty()) {
        m_db->relinkTracks(videoId, others.first().id);
    } else {
        const QString why = QStringLiteral("you turned down “%1”").arg(v->ytTitle);
        for (const TrackInfo &t : std::as_const(tracks))
            m_db->setTrackResult(t.id, QStringLiteral("not_found"), 0, why);
    }
    m_db->removeVideo(videoId);
    removeVideoFiles(*v);
    qInfo().noquote() << QStringLiteral("[review] rejected “%1” (%2)").arg(v->title, v->ytTitle);
    {
        QJsonArray names;
        for (const TrackInfo &t : std::as_const(tracks))
            names.append(QJsonObject{{QStringLiteral("trackId"), t.id}, {QStringLiteral("track"), t.title}, {QStringLiteral("album"), t.album}});
        appendLog({{QStringLiteral("event"), QStringLiteral("verdict")}, {QStringLiteral("verdict"), QStringLiteral("reject")},
                   {QStringLiteral("videoId"), videoId}, {QStringLiteral("id"), v->ytId}, {QStringLiteral("title"), v->ytTitle},
                   {QStringLiteral("optionsLeft"), others.size()}, {QStringLiteral("tracks"), names}});
    }
    emit videoRemoved(videoId);
    emit activityChanged();
}

void ImportManager::checkQuality()
{
    if (!m_started || m_upgrading.exchange(true))
        return;
    const ImportSettings cfg = settings();
    m_upgradeDone = 0;
    m_upgradeTotal = 0;
    emit activityChanged();
    m_auditPool.start([this, cfg] {
        upgradeVideos(cfg);
        m_upgrading = false;
        QMetaObject::invokeMethod(this, [this] {
            emit activityChanged();
            pump();
        }, Qt::QueuedConnection);
    });
}

int ImportManager::untrackedCount() const
{
    return int(m_db->untrackedVideos().size());
}

int ImportManager::deleteUntracked()
{
    // Not while tracks are being looked up or the folders are being read:
    // what is untracked is settled only once those are done.
    if (!m_started || busy())
        return 0;
    const QString root = QDir(settings().mvDir).absolutePath();
    QJsonArray names;
    int deleted = 0;
    for (const VideoInfo &v : m_db->untrackedVideos()) {
        m_db->removeAbsentTracksOf(v.id);
        m_db->removeVideo(v.id);
        removeVideoFiles(v);
        // An artist folder left empty goes too; never the MV folder itself.
        const QString dir = QFileInfo(v.path).absolutePath();
        if (dir != root && dir.startsWith(root + QLatin1Char('/')))
            QDir().rmdir(dir);
        names.append(QJsonObject{{QStringLiteral("ytId"), v.ytId}, {QStringLiteral("title"), v.title}, {QStringLiteral("artist"), v.artist}});
        ++deleted;
        emit videoRemoved(v.id);
    }
    if (deleted > 0) {
        qInfo("[library] deleted %d videos that no track in the music folders has", deleted);
        appendLog({{QStringLiteral("event"), QStringLiteral("untracked-deleted")}, {QStringLiteral("videos"), names}});
    }
    return deleted;
}

void ImportManager::fetchSubtitles()
{
    if (!m_started || settings().subtitleLangs.isEmpty() || m_upgrading.exchange(true))
        return;
    m_subtitling = true;
    const ImportSettings cfg = settings();
    m_upgradeDone = 0;
    m_upgradeTotal = 0;
    emit activityChanged();
    m_auditPool.start([this, cfg] {
        subtitleVideos(cfg);
        m_subtitling = false;
        m_upgrading = false;
        QMetaObject::invokeMethod(this, [this] {
            emit activityChanged();
            pump();
        }, Qt::QueuedConnection);
    });
}

void ImportManager::subtitleVideos(const ImportSettings &cfg)
{
    QVector<VideoInfo> videos;
    for (const VideoInfo &v : m_db->allVideos()) {
        const bool lacks = std::any_of(cfg.subtitleLangs.begin(), cfg.subtitleLangs.end(),
                                       [&v](const QString &l) { return !Subtitles::hasSidecar(v.path, l); });
        if (lacks && QFile::exists(v.path))
            videos << v;
    }
    m_upgradeTotal = int(videos.size());
    YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, &m_cancel);
    yt.setCacheDir(cacheDir(cfg.mvDir));
    const QString workDir = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/subtitles"));
    int withSubs = 0, failed = 0;
    for (const VideoInfo &v : std::as_const(videos)) {
        if (m_cancel || m_blocked)
            break;
        JobStatus st;
        st.upgrade = true;
        st.trackId = -v.id;
        st.title = v.title;
        st.artist = splitMulti(v.albumArtist).value(0, v.artist);
        st.stage = QStringLiteral("Fetching subtitles");
        emit jobChanged(st);

        QString error;
        const int got = fetchSubtitlesFor(yt, v.ytId, v.path, cfg.subtitleLangs, workDir, &error);
        if (m_cancel)
            break;
        if (got < 0)
            noteFailure(error);
        else
            noteSuccess();
        withSubs += got > 0;
        failed += got < 0;
        st.finished = true;
        st.outcome = got > 0 ? QStringLiteral("done") : got < 0 ? QStringLiteral("failed") : QStringLiteral("skipped");
        st.stage = got > 0 ? QStringLiteral("Subtitles added") : got < 0 ? QStringLiteral("Failed") : QStringLiteral("No subtitles");
        st.detail = got < 0 ? error : QString();
        if (got != 0)
            qInfo().noquote() << QStringLiteral("[subtitles] %1 — %2: %3%4").arg(st.artist, st.title, st.stage, got < 0 ? QStringLiteral(" (") + error + QLatin1Char(')') : QString());
        appendLog({{QStringLiteral("event"), QStringLiteral("subtitles")}, {QStringLiteral("ytId"), v.ytId},
                   {QStringLiteral("title"), v.title}, {QStringLiteral("outcome"), st.outcome}, {QStringLiteral("detail"), st.detail}});
        emit jobChanged(st);
        ++m_upgradeDone;
        emit activityChanged();
    }
    QDir(workDir).removeRecursively();
    if (!m_cancel) {
        qInfo("[subtitles] checked %d of %d videos: %d with subtitles, %d failed%s", m_upgradeDone.load(), int(videos.size()),
              withSubs, failed, m_blocked ? "; stopped because YouTube is limiting requests" : "");
    }
}

void ImportManager::upgradeVideos(const ImportSettings &cfg)
{
    // One video at a time: this is a courtesy pass, not worth YouTube's ire.
    const QVector<VideoInfo> videos = m_db->allVideos();
    m_upgradeTotal = int(videos.size());
    int rebuilt = 0, failed = 0;
    bool expired = false;
    for (const VideoInfo &v : videos) {
        if (m_cancel || m_blocked || expired)
            break;
        JobStatus st;
        st.upgrade = true;
        st.trackId = -v.id; // imports use track ids; these never collide with them
        st.title = v.title;
        st.artist = splitMulti(v.albumArtist).value(0, v.artist);
        st.stage = QStringLiteral("Checking quality");
        emit jobChanged(st);

        QString detail;
        const QString outcome = upgradeVideo(v, cfg, &detail);
        if (m_cancel)
            break;
        rebuilt += outcome == QLatin1String("done");
        failed += outcome == QLatin1String("failed");
        // Without the account there is nothing to find for any video.
        expired = outcome == QLatin1String("failed") && YtDlp::cookiesExpired(detail);
        appendLog({{QStringLiteral("event"), QStringLiteral("quality")}, {QStringLiteral("ytId"), v.ytId},
                   {QStringLiteral("title"), v.title}, {QStringLiteral("outcome"), outcome}, {QStringLiteral("detail"), detail}});
        st.finished = true;
        st.outcome = outcome;
        st.detail = detail;
        st.stage = outcome == QLatin1String("done") ? QStringLiteral("Upgraded")
            : outcome == QLatin1String("failed") ? QStringLiteral("Failed") : QStringLiteral("Already the best available");
        // "Already the best" with nothing to say about it is not worth a line each.
        if (outcome != QLatin1String("skipped") || detail.isEmpty() || !detail.at(0).isDigit())
            qInfo().noquote() << QStringLiteral("[quality] %1 — %2: %3 (%4)").arg(st.artist, st.title, st.stage, detail);
        emit jobChanged(st);
        ++m_upgradeDone;
        if (outcome == QLatin1String("done"))
            emit videoChanged(v.id);
        emit activityChanged();
    }
    if (!m_cancel) {
        const char *stopped = expired ? "; stopped because the account's cookies have expired"
            : m_blocked ? "; stopped because YouTube is limiting requests" : "";
        qInfo("[quality] checked %d of %d videos: %d upgraded, %d failed%s", m_upgradeDone.load(), int(videos.size()),
              rebuilt, failed, stopped);
        appendLog({{QStringLiteral("event"), QStringLiteral("quality-check")}, {QStringLiteral("checked"), m_upgradeDone.load()},
                   {QStringLiteral("videos"), int(videos.size())}, {QStringLiteral("upgraded"), rebuilt}, {QStringLiteral("failed"), failed},
                   {QStringLiteral("stopped"), QString::fromLatin1(stopped).mid(2)}});
    }
}

QString ImportManager::upgradeVideo(const VideoInfo &video, const ImportSettings &cfg, QString *detail)
{
    // Anonymous downloads top out around 130 kbit/s.
    constexpr double kOrdinaryKbps = 160;
    const auto fail = [&](const QString &why) {
        *detail = why;
        return QStringLiteral("failed");
    };
    if (!QFile::exists(video.path)) {
        *detail = QStringLiteral("file is missing");
        return QStringLiteral("skipped");
    }

    if (video.review) {
        *detail = QStringLiteral("waits for your review");
        return QStringLiteral("skipped");
    }
    const double have = video.ytAbr > 0 ? video.ytAbr : kOrdinaryKbps;
    if (have >= 200) {
        // Already built from Premium audio: not worth a request.
        *detail = QStringLiteral("%1p, audio %2 kbit/s").arg(video.height).arg(qRound(have));
        return QStringLiteral("skipped");
    }

    YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, &m_cancel);
    yt.setCacheDir(cacheDir(cfg.mvDir));
    QString error;
    QJsonObject offer;
    if (!yt.premiumInfo(video.ytId, &offer, &error)) {
        if (!m_cancel && !YtDlp::cookiesExpired(error))
            noteFailure(error);
        return fail(error);
    }
    noteSuccess();
    const double offered = YtDlp::bestAudioKbps(offer);
    // Only the account's audio counts: what anyone is offered is listed at
    // its nominal bitrate, above what a quiet song was measured at.
    const bool betterAudio = offered >= have * 1.3 && offered >= 200;
    const bool betterVideo = YtDlp::bestHeight(offer) > video.height;
    if (!betterAudio && !betterVideo) {
        *detail = QStringLiteral("%1p, audio %2 kbit/s").arg(video.height).arg(qRound(have));
        return QStringLiteral("skipped");
    }

    if (!claimVideo(video.ytId))
        return fail(QStringLiteral("cancelled"));
    const auto release = qScopeGuard([&] { releaseVideo(video.ytId); });
    const QString workDir = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/upgrade-%1").arg(video.id));
    QDir(workDir).removeRecursively();
    QDir().mkpath(workDir);
    const auto cleanup = qScopeGuard([&] { QDir(workDir).removeRecursively(); });

    QString audioFile;
    QJsonObject audioInfo;
    if (!yt.downloadAudio(video.ytId, workDir, &audioFile, &audioInfo, &error, true)) {
        if (!m_cancel)
            noteFailure(error);
        return fail(error);
    }
    QString videoFile = video.path, thumbFile;
    if (betterVideo && !yt.downloadVideo(video.ytId, workDir, nullptr, &videoFile, &thumbFile, &error)) {
        if (!m_cancel)
            noteFailure(error);
        return fail(error);
    }
    noteSuccess();

    // The library's own audio goes back in where it was used, from the best
    // file of the video's tracks.
    Muxer::Plan plan;
    plan.videoFile = videoFile;
    plan.ytAudioFile = audioFile;
    plan.workDir = workDir;
    plan.outFile = video.path;
    plan.ytId = video.ytId;
    plan.track.title = video.title;
    plan.track.artist = video.artist;
    plan.track.albumArtist = video.albumArtist;
    plan.track.album = video.album;
    plan.track.genre = video.genre;
    plan.track.year = video.year;
    plan.track.trackNo = video.trackNo;
    std::optional<TrackInfo> source;
    for (const TrackInfo &t : m_db->tracksForVideo(video.id)) {
        if (QFile::exists(t.path) && (!source || betterSource(t, *source)))
            source = t;
    }
    if (source && cfg.replaceAudio) {
        std::vector<int16_t> trackPcm, mvPcm;
        if (AudioAlign::decodeMono(source->path, &trackPcm, &m_cancel, &error)
            && AudioAlign::decodeMono(audioFile, &mvPcm, &m_cancel, &error)) {
            plan.align = fitted(AudioAlign::align(trackPcm, mvPcm), *source, video.ytTitle);
            plan.track = *source;
            plan.replaceAudio = audioMatches(plan.align) && audioReplaceable(plan.align)
                && libraryIsBetter(*source, youtubeQuality(audioInfo));
        }
    }
    if (m_cancel)
        return fail(QStringLiteral("cancelled"));
    // Never trade the library's audio for YouTube's, however good.
    if (!plan.replaceAudio && (video.audioSource == QLatin1String("library")
                               || (source && cfg.replaceAudio && libraryIsBetter(*source, youtubeQuality(audioInfo))))) {
        *detail = QStringLiteral("its library audio cannot be put back");
        return QStringLiteral("skipped");
    }

    QString audioDetail;
    if (!Muxer::mux(plan, &m_cancel, &audioDetail, &error))
        return fail(error);

    VideoInfo v = video;
    Muxer::Probe pr;
    if (Muxer::probe(video.path, &pr)) {
        v.duration = pr.duration;
        v.width = pr.width;
        v.height = pr.height;
        v.fps = pr.fps;
        v.vcodec = pr.vcodec;
    }
    v.audioSource = plan.replaceAudio ? QStringLiteral("library") : QStringLiteral("youtube");
    v.audioDetail = audioDetail;
    v.ytAbr = YtDlp::audioKbps(audioInfo);
    m_db->updateVideoMedia(v);
    QStringList gains;
    if (betterVideo)
        gains << QStringLiteral("%1p → %2p").arg(video.height).arg(v.height);
    if (betterAudio) {
        gains << (video.ytAbr > 0 ? QStringLiteral("YouTube audio %1 → %2 kbit/s").arg(qRound(have)).arg(qRound(v.ytAbr))
                                  : QStringLiteral("YouTube audio now %1 kbit/s").arg(qRound(v.ytAbr)));
    }
    *detail = gains.join(QStringLiteral(", "));
    return QStringLiteral("done");
}

void ImportManager::retryUnmatched()
{
    // Asked to look again: with fresh eyes, not at yesterday's search results.
    QDir(QDir(cacheDir(settings().mvDir)).filePath(QStringLiteral("search"))).removeRecursively();
    m_db->resetTracks({QStringLiteral("not_found"), QStringLiteral("failed")});
    for (const TrackInfo &t : m_db->pendingRecordings())
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
        if (r.added || r.changed || r.removed || r.returned)
            qInfo("[scan] %d tracks: %d new, %d changed, %d back, %d gone", r.total, r.added, r.changed, r.returned, r.removed);
        // What became of each file that was not simply unchanged.
        for (const QJsonObject &e : r.events)
            appendLog(e);
        const QStringList watched = m_watcher.directories();
        const QSet<QString> want(r.directories.begin(), r.directories.end());
        const QSet<QString> have(watched.begin(), watched.end());
        const QStringList drop = (have - want).values();
        const QStringList add = (want - have).values();
        if (!drop.isEmpty())
            m_watcher.removePaths(drop);
        if (!add.isEmpty())
            m_watcher.addPaths(add);

        m_db->requeueUntriedTitles();
        for (const TrackInfo &t : m_db->pendingRecordings())
            enqueue(t.id);
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
    // After a pause a single job tests the water before the rest follow.
    const int limit = m_probing ? 1 : m_jobPool.maxThreadCount();
    while (!m_blocked && m_active < limit && !m_queue.isEmpty()) {
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
    // No video under this title: the recording's other titles get their turn.
    if (m_started && m_db->requeueUntriedTitles() > 0) {
        for (const TrackInfo &t : m_db->pendingRecordings())
            enqueue(t.id);
    }
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
    if (!maybe || maybe->state != QLatin1String("pending") || maybe->absent)
        return;
    const TrackInfo track = *maybe;
    if (m_blocked)
        return; // stays pending; picked up again when the pause ends

    QElapsedTimer clock;
    clock.start();
    int nSearch = 0, nAudio = 0, nPreview = 0, nVideo = 0, nSubs = 0;
    const YtDlp *ytForLog = nullptr;
    QJsonArray logQueries, logCandidates, logChecked;
    qint64 heldVideoForLog = 0;
    QJsonArray logVideos; // what was brought into the library
    auto writeLog = [&](const QString &outcome, const QString &message) {
        // accept: the track has its video. review: it has videos waiting for
        // the user. reject: none of what was found is its video.
        const bool reviewing = std::any_of(logVideos.begin(), logVideos.end(), [](const QJsonValue &v) {
            return v.toObject().value(QLatin1String("review")).toBool();
        });
        const QString decision = outcome == QLatin1String("done") ? (reviewing || heldVideoForLog > 0 ? QStringLiteral("review") : QStringLiteral("accept"))
            : outcome == QLatin1String("not_found") ? QStringLiteral("reject")
            : outcome == QLatin1String("skipped") ? QStringLiteral("skipped")
            : QStringLiteral("undecided");
        appendLog({
            {QStringLiteral("event"), QStringLiteral("lookup")},
            {QStringLiteral("trackId"), track.id},
            {QStringLiteral("recording"), track.recording},
            {QStringLiteral("track"), track.title},
            {QStringLiteral("artist"), track.artist},
            {QStringLiteral("albumArtist"), track.albumArtist},
            {QStringLiteral("album"), track.album},
            {QStringLiteral("path"), track.path},
            {QStringLiteral("audio"), QStringLiteral("%1%2").arg(track.codec, track.lossless ? QStringLiteral(" %1/%2").arg(track.bitsPerSample).arg(track.sampleRate / 1000.0)
                                                                                           : QStringLiteral(" %1k").arg(track.bitrate))},
            {QStringLiteral("duration"), qRound(track.duration)},
            {QStringLiteral("outcome"), outcome},
            {QStringLiteral("decision"), decision},
            {QStringLiteral("message"), message},
            {QStringLiteral("videos"), logVideos},
            {QStringLiteral("seconds"), qRound(clock.elapsed() / 100.0) / 10.0},
            {QStringLiteral("requests"), QJsonObject{{QStringLiteral("search"), nSearch}, {QStringLiteral("audio"), nAudio},
                                                      {QStringLiteral("preview"), nPreview}, {QStringLiteral("video"), nVideo}, {QStringLiteral("subtitles"), nSubs}, {QStringLiteral("fromCache"), ytForLog ? ytForLog->cacheHits() : 0}}},
            {QStringLiteral("queries"), logQueries},
            {QStringLiteral("candidates"), logCandidates},
            {QStringLiteral("checked"), logChecked},
        });
    };
    // One examined candidate: what was decided about it, why, and the numbers.
    // decision: accept | review | reject | undecided (could not be examined).
    auto checked = [&](const YtCandidate &c, const QString &result, const QString &reason,
                       const AudioAlign::Result *ar = nullptr, const QJsonObject &more = {}) {
        static const QHash<QString, QString> decisions = {
            {QStringLiteral("match"), QStringLiteral("accept")}, {QStringLiteral("shared"), QStringLiteral("accept")},
            {QStringLiteral("unconfirmed"), QStringLiteral("review")}, {QStringLiteral("option"), QStringLiteral("review")},
            {QStringLiteral("shared-review"), QStringLiteral("review")},
            {QStringLiteral("error"), QStringLiteral("undecided")},
        };
        QJsonObject o{
            {QStringLiteral("id"), c.id},
            {QStringLiteral("title"), c.title},
            {QStringLiteral("channel"), c.channel},
            {QStringLiteral("duration"), qRound(c.duration)},
            {QStringLiteral("result"), result},
            {QStringLiteral("decision"), decisions.value(result, QStringLiteral("reject"))},
            {QStringLiteral("reason"), reason},
        };
        if (ar)
            o.insert(QStringLiteral("measured"), measurements(*ar));
        for (auto it = more.begin(); it != more.end(); ++it)
            o.insert(it.key(), it.value());
        logChecked.append(o);
    };

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

    // The options this track already has waiting, from an earlier look.
    qint64 heldGroup = 0;
    qint64 heldVideo = 0;
    if (track.videoId > 0) {
        if (const auto held = m_db->video(track.videoId); held && held->review) {
            heldVideo = held->id;
            heldVideoForLog = held->id;
            heldGroup = held->reviewGroup > 0 ? held->reviewGroup : held->id;
        }
    }

    auto finish = [&](const QString &result, qint64 resultVideo, const QString &resultMessage) {
        cleanup();
        if (m_cancel)
            return; // leave the track pending for the next run
        // A track that was only looking for more options keeps those it has.
        const bool keeps = heldVideo > 0 && result != QLatin1String("done");
        const QString outcome = keeps ? QStringLiteral("done") : result;
        const qint64 videoId = keeps ? heldVideo : resultVideo;
        const QString message = keeps ? QStringLiteral("no other video to offer for review") : resultMessage;
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
        writeLog(outcome, message);
        emit jobChanged(st);
    };

    // YouTube is refusing requests: put the track back untouched.
    auto postpone = [&] {
        cleanup();
        if (m_cancel)
            return;
        writeLog(QStringLiteral("postponed"), QString());
        st.finished = true;
        st.outcome = QStringLiteral("postponed");
        st.progress = -1;
        st.stage = QStringLiteral("Waiting");
        st.detail = QStringLiteral("YouTube is limiting requests");
        emit jobChanged(st);
    };

    QString why;
    if (Matcher::isNonMvTrack(track, &why)) {
        finish(QStringLiteral("skipped"), 0, why);
        return;
    }
    // Talk between songs (a stage announcement, an interview) has no video
    // to find. A title can mislead and so can the sound, so it takes both.
    if (Matcher::isTalkTitle(track.title)) {
        std::vector<int16_t> pcm;
        QString decodeError;
        if (AudioAlign::decodeMono(track.path, &pcm, &m_cancel, &decodeError)) {
            const TalkCheck::Result talk = TalkCheck::measure(pcm);
            if (talk.talk) {
                finish(QStringLiteral("skipped"), 0,
                       QStringLiteral("talk, not a song: titled as such and sounds like it (%1% pauses, beat %2)")
                           .arg(qRound(talk.pauses * 100))
                           .arg(talk.beat, 0, 'f', 2));
                return;
            }
        }
        if (m_cancel)
            return cleanup();
    }

    // ---- 1. Search ---------------------------------------------------------
    report(QStringLiteral("Searching"));
    YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, &m_cancel);
    yt.setCacheDir(cacheDir(cfg.mvDir));
    ytForLog = &yt;
    QVector<YtCandidate> candidates;
    QSet<QString> seen;
    bool searched = false;
    QString error;
    auto usable = [&](const YtCandidate &c) {
        return c.rejectReason.isEmpty() && (c.trusted || cfg.allowUnofficial);
    };
    for (const QString &query : Matcher::searchQueries(track)) {
        QVector<YtCandidate> found;
        ++nSearch;
        if (!yt.search(query, 10, &found, &error)) {
            if (m_cancel)
                return cleanup();
            logQueries.append(QJsonObject{{QStringLiteral("q"), query}, {QStringLiteral("error"), error}});
            if (noteFailure(error))
                return postpone();
            continue;
        }
        noteSuccess();
        logQueries.append(QJsonObject{{QStringLiteral("q"), query}, {QStringLiteral("results"), found.size()}});
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
    QVector<YtCandidate> shortlisted;
    QSet<QString> shortlist;
    for (const YtCandidate &c : std::as_const(candidates)) {
        if (usable(c) && shortlisted.size() < 4) {
            shortlisted.append(c);
            shortlist.insert(c.id);
        }
        QJsonObject o{{QStringLiteral("id"), c.id}, {QStringLiteral("title"), c.title}, {QStringLiteral("channel"), c.channel},
                      {QStringLiteral("duration"), qRound(c.duration)}, {QStringLiteral("score"), qRound(c.score)},
                      {QStringLiteral("trusted"), c.trusted},
                      // examined by listening: the best four that look official
                      {QStringLiteral("shortlisted"), usable(c) && shortlist.contains(c.id)}};
        if (!c.rejectReason.isEmpty())
            o.insert(QStringLiteral("rejected"), c.rejectReason);
        logCandidates.append(o);
    }
    if (shortlisted.isEmpty()) {
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
    // A video to bring in: the one that fits outright, or every candidate
    // that is the song without demonstrably being the recording, as options
    // for the user to choose from.
    struct Pick {
        YtCandidate c;
        QString dir, audio;
        QJsonObject info;
        AudioAlign::Result align;
    };
    QVector<Pick> unconfirmed;
    QStringList reasons;
    bool anyChecked = false;
    bool undecided = false; // some candidate could not be examined

    // Downloads fail transiently (throttling, expired stream URLs); one more
    // try after a short pause settles most of them.
    // A refusal aimed at this client as a whole is not retried at all.
    auto withRetry = [&](const std::function<bool()> &attempt) {
        if (attempt()) {
            noteSuccess();
            return true;
        }
        if (m_cancel || YtDlp::looksBlocked(error)) {
            noteFailure(error);
            return false;
        }
        for (int i = 0; i < 30 && !m_cancel; ++i)
            QThread::msleep(100);
        if (!m_cancel && attempt()) {
            noteSuccess();
            return true;
        }
        noteFailure(error);
        return false;
    };

    for (const YtCandidate &c : std::as_const(shortlisted)) {
        if (m_cancel)
            return cleanup();
        if (!claimVideo(c.id))
            return cleanup();
        bool keepClaim = false;
        auto unclaim = qScopeGuard([&] {
            if (!keepClaim)
                releaseVideo(c.id);
        });

        if (m_db->videoRejectedFor(track, c.id)) {
            anyChecked = true; // examined, by the user
            checked(c, QStringLiteral("rejected"), QStringLiteral("you turned this upload down for the track"));
            reasons << QStringLiteral("“%1” was turned down for this track").arg(c.title);
            continue;
        }

        if (const auto existing = m_db->videoByYtId(c.id)) {
            if (heldGroup > 0 && existing->review
                && (existing->reviewGroup > 0 ? existing->reviewGroup : existing->id) == heldGroup) {
                checked(c, QStringLiteral("option"), QStringLiteral("already waiting for review as one of the track's options"));
                continue; // already one of this track's options
            }
            // Already in the MV library through another track (a single and
            // its album cut, say). It still has to be this recording.
            // What the video sounds like: a video under review carries the
            // track it waits for, YouTube's audio, and the review stream; an
            // accepted one plays its track's audio where that was put in.
            const int heard = existing->review ? 1 : 0;
            std::vector<int16_t> pcm;
            if (QFile::exists(existing->path) && AudioAlign::decodeMono(existing->path, &pcm, &m_cancel, &error, heard)) {
                anyChecked = true;
                const AudioAlign::Result ar = fitted(AudioAlign::align(trackPcm, pcm), track, existing->ytTitle);
                if (audioMatches(ar)) {
                    // The video belongs to another track already. This one
                    // joins it when it is demonstrably the same recording (a
                    // single and its album cut) — measured against the audio
                    // the video plays, which is that other track's where the
                    // library's was put in. Being the same song is not
                    // enough: a live take does not get the studio video.
                    // A track of lesser quality than YouTube's audio has no
                    // audio of its own at stake and shares on the song alone.
                    const bool own = cfg.replaceAudio && libraryIsBetter(track, storedYoutubeQuality(*existing));
                    const bool recording = ownRecording(ar, track, existing->ytTitle);
                    if (existing->review && own && recording) {
                        // It waits for a verdict on behalf of a track that is
                        // only the song (a live take, looked up first). This
                        // track is demonstrably its recording: the video is
                        // this track's, and no longer a question.
                        const qint64 group = existing->reviewGroup > 0 ? existing->reviewGroup : existing->id;
                        const QVector<TrackInfo> waiting = m_db->tracksForVideo(existing->id);
                        if (putLibraryAudioIn(*existing, track, ar, cfg, &error, false, heard)) {
                            // Those tracks keep whatever other options they
                            // had; with none left, this was not their video.
                            QVector<VideoInfo> left = m_db->reviewOptions(group);
                            if (!left.isEmpty()) {
                                m_db->relinkTracks(existing->id, left.first().id);
                            } else {
                                for (const TrackInfo &w : waiting) {
                                    m_db->setTrackResult(w.id, QStringLiteral("not_found"), 0,
                                                         QStringLiteral("“%1” is the video of “%2” [%3]").arg(c.title, track.title, track.album));
                                }
                            }
                            checked(c, QStringLiteral("shared"),
                                    QStringLiteral("was waiting for review for another track; this one is its recording (%1% the same waveform) and takes it")
                                        .arg(qRound(100 * ar.goodSec / qMax(1.0, std::min(ar.trackSec, ar.videoSec)))),
                                    &ar);
                            finish(QStringLiteral("done"), existing->id, QStringLiteral("takes “%1” out of review").arg(c.title));
                            return;
                        }
                        if (m_cancel)
                            return cleanup();
                    }
                    if (existing->review || !own) {
                        checked(c, existing->review ? QStringLiteral("shared-review") : QStringLiteral("shared"),
                                existing->review ? QStringLiteral("already waiting for review through “%1”: one more track for the same verdict").arg(existing->title)
                                                 : QStringLiteral("already in the library through “%1”, and the song by its fingerprints").arg(existing->title),
                                &ar);
                        finish(QStringLiteral("done"), existing->id,
                               QStringLiteral("shares the video of “%1”").arg(existing->title));
                        return;
                    }
                    if (recording
                        && (existing->audioSource == QLatin1String("library") || putLibraryAudioIn(*existing, track, ar, cfg, &error))) {
                        checked(c, QStringLiteral("shared"),
                                QStringLiteral("already in the library through “%1”, and the same recording: %2% of it is demonstrably the same waveform")
                                    .arg(existing->title)
                                    .arg(qRound(100 * ar.goodSec / qMax(1.0, std::min(ar.trackSec, ar.videoSec)))),
                                &ar);
                        finish(QStringLiteral("done"), existing->id,
                               QStringLiteral("shares the video of “%1”").arg(existing->title));
                        return;
                    }
                    if (m_cancel)
                        return cleanup();
                    checked(c, QStringLiteral("different-recording"),
                            QStringLiteral("already in the library as the video of “%1”; this track is the song but not that recording: %2")
                                .arg(existing->title, unconfirmedReason(ar, track, existing->ytTitle)),
                            &ar);
                    reasons << QStringLiteral("“%1” is the video of another recording of the song").arg(c.title);
                    continue;
                }
                checked(c, QStringLiteral("different"),
                        QStringLiteral("already in the library through “%1”; %2").arg(existing->title, mismatchReason(ar)), &ar);
            } else if (!m_cancel) {
                checked(c, QStringLiteral("error"), QStringLiteral("already in the library, but its file cannot be read: %1").arg(error));
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
        ++nAudio;
        if (!withRetry([&] { return yt.downloadAudio(c.id, dir, &audioFile, &info, &error); })) {
            if (m_cancel)
                return cleanup();
            checked(c, QStringLiteral("error"), QStringLiteral("its audio could not be downloaded: %1").arg(error));
            if (m_blocked)
                return postpone();
            // Could not listen to it: that says nothing about the video.
            undecided = true;
            reasons << QStringLiteral("%1: %2").arg(c.id, error);
            continue;
        }
        std::vector<int16_t> mvPcm;
        if (!AudioAlign::decodeMono(audioFile, &mvPcm, &m_cancel, &error)) {
            if (m_cancel)
                return cleanup();
            checked(c, QStringLiteral("error"), QStringLiteral("its audio could not be decoded: %1").arg(error));
            reasons << QStringLiteral("%1: %2").arg(c.id, error);
            continue;
        }
        anyChecked = true;
        const AudioAlign::Result ar = fitted(AudioAlign::align(trackPcm, mvPcm), track, c.title);
        qInfo().noquote() << QStringLiteral("[align] %1 ~ “%2” [%3]: %4").arg(track.title, c.title, c.id, ar.summary());
        if (!audioMatches(ar)) {
            checked(c, QStringLiteral("different"), mismatchReason(ar), &ar);
            reasons << QStringLiteral("“%1” is a different recording").arg(c.title);
            QDir(dir).removeRecursively();
            continue;
        }

        // The library's audio is the better one but cannot be shown to be
        // the recording in this video: a live take, a cover, another mix.
        // YouTube's audio must not take its place, and the track's may not
        // belong there: that is for the user to say. Such a video is only
        // taken, for review, when no candidate fits outright. (A lossy file
        // may still be the lesser one next to the account's audio.)
        bool forReview = false;
        if (cfg.replaceAudio && libraryIsBetter(track, youtubeQuality(info)) && !ownRecording(ar, track, c.title)) {
            bool premiumWins = false;
            if (!track.lossless && yt.hasCookies()) {
                QString premiumFile;
                QJsonObject premiumInfo;
                ++nAudio;
                if (yt.downloadAudio(c.id, dir, &premiumFile, &premiumInfo, &error, true)
                    && !libraryIsBetter(track, youtubeQuality(premiumInfo))) {
                    premiumWins = true;
                    audioFile = premiumFile;
                    info = premiumInfo;
                }
                if (m_cancel)
                    return cleanup();
            }
            if (!premiumWins)
                forReview = true;
        }

        // Where nothing is at stake for the library's audio there is no
        // review either: such a video is simply not the song's.
        if (!forReview && interrupted(ar)) {
            checked(c, QStringLiteral("different"), interruptedReason(ar), &ar);
            reasons << QStringLiteral("“%1” interrupts the song").arg(c.title);
            QDir(dir).removeRecursively();
            continue;
        }

        if (cfg.skipStillImages) {
            report(QStringLiteral("Checking video"), -1, c.title);
            // A still image is one for every track that has it as a candidate.
            Muxer::StillCheck check;
            const QJsonObject known = yt.note(c.id, QStringLiteral("picture"));
            if (known.value(QLatin1String("samples")).toInt() > 0) {
                check.valid = true;
                check.still = known.value(QLatin1String("still")).toBool();
                check.movingShare = known.value(QLatin1String("movingShare")).toDouble();
                check.samples = known.value(QLatin1String("samples")).toInt();
            } else {
                QString preview;
                ++nPreview;
                if (!withRetry([&] { return yt.downloadPreview(c.id, dir, &preview, &error); })) {
                    if (m_cancel)
                        return cleanup();
                    checked(c, QStringLiteral("error"), QStringLiteral("its picture could not be downloaded: %1").arg(error), &ar);
                    if (m_blocked)
                        return postpone();
                    // Never accept a video whose picture could not be looked at.
                    undecided = true;
                    reasons << QStringLiteral("%1: %2").arg(c.id, error);
                    QDir(dir).removeRecursively();
                    continue;
                }
                check = Muxer::checkStill(preview, false, &m_cancel);
                QFile::remove(preview);
                if (m_cancel)
                    return cleanup();
                if (check.valid) {
                    yt.setNote(c.id, QStringLiteral("picture"),
                               {{QStringLiteral("still"), check.still}, {QStringLiteral("movingShare"), check.movingShare},
                                {QStringLiteral("samples"), check.samples}});
                }
            }
            const QJsonObject picture{{QStringLiteral("movingShare"), std::round(check.movingShare * 1000) / 1000},
                                      {QStringLiteral("framePairs"), qMax(0, check.samples - 1)}};
            if (!check.valid) {
                checked(c, QStringLiteral("error"), QStringLiteral("its picture could not be analysed"), &ar);
                undecided = true;
                reasons << QStringLiteral("“%1”: could not analyse the picture").arg(c.title);
                QDir(dir).removeRecursively();
                continue;
            }
            if (check.still) {
                checked(c, QStringLiteral("still"),
                        QStringLiteral("a still image: %1% of %2 sampled frame pairs change").arg(qRound(check.movingShare * 100)).arg(qMax(0, check.samples - 1)),
                        &ar, {{QStringLiteral("picture"), picture}});
                reasons << QStringLiteral("“%1” is a still image").arg(c.title);
                QDir(dir).removeRecursively();
                continue;
            }
        }

        if (forReview) {
            checked(c, QStringLiteral("unconfirmed"),
                    QStringLiteral("the song by its fingerprints, but %1").arg(unconfirmedReason(ar, track, c.title)),
                    &ar, {{QStringLiteral("youtubeKbps"), qRound(YtDlp::audioKbps(info))}});
            reasons << QStringLiteral("“%1” could not be confirmed as this recording").arg(c.title);
            unconfirmed.append({c, dir, audioFile, info, ar});
            continue;
        }

        checked(c, QStringLiteral("match"),
                ar.byOffset ? QStringLiteral("the same performance in another mix: fingerprints cover the song, loudness agrees, neither is marked as another version")
                : libraryIsBetter(track, youtubeQuality(info)) && cfg.replaceAudio
                    ? QStringLiteral("the track's recording: %1% of it is demonstrably the same waveform")
                          .arg(qRound(100 * ar.goodSec / qMax(1.0, std::min(ar.trackSec, ar.videoSec))))
                    : QStringLiteral("the song by its fingerprints; YouTube's audio is kept (the track is the lesser one, or library audio is switched off)"),
                &ar, {{QStringLiteral("youtubeKbps"), qRound(YtDlp::audioKbps(info))}});
        chosen = c;
        chosenDir = dir;
        ytAudio = audioFile;
        ytInfo = info;
        alignment = ar;
        keepClaim = true;
        break;
    }

    QVector<Pick> picks;
    const bool forReview = chosen.id.isEmpty();
    if (!forReview)
        picks.append({chosen, chosenDir, ytAudio, ytInfo, alignment});
    else
        picks = unconfirmed;

    if (picks.isEmpty()) {
        if (heldVideo > 0) {
            // Nothing new: the track keeps the options it has.
            finish(QStringLiteral("done"), heldVideo, QStringLiteral("no other video to offer for review"));
            return;
        }
        // "Not found" is a verdict and is not revisited for weeks. If any
        // candidate could not be examined (a download error, typically
        // YouTube throttling), the answer is still open: retry later.
        const bool open = undecided || !anyChecked;
        finish(open ? QStringLiteral("failed") : QStringLiteral("not_found"), 0, reasons.join(QStringLiteral("; ")));
        return;
    }

    // Brings one video into the library. Stopped: cancelled or postponed, and
    // already dealt with.
    enum class Got { Ok, Failed, Stopped };
    const auto bringIn = [&](Pick pick, bool needsReview, qint64 group, qint64 *videoIdOut, QString *summary) -> Got {
    YtCandidate &chosen = pick.c;
    QString &chosenDir = pick.dir;
    QString &ytAudio = pick.audio;
    QJsonObject &ytInfo = pick.info;
    AudioAlign::Result &alignment = pick.align;

    // ---- 3. Download -------------------------------------------------------
    report(QStringLiteral("Downloading"), 0, chosen.title);
    QString videoFile, thumbFile;
    double lastShown = -1;
    ++nVideo;
    const bool got = withRetry([&] {
        lastShown = -1;
        return yt.downloadVideo(chosen.id, chosenDir, [&](double p) {
            if (p - lastShown >= 0.01 || p >= 1.0) {
                lastShown = p;
                report(QStringLiteral("Downloading"), p, chosen.title);
            }
        }, &videoFile, &thumbFile, &error);
    });
    if (!got) {
        if (m_cancel) {
            cleanup();
            return Got::Stopped;
        }
        if (m_blocked) {
            postpone();
            return Got::Stopped;
        }
        error = QStringLiteral("download failed: %1").arg(error);
        return Got::Failed;
    }

    // With a Premium account the audio comes at about twice the bitrate. The
    // candidates were compared on the ordinary audio, which needs no account.
    if (yt.hasCookies() && !QFileInfo(ytAudio).fileName().startsWith(QLatin1String("premium."))) {
        report(QStringLiteral("Fetching Premium audio"), -1, chosen.title);
        QString premiumFile;
        QJsonObject premiumInfo;
        ++nAudio;
        if (yt.downloadAudio(chosen.id, chosenDir, &premiumFile, &premiumInfo, &error, true)) {
            if (youtubeQuality(premiumInfo) > youtubeQuality(ytInfo)) {
                ytAudio = premiumFile;
                ytInfo = premiumInfo;
            }
        } else {
            if (m_cancel) {
                cleanup();
                return Got::Stopped;
            }
            qWarning().noquote() << "[import] no Premium audio, keeping the ordinary one:" << error;
        }
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
    // (With the account's audio a lossy track may have become the lesser one.)
    needsReview = needsReview && libraryIsBetter(track, youtubeQuality(ytInfo));
    if (needsReview) {
        plan.align = placedWhole(alignment);
        needsReview = plan.align.byOffset;
    }
    plan.review = needsReview;
    plan.replaceAudio = needsReview
        || (cfg.replaceAudio && audioReplaceable(alignment) && libraryIsBetter(track, youtubeQuality(ytInfo)));

    report(plan.replaceAudio ? QStringLiteral("Muxing library audio") : QStringLiteral("Muxing"), -1, chosen.title);
    QString audioDetail;
    Muxer::ReviewSpan span;
    bool muxed = Muxer::mux(plan, &m_cancel, &audioDetail, &error, &span);
    // No falling back to YouTube's audio when the library's cannot be put in.
    if (!muxed) {
        if (m_cancel) {
            cleanup();
            return Got::Stopped;
        }
        return Got::Failed;
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

    // Its subtitles, where the uploader made some. Their absence, or
    // YouTube's refusal, is no reason not to have the video.
    if (!cfg.subtitleLangs.isEmpty() && !m_cancel) {
        report(QStringLiteral("Fetching subtitles"), -1, chosen.title);
        QString why;
        ++nSubs;
        if (fetchSubtitlesFor(yt, chosen.id, outFile, cfg.subtitleLangs, chosenDir, &why) < 0 && !m_cancel)
            qWarning().noquote() << "[import] no subtitles for" << chosen.id << ":" << why;
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
    v.ytAbr = YtDlp::audioKbps(ytInfo);
    v.review = needsReview;
    v.reviewStart = span.start;
    v.reviewEnd = span.end;
    v.reviewGroup = needsReview ? group : 0;
    v.ytTitle = chosen.title;
    v.ytChannel = chosen.channel;
    v.tags = track.tags;
    v.addedAt = QDateTime::currentSecsSinceEpoch();

    if (m_cancel) {
        QFile::remove(outFile);
        QFile::remove(outThumb);
        cleanup();
        return Got::Stopped;
    }
    const qint64 videoId = m_db->insertVideo(v);
    if (videoId <= 0) {
        QFile::remove(outFile);
        QFile::remove(outThumb);
        error = QStringLiteral("could not record the video in the library");
        return Got::Failed;
    }
    if (needsReview && group <= 0) {
        v.id = videoId;
        v.reviewGroup = videoId;
        m_db->updateVideoMedia(v);
    }
    *videoIdOut = videoId;
    *summary = QStringLiteral("%1 · %2×%3 · %4").arg(chosen.title).arg(pr.width).arg(pr.height).arg(audioDetail);
    return Got::Ok;
    };

    QVector<qint64> added;
    QString firstSummary, lastError;
    qint64 group = heldGroup;
    // The videos stay claimed until this track's result is recorded. Released
    // sooner, another track could act on one of them in between — take an
    // option out of review before the track waiting for it was linked to it,
    // which left that track with the wrong video and an option with none.
    QStringList claimed;
    const auto release = qScopeGuard([&] {
        for (const QString &id : std::as_const(claimed))
            releaseVideo(id);
    });
    if (forReview) {
        // All of them first, in one order for every track: two tracks that
        // each hold one option and wait for the other's would wait for ever.
        QStringList ids;
        for (const Pick &pick : std::as_const(picks))
            ids << pick.c.id;
        ids.sort();
        for (const QString &id : std::as_const(ids)) {
            if (!claimVideo(id))
                return cleanup();
            claimed << id;
        }
    } else {
        // The outright match still holds its claim from the examination.
        claimed << picks.first().c.id;
    }
    for (const Pick &pick : std::as_const(picks)) {
        if (forReview && m_db->videoByYtId(pick.c.id))
            continue; // another track brought it in meanwhile
        qint64 videoId = 0;
        QString summary;
        const Got got = bringIn(pick, forReview, group, &videoId, &summary);
        if (got == Got::Stopped) {
            // Half a set of options is no use: the track looks again later.
            for (qint64 id : std::as_const(added)) {
                if (const auto v = m_db->video(id)) {
                    m_db->removeVideo(id);
                    removeVideoFiles(*v);
                }
            }
            return;
        }
        if (got == Got::Failed) {
            lastError = error;
            logVideos.append(QJsonObject{{QStringLiteral("id"), pick.c.id}, {QStringLiteral("title"), pick.c.title},
                                         {QStringLiteral("failed"), error}});
            continue;
        }
        if (added.isEmpty())
            firstSummary = summary;
        logVideos.append(QJsonObject{{QStringLiteral("videoId"), videoId}, {QStringLiteral("id"), pick.c.id},
                                     {QStringLiteral("title"), pick.c.title}, {QStringLiteral("review"), forReview},
                                     {QStringLiteral("summary"), summary}});
        if (forReview && group <= 0)
            group = videoId;
        added << videoId;
    }
    if (m_cancel)
        return;
    if (added.isEmpty()) {
        if (heldVideo > 0)
            finish(QStringLiteral("done"), heldVideo, QStringLiteral("no other video to offer for review"));
        else
            finish(QStringLiteral("failed"), 0, lastError.isEmpty() ? QStringLiteral("could not bring the video in") : lastError);
        return;
    }
    if (!forReview && heldGroup > 0) {
        // A video that fits outright settles it: the options are not needed.
        for (const VideoInfo &old : m_db->reviewOptions(heldGroup)) {
            m_db->removeVideo(old.id);
            removeVideoFiles(old);
            emit videoRemoved(old.id);
        }
    }
    const int options = forReview ? int(m_db->reviewOptions(group).size()) : 0;
    finish(QStringLiteral("done"), forReview && heldVideo > 0 ? heldVideo : added.first(),
           forReview ? QStringLiteral("for your review (%1 %2): %3").arg(options).arg(options == 1 ? QStringLiteral("option") : QStringLiteral("options"), firstSummary)
                     : firstSummary);
    for (qint64 id : std::as_const(added))
        emit videoAdded(id);
}
