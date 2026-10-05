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
    int removed = 0;               // left the music folders (kept, as absent)
    int returned = 0;              // absent tracks whose file is back
    bool unsettled = false;        // some files were still being written; scan again soon
    bool ok = false;
    // For the import log: one entry per file that is new to the library,
    // with what was made of it (queued, skipped by its title, another file of
    // a known recording, a known track at a new place).
    QVector<QJsonObject> events;
};

// Brings the tracks table in line with the audio library folders `roots`.
// Tracks outside every root are dropped (their videos stay). The library
// itself is only ever read.
//
// Every track is identified by its sound (AudioPrint). A file that was moved,
// renamed, retagged or converted keeps its video and import state, and files
// that hold the same recording share one lookup and one video.
Result scan(const QStringList &roots, Database &db, const std::atomic<bool> *cancel);

} // namespace LibraryScanner
