#pragma once

#include <QString>
#include <QStringList>

// Subtitles are kept beside the video as "<video name>.<language>.srt" (or
// .ass), where the player picks them up.
namespace Subtitles {

// Converts YouTube's own subtitle format (srv3). Plain text becomes
// "<outBase>.srt", which the player styles itself; subtitles that carry
// colours, sizes or positions become "<outBase>.ass" and keep them.
// Returns the file written, or nothing: empty subtitles, or `error`.
QString convertSrv3(const QString &srv3File, const QString &outBase, QString *error);

// Of the languages a video has ("en", "en-GB", "ja-AAj-uoGhMZA"), the one
// that serves for `wanted` ("en"); empty when there is none.
QString pickLanguage(const QStringList &available, const QString &wanted);

// "<video without its extension>.<language>": what convertSrv3 is given.
QString sidecarBase(const QString &videoPath, const QString &language);
// The subtitle files that exist for a video.
QStringList sidecars(const QString &videoPath);
bool hasSidecar(const QString &videoPath, const QString &language);

} // namespace Subtitles
