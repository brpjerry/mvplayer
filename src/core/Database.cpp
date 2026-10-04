#include "core/Database.h"

#include "core/Util.h"

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>

#include <tuple>

namespace {

QAtomicInt g_connCounter;

// Owns the calling thread's connection and unregisters it when the thread exits.
struct ThreadConn {
    QHash<QString, QString> names; // db file -> connection name
    ~ThreadConn()
    {
        // The main thread's connections are torn down with the application.
        if (!QCoreApplication::instance())
            return;
        for (const QString &n : std::as_const(names))
            QSqlDatabase::removeDatabase(n);
    }
};
thread_local ThreadConn t_conn;

QString jsonText(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject jsonObj(const QString &s)
{
    return QJsonDocument::fromJson(s.toUtf8()).object();
}

const char *kTrackCols =
    "id, path, mtime, size, title, artist, album_artist, album, genre, year, track_no, disc_no, "
    "duration, codec, bitrate, sample_rate, bits, channels, lossless, tags_json, state, video_id, "
    "attempts, last_attempt, message, recording";

TrackInfo readTrack(const QSqlQuery &q)
{
    TrackInfo t;
    int i = 0;
    t.id = q.value(i++).toLongLong();
    t.path = q.value(i++).toString();
    t.mtime = q.value(i++).toLongLong();
    t.size = q.value(i++).toLongLong();
    t.title = q.value(i++).toString();
    t.artist = q.value(i++).toString();
    t.albumArtist = q.value(i++).toString();
    t.album = q.value(i++).toString();
    t.genre = q.value(i++).toString();
    t.year = q.value(i++).toInt();
    t.trackNo = q.value(i++).toInt();
    t.discNo = q.value(i++).toInt();
    t.duration = q.value(i++).toDouble();
    t.codec = q.value(i++).toString();
    t.bitrate = q.value(i++).toInt();
    t.sampleRate = q.value(i++).toInt();
    t.bitsPerSample = q.value(i++).toInt();
    t.channels = q.value(i++).toInt();
    t.lossless = q.value(i++).toBool();
    t.tags = jsonObj(q.value(i++).toString());
    t.state = q.value(i++).toString();
    t.videoId = q.value(i++).toLongLong();
    t.attempts = q.value(i++).toInt();
    t.lastAttempt = q.value(i++).toLongLong();
    t.message = q.value(i++).toString();
    t.recording = q.value(i++).toLongLong();
    return t;
}

const char *kVideoCols =
    "id, yt_id, path, thumb, title, artist, album_artist, album, genre, year, track_no, duration, "
    "width, height, fps, vcodec, audio_source, audio_detail, yt_title, yt_channel, tags_json, added_at";

VideoInfo readVideo(const QSqlQuery &q)
{
    VideoInfo v;
    int i = 0;
    v.id = q.value(i++).toLongLong();
    v.ytId = q.value(i++).toString();
    v.path = q.value(i++).toString();
    v.thumb = q.value(i++).toString();
    v.title = q.value(i++).toString();
    v.artist = q.value(i++).toString();
    v.albumArtist = q.value(i++).toString();
    v.album = q.value(i++).toString();
    v.genre = q.value(i++).toString();
    v.year = q.value(i++).toInt();
    v.trackNo = q.value(i++).toInt();
    v.duration = q.value(i++).toDouble();
    v.width = q.value(i++).toInt();
    v.height = q.value(i++).toInt();
    v.fps = q.value(i++).toDouble();
    v.vcodec = q.value(i++).toString();
    v.audioSource = q.value(i++).toString();
    v.audioDetail = q.value(i++).toString();
    v.ytTitle = q.value(i++).toString();
    v.ytChannel = q.value(i++).toString();
    v.tags = jsonObj(q.value(i++).toString());
    v.addedAt = q.value(i++).toLongLong();
    return v;
}

bool run(QSqlQuery &q)
{
    if (q.exec())
        return true;
    qWarning().noquote() << "[db]" << q.lastError().text() << "in" << q.lastQuery();
    return false;
}

} // namespace

Database::Database(const QString &file, const QString &mvDir)
    : m_file(file)
    , m_mvDir(QDir(mvDir).absolutePath())
{
}

// Inside the MV folder: relative to it, with "/" on every platform.
QString Database::storedPath(const QString &path) const
{
    const QString prefix = m_mvDir.endsWith(QLatin1Char('/')) ? m_mvDir : m_mvDir + QLatin1Char('/');
    if (path.startsWith(prefix, pathCase))
        return path.mid(prefix.size());
    return path;
}

QString Database::resolvedPath(const QString &stored) const
{
    if (stored.isEmpty() || QDir::isAbsolutePath(stored))
        return stored;
    return QDir(m_mvDir).filePath(stored);
}

VideoInfo Database::resolved(VideoInfo v) const
{
    v.path = resolvedPath(v.path);
    v.thumb = resolvedPath(v.thumb);
    return v;
}

// Up to 0.1.2 videos were stored by their absolute path. The folder may have
// been moved since, or come from another system, so a path that is not under
// the MV folder is taken by its place in the layout: "<Album Artist>/<file>".
void Database::makeVideoPathsRelative(QSqlDatabase &db)
{
    const auto relative = [this](const QString &old) {
        const QString inside = storedPath(old);
        if (inside != old || old.isEmpty() || !old.contains(QLatin1Char('/')))
            return inside;
        return old.section(QLatin1Char('/'), -2);
    };

    QVector<std::tuple<qint64, QString, QString>> rows;
    QSqlQuery select(db);
    if (select.exec(QStringLiteral("SELECT id, path, thumb FROM videos"))) {
        while (select.next())
            rows.append({select.value(0).toLongLong(), select.value(1).toString(), select.value(2).toString()});
    }
    db.transaction();
    for (const auto &[id, path, thumb] : rows) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("UPDATE videos SET path = ?, thumb = ? WHERE id = ?"));
        q.addBindValue(relative(path));
        q.addBindValue(relative(thumb));
        q.addBindValue(id);
        run(q);
    }
    db.commit();
}

