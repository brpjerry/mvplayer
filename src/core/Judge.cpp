#include "core/Judge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrl>

Judge::Judge(const QString &keyFile, QObject *parent)
    : QObject(parent)
    , m_keyFile(keyFile)
{
    QFile f(m_keyFile);
    if (f.open(QIODevice::ReadOnly))
        m_key = QString::fromUtf8(f.readAll()).trimmed();
}

QString Judge::keyHint() const
{
    if (m_key.size() < 12)
        return m_key.isEmpty() ? QString() : QStringLiteral("…");
    return m_key.left(7) + QStringLiteral("…") + m_key.right(4);
}

QString Judge::setKey(const QString &given)
{
    const QString key = given.trimmed();
    // Keys from the console: "sk-ant-" and then the kind and the key itself.
    if (!key.startsWith(QLatin1String("sk-ant-")) || key.size() < 20 || key.contains(QLatin1Char(' ')))
        return tr("That is not a Claude API key: they start with “sk-ant-”.");
    QSaveFile out(m_keyFile);
    if (!QDir().mkpath(QFileInfo(m_keyFile).absolutePath()) || !out.open(QIODevice::WriteOnly))
        return tr("Could not store the key.");
    // As good as a password: for this user only.
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (out.write(key.toUtf8()) < 0 || !out.commit())
        return tr("Could not store the key.");
    m_key = key;
    m_state.clear();
    m_status.clear();
    emit keyChanged();
    return {};
}

void Judge::removeKey()
{
    QFile::remove(m_keyFile);
    m_key.clear();
    m_state.clear();
    m_status.clear();
    emit keyChanged();
}

void Judge::check()
{
    if (m_checking || m_key.isEmpty())
        return;
    m_checking = true;
    m_state.clear();
    m_status.clear();
    emit checked();
    // The models the key may use: a request that costs nothing and says
    // whether the key is good.
    QNetworkRequest request(QUrl(apiBase() + QStringLiteral("/v1/models?limit=100")));
    request.setRawHeader("x-api-key", m_key.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    request.setTransferTimeout(20000);
    QNetworkReply *reply = m_net.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError && http == 0) {
            finish(QStringLiteral("error"), tr("Could not reach the API: %1").arg(reply->errorString()));
            return;
        }
        if (http == 401 || http == 403) {
            finish(QStringLiteral("bad"), tr("The API refuses the key: %1")
                       .arg(body.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString()));
            return;
        }
        if (http != 200) {
            const QString message = body.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString();
            finish(QStringLiteral("error"), message.isEmpty() ? tr("The API answered %1").arg(http) : message);
            return;
        }
        QStringList ids;
        for (const QJsonValue &m : body.value(QLatin1String("data")).toArray())
            ids << m.toObject().value(QLatin1String("id")).toString();
        finish(QStringLiteral("ok"), ids.isEmpty() ? tr("The key works, but no model is open to it.")
                                                   : tr("The key works: %1 models open to it, %2 among them.").arg(ids.size()).arg(ids.first()));
    });
}

void Judge::finish(const QString &state, const QString &status)
{
    m_checking = false;
    m_state = state;
    m_status = status;
    emit checked();
}
