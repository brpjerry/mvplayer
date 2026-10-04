#pragma once

#include "core/Types.h"

#include <QHash>
#include <QSqlDatabase>
#include <QVector>

#include <optional>

// SQLite store for the MV library. Safe to use from any thread: every thread
// lazily gets its own connection.
//
// Video and thumbnail files are stored relative to the MV folder, so the
// folder can be moved or copied to another device as a whole. Callers only
// ever see absolute paths.
class Database
{
public:
    // `mvDir` is the folder the stored video paths are relative to.
    Database(const QString &file, const QString &mvDir);

    bool init(QString *error);
    QString file() const { return m_file; }

    // Tracks (the audio library mirror)
    QHash<QString, TrackInfo> tracksByPath();
    QVector<TrackInfo> tracksInState(const QStringList &states);
    std::optional<TrackInfo> track(qint64 id);
    bool upsertTrack(TrackInfo &t);
    // The track's file is now at `path` (it was moved, or the library is on
    // another device); its import state and video stay with it.
    bool moveTrack(qint64 id, const QString &path);
    void removeTrack(qint64 id);
    void setTrackResult(qint64 id, const QString &state, qint64 videoId, const QString &message);
    void resetTracks(const QStringList &fromStates);
    // Makes failed / not-found tracks pending again once their last attempt
    // is old enough. The wait for failed tracks doubles with every attempt.
    void requeueStale(qint64 failedAfterSecs, qint64 notFoundAfterSecs);
    QVector<TrackInfo> tracksForVideo(qint64 videoId);
    QHash<QString, int> trackStateCounts();

    // Videos
    QVector<VideoInfo> allVideos();
    std::optional<VideoInfo> video(qint64 id);
    std::optional<VideoInfo> videoByYtId(const QString &ytId);
    qint64 insertVideo(const VideoInfo &v);
    void updateVideoTags(const VideoInfo &v);
    void removeVideo(qint64 id);
    // Videos imported before their picture was reliably checked for stills.
    QVector<VideoInfo> videosNotStillChecked();
    void markStillChecked(qint64 videoId);
    // Makes the tracks that point at a video pending again.
    void requeueTracksOfVideo(qint64 videoId);

private:
    QSqlDatabase conn();
    QString storedPath(const QString &path) const;
    QString resolvedPath(const QString &stored) const;
    VideoInfo resolved(VideoInfo v) const;
    void makeVideoPathsRelative(QSqlDatabase &db);

    QString m_file;
    QString m_mvDir;
};
