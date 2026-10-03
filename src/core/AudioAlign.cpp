#include "core/AudioAlign.h"

#include "core/Util.h"

#include <chromaprint.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace AudioAlign {

namespace {

constexpr int kFrame = kRate / 10;  // 100 ms consistency frames
constexpr int kFpWindow = 12;       // fingerprint items averaged when matching (~1.5 s)
constexpr double kFpMaxErr = 9.5;   // mean differing bits (of 32) still counted as a match
constexpr int kFpMinRun = 24;       // shortest fingerprint match worth keeping (~3 s)
constexpr double kMinCorr = 0.25;   // weakest correlation peak accepted when refining a lag
constexpr double kFrameCorr = 0.5;  // per-frame correlation needed to call two frames the same audio
constexpr double kSilenceRms = 60;  // 16-bit RMS below which a frame is treated as silent
constexpr int kMaxHole = 5;         // frames of disagreement bridged inside a segment (0.5 s)
constexpr int kMinGood = 20;        // matching frames a segment needs (2 s)

struct Fingerprint {
    std::vector<uint32_t> items;
    int hop = 1365; // samples per item
};

Fingerprint fingerprint(const std::vector<int16_t> &pcm)
{
    Fingerprint fp;
    ChromaprintContext *ctx = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
    if (!ctx)
        return fp;
    chromaprint_start(ctx, kRate, 1);
    const int hop = chromaprint_get_item_duration(ctx);
    if (hop > 0)
        fp.hop = hop;
    // Feed in bounded chunks; the API takes an int sample count.
    size_t pos = 0;
    while (pos < pcm.size()) {
        const size_t n = std::min<size_t>(pcm.size() - pos, 1 << 20);
        chromaprint_feed(ctx, pcm.data() + pos, int(n));
        pos += n;
    }
    chromaprint_finish(ctx);
    uint32_t *raw = nullptr;
    int size = 0;
    if (chromaprint_get_raw_fingerprint(ctx, &raw, &size) && raw) {
        fp.items.assign(raw, raw + size);
        chromaprint_dealloc(raw);
    }
    chromaprint_free(ctx);
    return fp;
}

// Marks fingerprint items that describe silence. Silence fingerprints equal
// each other, which would otherwise "match" at every offset.
std::vector<uint8_t> silentItems(const std::vector<int16_t> &pcm, size_t count, int hop)
{
    std::vector<uint8_t> silent(count, 0);
    const size_t span = size_t(hop) * 4;
    for (size_t i = 0; i < count; ++i) {
        const size_t a = i * hop;
        const size_t b = std::min(pcm.size(), a + span);
        if (a >= b) {
            silent[i] = 1;
            continue;
        }
        double e = 0;
        for (size_t k = a; k < b; ++k)
            e += double(pcm[k]) * pcm[k];
        silent[i] = std::sqrt(e / double(b - a)) < kSilenceRms;
    }
    return silent;
}

struct FpRun {
    int i0 = 0, i1 = 0; // item range on the video fingerprint
    int d = 0;          // diagonal: video item - track item
    double meanErr = 0;
};

// Finds stretches of the video fingerprint that match the track fingerprint
// at a constant offset, then keeps a non-overlapping best set of them.
QVector<FpRun> findRuns(const std::vector<uint32_t> &M, const std::vector<uint8_t> &silentM,
                        const std::vector<uint32_t> &T, const std::vector<uint8_t> &silentT)
{
    const int n = int(M.size()), m = int(T.size());
    const int thrSum = int(kFpMaxErr * kFpWindow);
    QVector<FpRun> all;
    std::vector<uint8_t> e;

    for (int d = -(m - 1); d <= n - 1; ++d) {
        const int iLo = std::max(0, d), iHi = std::min(n, m + d);
        const int len = iHi - iLo;
        if (len < kFpMinRun)
            continue;
        e.resize(len);
        int near = 0;
        for (int k = 0; k < len; ++k) {
            const int i = iLo + k, j = i - d;
            e[k] = (silentM[i] || silentT[j]) ? 16 : uint8_t(std::popcount(M[i] ^ T[j]));
            near += e[k] <= 9;
        }
        if (near < kFpMinRun / 2)
            continue;

        int sum = 0;
        for (int k = 0; k < kFpWindow; ++k)
            sum += e[k];
        int runStart = -1;
        for (int k = 0; k + kFpWindow <= len; ++k) {
            if (k > 0)
                sum += e[k + kFpWindow - 1] - e[k - 1];
            const bool ok = sum <= thrSum;
            if (ok && runStart < 0)
                runStart = k;
            const bool last = k + kFpWindow == len;
            if (runStart >= 0 && (!ok || last)) {
                const int a = runStart, b = (ok ? k : k - 1) + kFpWindow;
                if (b - a >= kFpMinRun) {
                    double mean = 0;
                    for (int x = a; x < b; ++x)
                        mean += e[x];
                    all.append({iLo + a, iLo + b, d, mean / (b - a)});
                }
                runStart = -1;
            }
        }
    }

    std::sort(all.begin(), all.end(), [](const FpRun &a, const FpRun &b) {
        return (a.i1 - a.i0) * (16.0 - a.meanErr) > (b.i1 - b.i0) * (16.0 - b.meanErr);
    });

    // Greedy: best run first; later runs only get what is still unclaimed.
    QVector<FpRun> chosen;
    std::vector<uint8_t> covered(n, 0);
    for (FpRun r : std::as_const(all)) {
        int bestA = 0, bestB = 0, a = -1;
        for (int i = r.i0; i <= r.i1; ++i) {
            const bool isFree = i < r.i1 && !covered[i];
            if (isFree && a < 0)
                a = i;
            if (!isFree && a >= 0) {
                if (i - a > bestB - bestA) {
                    bestA = a;
                    bestB = i;
                }
                a = -1;
            }
        }
        if (bestB - bestA < kFpMinRun)
            continue;
        r.i0 = bestA;
        r.i1 = bestB;
        std::fill(covered.begin() + r.i0, covered.begin() + r.i1, 1);
        chosen.append(r);
    }
    return chosen;
}

inline float dot(const float *a, const float *b, qint64 n)
{
    // Eight independent partial sums let the compiler vectorise the loop.
    float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    qint64 k = 0;
    for (; k + 8 <= n; k += 8) {
        for (int j = 0; j < 8; ++j)
            acc[j] += a[k + j] * b[k + j];
    }
    float s = 0;
    for (int j = 0; j < 8; ++j)
        s += acc[j];
    for (; k < n; ++k)
        s += a[k] * b[k];
    return s;
}

double dotLong(const float *a, const float *b, qint64 n)
{
    double s = 0;
    for (qint64 k = 0; k < n; k += 4096)
        s += dot(a + k, b + k, std::min<qint64>(4096, n - k));
    return s;
}

// Turns a coarse (fingerprint-resolution) lag into a sample-accurate one by
// cross-correlating a stretch of audio from the middle of the run.
bool refineLag(const std::vector<float> &M, const std::vector<float> &T, const std::vector<double> &cumT2,
               qint64 runStart, qint64 runEnd, qint64 lag0, int radius, qint64 *lagOut, double *corrOut)
{
    const qint64 nM = qint64(M.size()), nT = qint64(T.size());
    qint64 wS = std::max<qint64>({runStart, lag0 + radius, 0});
    const qint64 wE = std::min<qint64>({runEnd, nT + lag0 - radius, nM});
    if (wE - wS < kRate)
        return false;
    const qint64 N = std::min<qint64>(wE - wS, 12 * kRate);
    wS += (wE - wS - N) / 2;

    const double m2 = dotLong(&M[wS], &M[wS], N);
    if (m2 <= 0)
        return false;

    double best = -2;
    qint64 bestLag = lag0;
    for (qint64 L = lag0 - radius; L <= lag0 + radius; ++L) {
        const qint64 t0 = wS - L;
        const double t2 = cumT2[t0 + N] - cumT2[t0];
        if (t2 <= 0)
            continue;
        const double c = dotLong(&M[wS], &T[t0], N) / std::sqrt(m2 * t2);
        if (c > best) {
            best = c;
            bestLag = L;
        }
    }
    *lagOut = bestLag;
    *corrOut = best;
    return best >= kMinCorr;
}

enum FrameState : uint8_t { Out, Bad, Neutral, Good };

struct FrameScan {
    std::vector<uint8_t> state;
    std::vector<float> m2, t2; // per-frame energies
};

// Classifies every 100 ms frame of the video against the track at one lag.
FrameScan scanFrames(const std::vector<float> &M, const std::vector<float> &T, qint64 lag)
{
    const qint64 nM = qint64(M.size()), nT = qint64(T.size());
    const qint64 frames = nM / kFrame;
    FrameScan fs;
    fs.state.assign(frames, Out);
    fs.m2.assign(frames, 0);
    fs.t2.assign(frames, 0);
    const double quiet = kSilenceRms * kSilenceRms * kFrame;
    for (qint64 f = 0; f < frames; ++f) {
        const qint64 p = f * kFrame, t = p - lag;
        if (t < 0 || t + kFrame > nT)
            continue;
        const float m2 = dot(&M[p], &M[p], kFrame);
        const float t2 = dot(&T[t], &T[t], kFrame);
        fs.m2[f] = m2;
        fs.t2[f] = t2;
        if (m2 < quiet && t2 < quiet) {
            fs.state[f] = Neutral;
            continue;
        }
        if (m2 <= 0 || t2 <= 0) {
            fs.state[f] = Bad;
            continue;
        }
        const double c = dot(&M[p], &T[t], kFrame) / std::sqrt(double(m2) * t2);
        fs.state[f] = c >= kFrameCorr ? Good : Bad;
    }
    return fs;
}

} // namespace

