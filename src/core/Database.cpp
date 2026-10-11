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

#include <algorithm>
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
    "attempts, last_attempt, message, recording, absent, reimport";

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
    t.absent = q.value(i++).toBool();
    t.reimport = q.value(i++).toBool();
    return t;
}

const char *kVideoCols =
    "id, yt_id, path, thumb, title, artist, album_artist, album, genre, year, track_no, duration, "
    "width, height, fps, vcodec, audio_source, audio_detail, yt_title, yt_channel, tags_json, added_at, yt_abr, "
    "review, review_start, review_end, yt_stream, yt_channel_id, yt_channel_subs, yt_channel_verified, yt_channel_handle, vouched, "
    "yt_date";

// Text that was kept, though it may be empty: never the null string, which
// stands for "not kept".
QString keptText(const QString &s)
{
    return s.isNull() ? QStringLiteral("") : s;
}

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
    v.ytAbr = q.value(i++).toDouble();
    v.review = q.value(i++).toBool();
    v.reviewStart = q.value(i++).toDouble();
    v.reviewEnd = q.value(i++).toDouble();
    v.ytStream = q.value(i++).toInt();
    v.ytChannelId = q.value(i++).toString();
    v.ytChannelSubs = q.value(i).isNull() ? -2 : q.value(i).toLongLong();
    ++i;
    v.ytChannelVerified = q.value(i++).toBool();
    if (!q.value(i).isNull())
        v.ytChannelHandle = keptText(q.value(i).toString());
    ++i;
    v.vouched = q.value(i++).toBool();
    if (!q.value(i).isNull())
        v.ytDate = q.value(i).toString();
    ++i;
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

// The layout below is the one this version reads and writes. The one before
// it is brought up to it (upgradeLayout); any other is refused.
constexpr int kSchema = 6;

namespace {
// A track is a recording: its number, or minus the file's id where it has none.
const char *kTrackKeySql = "CASE WHEN %1recording > 0 THEN %1recording ELSE -%1id END";

QString trackKeySql(const char *alias = "")
{
    return QString::fromLatin1(kTrackKeySql).arg(QLatin1String(alias));
}
} // namespace

// Layout 4 had tracks share videos, and had review "groups": videos that
// waited as options for the same tracks, each track pointing at one of them.
// In layout 5 a video under review has candidates, and a track's video_id is
// its video in the library and nothing else.
namespace {
const char *kChannelListsSql =
    "CREATE TABLE IF NOT EXISTS channel_lists ("
    " channel_id TEXT PRIMARY KEY, list TEXT NOT NULL, name TEXT, handle TEXT, added_at INTEGER NOT NULL DEFAULT 0)";
}

