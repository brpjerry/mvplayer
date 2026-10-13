#include "core/ChatGptTransport.h"
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <cmath>
#include <utility>

namespace ChatGptTransport {
QJsonObject Reply::json() const { return QJsonDocument::fromJson(body).object(); }

Reply request(const QString &url, const QByteArray &body, const QString &token,
              bool asForm, const std::atomic<bool> *cancel, int timeoutMs, const std::function<bool()> &sessionEnded)
{
    Reply result;
    if (cancel && *cancel) { result.error = QStringLiteral("Cancelled."); return result; }
    QNetworkAccessManager net;
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(timeoutMs);
    if (!token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  asForm ? QStringLiteral("application/x-www-form-urlencoded") : QStringLiteral("application/json"));
    QNetworkReply *reply = body.isNull() ? net.get(req) : net.post(req, body);
    QEventLoop loop;
    QTimer poll, deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(&poll, &QTimer::timeout, reply, [&] { if ((cancel && *cancel) || (sessionEnded && sessionEnded())) reply->abort(); });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        result.body += reply->readAll();
        if (result.body.size() > 16 * 1024 * 1024) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    poll.start(100);
    deadline.start(timeoutMs);
    loop.exec();
    if (reply->isReadable()) result.body += reply->readAll();
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // Never echo a response body, URL or token in an error. Servers may echo
    // request parameters, including the authorization code, on a bad request.
    if (cancel && *cancel) result.error = QStringLiteral("Cancelled.");
    else if (sessionEnded && sessionEnded()) result.error = QStringLiteral("ChatGPT session ended.");
    else if (reply->error() != QNetworkReply::NoError || result.status != 200)
        result.error = QStringLiteral("ChatGPT request failed (HTTP %1, network error %2).")
                           .arg(result.status).arg(int(reply->error()));
    delete reply;
    return result;
}

QByteArray form(const QJsonObject &fields)
{
    // QUrlQuery leaves '+' literal; application/x-www-form-urlencoded does not.
    QByteArray out;
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        if (!out.isEmpty()) out += '&';
        out += QUrl::toPercentEncoding(it.key()) + '=' + QUrl::toPercentEncoding(it.value().toString());
    }
    return out;
}

QJsonObject completedResponse(const QByteArray &stream, QString *error)
{
    error->clear();
    QJsonObject completed;
    QMap<int, QJsonObject> outputItems;
    QByteArray data;
    bool failed = false;
    auto event = [&] {
        if (data.isEmpty()) return;
        if (data.endsWith('\n')) data.chop(1);
        if (data == "[DONE]") { data.clear(); return; }
        QJsonParseError parse;
        const QJsonObject e = QJsonDocument::fromJson(data, &parse).object();
        data.clear();
        const QString type = e.value("type").toString();
        if (parse.error != QJsonParseError::NoError || e.isEmpty()) { failed = true; return; }
        if (type == QLatin1String("response.output_item.done")) {
            const int index = e.value("output_index").toInt(-1);
            const auto item = e.value("item").toObject();
            if (index < 0 || item.isEmpty() || outputItems.contains(index) || !completed.isEmpty()) failed = true;
            else outputItems.insert(index, item);
        } else if (type == QLatin1String("response.completed")) {
            if (!completed.isEmpty()) failed = true;
            completed = e.value("response").toObject();
            if (completed.value("status").toString() != QLatin1String("completed")) failed = true;
        } else if (type == QLatin1String("error") || type == QLatin1String("response.failed")
                   || type == QLatin1String("response.incomplete")) failed = true;
    };
    auto lines = stream.split('\n');
    // split() adds a synthetic empty element after the final newline.
    if (stream.endsWith('\n')) lines.removeLast();
    for (QByteArray line : lines) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) { event(); continue; }
        if (line.startsWith("data:")) {
            line.remove(0, 5);
            if (line.startsWith(' ')) line.remove(0, 1);
            data += line + '\n';
        }
    }
    // An unterminated SSE event is incomplete, even if it looks like valid JSON.
    if (!data.isEmpty() || failed || completed.isEmpty()) {
        *error = QStringLiteral("ChatGPT did not return a completed response.");
        return {};
    }
    // Some streaming responses carry their messages only in output_item.done;
    // the terminal event supplies completion and usage without repeating them.
    // Keep the final item snapshots in output order, never text deltas (which
    // could otherwise duplicate the answer). Still require successful completion.
    if (completed.value("output").toArray().isEmpty() && !outputItems.isEmpty()) {
        QJsonArray output;
        for (const auto &item : std::as_const(outputItems)) output.append(item);
        completed.insert("output", output);
    }
    return completed;
}

bool matchesSchema(const QJsonValue &v, const QJsonObject &s)
{
    const QString type = s.value("type").toString();
    if (type == QLatin1String("object")) {
        if (!v.isObject()) return false;
        const auto object = v.toObject();
        const auto properties = s.value("properties").toObject();
        for (const auto &required : s.value("required").toArray())
            if (!object.contains(required.toString())) return false;
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!properties.contains(it.key())) {
                if (s.value("additionalProperties") == QJsonValue(false)) return false;
            } else if (!matchesSchema(it.value(), properties.value(it.key()).toObject())) return false;
        }
    } else if (type == QLatin1String("array")) {
        if (!v.isArray()) return false;
        for (const auto &item : v.toArray())
            if (!matchesSchema(item, s.value("items").toObject())) return false;
    } else if (type == QLatin1String("string") && !v.isString()) return false;
    else if (type == QLatin1String("boolean") && !v.isBool()) return false;
    else if (type == QLatin1String("number") && !v.isDouble()) return false;
    else if (type == QLatin1String("integer") && (!v.isDouble() || std::floor(v.toDouble()) != v.toDouble())) return false;
    if (s.contains("enum") && !s.value("enum").toArray().contains(v)) return false;
    return true;
}
}
