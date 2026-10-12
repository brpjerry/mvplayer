#include "core/Judge.h"

#include "core/Util.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>

Judge::Judge(const QString &keyFile, QObject *parent)
    : QObject(parent)
    , m_keyFile(keyFile)
{
    m_key = readKey(keyFile);
}

QString Judge::readKey(const QString &keyFile)
{
    QFile f(keyFile);
    if (f.open(QIODevice::ReadOnly))
        return QString::fromUtf8(f.readAll()).trimmed();
    return {};
}

QString Judge::keyHint() const
{
    if (m_key.size() < 12)
        return m_key.isEmpty() ? QString() : QStringLiteral("…");
    return m_key.left(7) + QStringLiteral("…") + m_key.right(4);
}

QString Judge::setKey(const QString &given)
{
    const QString key = given.trimmed();
    // Keys from the console: "sk-ant-" and then the kind and the key itself.
    if (!key.startsWith(QLatin1String("sk-ant-")) || key.size() < 20 || key.contains(QLatin1Char(' ')))
        return tr("That is not a Claude API key: they start with “sk-ant-”.");
    QSaveFile out(m_keyFile);
    if (!QDir().mkpath(QFileInfo(m_keyFile).absolutePath()) || !out.open(QIODevice::WriteOnly))
        return tr("Could not store the key.");
    // As good as a password: for this user only.
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (out.write(key.toUtf8()) < 0 || !out.commit())
        return tr("Could not store the key.");
    m_key = key;
    m_state.clear();
    m_status.clear();
    emit keyChanged();
    return {};
}

void Judge::removeKey()
{
    QFile::remove(m_keyFile);
    m_key.clear();
    m_state.clear();
    m_status.clear();
    emit keyChanged();
}

QString Judge::apiBase()
{
    const QString env = qEnvironmentVariable("MVPLAYER_CLAUDE_API");
    return env.isEmpty() ? QStringLiteral("https://api.anthropic.com") : env;
}

void Judge::check()
{
    if (m_checking || m_key.isEmpty())
        return;
    m_checking = true;
    m_state.clear();
    m_status.clear();
    emit checked();
    // The models the key may use: a request that costs nothing and says
    // whether the key is good.
    QNetworkRequest request(QUrl(apiBase() + QStringLiteral("/v1/models?limit=100")));
    request.setRawHeader("x-api-key", m_key.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    request.setTransferTimeout(20000);
    QNetworkReply *reply = m_net.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError && http == 0) {
            finish(QStringLiteral("error"), tr("Could not reach the API: %1").arg(reply->errorString()));
            return;
        }
        if (http == 401 || http == 403) {
            finish(QStringLiteral("bad"), tr("The API refuses the key: %1")
                       .arg(body.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString()));
            return;
        }
        if (http != 200) {
            const QString message = body.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString();
            finish(QStringLiteral("error"), message.isEmpty() ? tr("The API answered %1").arg(http) : message);
            return;
        }
        QStringList ids;
        for (const QJsonValue &m : body.value(QLatin1String("data")).toArray())
            ids << m.toObject().value(QLatin1String("id")).toString();
        finish(QStringLiteral("ok"), ids.isEmpty() ? tr("The key works, but no model is open to it.")
                                                   : tr("The key works: %1 models open to it, %2 among them.").arg(ids.size()).arg(ids.first()));
    });
}

void Judge::finish(const QString &state, const QString &status)
{
    m_checking = false;
    m_state = state;
    m_status = status;
    emit checked();
}

// ---- The models ------------------------------------------------------------

QStringList Judge::models()
{
    return {QStringLiteral("haiku"), QStringLiteral("sonnet"), QStringLiteral("opus")};
}

QString Judge::modelId(const QString &name)
{
    if (name == QLatin1String("haiku"))
        return QStringLiteral("claude-haiku-5-5");
    if (name == QLatin1String("opus"))
        return QStringLiteral("claude-opus-5-5");
    return QStringLiteral("claude-sonnet-5-5");
}

