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
    // A video has one track: every other track that has this one, in the
    // music folders or gone from them, has no video any more and waits for
    // a lookup. Returns the ones in the music folders.
    QVector<TrackInfo> unlinkOthers(qint64 videoId, const TrackInfo &keeper);
    // The same for every track that has the video: it is about to be
    // deleted as one that was never theirs, and they are looked up again.
    void unlinkAll(qint64 videoId);

    // The review queue: a video that waits for the user's verdict has
    // candidates, the tracks whose video it could be, and a track can be a
    // candidate of several videos. A track is a recording here as everywhere:
    // every file of it is the same candidate.
    struct Candidate {
        TrackInfo track;     // the best of the recording's files in the music folders
        int stream = 0;      // which audio stream of the video's file is this track's audio, from 1; 0: none
        double fit = 0;      // how close its waveform is to the upload's (AudioAlign likeness); 0: not measured
        QString audioDetail; // what that stream is, e.g. "FLAC 24/48"
        QVector<AudioSpan> spans; // where that stream is the track's audio, and where the track is in it
    };
    static qint64 trackKey(const TrackInfo &t) { return t.recording > 0 ? t.recording : -t.id; }
    // Several writes of the calling thread as one: nothing another thread
    // writes comes between them.
    class Transaction
    {
    public:
        explicit Transaction(Database *db);
        ~Transaction();
        Transaction(const Transaction &) = delete;
        Transaction &operator=(const Transaction &) = delete;

    private:
        Database *m_db;
        bool m_open = false;
    };
    void addCandidate(qint64 videoId, const TrackInfo &track, int stream, double fit, const QString &audioDetail,
                      const QVector<AudioSpan> &spans = {});
    void removeCandidate(qint64 videoId, const TrackInfo &track);
    // The judge's answer about a video under review for one of its
    // candidates (docs/import-rules-judge.html, X2): keep or not, how sure,
    // and what it made of the upload. One per video and candidate.
    struct Advice {
        qint64 videoId = 0;
        qint64 trackKey = 0;
        QString mode;  // "judge": given during the lookup; "advice": asked from the card
        QString model;
        bool keep = false;
        double probability = 0;
        QString kind, source, version, whole, note;
        qint64 askedAt = 0;
    };
    // A video goes (back) into the review queue, or leaves it, with nothing else changed.
    void setVideoReview(qint64 videoId, bool review);
    void setAdvice(const Advice &a);
    QVector<Advice> advice(qint64 videoId);
    void clearAdvice(qint64 videoId, qint64 trackKey);
    void clearCandidates(qint64 videoId);
    // In the order they became candidates. Tracks that have left the music
    // folders stay on record and are not listed; candidateCount counts them.
    QVector<Candidate> candidates(qint64 videoId);
    int candidateCount(qint64 videoId);
    // The videos under review the track is a candidate of.
    QVector<VideoInfo> candidacies(const TrackInfo &track);
    // Every video under review, oldest first.
    QVector<VideoInfo> videosUnderReview();
    // Keeping the queue in step with the library after the music folders
    // were read: a track that has a video is no candidate (returns the
    // videos it was one of), and a track that waits for review without
    // being anyone's candidate waits for a lookup instead (returns how many).
    QVector<qint64> dropCandidaciesOfTracksWithVideos();
    int requeueTracksWithoutCandidacies();
    // Videos in the library that tracks of several recordings have.
    QVector<qint64> sharedVideoIds();
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
    // What is known of the upload's channel: its id, subscribers, verified mark.
    void updateVideoChannel(const VideoInfo &v);
    // The videos the library has from a channel, by the channel's id.
    QVector<VideoInfo> videosOfChannel(const QString &channelId);
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

    // The music library is wider than the video folders: a track is looked
    // up only while TrackInfo::lookup holds (docs/import-rules.html, A12).
    // An album asked for from the music player ("Find videos for album") is
    // looked up wherever its files are; it is kept by album artist and title.
    struct AlbumKey {
        QString albumArtist;
        QString album;
    };
    static QString albumKey(const QString &albumArtist, const QString &album);
    QVector<AlbumKey> lookupAlbums();
    void addLookupAlbum(const QString &albumArtist, const QString &album);
    void removeLookupAlbum(const QString &albumArtist, const QString &album);
    // The tracks of an album in the music library, in disc and track order.
    QVector<qint64> trackIdsOfAlbum(const QString &albumArtist, const QString &album);
    // The files indexed for the music player that are not looked up (A12).
    QStringList pathsNotLookedUp();

    // The albums of the music library (absent files aside), for the music
    // player's grid: one per album artist and album title.
    struct AlbumSummary {
        QString albumArtist;
        QString album;
        QString artists;     // every artist of its files, "; "-joined
        QString genres;
        int year = 0;        // the earliest year its files carry; 0: none
        int tracks = 0;
        int videos = 0;      // of its tracks, how many have a video
        int lookedUp = 0;    // ...and how many are looked up (TrackInfo::lookup)
        double duration = 0;
        qint64 newest = 0;   // the newest file's mtime
        QString firstPath;   // its first file in disc and track order (the cover's)
    };
    QVector<AlbumSummary> albumSummaries();
    // The files of an album, in disc and track order.
    QVector<TrackInfo> tracksOfAlbum(const QString &albumArtist, const QString &album);
    // Every file of the music library, looked up or not.
    int libraryTrackCount();

    // An artist's YouTube channels, by id (or by name, for videos from
    // before ids were kept): from MusicBrainz, or vouched for by the user.
    bool isArtistChannel(const QString &artist, const QString &channelId, const QString &channelName);
    void addArtistChannel(const QString &artist, const QString &channelId, const QString &channelName, const QString &source);
    QVector<QStringList> artistChannels(const QString &artist); // {id, name, source} each
    // Whether MusicBrainz has been asked about the artist lately.
    bool artistChannelsKnown(const QString &artist);
    void setArtistChannelsKnown(const QString &artist, int days);
    // The channels the user allowed (each an artist's channel for every
    // track, and never ignored as a small one) and the ones the user denied
    // (their uploads are never taken), by id.
    struct ListedChannel {
        QString id;
        QString list;   // "allow" or "deny"
        QString name;   // as it was when listed, for showing
        QString handle; // "@name", where known
        qint64 addedAt = 0;
    };
    QString channelList(const QString &channelId); // "allow", "deny" or empty
    bool channelAllowed(const QString &channelId) { return channelList(channelId) == QLatin1String("allow"); }
    bool channelDenied(const QString &channelId) { return channelList(channelId) == QLatin1String("deny"); }
    void setChannelList(const QString &channelId, const QString &list, const QString &name, const QString &handle);
    void removeChannelList(const QString &channelId);
    QVector<ListedChannel> channelLists();
    // A channel by its id or by its handle ("@name", in any case), as the
    // library knows it from the videos it has of it and from the lists: its
    // id, its own name (not "Name and Other", as an upload listed under
    // several channels carries it) and its handle. An id it has never seen
    // comes back with no name; a handle it has never seen, with no id.
    ListedChannel knownChannel(const QString &idOrHandle);
    // A video the user turned down for a track is not offered for it again.
    void rejectVideoFor(const TrackInfo &track, const QString &ytId);
    bool videoRejectedFor(const TrackInfo &track, const QString &ytId);
    // ...until the verdict is taken back.
    void unrejectVideoFor(const TrackInfo &track, const QString &ytId);
    void removeVideo(qint64 id);

    // Taking a verdict back (ImportManager's undo): rows as they were, and
    // put back as they were.
    struct CandidateRow {
        qint64 trackKey = 0;
        int stream = 0;
        double fit = 0;
        QString audioDetail;
        QVector<AudioSpan> spans;
    };
    QVector<CandidateRow> candidateRows(qint64 videoId);
    void restoreCandidates(qint64 videoId, const QVector<CandidateRow> &rows);
    struct TrackState {
        qint64 id = 0;
        QString state;
        qint64 videoId = 0;
        QString message;
    };
    // The import state of every file of a candidate's recording.
    QVector<TrackState> trackStates(qint64 trackKey);
    void restoreTrackState(const TrackState &t);
    // The video's row as it was: updated where the row still exists,
    // inserted again with its id where it was deleted. False where another
    // video has its YouTube id by now.
    bool restoreVideo(const VideoInfo &v);

private:
    QSqlDatabase conn();
    QString storedPath(const QString &path) const;
    QString resolvedPath(const QString &stored) const;
    VideoInfo resolved(VideoInfo v) const;
    bool upgradeLayout(int from, QString *error);
    void rekeyCandidates(qint64 fromKey, qint64 toKey);

    QString m_file;
    QString m_mvDir;
};
