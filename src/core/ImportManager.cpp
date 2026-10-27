#include "core/ImportManager.h"

#include "core/ArtistChannels.h"
#include "core/MusicBrainz.h"
#include "core/Judge.h"

#include "core/TalkCheck.h"

#include "core/Subtitles.h"

#include "core/AudioAlign.h"
#include "core/Matcher.h"
#include "core/TagWriter.h"
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

#include <filesystem>
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
constexpr int kLogGenerations = 8;

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
    const double whole = std::min(r.trackSec, r.videoSec);
    return !r.segments.isEmpty() && r.goodSec >= kOwnWaveform * whole;
}

// A video already in the library plays its first track's audio, and later
// tracks were measured against that. So the English version of a song, looked
// up first, kept the Japanese video, and the Japanese track then "shared" it
// at 80%. A track that is all but identical to the video's own audio, where
// the track holding the video is clearly less so, becomes the audio the
// video plays. The holder stays linked to the video.
// A video shorter than the track is a cut of the song: the ninety seconds
// used as a show's opening, a "short ver.", an edit. Taken outright, such a
// clip stood in for four-minute tracks. Within a tenth of the track's
// length is the whole song give or take an intro.
constexpr double kShortCut = 0.90;

bool shortCut(const AudioAlign::Result &r)
{
    return r.trackSec > 0 && r.videoSec < kShortCut * r.trackSec;
}

double waveformShare(const AudioAlign::Result &r)
{
    return r.goodSec / qMax(1.0, std::min(r.trackSec, r.videoSec));
}

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

// R1, the track's own recording: what lets an upload be taken outright.
// Only on the artist's own channel: a third party's copy of the song's
// video fits as well as the real one, and a label's upload may be a cut.
// Only the same version by its words (R3): a live performance, a dance
// practice or a crossfade on the artist's channel carries the studio audio
// all but whole, and is not the music video. Everything else that is the
// song waits for the user (D1.14). Which audio the track has is no part of
// it (R6, F3).
bool sameRecordingAs(const AudioAlign::Result &r, const TrackInfo &track, const QString &videoTitle, bool artistChannel)
{
    return audioReplaceable(r) && !interrupted(r) && Matcher::sameVersion(track, videoTitle, artistChannel);
}

bool ownRecording(const AudioAlign::Result &r, const TrackInfo &track, const QString &videoTitle, bool artistChannel)
{
    return artistChannel && !shortCut(r) && sameRecordingAs(r, track, videoTitle, artistChannel);
}

// What a candidate that is not taken as the track's recording lacks.
QString unconfirmedReason(const AudioAlign::Result &r, const TrackInfo &track, const QString &videoTitle, const QString &channel,
                          bool artistChannel)
{
    const int share = qRound(100 * r.goodSec / qMax(1.0, std::min(r.trackSec, r.videoSec)));
    if (audioReplaceable(r) && interrupted(r))
        return QStringLiteral("%1% of it is demonstrably the track's waveform, but %2").arg(share).arg(interruptedReason(r));
    if (audioReplaceable(r) && shortCut(r))
        return QStringLiteral("%1% of it is demonstrably the track's waveform, but the video is a cut of the song: %2 s of %3 s")
            .arg(share).arg(qRound(r.videoSec)).arg(qRound(r.trackSec));
    if (audioReplaceable(r) && !artistChannel)
        return QStringLiteral("%1% of it is demonstrably the track's waveform, but “%2” is not known as the artist's channel")
            .arg(share).arg(channel);
    if (audioReplaceable(r))
        return QStringLiteral("%1% of it is demonstrably the track's waveform, but track and video are marked as different versions").arg(share);
    return QStringLiteral("only %1% of it is demonstrably the track's waveform (%2% needed)%3")
        .arg(share)
        .arg(qRound(100 * kOwnWaveform))
        .arg(Matcher::sameVersion(track, videoTitle, artistChannel) ? QString() : QStringLiteral("; track and video are marked as different versions"));
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


// The library's file is the better audio: it is what the video must play.
// YouTube's is only ever the main audio when the file is the lesser one.
bool libraryIsBetter(const TrackInfo &track, double youtubeQuality)
{
    return trackQuality(track) > youtubeQuality * 1.15;
}

// "Ignore small channels": an upload from a channel with fewer subscribers
// than the limit is not a track's video, unless YouTube marks the channel as
// verified (an artist's own can be a small one) or the user allowed it. A
// count that is not known is no reason: the channel hides it, or the upload
// is listed under several channels and the page gives none to go by (see
// YtDlp::channelOf).
bool smallChannel(qint64 subscribers, bool verified, bool allowed, const ImportSettings &cfg)
{
    return cfg.minSubscribers > 0 && subscribers >= 0 && subscribers < cfg.minSubscribers && !verified && !allowed;
}

QString smallChannelReason(qint64 subscribers, const ImportSettings &cfg)
{
    return QStringLiteral("its channel has %1 subscribers, under the limit of %2").arg(subscribers).arg(cfg.minSubscribers);
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
    m_replacePool.setMaxThreadCount(1);
    m_verdictPool.setMaxThreadCount(1);
    m_previewPool.setMaxThreadCount(2);
    m_judgePool.setMaxThreadCount(qBound(1, m_settings.judgeThreads, 8));
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
        // The cache of candidates grows by gigabytes an hour while a large
        // library is imported. What is in use was just read or written, and
        // the oldest goes first.
        if (m_started) {
            const QString dir = cacheDir(settings().mvDir);
            QThreadPool::globalInstance()->start([dir] { YtDlp::pruneCache(dir, 2LL * 1024 * 1024 * 1024, 14); });
        }
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
    m_judgePool.setMaxThreadCount(qBound(1, s.judgeThreads, 8));
    YtDlp::setCapture(s.captureDir);
    YtDlp::setReplay(s.replayDir);
    Judge::setCapture(s.captureDir);
    Judge::setReplay(s.replayDir);
    // Q2: with the judge running the lookups, A2–A4 do not skip a track; the
    // next scan unskips what they had.
    Matcher::setJudgeRunsLookups(s.judgeMode == QLatin1String("judge"));
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
    m_settledOnce = false;

    // Leftovers from a previous run that was interrupted: scratch files, and
    // half-written outputs if the process was killed outright mid-mux. And
    // what the last verdict of a previous run set aside: that run's undo.
    QDir(QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp"))).removeRecursively();
    QDir(undoDir()).removeRecursively();
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
    m_replacePool.waitForDone();
    m_verdictPool.waitForDone();
    if (m_previewStop)
        *m_previewStop = true;
    m_previewPool.waitForDone();
    m_judgePool.clear();
    m_judgePool.waitForDone();
    m_adviceLeft = 0;
    m_replacing = false;
    m_pending.clear();
    m_active = 0;
    m_scanning = false;
    m_upgrading = false;
    // The last verdict stands once the app closes.
    std::unique_ptr<Undo> undo;
    {
        QMutexLocker lock(&m_undoMutex);
        undo = std::move(m_undo);
    }
    if (undo) {
        dropUndo(std::move(undo));
        emit undoChanged();
    }
}

// ---- Taking the last verdict back -------------------------------------------

QString ImportManager::undoDir() const
{
    return QDir(dataDir(settings().mvDir)).filePath(QStringLiteral("undo"));
}

bool ImportManager::undoAvailable() const
{
    QMutexLocker lock(&m_undoMutex);
    return m_undo != nullptr;
}

QString ImportManager::undoLabel() const
{
    QMutexLocker lock(&m_undoMutex);
    return m_undo ? m_undo->label : QString();
}

void ImportManager::beginUndo(const QString &verdict, const QString &label)
{
    m_recording = std::make_unique<Undo>();
    m_recording->verdict = verdict;
    m_recording->label = label;
    m_recordingThread = QThread::currentThread();
}

bool ImportManager::recording() const
{
    return m_recording && QThread::currentThread() == m_recordingThread;
}

void ImportManager::recordVideo(const VideoInfo &v)
{
    for (const VideoInfo &had : std::as_const(m_recording->videos)) {
        if (had.id == v.id)
            return;
    }
    m_recording->videos << v;
    const QVector<Database::CandidateRow> rows = m_db->candidateRows(v.id);
    m_recording->candidates.insert(v.id, rows);
    for (const Database::CandidateRow &r : rows) {
        if (m_recording->trackKeys.contains(r.trackKey))
            continue;
        m_recording->trackKeys.insert(r.trackKey);
        m_recording->tracks << m_db->trackStates(r.trackKey);
    }
}

void ImportManager::setAside(const QString &path, bool copy)
{
    if (!m_recording || path.isEmpty() || !QFile::exists(path))
        return;
    const QString dir = undoDir();
    QDir().mkpath(dir);
    const QString aside = QDir(dir).filePath(QStringLiteral("%1-%2").arg(m_recording->files.size()).arg(QFileInfo(path).fileName()));
    std::error_code ec;
    if (copy) {
        // The file is about to be rewritten (in a new file, moved over this
        // one): a hard link keeps this one as it is.
        std::filesystem::create_hard_link(std::filesystem::path(path.toStdString()), std::filesystem::path(aside.toStdString()), ec);
        if (ec && !QFile::copy(path, aside))
            return;
    } else {
        std::filesystem::rename(std::filesystem::path(path.toStdString()), std::filesystem::path(aside.toStdString()), ec);
        if (ec && !(QFile::copy(path, aside) && QFile::remove(path)))
            return;
    }
    m_recording->files.insert(path, aside);
}

void ImportManager::endUndo()
{
    if (!m_recording)
        return;
    std::unique_ptr<Undo> done = std::move(m_recording);
    m_recordingThread = nullptr;
    if (!done->changed) {
        dropUndo(std::move(done));
        return;
    }
    // The verdict before it is past taking back.
    std::unique_ptr<Undo> previous;
    {
        QMutexLocker lock(&m_undoMutex);
        previous = std::move(m_undo);
        m_undo = std::move(done);
    }
    dropUndo(std::move(previous));
    emit undoChanged();
}

void ImportManager::dropUndo(std::unique_ptr<Undo> undo)
{
    if (!undo)
        return;
    for (const QString &aside : std::as_const(undo->files))
        QFile::remove(aside);
    QDir().rmdir(undoDir());
}

void ImportManager::undoVerdict()
{
    std::shared_ptr<Undo> undo;
    {
        QMutexLocker lock(&m_undoMutex);
        undo = std::move(m_undo);
    }
    if (!undo)
        return;
    emit undoChanged();
    ++m_reviewJobs;
    emit activityChanged();
    m_verdictPool.start([this, undo] {
        undoJob(*undo);
        QMetaObject::invokeMethod(this, [this] {
            // Counted down on the main thread, after whatever the job
            // queued there (a re-lookup) has landed: a scan ending in
            // between would otherwise find nothing busy, and a headless
            // run would end with the track still pending.
            --m_reviewJobs;
            emit activityChanged();
            pump();
        }, Qt::QueuedConnection);
    });
}

void ImportManager::undoJob(const Undo &undo)
{
    // Each video as it was, in the order they were recorded: the one judged
    // first, then the ones deleted with it.
    QStringList claimed;
    const auto release = qScopeGuard([&] {
        for (const QString &ytId : std::as_const(claimed))
            releaseVideo(ytId);
    });
    for (const VideoInfo &v : undo.videos) {
        if (!claimVideo(v.ytId))
            return;
        claimed << v.ytId;
    }
    QVector<qint64> added, changed;
    QJsonArray videos;
    {
        const Database::Transaction atOnce(m_db);
        for (const VideoInfo &v : undo.videos) {
            const bool existed = m_db->video(v.id).has_value();
            if (!m_db->restoreVideo(v)) {
                qWarning().noquote() << "[review] cannot put back" << v.ytTitle << ": another video has its YouTube id";
                continue;
            }
            m_db->restoreCandidates(v.id, undo.candidates.value(v.id));
            (existed ? changed : added) << v.id;
            videos.append(QJsonObject{{QStringLiteral("videoId"), v.id}, {QStringLiteral("id"), v.ytId}, {QStringLiteral("title"), v.ytTitle},
                                      {QStringLiteral("deleted"), !existed}});
        }
        for (const Database::TrackState &t : undo.tracks)
            m_db->restoreTrackState(t);
        for (const auto &r : undo.rejected)
            m_db->unrejectVideoFor(r.first, r.second);
    }
    for (auto it = undo.files.constBegin(); it != undo.files.constEnd(); ++it) {
        QDir().mkpath(QFileInfo(it.key()).absolutePath());
        std::error_code ec;
        std::filesystem::rename(std::filesystem::path(it.value().toStdString()), std::filesystem::path(it.key().toStdString()), ec);
        if (ec)
            qWarning().noquote() << "[review] cannot put back" << it.key() << ":" << QString::fromStdString(ec.message());
    }
    QDir().rmdir(undoDir());
    qInfo().noquote() << QStringLiteral("[review] taken back: %1").arg(undo.label);
    appendLog({{QStringLiteral("event"), QStringLiteral("undo")}, {QStringLiteral("verdict"), undo.verdict}, {QStringLiteral("videos"), videos}});
    for (qint64 id : std::as_const(added))
        emit videoAdded(id);
    for (qint64 id : std::as_const(changed))
        emit videoChanged(id);
}

void ImportManager::rescan()
{
    if (!m_started)
        return;
    if (m_scanning) {
        m_rescanWanted = true;
        return;
    }
    const ImportSettings cfg = settings();
    const QStringList roots = cfg.libraryDirs.isEmpty() ? cfg.musicDirs : cfg.libraryDirs;
    const QStringList lookup = cfg.musicDirs;
    m_scanning = true;
    emit activityChanged();
    m_scanPool.start([this, roots, lookup] {
        const LibraryScanner::Result r = LibraryScanner::scan(roots, lookup, *m_db, &m_cancel);
        QMetaObject::invokeMethod(this, [this, r] { onScanFinished(r); }, Qt::QueuedConnection);
    });
}

void ImportManager::writeTags(const QVector<qint64> &trackIds, const QHash<QString, QString> &changes,
                              const std::optional<TagWriter::Picture> &picture)
{
    m_writingTags = true;
    emit activityChanged();
    m_scanPool.start([this, trackIds, changes, picture] {
        QVector<qint64> written;
        QStringList failed;
        QJsonArray paths;
        for (qint64 id : trackIds) {
            const std::optional<TrackInfo> t = m_db->track(id);
            if (!t || t->absent)
                continue;
            QString error;
            if (TagWriter::write(t->path, changes, picture, &error)) {
                written << id;
                paths.append(t->path);
            } else {
                failed << QStringLiteral("%1: %2").arg(QFileInfo(t->path).fileName(), error);
            }
        }
        LibraryScanner::Result r;
        if (!written.isEmpty())
            r = LibraryScanner::refreshTracks(written, *m_db);
        QJsonObject tags;
        for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
            tags.insert(it.key(), it.value());
        appendLog({{QStringLiteral("event"), QStringLiteral("retag")},
                   {QStringLiteral("tags"), tags},
                   {QStringLiteral("picture"), picture ? QJsonValue(picture->mime) : QJsonValue()},
                   {QStringLiteral("files"), paths},
                   {QStringLiteral("failed"), QJsonArray::fromStringList(failed)},
                   {QStringLiteral("videosChanged"), r.videosChanged.size()}});
        QMetaObject::invokeMethod(this, [this, r, written, failed] {
            m_writingTags = false;
            if (m_started) {
                for (qint64 id : r.videosChanged)
                    emit videoChanged(id);
                // A track without a video whose new tags are worth a search (A10).
                for (const TrackInfo &t : m_db->pendingRecordings())
                    enqueue(t.id);
                emit libraryChanged();
            }
            emit tagsWritten(written.size(), failed);
            emit activityChanged();
            if (m_started)
                pump();
        }, Qt::QueuedConnection);
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
    // While paused, a job that was under way when the pause began may still
    // get a request through. Only what runs after the pause can end it.
    if (m_blocked)
        return;
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
        // .1 is the newest of the old ones. Enough of them for the import
        // of a large library to be looked into afterwards.
        QFile::remove(path + QStringLiteral(".%1").arg(kLogGenerations));
        for (int n = kLogGenerations - 1; n >= 1; --n)
            QFile::rename(path + QStringLiteral(".%1").arg(n), path + QStringLiteral(".%1").arg(n + 1));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append))
        f.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
}

bool ImportManager::putLibraryAudioIn(const VideoInfo &video, const TrackInfo &track, const AudioAlign::Result &align,
                                      const ImportSettings &cfg, QString *error, bool review)
{
    const QString workDir = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/audio-%1").arg(video.id));
    QDir(workDir).removeRecursively();
    QDir().mkpath(workDir);
    const auto cleanup = qScopeGuard([&] { QDir(workDir).removeRecursively(); });

    // The picture and YouTube's audio come from the file as it is.
    Muxer::Plan plan;
    plan.videoFile = video.path;
    plan.ytAudioFile = video.path;
    plan.ytAudioStream = video.ytStream;
    plan.workDir = workDir;
    plan.outFile = video.path;
    plan.track = track;
    plan.ytId = video.ytId;
    plan.align = align;
    plan.replaceAudio = true;
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
    v.spans = span.spans;
    v.ytStream = 1;
    m_db->updateVideoMedia(v);
    qInfo().noquote() << (review ? QStringLiteral("[audio] “%1” (%2) could not be confirmed as the track's recording: it waits for your review")
                                       .arg(video.title, video.ytTitle)
                                 : QStringLiteral("[audio] “%1” now plays the library's audio (%2)").arg(video.title, audioDetail));
    emit videoChanged(v.id);
    return true;
}

// The video plays YouTube's audio alone, as this track's: the track is the
// lesser audio, or its own is switched off (F3). Any candidate's audio the
// file carried goes, and the video is out of review.
bool ImportManager::keepYoutubeAudio(const VideoInfo &video, const TrackInfo &track, QString *error)
{
    if (!Muxer::keepAudio(video.path, -1, video.ytStream, track, video.ytId, &m_cancel, error))
        return false;
    VideoInfo v = video;
    v.audioSource = QStringLiteral("youtube");
    v.audioDetail = QStringLiteral("YouTube");
    v.ytStream = 0;
    v.review = false;
    v.spans.clear();
    m_db->updateVideoMedia(v);
    emit videoChanged(v.id);
    return true;
}

void ImportManager::discardVideo(const VideoInfo &v)
{
    m_db->removeVideo(v.id);
    // Deleted by a verdict that can be taken back: set aside instead.
    if (recording()) {
        for (const QString &sub : Subtitles::sidecars(v.path))
            setAside(sub, false);
        setAside(v.path, false);
        if (!v.thumb.isEmpty())
            setAside(v.thumb, false);
    }
    removeVideoFiles(v);
    // An artist folder left empty goes too; never the MV folder itself.
    const QString root = QDir(settings().mvDir).absolutePath();
    const QString dir = QFileInfo(v.path).absolutePath();
    if (dir != root && dir.startsWith(root + QLatin1Char('/')))
        QDir().rmdir(dir);
    emit videoRemoved(v.id);
}

void ImportManager::requeuePending()
{
    QMetaObject::invokeMethod(this, [this] {
        if (!m_started)
            return;
        for (const TrackInfo &t : m_db->pendingRecordings())
            enqueue(t.id);
        pump();
    }, Qt::QueuedConnection);
}

QVector<Database::Candidate> ImportManager::candidatesInOrder(Database *db, const VideoInfo &video)
{
    QVector<Database::Candidate> all = db->candidates(video.id);
    if (all.size() < 2)
        return all;
    // The upload's title says whose it is where it carries one candidate's
    // title and no other's ("Twelve (English Ver.)" against 「十二」).
    int named = -1, count = 0;
    for (int i = 0; i < all.size(); ++i) {
        if (Matcher::namesTitle(all[i].track, video.ytTitle)) {
            named = i;
            ++count;
        }
    }
    if (count == 1)
        all.move(named, 0);
    // The rest, or all of them, by how close each is to the upload.
    std::stable_sort(all.begin() + (count == 1 ? 1 : 0), all.end(),
                     [](const Database::Candidate &a, const Database::Candidate &b) { return a.fit > b.fit; });
    return all;
}

void ImportManager::withdraw(const TrackInfo &track, const QVector<VideoInfo> &from)
{
    for (const VideoInfo &was : from) {
        if (!claimVideo(was.ytId))
            return; // cancelled
        const auto release = qScopeGuard([&] { releaseVideo(was.ytId); });
        const std::optional<VideoInfo> v = m_db->video(was.id);
        if (!v || !v->review)
            continue;
        if (recording())
            recordVideo(*v);
        m_db->removeCandidate(v->id, track);
        const bool last = m_db->candidateCount(v->id) == 0;
        appendLog({{QStringLiteral("event"), QStringLiteral("withdrawn")}, {QStringLiteral("videoId"), v->id},
                   {QStringLiteral("id"), v->ytId}, {QStringLiteral("title"), v->ytTitle},
                   {QStringLiteral("trackId"), track.id}, {QStringLiteral("track"), track.title},
                   {QStringLiteral("album"), track.album}, {QStringLiteral("deleted"), last}});
        if (last) {
            qInfo().noquote() << QStringLiteral("[review] “%1” leaves the queue: “%2” [%3] was its only candidate")
                                     .arg(v->ytTitle, track.title, track.album);
            discardVideo(*v);
        } else {
            emit videoChanged(v->id);
        }
    }
}

void ImportManager::releaseCandidates(qint64 videoId, const QVector<Database::Candidate> &were, const TrackInfo *keeper, const QString &why)
{
    // (As one write: a track is not seen waiting for review with nothing to wait on.)
    const Database::Transaction atOnce(m_db);
    m_db->clearCandidates(videoId);
    for (const Database::Candidate &c : were) {
        if (keeper && Database::trackKey(c.track) == Database::trackKey(*keeper))
            continue;
        // One that is queued for a lookup keeps its turn; one that has a
        // video by now is settled.
        const std::optional<TrackInfo> now = m_db->track(c.track.id);
        if (now && now->state == QLatin1String("review") && m_db->candidacies(*now).isEmpty())
            m_db->setTrackResult(now->id, QStringLiteral("not_found"), 0, why);
    }
}

bool ImportManager::addCandidate(const VideoInfo &video, const TrackInfo &track, const AudioAlign::Result &align,
                                 const ImportSettings &cfg, QString *error)
{
    int stream = 0;
    QString detail;
    QVector<AudioSpan> spans;
    if (cfg.replaceAudio && libraryIsBetter(track, storedYoutubeQuality(video))) {
        const AudioAlign::Result placed = placedWhole(align);
        if (placed.byOffset) {
            const QString workDir = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/audio-%1").arg(video.id));
            QDir(workDir).removeRecursively();
            QDir().mkpath(workDir);
            const auto cleanup = qScopeGuard([&] { QDir(workDir).removeRecursively(); });
            Muxer::Plan plan;
            plan.videoFile = video.path;
            plan.ytAudioFile = video.path;
            plan.ytAudioStream = video.ytStream;
            plan.workDir = workDir;
            plan.outFile = video.path;
            plan.track = track;
            plan.ytId = video.ytId;
            plan.align = placed;
            plan.replaceAudio = true;
            Muxer::ReviewSpan span;
            if (!Muxer::addLibraryStream(plan, &m_cancel, &detail, error, &span))
                return false;
            spans = span.spans;
            VideoInfo v = video;
            stream = video.ytStream + 1;
            v.ytStream = video.ytStream + 1;
            if (video.ytStream == 0) {
                v.audioSource = QStringLiteral("library");
                v.audioDetail = detail;
                v.spans = spans;
            }
            m_db->updateVideoMedia(v);
        }
    }
    // (A fit of nothing at all still counts as measured.)
    m_db->addCandidate(video.id, track, stream, qMax(align.likeness, 1e-6), detail, spans);
    emit videoChanged(video.id);
    return true;
}

bool ImportManager::measureCandidates(qint64 videoId, const ImportSettings &cfg)
{
    std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v || !v->review)
        return false;
    if (!claimVideo(v->ytId))
        return false;
    const auto release = qScopeGuard([&, id = v->ytId] { releaseVideo(id); });
    v = m_db->video(videoId);
    if (!v || !v->review)
        return false;
    std::vector<int16_t> ytPcm;
    QString error;
    bool decoded = false;
    for (const Database::Candidate &c : m_db->candidates(videoId)) {
        if (c.fit > 0 && (c.stream > 0 || !(cfg.replaceAudio && libraryIsBetter(c.track, storedYoutubeQuality(*v)))))
            continue;
        if (!decoded && !AudioAlign::decodeMono(v->path, &ytPcm, &m_cancel, &error, v->ytStream))
            return false;
        decoded = true;
        std::vector<int16_t> trackPcm;
        if (!AudioAlign::decodeMono(c.track.path, &trackPcm, &m_cancel, &error)) {
            if (m_cancel)
                return false;
            continue;
        }
        const AudioAlign::Result ar = AudioAlign::align(trackPcm, ytPcm);
        if (m_cancel)
            return false;
        if (c.stream > 0) {
            m_db->addCandidate(videoId, c.track, c.stream, qMax(ar.likeness, 1e-6), c.audioDetail);
        } else if (!addCandidate(*v, c.track, ar, cfg, &error)) {
            if (!m_cancel)
                qWarning().noquote() << "[review] could not give" << v->ytTitle << "the audio of" << c.track.title << ":" << error;
            return false;
        }
        v = m_db->video(videoId); // (its streams may have changed)
        if (!v)
            return false;
    }
    emit videoChanged(videoId);
    return true;
}

void ImportManager::prepareCandidates(const ImportSettings &cfg)
{
    // Only where there is something to tell apart: a video with one
    // candidate is that track's to accept, whatever it measures.
    for (const VideoInfo &v : m_db->allVideos()) {
        if (m_cancel)
            return;
        if (!v.review)
            continue;
        const QVector<Database::Candidate> all = m_db->candidates(v.id);
        if (all.size() < 2 || std::none_of(all.begin(), all.end(), [](const Database::Candidate &c) { return c.fit <= 0; }))
            continue;
        measureCandidates(v.id, cfg);
    }
}

void ImportManager::approveVideo(qint64 videoId, qint64 trackId, bool byJudge)
{
    ++m_reviewJobs;
    emit activityChanged();
    const ImportSettings cfg = settings();
    const bool undoable = m_keepsUndo;
    m_verdictPool.start([this, videoId, trackId, cfg, undoable, byJudge] {
        approveJob(videoId, trackId, cfg, false, undoable, byJudge);
        QMetaObject::invokeMethod(this, [this] {
            // Counted down on the main thread, after whatever the job
            // queued there (a re-lookup) has landed: a scan ending in
            // between would otherwise find nothing busy, and a headless
            // run would end with the track still pending.
            --m_reviewJobs;
            emit activityChanged();
            pump();
        }, Qt::QueuedConnection);
    });
}

void ImportManager::rejectVideo(qint64 videoId, bool byJudge)
{
    ++m_reviewJobs;
    emit activityChanged();
    const bool undoable = m_keepsUndo;
    m_verdictPool.start([this, videoId, undoable, byJudge] {
        rejectJob(videoId, QString(), undoable, byJudge);
        QMetaObject::invokeMethod(this, [this] {
            // Counted down on the main thread, after whatever the job
            // queued there (a re-lookup) has landed: a scan ending in
            // between would otherwise find nothing busy, and a headless
            // run would end with the track still pending.
            --m_reviewJobs;
            emit activityChanged();
            pump();
        }, Qt::QueuedConnection);
    });
}

void ImportManager::approveJob(qint64 videoId, qint64 trackId, const ImportSettings &cfg, bool byChannel, bool undoable, bool byJudge)
{
    std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v || !v->review)
        return;
    const QString ytId = v->ytId;
    if (!claimVideo(ytId))
        return;
    bool holding = true;
    const auto release = qScopeGuard([&] {
        if (holding)
            releaseVideo(ytId);
    });
    v = m_db->video(videoId);
    if (!v || !v->review)
        return;
    // Kept so that it can be taken back: the rows as they are, and the file
    // as it is before the other candidates' audio is taken out of it.
    const auto keep = qScopeGuard([&] { endUndo(); });
    if (undoable) {
        beginUndo(QStringLiteral("accept"), QStringLiteral("Accepted “%1”").arg(v->ytTitle));
        recordVideo(*v);
        setAside(v->path, true);
    }

    // Whose video it is: the candidate named, or the one shown first.
    QVector<Database::Candidate> candidates = candidatesInOrder(m_db, *v);
    const std::optional<TrackInfo> named = trackId > 0 ? m_db->track(trackId) : std::nullopt;
    int mine = candidates.isEmpty() ? -1 : 0;
    if (trackId > 0) {
        mine = -1;
        for (int i = 0; named && i < candidates.size(); ++i) {
            if (Database::trackKey(candidates[i].track) == Database::trackKey(*named))
                mine = i;
        }
        if (mine < 0)
            return; // not a candidate of it (any more)
    }

    if (mine >= 0) {
        // A candidate from before review went by video may be the better
        // audio without having its audio in the file yet.
        if (candidates[mine].stream <= 0 && cfg.replaceAudio && libraryIsBetter(candidates[mine].track, storedYoutubeQuality(*v))) {
            const qint64 key = Database::trackKey(candidates[mine].track);
            releaseVideo(ytId);
            holding = false;
            measureCandidates(videoId, cfg);
            if (m_cancel || !claimVideo(ytId))
                return;
            holding = true;
            v = m_db->video(videoId);
            if (!v || !v->review)
                return;
            candidates = m_db->candidates(videoId);
            mine = -1;
            for (int i = 0; i < candidates.size(); ++i) {
                if (Database::trackKey(candidates[i].track) == key)
                    mine = i;
            }
            if (mine < 0)
                return;
        }
        const Database::Candidate &c = candidates[mine];
        if (m_recording) {
            m_recording->label = QStringLiteral("Accepted “%1” for “%2”").arg(v->ytTitle, c.track.title);
            m_recording->changed = true;
        }
        // The file keeps this track's audio and YouTube's, and no other
        // candidate's.
        QString error;
        if (!Muxer::keepAudio(v->path, c.stream - 1, v->ytStream, c.track, ytId, &m_cancel, &error)) {
            if (!m_cancel)
                qWarning().noquote() << "[review] could not accept" << v->ytTitle << ":" << error;
            return;
        }
        v->title = c.track.title;
        v->artist = c.track.artist;
        v->albumArtist = c.track.albumArtist;
        v->album = c.track.album;
        v->genre = c.track.genre;
        v->year = c.track.year;
        v->trackNo = c.track.trackNo;
        v->tags = c.track.tags;
        m_db->updateVideoTags(*v);
        if (c.stream > 0) {
            v->audioSource = QStringLiteral("library");
            if (!c.audioDetail.isEmpty())
                v->audioDetail = c.audioDetail;
            v->ytStream = 1;
            // Where this track sits in the video, where that was kept with
            // the candidate; else the first candidate's stays, near enough.
            if (!c.spans.isEmpty())
                v->spans = c.spans;
        } else {
            v->audioSource = QStringLiteral("youtube");
            v->audioDetail = QStringLiteral("YouTube");
            v->ytStream = 0;
            v->spans.clear();
        }
    }
    v->review = false;
    if (m_recording)
        m_recording->changed = true;
    // The user's verdict vouches for this upload as the track's video, so
    // that a re-import does not ask again whose channel it is on. (Accepted
    // because its channel was allowed, the list is what says so.)
    v->vouched = !byChannel;
    m_db->updateVideoMedia(*v);
    const QString how = byChannel ? QStringLiteral("“%1”, on a channel you allowed").arg(v->ytTitle)
                                  : QStringLiteral("you accepted “%1”").arg(v->ytTitle);
    QJsonArray others;
    for (int i = 0; i < candidates.size(); ++i) {
        if (i != mine) {
            others.append(QJsonObject{{QStringLiteral("trackId"), candidates[i].track.id}, {QStringLiteral("track"), candidates[i].track.title},
                                      {QStringLiteral("album"), candidates[i].track.album}});
        }
    }
    QJsonArray tracks;
    std::optional<TrackInfo> keeper;
    if (mine >= 0) {
        keeper = candidates[mine].track;
        m_db->setTrackResult(keeper->id, QStringLiteral("done"), videoId, how);
        tracks.append(QJsonObject{{QStringLiteral("trackId"), keeper->id}, {QStringLiteral("track"), keeper->title}, {QStringLiteral("album"), keeper->album}});
    }
    releaseCandidates(videoId, candidates, keeper ? &*keeper : nullptr,
                      !keeper ? how
                      : (byChannel ? QStringLiteral("“%1” is the video of “%2” [%3]: its channel is one you allowed")
                         : byJudge ? QStringLiteral("the judge accepted “%1” for “%2” [%3] on your behalf")
                                   : QStringLiteral("you accepted “%1” for “%2” [%3]")).arg(v->ytTitle, keeper->title, keeper->album));
    qInfo().noquote() << (byChannel ? QStringLiteral("[channel] “%1” (%2) is accepted: its channel is one you allowed")
                          : byJudge ? QStringLiteral("[judge] accepted “%1” (%2) on your behalf")
                                    : QStringLiteral("[review] accepted “%1” (%2)")).arg(v->title, v->ytTitle);
    appendLog({{QStringLiteral("event"), QStringLiteral("verdict")}, {QStringLiteral("verdict"), QStringLiteral("accept")},
               {QStringLiteral("by"), byChannel ? QStringLiteral("allowed-channel") : byJudge ? QStringLiteral("judge") : QStringLiteral("you")},
               {QStringLiteral("videoId"), videoId}, {QStringLiteral("id"), v->ytId}, {QStringLiteral("title"), v->ytTitle},
               {QStringLiteral("channel"), v->ytChannel},
               {QStringLiteral("tracks"), tracks}, {QStringLiteral("otherCandidates"), others}});
    emit videoChanged(videoId);
    // The track has its video: it is a candidate of nothing else.
    releaseVideo(ytId);
    holding = false;
    if (keeper)
        withdraw(*keeper, m_db->candidacies(*keeper));
}

