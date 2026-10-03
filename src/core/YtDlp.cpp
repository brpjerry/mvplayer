#include "core/YtDlp.h"

#include "core/Util.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace {

QString findFile(const QString &dir, const QString &base, const QStringList &skipSuffixes)
{
    const QFileInfoList entries = QDir(dir).entryInfoList({base + QStringLiteral(".*")}, QDir::Files);
    for (const QFileInfo &fi : entries) {
        const QString name = fi.fileName();
        bool skip = false;
        for (const QString &s : skipSuffixes)
            skip |= name.endsWith(s);
        if (!skip)
            return fi.absoluteFilePath();
    }
    return {};
}

} // namespace

YtDlp::YtDlp(const QString &program, const QStringList &extraArgs, const std::atomic<bool> *cancel)
    : m_program(program.isEmpty() ? QStringLiteral("yt-dlp") : program)
    , m_extraArgs(extraArgs)
    , m_cancel(cancel)
{
}

QStringList YtDlp::baseArgs() const
{
    QStringList a = {
        QStringLiteral("--ignore-config"), QStringLiteral("--no-playlist"), QStringLiteral("--no-warnings"),
        QStringLiteral("--socket-timeout"), QStringLiteral("30"), QStringLiteral("--retries"), QStringLiteral("5"),
    };
    a += m_extraArgs;
    return a;
}

bool YtDlp::looksBlocked(const QString &error)
{
    static const QStringList signs = {
        QStringLiteral("http error 429"), QStringLiteral("too many requests"),
        QStringLiteral("not a bot"), QStringLiteral("rate-limited"), QStringLiteral("rate limited"),
        QStringLiteral("try again later"),
    };
    const QString e = error.toLower();
    for (const QString &s : signs) {
        if (e.contains(s))
            return true;
    }
    return false;
}

QString YtDlp::url(const QString &id)
{
    return QStringLiteral("https://www.youtube.com/watch?v=") + id;
}

bool YtDlp::search(const QString &query, int count, QVector<YtCandidate> *out, QString *error)
{
    QStringList args = baseArgs();
    args << QStringLiteral("--flat-playlist") << QStringLiteral("-J")
         << QStringLiteral("ytsearch%1:%2").arg(count).arg(query);
    ProcOptions opts;
    opts.cancel = m_cancel;
    opts.timeoutMs = 120000;
    const ProcResult r = runProcess(m_program, args, opts);
    if (!r.ok()) {
        if (error)
            *error = r.errorText();
        return false;
    }
    const QJsonArray entries = QJsonDocument::fromJson(r.out).object().value(QLatin1String("entries")).toArray();
    int rank = 0;
    for (const QJsonValue &v : entries) {
        const QJsonObject e = v.toObject();
        YtCandidate c;
        c.id = e.value(QLatin1String("id")).toString();
        if (c.id.isEmpty() || e.value(QLatin1String("ie_key")).toString() != QLatin1String("Youtube"))
            continue;
        const QString live = e.value(QLatin1String("live_status")).toString();
        if (live == QLatin1String("is_live") || live == QLatin1String("is_upcoming"))
            continue;
        c.title = e.value(QLatin1String("title")).toString();
        c.channel = e.value(QLatin1String("channel")).toString();
        if (c.channel.isEmpty())
            c.channel = e.value(QLatin1String("uploader")).toString();
        c.duration = e.value(QLatin1String("duration")).toDouble();
        c.views = qint64(e.value(QLatin1String("view_count")).toDouble());
        c.verified = e.value(QLatin1String("channel_is_verified")).toBool();
        c.rank = rank++;
        out->append(c);
    }
    return true;
}

