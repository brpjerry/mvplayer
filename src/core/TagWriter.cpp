#include "core/TagWriter.h"

#include "core/Types.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

const QList<TagWriter::Field> &TagWriter::fields()
{
    static const QList<Field> list = {
        {"TITLE", "Title", true},
        {"ARTIST", "Artist", false},
        {"ALBUMARTIST", "Album artist", false},
        {"ALBUM", "Album", false},
        {"TRACKNUMBER", "Track number", true},
        {"DISCNUMBER", "Disc number", false},
        {"DATE", "Date", false},
        {"GENRE", "Genre", false},
        {"COMPOSER", "Composer", false},
        {"COMMENT", "Comment", false},
    };
    return list;
}

namespace {

TagLib::FileRef openFile(const QString &path)
{
#ifdef Q_OS_WIN
    const std::wstring name = QDir::toNativeSeparators(path).toStdWString();
    return TagLib::FileRef(name.c_str(), false);
#else
    return TagLib::FileRef(QFile::encodeName(path).constData(), false);
#endif
}

// The front cover among a file's pictures: the one that says so, else the first.
const TagLib::VariantMap *frontCover(const TagLib::List<TagLib::VariantMap> &pictures)
{
    const TagLib::VariantMap *best = nullptr;
    for (const TagLib::VariantMap &p : pictures) {
        if (!best)
            best = &p;
        if (p.value("pictureType").toString() == "Front Cover") {
            best = &p;
            break;
        }
    }
    return best;
}

} // namespace

std::optional<TagWriter::Picture> TagWriter::picture(const QString &path)
{
    TagLib::FileRef f = openFile(path);
    if (f.isNull())
        return std::nullopt;
    const TagLib::List<TagLib::VariantMap> pictures = f.complexProperties("PICTURE");
    const TagLib::VariantMap *best = frontCover(pictures);
    if (!best)
        return std::nullopt;
    const TagLib::ByteVector data = best->value("data").toByteVector();
    if (data.isEmpty())
        return std::nullopt;
    Picture pic;
    pic.data = QByteArray(data.data(), int(data.size()));
    pic.mime = QString::fromStdWString(best->value("mimeType").toString().toWString());
    return pic;
}

bool TagWriter::write(const QString &path, const QHash<QString, QString> &changes, const std::optional<Picture> &picture, QString *error)
{
    if (changes.isEmpty() && !picture)
        return true;
    const QFileInfo fi(path);
    if (!fi.isFile()) {
        if (error)
            *error = QStringLiteral("the file is gone");
        return false;
    }
    if (!fi.isWritable()) {
        if (error)
            *error = QStringLiteral("the file is read-only");
        return false;
    }
    TagLib::FileRef f = openFile(path);
    if (f.isNull() || !f.tag()) {
        if (error)
            *error = QStringLiteral("not a tagged audio file");
        return false;
    }
    TagLib::PropertyMap props = f.properties();
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        const TagLib::String key(it.key().toUpper().toStdWString());
        const QStringList values = splitMulti(it.value());
        if (values.isEmpty()) {
            props.erase(key);
            continue;
        }
        TagLib::StringList list;
        for (const QString &v : values)
            list.append(TagLib::String(v.toStdWString()));
        props.replace(key, list);
    }
    // ALBUMARTIST is the one name for it; a file that carries the tag under
    // the other spelling as well would otherwise keep the old value there,
    // which TagReader reads where the first is empty.
    if (changes.contains(QStringLiteral("ALBUMARTIST")))
        props.erase("ALBUM ARTIST");
    f.setProperties(props);
    if (picture && (!picture->onlyReplace || !f.complexProperties("PICTURE").isEmpty())) {
        TagLib::VariantMap pic;
        pic.insert("data", TagLib::ByteVector(picture->data.constData(), uint(picture->data.size())));
        pic.insert("mimeType", TagLib::String(picture->mime.toStdWString()));
        pic.insert("pictureType", TagLib::String("Front Cover"));
        pic.insert("description", TagLib::String(""));
        f.setComplexProperties("PICTURE", {pic});
    }
    if (!f.save()) {
        if (error)
            *error = QStringLiteral("the file could not be saved");
        return false;
    }
    return true;
}
