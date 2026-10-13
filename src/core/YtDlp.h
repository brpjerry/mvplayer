#pragma once

#include "core/Matcher.h"
#include "core/Util.h"

#include <QHash>
#include <QJsonObject>

#include <atomic>
#include <functional>
#include <memory>

class QTemporaryFile;

// Thin wrapper around the yt-dlp command line tool.
//
// With the cookies of a YouTube Premium account (a cookies.txt exported from
// the browser) YouTube offers audio at about twice the usual bitrate. The
// account is used for that and for videos that cannot be had without signing
// in, and for nothing else: searching and ordinary downloads stay anonymous,
// which also keeps the "1080p Premium" picture that signed-in clients are
// not offered.
//
// YouTube can refuse a connection's guests altogether ("Sign in to confirm
// you're not a bot"): every client, every video, for days. There is nothing
// to solve; what it asks for is the sign-in. With `accountOnBotCheck` a
// request refused that way is made again with the account, and while guests
// are refused the downloads go to the account straight away. That is for
// the user to switch on: an account that downloads a great deal is one
// YouTube may restrict.
class YtDlp
{
public:
    YtDlp(const QString &program, const QStringList &extraArgs, const QString &cookiesFile,
          const std::atomic<bool> *cancel, bool accountOnBotCheck = false);
    ~YtDlp();

    bool hasCookies() const { return !m_cookiesFile.isEmpty(); }

    // A folder to remember things in between tracks: the same video is a
    // candidate for the single, the album cut and the live take of a song.
    //  - search results, for a day;
    //  - each video's audio and description, so that it is fetched once, and
    //    the page is not opened again for its picture and its download while
    //    the addresses in the description are good (a couple of hours);
    //  - what a caller has found out about a video (see note()).
    // Without one, nothing is remembered.
    void setCacheDir(const QString &dir) { m_cacheDir = dir; }
    // Something found out about a video, by name; empty when not known.
    QJsonObject note(const QString &id, const QString &name) const;
    void setNote(const QString &id, const QString &name, const QJsonObject &value) const;
    // How many requests the cache has answered for this wrapper.
    int cacheHits() const { return m_cacheHits; }
    // How many requests were made again with the account after the
    // anonymous one was refused for wanting a sign-in.
    int accountRequests() const { return m_accountRequests; }
    // Drops what is older than `maxAgeDays`, then the oldest until the
    // folder is under `maxBytes`. For when nothing is being imported.
    static void pruneCache(const QString &dir, qint64 maxBytes, int maxAgeDays);

    // ---- Keeping everything, and replaying it ------------------------------
    // With a capture folder set, everything yt-dlp is asked and everything
    // it brings is kept there for good (README, "Keeping everything"):
    // search/<key>.json with search/index.jsonl, and video/<id>/ with the
    // audio and its page (audio.info.json), page.json, preview, video with
    // its thumbnail, subtitles and the account's premium answers. For every
    // wrapper in the process; empty: nothing is kept.
    static void setCapture(const QString &dir);
    // A replay answers from that folder wherever it has the item, and asks
    // yt-dlp as usual where it has not (keeping that too, with a capture on).
    static void setReplay(const QString &dir);
    static QString captureDir();
    static QString replayDir();

    // Lists the first `count` YouTube results for `query`. `fresh` asks
    // YouTube whatever the cache has (the probe, B5).
    bool search(const QString &query, int count, QVector<YtCandidate> *out, QString *error, bool fresh = false);

    // Downloads the best audio-only stream. `info` receives yt-dlp's metadata.
    // `premium` asks with the account, for the formats only it is offered.
    bool downloadAudio(const QString &id, const QString &dir, QString *file, QJsonObject *info, QString *error,
                       bool premium = false);

    // What YouTube offers the account for a video, without downloading.
    bool premiumInfo(const QString &id, QJsonObject *info, QString *error);
    // What the upload's page says, with nothing downloaded: its channel and
    // that channel's subscribers among the rest. From what an earlier look
    // left in the cache, where there is something.
    bool pageInfo(const QString &id, QJsonObject *info, QString *error);
    // The channel an upload is on, as its page (the info that comes with its
    // audio) has it. `listedAs`: the channel as the search results name it,
    // "A and B" for an upload YouTube lists under several channels.
    struct Channel {
        QString id;
        QString name;
        // -1: none to go by. The channel hides it; or the upload is listed
        // under several channels, where the page gives the first one's in
        // words ("7.43 million", which reads as 743) and the others' not at all.
        qint64 subscribers = -1;
        bool verified = false;
        bool several = false;
        // "@name" as the page has it; empty (never null) where it gives none.
        QString handle;
    };
    static Channel channelOf(const QJsonObject &info, const QString &listedAs = QString());
    // How a channel is named for good: its handle ("@name", in lower case:
    // YouTube does not tell cases apart) or its id ("UC…"), out of either of
    // them as typed or out of the address of the channel's page. Empty where
    // `typed` is neither: a channel's name will not do, two can have the same.
    static QString channelRef(const QString &typed);

