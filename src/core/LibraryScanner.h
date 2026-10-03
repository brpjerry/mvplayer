#pragma once

#include "core/Database.h"

#include <atomic>

namespace LibraryScanner {

struct Result {
    QVector<qint64> queued;        // tracks that now need a video lookup
    QVector<qint64> videosChanged; // videos whose tags were refreshed from their track
    QStringList directories;       // every directory seen, for change watching
    int total = 0;
    int added = 0;
    int changed = 0;
    int removed = 0;
    bool unsettled = false;        // some files were still being written; scan again soon
    bool ok = false;
};

// Brings the tracks table in line with the audio library folders `roots`.
// Tracks outside every root are dropped (their videos stay). The library
// itself is only ever read.
Result scan(const QStringList &roots, Database &db, const std::atomic<bool> *cancel);

} // namespace LibraryScanner
