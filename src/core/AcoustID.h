#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

// AcoustID, which knows recordings by their sound: the fingerprints people
// have submitted, each tied to the recordings on MusicBrainz that sound so
// (docs/import-rules.html, B15, B16).
namespace AcoustID {

// The score at which AcoustID is sure of a fingerprint (B15).
constexpr double kSureScore = 0.9;

// Whether AcoustID is asked at all. (Tests run without the network.)
bool enabled();

// One lookup at its web service: `form` posted, the answer as JSON. At most
// three a second, as AcoustID asks. An answer that is not "ok" (a refusal
// says why) is empty, with `error` set.
QJsonObject get(const QUrlQuery &form, QString *error);

// The recordings on MusicBrainz that AcoustID names for a fingerprint (as
// AudioPrint::pack makes it, of the whole file), where it is sure, of about
// `seconds` long (MusicBrainz::kLengthSlack). `error` says when it could
// not be asked.
QStringList recordingsOf(const QByteArray &packedPrint, double seconds, QString *error);

} // namespace AcoustID