QString Judge::modelLabel(const QString &name)
{
    if (name == QLatin1String("haiku"))
        return QStringLiteral("Haiku 5.5");
    if (name == QLatin1String("opus"))
        return QStringLiteral("Opus 5.5");
    return QStringLiteral("Sonnet 5.5");
}

double Judge::cost(const QString &modelId, const QJsonObject &usage)
{
    // Dollars per million tokens in and out, October 2026 (N).
    double in = 2, out = 10;
    if (modelId.contains(QLatin1String("haiku"))) {
        in = 0.10;
        out = 0.50;
    } else if (modelId.contains(QLatin1String("opus"))) {
        in = 4;
        out = 20;
    }
    return (usage.value(QLatin1String("input_tokens")).toDouble() * in
            + usage.value(QLatin1String("output_tokens")).toDouble() * out
            + usage.value(QLatin1String("cache_read_input_tokens")).toDouble() * in * 0.1
            + usage.value(QLatin1String("cache_creation_input_tokens")).toDouble() * in * 1.25) / 1e6;
}

// ---- One question ------------------------------------------------------------

Judge::Answer Judge::ask(const QString &key, const QString &modelId, const QString &words, const QJsonArray &content,
                         const QString &schemaJson, int maxTokens, const QString &cacheDir, const std::atomic<bool> *cancel)
{
    Answer a;
    a.model = modelId;
    if (key.isEmpty()) {
        a.error = QStringLiteral("no API key");
        a.refused = true;
        return a;
    }
    const QJsonObject schema = QJsonDocument::fromJson(schemaJson.toUtf8()).object();
    const QJsonObject body{
        {QStringLiteral("model"), modelId},
        {QStringLiteral("max_tokens"), maxTokens},
        {QStringLiteral("system"), words},
        {QStringLiteral("messages"), QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), content}}}},
        {QStringLiteral("output_config"), QJsonObject{{QStringLiteral("format"), QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")}, {QStringLiteral("schema"), schema}}},
                                                      {QStringLiteral("effort"), QStringLiteral("low")}}},
    };
    const QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);

    // Kept a day: the same question gets the same answer without a request.
    QString cacheFile;
    if (!cacheDir.isEmpty()) {
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(32));
        cacheFile = QDir(cacheDir).filePath(QStringLiteral("judge/%1.json").arg(hash));
        QFile f(cacheFile);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject kept = QJsonDocument::fromJson(f.readAll()).object();
            if (QDateTime::currentSecsSinceEpoch() - kept.value(QLatin1String("t")).toInteger() < 86400) {
                a.json = kept.value(QLatin1String("answer")).toObject();
                a.usage = kept.value(QLatin1String("usage")).toObject();
                a.cached = true;
                return a;
            }
        }
    }

    QElapsedTimer clock;
    clock.start();
    for (int attempt = 0; attempt <= kRetries; ++attempt) {
        if (cancel && *cancel) {
            a.error = QStringLiteral("cancelled");
            return a;
        }
        if (attempt > 0)
            QThread::msleep(attempt == 1 ? 2000 : 5000);
        // On the calling thread, with a loop of its own: the import jobs
        // have none.
        QNetworkAccessManager net;
        QNetworkRequest request(QUrl(apiBase() + QStringLiteral("/v1/messages")));
        request.setRawHeader("x-api-key", key.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setTransferTimeout(180000);
        QNetworkReply *reply = net.post(request, bytes);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (cancel && *cancel)
                reply->abort();
        });
        poll.start(200);
        loop.exec();
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        const QJsonObject answer = QJsonDocument::fromJson(raw).object();
        if (cancel && *cancel) {
            a.error = QStringLiteral("cancelled");
            return a;
        }
        if (http == 0) {
            a.error = QStringLiteral("could not reach the API: %1").arg(reply->errorString());
            continue;
        }
        if (http == 401 || http == 403) {
            a.error = QStringLiteral("the API refuses the key: %1")
                          .arg(answer.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString());
            a.refused = true;
            return a;
        }
        if (http == 429 || http == 529 || http >= 500) {
            a.error = QStringLiteral("the API answered %1: %2")
                          .arg(http).arg(answer.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString());
            continue;
        }
        if (http != 200) {
            a.error = QStringLiteral("the API answered %1: %2")
                          .arg(http).arg(answer.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString());
            a.refused = true;
            return a;
        }
        a.usage = answer.value(QLatin1String("usage")).toObject();
        a.cost = cost(modelId, a.usage);
        a.seconds = clock.elapsed() / 1000.0;
        QString text;
        for (const QJsonValue &block : answer.value(QLatin1String("content")).toArray()) {
            if (block.toObject().value(QLatin1String("type")).toString() == QLatin1String("text"))
                text += block.toObject().value(QLatin1String("text")).toString();
        }
        const QJsonObject json = QJsonDocument::fromJson(text.toUtf8()).object();
        if (json.isEmpty()) {
            // Not the JSON asked for (a refusal, or cut short): asked again (V5).
            a.error = QStringLiteral("the answer was not the JSON asked for (%1)")
                          .arg(answer.value(QLatin1String("stop_reason")).toString());
            continue;
        }
        a.json = json;
        a.error.clear();
        if (!cacheFile.isEmpty()) {
            QDir().mkpath(QFileInfo(cacheFile).absolutePath());
            QSaveFile f(cacheFile);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QJsonDocument(QJsonObject{{QStringLiteral("t"), QDateTime::currentSecsSinceEpoch()}, {QStringLiteral("model"), modelId},
                                                  {QStringLiteral("answer"), json}, {QStringLiteral("usage"), a.usage}})
                            .toJson(QJsonDocument::Compact));
                f.commit();
            }
        }
        return a;
    }
    return a;
}