void ImportManager::rejectJob(qint64 videoId, const QString &why, bool undoable, bool byJudge)
{
    std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v || !v->review)
        return;
    const QString ytId = v->ytId;
    if (!claimVideo(ytId))
        return;
    const auto release = qScopeGuard([&] { releaseVideo(ytId); });
    v = m_db->video(videoId);
    if (!v || !v->review)
        return;
    // Kept so that it can be taken back: the rows as they are, and the
    // file, set aside instead of deleted.
    const auto keep = qScopeGuard([&] { endUndo(); });
    if (undoable) {
        beginUndo(QStringLiteral("reject"), QStringLiteral("Turned down “%1”").arg(v->ytTitle));
        recordVideo(*v);
        m_recording->changed = true;
    }
    // This upload is not offered to any of its candidates again. Each waits
    // on what else it is a candidate of; with nothing, it has no video.
    const QVector<Database::Candidate> candidates = m_db->candidates(videoId);
    QJsonArray names;
    for (const Database::Candidate &c : candidates) {
        if (m_recording && !m_db->videoRejectedFor(c.track, v->ytId))
            m_recording->rejected << qMakePair(c.track, v->ytId);
        m_db->rejectVideoFor(c.track, v->ytId);
        names.append(QJsonObject{{QStringLiteral("trackId"), c.track.id}, {QStringLiteral("track"), c.track.title}, {QStringLiteral("album"), c.track.album}});
    }
    releaseCandidates(videoId, candidates, nullptr,
                      !why.isEmpty() ? QStringLiteral("%1: “%2”").arg(why, v->ytTitle)
                      : byJudge ? QStringLiteral("the judge turned down “%1” on your behalf").arg(v->ytTitle)
                                : QStringLiteral("you turned down “%1”").arg(v->ytTitle));
    discardVideo(*v);
    qInfo().noquote() << (!why.isEmpty() ? QStringLiteral("[channel] “%1” (%2) is deleted: its channel is one you denied")
                          : byJudge ? QStringLiteral("[judge] rejected “%1” (%2) on your behalf")
                                    : QStringLiteral("[review] rejected “%1” (%2)")).arg(v->title, v->ytTitle);
    appendLog({{QStringLiteral("event"), QStringLiteral("verdict")}, {QStringLiteral("verdict"), QStringLiteral("reject")},
               {QStringLiteral("by"), !why.isEmpty() ? QStringLiteral("denied-channel") : byJudge ? QStringLiteral("judge") : QStringLiteral("you")},
               {QStringLiteral("videoId"), videoId}, {QStringLiteral("id"), v->ytId}, {QStringLiteral("title"), v->ytTitle},
               {QStringLiteral("channel"), v->ytChannel}, {QStringLiteral("tracks"), names}});
}

ImportManager::ChannelVerdict ImportManager::judgeAsArtists(const VideoInfo &video, const ImportSettings &cfg, const std::atomic<bool> *stop)
{
    ChannelVerdict out;
    // Each candidate against the upload's own audio, which the file keeps.
    std::vector<int16_t> ytPcm;
    QString error;
    if (!AudioAlign::decodeMono(video.path, &ytPcm, stop, &error, video.ytStream)) {
        out.reason = QStringLiteral("its file cannot be read");
        return out;
    }
    double bestFit = -1;
    for (const Database::Candidate &c : candidatesInOrder(m_db, video)) {
        if (stop && *stop)
            return out;
        std::vector<int16_t> pcm;
        if (!AudioAlign::decodeMono(c.track.path, &pcm, stop, &error)) {
            if (out.reason.isEmpty())
                out.reason = QStringLiteral("“%1” cannot be read").arg(c.track.title);
            continue;
        }
        const AudioAlign::Result ar = AudioAlign::align(pcm, ytPcm);
        // As a lookup has it for a video the track is a candidate of
        // already (runJob), with the channel a vouched one.
        const bool recording = audioMatches(ar) && ownRecording(ar, c.track, video.ytTitle, true);
        if (recording) {
            if (ar.likeness > bestFit) {
                bestFit = ar.likeness;
                out.accept = true;
                out.track = c.track;
            }
            continue;
        }
        if (out.reason.isEmpty()) {
            out.reason = !audioMatches(ar) ? mismatchReason(ar)
                                           : unconfirmedReason(ar, c.track, video.ytTitle, video.ytChannel, true);
        }
    }
    if (out.accept)
        out.reason.clear();
    else if (out.reason.isEmpty())
        out.reason = QStringLiteral("it has no candidate track in the music folders");
    return out;
}

void ImportManager::askJudge(qint64 videoId, qint64 trackId, bool again)
{
    const ImportSettings cfg = settings();
    if (cfg.judgeMode == QLatin1String("off"))
        return;
    emit adviceQueueChanged(++m_adviceLeft);
    m_judgePool.start([this, videoId, trackId, again, cfg] {
        adviceJob(videoId, trackId, again, cfg);
        emit adviceQueueChanged(--m_adviceLeft);
    });
}

void ImportManager::askJudgeAll()
{
    if (settings().judgeMode != QLatin1String("advice"))
        return;
    for (const VideoInfo &v : m_db->videosUnderReview()) {
        const QVector<Database::Advice> have = m_db->advice(v.id);
        for (const Database::Candidate &c : candidatesInOrder(m_db, v)) {
            const qint64 key = Database::trackKey(c.track);
            if (std::any_of(have.cbegin(), have.cend(), [&](const Database::Advice &a) { return a.trackKey == key; }))
                continue; // answered already
            askJudge(v.id, c.track.id, false);
        }
    }
}

void ImportManager::adviceJob(qint64 videoId, qint64 trackId, bool again, const ImportSettings &cfg)
{
    // Advice mode (X2): the one question of V, about this video for this
    // candidate, from the file and what the library kept of the upload.
    const std::optional<VideoInfo> video = m_db->video(videoId);
    const std::optional<TrackInfo> track = m_db->track(trackId);
    if (!video || !video->review || !track || m_cancel)
        return;
    const qint64 key = Database::trackKey(*track);
    if (!again) {
        for (const Database::Advice &a : m_db->advice(videoId)) {
            if (a.trackKey == key)
                return; // asked already
        }
    }
    const QString apiKey = cfg.judgeProvider == QLatin1String("claude") ? Judge::readKey(cfg.judgeKeyFile) : QString();
    const QString model = cfg.judgeProvider == QLatin1String("chatgpt") ? cfg.judgeModel : Judge::modelId(cfg.judgeModel);
    Judge::Upload u;
    u.id = video->ytId;
    u.title = video->ytTitle;
    u.channel = video->ytChannel;
    u.handle = video->ytChannelHandle;
    u.verified = video->ytChannelVerified;
    u.subscribers = video->ytChannelSubs;
    u.date = video->ytDate;
    u.description = video->ytDescription;
    u.allowed = !video->ytChannelId.isEmpty() && m_db->channelAllowed(video->ytChannelId);
    u.duration = video->duration;
    u.width = video->width;
    u.height = video->height;
    std::vector<int16_t> ytPcm, pcm;
    QString error;
    if (AudioAlign::decodeMono(video->path, &ytPcm, &m_cancel, &error, video->ytStream) && AudioAlign::decodeMono(track->path, &pcm, &m_cancel, &error))
        u.audio = Judge::audioText(AudioAlign::align(pcm, ytPcm));
    else
        u.audio = QStringLiteral("not measured (%1)").arg(error);
    if (m_cancel)
        return;
    const QString work = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/advice-%1").arg(videoId));
    u.picture = Judge::frames(video->path, video->duration, work, &m_cancel);
    QVector<TrackInfo> others;
    for (const Database::Candidate &c : m_db->candidates(videoId)) {
        if (Database::trackKey(c.track) != key)
            others.append(c.track);
    }
    const Judge::Answer a = Judge::ask(apiKey, model, QString::fromUtf8(JudgeWords::kVerdictWords), Judge::verdictContent(*track, others, {u}),
                                       QString::fromUtf8(JudgeWords::kVerdictSchema), 4000, cacheDir(cfg.mvDir), &m_cancel, cfg.judgeProvider, cfg.chatGptAuthFile, cfg.chatGptAccount);
    QDir(work).removeRecursively();
    if (m_cancel)
        return;
    appendLog({{QStringLiteral("event"), QStringLiteral("judge")}, {QStringLiteral("question"), QStringLiteral("advice")},
               {QStringLiteral("model"), a.model}, {QStringLiteral("videoId"), videoId}, {QStringLiteral("id"), video->ytId},
               {QStringLiteral("title"), video->ytTitle}, {QStringLiteral("trackId"), track->id}, {QStringLiteral("track"), track->title},
               {QStringLiteral("artist"), track->artist}, {QStringLiteral("album"), track->album},
               {QStringLiteral("answer"), a.json}, {QStringLiteral("error"), a.error}, {QStringLiteral("usage"), a.usage},
               {QStringLiteral("provider"), a.provider},
               {QStringLiteral("cost"), a.provider == QLatin1String("chatgpt") ? QJsonValue() : QJsonValue(a.cost)}, {QStringLiteral("seconds"), a.seconds}, {QStringLiteral("cached"), a.cached}});
    Database::Advice ad;
    ad.videoId = videoId;
    ad.trackKey = key;
    ad.mode = QStringLiteral("advice");
    ad.model = model;
    ad.askedAt = QDateTime::currentSecsSinceEpoch();
    if (!a.error.isEmpty()) {
        qWarning().noquote() << "[judge] no advice on" << video->ytTitle << ":" << a.error;
        ad.kind = QStringLiteral("error");
        ad.note = a.error;
    } else {
        const QJsonObject o = Judge::optionAt(a.json, 1);
        ad.keep = o.value(QLatin1String("official_mv")).toBool();
        ad.probability = o.value(QLatin1String("confidence")).toDouble();
        ad.kind = o.value(QLatin1String("kind")).toString();
        ad.source = o.value(QLatin1String("source")).toString();
        ad.version = o.value(QLatin1String("version")).toString();
        ad.whole = o.value(QLatin1String("whole")).toString();
        ad.note = o.value(QLatin1String("note")).toString();
        qInfo().noquote() << QStringLiteral("[judge] advice on “%1” for “%2”: %3 %4").arg(video->ytTitle, track->title, ad.keep ? QStringLiteral("keep") : QStringLiteral("not this one")).arg(ad.probability, 0, 'f', 2);
    }
    m_db->setAdvice(ad);
    emit adviceChanged(videoId);
}

