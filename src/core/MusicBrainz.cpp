#include "core/MusicBrainz.h"

#include "core/Types.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <cmath>

namespace {

// MusicBrainz asks for one request a second and a User-Agent that says who.
QMutex g_pace;
QElapsedTimer g_last;

QString firstValue(const TrackInfo &track, const char *tag)
{
    return track.tags.value(QLatin1String(tag)).toString().section(QStringLiteral("; "), 0, 0).trimmed();
}

// A recording of the track's length, as far as MusicBrainz knows its length.
bool lengthFits(const QJsonObject &recording, double trackSeconds)
{
    const double ms = recording.value(QLatin1String("length")).toDouble();
    return ms <= 0 || trackSeconds <= 0 || std::abs(ms / 1000.0 - trackSeconds) <= MusicBrainz::kLengthSlack;
}

QStringList audioRecordings(const QJsonArray &recordings, double trackSeconds, int minScore)
{
    QStringList out;
    for (const QJsonValue &v : recordings) {
        const QJsonObject r = v.toObject();
        if (minScore > 0 && r.value(QLatin1String("score")).toVariant().toInt() < minScore)
            continue;
        const QString id = r.value(QLatin1String("id")).toString();
        if (!id.isEmpty() && !r.value(QLatin1String("video")).toBool() && lengthFits(r, trackSeconds) && !out.contains(id))
            out << id;
    }
    return out;
}

QString youtubeId(const QString &url)
{
    static const QRegularExpression re(QStringLiteral("(?:youtube\\.com/watch\\?(?:.*&)?v=|youtu\\.be/|youtube\\.com/embed/)([A-Za-z0-9_-]{11})"));
    const QRegularExpressionMatch m = re.match(url);
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

namespace MusicBrainz {

bool enabled()
{
    return !qEnvironmentVariableIsSet("MVPLAYER_NO_MUSICBRAINZ");
}

QByteArray get(const QString &path, QUrlQuery query, QString *error, bool *missing)
{
    if (missing)
        *missing = false;
    {
        QMutexLocker lock(&g_pace);
        if (g_last.isValid() && g_last.elapsed() < 1100)
            QThread::msleep(1100 - g_last.elapsed());
        g_last.restart();
    }
    // (Tests stand a server of their own in for it.)
    QUrl url(qEnvironmentVariable("MVPLAYER_MUSICBRAINZ_URL", QStringLiteral("https://musicbrainz.org/ws/2/")) + path);
    query.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
    url.setQuery(query);
    QNetworkAccessManager net;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("mvplayer/%1 (https://github.com/brpjerry/mvplayer)").arg(QCoreApplication::applicationVersion()));
    QNetworkReply *reply = net.get(req);
    QEventLoop loop;
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray out;
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
        out = reply->readAll();
    } else if (reply->isFinished() && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404) {
        if (missing)
            *missing = true;
    } else if (error) {
        *error = reply->isFinished() ? reply->errorString() : QStringLiteral("timed out");
    }
    reply->abort();
    reply->deleteLater();
    return out;
}

QString question(const TrackInfo &track)
{
    return QStringList{firstValue(track, "MUSICBRAINZ_TRACKID"), firstValue(track, "ISRC"), track.title,
                       track.artist.isEmpty() ? track.albumArtist : track.artist, QString::number(qRound(track.duration))}
        .join(QLatin1Char('|'));
}

QStringList recordingsOf(const TrackInfo &track, QString *how, QString *error)
{
    // B7: as MusicBrainz Picard tags a file.
    static const QRegularExpression mbid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    const QString id = firstValue(track, "MUSICBRAINZ_TRACKID").toLower();
    if (mbid.match(id).hasMatch()) {
        *how = QStringLiteral("id");
        return {id};
    }
    // B8: an ISRC that MusicBrainz knows.
    static const QRegularExpression isrcForm(QStringLiteral("^[A-Z]{2}[A-Z0-9]{3}[0-9]{7}$"));
    const QString isrc = firstValue(track, "ISRC").toUpper().remove(QLatin1Char('-')).remove(QLatin1Char(' '));
    if (isrcForm.match(isrc).hasMatch()) {
        bool missing = false;
        const QByteArray found = get(QStringLiteral("isrc/") + isrc, {}, error, &missing);
        if (!error->isEmpty())
            return {};
        if (!missing) {
            *how = QStringLiteral("isrc");
            return audioRecordings(QJsonDocument::fromJson(found).object().value(QLatin1String("recordings")).toArray(), track.duration, 0);
        }
    }
    // B9: the sure matches of its title and artist, a phrase each.
    const auto phrase = [](QString s) { return s.remove(QLatin1Char('"')).remove(QLatin1Char('\\')).trimmed(); };
    const QString title = phrase(track.title);
    const QString artist = phrase((track.artist.isEmpty() ? track.albumArtist : track.artist).section(QStringLiteral("; "), 0, 0));
    if (title.isEmpty() || artist.isEmpty())
        return {};
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("query"), QStringLiteral("recording:\"%1\" AND (artist:\"%2\" OR artistname:\"%2\")").arg(title, artist));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
    const QByteArray found = get(QStringLiteral("recording"), q, error);
    if (!error->isEmpty())
        return {};
    *how = QStringLiteral("title");
    return audioRecordings(QJsonDocument::fromJson(found).object().value(QLatin1String("recordings")).toArray(), track.duration, 100);
}

QStringList musicVideosOf(const QStringList &recordings, QString *error)
{
    QStringList videos;
    for (const QString &recording : recordings) {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("inc"), QStringLiteral("recording-rels"));
        bool missing = false;
        const QByteArray detail = get(QStringLiteral("recording/") + recording, q, error, &missing);
        if (!error->isEmpty())
            return {};
        for (const QJsonValue &v : QJsonDocument::fromJson(detail).object().value(QLatin1String("relations")).toArray()) {
            const QJsonObject rel = v.toObject();
            const QJsonObject other = rel.value(QLatin1String("recording")).toObject();
            const QString id = other.value(QLatin1String("id")).toString();
            // From a video's side the same link leads back to the song.
            if (rel.value(QLatin1String("type")).toString() == QLatin1String("music video") && other.value(QLatin1String("video")).toBool()
                && !id.isEmpty() && !videos.contains(id))
                videos << id;
        }
    }
    QStringList uploads;
    for (const QString &video : std::as_const(videos)) {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("inc"), QStringLiteral("url-rels"));
        bool missing = false;
        const QByteArray detail = get(QStringLiteral("recording/") + video, q, error, &missing);
        if (!error->isEmpty())
            return {};
        for (const QJsonValue &v : QJsonDocument::fromJson(detail).object().value(QLatin1String("relations")).toArray()) {
            const QString yt = youtubeId(v.toObject().value(QLatin1String("url")).toObject().value(QLatin1String("resource")).toString());
            if (!yt.isEmpty() && !uploads.contains(yt))
                uploads << yt;
        }
    }
    return uploads;
}

} // namespace MusicBrainz