bool YtDlp::downloadAudio(const QString &id, const QString &dir, QString *file, QJsonObject *info, QString *error)
{
    QStringList args = baseArgs();
    args << QStringLiteral("-f") << QStringLiteral("ba/b") << QStringLiteral("--no-progress")
         << QStringLiteral("--write-info-json") << QStringLiteral("-o")
         << QDir(dir).filePath(QStringLiteral("audio.%(ext)s")) << url(id);
    ProcOptions opts;
    opts.cancel = m_cancel;
    opts.timeoutMs = 15 * 60 * 1000;
    const ProcResult r = runProcess(m_program, args, opts);
    if (!r.ok()) {
        if (error)
            *error = r.errorText();
        return false;
    }
    QFile jf(QDir(dir).filePath(QStringLiteral("audio.info.json")));
    if (info && jf.open(QIODevice::ReadOnly))
        *info = QJsonDocument::fromJson(jf.readAll()).object();
    *file = findFile(dir, QStringLiteral("audio"), {QStringLiteral(".json"), QStringLiteral(".part")});
    if (file->isEmpty()) {
        if (error)
            *error = QStringLiteral("yt-dlp produced no audio file");
        return false;
    }
    return true;
}

bool YtDlp::downloadPreview(const QString &id, const QString &dir, QString *file, QString *error)
{
    QStringList args = baseArgs();
    args << QStringLiteral("-f") << QStringLiteral("wv*[height>=144]/wv*/w") << QStringLiteral("--no-progress")
         << QStringLiteral("-o") << QDir(dir).filePath(QStringLiteral("preview.%(ext)s")) << url(id);
    ProcOptions opts;
    opts.cancel = m_cancel;
    opts.timeoutMs = 15 * 60 * 1000;
    const ProcResult r = runProcess(m_program, args, opts);
    if (!r.ok()) {
        if (error)
            *error = r.errorText();
        return false;
    }
    *file = findFile(dir, QStringLiteral("preview"), {QStringLiteral(".part")});
    return !file->isEmpty();
}

bool YtDlp::downloadVideo(const QString &id, const QString &dir, const std::function<void(double)> &progress,
                          QString *file, QString *thumb, QString *error)
{
    QStringList args = baseArgs();
    args << QStringLiteral("-f") << QStringLiteral("bv*/b") << QStringLiteral("--write-thumbnail")
         << QStringLiteral("--convert-thumbnails") << QStringLiteral("jpg") << QStringLiteral("--newline")
         << QStringLiteral("--progress-template")
         << QStringLiteral("download:MVP %(progress.downloaded_bytes)s %(progress.total_bytes)s %(progress.total_bytes_estimate)s")
         << QStringLiteral("-o") << QDir(dir).filePath(QStringLiteral("video.%(ext)s")) << url(id);
    ProcOptions opts;
    opts.cancel = m_cancel;
    opts.timeoutMs = 4 * 60 * 60 * 1000;
    opts.onLine = [&](const QByteArray &line) {
        if (!progress || !line.startsWith("MVP "))
            return;
        const QList<QByteArray> parts = line.split(' ');
        if (parts.size() < 4)
            return;
        const double done = parts[1].toDouble();
        double total = parts[2].toDouble();
        if (total <= 0)
            total = parts[3].toDouble();
        if (total > 0)
            progress(qBound(0.0, done / total, 1.0));
    };
    const ProcResult r = runProcess(m_program, args, opts);
    if (!r.ok()) {
        if (error)
            *error = r.errorText();
        return false;
    }
    const QStringList notVideo = {QStringLiteral(".jpg"), QStringLiteral(".webp"), QStringLiteral(".png"),
                                  QStringLiteral(".part"), QStringLiteral(".json"), QStringLiteral(".ytdl")};
    *file = findFile(dir, QStringLiteral("video"), notVideo);
    const QString jpg = QDir(dir).filePath(QStringLiteral("video.jpg"));
    *thumb = QFile::exists(jpg) ? jpg : QString();
    if (file->isEmpty()) {
        if (error)
            *error = QStringLiteral("yt-dlp produced no video file");
        return false;
    }
    return true;
}
