#include "core/ArtistChannels.h"

#include "core/Database.h"
#include "core/MusicBrainz.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrlQuery>

namespace {

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
    QUrlQuery q;
    // An exact name, in any of the artist's spellings.
    q.addQueryItem(QStringLiteral("query"), QStringLiteral("artist:\"%1\" OR alias:\"%1\"").arg(QString(artist).replace(QLatin1Char('"'), QString())));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("3"));
    const QByteArray found = MusicBrainz::get(QStringLiteral("artist/"), q, error);
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
    QUrlQuery rq;
    rq.addQueryItem(QStringLiteral("inc"), QStringLiteral("url-rels"));
    const QByteArray detail = MusicBrainz::get(QStringLiteral("artist/") + id, rq, error);
    QStringList ids;
    for (const QJsonValue &v : QJsonDocument::fromJson(detail).object().value(QLatin1String("relations")).toArray()) {
        const QString cid = channelIdOf(v.toObject().value(QLatin1String("url")).toObject().value(QLatin1String("resource")).toString());
        if (!cid.isEmpty() && !ids.contains(cid))
            ids << cid;
    }
    return ids;
}

bool knownArtistChannel(Database &db, const QStringList &artistNames, const QString &channelId, const QString &channelName)
{
    if (db.channelAllowed(channelId))
        return true;
    for (const QString &artist : artistNames) {
        const QString name = artist.trimmed();
        if (name.size() >= 2 && db.isArtistChannel(name, channelId, channelName))
            return true;
    }
    return false;
}

bool isArtistChannel(Database &db, const QStringList &artistNames, const QString &channelId, const QString &channelName)
{
    // A channel the user allowed is an artist's channel for every track.
    if (db.channelAllowed(channelId))
        return true;
    for (const QString &artist : artistNames) {
        const QString name = artist.trimmed();
        if (name.size() < 2)
            continue;
        // (Tests run without the network.)
        if (!db.artistChannelsKnown(name) && MusicBrainz::enabled()) {
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
