#include "ui/YtDlpUpdater.h"

#include "core/Util.h"

#include <QCryptographicHash>
#include <QDir>
#include <QNetworkReply>
#include <QSaveFile>

namespace {
const QString kReleaseUrl = QStringLiteral("https://github.com/yt-dlp/yt-dlp/releases/latest/download/");
const QString kFileName = QStringLiteral("yt-dlp.exe");
} // namespace

YtDlpUpdater::YtDlpUpdater(QObject *parent)
    : QObject(parent)
{
    connect(&m_probe, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        const QString v = QString::fromUtf8(m_probe.readAllStandardOutput()).trimmed();
        m_version = st == QProcess::NormalExit && code == 0 ? v : QString();
        emit changed();
    });
    connect(&m_probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        m_version.clear();
        emit changed();
    });
    if (supported())
        queryVersion();
}

bool YtDlpUpdater::supported() const
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

void YtDlpUpdater::queryVersion()
{
    if (m_probe.state() != QProcess::NotRunning)
        return;
    m_probe.start(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))),
                  {QStringLiteral("--version")}, QIODevice::ReadOnly);
}

void YtDlpUpdater::update()
{
    if (m_busy || !supported())
        return;
    m_busy = true;
    m_status = tr("Checking…");
    emit changed();

    // The release's checksum list first, so the download can be verified.
    QNetworkReply *reply = m_net.get(QNetworkRequest(QUrl(kReleaseUrl + QStringLiteral("SHA2-256SUMS"))));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            finish(tr("Could not reach GitHub: %1").arg(reply->errorString()));
            return;
        }
        QByteArray expected;
        for (const QByteArray &line : reply->readAll().split('\n')) {
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() == 2 && parts[1] == kFileName.toLatin1())
                expected = parts[0].toLower();
        }
        if (expected.isEmpty()) {
            finish(tr("The release has no checksum for %1").arg(kFileName));
            return;
        }
        download(expected);
    });
}

void YtDlpUpdater::download(const QByteArray &expectedHash)
{
    const QString target = toolsDir() + QLatin1Char('/') + kFileName;
    QFile current(target);
    if (current.open(QIODevice::ReadOnly)) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&current);
        if (hash.result().toHex() == expectedHash) {
            finish(tr("Already the latest version"));
            return;
        }
    }

    QNetworkReply *reply = m_net.get(QNetworkRequest(QUrl(kReleaseUrl + kFileName)));
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total <= 0)
            return;
        m_status = tr("Downloading… %1%").arg(got * 100 / total);
        emit changed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, expectedHash, target] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            finish(tr("Download failed: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray data = reply->readAll();
        if (QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != expectedHash) {
            finish(tr("The download did not match its checksum"));
            return;
        }
        QDir().mkpath(toolsDir());
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            // Typically the old copy is running.
            finish(tr("Could not replace yt-dlp: %1").arg(file.errorString()));
            return;
        }
        finish(tr("Updated"));
    });
}

void YtDlpUpdater::finish(const QString &status)
{
    m_busy = false;
    m_status = status;
    emit changed();
    queryVersion();
}
