#pragma once

#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

// Album covers for the music player's grid: an image beside the album's
// first file (cover.jpg, folder.png, front.jpg and the like), else the
// picture embedded in that file; scaled down and kept as a JPEG under
// <mv dir>/.mvplayer/covers, named by the album's key. Made in the
// background; coverReady says when one is there.
class CoverCache : public QObject
{
    Q_OBJECT
public:
    explicit CoverCache(QObject *parent = nullptr);
    ~CoverCache() override;

    void setDir(const QString &dir);
    QString dir() const { return m_dir; }

    // The album's cached cover, if it is there and no older than the file it
    // came from; else empty, and with `request` it is made in the background
    // (coverReady follows), unless the album was found to have none.
    QString lookup(const QString &key, const QString &trackPath, bool request);

    // The pieces, for tests and the worker: where an album's cover file is,
    // making it now (empty when the album has no picture, with a marker so
    // that it is not tried again until the file changes), and the picture
    // itself (null when there is none).
    static QString fileFor(const QString &dir, const QString &key);
    static QString make(const QString &dir, const QString &key, const QString &trackPath);
    static QImage extract(const QString &trackPath);
    static constexpr int kMaxSide = 512;

signals:
    void coverReady(const QString &key, const QString &path);

private:
    QString m_dir;
    QSet<QString> m_inFlight;
    QThreadPool m_pool;
};