QString Result::summary() const
{
    QStringList parts;
    parts << QStringLiteral("fingerprint %1s of track %2s / video %3s")
                 .arg(fpMatchedSec, 0, 'f', 1).arg(trackSec, 0, 'f', 1).arg(videoSec, 0, 'f', 1);
    parts << QStringLiteral("waveform %1s in %2 segment(s)").arg(pcmMatchedSec, 0, 'f', 1).arg(segments.size());
    for (const Segment &s : segments) {
        parts << QStringLiteral("[video %1–%2s ← track %3s, corr %4]")
                     .arg(double(s.mvStart) / kRate, 0, 'f', 3)
                     .arg(double(s.mvEnd) / kRate, 0, 'f', 3)
                     .arg(double(s.mvStart - s.lag) / kRate, 0, 'f', 3)
                     .arg(s.corr, 0, 'f', 2);
    }
    if (!segments.isEmpty())
        parts << QStringLiteral("gain %1 dB").arg(20 * std::log10(gain), 0, 'f', 2);
    return parts.join(QStringLiteral(", "));
}

bool decodeMono(const QString &file, std::vector<int16_t> *pcm, const std::atomic<bool> *cancel, QString *error)
{
    const QStringList args = {
        QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"),
        QStringLiteral("-i"), file,
        QStringLiteral("-map"), QStringLiteral("0:a:0"), QStringLiteral("-vn"),
        QStringLiteral("-af"), QStringLiteral("aresample=%1:first_pts=0").arg(kRate),
        QStringLiteral("-ac"), QStringLiteral("1"),
        QStringLiteral("-f"), QStringLiteral("s16le"), QStringLiteral("-"),
    };
    ProcOptions opts;
    opts.cancel = cancel;
    opts.timeoutMs = 10 * 60 * 1000;
    const ProcResult r = runProcess(QStringLiteral("ffmpeg"), args, opts);
    if (!r.ok()) {
        if (error)
            *error = QStringLiteral("ffmpeg: ") + r.errorText();
        return false;
    }
    pcm->resize(size_t(r.out.size()) / 2);
    std::memcpy(pcm->data(), r.out.constData(), pcm->size() * 2);
    if (pcm->empty()) {
        if (error)
            *error = QStringLiteral("no audio decoded");
        return false;
    }
    return true;
}