QSqlDatabase Database::conn()
{
    auto it = t_conn.names.constFind(m_file);
    if (it != t_conn.names.constEnd())
        return QSqlDatabase::database(*it, false);

    const QString name = QStringLiteral("mvplayer-%1").arg(g_connCounter.fetchAndAddRelaxed(1));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(m_file);
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=15000"));
    if (!db.open()) {
        qWarning().noquote() << "[db] cannot open" << m_file << db.lastError().text();
    } else {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    }
    t_conn.names.insert(m_file, name);
    return db;
}

bool Database::init(QString *error)
{
    QSqlDatabase db = conn();
    if (!db.isOpen()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    const QStringList ddl = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS videos ("
            " id INTEGER PRIMARY KEY,"
            " yt_id TEXT UNIQUE NOT NULL,"
            " path TEXT NOT NULL,"
            " thumb TEXT,"
            " title TEXT, artist TEXT, album_artist TEXT, album TEXT, genre TEXT,"
            " year INTEGER, track_no INTEGER,"
            " duration REAL, width INTEGER, height INTEGER, fps REAL, vcodec TEXT,"
            " audio_source TEXT, audio_detail TEXT,"
            " yt_title TEXT, yt_channel TEXT,"
            " tags_json TEXT,"
            " added_at INTEGER)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS tracks ("
            " id INTEGER PRIMARY KEY,"
            " path TEXT UNIQUE NOT NULL,"
            " mtime INTEGER, size INTEGER,"
            " title TEXT, artist TEXT, album_artist TEXT, album TEXT, genre TEXT,"
            " year INTEGER, track_no INTEGER, disc_no INTEGER,"
            " duration REAL, codec TEXT, bitrate INTEGER, sample_rate INTEGER, bits INTEGER,"
            " channels INTEGER, lossless INTEGER,"
            " tags_json TEXT,"
            " state TEXT NOT NULL DEFAULT 'pending',"
            " video_id INTEGER REFERENCES videos(id) ON DELETE SET NULL,"
            " attempts INTEGER NOT NULL DEFAULT 0,"
            " last_attempt INTEGER NOT NULL DEFAULT 0,"
            " message TEXT)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS tracks_state ON tracks(state)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS tracks_video ON tracks(video_id)"),
    };
    for (const QString &sql : ddl) {
        QSqlQuery q(db);
        if (!q.exec(sql)) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }

    // Up to 0.1.1 a track whose candidates could not be downloaded was filed
    // as having no video. Give those another go, once.
    QSqlQuery version(db);
    int schema = 0;
    if (version.exec(QStringLiteral("PRAGMA user_version")) && version.next())
        schema = version.value(0).toInt();
    if (schema < 2) {
        QSqlQuery q(db);
        q.exec(QStringLiteral("UPDATE tracks SET state = 'pending' WHERE state = 'not_found' AND message LIKE '%ERROR:%'"));
        q.exec(QStringLiteral("PRAGMA user_version = 2"));
    }
    if (schema < 3) {
        makeVideoPathsRelative(db);
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA user_version = 3"));
    }

    // Added after 0.1.0: existing rows keep 0 and are audited once.
    QSqlQuery info(db);
    bool hasFlag = false;
    if (info.exec(QStringLiteral("PRAGMA table_info(videos)"))) {
        while (info.next())
            hasFlag |= info.value(1).toString() == QLatin1String("still_checked");
    }
    if (!hasFlag) {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("ALTER TABLE videos ADD COLUMN still_checked INTEGER NOT NULL DEFAULT 0"))) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }

    // Added after 0.1.2: which recording a file holds, and the fingerprint
    // that tells. Existing rows are identified by the next scan.
    bool hasRecording = false;
    if (info.exec(QStringLiteral("PRAGMA table_info(tracks)"))) {
        while (info.next())
            hasRecording |= info.value(1).toString() == QLatin1String("recording");
    }
    QStringList more;
    if (!hasRecording) {
        more << QStringLiteral("ALTER TABLE tracks ADD COLUMN recording INTEGER")
             << QStringLiteral("ALTER TABLE tracks ADD COLUMN fingerprint BLOB")
             << QStringLiteral("ALTER TABLE tracks ADD COLUMN fp_size INTEGER")
             // The title a file was last searched under. Until now every
             // file ran its own lookup, under its own title.
             << QStringLiteral("ALTER TABLE tracks ADD COLUMN searched_title TEXT")
             << QStringLiteral("UPDATE tracks SET searched_title = title WHERE state IN ('done', 'not_found')");
    }
    more << QStringLiteral("CREATE INDEX IF NOT EXISTS tracks_recording ON tracks(recording)");
    for (const QString &sql : std::as_const(more)) {
        QSqlQuery q(db);
        if (!q.exec(sql)) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    return true;
}