bool Database::upgradeLayout(int from, QString *error)
{
    if (from != 4 && from != 5)
        return false;
    QSqlDatabase db = conn();
    if (from == 5) {
        // (A column the step below reads, from before channel ids were kept.)
        QSqlQuery q(db);
        bool has = false;
        if (q.exec(QStringLiteral("PRAGMA table_info(videos)"))) {
            while (q.next())
                has |= q.value(1).toString() == QLatin1String("yt_channel_id");
        }
        if (!has)
            q.exec(QStringLiteral("ALTER TABLE videos ADD COLUMN yt_channel_id TEXT"));
    }
    // Layout 5 to 6: channels are allowed or denied by the user, and
    // accepting a video vouches for that upload, no longer for its channel.
    // The videos that are in the library on the strength of a channel
    // vouched for that way keep it, each as an upload the user accepted.
    const QStringList toSix = {
        QStringLiteral("ALTER TABLE videos ADD COLUMN vouched INTEGER NOT NULL DEFAULT 0"),
        QString::fromLatin1(kChannelListsSql),
        QStringLiteral("CREATE TABLE IF NOT EXISTS artist_channels ("
                       " artist TEXT NOT NULL, channel_id TEXT NOT NULL DEFAULT '', channel TEXT NOT NULL DEFAULT '',"
                       " source TEXT, checked_until INTEGER NOT NULL DEFAULT 0,"
                       " PRIMARY KEY (artist, channel_id, channel))"),
        QStringLiteral(
            "UPDATE videos SET vouched = 1 WHERE review = 0 AND EXISTS ("
            " SELECT 1 FROM tracks t JOIN artist_channels ac ON ac.source = 'approved'"
            "  AND ((ac.channel_id != '' AND ac.channel_id = videos.yt_channel_id) OR (ac.channel != '' AND ac.channel = videos.yt_channel))"
            "  AND (instr(COALESCE(t.artist, ''), ac.artist) > 0 OR instr(COALESCE(t.album_artist, ''), ac.artist) > 0)"
            " WHERE t.video_id = videos.id)"),
        QStringLiteral("DELETE FROM artist_channels WHERE source = 'approved'"),
        QStringLiteral("PRAGMA user_version = 6"),
    };
    const QStringList steps = from == 5 ? toSix : QStringList{
        QStringLiteral("CREATE TABLE candidates ("
                       " video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,"
                       " track_key INTEGER NOT NULL,"
                       " stream INTEGER NOT NULL DEFAULT 0, fit REAL NOT NULL DEFAULT 0, audio_detail TEXT,"
                       " PRIMARY KEY (video_id, track_key))"),
        QStringLiteral("ALTER TABLE videos ADD COLUMN yt_stream INTEGER NOT NULL DEFAULT 0"),
        // Where the library's audio was put in, YouTube's is the stream after it.
        QStringLiteral("UPDATE videos SET yt_stream = CASE WHEN audio_source = 'library' THEN 1 ELSE 0 END"),
        // Every option of a group becomes a video with the group's tracks as
        // its candidates. Its file was made with the audio of the track it
        // is listed under: that candidate has the stream.
        QStringLiteral(
            "INSERT INTO candidates (video_id, track_key, stream, fit, audio_detail)"
            " SELECT v.id, %1,"
            "  MAX(CASE WHEN v.audio_source = 'library' AND t.title IS v.title AND t.album IS v.album THEN 1 ELSE 0 END), 0, ''"
            " FROM videos v"
            " JOIN videos o ON o.review = 1 AND COALESCE(NULLIF(o.review_group, 0), o.id) = COALESCE(NULLIF(v.review_group, 0), v.id)"
            " JOIN tracks t ON t.video_id = o.id"
            " WHERE v.review = 1 GROUP BY v.id, %1").arg(trackKeySql("t.")),
        // (Tags may have changed since: a video's only candidate is the one.)
        QStringLiteral(
            "UPDATE candidates SET stream = 1 WHERE stream = 0"
            " AND video_id IN (SELECT id FROM videos WHERE review = 1 AND audio_source = 'library')"
            " AND (SELECT COUNT(*) FROM candidates c WHERE c.video_id = candidates.video_id) = 1"),
        QStringLiteral(
            "UPDATE candidates SET audio_detail = (SELECT audio_detail FROM videos WHERE id = candidates.video_id) WHERE stream = 1"),
        QStringLiteral(
            "UPDATE tracks SET state = CASE WHEN state = 'done' THEN 'review' ELSE state END, video_id = NULL"
            " WHERE video_id IN (SELECT id FROM videos WHERE review = 1)"),
        QStringLiteral("CREATE INDEX candidates_track ON candidates(track_key)"),
        QStringLiteral("PRAGMA user_version = 5"),
    };
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    for (const QString &sql : steps) {
        QSqlQuery q(db);
        if (!q.exec(sql)) {
            if (error)
                *error = QStringLiteral("could not bring %1 up to this version: %2").arg(m_file, q.lastError().text());
            db.rollback();
            return false;
        }
    }
    if (!db.commit()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    if (from == 5)
        qInfo("[db] library brought up from layout 5: channels are allowed or denied by you; an accepted video vouches for itself");
    else
        qInfo("[db] library brought up from layout 4: one track per video, review by video");
    return true;
}

bool Database::init(QString *error)
{
    QSqlDatabase db = conn();
    if (!db.isOpen()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    {
        QSqlQuery q(db);
        const bool exists = q.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'videos'"))
            && q.next();
        int schema = 0;
        if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next())
            schema = q.value(0).toInt();
        // One layout at a time, from the oldest this version reads.
        while (exists && schema >= 4 && schema < kSchema) {
            if (!upgradeLayout(schema, error))
                return false;
            ++schema;
        }
        if (exists && schema != kSchema) {
            if (error) {
                *error = QStringLiteral("%1 was written by another version of mvplayer (layout %2, this one uses %3);"
                                        " move it away or delete it to start a new library")
                             .arg(m_file).arg(schema).arg(kSchema);
            }
            return false;
        }
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
            " added_at INTEGER,"
            " yt_abr REAL,"
            " review INTEGER NOT NULL DEFAULT 0, review_start REAL, review_end REAL,"
            " yt_stream INTEGER NOT NULL DEFAULT 0,"
            " yt_channel_id TEXT,"
            " yt_channel_subs INTEGER, yt_channel_verified INTEGER NOT NULL DEFAULT 0, yt_channel_handle TEXT,"
            " vouched INTEGER NOT NULL DEFAULT 0,"
            " kept INTEGER NOT NULL DEFAULT 0)"),
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
            " message TEXT,"
            " recording INTEGER, fingerprint BLOB, fp_size INTEGER,"
            " searched_title TEXT,"
            " absent INTEGER NOT NULL DEFAULT 0,"
            " reimport INTEGER NOT NULL DEFAULT 0)"),
        // Videos turned down for a track: by recording, or by track where
        // the recording is not known.
        QStringLiteral("CREATE TABLE IF NOT EXISTS rejected_videos ("
                       " key TEXT NOT NULL, yt_id TEXT NOT NULL, PRIMARY KEY (key, yt_id))"),
        // An artist's own channels: id (empty for a name alone), name, where
        // known from. A row with an empty id and name holds when the artist
        // was last asked about.
        QStringLiteral("CREATE TABLE IF NOT EXISTS artist_channels ("
                       " artist TEXT NOT NULL, channel_id TEXT NOT NULL DEFAULT '', channel TEXT NOT NULL DEFAULT '',"
                       " source TEXT, checked_until INTEGER NOT NULL DEFAULT 0,"
                       " PRIMARY KEY (artist, channel_id, channel))"),
        // The tracks a video under review could belong to: by recording, or
        // by minus the track's id where the recording is not known.
        QStringLiteral("CREATE TABLE IF NOT EXISTS candidates ("
                       " video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,"
                       " track_key INTEGER NOT NULL,"
                       " stream INTEGER NOT NULL DEFAULT 0, fit REAL NOT NULL DEFAULT 0, audio_detail TEXT,"
                       " PRIMARY KEY (video_id, track_key))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS candidates_track ON candidates(track_key)"),
        QString::fromLatin1(kChannelListsSql),
        QStringLiteral("CREATE INDEX IF NOT EXISTS tracks_state ON tracks(state)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS tracks_video ON tracks(video_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS tracks_recording ON tracks(recording)"),
        QStringLiteral("PRAGMA user_version = %1").arg(kSchema),
    };
    for (const QString &sql : ddl) {
        QSqlQuery q(db);
        if (!q.exec(sql)) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    // Libraries from before tracks could be absent gain the column. It has a
    // default, so builds that do not know it read and write the table as before.
    {
        QSqlQuery q(db);
        bool has = false;
        if (q.exec(QStringLiteral("PRAGMA table_info(tracks)"))) {
            while (q.next())
                has |= q.value(1).toString() == QLatin1String("absent");
        }
        if (!has && !q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN absent INTEGER NOT NULL DEFAULT 0"))) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    {
        QSqlQuery q(db);
        bool has = false;
        if (q.exec(QStringLiteral("PRAGMA table_info(videos)"))) {
            while (q.next())
                has |= q.value(1).toString() == QLatin1String("yt_channel_id");
        }
        if (!has && !q.exec(QStringLiteral("ALTER TABLE videos ADD COLUMN yt_channel_id TEXT"))) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    // Likewise the mark on untracked videos the user keeps.
    {
        QSqlQuery q(db);
        bool has = false;
        if (q.exec(QStringLiteral("PRAGMA table_info(videos)"))) {
            while (q.next())
                has |= q.value(1).toString() == QLatin1String("kept");
        }
        if (!has && !q.exec(QStringLiteral("ALTER TABLE videos ADD COLUMN kept INTEGER NOT NULL DEFAULT 0"))) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    // What an upload's page says of its channel, kept since small channels
    // can be ignored: its subscribers and its verified mark, and its handle,
    // by which an exception names it. And when the upload was published.
    for (const char *column : {"yt_channel_subs INTEGER", "yt_channel_verified INTEGER NOT NULL DEFAULT 0", "yt_channel_handle TEXT",
                               "yt_date TEXT"}) {
        QSqlQuery q(db);
        const QString name = QString::fromLatin1(column).section(QLatin1Char(' '), 0, 0);
        bool has = false;
        if (q.exec(QStringLiteral("PRAGMA table_info(videos)"))) {
            while (q.next())
                has |= q.value(1).toString() == name;
        }
        if (!has && !q.exec(QStringLiteral("ALTER TABLE videos ADD COLUMN ") + QLatin1String(column))) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    // And the mark on tracks whose video is to be judged again.
    {
        QSqlQuery q(db);
        bool has = false;
        if (q.exec(QStringLiteral("PRAGMA table_info(tracks)"))) {
            while (q.next())
                has |= q.value(1).toString() == QLatin1String("reimport");
        }
        if (!has && !q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN reimport INTEGER NOT NULL DEFAULT 0"))) {
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
    q.prepare(QStringLiteral("SELECT %1 FROM tracks WHERE state IN (%2) AND absent = 0 ORDER BY path")
                  .arg(QLatin1String(kTrackCols), marks.join(QLatin1Char(','))));
    for (const QString &s : states)
        q.addBindValue(s);
    if (run(q)) {
        while (q.next())
            out << readTrack(q);
    }
    return out;
}

QVector<TrackInfo> Database::trackSummariesInState(const QStringList &states)
{
    QVector<TrackInfo> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT title, artist, album_artist, state, message FROM tracks "
                             "WHERE state IN (%1) AND absent = 0 ORDER BY path")
                  .arg(QStringList(states.size(), QStringLiteral("?")).join(QLatin1Char(','))));
    for (const QString &s : states)
        q.addBindValue(s);
    if (run(q)) {
        while (q.next()) {
            TrackInfo t;
            t.title = q.value(0).toString();
            t.artist = q.value(1).toString();
            t.albumArtist = q.value(2).toString();
            t.state = q.value(3).toString();
            t.message = q.value(4).toString();
            out << t;
        }
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
        " last_attempt=excluded.last_attempt, message=excluded.message, recording=excluded.recording,"
        " absent=0"));
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

void Database::setTrackAbsent(qint64 id, bool absent)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET absent = ? WHERE id = ?"));
    q.addBindValue(absent ? 1 : 0);
    q.addBindValue(id);
    run(q);
}

// A video in the library is tracked by the track that has it, one under
// review by its candidates. Written so that the indexes serve: the videos
// that are had and those with a candidate are gathered once, not searched
// for again per video (that took seconds on a large library, on the main
// thread, every time the importer's activity changed).
const char *kUntrackedSql =
    "WITH linked AS (SELECT DISTINCT video_id AS id FROM tracks WHERE absent = 0 AND video_id IS NOT NULL),"
    " wanted AS (SELECT DISTINCT c.video_id AS id FROM candidates c WHERE"
    "  (c.track_key > 0 AND EXISTS (SELECT 1 FROM tracks t WHERE t.recording = c.track_key AND t.absent = 0))"
    "  OR (c.track_key < 0 AND EXISTS (SELECT 1 FROM tracks t WHERE t.id = -c.track_key AND t.absent = 0)))"
    " %1 FROM videos WHERE kept = 0 AND id NOT IN (SELECT id FROM linked) AND id NOT IN (SELECT id FROM wanted)";

QVector<VideoInfo> Database::untrackedVideos()
{
    QVector<VideoInfo> out;
    QSqlQuery q(conn());
    q.prepare(QString::fromLatin1(kUntrackedSql).arg(QStringLiteral("SELECT ") + QLatin1String(kVideoCols)) + QStringLiteral(" ORDER BY id"));
    if (run(q)) {
        while (q.next())
            out << resolved(readVideo(q));
    }
    return out;
}

QSet<qint64> Database::untrackedVideoIds()
{
    QSet<qint64> out;
    QSqlQuery q(conn());
    q.prepare(QString::fromLatin1(kUntrackedSql).arg(QStringLiteral("SELECT id")));
    if (run(q)) {
        while (q.next())
            out.insert(q.value(0).toLongLong());
    }
    return out;
}

void Database::keepVideo(qint64 videoId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE videos SET kept = 1 WHERE id = ?"));
    q.addBindValue(videoId);
    run(q);
}

void Database::removeAbsentTracksOf(qint64 videoId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE video_id = ? AND absent = 1"));
    q.addBindValue(videoId);
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
    if (state == QLatin1String("done") || state == QLatin1String("review") || state == QLatin1String("not_found")) {
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
    const std::optional<TrackInfo> before = track(trackId);
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET recording = ? WHERE id = ?"));
    q.addBindValue(recording);
    q.addBindValue(trackId);
    run(q);
    // What the track is a candidate of goes with it, unless other files of
    // the recording it leaves are still there to be that candidate.
    if (!before)
        return;
    const qint64 from = trackKey(*before), to = recording > 0 ? recording : -trackId;
    if (from == to)
        return;
    if (from > 0) {
        QSqlQuery left(conn());
        left.prepare(QStringLiteral("SELECT 1 FROM tracks WHERE recording = ? LIMIT 1"));
        left.addBindValue(from);
        if (run(left) && left.next())
            return;
    }
    rekeyCandidates(from, to);
}

void Database::rekeyCandidates(qint64 fromKey, qint64 toKey)
{
    QSqlQuery move(conn());
    move.prepare(QStringLiteral("UPDATE OR IGNORE candidates SET track_key = ? WHERE track_key = ?"));
    move.addBindValue(toKey);
    move.addBindValue(fromKey);
    run(move);
    QSqlQuery rest(conn());
    rest.prepare(QStringLiteral("DELETE FROM candidates WHERE track_key = ?"));
    rest.addBindValue(fromKey);
    run(rest);
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

QVector<TrackInfo> Database::tracksForVideo(qint64 videoId, bool includeAbsent)
{
    QVector<TrackInfo> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM tracks WHERE video_id = ?%2 ORDER BY id")
                  .arg(QLatin1String(kTrackCols), includeAbsent ? QString() : QStringLiteral(" AND absent = 0")));
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
    q.prepare(QStringLiteral("SELECT state, COUNT(*) FROM tracks WHERE absent = 0 GROUP BY state"));
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
        " yt_channel, tags_json, added_at, yt_abr, review, review_start, review_end, yt_stream, yt_channel_id,"
        " yt_channel_subs, yt_channel_verified, yt_channel_handle, vouched, yt_date)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
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
    q.addBindValue(v.ytAbr > 0 ? QVariant(v.ytAbr) : QVariant());
    q.addBindValue(v.review);
    q.addBindValue(v.reviewStart);
    q.addBindValue(v.reviewEnd);
    q.addBindValue(v.ytStream);
    q.addBindValue(v.ytChannelId);
    q.addBindValue(v.ytChannelSubs > -2 ? QVariant(v.ytChannelSubs) : QVariant());
    q.addBindValue(v.ytChannelVerified);
    q.addBindValue(v.ytChannelHandle.isNull() ? QVariant() : QVariant(v.ytChannelHandle));
    q.addBindValue(v.vouched && !v.review);
    q.addBindValue(v.ytDate.isNull() ? QVariant() : QVariant(v.ytDate));
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

void Database::updateVideoMedia(const VideoInfo &v)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral(
        "UPDATE videos SET duration = ?, width = ?, height = ?, fps = ?, vcodec = ?, audio_source = ?,"
        " audio_detail = ?, yt_abr = ?, review = ?, review_start = ?, review_end = ?, yt_stream = ?, vouched = ? WHERE id = ?"));
    q.addBindValue(v.duration);
    q.addBindValue(v.width);
    q.addBindValue(v.height);
    q.addBindValue(v.fps);
    q.addBindValue(v.vcodec);
    q.addBindValue(v.audioSource);
    q.addBindValue(v.audioDetail);
    q.addBindValue(v.ytAbr > 0 ? QVariant(v.ytAbr) : QVariant());
    q.addBindValue(v.review);
    q.addBindValue(v.reviewStart);
    q.addBindValue(v.reviewEnd);
    q.addBindValue(v.ytStream);
    // (A video that waits for a verdict has none yet.)
    q.addBindValue(v.vouched && !v.review);
    q.addBindValue(v.id);
    run(q);
}

void Database::updateVideoChannel(const VideoInfo &v)
{
    QSqlQuery q(conn());
    // (The page read for the channel says when the upload was published too.)
    q.prepare(QStringLiteral("UPDATE videos SET yt_channel_id = ?, yt_channel_subs = ?, yt_channel_verified = ?,"
                             " yt_channel_handle = ?, yt_date = COALESCE(?, yt_date) WHERE id = ?"));
    q.addBindValue(keptText(v.ytChannelId));
    q.addBindValue(v.ytChannelSubs > -2 ? QVariant(v.ytChannelSubs) : QVariant());
    q.addBindValue(v.ytChannelVerified);
    q.addBindValue(v.ytChannelHandle.isNull() ? QVariant() : QVariant(v.ytChannelHandle));
    q.addBindValue(v.ytDate.isNull() ? QVariant() : QVariant(v.ytDate));
    q.addBindValue(v.id);
    run(q);
}

void Database::addCandidate(qint64 videoId, const TrackInfo &track, int stream, double fit, const QString &audioDetail)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("INSERT INTO candidates (video_id, track_key, stream, fit, audio_detail) VALUES (?, ?, ?, ?, ?)"
                             " ON CONFLICT(video_id, track_key) DO UPDATE SET stream = excluded.stream, fit = excluded.fit,"
                             " audio_detail = excluded.audio_detail"));
    q.addBindValue(videoId);
    q.addBindValue(trackKey(track));
    q.addBindValue(stream);
    q.addBindValue(fit);
    q.addBindValue(QString(audioDetail.isNull() ? QStringLiteral("") : audioDetail));
    run(q);
}

void Database::removeCandidate(qint64 videoId, const TrackInfo &track)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("DELETE FROM candidates WHERE video_id = ? AND track_key = ?"));
    q.addBindValue(videoId);
    q.addBindValue(trackKey(track));
    run(q);
}