// ---- The picture -------------------------------------------------------------

Judge::Picture Judge::frames(const QString &file, double durationSec, const QString &outDir, const std::atomic<bool> *cancel)
{
    Picture p;
    static const double positions[] = {0.10, 0.35, 0.50, 0.65, 0.90};
    if (durationSec <= 0) {
        p.error = QStringLiteral("the video's length is not known");
        return p;
    }
    QDir().mkpath(outDir);
    QVector<QByteArray> greys;
    int i = 0;
    for (double at : positions) {
        ++i;
        const QString jpeg = QDir(outDir).filePath(QStringLiteral("frame-%1.jpg").arg(i));
        const QString grey = QDir(outDir).filePath(QStringLiteral("frame-%1.gray").arg(i));
        ProcOptions o;
        o.cancel = cancel;
        o.timeoutMs = 60000;
        // The frame for the judge, and a small grey copy for the check.
        const ProcResult r = runProcess(toolPath(QStringLiteral("ffmpeg")),
                                        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
                                         QStringLiteral("-ss"), QString::number(durationSec * at, 'f', 2), QStringLiteral("-i"), file,
                                         QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-vf"), QStringLiteral("scale=%1:-2").arg(kFrameWidth),
                                         QStringLiteral("-q:v"), QStringLiteral("4"), jpeg,
                                         QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-vf"), QStringLiteral("scale=%1:%2").arg(kCheckWidth).arg(kCheckWidth * 9 / 16),
                                         QStringLiteral("-pix_fmt"), QStringLiteral("gray"), QStringLiteral("-f"), QStringLiteral("rawvideo"), grey},
                                        o);
        if (cancel && *cancel) {
            p.error = QStringLiteral("cancelled");
            return p;
        }
        QFile g(grey);
        if (!r.ok() || !QFile::exists(jpeg) || !g.open(QIODevice::ReadOnly)) {
            // The last frames of a file may lie past where the decoder can
            // seek; the ones that could be taken are enough.
            if (p.frames.size() >= 3)
                break;
            p.error = r.ok() ? QStringLiteral("no frame at %1%").arg(qRound(at * 100)) : r.errorText();
            return p;
        }
        greys.append(g.readAll());
        QFile::remove(grey);
        p.frames << jpeg;
        p.positions << at;
    }
    const int w = kCheckWidth, h = kCheckWidth * 9 / 16, n = w * h;
    for (const QByteArray &g : greys) {
        if (g.size() != n) {
            p.error = QStringLiteral("the frames could not be compared");
            return p;
        }
    }
    int moved = 0, inner = 0;
    for (int k = 0; k < n; ++k) {
        int lo = 255, hi = 0;
        for (const QByteArray &g : greys) {
            const int v = uchar(g.at(k));
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (hi - lo > kCheckLevels) {
            ++moved;
            const int row = k / w;
            if (row >= h * 0.10 && row < h * 0.85)
                ++inner;
        }
    }
    p.changed = double(moved) / n;
    p.edgeOnly = moved > 0 && double(inner) / n < 0.005;
    p.valid = true;
    return p;
}

