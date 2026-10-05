#pragma once

#include "core/Types.h"

#include <QVector>

// One YouTube search result, plus how well its metadata fits a track.
struct YtCandidate {
    QString id;
    QString title;
    QString channel;
    double duration = 0;
    qint64 views = 0;
    bool verified = false;
    int rank = 0;          // position in the search results

    double score = 0;
    bool trusted = false;  // looks like an official upload
    // The upload is vouched for: the artist's own channel or a verified one.
    // One that looks official by its title alone ("… (Music Video)") is
    // examined like the others, but not taken as the track's own video
    // short of a near-complete match.
    bool vouched = false;
    QString rejectReason;  // non-empty: never consider this result
};

namespace Matcher {

// Tracks that by their title cannot have a music video of their own
// (instrumentals, karaoke versions, ...).
bool isNonMvTrack(const TrackInfo &track, QString *why = nullptr);

// Titles that stand for talk between songs rather than a song: "MC",
// "MC06", "MC5 at <venue>", "Talk 2", "Interview", or such a word as a tag
// at the end ("... (MC)"). The word has to be the whole title: "Talk to Me"
// and "MC Hammer Medley" are songs.
bool isTalkTitle(const QString &title);

// Whether an upload names one of the track's artists, in its title or as its
// channel. Where the sound alone does not settle whose performance a video
// is, an upload that names none of them is not taken for the track's.
bool namesArtist(const TrackInfo &track, const QString &videoTitle, const QString &channel);

// Whether a channel is the artist's own: the artist's name and nothing else
// but "official", "channel" and the like. "Artist Latino" and "We love
// Artist" are somebody else's.
bool isOwnChannel(const TrackInfo &track, const QString &channel);

QStringList searchQueries(const TrackInfo &track);

// False when one of the two is marked as a version the other is not: a live
// take, a remix, a cover, ... (the track by its title or album).
bool sameVersion(const TrackInfo &track, const QString &videoTitle);

// Scores every candidate against the track and sorts best-first.
void rank(const TrackInfo &track, QVector<YtCandidate> &candidates);

} // namespace Matcher