QHash<QString, TrackInfo> Database::tracksByPath()
{
    QHash<QString, TrackInfo> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM tracks").arg(QLatin1String(kTrackCols)));
    if (run(q)) {
        while (q.next()) {
            TrackInfo t = readTrack(q);
            out.insert(t.path, t);
        }
    }
    return out;
}

QVector<TrackInfo> Database::tracksInState(const QStringList &states)
{
    QVector<TrackInfo> out;
    QStringList marks;
    for (int i = 0; i < states.size(); ++i)
        marks << QStringLiteral("?");
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM tracks WHERE state IN (%2) ORDER BY path")
                  .arg(QLatin1String(kTrackCols), marks.join(QLatin1Char(','))));
    for (const QString &s : states)
        q.addBindValue(s);
    if (run(q)) {
        while (q.next())
            out << readTrack(q);
    }
    return out;
}

std::optional<TrackInfo> Database::track(qint64 id)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM tracks WHERE id = ?").arg(QLatin1String(kTrackCols)));
    q.addBindValue(id);
    if (run(q) && q.next())
        return readTrack(q);
    return std::nullopt;
}

bool Database::upsertTrack(TrackInfo &t)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral(
        "INSERT INTO tracks (path, mtime, size, title, artist, album_artist, album, genre, year,"
        " track_no, disc_no, duration, codec, bitrate, sample_rate, bits, channels, lossless,"
        " tags_json, state, video_id, attempts, last_attempt, message, recording)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(path) DO UPDATE SET"
        " mtime=excluded.mtime, size=excluded.size, title=excluded.title, artist=excluded.artist,"
        " album_artist=excluded.album_artist, album=excluded.album, genre=excluded.genre,"
        " year=excluded.year, track_no=excluded.track_no, disc_no=excluded.disc_no,"
        " duration=excluded.duration, codec=excluded.codec, bitrate=excluded.bitrate,"
        " sample_rate=excluded.sample_rate, bits=excluded.bits, channels=excluded.channels,"
        " lossless=excluded.lossless, tags_json=excluded.tags_json, state=excluded.state,"
        " video_id=excluded.video_id, attempts=excluded.attempts,"
        " last_attempt=excluded.last_attempt, message=excluded.message, recording=excluded.recording"));
    q.addBindValue(t.path);
    q.addBindValue(t.mtime);
    q.addBindValue(t.size);
    q.addBindValue(t.title);
    q.addBindValue(t.artist);
    q.addBindValue(t.albumArtist);
    q.addBindValue(t.album);
    q.addBindValue(t.genre);
    q.addBindValue(t.year);
    q.addBindValue(t.trackNo);
    q.addBindValue(t.discNo);
    q.addBindValue(t.duration);
    q.addBindValue(t.codec);
    q.addBindValue(t.bitrate);
    q.addBindValue(t.sampleRate);
    q.addBindValue(t.bitsPerSample);
    q.addBindValue(t.channels);
    q.addBindValue(t.lossless);
    q.addBindValue(jsonText(t.tags));
    q.addBindValue(t.state);
    q.addBindValue(t.videoId > 0 ? QVariant(t.videoId) : QVariant());
    q.addBindValue(t.attempts);
    q.addBindValue(t.lastAttempt);
    q.addBindValue(t.message);
    q.addBindValue(t.recording > 0 ? QVariant(t.recording) : QVariant());
    if (!run(q))
        return false;

    QSqlQuery idq(conn());
    idq.prepare(QStringLiteral("SELECT id FROM tracks WHERE path = ?"));
    idq.addBindValue(t.path);
    if (run(idq) && idq.next())
        t.id = idq.value(0).toLongLong();
    return true;
}

