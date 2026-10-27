#include "core/AcoustID.h"

#include "core/MusicBrainz.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>

namespace {

// The key AcoustID gave MV Player as an application. It names the
// application, it is not a secret: every copy of the program sends it.
const char *kClient = "CgMnrZM9J6";

// AcoustID asks for no more than three requests a second.
QMutex g_pace;
QElapsedTimer g_last;

} // namespace

namespace AcoustID {

// One lookup: the form posted, the answer as JSON.
QJsonObject get(const QUrlQuery &form, QString *error)
{
    {
        QMutexLocker lock(&g_pace);
        if (g_last.isValid() && g_last.elapsed() < 340)
            QThread::msleep(340 - g_last.elapsed());
        g_last.restart();
    }
    // (Tests stand a server of their own in for it.)
    QNetworkRequest req(QUrl(qEnvironmentVariable("MVPLAYER_ACOUSTID_URL", QStringLiteral("https://api.acoustid.org/v2/lookup"))));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("mvplayer/%1 (https://github.com/brpjerry/mvplayer)").arg(QCoreApplication::applicationVersion()));
    QNetworkAccessManager net;
    QNetworkReply *reply = net.post(req, form.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QJsonObject out;
    if (!reply->isFinished()) {
        *error = QStringLiteral("timed out");
    } else {
        // A refusal (a bad key, a malformed fingerprint) comes as JSON too.
        out = QJsonDocument::fromJson(reply->readAll()).object();
        if (out.value(QLatin1String("status")).toString() != QLatin1String("ok")) {
            const QString said = out.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString();
            *error = !said.isEmpty() ? said : reply->error() != QNetworkReply::NoError ? reply->errorString() : QStringLiteral("no answer");
            out = {};
        }
    }
    reply->abort();
    reply->deleteLater();
    return out;
}

bool enabled()
{
    return !qEnvironmentVariableIsSet("MVPLAYER_NO_ACOUSTID");
}

QStringList recordingsOf(const QByteArray &packedPrint, double seconds, QString *error)
{
    if (packedPrint.isEmpty() || seconds <= 0)
        return {};
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("client"), QString::fromLatin1(kClient));
    form.addQueryItem(QStringLiteral("duration"), QString::number(qRound(seconds)));
    // As Chromaprint writes it out: URL-safe, unpadded.
    form.addQueryItem(QStringLiteral("fingerprint"),
                      QString::fromLatin1(packedPrint.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    form.addQueryItem(QStringLiteral("meta"), QStringLiteral("recordings"));
    form.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    const QJsonObject answer = get(form, error);
    QStringList out;
    for (const QJsonValue &v : answer.value(QLatin1String("results")).toArray()) {
        const QJsonObject result = v.toObject();
        if (result.value(QLatin1String("score")).toDouble() < kSureScore)
            continue;
        for (const QJsonValue &r : result.value(QLatin1String("recordings")).toArray()) {
            const QJsonObject recording = r.toObject();
            const QString id = recording.value(QLatin1String("id")).toString();
            const double length = recording.value(QLatin1String("duration")).toDouble();
            if (!id.isEmpty() && (length <= 0 || std::abs(length - seconds) <= MusicBrainz::kLengthSlack) && !out.contains(id))
                out << id;
        }
    }
    return out;
}

} // namespace AcoustID
