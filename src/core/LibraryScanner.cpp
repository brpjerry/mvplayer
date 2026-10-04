#include "core/LibraryScanner.h"

#include "core/Matcher.h"
#include "core/TagReader.h"
#include "core/Util.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QMultiHash>
#include <QSet>

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

// What makes two files the same track wherever they are stored.
QString contentKey(const TrackInfo &t)
{
    return QStringList{QString::number(t.size), QString::number(qRound64(t.duration * 1000)), QString::number(t.discNo),
                       QString::number(t.trackNo), t.title, t.artist, t.album}.join(QChar(0x1f));
}

Result scan(const QStringList &rootsIn, Database &db, const std::atomic<bool> *cancel)
{
    Result res;
    const QStringList roots = normaliseRoots(rootsIn);
    QHash<QString, TrackInfo> known = db.tracksByPath();
    const QStringList &exts = TagReader::audioExtensions();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSet<QString> dirs;
    QSet<QString> seen;
    QVector<TrackInfo> fresh;

    // A known track whose file changed: keep its video, refresh the tags.
    const auto refresh = [&](TrackInfo &t, const TrackInfo &old) {
        t.id = old.id;
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
        if (!db.upsertTrack(t))
            return;
        ++res.changed;
        if (t.state == QLatin1String("pending"))
            res.queued << t.id;
        if (t.state == QLatin1String("done") && t.videoId > 0) {
            if (auto v = db.video(t.videoId)) {
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
        }
    };

    for (const QString &root : roots) {
        const QDir rootDir(root);
        dirs.insert(rootDir.absolutePath());

        QDirIterator it(rootDir.absolutePath(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            if (cancel && cancel->load())
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
            if (old != known.constEnd() && old->mtime == mtime && old->size == size)
                continue;

            // A file modified within the last few seconds may still be copying.
            if (now - mtime < 3) {
                res.unsettled = true;
                continue;
            }

            TrackInfo t;
            t.path = path;
            t.mtime = mtime;
            t.size = size;
            if (!TagReader::read(t))
                continue;

            if (old == known.constEnd())
                fresh << t; // new, or a known track at a new place: settled after the walk
            else
                refresh(t, *old);
        }

    }

    // A file that turned up while a known track with the same content went
    // missing is that track at a new place (the library was reorganised, or
    // is on another device now): it keeps its video and its import state.
    QMultiHash<QString, TrackInfo> gone;
    for (auto k = known.constBegin(); k != known.constEnd(); ++k) {
        if (!seen.contains(k.key()))
            gone.insert(contentKey(*k), *k);
    }
    QSet<qint64> moved;
    for (TrackInfo &t : fresh) {
        const auto g = gone.find(contentKey(t));
        if (g != gone.end()) {
            const TrackInfo old = *g;
            gone.erase(g);
            if (db.moveTrack(old.id, t.path)) {
                moved.insert(old.id);
                refresh(t, old);
                continue;
            }
        }
        QString why;
        if (Matcher::isNonMvTrack(t, &why)) {
            t.state = QStringLiteral("skipped");
            t.message = why;
        }
        if (db.upsertTrack(t)) {
            ++res.added;
            if (t.state == QLatin1String("pending"))
                res.queued << t.id;
        }
    }

    // Tracks that left the library, or whose folder is no longer part of
    // it. Their videos stay in the MV library.
    for (auto k = known.constBegin(); k != known.constEnd(); ++k) {
        if (!seen.contains(k.key()) && !moved.contains(k->id)) {
            db.removeTrack(k->id);
            ++res.removed;
        }
    }

    res.directories = QStringList(dirs.begin(), dirs.end());
    res.ok = true;
    return res;
}

} // namespace LibraryScanner
