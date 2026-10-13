#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>

namespace ChatGptTransport {
struct Reply {
    int status = 0;
    QByteArray body;
    QString error;
    QJsonObject json() const;
};
// Bounded, cancellable HTTP exchange; does not follow redirects with credentials.
Reply request(const QString &url, const QByteArray &body = {}, const QString &token = {},
              bool form = false, const std::atomic<bool> *cancel = nullptr, int timeoutMs = 30000,
              const std::function<bool()> &sessionEnded = {});
QByteArray form(const QJsonObject &fields);
// A Responses stream is successful only after response.completed, never just
// because output text arrived. Handles LF/CRLF, split packets and multi-line data.
// Retains completed output items when the terminal event does not repeat them.
QJsonObject completedResponse(const QByteArray &stream, QString *error);
bool matchesSchema(const QJsonValue &value, const QJsonObject &schema);
}