Database::Transaction::Transaction(Database *db)
    : m_db(db)
{
    QSqlQuery q(m_db->conn());
    m_open = q.exec(QStringLiteral("BEGIN IMMEDIATE"));
}

Database::Transaction::~Transaction()
{
    if (!m_open)
        return;
    QSqlQuery q(m_db->conn());
    q.exec(QStringLiteral("COMMIT"));
}

void Database::clearCandidates(qint64 videoId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("DELETE FROM candidates WHERE video_id = ?"));
    q.addBindValue(videoId);
    run(q);
}

QVector<Database::Candidate> Database::candidates(qint64 videoId)
{
    QVector<Candidate> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT track_key, stream, fit, audio_detail FROM candidates WHERE video_id = ? ORDER BY rowid"));
    q.addBindValue(videoId);
    if (!run(q))
        return out;
    while (q.next()) {
        const qint64 key = q.value(0).toLongLong();
        QSqlQuery t(conn());
        t.prepare(QStringLiteral("SELECT %1 FROM tracks WHERE absent = 0 AND %2 = ?")
                      .arg(QLatin1String(kTrackCols), key > 0 ? QStringLiteral("recording") : QStringLiteral("id")));
        t.addBindValue(key > 0 ? key : -key);
        if (!run(t))
            continue;
        std::optional<TrackInfo> best;
        while (t.next()) {
            const TrackInfo file = readTrack(t);
            if (!best || betterSource(file, *best))
                best = file;
        }
        if (best)
            out << Candidate{*best, q.value(1).toInt(), q.value(2).toDouble(), q.value(3).toString()};
    }
    return out;
}