bool Database::moveTrack(qint64 id, const QString &path)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET path = ? WHERE id = ?"));
    q.addBindValue(path);
    q.addBindValue(id);
    return run(q);
}

void Database::removeTrack(qint64 id)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE id = ?"));
    q.addBindValue(id);
    run(q);
}

void Database::setTrackResult(qint64 id, const QString &state, qint64 videoId, const QString &message)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral(
        "UPDATE tracks SET state = ?, video_id = ?, message = ?, attempts = attempts + 1,"
        " last_attempt = ? WHERE id = ? OR recording = (SELECT recording FROM tracks WHERE id = ?)"));
    q.addBindValue(state);
    q.addBindValue(videoId > 0 ? QVariant(videoId) : QVariant());
    q.addBindValue(message);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.addBindValue(id);
    q.addBindValue(id);
    run(q);

    // The search ran under this file's title.
    if (state == QLatin1String("done") || state == QLatin1String("not_found")) {
        QSqlQuery t(conn());
        t.prepare(QStringLiteral("UPDATE tracks SET searched_title = title WHERE id = ?"));
        t.addBindValue(id);
        run(t);
    }
}

int Database::requeueUntriedTitles()
{
    struct Group {
        QSet<QString> tried;
        QVector<TrackInfo> files;
    };
    QHash<qint64, Group> groups;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1, searched_title FROM tracks WHERE state = 'not_found' AND recording IS NOT NULL")
                  .arg(QLatin1String(kTrackCols)));
    if (!run(q))
        return 0;
    while (q.next()) {
        const TrackInfo t = readTrack(q);
        Group &g = groups[t.recording];
        g.files << t;
        const QString tried = q.value(q.record().count() - 1).toString();
        if (!tried.isEmpty())
            g.tried.insert(foldText(tried));
    }

    int queued = 0;
    for (const Group &g : std::as_const(groups)) {
        const TrackInfo *next = nullptr;
        for (const TrackInfo &t : g.files) {
            if (!g.tried.contains(foldText(t.title)) && (!next || betterSource(t, *next)))
                next = &t;
        }
        // Nothing on record as tried: a library from before this was kept.
        if (!next || g.tried.isEmpty())
            continue;
        QSqlQuery p(conn());
        p.prepare(QStringLiteral("UPDATE tracks SET state = 'pending', message = '' WHERE id = ?"));
        p.addBindValue(next->id);
        if (run(p))
            ++queued;
    }
    return queued;
}

