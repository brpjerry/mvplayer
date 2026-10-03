#pragma once

#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <vector>

// Works out where a library track sits inside a music video's soundtrack.
//
// A video rarely contains exactly the track: it may open with a delay, cut to
// a scene in the middle, or run on after the song ends. The result is a list
// of segments on the video timeline, each mapping sample-accurately onto the
// track; everything between segments exists only in the video.
namespace AudioAlign {

constexpr int kRate = 11025; // analysis sample rate (mono)

struct Segment {
    qint64 mvStart = 0; // sample range on the video timeline, at kRate
    qint64 mvEnd = 0;
    qint64 lag = 0;     // track position = video position - lag
    double corr = 0;    // waveform correlation at that lag
};

struct Result {
    double trackSec = 0;
    double videoSec = 0;
    double fpMatchedSec = 0;   // video time whose fingerprint matches the track
    QVector<Segment> segments; // regions where the waveforms line up
    double pcmMatchedSec = 0;
    double gain = 1.0;         // track level relative to the video's audio
    QString summary() const;
};

// Decodes the first audio stream of `file` to mono 16-bit PCM at kRate,
// positioned on the container timeline (starts at t=0).
bool decodeMono(const QString &file, std::vector<int16_t> *pcm, const std::atomic<bool> *cancel, QString *error);

Result align(const std::vector<int16_t> &track, const std::vector<int16_t> &video);

} // namespace AudioAlign
