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
    // Records the outcome of a lookup, for every file of the track's recording.
    void setTrackResult(qint64 id, const QString &state, qint64 videoId, const QString &message);
    // Starts a recording of its own for the track.
    void setRecording(qint64 trackId, qint64 recording);
    // Makes the track one more file of the recording `memberId` belongs to.
    // The import state of the recording becomes the track's too, or the other
    // way round when `trackLeads`.
    void joinRecording(qint64 trackId, qint64 memberId, bool trackLeads);
    // A recording without a video is searched for once more under each
    // title its files carry that has not been tried: the same song may be
    // tagged in the original script in one place and romanised in another.
    // Makes one such file per recording pending; returns how many.
    int requeueUntriedTitles();
    // The tracks waiting for a lookup, one per recording: its best file.
    QVector<TrackInfo> pendingRecordings();

    // Audio fingerprints (AudioPrint::pack), by track id. One is current
    // while the file still has the size it was taken at.
    struct StoredPrint {
        QByteArray packed;
        bool current = false;
    };
    QHash<qint64, StoredPrint> fingerprints();
    void setFingerprint(qint64 trackId, const QByteArray &packed, qint64 fileSize);
    qint64 newRecording();
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
    // After a video's file was rebuilt from better streams.
    void updateVideoMedia(const VideoInfo &v);
    void removeVideo(qint64 id);
    // Videos imported before their picture was reliably checked for stills.
    QVector<VideoInfo> videosNotStillChecked();
    void markStillChecked(qint64 videoId);
    // Videos whose main audio is YouTube's and has not been reviewed: where
    // the library's own audio is the better one, it belongs in the video.
    QVector<VideoInfo> videosNotAudioChecked();
    void markAudioChecked(qint64 videoId);
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
