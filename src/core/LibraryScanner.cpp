#include "core/LibraryScanner.h"

#include "core/AudioPrint.h"
#include "core/Matcher.h"
#include "core/TagReader.h"
#include "core/Util.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QMultiHash>
#include <QSet>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <optional>

namespace LibraryScanner {

// Drops roots that do not exist or sit inside another root.
QStringList normaliseRoots(const QStringList &roots)
{
    QStringList out;
    for (const QString &r : roots) {
        const QDir d(r);
        if (r.isEmpty() || !d.exists())
            continue;
        const QString abs = d.absolutePath();
        if (!out.contains(abs, pathCase))
            out << abs;
    }
    QStringList top;
    for (const QString &a : out) {
        bool nested = false;
        for (const QString &b : out) {
            // A drive root ("C:/") already ends with the separator.
            const QString prefix = b.endsWith(QLatin1Char('/')) ? b : b + QLatin1Char('/');
            nested |= a != b && a.startsWith(prefix, pathCase);
        }
        if (!nested)
            top << a;
    }
    return top;
}

// Two files with the same bytes of audio and the same tags, wherever they are.
QString contentKey(const TrackInfo &t)
{
    return QStringList{QString::number(t.size), QString::number(qRound64(t.duration * 1000)), QString::number(t.discNo),
                       QString::number(t.trackNo), t.title, t.artist, t.album}.join(QChar(0x1f));
}

// The same tags on audio of the same length: a converted copy, when there is
// no fingerprint to compare.
bool sameTags(const TrackInfo &a, const TrackInfo &b)
{
    return a.title == b.title && a.artist == b.artist && a.album == b.album && a.trackNo == b.trackNo
        && a.discNo == b.discNo && std::abs(a.duration - b.duration) <= 2.0;
}

// How far a lookup has come; the further state wins when two files turn out
// to hold the same recording.
int progress(const QString &state)
{
    return state == QLatin1String("done") ? 3 : state == QLatin1String("pending") ? 2
         : state == QLatin1String("failed") ? 1 : 0;
}

// Fingerprints files on a few threads. An entry stays empty when its file
// cannot be decoded.
QVector<AudioPrint::Print> printsOf(const QStringList &files, const std::atomic<bool> *cancel)
{
    QVector<AudioPrint::Print> out(files.size());
    QThreadPool pool;
    pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 4));
    for (int i = 0; i < files.size(); ++i) {
        pool.start([&out, &files, cancel, i] {
            if (!cancel || !cancel->load())
                AudioPrint::ofFile(files[i], &out[i], cancel, nullptr);
        });
    }
    pool.waitForDone();
    return out;
}

constexpr int kPrintBatch = 16; // files fingerprinted between two saves

