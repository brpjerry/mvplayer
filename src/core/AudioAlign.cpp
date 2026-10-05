#include "core/AudioAlign.h"

#include "core/Util.h"

#include <chromaprint.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>

namespace AudioAlign {

namespace {

constexpr int kFrame = kRate / 10;  // 100 ms consistency frames
constexpr int kFpWindow = 12;       // fingerprint items averaged when matching (~1.5 s)
constexpr double kFpMaxErr = 9.5;   // mean differing bits (of 32) still counted as a match
constexpr int kFpMinRun = 24;       // shortest fingerprint match worth keeping (~3 s)
constexpr double kMinCorr = 0.25;   // weakest correlation peak accepted when refining a lag
constexpr double kFrameCorr = 0.5;  // per-frame correlation that alone says two frames are the same audio
constexpr double kNearCorr = 0.25;  // ... or this much over the half second around the frame,
constexpr double kOwnCorr = 0.12;   // with the frame itself not plainly unrelated
constexpr int kNear = 2;            // frames either side that make up that half second
constexpr int kTrackBlock = 5;      // frames over which the offset is followed (0.5 s)
constexpr double kTrackCorr = 0.3;  // correlation needed to move the offset by a sample
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
        // An upload with inverted polarity correlates just as well, negatively.
        if (std::abs(c) > std::abs(best) || best < -1.5) {
            best = c;
            bestLag = L;
        }
    }
    *lagOut = bestLag;
    *corrOut = best;
    return std::abs(best) >= kMinCorr && best >= -1.5;
}

// Keeps 150 Hz to 3 kHz, where a remastered or re-equalised copy still has
// the waveform of the original. Both signals get the same filter.
void midBand(std::vector<float> &x)
{
    const auto biquad = [&x](double b0, double b1, double b2, double a1, double a2) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (float &v : x) {
            const double in = v;
            const double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = out;
            v = float(out);
        }
    };
    const double q = 0.7071;
    {
        const double w = 2 * std::numbers::pi * 150.0 / kRate, alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha;
        biquad((1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - alpha) / a0);
    }
    {
        const double w = 2 * std::numbers::pi * 3000.0 / kRate, alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha;
        biquad((1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - alpha) / a0);
    }
}

// Soft: the two do not demonstrably match, but the video is no louder than
// the track there — nothing of its own is going on, the mix just differs.
enum FrameState : uint8_t { Out, Bad, Soft, Neutral, Good };

struct FrameScan {
    std::vector<uint8_t> state;
    std::vector<float> m2, t2; // per-frame energies
    std::vector<qint64> lag;   // the offset each frame was compared at
};

// Classifies every 100 ms frame of the video against the track. The offset
// starts as `lag` at frame `anchor` and is followed from there in both
// directions, a sample at a time: two copies of a recording can differ in
// length by a few milliseconds. `sign` is -1 for an upload of inverted polarity.
FrameScan scanFrames(const std::vector<float> &M, const std::vector<float> &T, qint64 lag, qint64 anchor, int sign)
{
    const qint64 nM = qint64(M.size()), nT = qint64(T.size());
    const qint64 frames = nM / kFrame;
    FrameScan fs;
    fs.state.assign(frames, Out);
    fs.m2.assign(frames, 0);
    fs.t2.assign(frames, 0);
    fs.lag.assign(frames, lag);
    if (frames <= 0)
        return fs;

    const auto follow = [&](qint64 blockStart, qint64 cur) {
        const qint64 p = blockStart * kFrame;
        const qint64 n = std::min<qint64>(kTrackBlock, frames - blockStart) * kFrame;
        double best = -2;
        qint64 bestLag = cur;
        for (qint64 L = cur - 1; L <= cur + 1; ++L) {
            const qint64 t = p - L;
            if (t < 0 || t + n > nT)
                continue;
            const double m2 = dotLong(&M[p], &M[p], n), t2 = dotLong(&T[t], &T[t], n);
            if (m2 <= 0 || t2 <= 0)
                continue;
            const double c = sign * dotLong(&M[p], &T[t], n) / std::sqrt(m2 * t2);
            // Staying put wins a tie.
            if (c > best + (L == cur ? -1e-6 : 1e-6)) {
                best = c;
                bestLag = L;
            }
        }
        return best >= kTrackCorr ? bestLag : cur;
    };
    anchor = std::clamp<qint64>(anchor, 0, frames - 1) / kTrackBlock * kTrackBlock;
    qint64 cur = lag;
    for (qint64 b = anchor; b < frames; b += kTrackBlock) {
        cur = follow(b, cur);
        std::fill(fs.lag.begin() + b, fs.lag.begin() + std::min<qint64>(b + kTrackBlock, frames), cur);
    }
    cur = lag;
    for (qint64 b = anchor - kTrackBlock; b >= 0; b -= kTrackBlock) {
        cur = follow(b, cur);
        std::fill(fs.lag.begin() + b, fs.lag.begin() + b + kTrackBlock, cur);
    }

    const double quiet = kSilenceRms * kSilenceRms * kFrame;
    std::vector<float> d(frames, 0);
    for (qint64 f = 0; f < frames; ++f) {
        const qint64 p = f * kFrame, t = p - fs.lag[f];
        if (t < 0 || t + kFrame > nT)
            continue;
        fs.m2[f] = dot(&M[p], &M[p], kFrame);
        fs.t2[f] = dot(&T[t], &T[t], kFrame);
        d[f] = sign * dot(&M[p], &T[t], kFrame);
        fs.state[f] = Bad;
    }
    double goodM2 = 0, goodT2 = 0;
    for (qint64 f = 0; f < frames; ++f) {
        if (fs.state[f] == Out)
            continue;
        const float m2 = fs.m2[f], t2 = fs.t2[f];
        if (m2 < quiet && t2 < quiet) {
            fs.state[f] = Neutral;
            continue;
        }
        if (m2 <= 0 || t2 <= 0)
            continue;
        const double own = d[f] / std::sqrt(double(m2) * t2);
        double nd = 0, nm = 0, nt = 0;
        for (qint64 g = std::max<qint64>(0, f - kNear); g <= std::min(frames - 1, f + kNear); ++g) {
            if (fs.state[g] == Out)
                continue;
            nd += d[g];
            nm += fs.m2[g];
            nt += fs.t2[g];
        }
        const double near = nm > 0 && nt > 0 ? nd / std::sqrt(nm * nt) : 0;
        if (own >= kFrameCorr || (near >= kNearCorr && own >= kOwnCorr)) {
            fs.state[f] = Good;
            goodM2 += m2;
            goodT2 += t2;
        }
    }
    // Where the video is louder than the track by more than 3 dB it has
    // something of its own to say (an effect, a line of dialogue).
    if (goodM2 > 0 && goodT2 > 0) {
        const double g2 = goodT2 / goodM2;
        for (qint64 f = 0; f < frames; ++f) {
            if (fs.state[f] == Bad && fs.t2[f] > 0 && fs.m2[f] * g2 <= 2.0 * fs.t2[f])
                fs.state[f] = Soft;
        }
    }
    return fs;
}

