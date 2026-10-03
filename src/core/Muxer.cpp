#include "core/Muxer.h"

#include "core/Util.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace Muxer {

namespace {

const QString kFfmpeg = QStringLiteral("ffmpeg");

// Read-only memory map of interleaved 32-bit PCM.
struct RawPcm {
    QFile file;
    const int32_t *data = nullptr;
    qint64 frames = 0;
    int channels = 2;

    bool open(const QString &path, int ch)
    {
        channels = ch;
        file.setFileName(path);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        frames = file.size() / (4 * qint64(ch));
        if (frames <= 0)
            return false;
        data = reinterpret_cast<const int32_t *>(file.map(0, frames * 4 * ch));
        return data != nullptr;
    }
    float mono(qint64 f) const
    {
        double s = 0;
        for (int c = 0; c < channels; ++c)
            s += data[f * channels + c];
        return float(s / channels / 65536.0);
    }
};

bool decodeRaw(const QString &in, const QString &out, int rate, int channels, const std::atomic<bool> *cancel,
               QString *error)
{
    const QStringList args = {
        QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
        QStringLiteral("-i"), in,
        QStringLiteral("-map"), QStringLiteral("0:a:0"), QStringLiteral("-vn"),
        QStringLiteral("-af"), QStringLiteral("aresample=%1:first_pts=0").arg(rate),
        QStringLiteral("-ac"), QString::number(channels),
        QStringLiteral("-c:a"), QStringLiteral("pcm_s32le"), QStringLiteral("-f"), QStringLiteral("s32le"), out,
    };
    ProcOptions opts;
    opts.cancel = cancel;
    opts.timeoutMs = 20 * 60 * 1000;
    const ProcResult r = runProcess(kFfmpeg, args, opts);
    if (!r.ok()) {
        if (error)
            *error = QStringLiteral("ffmpeg decode: ") + r.errorText();
        return false;
    }
    return true;
}

struct NativeSegment {
    qint64 start = 0, end = 0, lag = 0;
};

// The analysis ran at 11 kHz; nudge each lag to the exact sample at the
// track's own rate so crossfades stay phase-coherent.
qint64 refineNativeLag(const RawPcm &T, const RawPcm &M, qint64 start, qint64 end, qint64 lag, int rate)
{
    const int radius = rate / AudioAlign::kRate + 3;
    const qint64 N = std::min<qint64>(end - start - 2 * radius, qint64(rate) * 4);
    if (N < rate / 2)
        return lag;
    const qint64 w = start + (end - start - N) / 2;
    if (w - lag - radius < 0 || w - lag + radius + N > T.frames || w + N > M.frames)
        return lag;

    std::vector<float> m(N), t(N + 2 * radius);
    for (qint64 i = 0; i < N; ++i)
        m[i] = M.mono(w + i);
    const qint64 tBase = w - lag - radius;
    for (qint64 i = 0; i < qint64(t.size()); ++i)
        t[i] = T.mono(tBase + i);

    double best = -1e300;
    qint64 bestLag = lag;
    for (int s = -radius; s <= radius; ++s) {
        // Candidate lag = lag + s  =>  track index = w + i - lag - s.
        const float *tp = t.data() + (radius - s);
        double dot = 0, t2 = 0;
        for (qint64 i = 0; i < N; ++i) {
            dot += double(m[i]) * tp[i];
            t2 += double(tp[i]) * tp[i];
        }
        const double c = t2 > 0 ? dot / std::sqrt(t2) : -1e300;
        if (c > best) {
            best = c;
            bestLag = lag + s;
        }
    }
    return bestLag;
}

inline int32_t saturate(double v)
{
    return int32_t(std::clamp(v, -2147483648.0, 2147483647.0));
}

// Builds the new soundtrack on the video timeline: the track's samples inside
// the segments (copied bit-for-bit), the video's own audio everywhere else,
// with a short crossfade at each join.
bool compose(const RawPcm &T, const RawPcm &M, const QVector<NativeSegment> &segs, double gain, int rate,
             const QString &outPath, const std::atomic<bool> *cancel, QString *error)
{
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("cannot write %1").arg(outPath);
        return false;
    }
    const int ch = M.channels;
    const qint64 fade = std::max<qint64>(8, rate / 50); // 20 ms
    const qint64 half = fade / 2;
    const bool applyGain = std::abs(20 * std::log10(gain)) > 0.3;
    const qint64 block = 1 << 15;
    std::vector<int32_t> buf(size_t(block) * ch);