void Database::setRecording(qint64 trackId, qint64 recording)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET recording = ? WHERE id = ?"));
    q.addBindValue(recording);
    q.addBindValue(trackId);
    run(q);
}

void Database::joinRecording(qint64 trackId, qint64 memberId, bool trackLeads)
{
    // Values are read from the rows as they are now: a lookup may have
    // finished since the caller last looked.
    const auto copyResult = [this](qint64 from, const QString &where, const QVariantList &args) {
        QSqlQuery q(conn());
        q.prepare(QStringLiteral(
            "UPDATE tracks SET (state, video_id, message, attempts, last_attempt) ="
            " (SELECT state, video_id, message, attempts, last_attempt FROM tracks WHERE id = ?) WHERE ") + where);
        q.addBindValue(from);
        for (const QVariant &a : args)
            q.addBindValue(a);
        run(q);
    };
    const std::optional<TrackInfo> member = track(memberId);
    if (!member || member->recording <= 0)
        return;
    setRecording(trackId, member->recording);
    if (trackLeads)
        copyResult(trackId, QStringLiteral("recording = ? AND id != ?"), {member->recording, trackId});
    else
        copyResult(memberId, QStringLiteral("id = ?"), {trackId});
}

QVector<TrackInfo> Database::pendingRecordings()
{
    QVector<TrackInfo> out;
    QHash<qint64, int> byRecording; // recording -> index in out
    for (const TrackInfo &t : tracksInState({QStringLiteral("pending")})) {
        const auto it = t.recording > 0 ? byRecording.constFind(t.recording) : byRecording.constEnd();
        if (it == byRecording.constEnd()) {
            if (t.recording > 0)
                byRecording.insert(t.recording, out.size());
            out << t;
        } else if (betterSource(t, out[*it])) {
            out[*it] = t;
        }
    }
    return out;
}

QHash<qint64, Database::StoredPrint> Database::fingerprints()
{
    QHash<qint64, StoredPrint> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT id, fingerprint, fp_size = size FROM tracks WHERE fingerprint IS NOT NULL"));
    if (run(q)) {
        while (q.next())
            out.insert(q.value(0).toLongLong(), {q.value(1).toByteArray(), q.value(2).toBool()});
    }
    return out;
}

void Database::setFingerprint(qint64 trackId, const QByteArray &packed, qint64 fileSize)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET fingerprint = ?, fp_size = ? WHERE id = ?"));
    q.addBindValue(packed);
    q.addBindValue(fileSize);
    q.addBindValue(trackId);
    run(q);
}

qint64 Database::newRecording()
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT COALESCE(MAX(recording), 0) + 1 FROM tracks"));
    return run(q) && q.next() ? q.value(0).toLongLong() : 1;
}

void Database::resetTracks(const QStringList &fromStates)
{
    for (const QString &s : fromStates) {
        QSqlQuery q(conn());
        q.prepare(QStringLiteral("UPDATE tracks SET state = 'pending', message = '' WHERE state = ?"));
        q.addBindValue(s);
        run(q);
    }
}

void Database::requeueStale(qint64 failedAfterSecs, qint64 notFoundAfterSecs)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    {
        // 1st failure: failedAfterSecs, then x2 per attempt, at most the not-found interval.
        QSqlQuery q(conn());
        q.prepare(QStringLiteral(
            "UPDATE tracks SET state = 'pending' WHERE state = 'failed'"
            " AND last_attempt < ? - min(? * (1 << min(max(attempts, 1) - 1, 12)), ?)"));
        q.addBindValue(now);
        q.addBindValue(failedAfterSecs);
        q.addBindValue(notFoundAfterSecs);
        run(q);
    }
    {
        QSqlQuery q(conn());
        q.prepare(QStringLiteral("UPDATE tracks SET state = 'pending' WHERE state = 'not_found' AND last_attempt < ?"));
        q.addBindValue(now - notFoundAfterSecs);
        run(q);
    }
}

