#pragma once

#include "core/Database.h"

#include <QJsonObject>

#include <atomic>

namespace LibraryScanner {

struct Result {
    QVector<qint64> videosChanged; // videos whose tags were refreshed from their track
    QStringList directories;       // every directory seen, for change watching
    int total = 0;
    int added = 0;
    int changed = 0;
    int removed = 0;               // left the music library (kept, as absent)
    int returned = 0;              // absent tracks whose file is back
    bool unsettled = false;        // some files were still being written; scan again soon
    bool ok = false;
    // For the import log: one entry per file that is new to the library,
    // with what was made of it (queued, skipped by its title, another file of
    // a known recording, a known track at a new place).
    QVector<QJsonObject> events;
};

// Brings the tracks table in line with the music library: every audio file
// under `libraryRoots`, and under `lookupRoots` (the video folders, inside
// them as a rule). Tracks outside every root are kept as absent (their
// videos stay). The library itself is only ever read.
//
// A file under a lookup root, or on an album asked for
// (Database::lookupAlbums), is looked up (TrackInfo::lookup): identified by
// its sound (AudioPrint), so that a file that was moved, renamed, retagged or
// converted keeps its video and import state, and files that hold the same
// recording share one lookup and one video. Any other file is indexed by its
// tags alone, for the music player (docs/import-rules.html, A12).
Result scan(const QStringList &libraryRoots, const QStringList &lookupRoots, Database &db, const std::atomic<bool> *cancel);

} // namespace LibraryScanner