QString Judge::pictureText(const Picture &p)
{
    if (!p.valid)
        return QStringLiteral("the picture could not be looked at (%1)").arg(p.error);
    QString s = QStringLiteral("between the %1 frames sampled across the video, %2% of the picture ever changes")
                    .arg(p.frames.size()).arg(p.changed * 100, 0, 'f', 1);
    if (p.changed <= 0)
        s += QStringLiteral(" (the frames are identical)");
    else if (p.edgeOnly)
        s += QStringLiteral(", only along the top or bottom edge");
    return s;
}

// ---- The facts of a case -----------------------------------------------------

QString Judge::trackLine(const TrackInfo &t)
{
    const QString audio = t.lossless ? QStringLiteral("lossless")
        : QStringLiteral("%1 %2 kbit/s").arg(t.codec.isEmpty() ? QStringLiteral("?") : t.codec).arg(t.bitrate > 0 ? QString::number(t.bitrate) : QStringLiteral("?"));
    const QString artist = t.artist == t.albumArtist || t.albumArtist.isEmpty() ? t.artist
        : QStringLiteral("%1 (album artist %2)").arg(t.artist, t.albumArtist);
    QString line = QStringLiteral("“%1” by %2, album “%3”%4, %5 s, %6")
                       .arg(t.title, artist, t.album, t.year > 0 ? QStringLiteral(", released %1").arg(t.year) : QString())
                       .arg(qRound(t.duration))
                       .arg(audio);
    if (!t.genre.isEmpty())
        line += QStringLiteral(", genre tag “%1”").arg(t.genre);
    // The folder the file is in, two levels up: often the artist and the album.
    const QStringList parts = t.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() >= 3)
        line += QStringLiteral("; file under “%1/%2”").arg(parts.at(parts.size() - 3), parts.at(parts.size() - 2));
    return line;
}

QString Judge::audioText(const AudioAlign::Result &r)
{
    const double whole = std::min(r.trackSec, r.videoSec);
    if (whole <= 0)
        return QStringLiteral("not measured");
    QStringList parts{QStringLiteral("fingerprints match %1% of the video").arg(qRound(std::min(1.0, r.fpMatchedSec / whole) * 100)),
                      QStringLiteral("same waveform %1% of the track").arg(qRound(std::min(1.0, r.goodSec / whole) * 100))};
    const double gap = r.interruptedSec();
    if (gap > 0)
        parts << QStringLiteral("%1 s of other audio inside the song").arg(qRound(gap));
    if (r.byOffset)
        parts << QStringLiteral("placed by offset (same performance in another mix)");
    return parts.join(QStringLiteral("; "));
}

QJsonArray Judge::queryContent(const TrackInfo &t, const QString &sound)
{
    QString text = QStringLiteral("Track: %1.").arg(trackLine(t));
    if (!sound.isEmpty())
        text += QLatin1Char('\n') + sound;
    return {QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}}};
}

static QString views(qint64 n)
{
    if (n < 0)
        return QStringLiteral("views unknown");
    if (n >= 1000000)
        return QStringLiteral("%1M views").arg(n / 1e6, 0, 'f', 1);
    if (n >= 1000)
        return QStringLiteral("%1K views").arg(n / 1000);
    return QStringLiteral("%1 views").arg(n);
}

