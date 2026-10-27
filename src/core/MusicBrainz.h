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
// corrected, the file is another), what was kept of an earlier answer no
// longer holds. `packedPrint`: the file's fingerprint (AudioPrint::pack).
QString question(const TrackInfo &track, const QByteArray &packedPrint);

// B7–B10, B15, B16: the recordings that are the track's, all found in any of
// these ways: the recording id its file carries, AcoustID by the file's
// fingerprint (none where `packedPrint` is empty), its ISRC, the sure
// matches of its title and artist. `ways` names those that found one ("id",
// "acoustid", "isrc", "title"). `error`: MusicBrainz could not be asked, and
// nothing is found. `acoustidError`: AcoustID could not be, and the other
// ways count (B16). None, without an error: B10.
QStringList recordingsOf(const TrackInfo &track, const QByteArray &packedPrint, QStringList *ways, QString *error, QString *acoustidError);

// B11, B12: the YouTube uploads (video ids) linked to the music videos of
// those recordings, in MusicBrainz's order, each once. Not a link on the
// recordings themselves: that is the song's audio on YouTube.
QStringList musicVideosOf(const QStringList &recordings, QString *error);

} // namespace MusicBrainz
