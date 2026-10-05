#include "core/ArtistChannels.h"

#include "core/Database.h"

#include <QCoreApplication>
#include <QDateTime>
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
#include <QUrlQuery>

namespace {

// MusicBrainz asks for one request a second and a User-Agent that says who.
QMutex g_pace;
QElapsedTimer g_last;

QByteArray get(const QUrl &url, QString *error)
{
    {
        QMutexLocker lock(&g_pace);
        if (g_last.isValid() && g_last.elapsed() < 1100)
            QThread::msleep(1100 - g_last.elapsed());
        g_last.restart();
    }
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
    } else if (error) {
        *error = reply->isFinished() ? reply->errorString() : QStringLiteral("timed out");
    }
    reply->abort();
    reply->deleteLater();
    return out;
}

QString channelIdOf(const QString &url)
{
    static const QRegularExpression re(QStringLiteral("youtube\\.com/channel/(UC[A-Za-z0-9_-]{22})"));
    const QRegularExpressionMatch m = re.match(url);
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

namespace ArtistChannels {

QStringList fromMusicBrainz(const QString &artist, QString *error)
{
    QUrl search(QStringLiteral("https://musicbrainz.org/ws/2/artist/"));
    QUrlQuery q;
    // An exact name, in any of the artist's spellings.
    q.addQueryItem(QStringLiteral("query"), QStringLiteral("artist:\"%1\" OR alias:\"%1\"").arg(QString(artist).replace(QLatin1Char('"'), QString())));
    q.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("3"));
    search.setQuery(q);
    const QByteArray found = get(search, error);
    if (found.isEmpty())
        return {};
    QString id;
    for (const QJsonValue &v : QJsonDocument::fromJson(found).object().value(QLatin1String("artists")).toArray()) {
        const QJsonObject a = v.toObject();
        // Only a sure match: the top result with the full score.
        if (a.value(QLatin1String("score")).toInt() >= 100) {
            id = a.value(QLatin1String("id")).toString();
            break;
        }
    }
    if (id.isEmpty())
        return {};
    QUrl rels(QStringLiteral("https://musicbrainz.org/ws/2/artist/%1").arg(id));
    QUrlQuery rq;
    rq.addQueryItem(QStringLiteral("inc"), QStringLiteral("url-rels"));
    rq.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
    rels.setQuery(rq);
    const QByteArray detail = get(rels, error);
    QStringList ids;
    for (const QJsonValue &v : QJsonDocument::fromJson(detail).object().value(QLatin1String("relations")).toArray()) {
        const QString cid = channelIdOf(v.toObject().value(QLatin1String("url")).toObject().value(QLatin1String("resource")).toString());
        if (!cid.isEmpty() && !ids.contains(cid))
            ids << cid;
    }
    return ids;
}

bool isArtistChannel(Database &db, const QStringList &artistNames, const QString &channelId, const QString &channelName)
{
    for (const QString &artist : artistNames) {
        const QString name = artist.trimmed();
        if (name.size() < 2)
            continue;
        if (!db.artistChannelsKnown(name)) {
            QString error;
            const QStringList ids = fromMusicBrainz(name, &error);
            if (error.isEmpty()) {
                for (const QString &cid : ids)
                    db.addArtistChannel(name, cid, QString(), QStringLiteral("musicbrainz"));
                db.setArtistChannelsKnown(name, ids.isEmpty() ? 30 : 180);
            } else {
                // Not asked again for a day.
                db.setArtistChannelsKnown(name, 1);
            }
        }
        if (db.isArtistChannel(name, channelId, channelName))
            return true;
    }
    return false;
}

} // namespace ArtistChannels
