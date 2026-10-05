#include "core/YtDlp.h"

#include "core/Subtitles.h"

#include "core/Util.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QSaveFile>
#include <QFileInfo>
#include <QDateTime>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryFile>

#include <algorithm>
#include <memory>

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

// YouTube now and then serves a download at a trickle: a video then takes
// half an hour and holds up everything behind it. Watches what arrives in
// `dir` and says when to give up: less than 3 MB in each of two minutes
// running. Asked again, with a fresh look at the page, it usually comes fast.
std::function<bool()> stallWatch(const QString &dir)
{
    struct State {
        QElapsedTimer clock;
        qint64 lastBytes = 0;
        qint64 lastAt = 0;
        int slow = 0;
    };
    auto st = std::make_shared<State>();
    st->clock.start();
    return [st, dir] {
        const qint64 now = st->clock.elapsed();
        // (The interval can be shortened for tests.)
        static const qint64 interval = qEnvironmentVariableIsSet("MVPLAYER_STALL_SECS")
            ? qMax(1, qEnvironmentVariableIntValue("MVPLAYER_STALL_SECS")) * 1000 : 60000;
        if (now - st->lastAt < interval)
            return false;
        qint64 bytes = 0;
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            bytes += it.nextFileInfo().size();
        st->slow = bytes - st->lastBytes < 3 * 1024 * 1024 ? st->slow + 1 : 0;
        st->lastBytes = bytes;
        st->lastAt = now;
        return st->slow >= 2;
    };
}

} // namespace

YtDlp::YtDlp(const QString &program, const QStringList &extraArgs, const QString &cookiesFile,
             const std::atomic<bool> *cancel)
    : m_program(program.isEmpty() ? QStringLiteral("yt-dlp") : program)
    , m_extraArgs(extraArgs)
    , m_cookiesFile(looksLikeCookies(cookiesFile) ? cookiesFile : QString())
    , m_cancel(cancel)
{
}

YtDlp::~YtDlp() = default;

bool YtDlp::looksLikeCookies(const QString &file)
{
    QFile f(file);
    if (file.isEmpty() || !f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024)
        return false;
    bool youtube = false;
    while (!f.atEnd()) {
        // domain, subdomains, path, secure, expiry, name, value
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() || (line.startsWith('#') && !line.startsWith("#HttpOnly_")))
            continue;
        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() != 7)
            return false;
        youtube |= fields[0].endsWith("youtube.com");
    }
    return youtube;
}

QStringList YtDlp::accountArgs()
{
    if (m_cookiesFile.isEmpty())
        return {};
    if (!m_cookiesCopy) {
        auto copy = std::make_unique<QTemporaryFile>(QDir::temp().filePath(QStringLiteral("mvplayer-XXXXXX.txt")));
        QFile source(m_cookiesFile);
        if (!copy->open() || !source.open(QIODevice::ReadOnly) || copy->write(source.readAll()) < 0)
            return {};
        copy->close(); // the temporary file is readable by its owner only
        m_cookiesCopy = std::move(copy);
    }
    // The YouTube Music client is the one that is offered the high-bitrate audio.
    return {QStringLiteral("--cookies"), QDir::toNativeSeparators(m_cookiesCopy->fileName()),
            QStringLiteral("--extractor-args"), QStringLiteral("youtube:player_client=default,web_music")};
}

ProcResult YtDlp::run(const QStringList &args, const ProcOptions &opts)
{
    ProcResult r = runProcess(m_program, baseArgs() + args, opts);
    if (r.ok() || m_cookiesFile.isEmpty() || (m_cancel && m_cancel->load()))
        return r;
    static const QStringList signs = {
        QStringLiteral("confirm your age"), QStringLiteral("age-restricted"), QStringLiteral("members-only"),
        QStringLiteral("join this channel"), QStringLiteral("private video"), QStringLiteral("login required"),
    };
    const QString e = r.errorText().toLower();
    if (std::none_of(signs.begin(), signs.end(), [&e](const QString &s) { return e.contains(s); }))
        return r;
    const QStringList account = accountArgs();
    return account.isEmpty() ? r : runProcess(m_program, baseArgs() + account + args, opts);
}

