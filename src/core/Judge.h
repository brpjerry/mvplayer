#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

// Claude, asked through the API: the account's key, kept in a file of this
// user's alone, and whether it works. The judging of the review queue that
// it is for is not here yet.
class Judge : public QObject
{
    Q_OBJECT
public:
    // `keyFile`: where the key is kept (read at once, if it is there).
    explicit Judge(const QString &keyFile, QObject *parent = nullptr);

    bool hasKey() const { return !m_key.isEmpty(); }
    // The key with its middle left out, for showing: "sk-ant-…x4Yz".
    QString keyHint() const;
    // Stores the key; empty: the reason it was not.
    QString setKey(const QString &key);
    void removeKey();

    // Asks the API which models the key may use: checked() says.
    void check();
    bool checking() const { return m_checking; }
    // After a check: "ok", "bad" (the key is refused) or "error" (the
    // request did not get through); empty before one.
    QString state() const { return m_state; }
    QString status() const { return m_status; }

    static QString apiBase() { return QStringLiteral("https://api.anthropic.com"); }

signals:
    void keyChanged();
    void checked();

private:
    void finish(const QString &state, const QString &status);

    QString m_keyFile;
    QString m_key;
    QNetworkAccessManager m_net;
    bool m_checking = false;
    QString m_state;
    QString m_status;
};