int Database::candidateCount(qint64 videoId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM candidates WHERE video_id = ?"));
    q.addBindValue(videoId);
    return run(q) && q.next() ? q.value(0).toInt() : 0;
}

QVector<VideoInfo> Database::candidacies(const TrackInfo &track)
{
    QVector<VideoInfo> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM videos WHERE review = 1 AND id IN (SELECT video_id FROM candidates WHERE track_key = ?) ORDER BY id")
                  .arg(QLatin1String(kVideoCols)));
    q.addBindValue(trackKey(track));
    if (run(q)) {
        while (q.next())
            out << resolved(readVideo(q));
    }
    return out;
}

QVector<qint64> Database::dropCandidaciesOfTracksWithVideos()
{
    QVector<qint64> videos;
    const QString settled = QStringLiteral(
        "track_key IN (SELECT %1 FROM tracks WHERE absent = 0 AND state = 'done' AND video_id IS NOT NULL)").arg(trackKeySql());
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT DISTINCT video_id FROM candidates WHERE ") + settled);
    if (run(q)) {
        while (q.next())
            videos << q.value(0).toLongLong();
    }
    if (!videos.isEmpty()) {
        QSqlQuery d(conn());
        d.prepare(QStringLiteral("DELETE FROM candidates WHERE ") + settled);
        run(d);
    }
    return videos;
}