QString YtDlp::cacheEntry(const QString &id) const
{
    return m_cacheDir.isEmpty() ? QString() : QDir(m_cacheDir).filePath(QStringLiteral("videos/") + id);
}

QString YtDlp::freshInfo(const QString &id) const
{
    const QString entry = cacheEntry(id);
    if (entry.isEmpty())
        return {};
    // The addresses in it are signed for about six hours.
    const QFileInfo fi(QDir(entry).filePath(QStringLiteral("audio.info.json")));
    const bool fresh = fi.exists() && fi.lastModified().secsTo(QDateTime::currentDateTime()) < 2 * 3600;
    return fresh ? fi.absoluteFilePath() : QString();
}

ProcResult YtDlp::runFor(const QString &id, const QStringList &args, const ProcOptions &opts)
{
    const QString info = freshInfo(id);
    if (!info.isEmpty()) {
        const ProcResult r = runProcess(m_program, baseArgs() + args + QStringList{QStringLiteral("--load-info-json"), info}, opts);
        if (r.stalled)
            qInfo().noquote() << "[yt-dlp]" << id << "came at a trickle; asking the page again";
        if (r.ok() || (m_cancel && m_cancel->load())) {
            ++m_cacheHits;
            return r;
        }
        // An address that lapsed early, a page that changed: ask again.
    }
    return run(args + QStringList{url(id)}, opts);
}

QJsonObject YtDlp::note(const QString &id, const QString &name) const
{
    const QString entry = cacheEntry(id);
    QFile f(QDir(entry).filePath(name + QStringLiteral(".note.json")));
    if (entry.isEmpty() || !f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void YtDlp::setNote(const QString &id, const QString &name, const QJsonObject &value) const
{
    const QString entry = cacheEntry(id);
    if (entry.isEmpty() || !QDir().mkpath(entry))
        return;
    QSaveFile f(QDir(entry).filePath(name + QStringLiteral(".note.json")));
    if (f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(value).toJson(QJsonDocument::Compact)) > 0)
        f.commit();
}

void YtDlp::pruneCache(const QString &dir, qint64 maxBytes, int maxAgeDays)
{
    const QDateTime now = QDateTime::currentDateTime();
    QDir searches(QDir(dir).filePath(QStringLiteral("search")));
    for (const QFileInfo &fi : searches.entryInfoList(QDir::Files)) {
        if (fi.lastModified().secsTo(now) > 24 * 3600)
            QFile::remove(fi.absoluteFilePath());
    }
    struct Entry {
        QString path;
        QDateTime when;
        qint64 bytes = 0;
    };
    QVector<Entry> entries;
    qint64 total = 0;
    QDir videos(QDir(dir).filePath(QStringLiteral("videos")));
    for (const QFileInfo &d : videos.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        Entry e;
        e.path = d.absoluteFilePath();
        for (const QFileInfo &f : QDir(e.path).entryInfoList(QDir::Files)) {
            e.bytes += f.size();
            if (!e.when.isValid() || f.lastModified() > e.when)
                e.when = f.lastModified();
        }
        if (!e.when.isValid() || e.when.daysTo(now) > maxAgeDays) {
            QDir(e.path).removeRecursively();
            continue;
        }
        total += e.bytes;
        entries << e;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.when < b.when; });
    for (const Entry &e : std::as_const(entries)) {
        if (total <= maxBytes)
            break;
        QDir(e.path).removeRecursively();
        total -= e.bytes;
    }
}