    for (qint64 p0 = 0; p0 < M.frames; p0 += block) {
        if (cancel && cancel->load())
            return false;
        const qint64 n = std::min(block, M.frames - p0);
        const int32_t *src = M.data + p0 * ch;
        if (applyGain) {
            for (qint64 i = 0; i < n * ch; ++i)
                buf[i] = saturate(double(src[i]) * gain);
        } else {
            std::memcpy(buf.data(), src, size_t(n) * ch * 4);
        }

        for (const NativeSegment &s : segs) {
            // A segment that runs to the edge of the video needs no fade there.
            const qint64 fadeInA = s.start <= 0 ? -1 : s.start - half;
            const qint64 fadeOutB = s.end >= M.frames ? M.frames + fade : s.end + half;
            const qint64 a = std::max(p0, std::max<qint64>(fadeInA, 0));
            const qint64 b = std::min(p0 + n, std::min(fadeOutB, M.frames));
            for (qint64 p = a; p < b; ++p) {
                const qint64 t = p - s.lag;
                if (t < 0 || t >= T.frames)
                    continue;
                double w = 1.0;
                if (s.start > 0)
                    w = std::min(w, double(p - fadeInA) / fade);
                if (s.end < M.frames)
                    w = std::min(w, double(fadeOutB - p) / fade);
                w = std::clamp(w, 0.0, 1.0);
                int32_t *o = buf.data() + (p - p0) * ch;
                const int32_t *ts = T.data + t * ch;
                if (w >= 1.0) {
                    std::memcpy(o, ts, size_t(ch) * 4);
                } else {
                    for (int c = 0; c < ch; ++c)
                        o[c] = saturate(std::round(double(o[c]) * (1.0 - w) + double(ts[c]) * w));
                }
            }
        }
        if (out.write(reinterpret_cast<const char *>(buf.data()), n * ch * 4) != n * ch * 4) {
            if (error)
                *error = QStringLiteral("short write composing audio");
            return false;
        }
    }
    return true;
}

void addTag(QStringList &args, const QString &key, const QString &value)
{
    if (!value.isEmpty())
        args << QStringLiteral("-metadata") << key + QLatin1Char('=') + value;
}

void addTags(QStringList &args, const Plan &plan)
{
    const TrackInfo &t = plan.track;
    addTag(args, QStringLiteral("title"), t.title);
    addTag(args, QStringLiteral("artist"), t.artist);
    addTag(args, QStringLiteral("album_artist"), t.albumArtist);
    addTag(args, QStringLiteral("album"), t.album);
    addTag(args, QStringLiteral("genre"), t.genre);
    if (t.year > 0)
        addTag(args, QStringLiteral("date"), QString::number(t.year));
    if (t.trackNo > 0)
        addTag(args, QStringLiteral("track"), QString::number(t.trackNo));
    addTag(args, QStringLiteral("comment"), QStringLiteral("https://www.youtube.com/watch?v=") + plan.ytId);
}

} // namespace