int ImportManager::previewAllowChannel(const QString &channelId)
{
    cancelChannelPreview();
    const int ticket = ++m_previewTicket;
    const auto stop = std::make_shared<std::atomic<bool>>(false);
    m_previewStop = stop;
    const ImportSettings cfg = settings();
    QVector<VideoInfo> waiting;
    for (const VideoInfo &v : m_db->videosOfChannel(channelId)) {
        if (v.review)
            waiting << v;
    }
    const auto left = std::make_shared<std::atomic<int>>(waiting.size());
    if (waiting.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, ticket] { emit channelPreviewDone(ticket); }, Qt::QueuedConnection);
        return ticket;
    }
    for (const VideoInfo &was : std::as_const(waiting)) {
        m_previewPool.start([this, was, cfg, ticket, stop, left] {
            const auto done = qScopeGuard([&] {
                if (--*left == 0 && !*stop)
                    emit channelPreviewDone(ticket);
            });
            if (*stop || m_cancel || !claimVideo(was.ytId))
                return;
            const auto release = qScopeGuard([&] { releaseVideo(was.ytId); });
            const std::optional<VideoInfo> v = m_db->video(was.id);
            if (!v || !v->review || *stop)
                return;
            const ChannelVerdict verdict = judgeAsArtists(*v, cfg, stop.get());
            if (!*stop && !m_cancel)
                emit channelJudged(ticket, v->id, verdict.accept, verdict.track.id, verdict.track.title, verdict.reason);
        });
    }
    return ticket;
}

void ImportManager::cancelChannelPreview()
{
    if (m_previewStop)
        *m_previewStop = true;
    m_previewStop.reset();
}

void ImportManager::allowChannel(const QString &channelId, const QString &name, const QString &handle)
{
    if (channelId.isEmpty())
        return;
    cancelChannelPreview();
    m_db->setChannelList(channelId, QStringLiteral("allow"), name, handle);
    qInfo().noquote() << QStringLiteral("[channel] allowed: “%1” [%2]").arg(name, channelId);
    appendLog({{QStringLiteral("event"), QStringLiteral("channel")}, {QStringLiteral("list"), QStringLiteral("allow")},
               {QStringLiteral("channelId"), channelId}, {QStringLiteral("channel"), name}, {QStringLiteral("handle"), handle}});
    emit channelListsChanged();
    ++m_reviewJobs;
    emit activityChanged();
    const ImportSettings cfg = settings();
    m_verdictPool.start([this, channelId, cfg] {
        allowJob(channelId, cfg);
        QMetaObject::invokeMethod(this, [this] {
            // Counted down on the main thread, after whatever the job
            // queued there (a re-lookup) has landed: a scan ending in
            // between would otherwise find nothing busy, and a headless
            // run would end with the track still pending.
            --m_reviewJobs;
            emit activityChanged();
            pump();
        }, Qt::QueuedConnection);
    });
}

void ImportManager::allowJob(const QString &channelId, const ImportSettings &cfg)
{
    for (const VideoInfo &was : m_db->videosOfChannel(channelId)) {
        if (m_cancel)
            return;
        if (!was.review)
            continue;
        ChannelVerdict verdict;
        {
            if (!claimVideo(was.ytId))
                return;
            const auto release = qScopeGuard([&] { releaseVideo(was.ytId); });
            const std::optional<VideoInfo> v = m_db->video(was.id);
            if (!v || !v->review)
                continue;
            verdict = judgeAsArtists(*v, cfg, &m_cancel);
        }
        if (m_cancel)
            return;
        if (verdict.accept) {
            approveJob(was.id, verdict.track.id, cfg, true);
        } else {
            qInfo().noquote() << QStringLiteral("[channel] “%1” stays under review: %2").arg(was.ytTitle, verdict.reason);
            appendLog({{QStringLiteral("event"), QStringLiteral("channel-kept-in-review")}, {QStringLiteral("videoId"), was.id},
                       {QStringLiteral("id"), was.ytId}, {QStringLiteral("title"), was.ytTitle}, {QStringLiteral("reason"), verdict.reason}});
        }
        emit channelJudged(0, was.id, verdict.accept, verdict.track.id, verdict.track.title, verdict.reason);
    }
}

void ImportManager::denyChannel(const QString &channelId, const QString &name, const QString &handle)
{
    if (channelId.isEmpty())
        return;
    cancelChannelPreview();
    m_db->setChannelList(channelId, QStringLiteral("deny"), name, handle);
    qInfo().noquote() << QStringLiteral("[channel] denied: “%1” [%2]").arg(name, channelId);
    appendLog({{QStringLiteral("event"), QStringLiteral("channel")}, {QStringLiteral("list"), QStringLiteral("deny")},
               {QStringLiteral("channelId"), channelId}, {QStringLiteral("channel"), name}, {QStringLiteral("handle"), handle}});
    emit channelListsChanged();
    ++m_reviewJobs;
    emit activityChanged();
    m_verdictPool.start([this, channelId] {
        denyJob(channelId);
        QMetaObject::invokeMethod(this, [this] {
            // Counted down on the main thread, after whatever the job
            // queued there (a re-lookup) has landed: a scan ending in
            // between would otherwise find nothing busy, and a headless
            // run would end with the track still pending.
            --m_reviewJobs;
            emit activityChanged();
            pump();
        }, Qt::QueuedConnection);
    });
}

void ImportManager::denyJob(const QString &channelId)
{
    bool requeued = false;
    for (const VideoInfo &was : m_db->videosOfChannel(channelId)) {
        if (m_cancel)
            return;
        if (was.review) {
            // Rejected, for all its candidates.
            rejectJob(was.id, QStringLiteral("its channel is one you denied"));
            continue;
        }
        // A video that tracks have: it goes, and they are looked up again,
        // to see whether another upload is theirs.
        if (!claimVideo(was.ytId))
            return;
        const auto release = qScopeGuard([&] { releaseVideo(was.ytId); });
        const std::optional<VideoInfo> v = m_db->video(was.id);
        if (!v)
            continue;
        if (v->review) {
            releaseVideo(was.ytId);
            rejectJob(was.id, QStringLiteral("its channel is one you denied"));
            if (!claimVideo(was.ytId))
                return;
            continue;
        }
        QJsonArray names;
        for (const TrackInfo &t : m_db->tracksForVideo(v->id))
            names.append(QJsonObject{{QStringLiteral("trackId"), t.id}, {QStringLiteral("track"), t.title}, {QStringLiteral("album"), t.album}});
        m_db->removeAbsentTracksOf(v->id);
        m_db->unlinkAll(v->id);
        discardVideo(*v);
        requeued = requeued || !names.isEmpty();
        qInfo().noquote() << QStringLiteral("[channel] “%1” (%2) is deleted: its channel is one you denied").arg(v->title, v->ytTitle);
        appendLog({{QStringLiteral("event"), QStringLiteral("denied-video")}, {QStringLiteral("videoId"), v->id},
                   {QStringLiteral("id"), v->ytId}, {QStringLiteral("title"), v->ytTitle}, {QStringLiteral("channel"), v->ytChannel},
                   {QStringLiteral("tracks"), names}});
    }
    if (requeued)
        requeuePending();
}

void ImportManager::unlistChannel(const QString &channelId)
{
    m_db->removeChannelList(channelId);
    qInfo().noquote() << QStringLiteral("[channel] taken off the lists: %1").arg(channelId);
    appendLog({{QStringLiteral("event"), QStringLiteral("channel")}, {QStringLiteral("list"), QStringLiteral("none")},
               {QStringLiteral("channelId"), channelId}});
    emit channelListsChanged();
}

bool ImportManager::settleShared(qint64 videoId, const ImportSettings &cfg)
{
    std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v || v->review)
        return false;
    const auto recordings = [this](qint64 id) {
        QHash<qint64, TrackInfo> best; // one file per recording: its best
        for (const TrackInfo &t : m_db->tracksForVideo(id)) {
            const auto it = best.find(Database::trackKey(t));
            if (it == best.end())
                best.insert(Database::trackKey(t), t);
            else if (betterSource(t, *it))
                *it = t;
        }
        return best;
    };
    if (recordings(videoId).size() < 2)
        return false;
    if (!claimVideo(v->ytId))
        return false;
    const auto release = qScopeGuard([&, id = v->ytId] { releaseVideo(id); });
    v = m_db->video(videoId);
    const QHash<qint64, TrackInfo> holders = recordings(videoId);
    if (!v || v->review || holders.size() < 2)
        return false;

    // Each against the upload's own audio, which the file keeps.
    std::vector<int16_t> ytPcm;
    QString error;
    if (!AudioAlign::decodeMono(v->path, &ytPcm, &m_cancel, &error, v->ytStream)) {
        if (!m_cancel)
            qWarning().noquote() << "[import] cannot settle whose" << v->ytTitle << "is:" << error;
        return false;
    }
    std::optional<TrackInfo> keeper;
    AudioAlign::Result keeperAlign;
    QJsonArray measured;
    QList<TrackInfo> order = holders.values();
    std::sort(order.begin(), order.end(), [](const TrackInfo &a, const TrackInfo &b) { return a.id < b.id; });
    for (const TrackInfo &t : std::as_const(order)) {
        std::vector<int16_t> pcm;
        AudioAlign::Result ar;
        if (AudioAlign::decodeMono(t.path, &pcm, &m_cancel, &error))
            ar = AudioAlign::align(pcm, ytPcm);
        if (m_cancel)
            return false;
        measured.append(QJsonObject{{QStringLiteral("trackId"), t.id}, {QStringLiteral("track"), t.title}, {QStringLiteral("album"), t.album},
                                    {QStringLiteral("fit"), std::round(ar.likeness * 1000) / 1000}});
        if (!keeper || ar.likeness > keeperAlign.likeness) {
            keeper = t;
            keeperAlign = ar;
        }
    }
    // The video plays its track's audio, or YouTube's: whichever of the
    // tracks had theirs in it until now.
    const bool own = cfg.replaceAudio && libraryIsBetter(*keeper, storedYoutubeQuality(*v)) && audioMatches(keeperAlign) && audioReplaceable(keeperAlign);
    if (own) {
        if (!putLibraryAudioIn(*v, *keeper, keeperAlign, cfg, &error))
            return false;
    } else if (v->audioSource == QLatin1String("library")) {
        if (!Muxer::keepAudio(v->path, -1, v->ytStream, *keeper, v->ytId, &m_cancel, &error))
            return false;
        v->audioSource = QStringLiteral("youtube");
        v->audioDetail = QStringLiteral("YouTube");
        v->ytStream = 0;
        v->spans.clear();
        m_db->updateVideoMedia(*v);
    }
    if (auto now = m_db->video(videoId)) {
        now->title = keeper->title;
        now->artist = keeper->artist;
        now->albumArtist = keeper->albumArtist;
        now->album = keeper->album;
        now->genre = keeper->genre;
        now->year = keeper->year;
        now->trackNo = keeper->trackNo;
        now->tags = keeper->tags;
        m_db->updateVideoTags(*now);
    }
    QStringList names;
    for (const TrackInfo &t : m_db->unlinkOthers(videoId, *keeper)) {
        // They are looked up as tracks without a video.
        m_db->setTrackReimport(t.id);
        const QString name = QStringLiteral("“%1” [%2]").arg(t.title, t.album);
        if (!names.contains(name))
            names << name;
    }
    qInfo().noquote() << QStringLiteral("[import] “%1” is the video of “%2” [%3] alone now, the closest fit to it (%4%); back in the queue: %5")
                             .arg(v->ytTitle, keeper->title, keeper->album).arg(100 * keeperAlign.likeness, 0, 'f', 1).arg(names.join(QStringLiteral(", ")));
    appendLog({{QStringLiteral("event"), QStringLiteral("shared-settled")}, {QStringLiteral("videoId"), videoId},
               {QStringLiteral("id"), v->ytId}, {QStringLiteral("title"), v->ytTitle},
               {QStringLiteral("keeps"), keeper->id}, {QStringLiteral("measured"), measured}});
    emit videoChanged(videoId);
    requeuePending();
    return true;
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

QSet<qint64> ImportManager::untrackedIds() const
{
    return m_db->untrackedVideoIds();
}

void ImportManager::removeUntracked(const VideoInfo &v)
{
    m_db->removeAbsentTracksOf(v.id);
    m_db->removeVideo(v.id);
    removeVideoFiles(v);
    // An artist folder left empty goes too; never the MV folder itself.
    const QString root = QDir(settings().mvDir).absolutePath();
    const QString dir = QFileInfo(v.path).absolutePath();
    if (dir != root && dir.startsWith(root + QLatin1Char('/')))
        QDir().rmdir(dir);
    emit videoRemoved(v.id);
}

bool ImportManager::dropBrokenVideos(const TrackInfo &track)
{
    // A machine that goes down before a new file's data is written out can
    // leave the file empty. Such a video has nothing a re-import could judge
    // and nothing to watch: it leaves the library, and its upload is found
    // and brought in again like any other.
    QVector<VideoInfo> videos = m_db->candidacies(track);
    if (const std::optional<VideoInfo> own = track.videoId > 0 ? m_db->video(track.videoId) : std::nullopt)
        videos << *own;
    QVector<qint64> orphaned;
    bool dropped = false;
    for (const VideoInfo &v : std::as_const(videos)) {
        if (QFileInfo(v.path).size() > 0)
            continue;
        if (!claimVideo(v.ytId))
            break; // cancelled
        const bool missing = !QFile::exists(v.path);
        // Whoever else had it, or was a candidate of it, looks again.
        QVector<TrackInfo> others = m_db->tracksForVideo(v.id);
        for (const Database::Candidate &c : m_db->candidates(v.id))
            others << c.track;
        m_db->removeVideo(v.id);
        removeVideoFiles(v);
        releaseVideo(v.ytId);
        dropped = true;
        for (const TrackInfo &t : std::as_const(others)) {
            if (Database::trackKey(t) != Database::trackKey(track) && !orphaned.contains(t.id)
                && (!v.review || m_db->candidacies(t).isEmpty()))
                orphaned << t.id;
        }
        qInfo().noquote() << QStringLiteral("[import] “%1” [%2]: deleted “%3”, whose file is %4")
                                 .arg(track.title, track.album, v.ytTitle, missing ? QStringLiteral("missing") : QStringLiteral("empty"));
        appendLog({{QStringLiteral("event"), QStringLiteral("broken-video")}, {QStringLiteral("videoId"), v.id},
                   {QStringLiteral("id"), v.ytId}, {QStringLiteral("title"), v.ytTitle}, {QStringLiteral("review"), v.review},
                   {QStringLiteral("file"), missing ? QStringLiteral("missing") : QStringLiteral("empty")},
                   {QStringLiteral("trackId"), track.id}, {QStringLiteral("track"), track.title}});
        emit videoRemoved(v.id);
    }
    if (!orphaned.isEmpty()) {
        for (qint64 id : std::as_const(orphaned))
            m_db->setTrackReimport(id);
        requeuePending();
    }
    return dropped;
}

int ImportManager::deleteUntracked()
{
    // Not while tracks are being looked up or the folders are being read:
    // what is untracked is settled only once those are done.
    if (!m_started || busy())
        return 0;
    QJsonArray names;
    int deleted = 0;
    for (const VideoInfo &v : m_db->untrackedVideos()) {
        removeUntracked(v);
        names.append(QJsonObject{{QStringLiteral("ytId"), v.ytId}, {QStringLiteral("title"), v.title}, {QStringLiteral("artist"), v.artist}});
        ++deleted;
    }
    if (deleted > 0) {
        qInfo("[library] deleted %d videos that no track in the music folders has", deleted);
        appendLog({{QStringLiteral("event"), QStringLiteral("untracked-deleted")}, {QStringLiteral("videos"), names}});
    }
    return deleted;
}

void ImportManager::keepUntracked(qint64 videoId)
{
    const std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v || v->review || !m_db->untrackedVideoIds().contains(videoId))
        return;
    m_db->keepVideo(videoId);
    qInfo().noquote() << QStringLiteral("[library] keeping “%1” (%2), whose tracks are gone").arg(v->title, v->ytTitle);
    appendLog({{QStringLiteral("event"), QStringLiteral("untracked-kept")}, {QStringLiteral("videoId"), videoId},
               {QStringLiteral("ytId"), v->ytId}, {QStringLiteral("title"), v->title}, {QStringLiteral("artist"), v->artist}});
}

bool ImportManager::deleteUntracked(qint64 videoId)
{
    // A scan may be about to bring its tracks back.
    if (!m_started || m_scanning)
        return false;
    const std::optional<VideoInfo> v = m_db->video(videoId);
    if (!v || v->review || !m_db->untrackedVideoIds().contains(videoId))
        return false;
    removeUntracked(*v);
    qInfo().noquote() << QStringLiteral("[library] deleted “%1” (%2), whose tracks are gone").arg(v->title, v->ytTitle);
    appendLog({{QStringLiteral("event"), QStringLiteral("untracked-deleted")},
               {QStringLiteral("videos"), QJsonArray{QJsonObject{{QStringLiteral("ytId"), v->ytId}, {QStringLiteral("title"), v->title},
                                                                {QStringLiteral("artist"), v->artist}}}}});
    return true;
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
    YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, &m_cancel, cfg.accountOnBotCheck);
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

    YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, &m_cancel, cfg.accountOnBotCheck);
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
            plan.align = AudioAlign::align(trackPcm, mvPcm);
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
    Muxer::ReviewSpan span;
    if (!Muxer::mux(plan, &m_cancel, &audioDetail, &error, &span))
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
    v.ytStream = plan.replaceAudio ? 1 : 0;
    v.audioDetail = audioDetail;
    v.ytAbr = YtDlp::audioKbps(audioInfo);
    v.reviewStart = span.start;
    v.reviewEnd = span.end;
    v.spans = span.spans;
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

bool ImportManager::dropOutdatedOptions(const TrackInfo &track)
{
    // A video brought in for review under earlier rules, which today's
    // would not examine for this track: an upload under half the track's
    // length, the opening of a show against the whole song. It was only ever
    // there to be judged, and the rule has judged it. Its other candidates
    // keep it; with none left it goes.
    // Likewise one from a small channel, by what the library kept of its
    // channel.
    // Not the upload MusicBrainz links to the track, by what was kept of its
    // answer (D7).
    const ImportSettings cfg = settings();
    const QStringList linked = m_db->musicVideosKept(track);
    bool dropped = false;
    for (const VideoInfo &was : m_db->candidacies(track)) {
        QString why = Matcher::lengthMismatch(track, was.duration);
        if (why.isEmpty() && smallChannel(was.ytChannelSubs, was.ytChannelVerified, m_db->channelAllowed(was.ytChannelId) || linked.contains(was.ytId), cfg))
            why = smallChannelReason(was.ytChannelSubs, cfg);
        if (why.isEmpty())
            continue;
        if (!claimVideo(was.ytId))
            break; // cancelled
        const auto release = qScopeGuard([&] { releaseVideo(was.ytId); });
        const std::optional<VideoInfo> v = m_db->video(was.id);
        if (!v || !v->review)
            continue;
        m_db->removeCandidate(v->id, track);
        const bool last = m_db->candidateCount(v->id) == 0;
        dropped = true;
        qInfo().noquote() << QStringLiteral("[import] “%1” [%2]: no longer a candidate of “%3”, which waits for review: %4")
                                 .arg(track.title, track.album, v->ytTitle,
                                      why.startsWith(QLatin1String("its channel")) ? why
                                          : QStringLiteral("%1 (%2 of %3)").arg(why, formatDuration(v->duration), formatDuration(track.duration)));
        appendLog({{QStringLiteral("event"), QStringLiteral("outdated-option")}, {QStringLiteral("videoId"), v->id},
                   {QStringLiteral("id"), v->ytId}, {QStringLiteral("title"), v->ytTitle}, {QStringLiteral("channel"), v->ytChannel},
                   {QStringLiteral("duration"), qRound(v->duration)}, {QStringLiteral("reason"), why},
                   {QStringLiteral("trackId"), track.id}, {QStringLiteral("track"), track.title},
                   {QStringLiteral("trackDuration"), qRound(track.duration)}, {QStringLiteral("deleted"), last}});
        if (last)
            discardVideo(*v);
        else
            emit videoChanged(v->id);
    }
    return dropped;
}