int Database::requeueTracksWithoutCandidacies()
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET state = 'pending', message = '' WHERE state = 'review' AND absent = 0"
                             " AND NOT EXISTS (SELECT 1 FROM candidates c JOIN videos v ON v.id = c.video_id AND v.review = 1"
                             "  WHERE c.track_key = %1)").arg(trackKeySql("tracks.")));
    return run(q) ? q.numRowsAffected() : 0;
}

QVector<qint64> Database::sharedVideoIds()
{
    QVector<qint64> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT video_id FROM tracks WHERE absent = 0 AND video_id IS NOT NULL"
                             " GROUP BY video_id HAVING COUNT(DISTINCT %1) > 1 ORDER BY video_id").arg(trackKeySql()));
    if (run(q)) {
        while (q.next())
            out << q.value(0).toLongLong();
    }
    return out;
}

bool Database::isArtistChannel(const QString &artist, const QString &channelId, const QString &channelName)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT 1 FROM artist_channels WHERE artist = ? AND"
                             " ((channel_id != '' AND channel_id = ?) OR (channel != '' AND channel = ?))"));
    q.addBindValue(artist);
    q.addBindValue(channelId);
    q.addBindValue(channelName);
    return run(q) && q.next();
}

void Database::addArtistChannel(const QString &artist, const QString &channelId, const QString &channelName, const QString &source)
{
    if (channelId.isEmpty() && channelName.isEmpty())
        return;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO artist_channels (artist, channel_id, channel, source) VALUES (?, ?, ?, ?)"));
    q.addBindValue(artist);
    // A null QString would be bound as NULL, which the columns refuse.
    q.addBindValue(QString(channelId.isNull() ? QStringLiteral("") : channelId));
    q.addBindValue(QString(channelName.isNull() ? QStringLiteral("") : channelName));
    q.addBindValue(source);
    run(q);
}

