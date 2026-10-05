#pragma once

#include <QString>
#include <QStringList>

// Which YouTube channels are an artist's own. Beyond the channel's name
// (Matcher::isOwnChannel), two sources: MusicBrainz, whose artist records
// link to channels, and the user, whose approval of a video in review
// vouches for its channel. Both are kept in the library's database.
class Database;

namespace ArtistChannels {

// The channel ids MusicBrainz lists for `artist` (UC…). An empty list is a
// result too; `error` says when the question could not be asked.
QStringList fromMusicBrainz(const QString &artist, QString *error);

// Whether `channelId` (or, for a video from before ids were kept, the
// channel's name) is one of the artist's known channels, asking MusicBrainz
// once per artist and remembering the answer. Any of the track's names.
bool isArtistChannel(Database &db, const QStringList &artistNames, const QString &channelId, const QString &channelName);

} // namespace ArtistChannels