QVector<TrackInfo> Database::tracksForVideo(qint64 videoId)
{
    QVector<TrackInfo> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM tracks WHERE video_id = ? ORDER BY id").arg(QLatin1String(kTrackCols)));
    q.addBindValue(videoId);
    if (run(q)) {
        while (q.next())
            out << readTrack(q);
    }
    return out;
}

QHash<QString, int> Database::trackStateCounts()
{
    QHash<QString, int> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT state, COUNT(*) FROM tracks GROUP BY state"));
    if (run(q)) {
        while (q.next())
            out.insert(q.value(0).toString(), q.value(1).toInt());
    }
    return out;
}

QVector<VideoInfo> Database::allVideos()
{
    QVector<VideoInfo> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM videos ORDER BY id").arg(QLatin1String(kVideoCols)));
    if (run(q)) {
        while (q.next())
            out << resolved(readVideo(q));
    }
    return out;
}

std::optional<VideoInfo> Database::video(qint64 id)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM videos WHERE id = ?").arg(QLatin1String(kVideoCols)));
    q.addBindValue(id);
    if (run(q) && q.next())
        return resolved(readVideo(q));
    return std::nullopt;
}

std::optional<VideoInfo> Database::videoByYtId(const QString &ytId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM videos WHERE yt_id = ?").arg(QLatin1String(kVideoCols)));
    q.addBindValue(ytId);
    if (run(q) && q.next())
        return resolved(readVideo(q));
    return std::nullopt;
}

qint64 Database::insertVideo(const VideoInfo &v)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral(
        "INSERT INTO videos (yt_id, path, thumb, title, artist, album_artist, album, genre, year,"
        " track_no, duration, width, height, fps, vcodec, audio_source, audio_detail, yt_title,"
        " yt_channel, tags_json, added_at, still_checked)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1)"));
    q.addBindValue(v.ytId);
    q.addBindValue(storedPath(v.path));
    q.addBindValue(storedPath(v.thumb));
    q.addBindValue(v.title);
    q.addBindValue(v.artist);
    q.addBindValue(v.albumArtist);
    q.addBindValue(v.album);
    q.addBindValue(v.genre);
    q.addBindValue(v.year);
    q.addBindValue(v.trackNo);
    q.addBindValue(v.duration);
    q.addBindValue(v.width);
    q.addBindValue(v.height);
    q.addBindValue(v.fps);
    q.addBindValue(v.vcodec);
    q.addBindValue(v.audioSource);
    q.addBindValue(v.audioDetail);
    q.addBindValue(v.ytTitle);
    q.addBindValue(v.ytChannel);
    q.addBindValue(jsonText(v.tags));
    q.addBindValue(v.addedAt);
    if (!run(q))
        return 0;
    return q.lastInsertId().toLongLong();
}

void Database::updateVideoTags(const VideoInfo &v)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral(
        "UPDATE videos SET title = ?, artist = ?, album_artist = ?, album = ?, genre = ?, year = ?,"
        " track_no = ?, tags_json = ? WHERE id = ?"));
    q.addBindValue(v.title);
    q.addBindValue(v.artist);
    q.addBindValue(v.albumArtist);
    q.addBindValue(v.album);
    q.addBindValue(v.genre);
    q.addBindValue(v.year);
    q.addBindValue(v.trackNo);
    q.addBindValue(jsonText(v.tags));
    q.addBindValue(v.id);
    run(q);
}

QVector<VideoInfo> Database::videosNotStillChecked()
{
    QVector<VideoInfo> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM videos WHERE still_checked = 0 ORDER BY id").arg(QLatin1String(kVideoCols)));
    if (run(q)) {
        while (q.next())
            out << resolved(readVideo(q));
    }
    return out;
}

void Database::markStillChecked(qint64 videoId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE videos SET still_checked = 1 WHERE id = ?"));
    q.addBindValue(videoId);
    run(q);
}

void Database::requeueTracksOfVideo(qint64 videoId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET state = 'pending', video_id = NULL, message = '' WHERE video_id = ?"));
    q.addBindValue(videoId);
    run(q);
}

void Database::removeVideo(qint64 id)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("DELETE FROM videos WHERE id = ?"));
    q.addBindValue(id);
    run(q);
}
