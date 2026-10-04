#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <atomic>

// Identifies a recording by how it sounds, so a music file is recognised
// again after it was moved, renamed, retagged or converted to another format,
// and two files holding the same recording share one music video.
namespace AudioPrint {

using Print = QVector<quint32>; // Chromaprint items, about eight per second

// Fingerprints the whole first audio stream of `file`.
bool ofFile(const QString &file, Print *print, const std::atomic<bool> *cancel, QString *error);

// Share of bits that differ between two prints at their best alignment:
// about 0.01 for a lossy copy of the same file, 0.5 for unrelated audio.
double distance(const Print &a, const Print &b);

// The same recording from start to end. A short edit is not the same as the
// full version, and neither is an instrumental or a live take.
bool sameRecording(const Print &a, const Print &b);

// Compact form for storage.
QByteArray pack(const Print &print);
Print unpack(const QByteArray &packed);

} // namespace AudioPrint