Result scan(const QStringList &rootsIn, Database &db, const std::atomic<bool> *cancel)
{
    Result res;
    const QStringList roots = normaliseRoots(rootsIn);
    const QHash<QString, TrackInfo> known = db.tracksByPath();
    const QStringList &exts = TagReader::audioExtensions();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const auto cancelled = [cancel] { return cancel && cancel->load(); };
    QSet<QString> dirs;
    QSet<QString> seen;
    QVector<TrackInfo> fresh;
    QHash<qint64, TrackInfo> current; // tracks in the library, as the database has them now

    const auto save = [&](TrackInfo &t) {
        if (!db.upsertTrack(t))
            return false;
        current.insert(t.id, t);
        return true;
    };

    // A known track whose file changed or moved: it keeps its video and
    // import state, the tags are refreshed.
    const auto refresh = [&](TrackInfo &t, const TrackInfo &old) {
        t.id = old.id;
        t.recording = old.recording;
        t.videoId = old.videoId;
        t.attempts = old.attempts;
        t.lastAttempt = old.lastAttempt;
        t.state = old.state;
        t.message = old.message;
        const bool retagged = t.title != old.title || t.artist != old.artist
            || t.albumArtist != old.albumArtist || t.album != old.album;
        if (t.state != QLatin1String("done") && retagged) {
            // New tags can turn a failed lookup into a hit.
            QString why;
            const bool nonMv = Matcher::isNonMvTrack(t, &why);
            t.state = nonMv ? QStringLiteral("skipped") : QStringLiteral("pending");
            t.message = nonMv ? why : QString();
        }
        if (save(t))
            ++res.changed;
    };

    for (const QString &root : roots) {
        const QDir rootDir(root);
        dirs.insert(rootDir.absolutePath());

        QDirIterator it(rootDir.absolutePath(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            if (cancelled())
                return res;
            const QFileInfo fi = it.nextFileInfo();
            if (fi.isDir()) {
                dirs.insert(fi.absoluteFilePath());
                continue;
            }
            if (fi.fileName().startsWith(QLatin1Char('.')) || !exts.contains(fi.suffix().toLower()))
                continue;

            const QString path = fi.absoluteFilePath();
            const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
            const qint64 size = fi.size();
            seen.insert(path);
            ++res.total;

            const auto old = known.constFind(path);
            if (old != known.constEnd() && old->mtime == mtime && old->size == size) {
                current.insert(old->id, *old);
                continue;
            }

            // A file modified within the last few seconds may still be copying.
            if (now - mtime < 3) {
                res.unsettled = true;
                if (old != known.constEnd())
                    current.insert(old->id, *old);
                continue;
            }

            TrackInfo t;
            t.path = path;
            t.mtime = mtime;
            t.size = size;
            if (!TagReader::read(t)) {
                if (old != known.constEnd())
                    current.insert(old->id, *old);
                continue;
            }

            if (old == known.constEnd())
                fresh << t; // new, or a known track at a new place: settled after the walk
            else
                refresh(t, *old);
        }

    }

    // ---- Known tracks whose file is gone -----------------------------------
    // A file that turned up while one of these went missing may be that track
    // at a new place: the library was reorganised, converted to another
    // format, or is on another device now.
    QVector<TrackInfo> gone;
    for (auto k = known.constBegin(); k != known.constEnd(); ++k) {
        if (!seen.contains(k.key()))
            gone << *k;
    }
    QSet<qint64> moved;
    const auto moveHere = [&](TrackInfo &t, const TrackInfo &old) {
        if (!db.moveTrack(old.id, t.path))
            return false;
        moved.insert(old.id);
        refresh(t, old);
        return true;
    };

    // The same file: nothing to decode.
    {
        QMultiHash<QString, int> byContent;
        for (int i = 0; i < gone.size(); ++i)
            byContent.insert(contentKey(gone[i]), i);
        QVector<TrackInfo> rest;
        for (TrackInfo &t : fresh) {
            const auto g = byContent.find(contentKey(t));
            if (g != byContent.end()) {
                const int i = *g;
                byContent.erase(g);
                if (moveHere(t, gone[i]))
                    continue;
            }
            rest << t;
        }
        fresh = rest;
    }

    // ---- Identify by sound ---------------------------------------------------
    // Needed for: files not seen before, tracks from before fingerprints were
    // kept, and tracks whose file changed size since theirs was taken.
    QHash<qint64, AudioPrint::Print> prints; // current fingerprints of tracks in the library
    QHash<qint64, AudioPrint::Print> stale;
    QHash<qint64, AudioPrint::Print> gonePrints;
    {
        const QHash<qint64, Database::StoredPrint> stored = db.fingerprints();
        for (auto s = stored.constBegin(); s != stored.constEnd(); ++s) {
            const AudioPrint::Print print = AudioPrint::unpack(s->packed);
            if (!current.contains(s.key()))
                gonePrints.insert(s.key(), print);
            else if (s->current)
                prints.insert(s.key(), print);
            else
                stale.insert(s.key(), print);
        }
    }

    struct Todo {
        TrackInfo t;
        bool isNew = false;
    };
    QVector<Todo> todo;
    for (TrackInfo &t : fresh) {
        QString why;
        if (Matcher::isNonMvTrack(t, &why)) {
            // Never looked up, so there is nothing to carry over or share.
            t.state = QStringLiteral("skipped");
            t.message = why;
            if (save(t))
                ++res.added;
            continue;
        }
        todo.append({t, true});
    }
    for (auto c = current.constBegin(); c != current.constEnd(); ++c) {
        if (c->state != QLatin1String("skipped") && !prints.contains(c->id))
            todo.append({*c, false});
    }
    // New files first: they may be tracks that went missing, which has to be
    // settled before anything is taken for a recording not seen before.
    std::sort(todo.begin(), todo.end(), [](const Todo &a, const Todo &b) {
        return a.isNew != b.isNew ? a.isNew : a.t.path < b.t.path;
    });
    const int newCount = int(std::count_if(todo.begin(), todo.end(), [](const Todo &e) { return e.isNew; }));
    if (!todo.isEmpty())
        qInfo("[scan] identifying %d tracks by their sound", int(todo.size()));

    // A file of the recording a fingerprint belongs to, among the tracks in
    // the library.
    const auto findRecording = [&](const AudioPrint::Print &print, qint64 notTrack) -> std::optional<TrackInfo> {
        for (auto p = prints.constBegin(); p != prints.constEnd(); ++p) {
            if (p.key() == notTrack)
                continue;
            const auto member = current.constFind(p.key());
            if (member != current.constEnd() && member->recording > 0 && AudioPrint::sameRecording(print, *p))
                return *member;
        }
        return std::nullopt;
    };
    // After a change to a recording, what the database holds for its files.
    const auto reload = [&](qint64 recording) {
        for (TrackInfo &t : current) {
            if (t.recording == recording) {
                if (const auto now = db.track(t.id))
                    t = *now;
            }
        }
    };

    for (int from = 0, count = 0; from < todo.size(); from += count) {
        // While tracks are missing, all new files are looked at together.
        const bool matchGone = from < newCount && moved.size() < gone.size();
        count = matchGone ? newCount - from : qMin(kPrintBatch, int(todo.size()) - from);
        QStringList files;
        for (int i = 0; i < count; ++i)
            files << todo[from + i].t.path;
        const QVector<AudioPrint::Print> batch = printsOf(files, cancel);
        if (cancelled())
            return res;

        // A converted or re-encoded copy of a track that went missing takes
        // its place. Where several missing tracks sound the same (a single
        // and its album cut), the one with the same tags is meant; a missing
        // track without a fingerprint is known by its tags alone.
        QSet<int> placed;
        enum Pass { SoundAndTags, Sound, Tags };
        for (const Pass pass : {SoundAndTags, Sound, Tags}) {
            for (int i = 0; matchGone && i < count; ++i) {
                TrackInfo &t = todo[from + i].t;
                if (placed.contains(i) || (pass != Tags && batch[i].isEmpty()))
                    continue;
                for (const TrackInfo &g : std::as_const(gone)) {
                    if (moved.contains(g.id))
                        continue;
                    const auto gp = gonePrints.constFind(g.id);
                    const bool hasPrint = gp != gonePrints.constEnd() && !gp->isEmpty();
                    const bool same = pass == Tags ? !hasPrint && sameTags(t, g)
                        : hasPrint && (pass == Sound || sameTags(t, g)) && AudioPrint::sameRecording(batch[i], *gp);
                    if (same && moveHere(t, g)) {
                        placed.insert(i);
                        break;
                    }
                }
            }
        }

        for (int i = 0; i < count; ++i) {
            TrackInfo t = todo[from + i].t;
            const AudioPrint::Print &print = batch[i];
            if (todo[from + i].isNew && !placed.contains(i)) {
                if (!save(t))
                    continue;
                ++res.added;
            }
            if (print.isEmpty()) {
                // Cannot be decoded: stays a track of its own, and is not tried again.
                db.setFingerprint(t.id, QByteArrayLiteral("-"), t.size);
                prints.insert(t.id, print);
                continue;
            }

            bool isNew = todo[from + i].isNew && t.attempts == 0 && t.recording <= 0;
            const auto before = stale.constFind(t.id);
            if (before != stale.constEnd() && !AudioPrint::sameRecording(print, *before)) {
                // Different audio under the same name: a new track as far as
                // its video is concerned.
                t.recording = 0;
                t.state = QStringLiteral("pending");
                t.videoId = 0;
                t.attempts = 0;
                t.lastAttempt = 0;
                t.message.clear();
                save(t);
                isNew = true;
            }
            if (t.recording <= 0) {
                const std::optional<TrackInfo> member = findRecording(print, t.id);
                // Two files that each have a video of their own keep them.
                if (member && !(t.state == QLatin1String("done") && member->state == QLatin1String("done")
                                && t.videoId != member->videoId)) {
                    // A file new to the library takes what is known about its
                    // recording; otherwise whichever side has come further
                    // with its lookup passes that on to the other.
                    db.joinRecording(t.id, member->id, !isNew && progress(t.state) > progress(member->state));
                    t.recording = member->recording;
                } else {
                    t.recording = db.newRecording();
                    db.setRecording(t.id, t.recording);
                }
                current.insert(t.id, t);
                reload(t.recording);
            }
            db.setFingerprint(t.id, AudioPrint::pack(print), t.size);
            prints.insert(t.id, print);
        }
    }

    // Tracks that left the library, or whose folder is no longer part of
    // it. Their videos stay in the MV library.
    for (const TrackInfo &g : std::as_const(gone)) {
        if (!moved.contains(g.id)) {
            db.removeTrack(g.id);
            ++res.removed;
        }
    }

    // A video carries the tags of the best file it belongs to.
    QHash<qint64, TrackInfo> best;
    for (const TrackInfo &t : std::as_const(current)) {
        if (t.state != QLatin1String("done") || t.videoId <= 0)
            continue;
        const auto b = best.find(t.videoId);
        if (b == best.end())
            best.insert(t.videoId, t);
        else if (betterSource(t, *b))
            *b = t;
    }
    for (const TrackInfo &t : std::as_const(best)) {
        auto v = db.video(t.videoId);
        if (!v || (v->title == t.title && v->artist == t.artist && v->albumArtist == t.albumArtist && v->album == t.album
                   && v->genre == t.genre && v->year == t.year && v->trackNo == t.trackNo && v->tags == t.tags))
            continue;
        v->title = t.title;
        v->artist = t.artist;
        v->albumArtist = t.albumArtist;
        v->album = t.album;
        v->genre = t.genre;
        v->year = t.year;
        v->trackNo = t.trackNo;
        v->tags = t.tags;
        db.updateVideoTags(*v);
        res.videosChanged << v->id;
    }

    res.directories = QStringList(dirs.begin(), dirs.end());
    res.ok = true;
    return res;
}

} // namespace LibraryScanner
