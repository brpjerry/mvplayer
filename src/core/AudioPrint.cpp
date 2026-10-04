#include "core/AudioPrint.h"

#include "core/AudioAlign.h"

#include <QtAlgorithms>

#include <chromaprint.h>

#include <algorithm>

namespace AudioPrint {

namespace {

// Measured on a library of 433 tracks: lossy copies of a file differ in up to
// 3% of bits and the same master on another release in up to 7%, while a
// song's instrumental starts at 19% and other takes of it at 22%.
constexpr double kSameRecording = 0.12;
constexpr int kMaxShift = 24;      // items the two may be offset by (3 s)
constexpr int kMaxLengthDiff = 40; // items their lengths may differ by (5 s)
constexpr int kMinOverlap = 16;   // items; fewer in common say nothing (2 s)
constexpr int kProbe = 256;        // items compared first to rule a pair out cheaply
constexpr double kProbeReject = 0.30;

// Bit error rate of a[from, to) against b shifted by `shift` items.
double errorRate(const Print &a, const Print &b, int from, int to, int shift)
{
    const int lo = std::max(from, -shift);
    const int hi = std::min(to, int(b.size()) - shift);
    // Ten seconds in common, or most of a track shorter than that.
    const int enough = std::clamp(int(std::min(a.size(), b.size())) * 3 / 4, kMinOverlap, 80);
    if (hi - lo < enough)
        return 1.0;
    quint64 bits = 0;
    for (int i = lo; i < hi; ++i)
        bits += qPopulationCount(a[i] ^ b[i + shift]);
    return double(bits) / (32.0 * (hi - lo));
}

double bestErrorRate(const Print &a, const Print &b, int from, int to)
{
    double best = 1.0;
    for (int shift = -kMaxShift; shift <= kMaxShift; ++shift)
        best = std::min(best, errorRate(a, b, from, to, shift));
    return best;
}

} // namespace

bool ofFile(const QString &file, Print *print, const std::atomic<bool> *cancel, QString *error)
{
    std::vector<int16_t> pcm;
    if (!AudioAlign::decodeMono(file, &pcm, cancel, error))
        return false;
    *print = AudioAlign::fingerprintItems(pcm);
    if (print->isEmpty() && error)
        *error = QStringLiteral("too short to fingerprint");
    return !print->isEmpty();
}

double distance(const Print &a, const Print &b)
{
    if (a.isEmpty() || b.isEmpty())
        return 1.0;
    // Most pairs are unrelated; a stretch from the middle shows that.
    if (a.size() > 2 * kProbe) {
        const int mid = int(a.size()) / 2;
        const double probe = bestErrorRate(a, b, mid - kProbe / 2, mid + kProbe / 2);
        if (probe > kProbeReject)
            return probe;
    }
    return bestErrorRate(a, b, 0, int(a.size()));
}

bool sameRecording(const Print &a, const Print &b)
{
    if (std::abs(a.size() - b.size()) > kMaxLengthDiff)
        return false;
    return distance(a, b) < kSameRecording;
}

QByteArray pack(const Print &print)
{
    char *encoded = nullptr;
    int size = 0;
    if (print.isEmpty()
        || !chromaprint_encode_fingerprint(print.constData(), int(print.size()), CHROMAPRINT_ALGORITHM_DEFAULT,
                                           &encoded, &size, 0)
        || !encoded)
        return {};
    const QByteArray out(encoded, size);
    chromaprint_dealloc(encoded);
    return out;
}

Print unpack(const QByteArray &packed)
{
    uint32_t *items = nullptr;
    int size = 0, algorithm = 0;
    if (packed.isEmpty()
        || !chromaprint_decode_fingerprint(packed.constData(), int(packed.size()), &items, &size, &algorithm, 0)
        || !items)
        return {};
    const Print out(items, items + size);
    chromaprint_dealloc(items);
    return out;
}

} // namespace AudioPrint
