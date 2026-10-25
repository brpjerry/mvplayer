#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

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
    // Whether the track is looked up (docs/import-rules.html, A): 1 in a
    // video folder (a ticked folder) or on an album asked for; 2 named by a
    // re-import, which no scan takes back (H9); 0 indexed by its tags for the
    // music player, not fingerprinted and not looked up (A12).
    int lookup = 1;
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

// A stretch of a video over which its library audio stream is the track's
// own audio: seconds of the video, and where the track is then (track time =
// video time - lag). Between the spans, and outside them, the stream is the
// video's own audio. Kept with the video so that a track that plays from
// its file can go on as its video from the same moment of the song.
struct AudioSpan {
    double start = 0;
    double end = 0;
    double lag = 0;
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
    // Where the library stream is the track's own audio, and where the track
    // is in it (AudioSpan). Empty: YouTube's audio, or not kept (videos from
    // before it was; reviewStart then says where the track's audio begins).
    QVector<AudioSpan> spans;

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
    // When the upload was published, "YYYY-MM-DD" (its premiere date where
    // it had one). Null: not known (videos from before it was kept).
    QString ytDate;
    QString ytDescription; // the first 600 characters of the upload's description, one line; empty: not kept
    QJsonObject tags;
    qint64 addedAt = 0;
};

// The spans as the database keeps them: "start-end@lag,…", seconds to the
// millisecond; and back. Malformed pieces are left out.
inline QString audioSpansText(const QVector<AudioSpan> &spans)
{
    QStringList parts;
    for (const AudioSpan &s : spans)
        parts << QStringLiteral("%1-%2@%3").arg(s.start, 0, 'f', 3).arg(s.end, 0, 'f', 3).arg(s.lag, 0, 'f', 3);
    return parts.join(QLatin1Char(','));
}

inline QVector<AudioSpan> audioSpansFromText(const QString &text)
{
    QVector<AudioSpan> spans;
    for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int at = part.indexOf(QLatin1Char('@'));
        const int dash = part.indexOf(QLatin1Char('-'), 1); // (a start is never negative; a lag may be)
        if (at < 0 || dash < 0 || dash > at)
            continue;
        bool ok1 = false, ok2 = false, ok3 = false;
        AudioSpan s;
        s.start = part.left(dash).toDouble(&ok1);
        s.end = part.mid(dash + 1, at - dash - 1).toDouble(&ok2);
        s.lag = part.mid(at + 1).toDouble(&ok3);
        if (ok1 && ok2 && ok3 && s.end > s.start)
            spans.append(s);
    }
    return spans;
}

// Where second `trackSec` of the track is in the video: by the span that
// holds it; in a part of the song the video leaves out, or past where the
// video has the song, at the edge of the nearest span. Without spans
// (YouTube's audio, or a video from before they were kept) the track is
// taken to start at `fallbackStart` seconds of the video.
inline double videoTimeOfTrack(const QVector<AudioSpan> &spans, double fallbackStart, double trackSec)
{
    if (spans.isEmpty())
        return qMax(0.0, trackSec + fallbackStart);
    const AudioSpan *nearest = nullptr;
    double nearestGap = 0;
    for (const AudioSpan &s : spans) {
        const double from = s.start - s.lag, to = s.end - s.lag; // the span, in seconds of the track
        if (trackSec >= from && trackSec <= to)
            return trackSec + s.lag;
        const double gap = trackSec < from ? from - trackSec : trackSec - to;
        if (!nearest || gap < nearestGap) {
            nearest = &s;
            nearestGap = gap;
        }
    }
    return qBound(nearest->start, trackSec + nearest->lag, nearest->end);
}

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