QVector<QStringList> Database::artistChannels(const QString &artist)
{
    QVector<QStringList> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT channel_id, channel, source FROM artist_channels WHERE artist = ? AND (channel_id != '' OR channel != '')"));
    q.addBindValue(artist);
    if (run(q)) {
        while (q.next())
            out << QStringList{q.value(0).toString(), q.value(1).toString(), q.value(2).toString()};
    }
    return out;
}

bool Database::artistChannelsKnown(const QString &artist)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT 1 FROM artist_channels WHERE artist = ? AND channel_id = '' AND channel = '' AND checked_until > ?"));
    q.addBindValue(artist);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    return run(q) && q.next();
}

void Database::setArtistChannelsKnown(const QString &artist, int days)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO artist_channels (artist, channel_id, channel, source, checked_until) VALUES (?, '', '', 'musicbrainz', ?)"));
    q.addBindValue(artist);
    q.addBindValue(QDateTime::currentSecsSinceEpoch() + qint64(days) * 86400);
    run(q);
}

void Database::setTrackPending(qint64 trackId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET state = 'pending' WHERE id = ?"));
    q.addBindValue(trackId);
    run(q);
}

QVector<TrackInfo> Database::unlinkOthers(qint64 videoId, const TrackInfo &keeper)
{
    QVector<TrackInfo> out;
    for (const TrackInfo &t : tracksForVideo(videoId)) {
        if (trackKey(t) != trackKey(keeper))
            out << t;
    }
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET state = 'pending', video_id = NULL, message = ''"
                             " WHERE video_id = ? AND %1 != ?").arg(trackKeySql()));
    q.addBindValue(videoId);
    q.addBindValue(trackKey(keeper));
    run(q);
    return out;
}

void Database::unlinkAll(qint64 videoId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET state = 'pending', video_id = NULL, message = '' WHERE video_id = ?"));
    q.addBindValue(videoId);
    run(q);
}

void Database::setTrackReimport(qint64 trackId)
{
    // The whole recording: the lookup runs under its best file, whichever
    // that is (see pendingRecordings), and that file has to carry the mark.
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET state = 'pending', reimport = 1, message = ''"
                             " WHERE absent = 0 AND (id = ? OR recording = (SELECT recording FROM tracks WHERE id = ?))"));
    q.addBindValue(trackId);
    q.addBindValue(trackId);
    run(q);
}

void Database::clearTrackReimport(qint64 trackId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET reimport = 0"
                             " WHERE id = ? OR recording = (SELECT recording FROM tracks WHERE id = ?)"));
    q.addBindValue(trackId);
    q.addBindValue(trackId);
    run(q);
}

QVector<qint64> Database::trackIdsUnder(const QString &path)
{
    QVector<qint64> out;
    QString dir = QDir::cleanPath(path);
    while (dir.size() > 1 && dir.endsWith(QLatin1Char('/')))
        dir.chop(1);
    QSqlQuery q(conn());
    // Paths are stored as the scan found them, with "/" between folders.
    q.prepare(QStringLiteral("SELECT id FROM tracks WHERE absent = 0 AND (path = ? OR substr(path, 1, ?) = ?) ORDER BY path"));
    q.addBindValue(dir);
    q.addBindValue(dir.size() + 1);
    q.addBindValue(dir + QLatin1Char('/'));
    if (run(q)) {
        while (q.next())
            out << q.value(0).toLongLong();
    }
    return out;
}

