#pragma once

#include <QJsonObject>
#include <QObject>
#include <QTcpServer>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <utility>

// MV Player's own Sign in with ChatGPT registration. Never reads credentials
// belonging to Codex, a browser, or another application. See judge rules X5–X6.
class ChatGptAuth : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(QString account READ account NOTIFY changed)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool signingIn READ signingIn NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QVariantList models READ models NOTIFY changed)
    Q_PROPERTY(QString model READ model WRITE setModel NOTIFY changed)
public:
    explicit ChatGptAuth(const QString &file, QObject *parent = nullptr);
    ~ChatGptAuth() override;
    static QString defaultFile();
    QVariantList accounts() const;
    QString account() const;
    bool signedIn() const;
    bool busy() const { return m_busy; }
    bool signingIn() const { return m_server.isListening(); }
    QString status() const { return m_status; }
    QVariantList models() const;
    QString model() const;
    void setModel(const QString &model);
    Q_INVOKABLE void selectAccount(const QString &id);
    // Empty id registers another account; a saved id reauthorizes that account.
    Q_INVOKABLE void signIn(const QString &id = QString());
    Q_INVOKABLE void cancelSignIn();
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void refreshModels();

    static QJsonObject readStore(const QString &file);
    static bool hasSession(const QString &file, const QString &id);
    static QJsonObject accountRecord(const QString &file, const QString &id);
    // Blocking on the caller's thread. Refresh and sign-out share a file lock,
    // including across GUI and CLI processes, to protect rotating tokens.
    static QString accessToken(const QString &file, const QString &id, QString *error,
                               const std::atomic<bool> *cancel, const QString &rejectedToken = QString());
    static QString authBase();
    static QString apiBase();
    // Pure validation, also exercised with signed test tokens and a test JWKS.
    static QJsonObject verifyIdentity(const QString &jwt, const QJsonObject &jwks,
                                     const QString &client, const QString &nonce, QString *error);
signals:
    void changed();
    void openBrowser(const QUrl &url);
    void finished(bool success);
private:
    void reload();
    void finish(const QString &error, const QString &success);
    struct OperationResult {
        QString error, notice;
        OperationResult(QString error = {}, QString notice = {}) : error(std::move(error)), notice(std::move(notice)) {}
    };
    void run(std::function<OperationResult(const std::atomic<bool> *)> work, const QString &success);
    void callback();
    QString m_file;
    QJsonObject m_store;
    QTcpServer m_server;
    QTimer m_timeout;
    QThreadPool m_pool;
    std::atomic<bool> m_cancel{false};
    bool m_busy = false;
    QString m_status, m_pendingClient, m_pendingSubject, m_state, m_nonce, m_verifier, m_redirect;
};
