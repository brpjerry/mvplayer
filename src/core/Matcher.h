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

QStringList searchQueries(const TrackInfo &track);

// False when one of the two is marked as a version the other is not: a live
// take, a remix, a cover, ... (the track by its title or album).
bool sameVersion(const TrackInfo &track, const QString &videoTitle);

// Scores every candidate against the track and sorts best-first.
void rank(const TrackInfo &track, QVector<YtCandidate> &candidates);

} // namespace Matcher
