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
bool mux(const Plan &plan, const std::atomic<bool> *cancel, QString *audioDetail, QString *error);

bool probe(const QString &file, Probe *out);

// True when the picture never changes (a still image set to music).
bool isStaticVideo(const QString &file, const std::atomic<bool> *cancel);

} // namespace Muxer