bool mux(const Plan &plan, const std::atomic<bool> *cancel, QString *audioDetail, QString *error)
{
    QDir().mkpath(QFileInfo(plan.outFile).absolutePath());
    const QString partFile = plan.outFile + QStringLiteral(".part.mkv");
    ProcOptions opts;
    opts.cancel = cancel;
    opts.timeoutMs = 60 * 60 * 1000;

    QStringList args = {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-y")};

    if (plan.replaceAudio && !plan.align.segments.isEmpty()) {
        const TrackInfo &t = plan.track;
        const int rate = t.codec == QLatin1String("dsd") ? 88200 : std::clamp(t.sampleRate, 8000, 192000);
        const int ch = std::clamp(t.channels, 1, 8);
        const QString trackRaw = QDir(plan.workDir).filePath(QStringLiteral("track.raw"));
        const QString mvRaw = QDir(plan.workDir).filePath(QStringLiteral("mv.raw"));
        const QString outRaw = QDir(plan.workDir).filePath(QStringLiteral("mix.raw"));

        if (!decodeRaw(t.path, trackRaw, rate, ch, cancel, error)
            || !decodeRaw(plan.ytAudioFile, mvRaw, rate, ch, cancel, error))
            return false;
        {
            RawPcm T, M;
            if (!T.open(trackRaw, ch) || !M.open(mvRaw, ch)) {
                if (error)
                    *error = QStringLiteral("cannot map decoded audio");
                return false;
            }
            const double k = double(rate) / AudioAlign::kRate;
            QVector<NativeSegment> segs;
            for (const AudioAlign::Segment &s : plan.align.segments) {
                NativeSegment n;
                n.start = std::clamp<qint64>(std::llround(s.mvStart * k), 0, M.frames);
                n.end = std::clamp<qint64>(std::llround(s.mvEnd * k), 0, M.frames);
                n.lag = refineNativeLag(T, M, n.start, n.end, std::llround(s.lag * k), rate);
                // Never read outside the track.
                n.start = std::max(n.start, n.lag);
                n.end = std::min(n.end, T.frames + n.lag);
                if (n.end - n.start > rate / 2)
                    segs.append(n);
            }
            if (segs.isEmpty()) {
                if (error)
                    *error = QStringLiteral("no usable segments after refinement");
                return false;
            }
            const double gain = std::clamp(plan.align.gain, 0.25, 4.0);
            if (!compose(T, M, segs, gain, rate, outRaw, cancel, error)) {
                if (error && error->isEmpty())
                    *error = QStringLiteral("cancelled");
                return false;
            }
        }
        QFile::remove(trackRaw);
        QFile::remove(mvRaw);

        const bool deep = !t.lossless || t.bitsPerSample > 16;
        args << QStringLiteral("-i") << plan.videoFile
             << QStringLiteral("-f") << QStringLiteral("s32le") << QStringLiteral("-ar") << QString::number(rate)
             << QStringLiteral("-ac") << QString::number(ch) << QStringLiteral("-i") << outRaw
             << QStringLiteral("-i") << plan.ytAudioFile
             << QStringLiteral("-map") << QStringLiteral("0:v:0") << QStringLiteral("-map") << QStringLiteral("1:a:0")
             << QStringLiteral("-map") << QStringLiteral("2:a:0")
             << QStringLiteral("-c:v") << QStringLiteral("copy")
             << QStringLiteral("-c:a:0") << QStringLiteral("flac")
             << QStringLiteral("-sample_fmt:a:0") << (deep ? QStringLiteral("s32") : QStringLiteral("s16"));
        if (deep)
            args << QStringLiteral("-bits_per_raw_sample:a:0") << QStringLiteral("24");
        args << QStringLiteral("-c:a:1") << QStringLiteral("copy")
             << QStringLiteral("-disposition:a:0") << QStringLiteral("default")
             << QStringLiteral("-disposition:a:1") << QStringLiteral("0")
             << QStringLiteral("-metadata:s:a:0") << QStringLiteral("title=Library audio")
             << QStringLiteral("-metadata:s:a:1") << QStringLiteral("title=YouTube audio");
        if (audioDetail) {
            const QString khz = QString::number(rate / 1000.0, 'g', 4);
            *audioDetail = QStringLiteral("FLAC %1/%2").arg(deep ? 24 : 16).arg(khz);
        }
    } else {
        args << QStringLiteral("-i") << plan.videoFile << QStringLiteral("-i") << plan.ytAudioFile
             << QStringLiteral("-map") << QStringLiteral("0:v:0") << QStringLiteral("-map") << QStringLiteral("1:a:0")
             << QStringLiteral("-c") << QStringLiteral("copy")
             << QStringLiteral("-disposition:a:0") << QStringLiteral("default")
             << QStringLiteral("-metadata:s:a:0") << QStringLiteral("title=YouTube audio");
        if (audioDetail)
            *audioDetail = QStringLiteral("YouTube");
    }
    addTags(args, plan);
    args << QStringLiteral("-f") << QStringLiteral("matroska") << partFile;

    const ProcResult r = runProcess(kFfmpeg, args, opts);
    if (!r.ok()) {
        QFile::remove(partFile);
        if (error)
            *error = QStringLiteral("ffmpeg mux: ") + r.errorText();
        return false;
    }
    QFile::remove(plan.outFile);
    if (!QFile::rename(partFile, plan.outFile)) {
        QFile::remove(partFile);
        if (error)
            *error = QStringLiteral("cannot move video into the library");
        return false;
    }
    return true;
}

bool probe(const QString &file, Probe *out)
{
    const QStringList args = {
        QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"), QStringLiteral("stream=codec_name,width,height,avg_frame_rate:format=duration"),
        QStringLiteral("-of"), QStringLiteral("json"), file,
    };
    ProcOptions opts;
    opts.timeoutMs = 60000;
    const ProcResult r = runProcess(QStringLiteral("ffprobe"), args, opts);
    if (!r.ok())
        return false;
    const QJsonObject root = QJsonDocument::fromJson(r.out).object();
    const QJsonObject s = root.value(QLatin1String("streams")).toArray().at(0).toObject();
    out->width = s.value(QLatin1String("width")).toInt();
    out->height = s.value(QLatin1String("height")).toInt();
    out->vcodec = s.value(QLatin1String("codec_name")).toString();
    const QStringList fr = s.value(QLatin1String("avg_frame_rate")).toString().split(QLatin1Char('/'));
    if (fr.size() == 2 && fr[1].toDouble() > 0)
        out->fps = fr[0].toDouble() / fr[1].toDouble();
    out->duration = root.value(QLatin1String("format")).toObject().value(QLatin1String("duration")).toString().toDouble();
    return out->width > 0;
}

StillCheck checkStill(const QString &file, bool keyframesOnly, const std::atomic<bool> *cancel)
{
    // Thresholds come from measuring real uploads at YouTube's lowest
    // quality: a still picture is not perfectly still there (the encoder
    // shimmers by a level or two at every segment boundary), but almost no
    // pixel moves by much. Real videos change a large share of their pixels
    // between nearly every pair of samples.
    constexpr int w = 128, h = 72;
    constexpr int pixelDelta = 16;        // grey levels a pixel must move to count as changed
    constexpr double changedShare = 0.004; // share of pixels changed for a pair to count as moving
    constexpr double movingShare = 0.10;   // share of moving pairs below which the video is a still

    StillCheck res;
    QStringList args = {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin")};
    // Decoding only keyframes makes a 4K AV1 file cost under a second. Not
    // every decoder honours it (VP9 does not); those files are decoded in
    // full, which the sample spacing below makes equivalent, just slower.
    if (keyframesOnly)
        args << QStringLiteral("-skip_frame") << QStringLiteral("nokey");
    // Keep frames at least two seconds apart, however many the decoder hands over.
    args << QStringLiteral("-i") << file << QStringLiteral("-an") << QStringLiteral("-vf")
         << QStringLiteral("select='isnan(prev_selected_t)+gte(t-prev_selected_t\\,2)',scale=%1:%2,format=gray").arg(w).arg(h)
         << QStringLiteral("-fps_mode") << QStringLiteral("passthrough")
         << QStringLiteral("-f") << QStringLiteral("rawvideo") << QStringLiteral("-");

    ProcOptions opts;
    opts.cancel = cancel;
    opts.timeoutMs = 5 * 60 * 1000;
    const ProcResult r = runProcess(kFfmpeg, args, opts);
    if (!r.ok())
        return res; // cannot tell

    const int frameSize = w * h;
    const int frames = int(r.out.size() / frameSize);
    if (frames < 6) {
        // Too few keyframes to judge by (long keyframe intervals): look at
        // every frame instead.
        return keyframesOnly ? checkStill(file, false, cancel) : res;
    }
    const auto *d = reinterpret_cast<const uchar *>(r.out.constData());
    int moving = 0;
    for (int f = 1; f < frames; ++f) {
        const uchar *a = d + qint64(f - 1) * frameSize;
        const uchar *b = d + qint64(f) * frameSize;
        int changed = 0;
        for (int i = 0; i < frameSize; ++i)
            changed += std::abs(int(a[i]) - int(b[i])) > pixelDelta;
        moving += changed >= frameSize * changedShare;
    }
    res.valid = true;
    res.samples = frames;
    res.movingShare = double(moving) / (frames - 1);
    res.still = res.movingShare < movingShare;
    return res;
}

bool isStaticVideo(const QString &file, const std::atomic<bool> *cancel)
{
    const StillCheck c = checkStill(file, false, cancel);
    return c.valid && c.still;
}

} // namespace Muxer
