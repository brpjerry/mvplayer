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

    // Files that hold the same recording (see AudioPrint) share this number
    // and with it one lookup and one video. 0: not identified yet.
    qint64 recording = 0;

    // Import state: pending | done | not_found | failed | skipped
    QString state = QStringLiteral("pending");
    qint64 videoId = 0;
    int attempts = 0;
    qint64 lastAttempt = 0;
    QString message;
    // Its file is in none of the music folders any more. The track is kept
    // with what is known about it, and takes that up again if the file
    // comes back; until then it is in no count and is not looked up.
    bool absent = false;
    // Asked to be looked up again although it has a video: that video is
    // judged by today's rules as one candidate among the search results,
    // and kept, sent to review, replaced or let go as they decide.
    bool reimport = false;
};

inline double trackQuality(const TrackInfo &t)
{
    if (t.lossless)
        return 1e6;
    double eff = 1.0;
    if (t.codec == QLatin1String("opus"))
        eff = 1.5;
    else if (t.codec == QLatin1String("aac") || t.codec == QLatin1String("vorbis"))
        eff = 1.2;
    return t.bitrate * eff;
}

// Of several files with the same recording, the one whose audio and tags
// stand for it: the best quality, then the one known longest.
inline bool betterSource(const TrackInfo &a, const TrackInfo &b)
{
    const double qa = trackQuality(a), qb = trackQuality(b);
    if (qa != qb)
        return qa > qb;
    const qint64 ra = qint64(a.bitsPerSample) * a.sampleRate, rb = qint64(b.bitsPerSample) * b.sampleRate;
    if (ra != rb)
        return ra > rb;
    return a.id < b.id;
}

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
    double ytAbr = 0;     // bitrate of the YouTube audio in the file, kbit/s; 0: not recorded

    // Waiting for the user to say whether this is the track's video: the
    // fingerprints say it is the song, the waveforms cannot confirm the
    // recording. It carries the track's audio and YouTube's to compare; the
    // track's sits between these two times of the video.
    bool review = false;
    double reviewStart = 0;
    double reviewEnd = 0;
    // Several videos can wait as options for the same tracks: they share this
    // number, and the tracks point at one of them.
    qint64 reviewGroup = 0;

    QString ytTitle;
    QString ytChannel;
    QString ytChannelId; // UC…; empty for videos from before it was kept
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
