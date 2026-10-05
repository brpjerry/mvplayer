#pragma once

#include <cstdint>
#include <vector>

// Tells talk from music by the sound alone: a stage announcement, an
// interview, a radio segment on an album of songs.
namespace TalkCheck {

struct Result {
    bool valid = false; // long enough to say anything about
    // Share of moments far quieter than the track is on average. Speech is
    // full of gaps; music is not.
    double pauses = 0;
    // How strongly the sound repeats at a regular interval of 0.3 to 1.2
    // seconds. Music has a pulse; talk does not.
    double beat = 0;
    bool talk = false;
};

// `pcm`: mono, 16 bit, at AudioAlign::kRate. Measured on a library of 654
// tracks, 49 of them stage talk: 45 of those come out as talk, and 4 of the
// 605 others (sparse solo pieces and a radio segment). On its own that is
// not enough to drop a track; it confirms what a title says.
Result measure(const std::vector<int16_t> &pcm);

} // namespace TalkCheck
