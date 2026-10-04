#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>

// Keeps the application's own copy of yt-dlp current on Windows, where no
// package manager does it: downloads the latest release into toolsDir().
// YouTube changes often enough that an old yt-dlp stops working.
class YtDlpUpdater : public QObject
{
    Q_OBJECT
    // False where yt-dlp comes from the system's packages.
    Q_PROPERTY(bool supported READ supported CONSTANT)
    // Version of the yt-dlp that imports use; empty when there is none.
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // Progress or the result of the last update.
    Q_PROPERTY(QString status READ status NOTIFY changed)

public:
    explicit YtDlpUpdater(QObject *parent = nullptr);

    bool supported() const;
    QString version() const { return m_version; }
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }

    Q_INVOKABLE void update();

signals:
    void changed();

private:
    void queryVersion();
    void download(const QByteArray &expectedHash);
    void finish(const QString &status);

    QNetworkAccessManager m_net;
    QProcess m_probe;
    QString m_version;
    QString m_status;
    bool m_busy = false;
};