constexpr int kContourHop = kRate / 100; // 10 ms
constexpr int kContourReach = 30 * 100;  // offsets tried either way: 30 s

// The offset at which the loudness of the two moves together.
void contour(const std::vector<float> &M, const std::vector<float> &T, qint64 *lag, double *corr)
{
    const auto envelope = [](const std::vector<float> &x) {
        std::vector<float> e(x.size() / kContourHop);
        double mean = 0;
        for (size_t i = 0; i < e.size(); ++i) {
            e[i] = std::sqrt(dot(&x[i * kContourHop], &x[i * kContourHop], kContourHop) / kContourHop);
            mean += e[i];
        }
        mean /= std::max<size_t>(e.size(), 1);
        for (float &v : e)
            v -= float(mean);
        return e;
    };
    const std::vector<float> m = envelope(M), t = envelope(T);
    const qint64 nm = qint64(m.size()), nt = qint64(t.size());
    double best = 0;
    qint64 bestLag = 0;
    for (qint64 L = -kContourReach; L <= kContourReach; ++L) {
        // m[i] against t[i - L]
        const qint64 a = std::max<qint64>(0, L), b = std::min(nm, nt + L);
        if (b - a < 20 * 100)
            continue;
        const double d = dotLong(&m[a], &t[a - L], b - a);
        const double e = dotLong(&m[a], &m[a], b - a) * dotLong(&t[a - L], &t[a - L], b - a);
        const double c = e > 0 ? d / std::sqrt(e) : 0;
        if (c > best) {
            best = c;
            bestLag = L;
        }
    }
    *lag = bestLag * kContourHop;
    *corr = best;
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
    if (!segments.isEmpty() && pcmMatchedSec > 0)
        parts << QStringLiteral("%1% of it plainly the same").arg(qRound(100 * goodSec / pcmMatchedSec));
    if (inverted)
        parts << QStringLiteral("polarity inverted");
    if (!segments.isEmpty())
        parts << QStringLiteral("loudness agrees at %1s (%2)").arg(double(contourLag) / kRate, 0, 'f', 2).arg(contourCorr, 0, 'f', 2);
    if (byOffset)
        parts << QStringLiteral("same performance in another mix: placed whole by its offset");
    return parts.join(QStringLiteral(", "));
}

QVector<quint32> fingerprintItems(const std::vector<int16_t> &pcm)
{
    const Fingerprint fp = fingerprint(pcm);
    return QVector<quint32>(fp.items.begin(), fp.items.end());
}

bool decodeMono(const QString &file, std::vector<int16_t> *pcm, const std::atomic<bool> *cancel, QString *error, int stream)
{
    const QStringList args = {
        QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"),
        QStringLiteral("-i"), file,
        QStringLiteral("-map"), QStringLiteral("0:a:%1").arg(stream), QStringLiteral("-vn"),
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
    midBand(M);
    midBand(T);
    contour(M, T, &res.contourLag, &res.contourCorr);
    std::vector<double> cumT2(T.size() + 1, 0.0);
    for (size_t i = 0; i < T.size(); ++i)
        cumT2[i + 1] = cumT2[i] + double(T[i]) * T[i];

    const qint64 nM = qint64(M.size()), nT = qint64(T.size());
    const qint64 frames = nM / kFrame;
    std::vector<uint8_t> claimed(frames, 0);
    double sumM2 = 0, sumT2 = 0;
    QVector<qint64> lagsDone;
    int sign = 0; // polarity of the video's audio against the track's, set by the best run

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
        if (sign == 0) {
            sign = corr < 0 ? -1 : 1;
            res.inverted = sign < 0;
        } else if ((corr < 0) != (sign < 0)) {
            continue;
        }
        corr = std::abs(corr);

        const FrameScan fs = scanFrames(M, T, lag, (runStart + runEnd) / 2 / kFrame, sign);
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
                if (fs.state[f] == Good || fs.state[f] == Neutral)
                    res.goodSec += double(kFrame) / kRate;
            }
            Segment seg;
            seg.mvStart = a * kFrame;
            seg.mvEnd = b * kFrame;
            // The offset as it stands in the middle of the segment.
            const qint64 lag = fs.lag[(a + b) / 2];
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
            } else if (st == Soft) {
                // The track carries on underneath: part of the segment if
                // matching audio follows at this offset, whatever the gap.
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
