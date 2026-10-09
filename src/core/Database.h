#pragma once

#include "core/Types.h"

#include <QHash>
#include <QSet>
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
    // The same tracks with only what a list of them shows filled in: title,
    // artist, album artist, state and message. Several times quicker (no tags
    // to read).
    QVector<TrackInfo> trackSummariesInState(const QStringList &states);
    std::optional<TrackInfo> track(qint64 id);
    bool upsertTrack(TrackInfo &t);
    // The track's file is now at `path` (it was moved, or the library is on
    // another device); its import state and video stay with it.
    bool moveTrack(qint64 id, const QString &path);
    void removeTrack(qint64 id);
    // Marks a track as gone from the music folders, or as back in them.
    void setTrackAbsent(qint64 id, bool absent);
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
    // The tracks in the music folders that have this video; with
    // `includeAbsent`, also those whose files have left them.
    QVector<TrackInfo> tracksForVideo(qint64 videoId, bool includeAbsent = false);
    // Videos that no track in the music folders has: their tracks left, with
    // a folder that was removed or files that were deleted. Not those the
    // user said to keep all the same.
    QVector<VideoInfo> untrackedVideos();
    QSet<qint64> untrackedVideoIds();
    // The user keeps an untracked video: it is not untracked any more.
    void keepVideo(qint64 videoId);
    // Forgets the absent tracks of a video, for when the video is deleted:
    // if their files come back, they are looked up afresh.
    void removeAbsentTracksOf(qint64 videoId);
    QHash<QString, int> trackStateCounts();

    // Videos
    QVector<VideoInfo> allVideos();
    std::optional<VideoInfo> video(qint64 id);
    std::optional<VideoInfo> videoByYtId(const QString &ytId);
    qint64 insertVideo(const VideoInfo &v);
    void updateVideoTags(const VideoInfo &v);
    // After a video's file was rebuilt from better streams.
    void updateVideoMedia(const VideoInfo &v);
    // The videos that wait for review as options for the same tracks.
    QVector<VideoInfo> reviewOptions(qint64 group);
    // Points the tracks of one video at another.
    void relinkTracks(qint64 fromVideoId, qint64 toVideoId);
    // Makes a track look for (more) videos again; what it has stays with it.
    void setTrackPending(qint64 trackId);
    // Makes the track's recording pending for a re-import (TrackInfo::reimport),
    // or ends one. Absent tracks are left alone.
    void setTrackReimport(qint64 trackId);
    void clearTrackReimport(qint64 trackId);
    // The tracks in the music folders at `path`: that file, or every file
    // under that folder.
    QVector<qint64> trackIdsUnder(const QString &path);
    // The tracks in the music folders that have a video and were last looked
    // up before `secs` (Unix time).
    QVector<qint64> trackIdsImportedBefore(qint64 secs);

    // An artist's YouTube channels, by id (or by name, for videos from
    // before ids were kept): from MusicBrainz, or vouched for by the user.
    bool isArtistChannel(const QString &artist, const QString &channelId, const QString &channelName);
    void addArtistChannel(const QString &artist, const QString &channelId, const QString &channelName, const QString &source);
    QVector<QStringList> artistChannels(const QString &artist); // {id, name, source} each
    // Whether MusicBrainz has been asked about the artist lately.
    bool artistChannelsKnown(const QString &artist);
    void setArtistChannelsKnown(const QString &artist, int days);
    // A video the user turned down for a track is not offered for it again.
    void rejectVideoFor(const TrackInfo &track, const QString &ytId);
    bool videoRejectedFor(const TrackInfo &track, const QString &ytId);
    void removeVideo(qint64 id);

private:
    QSqlDatabase conn();
    QString storedPath(const QString &path) const;
    QString resolvedPath(const QString &stored) const;
    VideoInfo resolved(VideoInfo v) const;

    QString m_file;
    QString m_mvDir;
};
