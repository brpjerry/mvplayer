#pragma once

#include "core/AudioAlign.h"
#include "core/Types.h"

#include <atomic>

namespace Muxer {

struct Plan {
    QString videoFile;   // downloaded video stream
    QString ytAudioFile; // downloaded YouTube audio
    QString workDir;     // scratch space; the caller removes it
    QString outFile;     // final .mkv

    TrackInfo track;     // source of tags; track.path is only ever read
    QString ytId;

    // When set, the track's audio replaces the video's wherever
    // `align.segments` says they are the same recording.
    bool replaceAudio = false;
    AudioAlign::Result align;

    // For a video that waits for the user's verdict (needs replaceAudio): a
    // third audio stream, played by default, that alternates every ten
    // seconds between YouTube's audio and the track's, level-matched.
    bool review = false;
};

// Where the track's audio sits in a review stream, in seconds of the video.
struct ReviewSpan {
    double start = 0;
    double end = 0;
};
constexpr int kReviewSeconds = 10; // length of each stretch in the review stream

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

// Once the user has accepted a video: removes the review stream and makes the
// track's audio the default.
bool dropReviewStream(const QString &file, const std::atomic<bool> *cancel, QString *error);

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