QVector<qint64> Database::trackIdsImportedBefore(qint64 secs)
{
    QVector<qint64> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT id FROM tracks WHERE absent = 0 AND ((state = 'done' AND video_id IS NOT NULL) OR state = 'review')"
                             " AND last_attempt < ? ORDER BY path"));
    q.addBindValue(secs);
    if (run(q)) {
        while (q.next())
            out << q.value(0).toLongLong();
    }
    return out;
}

namespace {
QString rejectionKey(const TrackInfo &t)
{
    return t.recording > 0 ? QStringLiteral("r%1").arg(t.recording) : QStringLiteral("t%1").arg(t.id);
}
} // namespace

void Database::rejectVideoFor(const TrackInfo &track, const QString &ytId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO rejected_videos (key, yt_id) VALUES (?, ?)"));
    q.addBindValue(rejectionKey(track));
    q.addBindValue(ytId);
    run(q);
}

void Database::unrejectVideoFor(const TrackInfo &track, const QString &ytId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("DELETE FROM rejected_videos WHERE key = ? AND yt_id = ?"));
    q.addBindValue(rejectionKey(track));
    q.addBindValue(ytId);
    run(q);
}

QVector<Database::CandidateRow> Database::candidateRows(qint64 videoId)
{
    QVector<CandidateRow> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT track_key, stream, fit, audio_detail FROM candidates WHERE video_id = ? ORDER BY rowid"));
    q.addBindValue(videoId);
    if (!run(q))
        return out;
    while (q.next())
        out << CandidateRow{q.value(0).toLongLong(), q.value(1).toInt(), q.value(2).toDouble(), q.value(3).toString()};
    return out;
}

void Database::restoreCandidates(qint64 videoId, const QVector<CandidateRow> &rows)
{
    clearCandidates(videoId);
    for (const CandidateRow &r : rows) {
        QSqlQuery q(conn());
        q.prepare(QStringLiteral("INSERT INTO candidates (video_id, track_key, stream, fit, audio_detail) VALUES (?, ?, ?, ?, ?)"));
        q.addBindValue(videoId);
        q.addBindValue(r.trackKey);
        q.addBindValue(r.stream);
        q.addBindValue(r.fit);
        q.addBindValue(r.audioDetail);
        run(q);
    }
}

QVector<Database::TrackState> Database::trackStates(qint64 trackKey)
{
    QVector<TrackState> out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT id, state, video_id, message FROM tracks WHERE %1 = ?")
                  .arg(trackKey > 0 ? QStringLiteral("recording") : QStringLiteral("id")));
    q.addBindValue(trackKey > 0 ? trackKey : -trackKey);
    if (!run(q))
        return out;
    while (q.next())
        out << TrackState{q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toLongLong(), q.value(3).toString()};
    return out;
}

void Database::restoreTrackState(const TrackState &t)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("UPDATE tracks SET state = ?, video_id = ?, message = ? WHERE id = ?"));
    q.addBindValue(t.state);
    q.addBindValue(t.videoId > 0 ? QVariant(t.videoId) : QVariant());
    q.addBindValue(t.message);
    q.addBindValue(t.id);
    run(q);
}

bool Database::restoreVideo(const VideoInfo &v)
{
    // (The columns of kVideoCols after the id, in that order.)
    static const QStringList cols = QString::fromLatin1(kVideoCols).split(QStringLiteral(", ")).mid(1);
    const auto bind = [&](QSqlQuery &q) {
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
        q.addBindValue(v.ytAbr > 0 ? QVariant(v.ytAbr) : QVariant());
        q.addBindValue(v.review);
        q.addBindValue(v.reviewStart);
        q.addBindValue(v.reviewEnd);
        q.addBindValue(v.ytStream);
        q.addBindValue(v.ytChannelId);
        q.addBindValue(v.ytChannelSubs > -2 ? QVariant(v.ytChannelSubs) : QVariant());
        q.addBindValue(v.ytChannelVerified);
        q.addBindValue(v.ytChannelHandle.isNull() ? QVariant() : QVariant(v.ytChannelHandle));
        q.addBindValue(v.vouched && !v.review);
        q.addBindValue(v.ytDate.isNull() ? QVariant() : QVariant(v.ytDate));
    };
    QStringList sets;
    for (const QString &c : cols)
        sets << c + QStringLiteral(" = ?");
    QSqlQuery u(conn());
    u.prepare(QStringLiteral("UPDATE videos SET %1 WHERE id = ?").arg(sets.join(QStringLiteral(", "))));
    bind(u);
    u.addBindValue(v.id);
    if (!run(u))
        return false;
    if (u.numRowsAffected() > 0)
        return true;
    if (videoByYtId(v.ytId))
        return false;
    QSqlQuery i(conn());
    i.prepare(QStringLiteral("INSERT INTO videos (id, %1) VALUES (?%2)")
                  .arg(cols.join(QStringLiteral(", ")), QStringLiteral(", ?").repeated(cols.size())));
    i.addBindValue(v.id);
    bind(i);
    return run(i);
}

