#pragma once

#include "core/Types.h"

namespace TagReader {

// File extensions (lower case, no dot) treated as audio tracks.
const QStringList &audioExtensions();

// Fills the tag and stream fields of `out` from the file at out.path.
// Returns false when the file is not readable audio.
bool read(TrackInfo &out);

} // namespace TagReader
