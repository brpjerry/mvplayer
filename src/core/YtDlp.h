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
class YtDlp
{
public:
    YtDlp(const QString &program, const QStringList &extraArgs, const QString &cookiesFile,
          const std::atomic<bool> *cancel);
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
    // Drops what is older than `maxAgeDays`, then the oldest until the
    // folder is under `maxBytes`. For when nothing is being imported.
    static void pruneCache(const QString &dir, qint64 maxBytes, int maxAgeDays);

    // Lists the first `count` YouTube results for `query`.
    bool search(const QString &query, int count, QVector<YtCandidate> *out, QString *error);

    // Downloads the best audio-only stream. `info` receives yt-dlp's metadata.
    // `premium` asks with the account, for the formats only it is offered.
    bool downloadAudio(const QString &id, const QString &dir, QString *file, QJsonObject *info, QString *error,
                       bool premium = false);

    // What YouTube offers the account for a video, without downloading.
    bool premiumInfo(const QString &id, QJsonObject *info, QString *error);

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
    static QString url(const QString &id);
    QString cacheEntry(const QString &id) const; // the video's folder in the cache, or empty
    // The video's description from the cache, while its addresses are good.
    QString freshInfo(const QString &id) const;
    // Runs a download from the remembered description; from the page when
    // there is none or it no longer works.
    ProcResult runFor(const QString &id, const QStringList &args, const ProcOptions &opts);

    QString m_program;
    QStringList m_extraArgs;
    QString m_cookiesFile;
    // yt-dlp writes refreshed cookies back to the file it is given, and
    // several run at once: each wrapper works on a copy of its own.
    std::unique_ptr<QTemporaryFile> m_cookiesCopy;
    const std::atomic<bool> *m_cancel;
    QString m_cacheDir;
    int m_cacheHits = 0;
};
