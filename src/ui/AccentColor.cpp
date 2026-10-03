#include "ui/AccentColor.h"

#include <algorithm>
#include <cmath>

namespace AccentColor {

namespace {

constexpr int kBins = 36;
constexpr double kTargetLuminance = 0.33; // relative luminance of every accent

double linear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor &c)
{
    return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
}

} // namespace

bool dominantHue(const QByteArray &rgba, double *hue, double *saturation)
{
    const int pixels = int(rgba.size() / 4);
    if (pixels == 0)
        return false;
    const auto *p = reinterpret_cast<const uchar *>(rgba.constData());

    // Hue histogram. Colourful, bright pixels count most; near-grey ones not at all.
    double weight[kBins] = {};
    double satSum[kBins] = {};
    double total = 0;
    for (int i = 0; i < pixels; ++i, p += 4) {
        float h, s, v;
        QColor(p[0], p[1], p[2]).getHsvF(&h, &s, &v);
        if (h < 0 || s < 0.18f || v < 0.2f)
            continue;
        const double w = double(s) * s * v;
        const int bin = std::min(kBins - 1, int(h * kBins));
        weight[bin] += w;
        satSum[bin] += w * s;
        total += w;
    }
    // Too little colour to call: a few stray pixels should not pick the accent.
    if (total < pixels * 0.012)
        return false;

    int best = 0;
    double bestScore = -1;
    for (int i = 0; i < kBins; ++i) {
        const double score = weight[i] + 0.6 * (weight[(i + 1) % kBins] + weight[(i + kBins - 1) % kBins]);
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }

    // Weighted circular mean over the winning bin and its neighbours.
    double x = 0, y = 0, w = 0, s = 0;
    for (int d = -2; d <= 2; ++d) {
        const int i = (best + d + kBins) % kBins;
        const double angle = (i + 0.5) / kBins * 2 * M_PI;
        x += weight[i] * std::cos(angle);
        y += weight[i] * std::sin(angle);
        w += weight[i];
        s += satSum[i];
    }
    double h = std::atan2(y, x) / (2 * M_PI);
    if (h < 0)
        h += 1;
    *hue = h;
    *saturation = w > 0 ? s / w : 0;
    return true;
}

QColor fromHue(double hue, double saturation)
{
    hue -= std::floor(hue);
    // Keep it clearly coloured but never neon.
    const double s = std::clamp(0.4 + saturation * 0.6, 0.55, 0.9);
    // HSL lightness does not track perceived brightness across hues (yellow
    // is far brighter than blue), so solve for a fixed luminance instead.
    double lo = 0.2, hi = 0.95;
    for (int i = 0; i < 18; ++i) {
        const double mid = (lo + hi) / 2;
        if (luminance(QColor::fromHslF(float(hue), float(s), float(mid))) < kTargetLuminance)
            lo = mid;
        else
            hi = mid;
    }
    return QColor::fromHslF(float(hue), float(s), float((lo + hi) / 2));
}

QColor fromFrame(const QByteArray &rgba)
{
    double hue = 0, saturation = 0;
    if (dominantHue(rgba, &hue, &saturation))
        return fromHue(hue, saturation);

    const int pixels = int(rgba.size() / 4);
    const auto *p = reinterpret_cast<const uchar *>(rgba.constData());
    double sum = 0;
    for (int i = 0; i < pixels; ++i, p += 4)
        sum += std::max({p[0], p[1], p[2]});
    const double brightness = pixels > 0 ? sum / (pixels * 255.0) : 0;
    const float level = float(0.76 + 0.24 * brightness);
    return QColor::fromRgbF(level, level, level);
}

} // namespace AccentColor
