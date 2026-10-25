#pragma once

#include "core/AudioAlign.h"
#include "core/Types.h"

#include <atomic>

namespace Muxer {

struct Plan {
    QString videoFile;   // downloaded video stream
    QString ytAudioFile; // downloaded YouTube audio
    int ytAudioStream = 0; // which audio stream of that file it is (a file already in the library has several)
    QString workDir;     // scratch space; the caller removes it
    QString outFile;     // final .mkv

    TrackInfo track;     // source of tags; track.path is only ever read
    QString ytId;

    // When set, the track's audio replaces the video's wherever
    // `align.segments` says they are the same recording.
    bool replaceAudio = false;
    AudioAlign::Result align;
};

// Where the track's audio sits in the library stream, in seconds of the
// video: outside it that stream is the video's own audio.
struct ReviewSpan {
    double start = 0;
    double end = 0;
    // Each stretch, and where the track is in it (kept with the video, so
    // that the player can go from the track into its video mid-song).
    QVector<AudioSpan> spans;
};

struct Probe {
    int width = 0;
    int height = 0;
    double fps = 0;
    double duration = 0;
    QString vcodec;
};

// Writes plan.outFile. On success `audioDetail` describes the main audio
// stream (e.g. "FLAC 24/48").
bool mux(const Plan &plan, const std::atomic<bool> *cancel, QString *audioDetail, QString *error,
         ReviewSpan *reviewSpan = nullptr);

// A video under review gains one more candidate's audio: the track of
// `plan`, placed by plan.align, as a stream after the library streams the
// file has and ahead of YouTube's. plan.outFile is the file (its picture and
// its other audio are copied); plan.ytAudioStream says which of its audio
// streams is YouTube's, and the new stream takes that place.
bool addLibraryStream(const Plan &plan, const std::atomic<bool> *cancel, QString *audioDetail, QString *error,
                      ReviewSpan *reviewSpan = nullptr);

// Once the user has accepted a video for one of its candidates: the file
// keeps that track's audio (stream `libraryStream`, from 0; -1 where the
// track has none in it) and YouTube's (`ytStream`), in that order, and
// nothing else; it is tagged as `track`. Also for a video whose track has
// changed and whose audio is YouTube's again. A file that is like that
// already is left alone.
bool keepAudio(const QString &file, int libraryStream, int ytStream, const TrackInfo &track, const QString &ytId,
               const std::atomic<bool> *cancel, QString *error);

bool probe(const QString &file, Probe *out);

struct StillCheck {
    bool valid = false;     // false: the file could not be analysed
    bool still = false;     // the picture is a still image (or a handful of them)
    double movingShare = 0; // share of sampled frame pairs with real change
    int samples = 0;
};

// Decides whether a video's picture is a still image set to music.
// `keyframesOnly` asks the decoder for keyframes alone, which is much faster
// on full-quality files where the codec supports it.
StillCheck checkStill(const QString &file, bool keyframesOnly, const std::atomic<bool> *cancel);

// True when the picture never changes. Unanalysable files count as moving.
bool isStaticVideo(const QString &file, const std::atomic<bool> *cancel);

} // namespace Muxer