    // The video's own subtitles (not the machine-made ones) in YouTube's
    // format, for the languages wanted: language wanted -> file in `dir`.
    // None is not an error.
    bool downloadSubtitles(const QString &id, const QString &dir, const QStringList &languages,
                           QHash<QString, QString> *files, QString *error);

    // Whether the account's cookies still work, asked of one video.
    enum class Account { Premium, Ordinary, Expired, Unknown };
    Account checkAccount(const QString &id, double *kbps, QString *error);
    // From yt-dlp's metadata: the best audio bitrate on offer (kbit/s), the
    // tallest picture, and the bitrate of the format that was downloaded.
    static double bestAudioKbps(const QJsonObject &info);
    static int bestHeight(const QJsonObject &info);
    static double audioKbps(const QJsonObject &info);
    // When the upload was published, "YYYY-MM-DD": its premiere date where
    // it had one, else its upload date. Null where the page gives neither.
    static QString publishedOn(const QJsonObject &info);
    // The upload's description as the library keeps it: its first 600
    // characters on one line (lines joined with " | "), for the judge (V1).
    static QString descriptionOf(const QJsonObject &info);

    // True for a cookies.txt in the Netscape format with YouTube cookies in it.
    static bool looksLikeCookies(const QString &file);

    // Downloads the smallest video stream; used to look at the picture cheaply.
    bool downloadPreview(const QString &id, const QString &dir, QString *file, QString *error);

    // Downloads the best video stream and the thumbnail (converted to JPEG).
    bool downloadVideo(const QString &id, const QString &dir, const std::function<void(double)> &progress,
                       QString *file, QString *thumb, QString *error);

    // True when an error message means YouTube is refusing this client
    // altogether (rate limit, bot check) rather than one video failing.
    static bool looksBlocked(const QString &error);
    // ... and when what it wants of this client is a sign-in.
    static bool looksBotCheck(const QString &error);
    // True when an error message says the video itself is gone or withheld
    // (removed, private, not available here): asking again will not help.
    static bool looksUnavailable(const QString &error);
    // True when an error message says the account's cookies are no longer
    // accepted: browsers rotate them, and an export goes stale within days.
    static bool cookiesExpired(const QString &error);

private:
    QStringList baseArgs() const;
    QStringList accountArgs();
    // Runs yt-dlp anonymously, and once more with the account when the
    // failure says that signing in is what is missing.
    ProcResult run(const QStringList &args, const ProcOptions &opts);
    ProcResult runWithAccount(const QStringList &account, const QStringList &args, const ProcOptions &opts);
    static QString url(const QString &id);
    QString cacheEntry(const QString &id) const; // the video's folder in the cache, or empty
    // The video's description from the cache, while its addresses are good.
    QString freshInfo(const QString &id) const;
    // Runs a download from the remembered description; from the page when
    // there is none or it no longer works.
    ProcResult runFor(const QString &id, const QStringList &args, const ProcOptions &opts);

    // The replay's copy of a video's file named `base` into `dir`; empty when it has none.
    static QString replayFile(const QString &id, const QString &base, const QString &dir, const QStringList &skipSuffixes);
    // Keeps a file the lookup brought under the capture's folder for the video.
    static void keepFile(const QString &id, const QString &file);
    static QString videoFolder(const QString &root, const QString &id);

    QString m_program;
    QStringList m_extraArgs;
    QString m_cookiesFile;
    bool m_accountOnBotCheck = false;
    // yt-dlp writes refreshed cookies back to the file it is given, and
    // several run at once: each wrapper works on a copy of its own.
    std::unique_ptr<QTemporaryFile> m_cookiesCopy;
    const std::atomic<bool> *m_cancel;
    QString m_cacheDir;
    int m_cacheHits = 0;
    int m_accountRequests = 0;
};
