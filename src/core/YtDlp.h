#pragma once

#include "core/Matcher.h"
#include "core/Util.h"

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

    // Lists the first `count` YouTube results for `query`.
    bool search(const QString &query, int count, QVector<YtCandidate> *out, QString *error);

    // Downloads the best audio-only stream. `info` receives yt-dlp's metadata.
    // `premium` asks with the account, for the formats only it is offered.
    bool downloadAudio(const QString &id, const QString &dir, QString *file, QJsonObject *info, QString *error,
                       bool premium = false);

    // What YouTube offers the account for a video, without downloading.
    bool premiumInfo(const QString &id, QJsonObject *info, QString *error);
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

private:
    QStringList baseArgs() const;
    QStringList accountArgs();
    // Runs yt-dlp anonymously, and once more with the account when the
    // failure says that signing in is what is missing.
    ProcResult run(const QStringList &args, const ProcOptions &opts);
    static QString url(const QString &id);

    QString m_program;
    QStringList m_extraArgs;
    QString m_cookiesFile;
    // yt-dlp writes refreshed cookies back to the file it is given, and
    // several run at once: each wrapper works on a copy of its own.
    std::unique_ptr<QTemporaryFile> m_cookiesCopy;
    const std::atomic<bool> *m_cancel;
};