Result align(const std::vector<int16_t> &track, const std::vector<int16_t> &video)
{
    Result res;
    res.trackSec = double(track.size()) / kRate;
    res.videoSec = double(video.size()) / kRate;

    const Fingerprint fpT = fingerprint(track);
    const Fingerprint fpM = fingerprint(video);
    if (fpT.items.size() < size_t(kFpMinRun) || fpM.items.size() < size_t(kFpMinRun))
        return res;
    const int hop = fpM.hop;

    const QVector<FpRun> runs = findRuns(fpM.items, silentItems(video, fpM.items.size(), hop),
                                         fpT.items, silentItems(track, fpT.items.size(), hop));
    for (const FpRun &r : runs)
        res.fpMatchedSec += double(r.i1 - r.i0) * hop / kRate;
    if (runs.isEmpty())
        return res;

    std::vector<float> M(video.begin(), video.end());
    std::vector<float> T(track.begin(), track.end());
    std::vector<double> cumT2(T.size() + 1, 0.0);
    for (size_t i = 0; i < T.size(); ++i)
        cumT2[i + 1] = cumT2[i] + double(T[i]) * T[i];

    const qint64 nM = qint64(M.size()), nT = qint64(T.size());
    const qint64 frames = nM / kFrame;
    std::vector<uint8_t> claimed(frames, 0);
    double sumM2 = 0, sumT2 = 0;
    QVector<qint64> lagsDone;

    // `runs` is ordered best-first, so stronger matches claim their frames first.
    for (const FpRun &r : runs) {
        const qint64 runStart = qint64(r.i0) * hop;
        const qint64 runEnd = std::min<qint64>(nM, qint64(r.i1) * hop + 3 * hop);
        qint64 lag = 0;
        double corr = 0;
        if (!refineLag(M, T, cumT2, runStart, runEnd, qint64(r.d) * hop, hop * 3 / 2, &lag, &corr))
            continue;
        if (std::any_of(lagsDone.begin(), lagsDone.end(), [&](qint64 l) { return std::llabs(l - lag) <= 2; }))
            continue;
        lagsDone.append(lag);

        const FrameScan fs = scanFrames(M, T, lag);
        const qint64 coreA = runStart / kFrame, coreB = runEnd / kFrame;

        // Maximal stretches of agreeing frames, bridging short disagreements.
        qint64 start = -1, lastOk = -1;
        int bad = 0;
        auto close = [&]() {
            if (start < 0)
                return;
            qint64 a = start, b = lastOk + 1;
            start = -1;
            if (b <= coreA || a >= coreB)
                return;
            // Keep the longest part nobody else has claimed.
            qint64 bestA = 0, bestB = 0, s = -1;
            for (qint64 f = a; f <= b; ++f) {
                const bool isFree = f < b && !claimed[f];
                if (isFree && s < 0)
                    s = f;
                if (!isFree && s >= 0) {
                    if (f - s > bestB - bestA) {
                        bestA = s;
                        bestB = f;
                    }
                    s = -1;
                }
            }
            a = bestA;
            b = bestB;
            int good = 0;
            for (qint64 f = a; f < b; ++f)
                good += fs.state[f] == Good;
            if (good < kMinGood)
                return;
            std::fill(claimed.begin() + a, claimed.begin() + b, 1);
            for (qint64 f = a; f < b; ++f) {
                if (fs.state[f] == Good) {
                    sumM2 += fs.m2[f];
                    sumT2 += fs.t2[f];
                }
            }
            Segment seg;
            seg.mvStart = a * kFrame;
            seg.mvEnd = b * kFrame;
            seg.lag = lag;
            seg.corr = corr;
            // Frames are a coarse grid: when a segment stops within a frame
            // of where the track itself starts or ends, use all of the track.
            const qint64 trackStartOnMv = std::max<qint64>(lag, 0);
            const qint64 trackEndOnMv = std::min<qint64>(nT + lag, nM);
            if (seg.mvStart - trackStartOnMv < kFrame && (a == 0 || !claimed[a - 1]))
                seg.mvStart = trackStartOnMv;
            if (trackEndOnMv - seg.mvEnd < kFrame && (b >= frames || !claimed[b]))
                seg.mvEnd = trackEndOnMv;
            res.segments.append(seg);
        };
        for (qint64 f = 0; f < frames; ++f) {
            const uint8_t st = fs.state[f];
            if (st == Good || st == Neutral) {
                if (start < 0)
                    start = f;
                lastOk = f;
                bad = 0;
            } else if (st == Out) {
                close();
                bad = 0;
            } else if (start >= 0 && ++bad > kMaxHole) {
                close();
                bad = 0;
            }
        }
        close();
    }

    std::sort(res.segments.begin(), res.segments.end(),
              [](const Segment &a, const Segment &b) { return a.mvStart < b.mvStart; });
    for (const Segment &s : std::as_const(res.segments))
        res.pcmMatchedSec += double(s.mvEnd - s.mvStart) / kRate;
    if (sumM2 > 0 && sumT2 > 0)
        res.gain = std::sqrt(sumT2 / sumM2);
    return res;
}

} // namespace AudioAlign