static QString channelLine(const Judge::Upload &u)
{
    QString s = QStringLiteral("channel “%1”").arg(u.channel);
    if (!u.handle.isEmpty())
        s += QStringLiteral(" (%1)").arg(u.handle);
    if (u.verified)
        s += QStringLiteral(" ✓verified");
    if (u.subscribers >= 0)
        s += QStringLiteral(", %1 subscribers").arg(u.subscribers);
    if (u.allowed)
        s += QStringLiteral(", a channel you allowed");
    return s;
}

QJsonArray Judge::pickContent(const TrackInfo &t, const QVector<Upload> &results)
{
    QStringList lines;
    int i = 0;
    for (const Upload &u : results) {
        lines << QStringLiteral("%1. id %2: “%3” — %4, %5 s, %6 (query %7, result %8)")
                     .arg(++i).arg(u.id, u.title, channelLine(u)).arg(qRound(u.duration)).arg(views(u.views)).arg(u.query).arg(u.rank);
    }
    return {QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("Track: %1.\n\nSearch results:\n%2\n\nWhich uploads should be downloaded and listened to, best first?")
                                                     .arg(trackLine(t), lines.join(QLatin1Char('\n')))}}};
}

QJsonArray Judge::verdictContent(const TrackInfo &t, const QVector<TrackInfo> &others, const QVector<Upload> &uploads)
{
    QString head = QStringLiteral("Track: %1.").arg(trackLine(t));
    if (!others.isEmpty()) {
        QStringList lines;
        for (const TrackInfo &o : others)
            lines << trackLine(o);
        head += QStringLiteral("\nOther tracks in the library that the audio check also matched to these uploads (the track above is the one to judge for): ")
            + lines.join(QStringLiteral("; "));
    }
    QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), head + QStringLiteral("\n\n%1 upload(s) that contain the song:").arg(uploads.size())}}};
    int i = 0;
    for (const Upload &u : uploads) {
        ++i;
        QString text = QStringLiteral("\nUpload %1: “%2” on %3, %4 s").arg(i).arg(u.title, channelLine(u)).arg(qRound(u.duration));
        if (u.width > 0)
            text += QStringLiteral(", %1x%2").arg(u.width).arg(u.height);
        else if (u.height > 0)
            text += QStringLiteral(", %1p").arg(u.height);
        if (!u.date.isEmpty())
            text += QStringLiteral(", uploaded %1").arg(u.date);
        if (u.views >= 0)
            text += QStringLiteral(", %1").arg(views(u.views));
        text += QStringLiteral(".\nAudio check: %1.").arg(u.audio.isEmpty() ? QStringLiteral("not measured") : u.audio);
        text += QStringLiteral("\nPicture check: %1.").arg(u.picture.valid ? pictureText(u.picture) : u.pictureNote.isEmpty() ? pictureText(u.picture) : u.pictureNote);
        if (u.picture.valid && !u.picture.frames.isEmpty()) {
            QStringList at;
            for (double pos : u.picture.positions)
                at << QStringLiteral("%1%").arg(qRound(pos * 100));
            text += QStringLiteral("\n%1 frames at %2 of the video:").arg(u.picture.frames.size()).arg(at.join(QStringLiteral(", ")));
        }
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}});
        if (u.picture.valid) {
            for (const QString &frame : u.picture.frames) {
                QFile f(frame);
                if (!f.open(QIODevice::ReadOnly))
                    continue;
                content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                           {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                                                                  {QStringLiteral("media_type"), QStringLiteral("image/jpeg")},
                                                                                  {QStringLiteral("data"), QString::fromLatin1(f.readAll().toBase64())}}}});
            }
        }
    }
    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("text"), QStringLiteral("\nJudge each upload, then give the verdict for the track: accept (with the index of the upload to keep), reject all, or unsure.")}});
    return content;
}

QJsonObject Judge::optionAt(const QJsonObject &verdict, int index)
{
    for (const QJsonValue &o : verdict.value(QLatin1String("options")).toArray()) {
        if (o.toObject().value(QLatin1String("index")).toInt() == index)
            return o.toObject();
    }
    return {};
}
