#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

// A file in the (read-only) audio library.
struct TrackInfo {
    qint64 id = 0;
    QString path;
    qint64 mtime = 0;
    qint64 size = 0;

    QString title;
    QString artist;       // multi-valued tags are joined with "; "
    QString albumArtist;
    QString album;
    QString genre;
    int year = 0;
    int trackNo = 0;
    int discNo = 0;

    double duration = 0;  // seconds
    QString codec;
    int bitrate = 0;      // kbit/s
    int sampleRate = 0;
    int bitsPerSample = 0;
    int channels = 0;
    bool lossless = false;

    QJsonObject tags;     // every tag in the file, key -> "a; b"

    // Import state: pending | done | not_found | failed | skipped
    QString state = QStringLiteral("pending");
    qint64 videoId = 0;
    int attempts = 0;
    qint64 lastAttempt = 0;
    QString message;
};

// A music video in the MV library.
struct VideoInfo {
    qint64 id = 0;
    QString ytId;
    QString path;
    QString thumb;

    QString title;
    QString artist;
    QString albumArtist;
    QString album;
    QString genre;
    int year = 0;
    int trackNo = 0;

    double duration = 0;
    int width = 0;
    int height = 0;
    double fps = 0;
    QString vcodec;
    QString audioSource;  // "library" when local audio was muxed in, else "youtube"
    QString audioDetail;  // e.g. "FLAC 24/48"

    QString ytTitle;
    QString ytChannel;
    QJsonObject tags;
    qint64 addedAt = 0;
};

inline QStringList splitMulti(const QString &s)
{
    QStringList out;
    for (const QString &part : s.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString t = part.trimmed();
        if (!t.isEmpty() && !out.contains(t))
            out << t;
    }
    return out;
}
