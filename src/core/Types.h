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

    // Import state: pending | done | review | not_found | failed | skipped.
    // review: it has no video, and is a candidate of videos that wait for
    // the user's verdict (Database::candidacies).
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

    // Waiting for the user to say whose video this is, if anyone's: the
    // fingerprints say it is the song, the waveforms cannot confirm the
    // recording. The tracks it could belong to are its candidates
    // (Database::candidates); its file carries the audio of each of them that
    // is the better audio, then YouTube's. The first candidate's audio sits
    // between these two times of the video.
    bool review = false;
    // The user accepted this upload as its track's video: it counts as on
    // the artist's channel, whoever's channel it is on. Never set on a video
    // under review.
    bool vouched = false;
    double reviewStart = 0;
    double reviewEnd = 0;
    // Which of the file's audio streams is YouTube's, from 0: the ones before
    // it are the library's.
    int ytStream = 0;

    QString ytTitle;
    QString ytChannel;
    QString ytChannelId; // UC…; empty for videos from before it was kept
    // What the upload's page said of its channel when the video was brought
    // in, or when the page was last read for it: how many subscribers it
    // has, and whether YouTube marks it as verified. -2: the page was never
    // read for it (videos from before this was kept); -1: read, and there is
    // no count to go by (the channel hides it, or YouTube lists the upload
    // under several channels).
    qint64 ytChannelSubs = -2;
    bool ytChannelVerified = false;
    // The channel's handle as its page has it ("@name"), the other way
    // besides its id that an exception to "Ignore small channels" names it.
    // Null: never kept (the page was read before handles were); empty: the
    // page gave none.
    QString ytChannelHandle;
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