int ImportManager::reimport(const QVector<qint64> &trackIds)
{
    if (!m_started)
        return 0;
    QSet<qint64> recordings;
    QVector<qint64> marked;
    int queued = 0;
    bool widened = false; // a track outside the video folders was named (H9)
    for (qint64 id : trackIds) {
        const std::optional<TrackInfo> t = m_db->track(id);
        if (!t || t->absent)
            continue;
        widened |= t->lookup == 0;
        // Files of one recording are one lookup; a track that is already
        // waiting its turn is not asked twice.
        const qint64 key = t->recording > 0 ? t->recording : -t->id;
        if (recordings.contains(key))
            continue;
        recordings.insert(key);
        m_db->setTrackReimport(t->id);
        marked << t->id;
        ++queued;
    }
    if (queued == 0)
        return 0;
    // What the rules settle without asking YouTube is settled now: a lookup
    // may have to wait out a pause.
    const ImportSettings cfg = settings();
    m_jobPool.start([this, marked, cfg] {
        for (qint64 id : marked) {
            if (m_cancel)
                break;
            if (const std::optional<TrackInfo> t = m_db->track(id)) {
                dropOutdatedOptions(*t);
                if (t->videoId > 0)
                    settleShared(t->videoId, cfg);
            }
        }
    });
    for (const TrackInfo &t : m_db->pendingRecordings())
        enqueue(t.id);
    qInfo("[import] re-importing %d tracks", queued);
    appendLog({{QStringLiteral("event"), QStringLiteral("reimport")}, {QStringLiteral("tracks"), queued}});
    // A track looked up from now on has no fingerprint yet: the next scan
    // takes it (A1), while its lookup runs under its own id meanwhile.
    if (widened)
        rescan();
    emit activityChanged();
    pump();
    return queued;
}

// ---- Replacing a video by hand ---------------------------------------------

std::optional<TrackInfo> ImportManager::replaceTrack(const VideoInfo &video)
{
    // Of a video under review: the candidate shown first.
    if (video.review) {
        const QVector<Database::Candidate> candidates = candidatesInOrder(m_db, video);
        return candidates.isEmpty() ? std::nullopt : std::optional<TrackInfo>(candidates.first().track);
    }
    QVector<TrackInfo> tracks = m_db->tracksForVideo(video.id);
    if (tracks.isEmpty())
        return std::nullopt;
    std::sort(tracks.begin(), tracks.end(), betterSource);
    return tracks.first();
}

bool ImportManager::replaceSearch(qint64 videoId)
{
    const std::optional<VideoInfo> v = m_db->video(videoId);
    const std::optional<TrackInfo> t = v ? replaceTrack(*v) : std::nullopt;
    if (!m_started || !v || !t) {
        emit replaceOptions(videoId, {}, QStringLiteral("no music file of your library has this video"));
        return false;
    }
    if (m_replacing.exchange(true))
        return false;
    const ImportSettings cfg = settings();
    emit activityChanged();
    m_replacePool.start([this, v = *v, t = *t, cfg] {
        replaceSearchJob(v, t, cfg);
        m_replacing = false;
        QMetaObject::invokeMethod(this, [this, id = v.id] {
            emit replaceStageChanged(id, QString());
            emit activityChanged();
        }, Qt::QueuedConnection);
    });
    return true;
}

bool ImportManager::replaceCheck(qint64 videoId, const QString &ytId)
{
    const std::optional<VideoInfo> v = m_db->video(videoId);
    const std::optional<TrackInfo> t = v ? replaceTrack(*v) : std::nullopt;
    if (!m_started || !v || !t) {
        emit replaceChecked(videoId, {{QStringLiteral("id"), ytId}, {QStringLiteral("matches"), false},
                                      {QStringLiteral("error"), QStringLiteral("no music file of your library has this video")}});
        return false;
    }
    if (m_replacing.exchange(true))
        return false;
    const ImportSettings cfg = settings();
    emit activityChanged();
    m_replacePool.start([this, v = *v, t = *t, ytId, cfg] {
        replaceCheckJob(v, t, ytId, cfg);
        m_replacing = false;
        QMetaObject::invokeMethod(this, [this, id = v.id] {
            emit replaceStageChanged(id, QString());
            emit activityChanged();
        }, Qt::QueuedConnection);
    });
    return true;
}

bool ImportManager::replaceWith(qint64 videoId, const QString &ytId)
{
    const std::optional<VideoInfo> v = m_db->video(videoId);
    const std::optional<TrackInfo> t = v ? replaceTrack(*v) : std::nullopt;
    if (!m_started || !v || !t || ytId.isEmpty() || ytId == v->ytId)
        return false;
    if (m_replacing.exchange(true))
        return false;
    const ImportSettings cfg = settings();
    emit activityChanged();
    m_replacePool.start([this, v = *v, t = *t, ytId, cfg] {
        replaceWithJob(v, t, ytId, cfg);
        m_replacing = false;
        QMetaObject::invokeMethod(this, [this, id = v.id] {
            emit replaceStageChanged(id, QString());
            emit activityChanged();
            pump();
        }, Qt::QueuedConnection);
    });
    return true;
}

namespace {

// One upload listened to for a track, by hand.
struct Heard {
    bool ok = false;          // its audio could be had and compared
    bool matches = false;     // it is the song, and not a still image or interrupted
    AudioAlign::Result ar;
    QJsonObject info;
    QString audioFile;
    QString reason;           // why it is or is not the track's
    bool still = false;
};

QString heardSummary(const AudioAlign::Result &ar)
{
    const double whole = qMax(1.0, std::min(ar.trackSec, ar.videoSec));
    return QStringLiteral("%1% the same waveform, fingerprints cover %2%")
        .arg(qRound(100 * ar.goodSec / whole)).arg(qRound(100 * ar.fpMatchedSec / whole));
}

} // namespace

// Fetches an upload's audio, compares it with the track, and looks at its
// picture: the examination a candidate gets in a lookup, without the rules
// about whose channel it is.
static Heard listenTo(YtDlp &yt, const TrackInfo &track, const std::vector<int16_t> &trackPcm, const QString &ytId,
                      const QString &title, const QString &channel, const QString &dir, const ImportSettings &cfg,
                      const std::atomic<bool> *cancel)
{
    Heard h;
    QString error;
    QDir().mkpath(dir);
    if (!yt.downloadAudio(ytId, dir, &h.audioFile, &h.info, &error)) {
        h.reason = YtDlp::looksUnavailable(error) ? QStringLiteral("the video is not available: %1").arg(error.left(160))
                                                  : QStringLiteral("its audio could not be downloaded: %1").arg(error);
        return h;
    }
    std::vector<int16_t> pcm;
    if (!AudioAlign::decodeMono(h.audioFile, &pcm, cancel, &error)) {
        h.reason = QStringLiteral("its audio could not be decoded: %1").arg(error);
        return h;
    }
    h.ok = true;
    h.ar = AudioAlign::align(trackPcm, pcm);
    if (!audioMatches(h.ar)) {
        h.reason = mismatchReason(h.ar);
        return h;
    }
    if (interrupted(h.ar)) {
        h.reason = interruptedReason(h.ar);
        return h;
    }
    if (cfg.skipStillImages) {
        Muxer::StillCheck check;
        const QJsonObject known = yt.note(ytId, QStringLiteral("picture"));
        if (known.value(QLatin1String("samples")).toInt() > 0) {
            check.valid = true;
            check.still = known.value(QLatin1String("still")).toBool();
            check.movingShare = known.value(QLatin1String("movingShare")).toDouble();
            check.samples = known.value(QLatin1String("samples")).toInt();
        } else {
            QString preview;
            if (yt.downloadPreview(ytId, dir, &preview, &error)) {
                check = Muxer::checkStill(preview, false, cancel);
                QFile::remove(preview);
                if (check.valid) {
                    yt.setNote(ytId, QStringLiteral("picture"),
                               {{QStringLiteral("still"), check.still}, {QStringLiteral("movingShare"), check.movingShare},
                                {QStringLiteral("samples"), check.samples}});
                }
            }
        }
        if (check.valid && check.still) {
            h.still = true;
            h.reason = QStringLiteral("the song, but a still image: %1% of %2 sampled frame pairs change")
                           .arg(qRound(check.movingShare * 100)).arg(qMax(0, check.samples - 1));
            return h;
        }
    }
    h.matches = true;
    h.reason = heardSummary(h.ar);
    // A short edit of the song: the user may want it all the same.
    if (shortCut(h.ar))
        h.reason = QStringLiteral("a cut of the song, %1 s of %2 s · ").arg(qRound(h.ar.videoSec)).arg(qRound(h.ar.trackSec)) + h.reason;
    return h;
}

void ImportManager::replaceSearchJob(const VideoInfo &video, const TrackInfo &track, const ImportSettings &cfg)
{
    const QString workDir = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/replace-%1").arg(video.id));
    const auto cleanup = qScopeGuard([&] { QDir(workDir).removeRecursively(); });
    auto stage = [&](const QString &s) { emit replaceStageChanged(video.id, s); };
    auto fail = [&](const QString &why) { emit replaceOptions(video.id, {}, why); };

    stage(QStringLiteral("Searching"));
    YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, &m_cancel, cfg.accountOnBotCheck);
    yt.setCacheDir(cacheDir(cfg.mvDir));
    QVector<YtCandidate> candidates;
    QSet<QString> seen;
    QString error;
    bool searched = false;
    for (const QString &query : Matcher::searchQueries(track)) {
        QVector<YtCandidate> found;
        if (!yt.search(query, 10, &found, &error)) {
            if (m_cancel)
                return;
            continue;
        }
        searched = true;
        for (YtCandidate &c : found) {
            if (seen.contains(c.id))
                continue;
            seen.insert(c.id);
            if (!candidates.isEmpty())
                c.rank += 2;
            candidates.append(c);
        }
    }
    if (!searched)
        return fail(QStringLiteral("search failed: %1").arg(error));
    Matcher::rank(track, candidates, [&](const YtCandidate &c) {
        return ArtistChannels::isArtistChannel(*m_db, Matcher::artistNames(track), c.channelId, c.channel);
    });

    std::vector<int16_t> trackPcm;
    if (!AudioAlign::decodeMono(track.path, &trackPcm, &m_cancel, &error))
        return fail(QStringLiteral("cannot decode the track: %1").arg(error));

    // Uploads that could be the track's, best first: every channel, not
    // only the artist's own. What the titles rule out stays out (covers,
    // other songs), as does what the user turned down for the track.
    QVariantList options;
    int examined = 0;
    for (const YtCandidate &c : std::as_const(candidates)) {
        if (m_cancel)
            return;
        if (options.size() >= 3 || examined >= 8)
            break;
        if (!c.rejectReason.isEmpty() || c.id == video.ytId || m_db->videoRejectedFor(track, c.id))
            continue;
        ++examined;
        stage(QStringLiteral("Checking audio: %1").arg(c.title));
        const Heard h = listenTo(yt, track, trackPcm, c.id, c.title, c.channel, QDir(workDir).filePath(c.id), cfg, &m_cancel);
        if (m_cancel)
            return;
        if (!h.matches)
            continue;
        const bool artistChannel = c.ownChannel || ArtistChannels::isArtistChannel(*m_db, Matcher::artistNames(track), c.channelId, c.channel);
        options << QVariantMap{
            {QStringLiteral("id"), c.id}, {QStringLiteral("title"), c.title}, {QStringLiteral("channel"), c.channel},
            {QStringLiteral("duration"), c.duration}, {QStringLiteral("thumbnail"), h.info.value(QLatin1String("thumbnail")).toString()},
            {QStringLiteral("waveform"), waveformShare(h.ar)},
            {QStringLiteral("fingerprint"), h.ar.fpMatchedSec / qMax(1.0, std::min(h.ar.trackSec, h.ar.videoSec))},
            {QStringLiteral("artistChannel"), artistChannel}, {QStringLiteral("summary"), h.reason},
        };
    }
    emit replaceOptions(video.id, options, options.isEmpty() ? QStringLiteral("none of %1 uploads examined sounds like the track").arg(examined) : QString());
}

void ImportManager::replaceCheckJob(const VideoInfo &video, const TrackInfo &track, const QString &ytId, const ImportSettings &cfg)
{
    const QString workDir = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/replace-%1").arg(video.id));
    const auto cleanup = qScopeGuard([&] { QDir(workDir).removeRecursively(); });
    emit replaceStageChanged(video.id, QStringLiteral("Checking audio"));
    QVariantMap r{{QStringLiteral("id"), ytId}, {QStringLiteral("matches"), false}};
    auto answer = [&] { emit replaceChecked(video.id, r); };
    YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, &m_cancel, cfg.accountOnBotCheck);
    yt.setCacheDir(cacheDir(cfg.mvDir));
    std::vector<int16_t> trackPcm;
    QString error;
    if (!AudioAlign::decodeMono(track.path, &trackPcm, &m_cancel, &error)) {
        r.insert(QStringLiteral("error"), QStringLiteral("cannot decode the track: %1").arg(error));
        return answer();
    }
    // Title and channel are in the description that comes with the audio.
    Heard h = listenTo(yt, track, trackPcm, ytId, QString(), QString(), QDir(workDir).filePath(ytId), cfg, &m_cancel);
    if (m_cancel)
        return;
    const QString title = h.info.value(QLatin1String("title")).toString();
    QString channel = h.info.value(QLatin1String("channel")).toString();
    if (channel.isEmpty())
        channel = h.info.value(QLatin1String("uploader")).toString();
    if (h.ok && !title.isEmpty()) {
        // Judged again with the names known: another version's words count.
        h = listenTo(yt, track, trackPcm, ytId, title, channel, QDir(workDir).filePath(ytId), cfg, &m_cancel);
        if (m_cancel)
            return;
    }
    r.insert(QStringLiteral("title"), title);
    r.insert(QStringLiteral("channel"), channel);
    r.insert(QStringLiteral("duration"), h.info.value(QLatin1String("duration")).toDouble());
    r.insert(QStringLiteral("thumbnail"), h.info.value(QLatin1String("thumbnail")).toString());
    r.insert(QStringLiteral("matches"), h.matches);
    r.insert(QStringLiteral("reason"), h.reason);
    if (!h.ok)
        r.insert(QStringLiteral("error"), h.reason);
    answer();
}

void ImportManager::replaceWithJob(const VideoInfo &video, const TrackInfo &track, const QString &ytId, const ImportSettings &cfg)
{
    const QString workDir = QDir(dataDir(cfg.mvDir)).filePath(QStringLiteral("tmp/replace-%1").arg(video.id));
    QDir(workDir).removeRecursively();
    const auto cleanup = qScopeGuard([&] { QDir(workDir).removeRecursively(); });
    auto stage = [&](const QString &s) { emit replaceStageChanged(video.id, s); };

    JobStatus st;
    st.trackId = track.id;
    st.title = track.title;
    st.artist = splitMulti(track.albumArtist).value(0, track.artist);
    auto report = [&](const QString &s, double progress = -1, const QString &detail = QString()) {
        st.stage = s;
        st.progress = progress;
        st.detail = detail;
        emit jobChanged(st);
        stage(s);
    };
    auto finish = [&](bool ok, const QString &message) {
        st.finished = true;
        st.outcome = ok ? QStringLiteral("done") : QStringLiteral("failed");
        st.progress = -1;
        st.stage = ok ? QStringLiteral("Replaced") : QStringLiteral("Not replaced");
        st.detail = message;
        qInfo().noquote() << QStringLiteral("[replace] %1 — %2: %3 (%4)").arg(st.artist, st.title, st.stage, message);
        emit jobChanged(st);
    };

    // The upload the user chose is the track's video, accepted. The video
    // the track had goes; and the track is no longer a candidate of any
    // video under review (one left without a candidate goes too). A track
    // that had the chosen video until now looks for another.
    auto takeOver = [&](qint64 newId, const QString &newTitle) {
        QJsonArray gone;
        if (!video.review && video.id != newId) {
            m_db->removeVideo(video.id);
            removeVideoFiles(video);
            emit videoRemoved(video.id);
            gone.append(QJsonObject{{QStringLiteral("videoId"), video.id}, {QStringLiteral("id"), video.ytId}, {QStringLiteral("title"), video.ytTitle}});
        }
        const bool requeue = !m_db->unlinkOthers(newId, track).isEmpty();
        m_db->setTrackResult(track.id, QStringLiteral("done"), newId, QStringLiteral("you chose “%1”").arg(newTitle));
        const QVector<VideoInfo> was = m_db->candidacies(track);
        for (const VideoInfo &o : was) {
            if (o.id != newId)
                gone.append(QJsonObject{{QStringLiteral("videoId"), o.id}, {QStringLiteral("id"), o.ytId}, {QStringLiteral("title"), o.ytTitle}});
        }
        withdraw(track, was);
        appendLog({{QStringLiteral("event"), QStringLiteral("replace")}, {QStringLiteral("trackId"), track.id},
                   {QStringLiteral("track"), track.title}, {QStringLiteral("artist"), track.artist}, {QStringLiteral("album"), track.album},
                   {QStringLiteral("videoId"), newId}, {QStringLiteral("id"), ytId}, {QStringLiteral("title"), newTitle},
                   {QStringLiteral("replaced"), gone}});
        if (requeue)
            requeuePending();
    };
    if (const auto there = m_db->videoByYtId(ytId)) {
        // In the library already: nothing is fetched.
        if (there->review) {
            // A video waiting for review: this is the verdict, for this track.
            const QVector<Database::Candidate> candidates = m_db->candidates(there->id);
            if (std::none_of(candidates.begin(), candidates.end(), [&](const Database::Candidate &c) {
                    return Database::trackKey(c.track) == Database::trackKey(track); }))
                m_db->addCandidate(there->id, track, 0, 0, QString());
            approveJob(there->id, track.id, cfg);
            const std::optional<TrackInfo> now = m_db->track(track.id);
            if (!now || now->videoId != there->id) {
                finish(false, QStringLiteral("“%1” could not be made the track's video").arg(there->ytTitle));
                return;
            }
            if (!video.review && video.id != there->id) {
                m_db->removeVideo(video.id);
                removeVideoFiles(video);
                emit videoRemoved(video.id);
            }
        } else {
            takeOver(there->id, there->ytTitle);
        }
        emit videoChanged(there->id);
        finish(true, QStringLiteral("now “%1”, which was in the library already").arg(there->ytTitle));
        return;
    }

    YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, &m_cancel, cfg.accountOnBotCheck);
    yt.setCacheDir(cacheDir(cfg.mvDir));
    const QString dir = QDir(workDir).filePath(ytId);
    QDir().mkpath(dir);
    QString error;

    // Its audio against the track's: where they are the same recording the
    // track's audio goes in, as in any import. Where they are not (the user
    // said to take it all the same) the video keeps its own.
    report(QStringLiteral("Checking audio"));
    std::vector<int16_t> trackPcm;
    AudioAlign::Result ar;
    QString ytAudio;
    QJsonObject ytInfo;
    bool heard = false;
    if (AudioAlign::decodeMono(track.path, &trackPcm, &m_cancel, &error)) {
        const Heard h = listenTo(yt, track, trackPcm, ytId, QString(), QString(), dir, cfg, &m_cancel);
        if (m_cancel)
            return;
        if (!h.ok && h.audioFile.isEmpty()) {
            finish(false, h.reason);
            return;
        }
        heard = h.ok;
        ar = h.ar;
        ytAudio = h.audioFile;
        ytInfo = h.info;
    }
    if (m_cancel)
        return;
    const QString title = ytInfo.value(QLatin1String("title")).toString();
    QString channel = ytInfo.value(QLatin1String("channel")).toString();
    if (channel.isEmpty())
        channel = ytInfo.value(QLatin1String("uploader")).toString();
    report(QStringLiteral("Downloading"), 0, title);
    QString videoFile, thumbFile;
    double lastShown = -1;
    if (!yt.downloadVideo(ytId, dir, [&](double p) {
            if (p - lastShown >= 0.01 || p >= 1.0) {
                lastShown = p;
                report(QStringLiteral("Downloading"), p, title);
            }
        }, &videoFile, &thumbFile, &error)) {
        if (m_cancel)
            return;
        finish(false, QStringLiteral("download failed: %1").arg(error));
        return;
    }
    if (yt.hasCookies() && !ytAudio.isEmpty() && !QFileInfo(ytAudio).fileName().startsWith(QLatin1String("premium."))) {
        report(QStringLiteral("Fetching Premium audio"), -1, title);
        QString premiumFile;
        QJsonObject premiumInfo;
        if (yt.downloadAudio(ytId, dir, &premiumFile, &premiumInfo, &error, true) && youtubeQuality(premiumInfo) > youtubeQuality(ytInfo)) {
            ytAudio = premiumFile;
            ytInfo = premiumInfo;
        }
        if (m_cancel)
            return;
    }

    const QString artistDir = sanitizeFileName(splitMulti(track.albumArtist).value(0, QStringLiteral("Unknown Artist")));
    const QString baseName = QStringLiteral("%1 [%2]").arg(sanitizeFileName(track.title), ytId);
    const QString outDir = QDir(cfg.mvDir).filePath(artistDir);
    const QString outFile = QDir(outDir).filePath(baseName + QStringLiteral(".mkv"));
    const QString outThumb = QDir(outDir).filePath(baseName + QStringLiteral(".jpg"));

    Muxer::Plan plan;
    plan.videoFile = videoFile;
    plan.ytAudioFile = ytAudio.isEmpty() ? videoFile : ytAudio;
    plan.workDir = dir;
    plan.outFile = outFile;
    plan.track = track;
    plan.ytId = ytId;
    plan.align = ar;
    plan.replaceAudio = heard && cfg.replaceAudio && audioMatches(ar) && audioReplaceable(ar) && libraryIsBetter(track, youtubeQuality(ytInfo));
    report(plan.replaceAudio ? QStringLiteral("Muxing library audio") : QStringLiteral("Muxing"), -1, title);
    QString audioDetail;
    if (!Muxer::mux(plan, &m_cancel, &audioDetail, &error)) {
        if (m_cancel)
            return;
        finish(false, QStringLiteral("could not build the file: %1").arg(error));
        return;
    }
    Muxer::Probe pr;
    Muxer::probe(outFile, &pr);
    QFile::remove(outThumb);
    if (thumbFile.isEmpty() || !QFile::rename(thumbFile, outThumb)) {
        ProcOptions o;
        o.timeoutMs = 60000;
        runProcess(QStringLiteral("ffmpeg"),
                   {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
                    QStringLiteral("-ss"), QString::number(pr.duration * 0.25, 'f', 2), QStringLiteral("-i"), outFile,
                    QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-vf"),
                    QStringLiteral("scale=1280:-2"), outThumb}, o);
    }
    if (!cfg.subtitleLangs.isEmpty() && !m_cancel) {
        report(QStringLiteral("Fetching subtitles"), -1, title);
        QString why;
        if (fetchSubtitlesFor(yt, ytId, outFile, cfg.subtitleLangs, dir, &why) < 0 && !m_cancel)
            qWarning().noquote() << "[replace] no subtitles for" << ytId << ":" << why;
    }
    if (m_cancel) {
        QFile::remove(outFile);
        QFile::remove(outThumb);
        return;
    }

    VideoInfo v;
    v.ytId = ytId;
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
    v.ytStream = plan.replaceAudio ? 1 : 0;
    v.audioDetail = audioDetail;
    v.ytAbr = YtDlp::audioKbps(ytInfo);
    v.ytTitle = title;
    v.ytChannel = channel;
    v.ytChannelId = ytInfo.value(QLatin1String("channel_id")).toString();
    const YtDlp::Channel channelFacts = YtDlp::channelOf(ytInfo);
    v.ytChannelSubs = channelFacts.subscribers;
    v.ytChannelVerified = channelFacts.verified;
    v.ytChannelHandle = channelFacts.handle;
    v.ytDate = YtDlp::publishedOn(ytInfo);
    v.ytDescription = YtDlp::descriptionOf(ytInfo);
    v.tags = track.tags;
    v.addedAt = QDateTime::currentSecsSinceEpoch();
    const qint64 newId = m_db->insertVideo(v);
    if (newId <= 0) {
        QFile::remove(outFile);
        QFile::remove(outThumb);
        finish(false, QStringLiteral("could not record the video in the library"));
        return;
    }
    takeOver(newId, title);
    emit videoAdded(newId);
    finish(true, QStringLiteral("%1 · %2×%3 · %4").arg(title).arg(pr.width).arg(pr.height).arg(audioDetail));
}

