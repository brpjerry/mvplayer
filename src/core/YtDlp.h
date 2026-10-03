#pragma once

#include "core/Matcher.h"

#include <QJsonObject>

#include <atomic>
#include <functional>

// Thin wrapper around the yt-dlp command line tool.
class YtDlp
{
public:
    YtDlp(const QString &program, const QStringList &extraArgs, const std::atomic<bool> *cancel);

    // Lists the first `count` YouTube results for `query`.
    bool search(const QString &query, int count, QVector<YtCandidate> *out, QString *error);

    // Downloads the best audio-only stream. `info` receives yt-dlp's metadata.
    bool downloadAudio(const QString &id, const QString &dir, QString *file, QJsonObject *info, QString *error);

    // Downloads the smallest video stream; used to look at the picture cheaply.
    bool downloadPreview(const QString &id, const QString &dir, QString *file, QString *error);

    // Downloads the best video stream and the thumbnail (converted to JPEG).
    bool downloadVideo(const QString &id, const QString &dir, const std::function<void(double)> &progress,
                       QString *file, QString *thumb, QString *error);

private:
    QStringList baseArgs() const;
    static QString url(const QString &id);

    QString m_program;
    QStringList m_extraArgs;
    const std::atomic<bool> *m_cancel;
};