QStringList YtDlp::baseArgs() const
{
    QStringList a = {
        QStringLiteral("--ignore-config"), QStringLiteral("--no-playlist"), QStringLiteral("--no-warnings"),
        QStringLiteral("--socket-timeout"), QStringLiteral("30"), QStringLiteral("--retries"), QStringLiteral("5"),
        // Below this yt-dlp asks YouTube for the stream again.
        QStringLiteral("--throttled-rate"), QStringLiteral("100K"),
    };
#ifdef Q_OS_WIN
    // yt-dlp looks for its helpers beside itself and on PATH; here ffmpeg is
    // installed with the application and the JS runtime sits in toolsDir().
    const QString appDir = QCoreApplication::applicationDirPath();
    if (QFile::exists(appDir + QStringLiteral("/ffmpeg.exe")))
        a << QStringLiteral("--ffmpeg-location") << QDir::toNativeSeparators(appDir);
    const QString deno = toolsDir() + QStringLiteral("/deno.exe");
    if (QFile::exists(deno))
        a << QStringLiteral("--js-runtimes") << QStringLiteral("deno:") + QDir::toNativeSeparators(deno);
#endif
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
    // The same query comes up for every file of a song.
    QString cached;
    QByteArray json;
    if (!m_cacheDir.isEmpty()) {
        const QByteArray key = QCryptographicHash::hash(QStringLiteral("%1:%2").arg(count).arg(query).toUtf8(), QCryptographicHash::Sha1).toHex();
        cached = QDir(m_cacheDir).filePath(QStringLiteral("search/%1.json").arg(QString::fromLatin1(key)));
        QFile f(cached);
        if (QFileInfo(cached).lastModified().secsTo(QDateTime::currentDateTime()) < 24 * 3600 && f.open(QIODevice::ReadOnly)) {
            json = f.readAll();
            ++m_cacheHits;
        }
    }
    if (json.isEmpty()) {
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
        json = r.out;
        // An empty page is not kept: YouTube now and then answers a search
        // with nothing that it finds a moment later.
        if (!cached.isEmpty() && !QJsonDocument::fromJson(json).object().value(QLatin1String("entries")).toArray().isEmpty()) {
            QDir().mkpath(QFileInfo(cached).absolutePath());
            QSaveFile f(cached);
            if (f.open(QIODevice::WriteOnly) && f.write(json) == json.size())
                f.commit();
        }
    }
    const QJsonArray entries = QJsonDocument::fromJson(json).object().value(QLatin1String("entries")).toArray();
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

bool YtDlp::downloadAudio(const QString &id, const QString &workDir, QString *file, QJsonObject *info, QString *error,
                          bool premium)
{
    const QString name = premium ? QStringLiteral("premium") : QStringLiteral("audio");
    // What anyone is offered is kept: the next track that has this video as
    // a candidate listens to the same file.
    const QString entry = premium ? QString() : cacheEntry(id);
    const QString dir = entry.isEmpty() ? workDir : entry;
    if (!entry.isEmpty()) {
        const QString have = findFile(entry, name, {QStringLiteral(".json"), QStringLiteral(".part"), QStringLiteral(".ytdl")});
        QFile jf(QDir(entry).filePath(name + QStringLiteral(".info.json")));
        if (!have.isEmpty() && jf.open(QIODevice::ReadOnly)) {
            const QJsonObject cachedInfo = QJsonDocument::fromJson(jf.readAll()).object();
            if (!cachedInfo.isEmpty()) {
                if (info)
                    *info = cachedInfo;
                *file = have;
                ++m_cacheHits;
                // In use: not the next to be pruned.
                QFile used(have);
                if (used.open(QIODevice::ReadWrite))
                    used.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
                return true;
            }
        }
        // Half an entry is none.
        QDir(entry).removeRecursively();
        QDir().mkpath(entry);
    }
    QStringList args;
    args << QStringLiteral("-f") << QStringLiteral("ba/b") << QStringLiteral("--no-progress")
         << QStringLiteral("--write-info-json") << QStringLiteral("-o")
         << QDir(dir).filePath(name + QStringLiteral(".%(ext)s")) << url(id);
    ProcOptions opts;
    opts.cancel = m_cancel;
    opts.timeoutMs = 15 * 60 * 1000;
    opts.abortIf = stallWatch(dir);
    ProcResult r;
    if (premium) {
        const QStringList account = accountArgs();
        if (account.isEmpty()) {
            if (error)
                *error = QStringLiteral("no account cookies");
            return false;
        }
        r = runProcess(m_program, baseArgs() + account + args, opts);
    } else {
        r = run(args, opts);
    }
    if (!r.ok()) {
        if (error)
            *error = r.errorText();
        return false;
    }
    QFile jf(QDir(dir).filePath(name + QStringLiteral(".info.json")));
    if (info && jf.open(QIODevice::ReadOnly))
        *info = QJsonDocument::fromJson(jf.readAll()).object();
    *file = findFile(dir, name, {QStringLiteral(".json"), QStringLiteral(".part")});
    if (file->isEmpty()) {
        if (error)
            *error = QStringLiteral("yt-dlp produced no audio file");
        return false;
    }
    return true;
}

bool YtDlp::downloadSubtitles(const QString &id, const QString &dir, const QStringList &languages,
                              QHash<QString, QString> *files, QString *error)
{
    files->clear();
    if (languages.isEmpty())
        return true;
    // Each language with its variants: "en", "en-GB", "en-<label>".
    QStringList wanted;
    for (const QString &l : languages)
        wanted << l << l + QStringLiteral("-.*");
    QStringList args;
    args << QStringLiteral("--skip-download") << QStringLiteral("--write-subs") << QStringLiteral("--sub-langs")
         << wanted.join(QLatin1Char(',')) << QStringLiteral("--sub-format") << QStringLiteral("srv3")
         << QStringLiteral("--no-progress") << QStringLiteral("-o") << QDir(dir).filePath(QStringLiteral("subs.%(ext)s"))
         << url(id);
    ProcOptions opts;
    opts.cancel = m_cancel;
    opts.timeoutMs = 5 * 60 * 1000;
    const ProcResult r = run(args, opts);
    if (!r.ok()) {
        if (error)
            *error = r.errorText();
        return false;
    }
    // subs.<language>.srv3
    QHash<QString, QString> got;
    const QFileInfoList entries = QDir(dir).entryInfoList({QStringLiteral("subs.*.srv3")}, QDir::Files);
    for (const QFileInfo &fi : entries) {
        const QString name = fi.fileName();
        got.insert(name.mid(5, name.size() - 5 - 5), fi.absoluteFilePath());
    }
    for (const QString &l : languages) {
        const QString key = Subtitles::pickLanguage(got.keys(), l);
        if (!key.isEmpty())
            files->insert(l, got.value(key));
    }
    return true;
}

bool YtDlp::premiumInfo(const QString &id, QJsonObject *info, QString *error)
{
    const QStringList account = accountArgs();
    if (account.isEmpty()) {
        if (error)
            *error = QStringLiteral("no account cookies");
        return false;
    }
    ProcOptions opts;
    opts.cancel = m_cancel;
    opts.timeoutMs = 120000;
    // With its warnings: that the cookies are no longer accepted is one.
    QStringList base = baseArgs();
    base.removeAll(QStringLiteral("--no-warnings"));
    const ProcResult r = runProcess(m_program, base + account + QStringList{QStringLiteral("-J"), url(id)}, opts);
    if (!r.ok()) {
        if (error)
            *error = r.errorText();
        return false;
    }
    // yt-dlp carries on without the account, and lists what anyone is offered.
    if (r.err.contains("cookies are no longer valid")) {
        if (error)
            *error = QStringLiteral("the account's cookies have expired: export cookies.txt from the browser again");
        return false;
    }
    *info = QJsonDocument::fromJson(r.out).object();
    if (info->value(QLatin1String("formats")).toArray().isEmpty()) {
        if (error)
            *error = QStringLiteral("yt-dlp listed no formats");
        return false;
    }
    return true;
}

YtDlp::Account YtDlp::checkAccount(const QString &id, double *kbps, QString *error)
{
    QJsonObject info;
    QString why;
    if (!premiumInfo(id, &info, &why)) {
        if (error)
            *error = why;
        return cookiesExpired(why) ? Account::Expired : Account::Unknown;
    }
    const double best = bestAudioKbps(info);
    if (kbps)
        *kbps = best;
    // Anyone is offered about 130 kbit/s, a Premium account 256.
    return best >= 200 ? Account::Premium : Account::Ordinary;
}

bool YtDlp::looksUnavailable(const QString &error)
{
    static const QStringList signs = {
        QStringLiteral("video is not available"), QStringLiteral("video unavailable"),
        QStringLiteral("has been removed"), QStringLiteral("account associated with this video has been terminated"),
        QStringLiteral("not available in your country"), QStringLiteral("blocked it in your country"),
    };
    const QString e = error.toLower();
    return !looksBlocked(error) && std::any_of(signs.begin(), signs.end(), [&e](const QString &s) { return e.contains(s); });
}

bool YtDlp::cookiesExpired(const QString &error)
{
    return error.contains(QLatin1String("cookies have expired"));
}

double YtDlp::bestAudioKbps(const QJsonObject &info)
{
    double best = 0;
    for (const QJsonValue &v : info.value(QLatin1String("formats")).toArray()) {
        const QJsonObject f = v.toObject();
        if (f.value(QLatin1String("vcodec")).toString() == QLatin1String("none")
            && f.value(QLatin1String("acodec")).toString() != QLatin1String("none"))
            best = std::max(best, f.value(QLatin1String("abr")).toDouble());
    }
    return best;
}

int YtDlp::bestHeight(const QJsonObject &info)
{
    int best = 0;
    for (const QJsonValue &v : info.value(QLatin1String("formats")).toArray()) {
        const QJsonObject f = v.toObject();
        if (f.value(QLatin1String("vcodec")).toString() != QLatin1String("none"))
            best = std::max(best, f.value(QLatin1String("height")).toInt());
    }
    return best;
}

double YtDlp::audioKbps(const QJsonObject &info)
{
    const double abr = info.value(QLatin1String("abr")).toDouble();
    return abr > 0 ? abr : info.value(QLatin1String("tbr")).toDouble();
}

bool YtDlp::downloadPreview(const QString &id, const QString &dir, QString *file, QString *error)
{
    QStringList args;
    args << QStringLiteral("-f") << QStringLiteral("wv*[height>=144]/wv*/w") << QStringLiteral("--no-progress")
         << QStringLiteral("-o") << QDir(dir).filePath(QStringLiteral("preview.%(ext)s"));
    ProcOptions opts;
    opts.cancel = m_cancel;
    opts.timeoutMs = 15 * 60 * 1000;
    opts.abortIf = stallWatch(dir);
    const ProcResult r = runFor(id, args, opts);
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
    QStringList args;
    args << QStringLiteral("-f") << QStringLiteral("bv*/b") << QStringLiteral("--write-thumbnail")
         << QStringLiteral("--convert-thumbnails") << QStringLiteral("jpg") << QStringLiteral("--newline")
         << QStringLiteral("--progress-template")
         << QStringLiteral("download:MVP %(progress.downloaded_bytes)s %(progress.total_bytes)s %(progress.total_bytes_estimate)s")
         << QStringLiteral("-o") << QDir(dir).filePath(QStringLiteral("video.%(ext)s"));
    ProcOptions opts;
    opts.cancel = m_cancel;
    opts.timeoutMs = 4 * 60 * 60 * 1000;
    opts.abortIf = stallWatch(dir);
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
    const ProcResult r = runFor(id, args, opts);
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
