#include "core/TalkCheck.h"

#include "core/AudioAlign.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace {

constexpr int kWindow = 352;  // 32 ms
constexpr int kFft = 512;
constexpr int kHop = 110;     // 10 ms
constexpr int kMaxSeconds = 240;
constexpr int kBeatFrames = 6000; // the first minute

void fft(std::vector<std::complex<float>> &a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * float(M_PI) / float(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

} // namespace

namespace TalkCheck {

Result measure(const std::vector<int16_t> &pcm)
{
    Result r;
    const size_t usable = std::min<size_t>(pcm.size(), size_t(kMaxSeconds) * AudioAlign::kRate);
    if (usable < size_t(5) * AudioAlign::kRate)
        return r;
    const size_t frames = (usable - kWindow) / kHop;

    std::vector<float> window(kWindow);
    for (int i = 0; i < kWindow; ++i)
        window[i] = 0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / (kWindow - 1));

    // Loudness of every frame, and for the first minute how much louder
    // each part of the spectrum got since the frame before (its "flux").
    std::vector<float> energy(frames);
    std::vector<float> flux;
    std::vector<float> prev(kFft / 2 + 1, 0.0f), mag(kFft / 2 + 1);
    std::vector<std::complex<float>> buf(kFft);
    double energySum = 0;
    for (size_t f = 0; f < frames; ++f) {
        const int16_t *x = pcm.data() + f * kHop;
        double sq = 0;
        for (int i = 0; i < kWindow; ++i) {
            const float v = x[i] / 32768.0f * window[i];
            sq += double(v) * v;
            buf[i] = v;
        }
        energy[f] = float(std::sqrt(sq / kWindow)) + 1e-9f;
        energySum += energy[f];
        if (f > size_t(kBeatFrames))
            continue;
        std::fill(buf.begin() + kWindow, buf.end(), std::complex<float>(0));
        fft(buf);
        float rise = 0;
        for (int k = 0; k <= kFft / 2; ++k) {
            mag[k] = std::log1p(100.0f * std::abs(buf[k]));
            if (f > 0)
                rise += std::max(0.0f, mag[k] - prev[k]);
        }
        prev.swap(mag);
        if (f > 0)
            flux.push_back(rise);
    }

    const float half = float(0.5 * energySum / double(frames));
    r.pauses = double(std::count_if(energy.begin(), energy.end(), [half](float e) { return e < half; })) / double(frames);

    // The strongest self-similarity of the flux at a lag of 0.3 to 1.2 s.
    double mean = 0;
    for (float v : flux)
        mean += v;
    mean /= double(std::max<size_t>(1, flux.size()));
    for (float &v : flux)
        v -= float(mean);
    double zero = 1e-9;
    for (float v : flux)
        zero += double(v) * v;
    for (size_t lag = 30; lag < 120 && lag < flux.size(); ++lag) {
        double sum = 0;
        for (size_t i = 0; i + lag < flux.size(); ++i)
            sum += double(flux[i]) * flux[i + lag];
        r.beat = std::max(r.beat, sum / zero);
    }
    r.valid = true;
    r.talk = r.pauses >= 0.40 && r.beat <= 0.26;
    return r;
}

} // namespace TalkCheck