bool Database::videoRejectedFor(const TrackInfo &track, const QString &ytId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT 1 FROM rejected_videos WHERE key = ? AND yt_id = ?"));
    q.addBindValue(rejectionKey(track));
    q.addBindValue(ytId);
    return run(q) && q.next();
}

QVector<VideoInfo> Database::videosOfChannel(const QString &channelId)
{
    QVector<VideoInfo> out;
    if (channelId.isEmpty())
        return out;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT %1 FROM videos WHERE yt_channel_id = ? ORDER BY artist COLLATE NOCASE, title COLLATE NOCASE, id")
                  .arg(QLatin1String(kVideoCols)));
    q.addBindValue(channelId);
    if (run(q)) {
        while (q.next())
            out << resolved(readVideo(q));
    }
    return out;
}

QString Database::channelList(const QString &channelId)
{
    if (channelId.isEmpty())
        return {};
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT list FROM channel_lists WHERE channel_id = ?"));
    q.addBindValue(channelId);
    return run(q) && q.next() ? q.value(0).toString() : QString();
}

void Database::setChannelList(const QString &channelId, const QString &list, const QString &name, const QString &handle)
{
    if (channelId.isEmpty())
        return;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO channel_lists (channel_id, list, name, handle, added_at) VALUES (?, ?, ?, ?, ?)"));
    q.addBindValue(channelId);
    q.addBindValue(list);
    q.addBindValue(name);
    q.addBindValue(handle);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    run(q);
}

void Database::removeChannelList(const QString &channelId)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("DELETE FROM channel_lists WHERE channel_id = ?"));
    q.addBindValue(channelId);
    run(q);
}

namespace {
// A channel's own name out of the names its uploads are listed under. An
// upload that YouTube lists under several channels carries "Name and Other"
// (or "Name and 2 more"), and the library keeps what the listing said.
QString ownChannelName(const QStringList &listed)
{
    if (listed.isEmpty())
        return {};
    // One of them is the name alone: the others begin with it.
    for (const QString &name : listed) {
        if (std::all_of(listed.begin(), listed.end(), [&name](const QString &other) {
                return other == name || other.startsWith(name + QStringLiteral(" and "));
            }))
            return name;
    }
    // None is: what they have in common, before the " and ".
    QString common = listed.first();
    for (const QString &other : listed) {
        qsizetype n = 0;
        while (n < common.size() && n < other.size() && common[n] == other[n])
            ++n;
        common.truncate(n);
    }
    const qsizetype cut = common.lastIndexOf(QStringLiteral(" and "));
    return cut > 0 ? common.left(cut) : listed.first();
}
}

Database::ListedChannel Database::knownChannel(const QString &idOrHandle)
{
    ListedChannel c;
    const bool byHandle = idOrHandle.startsWith(QLatin1Char('@'));
    if (byHandle) {
        QSqlQuery q(conn());
        q.prepare(QStringLiteral("SELECT yt_channel_id FROM videos WHERE yt_channel_id != '' AND lower(yt_channel_handle) = lower(?) LIMIT 1"));
        q.addBindValue(idOrHandle);
        if (run(q) && q.next())
            c.id = q.value(0).toString();
    } else {
        c.id = idOrHandle;
    }
    QSqlQuery l(conn());
    l.prepare(QStringLiteral("SELECT channel_id, list, name, handle, added_at FROM channel_lists WHERE %1")
                  .arg(byHandle && c.id.isEmpty() ? QStringLiteral("lower(handle) = lower(?)") : QStringLiteral("channel_id = ?")));
    l.addBindValue(c.id.isEmpty() ? idOrHandle : c.id);
    if (run(l) && l.next()) {
        c.id = l.value(0).toString();
        c.list = l.value(1).toString();
        c.name = l.value(2).toString();
        c.handle = l.value(3).toString();
        c.addedAt = l.value(4).toLongLong();
    }
    if (c.id.isEmpty())
        return c;
    // Its name and handle as the videos the library has of it carry them.
    QStringList names;
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("SELECT DISTINCT yt_channel, yt_channel_handle FROM videos WHERE yt_channel_id = ? ORDER BY length(yt_channel), yt_channel"));
    q.addBindValue(c.id);
    if (run(q)) {
        while (q.next()) {
            if (!q.value(0).toString().isEmpty() && !names.contains(q.value(0).toString()))
                names << q.value(0).toString();
            if (c.handle.isEmpty())
                c.handle = q.value(1).toString();
        }
    }
    if (!names.isEmpty())
        c.name = ownChannelName(names);
    return c;
}

QVector<Database::ListedChannel> Database::channelLists()
{
    QVector<ListedChannel> out;
    QStringList ids;
    {
        QSqlQuery q(conn());
        if (q.exec(QStringLiteral("SELECT channel_id FROM channel_lists ORDER BY name COLLATE NOCASE, channel_id"))) {
            while (q.next())
                ids << q.value(0).toString();
        }
    }
    // (Each under its own name as the library knows it now, not only as it
    // was listed on the upload the user had in front of them.)
    for (const QString &id : std::as_const(ids))
        out << knownChannel(id);
    return out;
}

void Database::removeVideo(qint64 id)
{
    QSqlQuery q(conn());
    q.prepare(QStringLiteral("DELETE FROM videos WHERE id = ?"));
    q.addBindValue(id);
    run(q);
}