int ImportManager::reimportVideo(qint64 videoId)
{
    // The track that has it; or, of a video under review, its candidates.
    QVector<qint64> ids;
    for (const TrackInfo &t : m_db->tracksForVideo(videoId))
        ids << t.id;
    for (const Database::Candidate &c : m_db->candidates(videoId))
        ids << c.track.id;
    return reimport(ids);
}

int ImportManager::reimportPath(const QString &path)
{
    return reimport(m_db->trackIdsUnder(path));
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

        // The review queue in step with the library: files that turned out
        // to be one recording bring a video and candidacies together.
        for (qint64 id : m_db->dropCandidaciesOfTracksWithVideos()) {
            const std::optional<VideoInfo> v = m_db->video(id);
            if (v && v->review && m_db->candidateCount(id) == 0)
                discardVideo(*v);
            else if (v)
                emit videoChanged(id);
        }
        m_db->requeueTracksWithoutCandidacies();
        m_db->requeueUntriedTitles();
        for (const TrackInfo &t : m_db->pendingRecordings())
            enqueue(t.id);
        for (qint64 id : r.videosChanged)
            emit videoChanged(id);
        emit libraryChanged();
        if (!m_settledOnce) {
            m_settledOnce = true;
            // A library from before a video had one track: the videos that
            // several tracks have are settled by a re-import of those tracks.
            QVector<qint64> sharers;
            for (qint64 id : m_db->sharedVideoIds()) {
                for (const TrackInfo &t : m_db->tracksForVideo(id))
                    sharers << t.id;
            }
            if (!sharers.isEmpty()) {
                qInfo("[import] videos that several tracks have, from before one track, one video: their tracks are looked up again");
                reimport(sharers);
            }
            // And one from before review went by video: its candidates are
            // measured, and each given its audio in the file.
            ++m_reviewJobs;
            const ImportSettings cfg = settings();
            m_auditPool.start([this, cfg] {
                prepareCandidates(cfg);
                QMetaObject::invokeMethod(this, [this] {
                    // Counted down on the main thread, after whatever the job
                    // queued there (a re-lookup) has landed: a scan ending in
                    // between would otherwise find nothing busy, and a headless
                    // run would end with the track still pending.
                    --m_reviewJobs;
                    emit activityChanged();
                    pump();
                }, Qt::QueuedConnection);
            });
        }
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
    std::optional<TrackInfo> maybe = m_db->track(trackId);
    if (!maybe || maybe->state != QLatin1String("pending") || maybe->absent || maybe->lookup == 0)
        return;
    if (m_blocked)
        return; // stays pending; picked up again when the pause ends
    if (maybe->reimport) {
        // What a re-import settles before anything is judged. (Also done
        // when it is asked for; a lookup may get here first.) A video that
        // several tracks have goes to the closest fit of them: this track
        // may be left without one, and is then looked up as such.
        bool settled = dropBrokenVideos(*maybe) | dropOutdatedOptions(*maybe);
        if (maybe->videoId > 0 && !m_cancel)
            settled |= settleShared(maybe->videoId, cfg);
        if (settled) {
            maybe = m_db->track(trackId);
            if (!maybe || maybe->state != QLatin1String("pending") || maybe->absent || maybe->lookup == 0)
                return;
        }
    }
    const TrackInfo track = *maybe;

    QElapsedTimer clock;
    clock.start();
    int nSearch = 0, nAudio = 0, nPreview = 0, nVideo = 0, nSubs = 0, nPage = 0;
    const YtDlp *ytForLog = nullptr;
    QJsonArray logQueries, logCandidates, logChecked;
    QJsonObject logMusicBrainz; // B7–B14: how the recordings were found, what they link to
    std::optional<VideoInfo> prior; // re-import: the video the track had going in (set below)
    QString priorFate;              // and what became of it: kept | review | removed | released | unjudged
    QJsonArray logVideos; // what was brought into the library
    auto writeLog = [&](const QString &outcome, const QString &message) {
        // accept: the track has its video. review: it is a candidate of
        // videos waiting for the user. reject: none of what was found is
        // its video.
        const QString decision = outcome == QLatin1String("done") ? QStringLiteral("accept")
            : outcome == QLatin1String("review") ? QStringLiteral("review")
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
            {QStringLiteral("reimport"), track.reimport},
            // The video the track had going in, and what became of it.
            {QStringLiteral("prior"), prior ? QJsonValue(QJsonObject{{QStringLiteral("videoId"), prior->id}, {QStringLiteral("id"), prior->ytId},
                                                                      {QStringLiteral("title"), prior->ytTitle}, {QStringLiteral("fate"), priorFate}})
                                            : QJsonValue()},
            {QStringLiteral("videos"), logVideos},
            {QStringLiteral("seconds"), qRound(clock.elapsed() / 100.0) / 10.0},
            {QStringLiteral("requests"), QJsonObject{{QStringLiteral("search"), nSearch}, {QStringLiteral("audio"), nAudio},
                                                      {QStringLiteral("preview"), nPreview}, {QStringLiteral("video"), nVideo}, {QStringLiteral("subtitles"), nSubs}, {QStringLiteral("page"), nPage}, {QStringLiteral("fromCache"), ytForLog ? ytForLog->cacheHits() : 0}}},
            {QStringLiteral("queries"), logQueries},
            {QStringLiteral("musicbrainz"), logMusicBrainz.isEmpty() ? QJsonValue() : QJsonValue(logMusicBrainz)},
            {QStringLiteral("candidates"), logCandidates},
            {QStringLiteral("checked"), logChecked},
        });
    };
    // One examined candidate: what was decided about it, why, and the numbers.
    // decision: accept | review | reject | undecided (could not be examined).
    auto checked = [&](const YtCandidate &c, const QString &result, const QString &reason,
                       const AudioAlign::Result *ar = nullptr, const QJsonObject &more = {}) {
        static const QHash<QString, QString> decisions = {
            {QStringLiteral("match"), QStringLiteral("accept")}, {QStringLiteral("takeover"), QStringLiteral("accept")},
            {QStringLiteral("unconfirmed"), QStringLiteral("review")}, {QStringLiteral("option"), QStringLiteral("review")},
            {QStringLiteral("candidate"), QStringLiteral("review")},
            {QStringLiteral("error"), QStringLiteral("undecided")}, {QStringLiteral("judged"), QStringLiteral("undecided")},
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

    // The videos under review this track is a candidate of already, from an
    // earlier look.
    QSet<qint64> held;
    for (const VideoInfo &v : m_db->candidacies(track))
        held.insert(v.id);

    // Once the track has its video it is a candidate of nothing else: the
    // videos it leaves are settled when this lookup is over and holds no
    // video any more (see withdraw).
    QVector<VideoInfo> leaving;
    const auto leave = qScopeGuard([&] {
        if (!m_cancel && !leaving.isEmpty())
            withdraw(track, leaving);
    });

    // Re-import: the video the track has is judged by today's rules like any
    // candidate, on the upload's own audio and picture, which are in its
    // file: nothing is downloaded for it. What becomes of it is settled with
    // the track's result.
    if (track.reimport && track.videoId > 0) {
        prior = m_db->video(track.videoId);
        if (prior && (prior->review || !QFile::exists(prior->path)))
            prior.reset();
    }
    bool priorRejected = false; // today's rules turn it down
    QString priorReason;
    bool priorKept = false;     // it is the track's video still, or one it is a candidate of

    // The video the track had is not its video any more, and goes. (Not
    // where a track that has left the music folders has it too.)
    auto dropPrior = [&](const QString &why) {
        bool held = false;
        for (const TrackInfo &o : m_db->tracksForVideo(prior->id, true)) {
            if (Database::trackKey(o) == Database::trackKey(track))
                continue;
            held = true;
            break;
        }
        if (held) {
            priorFate = QStringLiteral("released");
            qInfo().noquote() << QStringLiteral("[import] “%1” [%2] lets go of “%3” (%4)")
                                     .arg(track.title, track.album, prior->ytTitle, why);
            return;
        }
        priorFate = QStringLiteral("removed");
        m_db->removeVideo(prior->id);
        removeVideoFiles(*prior);
        const QString root = QDir(cfg.mvDir).absolutePath();
        const QString dir = QFileInfo(prior->path).absolutePath();
        if (dir != root && dir.startsWith(root + QLatin1Char('/')))
            QDir().rmdir(dir);
        qInfo().noquote() << QStringLiteral("[import] “%1” [%2]: deleted “%3”, its video until now (%4)")
                                 .arg(track.title, track.album, prior->ytTitle, why);
        emit videoRemoved(prior->id);
    };

    // The track's own sound, for everything that is settled by listening.
    std::vector<int16_t> trackPcm;
    bool heardTrack = false;

    auto finish = [&](const QString &result, qint64 resultVideo, const QString &resultMessage) {
        cleanup();
        if (m_cancel)
            return; // leave the track pending for the next run
        // A track that today's rules skip waits for nothing: it is no longer
        // a candidate of any video under review, and one left with none goes
        // (see withdraw).
        const bool skipped = result == QLatin1String("skipped");
        if (skipped)
            leaving = m_db->candidacies(track);
        // A lookup that gives the track no video leaves it waiting on the
        // videos it is a candidate of. (Read now: a lookup takes minutes,
        // and another one may have settled some of them meanwhile.)
        const bool waits = result != QLatin1String("done") && result != QLatin1String("review") && !skipped
            && !m_db->candidacies(track).isEmpty();
        QString outcome = waits ? QStringLiteral("review") : result;
        qint64 videoId = waits ? 0 : resultVideo;
        QString message = waits ? QStringLiteral("no other video to offer for review") : resultMessage;
        bool dropped = false;
        if (prior) {
            if (priorKept) {
                // Settled where it was brought in: kept, or for review.
            } else if (outcome == QLatin1String("done") || outcome == QLatin1String("review") || priorRejected
                       || outcome == QLatin1String("skipped")) {
                // Another video is the track's, or the rules say this one
                // never was. A lookup that could not finish settles nothing.
                dropped = true;
            } else {
                priorFate = QStringLiteral("unjudged");
                message = QStringLiteral("re-import could not finish, keeping its video: %1").arg(message);
                outcome = QStringLiteral("done");
                videoId = prior->id;
            }
        }
        m_db->setTrackResult(trackId, outcome, videoId, message);
        if (track.reimport)
            m_db->clearTrackReimport(trackId);
        if (dropped) {
            dropPrior(outcome == QLatin1String("done") ? QStringLiteral("another video is the track's")
                      : !priorReason.isEmpty() ? priorReason : message);
        }
        st.finished = true;
        st.outcome = outcome == QLatin1String("review") ? QStringLiteral("done") : outcome;
        st.progress = -1;
        st.detail = message;
        st.stage = outcome == QLatin1String("done") ? (track.reimport ? QStringLiteral("Re-imported") : QStringLiteral("Imported"))
            : outcome == QLatin1String("review") ? QStringLiteral("For your review")
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

    // ---- Judge mode (docs/import-rules-judge.html) ---------------------------
    // The judge runs the lookup: Q (the query), S (the pick), L (gates),
    // V (the verdict), W (what the track gets). The rules of thumb of C, D
    // and E do not apply; what stays of the main page is listed there.
    const bool judging = cfg.judgeMode == QLatin1String("judge");
    const QString judgeKey = judging && cfg.judgeProvider == QLatin1String("claude") ? Judge::readKey(cfg.judgeKeyFile) : QString();
    const QString judgeModel = cfg.judgeProvider == QLatin1String("chatgpt") ? cfg.judgeModel : Judge::modelId(cfg.judgeModel);
    // The judge cannot answer (V5): the lookup is postponed as for a pause,
    // and the pause says why.
    auto judgePause = [&](const QString &why) {
        const QString reason = QStringLiteral("The judge cannot answer: %1").arg(why);
        if (!m_blocked.exchange(true))
            QMetaObject::invokeMethod(this, [this, reason] { tripBreaker(reason); }, Qt::QueuedConnection);
        postpone();
    };
    // Every answer of the judge is logged (X3).
    auto logJudge = [&](const QString &question, const Judge::Answer &a, const QJsonArray &uploads) {
        appendLog({{QStringLiteral("event"), QStringLiteral("judge")}, {QStringLiteral("question"), question},
                   {QStringLiteral("model"), a.model}, {QStringLiteral("trackId"), track.id}, {QStringLiteral("track"), track.title},
                   {QStringLiteral("artist"), track.artist}, {QStringLiteral("album"), track.album}, {QStringLiteral("uploads"), uploads},
                   {QStringLiteral("answer"), a.json}, {QStringLiteral("error"), a.error}, {QStringLiteral("usage"), a.usage},
                   {QStringLiteral("provider"), a.provider},
                   {QStringLiteral("cost"), a.provider == QLatin1String("chatgpt") ? QJsonValue() : QJsonValue(a.cost)}, {QStringLiteral("seconds"), a.seconds}, {QStringLiteral("cached"), a.cached}});
    };

    QString why;
    if (!judging && Matcher::isNonMvTrack(track, &why)) {
        finish(QStringLiteral("skipped"), 0, why);
        return;
    }
    // Talk between songs (a stage announcement, an interview) has no video
    // to find. A title can mislead and so can the sound, so it takes both
    // (A4). In judge mode the judge is told how the track sounds (Q2).
    QString soundLine;
    if (Matcher::isTalkTitle(track.title)) {
        std::vector<int16_t> pcm;
        QString decodeError;
        if (AudioAlign::decodeMono(track.path, &pcm, &m_cancel, &decodeError)) {
            const TalkCheck::Result talk = TalkCheck::measure(pcm);
            if (judging) {
                if (talk.valid)
                    soundLine = QStringLiteral("Sound: %1% of its moments are far quieter than its average, and its pulse is %2 (speech: at least 40% and at most 0.26). By the library's own measure it sounds like %3.")
                                    .arg(qRound(talk.pauses * 100))
                                    .arg(talk.beat, 0, 'f', 2)
                                    .arg(talk.talk ? QStringLiteral("talk") : QStringLiteral("music"));
            } else if (talk.talk) {
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

    // ---- Q. What to search for ------------------------------------------------
    // In judge mode the judge replaces A2–A4 (Q2) and writes the queries (Q3).
    QStringList queries;
    if (judging) {
        if (cfg.judgeProvider == QLatin1String("claude") && judgeKey.isEmpty())
            return judgePause(QStringLiteral("no API key is stored (Settings → Judge)"));
        report(QStringLiteral("Asking the judge"));
        const Judge::Answer a = Judge::ask(judgeKey, judgeModel, QString::fromUtf8(JudgeWords::kQueryWords), Judge::queryContent(track, soundLine),
                                           QString::fromUtf8(JudgeWords::kQuerySchema), 1500, cacheDir(cfg.mvDir), &m_cancel, cfg.judgeProvider, cfg.chatGptAuthFile, cfg.chatGptAccount);
        if (m_cancel)
            return cleanup();
        logJudge(QStringLiteral("query"), a, {});
        if (!a.error.isEmpty())
            return judgePause(a.error);
        if (!a.json.value(QLatin1String("lookup")).toBool()) {
            const QString skip = a.json.value(QLatin1String("skip_reason")).toString().trimmed();
            finish(QStringLiteral("skipped"), 0, QStringLiteral("the judge: %1").arg(skip.isEmpty() ? QStringLiteral("nothing to look for") : skip));
            return;
        }
        for (const QJsonValue &q : a.json.value(QLatin1String("queries")).toArray()) {
            const QString query = q.toString().trimmed();
            if (!query.isEmpty() && queries.size() < Judge::kMaxQueries && !queries.contains(query))
                queries << query;
        }
        if (queries.isEmpty()) {
            finish(QStringLiteral("skipped"), 0, QStringLiteral("the judge gave no query"));
            return;
        }
    } else {
        queries = Matcher::searchQueries(track);
    }

    // ---- 1. Search ---------------------------------------------------------
    report(QStringLiteral("Searching"));
    YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, &m_cancel, cfg.accountOnBotCheck);
    yt.setCacheDir(cacheDir(cfg.mvDir));
    ytForLog = &yt;
    QVector<YtCandidate> candidates;
    QSet<QString> seen;
    // The uploads MusicBrainz links as the track's music video (B11).
    QSet<QString> linked;
    bool searched = false;
    QString error;
    auto usable = [&](const YtCandidate &c) {
        return c.rejectReason.isEmpty() && (c.trusted || cfg.allowUnofficial);
    };
    // On sight. Whose channel an upload is on: the artist's by its name, by
    // MusicBrainz or because the user allowed it; and the track's own video
    // counts as on it where the user accepted that upload, as does an upload
    // MusicBrainz links as the track's music video (R5). An upload from a
    // channel the user denied is dropped.
    const auto rankAll = [&] {
        Matcher::rank(track, candidates, [&](const YtCandidate &c) {
            return (prior && prior->vouched && c.id == prior->ytId) || linked.contains(c.id)
                || ArtistChannels::isArtistChannel(*m_db, Matcher::artistNames(track), c.channelId, c.channel);
        });
        for (YtCandidate &c : candidates) {
            if (judging && c.rejectReason != QLatin1String("from a channel you denied"))
                c.rejectReason.clear(); // S3: nothing is dropped on sight
            if (c.rejectReason.isEmpty() && m_db->channelDenied(c.channelId))
                c.rejectReason = QStringLiteral("from a channel you denied");
        }
    };
    // Judge mode: what it picked from the listing (S3), best first, and
    // where each result came from (S2).
    QStringList picked;
    QString pickReason;
    QHash<QString, QPair<int, int>> cameFrom; // id -> query, rank
    int queryNo = 0;
    for (const QString &query : std::as_const(queries)) {
        QVector<YtCandidate> found;
        ++nSearch;
        ++queryNo;
        if (!yt.search(query, 10, &found, &error)) {
            if (m_cancel)
                return cleanup();
            logQueries.append(QJsonObject{{QStringLiteral("q"), query}, {QStringLiteral("error"), error}});
            if (noteFailure(error))
                return postpone();
            continue;
        }
        // A search that works shows the network is there, not that videos
        // can be had: YouTube asks "are you a bot" of video pages alone.
        // Only a download proves the pause is over (see withRetry).
        m_failStreak = 0;
        logQueries.append(QJsonObject{{QStringLiteral("q"), query}, {QStringLiteral("results"), found.size()}});
        if (found.isEmpty()) {
            // An empty answer is how YouTube refuses a guest it has had
            // enough of; but a query can match nothing too. The probe tells
            // the two apart: a search whose answer is empty only when
            // YouTube refuses, asked fresh, its results not looked at.
            QVector<YtCandidate> probe;
            QString probeError;
            ++nSearch;
            const bool answered = yt.search(QStringLiteral("official music video"), 10, &probe, &probeError, true);
            if (m_cancel)
                return cleanup();
            logQueries.append(answered ? QJsonObject{{QStringLiteral("probe"), true}, {QStringLiteral("results"), probe.size()}}
                                       : QJsonObject{{QStringLiteral("probe"), true}, {QStringLiteral("error"), probeError}});
            if (!answered && noteFailure(probeError))
                return postpone();
            if (answered && !probe.isEmpty()) {
                // B6 (S5 in judge mode): nothing matched the query. Asked
                // again it would find nothing again: the next query.
                searched = true;
                continue;
            }
            // B5 (S1 in judge mode): refused; it is not "no video". The
            // query is asked again after the pause.
            const QString reason = QStringLiteral("YouTube answered the search “%1” with nothing, and the probe after it likewise: refused").arg(query);
            if (!m_blocked.exchange(true))
                QMetaObject::invokeMethod(this, [this, reason] { tripBreaker(reason); }, Qt::QueuedConnection);
            return postpone();
        }
        searched = true;
        int rank = 0;
        for (YtCandidate &c : found) {
            ++rank;
            if (seen.contains(c.id))
                continue;
            seen.insert(c.id);
            cameFrom.insert(c.id, {queryNo, rank});
            if (!candidates.isEmpty())
                c.rank += 2; // results of fallback queries rank slightly lower
            candidates.append(c);
        }
        rankAll();
        if (!judging) {
            if (std::any_of(candidates.begin(), candidates.end(), usable))
                break;
            continue;
        }
        // S2: the listing to the judge, without what you turned down for
        // this track (D0) or denied (G8), a channel you allowed marked (G7).
        QVector<Judge::Upload> listing;
        QJsonArray logListing;
        for (const YtCandidate &c : std::as_const(candidates)) {
            if (!c.rejectReason.isEmpty() || m_db->videoRejectedFor(track, c.id))
                continue;
            Judge::Upload u;
            u.id = c.id;
            u.title = c.title;
            u.channel = c.channel;
            u.verified = c.verified;
            u.views = c.views > 0 ? c.views : -1;
            u.duration = c.duration;
            u.allowed = !c.channelId.isEmpty() && m_db->channelAllowed(c.channelId);
            u.query = cameFrom.value(c.id).first;
            u.rank = cameFrom.value(c.id).second;
            listing.append(u);
            logListing.append(QJsonObject{{QStringLiteral("id"), c.id}, {QStringLiteral("title"), c.title}, {QStringLiteral("channel"), c.channel}});
        }
        if (listing.isEmpty())
            continue;
        // S3: its pick ends the search; none, and the next query is asked.
        report(QStringLiteral("Asking the judge what to listen to"));
        const Judge::Answer a = Judge::ask(judgeKey, judgeModel, QString::fromUtf8(JudgeWords::kPickWords), Judge::pickContent(track, listing),
                                           QString::fromUtf8(JudgeWords::kPickSchema), 1500, cacheDir(cfg.mvDir), &m_cancel, cfg.judgeProvider, cfg.chatGptAuthFile, cfg.chatGptAccount);
        if (m_cancel)
            return cleanup();
        logJudge(QStringLiteral("pick"), a, logListing);
        if (!a.error.isEmpty())
            return judgePause(a.error);
        picked.clear();
        pickReason = a.json.value(QLatin1String("reason")).toString();
        for (const QJsonValue &pick : a.json.value(QLatin1String("picks")).toArray()) {
            const QString id = pick.toObject().value(QLatin1String("id")).toString();
            if (picked.size() < Judge::kMaxPicks && !picked.contains(id)
                && std::any_of(listing.cbegin(), listing.cend(), [&](const Judge::Upload &u) { return u.id == id; }))
                picked << id;
        }
        if (!picked.isEmpty())
            break;
    }
    if (!searched) {
        finish(QStringLiteral("failed"), 0, QStringLiteral("search failed: %1").arg(error));
        return;
    }
    // Re-import: what the library did not keep about a video of the track's
    // (its own, or one under review it is a candidate of) is read from the
    // upload's page, once: its channel's id, by which a channel is known as
    // the artist's whatever name the track's tags carry, and how many
    // subscribers the channel has. One request, nothing downloaded.
    enum class Page { Known, Postponed, Cancelled };
    const auto knowChannel = [&](VideoInfo &v) {
        // (The judge is told the upload's description too, V1: a video from
        // before the library kept it has its page read once for it.)
        if (v.ytChannelSubs != -2 && (!judging || !v.ytDescription.isEmpty()))
            return Page::Known;
        QJsonObject page;
        QString why;
        const int fromCache = yt.cacheHits(), signedIn = yt.accountRequests();
        ++nPage;
        if (!yt.pageInfo(v.ytId, &page, &why)) {
            if (m_cancel)
                return Page::Cancelled;
            if (YtDlp::looksUnavailable(why)) {
                // Gone from YouTube: there is nothing more to learn of it.
                v.ytChannelSubs = -1;
                m_db->updateVideoChannel(v);
                return Page::Known;
            }
            // Refused: the lookup waits. Anything else: judged on what is known.
            return noteFailure(why) ? Page::Postponed : Page::Known;
        }
        if (yt.cacheHits() == fromCache && yt.accountRequests() == signedIn)
            noteSuccess();
        const YtDlp::Channel channel = YtDlp::channelOf(page, v.ytChannel);
        if (v.ytChannelId.isEmpty())
            v.ytChannelId = channel.id;
        v.ytChannelSubs = channel.subscribers;
        v.ytChannelVerified = channel.verified;
        v.ytChannelHandle = channel.handle;
        if (v.ytDate.isNull())
            v.ytDate = YtDlp::publishedOn(page);
        if (v.ytDescription.isEmpty())
            v.ytDescription = YtDlp::descriptionOf(page);
        m_db->updateVideoChannel(v);
        return Page::Known;
    };
    if (prior) {
        const Page page = knowChannel(*prior);
        if (page == Page::Cancelled)
            return cleanup();
        if (page == Page::Postponed)
            return postpone();
    }
    // What the search returned: the best four of these are listened to.
    const QSet<QString> returned = seen;
    // A video of the track's is examined whether or not today's search turns
    // it up: ranked with the results, by what was kept about it.
    bool beside = false;
    const auto putBeside = [&](const VideoInfo &v) {
        if (seen.contains(v.ytId)) {
            // Whose channel it is on is what the library kept, where the
            // search results do not say.
            for (YtCandidate &c : candidates) {
                if (c.id == v.ytId && c.channelId.isEmpty() && !v.ytChannelId.isEmpty()) {
                    c.channelId = v.ytChannelId;
                    beside = true;
                }
            }
            return;
        }
        YtCandidate c;
        c.id = v.ytId;
        c.title = v.ytTitle;
        c.channel = v.ytChannel;
        c.channelId = v.ytChannelId;
        c.verified = v.ytChannelVerified;
        c.duration = v.duration;
        c.rank = candidates.size();
        seen.insert(c.id);
        candidates.append(c);
        beside = true;
    };
    if (prior)
        putBeside(*prior);
    // Re-import: so is every video under review the track is a candidate
    // of. What today's search happens to return does not decide whether one
    // is judged by today's rules.
    QSet<QString> heldUploads;
    if (track.reimport) {
        const QVector<VideoInfo> waiting = m_db->candidacies(track);
        for (VideoInfo v : waiting) {
            const Page page = knowChannel(v);
            if (page == Page::Cancelled)
                return cleanup();
            if (page == Page::Postponed)
                return postpone();
            heldUploads.insert(v.ytId);
            putBeside(v);
        }
    }
    // B7–B14: the uploads MusicBrainz links as the music video of the
    // track's recording, beside whatever the search returned. Not in judge
    // mode: there the judge picks what is listened to (judge page, S3).
    if (!judging && MusicBrainz::enabled()) {
        const QString question = MusicBrainz::question(track);
        QStringList uploads;
        if (m_db->musicVideosKnown(track, question, &uploads)) {
            logMusicBrainz.insert(QStringLiteral("kept"), true);
        } else {
            report(QStringLiteral("Asking MusicBrainz"));
            QString how, mbError;
            const QStringList recordings = MusicBrainz::recordingsOf(track, &how, &mbError);
            if (mbError.isEmpty() && !recordings.isEmpty())
                uploads = MusicBrainz::musicVideosOf(recordings, &mbError);
            if (m_cancel)
                return cleanup();
            // B13: not a failure of the lookup, which goes on without it.
            if (!mbError.isEmpty())
                uploads.clear();
            m_db->setMusicVideos(track, question, uploads, mbError.isEmpty() ? 30 : 1);
            logMusicBrainz = {{QStringLiteral("by"), how}, {QStringLiteral("recordings"), QJsonArray::fromStringList(recordings)}};
            if (!mbError.isEmpty())
                logMusicBrainz.insert(QStringLiteral("error"), mbError);
        }
        logMusicBrainz.insert(QStringLiteral("uploads"), QJsonArray::fromStringList(uploads));
        QJsonArray passed;
        for (const QString &id : std::as_const(uploads)) {
            if (seen.contains(id)) {
                linked.insert(id);
                continue;
            }
            // One the search did not return: its page says what a search
            // result would. One request, nothing downloaded.
            QJsonObject page;
            QString why;
            const int fromCache = yt.cacheHits(), signedIn = yt.accountRequests();
            ++nPage;
            if (!yt.pageInfo(id, &page, &why)) {
                if (m_cancel)
                    return cleanup();
                // Refused: the lookup waits. Gone or private, or not to be
                // had for another reason: passed over.
                if (!YtDlp::looksUnavailable(why) && noteFailure(why))
                    return postpone();
                passed.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("error"), why.left(160)}});
                continue;
            }
            if (yt.cacheHits() == fromCache && yt.accountRequests() == signedIn)
                noteSuccess();
            const YtDlp::Channel channel = YtDlp::channelOf(page);
            // Listed under several channels: named as the search names it, "A and B".
            QStringList creators;
            for (const QJsonValue &v : page.value(QLatin1String("creators")).toArray())
                creators << v.toString();
            YtCandidate c;
            c.id = id;
            c.title = page.value(QLatin1String("title")).toString();
            c.channel = creators.size() >= 2 && creators.first() == channel.name ? creators.join(QStringLiteral(" and ")) : channel.name;
            c.channelId = channel.id;
            c.verified = channel.verified;
            c.duration = page.value(QLatin1String("duration")).toDouble();
            c.views = page.value(QLatin1String("view_count")).toVariant().toLongLong();
            c.rank = candidates.size();
            seen.insert(id);
            candidates.append(c);
            linked.insert(id);
        }
        if (!passed.isEmpty())
            logMusicBrainz.insert(QStringLiteral("passedOver"), passed);
        beside |= !linked.isEmpty();
    }
    if (beside)
        rankAll();
    QVector<YtCandidate> shortlisted;
    QSet<QString> shortlist;
    QStringList reasons; // why no candidate was taken, for the track's message
    int best = 0; // usable search results taken
    for (const YtCandidate &c : std::as_const(candidates)) {
        const bool isPrior = prior && c.id == prior->ytId;
        const bool fromMusicBrainz = linked.contains(c.id) && usable(c);
        const bool result = judging ? picked.contains(c.id) : (!fromMusicBrainz && returned.contains(c.id) && usable(c) && best < 4);
        if (result)
            ++best;
        // The track's own video is listened to whether it looks official or
        // not: that picks which search results are worth it, and this one is
        // in the library already. A video under review the track is a
        // candidate of is not dropped on sight at all: its length and its
        // channel's size are what rule one out, and that was done when the
        // re-import began (dropOutdatedOptions).
        if (result || fromMusicBrainz || heldUploads.contains(c.id) || (isPrior && c.rejectReason.isEmpty())) {
            shortlisted.append(c);
            shortlist.insert(c.id);
        } else if (isPrior) {
            // Dropped on sight, as it would be today: the rules have changed under it.
            priorRejected = true;
            priorReason = c.rejectReason;
            checked(c, QStringLiteral("dropped"), QStringLiteral("the track's video until now; by today's rules %1").arg(priorReason));
            reasons << QStringLiteral("“%1”, the track's video until now, %2").arg(c.title, priorReason);
        }
        QJsonObject o{{QStringLiteral("id"), c.id}, {QStringLiteral("title"), c.title}, {QStringLiteral("channel"), c.channel},
                      {QStringLiteral("duration"), qRound(c.duration)}, {QStringLiteral("score"), qRound(c.score)},
                      {QStringLiteral("trusted"), c.trusted},
                      // examined by listening: the best four that look official
                      {QStringLiteral("shortlisted"), shortlist.contains(c.id)}};
        if (linked.contains(c.id))
            o.insert(QStringLiteral("musicbrainz"), true);
        if (!c.rejectReason.isEmpty())
            o.insert(QStringLiteral("rejected"), c.rejectReason);
        logCandidates.append(o);
    }
    // A cut of the song (the 90 seconds used as a show's opening) is its
    // video only for want of a full one: uploads of about the track's length
    // are examined first.
    if (judging) {
        // In the judge's order (S3); the track's own video and those under
        // review after its picks (S4).
        std::stable_sort(shortlisted.begin(), shortlisted.end(), [&](const YtCandidate &a, const YtCandidate &b) {
            const int ia = picked.indexOf(a.id), ib = picked.indexOf(b.id);
            return (ia < 0 ? 1000 : ia) < (ib < 0 ? 1000 : ib);
        });
    } else {
        // MusicBrainz's first (C9).
        std::stable_partition(shortlisted.begin(), shortlisted.end(), [&linked](const YtCandidate &c) { return linked.contains(c.id); });
        if (track.duration > 0) {
            std::stable_partition(shortlisted.begin(), shortlisted.end(), [&track](const YtCandidate &c) {
                return c.duration <= 0 || c.duration >= 0.6 * track.duration;
            });
        }
    }
    const auto hearTrack = [&] {
        if (heardTrack)
            return true;
        if (!AudioAlign::decodeMono(track.path, &trackPcm, &m_cancel, &error))
            return false;
        heardTrack = true;
        return true;
    };

    if (shortlisted.isEmpty()) {
        // (S4 in judge mode.) No results at all: YouTube answered the probe,
        // so the search matched nothing (B6).
        finish(QStringLiteral("not_found"), 0,
               (judging ? QStringLiteral("the judge found nothing that looks like the track's official video among %1 results%2")
                              .arg(candidates.size()).arg(pickReason.isEmpty() ? QString() : QStringLiteral(": ") + pickReason)
                        : candidates.isEmpty() ? QStringLiteral("the search matched nothing")
                                               : QStringLiteral("none of %1 search results looked like an official video").arg(candidates.size()))
                   + (priorRejected ? QStringLiteral("; ") + reasons.join(QStringLiteral("; ")) : QString()));
        return;
    }

    // ---- 2. Verify by listening -------------------------------------------
    if (!hearTrack()) {
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
        bool existing = false; // re-import: the track's own video, in its file already
    };
    QVector<Pick> unconfirmed;
    bool anyChecked = false;
    bool undecided = false; // some candidate could not be examined
    // A video that waits for review with other tracks as its candidates,
    // which this track is the song of as well: it becomes one more candidate
    // of it, where nothing fits the track outright.
    struct Join {
        VideoInfo video;
        AudioAlign::Result align; // the track against the upload's own audio
    };
    QVector<Join> joins;

    // Judge mode: what the lookup gathers about each pick for the verdict
    // (V1); nothing is decided until the judge has answered (L6).
    struct JudgeCase {
        YtCandidate c;
        QString dir, audio; // a new download: its folder and its audio
        QJsonObject info;
        AudioAlign::Result align;
        bool song = false;  // L1: the audio says it is the song
        Judge::Upload upload;
        std::optional<VideoInfo> existing; // in the library already: under review, or the track's own (H4, H8)
    };
    QVector<JudgeCase> cases;
    QStringList caseClaims;
    const auto uploadOf = [&](const YtCandidate &c, const YtDlp::Channel &channel, const QString &channelId, const QString &date, qint64 views, int height,
                              const QString &description) {
        Judge::Upload u;
        u.id = c.id;
        u.title = c.title;
        u.channel = channel.several || channel.name.isEmpty() ? c.channel : channel.name;
        if (u.channel.isEmpty())
            u.channel = c.channel;
        u.handle = channel.handle;
        u.verified = channel.verified || c.verified;
        u.subscribers = channel.subscribers;
        u.date = date;
        u.description = description;
        u.views = views > 0 ? views : -1;
        u.allowed = !channelId.isEmpty() && m_db->channelAllowed(channelId);
        u.duration = c.duration;
        u.height = height;
        u.query = cameFrom.value(c.id).first;
        u.rank = cameFrom.value(c.id).second;
        return u;
    };

    // Downloads fail transiently (throttling, expired stream URLs); one more
    // try after a short pause settles most of them.
    // A refusal aimed at this client as a whole is not retried at all.
    auto withRetry = [&](const std::function<bool()> &attempt) {
        // What the cache answered, or what was downloaded from a remembered
        // description, did not open the video's page: no proof either.
        // Nor did a request that only got through signed in show that
        // anonymous ones are let through again.
        const int fromCache = yt.cacheHits(), signedIn = yt.accountRequests();
        const auto proven = [&] {
            if (yt.cacheHits() == fromCache && yt.accountRequests() == signedIn)
                noteSuccess();
        };
        if (attempt()) {
            proven();
            return true;
        }
        if (m_cancel || YtDlp::looksBlocked(error)) {
            noteFailure(error);
            return false;
        }
        for (int i = 0; i < 30 && !m_cancel; ++i)
            QThread::msleep(100);
        if (!m_cancel && attempt()) {
            proven();
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

        if (auto existing = m_db->videoByYtId(c.id)) {
            const bool theTracks = prior && existing->id == prior->id;
            const bool candidateOf = existing->review && held.contains(existing->id);
            if (candidateOf) {
                const Page page = knowChannel(*existing);
                if (page == Page::Cancelled)
                    return cleanup();
                if (page == Page::Postponed)
                    return postpone();
            }
            // A channel the user denied (known by now, where the library had
            // not kept its id when it was denied), or a small one.
            const bool denied = m_db->channelDenied(existing->ytChannelId);
            if (denied || (!judging && smallChannel(existing->ytChannelSubs, existing->ytChannelVerified,
                                                    m_db->channelAllowed(existing->ytChannelId) || linked.contains(c.id), cfg))) {
                const QString small = denied ? QStringLiteral("its channel is one you denied") : smallChannelReason(existing->ytChannelSubs, cfg);
                const QString result = denied ? QStringLiteral("denied-channel") : QStringLiteral("small-channel");
                anyChecked = true;
                if (theTracks) {
                    priorRejected = true;
                    priorReason = small;
                    checked(c, result, QStringLiteral("the track's video until now; by today's rules %1").arg(small));
                } else if (candidateOf) {
                    // The track is no longer a candidate of it; with none left it goes.
                    m_db->removeCandidate(existing->id, track);
                    held.remove(existing->id);
                    const bool last = m_db->candidateCount(existing->id) == 0;
                    checked(c, result, QStringLiteral("the track was a candidate of it; %1").arg(small));
                    appendLog({{QStringLiteral("event"), QStringLiteral("outdated-option")}, {QStringLiteral("videoId"), existing->id},
                               {QStringLiteral("id"), existing->ytId}, {QStringLiteral("title"), existing->ytTitle},
                               {QStringLiteral("channel"), existing->ytChannel}, {QStringLiteral("reason"), small},
                               {QStringLiteral("trackId"), track.id}, {QStringLiteral("track"), track.title}, {QStringLiteral("deleted"), last}});
                    if (last)
                        discardVideo(*existing);
                    else
                        emit videoChanged(existing->id);
                } else {
                    checked(c, result, QStringLiteral("already in the library; %1").arg(small));
                }
                reasons << (denied ? QStringLiteral("“%1” is from a channel you denied").arg(c.title)
                                   : QStringLiteral("“%1” is from a small channel (%2 subscribers)").arg(c.title).arg(existing->ytChannelSubs));
                continue;
            }
            if (judging && (theTracks || existing->review)) {
                // The track's own video (H4) and a video under review (H8,
                // D5) are the judge's to weigh, from their files (L5).
                if (existing->review && !candidateOf) {
                    const Page page = knowChannel(*existing);
                    if (page == Page::Cancelled)
                        return cleanup();
                    if (page == Page::Postponed)
                        return postpone();
                }
                std::vector<int16_t> pcm;
                if (!QFile::exists(existing->path) || !AudioAlign::decodeMono(existing->path, &pcm, &m_cancel, &error, existing->ytStream)) {
                    if (m_cancel)
                        return cleanup();
                    checked(c, QStringLiteral("error"), QStringLiteral("in the library already, but its file cannot be read: %1").arg(error));
                    undecided = true;
                    reasons << QStringLiteral("“%1”: its file cannot be read").arg(c.title);
                    continue;
                }
                anyChecked = true;
                JudgeCase jc;
                jc.c = c;
                jc.align = AudioAlign::align(trackPcm, pcm);
                jc.song = audioMatches(jc.align);
                jc.existing = existing;
                YtDlp::Channel channel;
                channel.id = existing->ytChannelId;
                channel.name = existing->ytChannel;
                channel.subscribers = existing->ytChannelSubs;
                channel.verified = existing->ytChannelVerified;
                channel.handle = existing->ytChannelHandle;
                jc.upload = uploadOf(c, channel, existing->ytChannelId, existing->ytDate, -1, existing->height, existing->ytDescription);
                jc.upload.width = existing->width;
                jc.upload.audio = Judge::audioText(jc.align);
                report(QStringLiteral("Looking at the picture"), -1, c.title);
                jc.upload.picture = Judge::frames(existing->path, existing->duration, QDir(workDir).filePath(c.id + QStringLiteral("/frames")), &m_cancel);
                if (m_cancel)
                    return cleanup();
                qInfo().noquote() << QStringLiteral("[align] %1 ~ “%2” [%3] (%4): %5").arg(track.title, c.title, c.id, theTracks ? QStringLiteral("its video until now") : QStringLiteral("under review"), jc.align.summary());
                checked(c, QStringLiteral("judged"), QStringLiteral("%1; for the judge: %2").arg(theTracks ? QStringLiteral("the track's video until now") : QStringLiteral("under review"),
                                                                                            jc.song ? QStringLiteral("the song by its fingerprints") : mismatchReason(jc.align)), &jc.align);
                cases.append(jc);
                keepClaim = true;
                caseClaims << c.id;
                continue;
            }
            if (theTracks) {
                // Re-import: the track's own video, judged like a candidate
                // just found. The upload's audio is in its file (second
                // stream where the library's was put in), and so is the picture.
                std::vector<int16_t> pcm;
                if (!AudioAlign::decodeMono(existing->path, &pcm, &m_cancel, &error, existing->ytStream)) {
                    if (m_cancel)
                        return cleanup();
                    checked(c, QStringLiteral("error"), QStringLiteral("the track's video until now; its file cannot be read: %1").arg(error));
                    undecided = true;
                    reasons << QStringLiteral("“%1”: %2").arg(c.title, error);
                    continue;
                }
                anyChecked = true;
                const AudioAlign::Result ar = AudioAlign::align(trackPcm, pcm);
                qInfo().noquote() << QStringLiteral("[align] %1 ~ “%2” [%3] (its video until now): %4").arg(track.title, c.title, c.id, ar.summary());
                auto turnDown = [&](const QString &result, const QString &reason) {
                    priorRejected = true;
                    priorReason = reason;
                    checked(c, result, QStringLiteral("the track's video until now; by today's rules %1").arg(reason), &ar);
                    reasons << QStringLiteral("“%1”, the track's video until now, %2").arg(c.title, reason);
                };
                if (!audioMatches(ar)) {
                    turnDown(QStringLiteral("different"), mismatchReason(ar));
                    continue;
                }
                // (An upload the user accepted counts as on the artist's channel.)
                const bool artistChannel = c.ownChannel || existing->vouched
                    || ArtistChannels::isArtistChannel(*m_db, Matcher::artistNames(track), c.channelId, c.channel);
                // D1.13, D1.14: taken outright only as the track's own
                // recording; the song otherwise waits for a verdict,
                // whatever audio the track has.
                const bool forReview = !ownRecording(ar, track, c.title, artistChannel);
                if (cfg.skipStillImages) {
                    report(QStringLiteral("Checking video"), -1, c.title);
                    Muxer::StillCheck check;
                    const QJsonObject known = yt.note(c.id, QStringLiteral("picture"));
                    if (known.value(QLatin1String("samples")).toInt() > 0) {
                        check.valid = true;
                        check.still = known.value(QLatin1String("still")).toBool();
                        check.movingShare = known.value(QLatin1String("movingShare")).toDouble();
                        check.samples = known.value(QLatin1String("samples")).toInt();
                    } else {
                        // The full-quality file, so by its keyframes.
                        check = Muxer::checkStill(existing->path, true, &m_cancel);
                        if (m_cancel)
                            return cleanup();
                        if (check.valid) {
                            yt.setNote(c.id, QStringLiteral("picture"),
                                       {{QStringLiteral("still"), check.still}, {QStringLiteral("movingShare"), check.movingShare},
                                        {QStringLiteral("samples"), check.samples}});
                        }
                    }
                    if (!check.valid) {
                        checked(c, QStringLiteral("error"), QStringLiteral("the track's video until now; its picture could not be analysed"), &ar);
                        undecided = true;
                        reasons << QStringLiteral("“%1”: could not analyse the picture").arg(c.title);
                        continue;
                    }
                    if (check.still) {
                        turnDown(QStringLiteral("still"),
                                 QStringLiteral("a still image: %1% of %2 sampled frame pairs change").arg(qRound(check.movingShare * 100)).arg(qMax(0, check.samples - 1)));
                        continue;
                    }
                }
                if (forReview) {
                    checked(c, QStringLiteral("unconfirmed"),
                            QStringLiteral("the track's video until now; the song by its fingerprints, but %1")
                                .arg(unconfirmedReason(ar, track, c.title, c.channel, artistChannel)),
                            &ar, {{QStringLiteral("youtubeKbps"), qRound(existing->ytAbr)}});
                    reasons << QStringLiteral("“%1” could not be confirmed as this recording").arg(c.title);
                    unconfirmed.append({c, QString(), QString(), QJsonObject(), ar, true});
                    continue;
                }
                checked(c, QStringLiteral("match"),
                        QStringLiteral("the track's video until now, and by today's rules outright: %1% of it is demonstrably the same waveform")
                            .arg(qRound(100 * waveformShare(ar))),
                        &ar, {{QStringLiteral("youtubeKbps"), qRound(existing->ytAbr)}});
                chosen = c;
                chosenDir.clear();
                ytAudio.clear();
                ytInfo = QJsonObject();
                alignment = ar;
                keepClaim = true;
                break;
            }
            // Whose channel: by its name, by MusicBrainz, or by the user's verdicts.
            const bool knownExisting = ArtistChannels::isArtistChannel(*m_db, Matcher::artistNames(track), c.channelId, c.channel);
            const bool vouchedExisting = c.ownChannel || knownExisting;
            // Heard on the upload's own audio, which every file keeps.
            std::vector<int16_t> pcm;
            if (!QFile::exists(existing->path) || !AudioAlign::decodeMono(existing->path, &pcm, &m_cancel, &error, existing->ytStream)) {
                if (m_cancel)
                    return cleanup();
                checked(c, QStringLiteral("error"), QStringLiteral("already in the library, but its file cannot be read: %1").arg(error));
                reasons << QStringLiteral("“%1”: its file cannot be read").arg(c.title);
                continue;
            }
            anyChecked = true;
            const AudioAlign::Result ar = AudioAlign::align(trackPcm, pcm);
            qInfo().noquote() << QStringLiteral("[align] %1 ~ “%2” [%3] (in the library): %4").arg(track.title, c.title, c.id, ar.summary());
            // The track is the better audio: the video is to play it (F3).
            const bool own = cfg.replaceAudio && libraryIsBetter(track, storedYoutubeQuality(*existing));
            const bool recording = audioMatches(ar) && ownRecording(ar, track, existing->ytTitle, vouchedExisting);

            if (existing->review) {
                const bool mine = held.contains(existing->id);
                if (!audioMatches(ar)) {
                    checked(c, mine ? QStringLiteral("option") : QStringLiteral("different"),
                            mine ? QStringLiteral("the track is a candidate of it already; still: %1").arg(mismatchReason(ar))
                                 : QStringLiteral("waits for review with other tracks as its candidates; %1").arg(mismatchReason(ar)),
                            &ar);
                    if (!mine)
                        reasons << QStringLiteral("“%1” is a different recording").arg(c.title);
                    continue;
                }
                if (recording) {
                    // It waits for a verdict as the video of tracks that are
                    // only the song (a live take, looked up first; or this
                    // track, under yesterday's rules). This track is
                    // demonstrably its recording: the video is this track's,
                    // and no longer a question. It plays the track's audio
                    // where that is the better one, YouTube's otherwise (F3).
                    const QVector<Database::Candidate> were = m_db->candidates(existing->id);
                    if (own ? putLibraryAudioIn(*existing, track, ar, cfg, &error) : keepYoutubeAudio(*existing, track, &error)) {
                        releaseCandidates(existing->id, were, &track,
                                          QStringLiteral("“%1” is the video of “%2” [%3]").arg(c.title, track.title, track.album));
                        checked(c, QStringLiteral("match"),
                                (mine ? QStringLiteral("the track was a candidate of it; it now fits outright: %1% of it is demonstrably the same waveform")
                                      : QStringLiteral("was waiting for review with other tracks as its candidates; this one is its recording (%1% the same waveform) and takes it"))
                                    .arg(qRound(100 * waveformShare(ar))),
                                &ar);
                        leaving = m_db->candidacies(track);
                        finish(QStringLiteral("done"), existing->id,
                               mine ? QStringLiteral("%1 (from review)").arg(c.title) : QStringLiteral("takes “%1” out of review").arg(c.title));
                        return;
                    }
                    if (m_cancel)
                        return cleanup();
                }
                if (mine) {
                    checked(c, QStringLiteral("option"),
                            QStringLiteral("the track is a candidate of it already; still: %1")
                                .arg(unconfirmedReason(ar, track, existing->ytTitle, existing->ytChannel, vouchedExisting)),
                            &ar);
                    continue;
                }
                // Whose video it is, of its candidates, is for the user to
                // say: this track is one more of them, where nothing fits it
                // outright.
                QStringList theirs;
                for (const Database::Candidate &o : m_db->candidates(existing->id))
                    theirs << o.track.title;
                checked(c, QStringLiteral("candidate"),
                        QStringLiteral("waits for review with “%1” as its candidate, and this track is the song too: one more candidate of it if nothing fits this track outright")
                            .arg(theirs.join(QStringLiteral("”, “"))),
                        &ar);
                reasons << QStringLiteral("“%1” waits for your review").arg(c.title);
                joins.append({*existing, ar});
                continue;
            }

            // In the library: another track has it, and a video has one
            // track. This one takes it only as the closer fit to the upload.
            if (!audioMatches(ar)) {
                checked(c, QStringLiteral("different"),
                        QStringLiteral("already in the library as the video of “%1”; %2").arg(existing->title, mismatchReason(ar)), &ar);
                reasons << QStringLiteral("“%1” is a different recording").arg(c.title);
                continue;
            }
            QHash<qint64, TrackInfo> holders; // one file per recording: its best
            for (const TrackInfo &o : m_db->tracksForVideo(existing->id)) {
                if (Database::trackKey(o) == Database::trackKey(track))
                    continue;
                const auto it = holders.find(Database::trackKey(o));
                if (it == holders.end())
                    holders.insert(Database::trackKey(o), o);
                else if (betterSource(o, *it))
                    *it = o;
            }
            double theirFit = -1;
            QString theirTitle = existing->title;
            for (const TrackInfo &o : std::as_const(holders)) {
                std::vector<int16_t> oPcm;
                QString oError;
                // A track that cannot be read keeps what it has.
                const double fit = AudioAlign::decodeMono(o.path, &oPcm, &m_cancel, &oError) ? AudioAlign::align(oPcm, pcm).likeness : 1.0;
                if (m_cancel)
                    return cleanup();
                if (fit > theirFit) {
                    theirFit = fit;
                    theirTitle = o.title;
                }
            }
            // Not a video its length rules out for this track, nor a cut of the song.
            QString unfit = Matcher::lengthMismatch(track, existing->duration);
            if (unfit.isEmpty() && shortCut(ar))
                unfit = QStringLiteral("the video is a cut of the song: %1 s of %2 s").arg(qRound(ar.videoSec)).arg(qRound(ar.trackSec));
            // One that no track in the music folders has is this track's on
            // the terms of an upload just found (D1.13, D1.14).
            const bool takes = unfit.isEmpty() && (holders.isEmpty() ? recording : ar.likeness > theirFit);
            if (takes) {
                // The video plays this track's audio where it is the better
                // one and demonstrably the recording; otherwise YouTube's.
                bool ok = true;
                if (own && audioReplaceable(ar))
                    ok = putLibraryAudioIn(*existing, track, ar, cfg, &error);
                else if (existing->audioSource == QLatin1String("library"))
                    ok = keepYoutubeAudio(*existing, track, &error);
                if (m_cancel)
                    return cleanup();
                if (!ok) {
                    checked(c, QStringLiteral("error"), QStringLiteral("this track is the closer fit to it, but its file could not be made over: %1").arg(error), &ar);
                    undecided = true;
                    reasons << QStringLiteral("“%1”: %2").arg(c.title, error);
                    continue;
                }
                // It is listed under the track that has it.
                if (auto v = m_db->video(existing->id)) {
                    v->title = track.title;
                    v->artist = track.artist;
                    v->albumArtist = track.albumArtist;
                    v->album = track.album;
                    v->genre = track.genre;
                    v->year = track.year;
                    v->trackNo = track.trackNo;
                    v->tags = track.tags;
                    m_db->updateVideoTags(*v);
                }
                // The track that had it has no video, and is looked up again.
                const QVector<TrackInfo> out = m_db->unlinkOthers(existing->id, track);
                const QString fits = holders.isEmpty() ? QString()
                    : QStringLiteral("%1% against %2%").arg(100 * ar.likeness, 0, 'f', 1).arg(100 * theirFit, 0, 'f', 1);
                checked(c, QStringLiteral("takeover"),
                        holders.isEmpty() ? QStringLiteral("already in the library, and no track in the music folders has it: this track's")
                                          : QStringLiteral("already in the library as the video of “%1”, but this track is the closer fit to the upload: %2. “%1” goes back to the queue")
                                                .arg(theirTitle, fits),
                        &ar, {{QStringLiteral("fit"), std::round(ar.likeness * 1000) / 1000},
                              {QStringLiteral("holderFit"), std::round(theirFit * 1000) / 1000}});
                if (!holders.isEmpty()) {
                    qInfo().noquote() << QStringLiteral("[import] “%1” [%2] takes “%3” from “%4”: the closer fit, %5")
                                             .arg(track.title, track.album, c.title, theirTitle, fits);
                    QJsonArray names;
                    for (const TrackInfo &o : out)
                        names.append(QJsonObject{{QStringLiteral("trackId"), o.id}, {QStringLiteral("track"), o.title}, {QStringLiteral("album"), o.album}});
                    appendLog({{QStringLiteral("event"), QStringLiteral("takeover")}, {QStringLiteral("videoId"), existing->id},
                               {QStringLiteral("id"), existing->ytId}, {QStringLiteral("title"), existing->ytTitle},
                               {QStringLiteral("trackId"), track.id}, {QStringLiteral("track"), track.title}, {QStringLiteral("album"), track.album},
                               {QStringLiteral("fit"), std::round(ar.likeness * 1000) / 1000},
                               {QStringLiteral("holderFit"), std::round(theirFit * 1000) / 1000}, {QStringLiteral("requeued"), names}});
                }
                emit videoChanged(existing->id);
                leaving = m_db->candidacies(track);
                finish(QStringLiteral("done"), existing->id,
                       holders.isEmpty() ? c.title : QStringLiteral("takes “%1” from “%2”: the closer fit (%3)").arg(c.title, theirTitle, fits));
                if (!out.isEmpty())
                    requeuePending();
                return;
            }
            if (holders.isEmpty() && unfit.isEmpty()) {
                // No track has it, and this one is the song without being
                // demonstrably its recording: a card with this track as its
                // candidate (D1.14), as a new upload would be.
                if (!addCandidate(*existing, track, ar, cfg, &error)) {
                    if (m_cancel)
                        return cleanup();
                    checked(c, QStringLiteral("error"), QStringLiteral("already in the library with no track; could not be made over for review: %1").arg(error), &ar);
                    undecided = true;
                    reasons << QStringLiteral("“%1”: %2").arg(c.title, error);
                    continue;
                }
                m_db->setVideoReview(existing->id, true);
                emit videoChanged(existing->id);
                checked(c, QStringLiteral("candidate"),
                        QStringLiteral("already in the library, and no track in the music folders has it; this track is the song but %1: it waits for your verdict with this track as its candidate")
                            .arg(unconfirmedReason(ar, track, existing->ytTitle, existing->ytChannel, vouchedExisting)),
                        &ar);
                reasons << QStringLiteral("“%1” waits for your review").arg(c.title);
                continue;
            }
            checked(c, QStringLiteral("has-its-track"),
                    !unfit.isEmpty() ? QStringLiteral("already in the library as the video of “%1”, and not one for this track: %2").arg(theirTitle, unfit)
                    : QStringLiteral("already in the library as the video of “%1”, which is the closer fit to the upload: %2% against this track's %3%")
                          .arg(theirTitle).arg(100 * theirFit, 0, 'f', 1).arg(100 * ar.likeness, 0, 'f', 1),
                    &ar, {{QStringLiteral("fit"), std::round(ar.likeness * 1000) / 1000},
                          {QStringLiteral("holderFit"), std::round(theirFit * 1000) / 1000}});
            reasons << QStringLiteral("“%1” is the video of “%2”").arg(c.title, theirTitle);
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
            // A video that is gone is no candidate; that is not a failure
            // of the lookup, which would have the track tried over and over.
            if (YtDlp::looksUnavailable(error)) {
                anyChecked = true;
                checked(c, QStringLiteral("unavailable"), QStringLiteral("the video is not available: %1").arg(error.left(160)));
                reasons << QStringLiteral("“%1” is not available").arg(c.title);
                continue;
            }
            checked(c, QStringLiteral("error"), QStringLiteral("its audio could not be downloaded: %1").arg(error));
            if (m_blocked)
                return postpone();
            // Could not listen to it: that says nothing about the video.
            undecided = true;
            reasons << QStringLiteral("%1: %2").arg(c.id, error);
            continue;
        }
        const YtDlp::Channel channel = YtDlp::channelOf(info, c.channel);
        const QString channelId = c.channelId.isEmpty() ? channel.id : c.channelId;
        if (m_db->channelDenied(channelId)) {
            // (The search results did not say whose upload it is.)
            anyChecked = true;
            checked(c, QStringLiteral("denied-channel"), QStringLiteral("its channel is one you denied"));
            reasons << QStringLiteral("“%1” is from a channel you denied").arg(c.title);
            QDir(dir).removeRecursively();
            continue;
        }
        if (!judging && smallChannel(channel.subscribers, channel.verified || c.verified, m_db->channelAllowed(channelId) || linked.contains(c.id), cfg)) {
            anyChecked = true;
            checked(c, QStringLiteral("small-channel"), smallChannelReason(channel.subscribers, cfg), nullptr,
                    {{QStringLiteral("subscribers"), channel.subscribers}});
            reasons << QStringLiteral("“%1” is from a small channel (%2 subscribers)").arg(c.title).arg(channel.subscribers);
            QDir(dir).removeRecursively();
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
        const AudioAlign::Result ar = AudioAlign::align(trackPcm, mvPcm);
        qInfo().noquote() << QStringLiteral("[align] %1 ~ “%2” [%3]: %4").arg(track.title, c.title, c.id, ar.summary());
        if (!judging && !audioMatches(ar)) {
            checked(c, QStringLiteral("different"), mismatchReason(ar), &ar);
            reasons << QStringLiteral("“%1” is a different recording").arg(c.title);
            QDir(dir).removeRecursively();
            continue;
        }

        // The song, but not demonstrably the track's own recording (R1): a
        // live take, a cover, another mix, a third party's copy. That is for
        // the user to say (D1.14), whatever audio the track has; only the
        // track's own recording on the artist's channel is taken outright
        // (D1.13). (The channel by its id as the upload's page has it, where
        // the search results gave none.)
        const bool artistChannel = c.ownChannel
            || ArtistChannels::isArtistChannel(*m_db, Matcher::artistNames(track), channelId, c.channel);
        const bool forReview = !judging && !ownRecording(ar, track, c.title, artistChannel);

        QString preview; // judge mode: the picture stream, for the frames (L4.1)
        if (cfg.skipStillImages || judging) {
            report(QStringLiteral("Checking video"), -1, c.title);
            // A still image is one for every track that has it as a candidate.
            Muxer::StillCheck check;
            const QJsonObject known = yt.note(c.id, QStringLiteral("picture"));
            if (known.value(QLatin1String("samples")).toInt() > 0) {
                check.valid = true;
                check.still = known.value(QLatin1String("still")).toBool();
                check.movingShare = known.value(QLatin1String("movingShare")).toDouble();
                check.samples = known.value(QLatin1String("samples")).toInt();
            }
            if (!check.valid || (judging && !check.still)) {
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
                if (!check.valid) {
                    check = Muxer::checkStill(preview, false, &m_cancel);
                    if (m_cancel)
                        return cleanup();
                    if (check.valid) {
                        yt.setNote(c.id, QStringLiteral("picture"),
                                   {{QStringLiteral("still"), check.still}, {QStringLiteral("movingShare"), check.movingShare},
                                    {QStringLiteral("samples"), check.samples}});
                    }
                }
                if (!judging)
                    QFile::remove(preview);
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

        if (judging) {
            // Nothing is decided here: the judge is asked once, with all of
            // them (L6).
            JudgeCase jc;
            jc.c = c;
            jc.dir = dir;
            jc.audio = audioFile;
            jc.info = info;
            jc.align = ar;
            jc.song = audioMatches(ar);
            jc.upload = uploadOf(c, channel, channelId, YtDlp::publishedOn(info),
                                 c.views > 0 ? c.views : qint64(info.value(QLatin1String("view_count")).toDouble()), YtDlp::bestHeight(info),
                                 YtDlp::descriptionOf(info));
            jc.upload.audio = Judge::audioText(ar);
            report(QStringLiteral("Looking at the picture"), -1, c.title);
            jc.upload.picture = Judge::frames(preview, c.duration > 0 ? c.duration : ar.videoSec, QDir(dir).filePath(QStringLiteral("frames")), &m_cancel);
            if (m_cancel)
                return cleanup();
            QFile::remove(preview);
            checked(c, QStringLiteral("judged"), QStringLiteral("for the judge: %1").arg(jc.song ? QStringLiteral("the song by its fingerprints") : mismatchReason(ar)), &ar,
                    {{QStringLiteral("youtubeKbps"), qRound(YtDlp::audioKbps(info))}, {QStringLiteral("pictureChanged"), std::round(jc.upload.picture.changed * 1000) / 1000}});
            cases.append(jc);
            keepClaim = true;
            caseClaims << c.id;
            continue;
        }

        if (forReview) {
            checked(c, QStringLiteral("unconfirmed"),
                    QStringLiteral("the song by its fingerprints, but %1").arg(unconfirmedReason(ar, track, c.title, c.channel, artistChannel)),
                    &ar, {{QStringLiteral("youtubeKbps"), qRound(YtDlp::audioKbps(info))}});
            reasons << QStringLiteral("“%1” could not be confirmed as this recording").arg(c.title);
            unconfirmed.append({c, dir, audioFile, info, ar});
            continue;
        }

        checked(c, QStringLiteral("match"),
                QStringLiteral("the track's recording: %1% of it is demonstrably the same waveform").arg(qRound(100 * waveformShare(ar))),
                &ar, {{QStringLiteral("youtubeKbps"), qRound(YtDlp::audioKbps(info))}});
        chosen = c;
        chosenDir = dir;
        ytAudio = audioFile;
        ytInfo = info;
        alignment = ar;
        keepClaim = true;
        break;
    }

    // ---- V. The judge's verdict; W. what the track gets -----------------------
    bool judgeReview = false; // W2, W4: cards under review whatever the audio says
    bool judgeVouch = false;  // W1: the verdict vouches for the upload (R5)
    QHash<QString, Database::Advice> adviceById;
    QString judgeSays;
    if (judging) {
        // (Claimed again below, as every lookup claims what it brings in.)
        for (const QString &id : std::as_const(caseClaims))
            releaseVideo(id);
        if (cases.isEmpty()) {
            // W5: no pick could be examined; or every one was turned down for
            // the track already.
            const bool open = undecided || !anyChecked;
            finish(open ? QStringLiteral("failed") : QStringLiteral("not_found"), 0, reasons.join(QStringLiteral("; ")));
            return;
        }
        report(QStringLiteral("Asking the judge for its verdict"));
        QVector<Judge::Upload> uploads;
        QJsonArray logUploads;
        QVector<TrackInfo> others;
        QSet<qint64> otherKeys{Database::trackKey(track)};
        for (const JudgeCase &jc : std::as_const(cases)) {
            uploads.append(jc.upload);
            logUploads.append(QJsonObject{{QStringLiteral("id"), jc.c.id}, {QStringLiteral("title"), jc.c.title}, {QStringLiteral("channel"), jc.upload.channel},
                                          {QStringLiteral("duration"), qRound(jc.c.duration)}, {QStringLiteral("song"), jc.song},
                                          {QStringLiteral("audio"), jc.upload.audio}, {QStringLiteral("picture"), Judge::pictureText(jc.upload.picture)},
                                          {QStringLiteral("existing"), jc.existing ? jc.existing->id : 0}});
            if (jc.existing && jc.existing->review) {
                for (const Database::Candidate &o : m_db->candidates(jc.existing->id)) {
                    if (!otherKeys.contains(Database::trackKey(o.track))) {
                        otherKeys.insert(Database::trackKey(o.track));
                        others.append(o.track);
                    }
                }
            }
        }
        const Judge::Answer v = Judge::ask(judgeKey, judgeModel, QString::fromUtf8(JudgeWords::kVerdictWords), Judge::verdictContent(track, others, uploads),
                                           QString::fromUtf8(JudgeWords::kVerdictSchema), 4000, cacheDir(cfg.mvDir), &m_cancel, cfg.judgeProvider, cfg.chatGptAuthFile, cfg.chatGptAccount);
        if (m_cancel)
            return cleanup();
        logJudge(QStringLiteral("verdict"), v, logUploads);
        if (!v.error.isEmpty())
            return judgePause(v.error);
        const QString verdict = v.json.value(QLatin1String("verdict")).toString();
        const int chosenIdx = v.json.value(QLatin1String("chosen")).toInt();
        const double confidence = v.json.value(QLatin1String("confidence")).toDouble();
        judgeSays = v.json.value(QLatin1String("reason")).toString();
        const bool accept = verdict == QLatin1String("accept") && chosenIdx >= 1 && chosenIdx <= cases.size();
        for (int i = 0; i < cases.size(); ++i) {
            const QJsonObject o = Judge::optionAt(v.json, i + 1);
            Database::Advice ad;
            ad.trackKey = Database::trackKey(track);
            ad.mode = QStringLiteral("judge");
            ad.model = judgeModel;
            ad.keep = o.value(QLatin1String("official_mv")).toBool() || (accept && i + 1 == chosenIdx);
            ad.probability = o.value(QLatin1String("confidence")).toDouble();
            ad.kind = o.value(QLatin1String("kind")).toString();
            ad.source = o.value(QLatin1String("source")).toString();
            ad.version = o.value(QLatin1String("version")).toString();
            ad.whole = o.value(QLatin1String("whole")).toString();
            ad.note = o.value(QLatin1String("note")).toString();
            ad.askedAt = QDateTime::currentSecsSinceEpoch();
            adviceById.insert(cases[i].c.id, ad);
        }
        qInfo().noquote() << QStringLiteral("[judge] %1 — %2: %3 %4 (%5): %6").arg(st.artist, st.title, verdict).arg(chosenIdx).arg(confidence, 0, 'f', 2).arg(judgeSays);
        // The gates (L1, L2, L4.1): between outright and your verdict.
        const auto passes = [&](const JudgeCase &jc) {
            return jc.song && jc.upload.picture.valid && !shortCut(jc.align) && !interrupted(jc.align)
                && Matcher::lengthMismatch(track, jc.align.videoSec > 0 ? jc.align.videoSec : jc.c.duration).isEmpty();
        };
        const auto dropCase = [&](const JudgeCase &jc) {
            if (!jc.dir.isEmpty())
                QDir(jc.dir).removeRecursively();
        };
        bool outright = false;
        if (accept && confidence >= Judge::kGate && passes(cases[chosenIdx - 1])) {
            // W1: the track's video.
            const JudgeCase &jc = cases[chosenIdx - 1];
            if (!jc.existing || (prior && jc.existing->id == prior->id)) {
                if (!claimVideo(jc.c.id))
                    return cleanup();
                chosen = jc.c;
                chosenDir = jc.dir;
                ytAudio = jc.audio;
                ytInfo = jc.info;
                alignment = jc.align;
                judgeVouch = true;
                outright = true;
            } else if (jc.existing->review && cfg.replaceAudio && audioReplaceable(jc.align) && libraryIsBetter(track, storedYoutubeQuality(*jc.existing))) {
                // Under review with other tracks as its candidates: this
                // track's, and no longer a question (D5.6 as a lookup has it).
                if (!claimVideo(jc.c.id))
                    return cleanup();
                const QVector<Database::Candidate> were = m_db->candidates(jc.existing->id);
                if (putLibraryAudioIn(*jc.existing, track, jc.align, cfg, &error)) {
                    releaseCandidates(jc.existing->id, were, &track, QStringLiteral("“%1” is the video of “%2” [%3]").arg(jc.c.title, track.title, track.album));
                    for (const JudgeCase &other : std::as_const(cases))
                        dropCase(other);
                    leaving = m_db->candidacies(track);
                    emit videoChanged(jc.existing->id);
                    emit judgeDecided(track.id, true);
                    finish(QStringLiteral("done"), jc.existing->id, QStringLiteral("takes “%1” out of review — the judge: %2").arg(jc.c.title, judgeSays));
                    releaseVideo(jc.c.id);
                    return;
                }
                releaseVideo(jc.c.id);
                if (m_cancel)
                    return cleanup();
                // Could not be made over: a card with this track as a candidate instead (W2).
            }
            // (A video under review that would keep YouTube's audio becomes
            // a card with this track as a candidate, W2: taking it out of
            // review for a track whose audio it cannot carry is your call.)
        }
        if (outright) {
            for (int i = 0; i < cases.size(); ++i) {
                if (i + 1 != chosenIdx)
                    dropCase(cases[i]);
            }
            emit judgeDecided(track.id, true);
        } else if (verdict == QLatin1String("reject all") && confidence >= Judge::kGate) {
            // W3: no video; the uploads are remembered as turned down for
            // this track (D0), and a card loses it as a candidate.
            for (const JudgeCase &jc : std::as_const(cases)) {
                if (!jc.existing) {
                    m_db->rejectVideoFor(track, jc.c.id);
                    dropCase(jc);
                } else if (prior && jc.existing->id == prior->id) {
                    priorRejected = true;
                    priorReason = QStringLiteral("the judge: %1").arg(judgeSays);
                } else if (jc.existing->review && held.contains(jc.existing->id)) {
                    m_db->removeCandidate(jc.existing->id, track);
                    held.remove(jc.existing->id);
                    if (m_db->candidateCount(jc.existing->id) == 0)
                        discardVideo(*jc.existing);
                    else
                        emit videoChanged(jc.existing->id);
                }
            }
            emit judgeDecided(track.id, true);
            finish(QStringLiteral("not_found"), 0, QStringLiteral("the judge: %1").arg(judgeSays));
            return;
        } else {
            // W2, W4: cards under review, each with the judge's advice (X2).
            // Accepted below the gate (or gated): the uploads it would keep;
            // rejected below the gate, or unsure: those that are the song.
            judgeReview = true;
            for (int i = 0; i < cases.size(); ++i) {
                const JudgeCase &jc = cases[i];
                const bool card = accept ? adviceById.value(jc.c.id).keep : jc.song;
                if (!card) {
                    dropCase(jc);
                    continue;
                }
                if (!jc.existing)
                    unconfirmed.append({jc.c, jc.dir, jc.audio, jc.info, jc.align, false});
                else if (prior && jc.existing->id == prior->id)
                    unconfirmed.append({jc.c, QString(), QString(), QJsonObject(), jc.align, true});
                else if (jc.existing->review)
                    joins.append({*jc.existing, jc.align});
            }
            if (unconfirmed.isEmpty() && joins.isEmpty()) {
                finish(QStringLiteral("not_found"), 0, QStringLiteral("the judge: %1").arg(judgeSays));
                return;
            }
            emit judgeDecided(track.id, false);
        }
    }

    QVector<Pick> picks;
    const bool forReview = chosen.id.isEmpty();
    if (!forReview) {
        picks.append({chosen, chosenDir, ytAudio, ytInfo, alignment, prior && chosen.id == prior->ytId});
    } else {
        picks = unconfirmed;
        // The track's own video first: it is a candidate whatever else turns up.
        std::stable_partition(picks.begin(), picks.end(), [](const Pick &p) { return p.existing; });
    }

    if (picks.isEmpty() && joins.isEmpty()) {
        // Nothing new: the track stays a candidate of the videos it was one
        // of (finish sees to that). Otherwise "not found" is a verdict and
        // is not revisited for weeks. If any candidate could not be examined
        // (a download error, typically YouTube throttling), the answer is
        // still open: retry later.
        const bool open = undecided || !anyChecked;
        finish(open ? QStringLiteral("failed") : QStringLiteral("not_found"), 0, reasons.join(QStringLiteral("; ")));
        return;
    }

    // Brings one video into the library. Stopped: cancelled or postponed, and
    // already dealt with.
    enum class Got { Ok, Failed, Stopped };
    // `review`: whether it came in for review, or (out) in the end did not.
    const auto bringIn = [&](Pick pick, bool *review, qint64 *videoIdOut, QString *summary) -> Got {
    bool needsReview = *review;
    if (pick.existing) {
        // Re-import: the track's video is what today's rules would bring in.
        // Its file stays; what may change is whether it waits for review,
        // and whether the track's audio is in it.
        const VideoInfo v = *prior;
        const double ytQuality = storedYoutubeQuality(v);
        AudioAlign::Result al = pick.align;
        const bool better = cfg.replaceAudio && libraryIsBetter(track, ytQuality);
        const bool replace = better && audioReplaceable(pick.align);
        // A card plays the track laid whole at the offset where the loudness
        // agrees, where the track is the better audio (F3).
        if (needsReview && !judgeReview && better)
            al = placedWhole(pick.align);
        if (needsReview && (judgeReview ? !replace : !al.byOffset)) {
            // A card with YouTube's audio: the track's cannot be laid in
            // (the judge's, L1), or it is the lesser one.
            report(QStringLiteral("Preparing for review"), -1, pick.c.title);
            m_db->setVideoReview(v.id, true);
            m_db->addCandidate(v.id, track, v.audioSource == QLatin1String("library") ? 1 : 0, qMax(pick.align.likeness, 1e-6), v.audioDetail);
            if (!m_db->unlinkOthers(v.id, track).isEmpty())
                requeuePending();
            priorFate = QStringLiteral("review");
            priorKept = true;
            *review = true;
            *videoIdOut = v.id;
            *summary = QStringLiteral("%1 · kept, now for review").arg(pick.c.title);
            return Got::Ok;
        }
        if (needsReview) {
            // It goes back to the queue, with this track as its candidate.
            report(QStringLiteral("Preparing for review"), -1, pick.c.title);
            if (!putLibraryAudioIn(v, track, al, cfg, &error, true)) {
                if (m_cancel) {
                    cleanup();
                    return Got::Stopped;
                }
                return Got::Failed;
            }
            const std::optional<VideoInfo> now = m_db->video(v.id);
            m_db->addCandidate(v.id, track, 1, qMax(pick.align.likeness, 1e-6), now ? now->audioDetail : QString());
            // (A track that has left the music folders may have had it too.)
            if (!m_db->unlinkOthers(v.id, track).isEmpty())
                requeuePending();
            priorFate = QStringLiteral("review");
        } else if (replace && v.audioSource != QLatin1String("library")) {
            report(QStringLiteral("Muxing library audio"), -1, pick.c.title);
            if (!putLibraryAudioIn(v, track, pick.align, cfg, &error, false)) {
                if (m_cancel) {
                    cleanup();
                    return Got::Stopped;
                }
                return Got::Failed;
            }
            priorFate = QStringLiteral("kept");
        } else {
            priorFate = QStringLiteral("kept");
        }
        priorKept = true;
        *review = needsReview;
        *videoIdOut = v.id;
        *summary = QStringLiteral("%1 · kept%2").arg(pick.c.title, needsReview ? QStringLiteral(", now for review") : QString());
        return Got::Ok;
    }
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
    const bool better = cfg.replaceAudio && libraryIsBetter(track, youtubeQuality(ytInfo));
    if (needsReview && !judgeReview && better) {
        // A card plays the track laid whole at the offset where the loudness
        // agrees (F3); YouTube's alone where that cannot be done.
        plan.align = placedWhole(alignment);
        plan.replaceAudio = plan.align.byOffset;
    } else {
        // Taken outright: the track's audio where it is the better one (it
        // is the recording, D1.13). The judge's card: the track's audio
        // beside YouTube's where it can be laid in, YouTube's alone where
        // not (L1).
        plan.replaceAudio = better && audioReplaceable(alignment);
    }

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
        if (fetchSubtitlesFor(yt, chosen.id, outFile, cfg.subtitleLangs, chosenDir, &why) < 0 && !m_cancel) {
            qWarning().noquote() << "[import] no subtitles for" << chosen.id << ":" << why;
            // The video is had; but a refusal here is a refusal all the same.
            if (YtDlp::looksBlocked(why))
                noteFailure(why);
        }
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
    v.ytStream = plan.replaceAudio ? 1 : 0;
    v.audioDetail = audioDetail;
    v.ytAbr = YtDlp::audioKbps(ytInfo);
    v.review = needsReview;
    v.vouched = judgeVouch && !needsReview;
    v.reviewStart = span.start;
    v.reviewEnd = span.end;
    v.spans = span.spans;
    v.ytTitle = chosen.title;
    v.ytChannel = chosen.channel;
    const YtDlp::Channel channel = YtDlp::channelOf(ytInfo, chosen.channel);
    v.ytChannelId = chosen.channelId.isEmpty() ? channel.id : chosen.channelId;
    v.ytChannelSubs = channel.subscribers;
    v.ytChannelVerified = channel.verified || chosen.verified;
    v.ytChannelHandle = channel.handle;
    v.ytDate = YtDlp::publishedOn(ytInfo);
    v.ytDescription = YtDlp::descriptionOf(ytInfo);
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
    // Under review it is a card of its own, with this track as its candidate.
    if (needsReview)
        m_db->addCandidate(videoId, track, plan.replaceAudio ? 1 : 0, qMax(alignment.likeness, 1e-6), audioDetail, span.spans);
    *review = needsReview;
    *videoIdOut = videoId;
    *summary = QStringLiteral("%1 · %2×%3 · %4").arg(chosen.title).arg(pr.width).arg(pr.height).arg(audioDetail);
    return Got::Ok;
    };

    QVector<qint64> added;
    QString firstSummary, lastError;
    // The videos stay claimed until this track's result is recorded. Released
    // sooner, another track could act on one of them in between: take a
    // video out of review before this track was made its candidate.
    QStringList claimed;
    const auto release = qScopeGuard([&] {
        for (const QString &id : std::as_const(claimed))
            releaseVideo(id);
    });
    if (forReview) {
        // All of them first, in one order for every track: two tracks that
        // each hold one video and wait for the other's would wait for ever.
        QStringList ids;
        for (const Pick &pick : std::as_const(picks))
            ids << pick.c.id;
        for (const Join &j : std::as_const(joins)) {
            if (!ids.contains(j.video.ytId))
                ids << j.video.ytId;
        }
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
    // What was brought in goes again when the lookup is cut short: half of
    // what a track could have is no use, and it looks again later. (Its own
    // video stays, as it was before: in review, or not.)
    const auto abandon = [&] {
        for (qint64 id : std::as_const(added)) {
            if (prior && id == prior->id)
                continue;
            if (const auto v = m_db->video(id)) {
                m_db->removeVideo(id);
                removeVideoFiles(*v);
            }
        }
    };
    qint64 outright = 0; // the video that is the track's, accepted
    for (const Pick &pick : std::as_const(picks)) {
        if (forReview && !pick.existing) {
            // Another track brought it in meanwhile. Under review, this
            // track is one more candidate of it; in the library by now, it
            // is that track's.
            if (const auto there = m_db->videoByYtId(pick.c.id)) {
                if (there->review)
                    joins.append({*there, pick.align});
                continue;
            }
        }
        qint64 videoId = 0;
        QString summary;
        bool review = forReview;
        const Got got = bringIn(pick, &review, &videoId, &summary);
        if (got == Got::Stopped) {
            abandon();
            return;
        }
        if (got == Got::Failed && pick.existing) {
            // The track's own video could not be made over for review. That
            // says nothing about it, and the videos brought in beside it
            // would stand in its place: the lookup is left unfinished, and
            // the track keeps its video as it was.
            abandon();
            logVideos = QJsonArray();
            finish(QStringLiteral("failed"), 0, QStringLiteral("could not rebuild its video for review: %1").arg(error));
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
                                     {QStringLiteral("title"), pick.c.title}, {QStringLiteral("review"), review},
                                     {QStringLiteral("summary"), summary}});
        added << videoId;
        if (review && adviceById.contains(pick.c.id)) {
            Database::Advice ad = adviceById.value(pick.c.id);
            ad.videoId = videoId;
            m_db->setAdvice(ad);
        }
        if (!review) {
            // It fits after all (with the account's audio the track is the
            // lesser one): the track's video, and nothing else is needed.
            outright = videoId;
            firstSummary = summary;
            break;
        }
    }
    int joined = 0;
    if (forReview && outright <= 0) {
        for (const Join &j : std::as_const(joins)) {
            if (m_cancel) {
                abandon();
                return cleanup();
            }
            // (Settled by another lookup meanwhile: not a question any more.)
            const std::optional<VideoInfo> there = m_db->video(j.video.id);
            if (!there || !there->review)
                continue;
            report(QStringLiteral("Preparing for review"), -1, there->ytTitle);
            if (!addCandidate(*there, track, j.align, cfg, &error)) {
                if (m_cancel) {
                    abandon();
                    return cleanup();
                }
                lastError = error;
                logVideos.append(QJsonObject{{QStringLiteral("id"), there->ytId}, {QStringLiteral("title"), there->ytTitle},
                                             {QStringLiteral("failed"), error}});
                continue;
            }
            ++joined;
            if (adviceById.contains(there->ytId)) {
                Database::Advice ad = adviceById.value(there->ytId);
                ad.videoId = there->id;
                m_db->setAdvice(ad);
            }
            if (firstSummary.isEmpty())
                firstSummary = there->ytTitle;
            logVideos.append(QJsonObject{{QStringLiteral("videoId"), there->id}, {QStringLiteral("id"), there->ytId},
                                         {QStringLiteral("title"), there->ytTitle}, {QStringLiteral("review"), true},
                                         {QStringLiteral("summary"), QStringLiteral("one more candidate of it")}});
        }
    }
    if (m_cancel) {
        abandon();
        return cleanup();
    }
    if (outright > 0) {
        // A video that fits outright settles it: the track is a candidate
        // of nothing else.
        leaving = m_db->candidacies(track);
        finish(QStringLiteral("done"), outright, judgeSays.isEmpty() ? firstSummary : QStringLiteral("%1 — the judge: %2").arg(firstSummary, judgeSays));
    } else if (added.isEmpty() && joined == 0) {
        // (finish keeps the track waiting on what it is a candidate of.)
        finish(QStringLiteral("failed"), 0, lastError.isEmpty() ? QStringLiteral("could not bring the video in") : lastError);
        return;
    } else {
        const int waiting = int(m_db->candidacies(track).size());
        finish(QStringLiteral("review"), 0,
               QStringLiteral("for your review (%1 %2): %3%4").arg(waiting).arg(waiting == 1 ? QStringLiteral("video") : QStringLiteral("videos"), firstSummary,
                                                               judgeSays.isEmpty() ? QString() : QStringLiteral(" — the judge: ") + judgeSays));
    }
    for (qint64 id : std::as_const(added)) {
        if (prior && id == prior->id)
            emit videoChanged(id);
        else
            emit videoAdded(id);
    }
}
