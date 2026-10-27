#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

struct TrackInfo;

// MusicBrainz, the open music encyclopedia: whose channel an upload is on
// (ArtistChannels, docs/import-rules.html R5), and the music video it links
// to a track's recording (B7–B14).
namespace MusicBrainz {

// How far a recording's length may be from the track's (B8, B9).
constexpr int kLengthSlack = 10; // seconds

// Whether MusicBrainz is asked at all. (Tests run without the network.)
bool enabled();

// One request to its web service: `path` under ws/2, as JSON. At most one a
// second, as MusicBrainz asks, with a User-Agent that says who. A record it
// does not have sets `missing` and no error.
QByteArray get(const QString &path, QUrlQuery query, QString *error, bool *missing = nullptr);

// What the recordings are found by: when it changes (the tags were
// corrected), what was kept of an earlier answer no longer holds.
QString question(const TrackInfo &track);

// B7–B10: the recordings that are the track's. By the recording id its file
// carries; else by its ISRC; else the sure matches of its title and artist.
// `how` says which: "id", "isrc" or "title". None, without an error: B10.
QStringList recordingsOf(const TrackInfo &track, QString *how, QString *error);

// B11, B12: the YouTube uploads (video ids) linked to the music videos of
// those recordings, in MusicBrainz's order, each once. Not a link on the
// recordings themselves: that is the song's audio on YouTube.
QStringList musicVideosOf(const QStringList &recordings, QString *error);

} // namespace MusicBrainz
