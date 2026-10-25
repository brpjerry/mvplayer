#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

// Writes tags to a file of the music library: the one thing the app changes
// there, and only at the user's hand (the tag editor; `mvplayer-import tags`).
namespace TagWriter {

// The tags the editor offers, as TagReader keys them (TagLib's property
// names, upper case), in the order they are shown. `perTrack` marks a
// track's own (its title, its number), which are not offered for several at
// once; the rest are the ones a whole album shares.
struct Field {
    const char *key;
    const char *label;
    bool perTrack;
};
const QList<Field> &fields();

// A picture embedded in a file: its bytes and their type ("image/jpeg").
struct Picture {
    QByteArray data;
    QString mime;
    // Put into a file only in place of a picture it has; a file without
    // one is left as it is (the editor replaces art, it does not add it).
    bool onlyReplace = false;
};
// The file's front cover (the first picture, where none says it is the
// front), or nothing.
std::optional<Picture> picture(const QString &path);

// Writes `changes` to the file: each key's value takes the place of what
// the file has under it, several values separated by ";" as TagReader joins
// them; an empty value removes the tag. With `picture`, it becomes the
// file's one picture, as its front cover. Nothing else in the file is
// touched. Returns false with `error` set when the file cannot be read or
// saved.
bool write(const QString &path, const QHash<QString, QString> &changes, const std::optional<Picture> &picture, QString *error);

} // namespace TagWriter
