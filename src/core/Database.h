#pragma once

#include "core/Types.h"

#include <QHash>
#include <QSqlDatabase>
#include <QVector>

#include <optional>

// SQLite store for the MV library. Safe to use from any thread: every thread
// lazily gets its own connection.
class Database
{
public:
    explicit Database(const QString &file);

    bool init(QString *error);
    QString file() const { return m_file; }

    // Tracks (the audio library mirror)
    QHash<QString, TrackInfo> tracksByPath();
    QVector<TrackInfo> tracksInState(const QStringList &states);
    std::optional<TrackInfo> track(qint64 id);
    bool upsertTrack(TrackInfo &t);
    void removeTrack(qint64 id);
    void setTrackResult(qint64 id, const QString &state, qint64 videoId, const QString &message);
    void resetTracks(const QStringList &fromStates);
    // Makes failed / not-found tracks pending again once their last attempt is old enough.
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

private:
    QSqlDatabase conn();
    QString m_file;
};
