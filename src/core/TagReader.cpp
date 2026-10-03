#include "core/TagReader.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <taglib/aiffproperties.h>
#include <taglib/apeproperties.h>
#include <taglib/dsfproperties.h>
#include <taglib/fileref.h>
#include <taglib/flacproperties.h>
#include <taglib/mp4properties.h>
#include <taglib/mpegproperties.h>
#include <taglib/opusproperties.h>
#include <taglib/tpropertymap.h>
#include <taglib/trueaudioproperties.h>
#include <taglib/vorbisproperties.h>
#include <taglib/wavpackproperties.h>
#include <taglib/wavproperties.h>

namespace {

QString toQ(const TagLib::String &s)
{
    return QString::fromStdWString(s.toWString());
}

// Tags often pack several values into one field ("A;B") as well as using
// repeated fields. Normalise both to "A; B".
QString joinValues(const TagLib::StringList &values)
{
    QStringList parts;
    for (const auto &v : values) {
        for (const QString &p : splitMulti(toQ(v))) {
            if (!parts.contains(p))
                parts << p;
        }
    }
    return parts.join(QStringLiteral("; "));
}

int leadingInt(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("(\\d+)"));
    const auto m = re.match(s);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

int yearOf(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("(\\d{4})"));
    const auto m = re.match(s);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

} // namespace

const QStringList &TagReader::audioExtensions()
{
    static const QStringList exts = {
        QStringLiteral("flac"), QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("ogg"), QStringLiteral("oga"), QStringLiteral("opus"), QStringLiteral("wav"),
        QStringLiteral("aiff"), QStringLiteral("aif"), QStringLiteral("ape"), QStringLiteral("wv"),
        QStringLiteral("tta"), QStringLiteral("dsf"), QStringLiteral("wma"), QStringLiteral("mka"),
    };
    return exts;
}

bool TagReader::read(TrackInfo &out)
{
    TagLib::FileRef f(QFile::encodeName(out.path).constData(), true, TagLib::AudioProperties::Average);
    if (f.isNull() || !f.audioProperties())
        return false;

    const TagLib::PropertyMap props = f.properties();
    QJsonObject tags;
    for (auto it = props.begin(); it != props.end(); ++it) {
        const QString value = joinValues(it->second);
        if (!value.isEmpty())
            tags.insert(toQ(it->first).toUpper(), value);
    }
    out.tags = tags;

    auto tag = [&](const char *key) { return tags.value(QLatin1String(key)).toString(); };
    out.title = tag("TITLE");
    out.artist = tag("ARTIST");
    out.albumArtist = tag("ALBUMARTIST");
    if (out.albumArtist.isEmpty())
        out.albumArtist = tag("ALBUM ARTIST");
    out.album = tag("ALBUM");
    out.genre = tag("GENRE");
    out.year = yearOf(tag("DATE"));
    if (!out.year)
        out.year = yearOf(tag("YEAR"));
    if (!out.year)
        out.year = yearOf(tag("ORIGINALDATE"));
    out.trackNo = leadingInt(tag("TRACKNUMBER"));
    out.discNo = leadingInt(tag("DISCNUMBER"));

    if (out.title.isEmpty())
        out.title = QFileInfo(out.path).completeBaseName();
    if (out.albumArtist.isEmpty())
        out.albumArtist = splitMulti(out.artist).value(0);

    const TagLib::AudioProperties *ap = f.audioProperties();
    out.duration = ap->lengthInMilliseconds() / 1000.0;
    out.bitrate = ap->bitrate();
    out.sampleRate = ap->sampleRate();
    out.channels = ap->channels();
    out.bitsPerSample = 0;
    out.lossless = false;
    out.codec = QFileInfo(out.path).suffix().toLower();

    if (auto p = dynamic_cast<const TagLib::FLAC::Properties *>(ap)) {
        out.codec = QStringLiteral("flac");
        out.bitsPerSample = p->bitsPerSample();
        out.lossless = true;
    } else if (auto p = dynamic_cast<const TagLib::RIFF::WAV::Properties *>(ap)) {
        out.codec = QStringLiteral("wav");
        out.bitsPerSample = p->bitsPerSample();
        out.lossless = true;
    } else if (auto p = dynamic_cast<const TagLib::RIFF::AIFF::Properties *>(ap)) {
        out.codec = QStringLiteral("aiff");
        out.bitsPerSample = p->bitsPerSample();
        out.lossless = true;
    } else if (auto p = dynamic_cast<const TagLib::APE::Properties *>(ap)) {
        out.codec = QStringLiteral("ape");
        out.bitsPerSample = p->bitsPerSample();
        out.lossless = true;
    } else if (auto p = dynamic_cast<const TagLib::WavPack::Properties *>(ap)) {
        out.codec = QStringLiteral("wavpack");
        out.bitsPerSample = p->bitsPerSample();
        out.lossless = p->isLossless();
    } else if (auto p = dynamic_cast<const TagLib::TrueAudio::Properties *>(ap)) {
        out.codec = QStringLiteral("tta");
        out.bitsPerSample = p->bitsPerSample();
        out.lossless = true;
    } else if (auto p = dynamic_cast<const TagLib::DSF::Properties *>(ap)) {
        out.codec = QStringLiteral("dsd");
        out.bitsPerSample = p->bitsPerSample();
        out.lossless = true;
    } else if (auto p = dynamic_cast<const TagLib::MP4::Properties *>(ap)) {
        const bool alac = p->codec() == TagLib::MP4::Properties::ALAC;
        out.codec = alac ? QStringLiteral("alac") : QStringLiteral("aac");
        out.bitsPerSample = alac ? p->bitsPerSample() : 0;
        out.lossless = alac;
    } else if (dynamic_cast<const TagLib::MPEG::Properties *>(ap)) {
        out.codec = QStringLiteral("mp3");
    } else if (dynamic_cast<const TagLib::Ogg::Opus::Properties *>(ap)) {
        out.codec = QStringLiteral("opus");
    } else if (dynamic_cast<const TagLib::Vorbis::Properties *>(ap)) {
        out.codec = QStringLiteral("vorbis");
    }
    return out.duration > 0;
}
