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
//
// Nor is an upload always the album master: it is commonly quieter, equalised
// differently, of inverted polarity, or a few milliseconds longer or shorter
// over the song. The waveforms are therefore compared in the mid band, by the
// size of their correlation, and at an offset that is followed as it drifts.
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
    double pcmMatchedSec = 0;  // length of the segments
    double goodSec = 0;        // of that, where the two demonstrably are the same audio
    double gain = 1.0;         // track level relative to the video's audio
    bool inverted = false;     // the video's audio has the opposite polarity
    // Where the track sits going by loudness alone: the offset (in samples at
    // kRate, as Segment::lag) at which the two rise and fall together, and
    // how well they do. Coarse, but it cannot lock onto the wrong beat the
    // way a weak waveform match can.
    qint64 contourLag = 0;
    double contourCorr = 0;
    // Set by the importer when the same performance was recognised in another
    // mix: the one segment places the whole track by its offset.
    bool byOffset = false;
    // Where the video stops the song and carries on with it later — a
    // reaction video pausing to talk, a scene cut into a music video: how
    // many seconds of other audio lie inside the song, and in how many
    // places. Time before the song starts or after it ends is not counted.
    double interruptedSec(int *places = nullptr) const;

    QString summary() const;
};

// Decodes the first audio stream of `file` to mono 16-bit PCM at kRate,
// positioned on the container timeline (starts at t=0).
// `stream`: which of the file's audio streams, from 0.
bool decodeMono(const QString &file, std::vector<int16_t> *pcm, const std::atomic<bool> *cancel, QString *error,
                int stream = 0);

// Chromaprint items of decoded audio, about eight per second.
QVector<quint32> fingerprintItems(const std::vector<int16_t> &pcm);

Result align(const std::vector<int16_t> &track, const std::vector<int16_t> &video);

} // namespace AudioAlign
