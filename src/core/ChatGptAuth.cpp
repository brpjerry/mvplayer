#include "core/ChatGptAuth.h"
#include "core/ChatGptTransport.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QTcpSocket>
#include <QThread>
#include <QUrlQuery>
#include <QUuid>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <memory>

using namespace ChatGptTransport;
namespace {
const QString resource = QStringLiteral("https://api.openai.com/v1");
const QString planScope = QStringLiteral("chatgpt.tokens.use.direct");
QString endpoint(const char *env, const QString &production)
{
    const QString override = qEnvironmentVariable(env);
    if (override.isEmpty()) return production;
    const QUrl u(override);
    // Test endpoints cannot turn a typo into sending account tokens elsewhere.
    if (u.scheme() != QLatin1String("http") || (u.host() != QLatin1String("127.0.0.1") && u.host() != QLatin1String("::1"))
        || !u.userInfo().isEmpty() || u.hasQuery() || u.hasFragment()) return {};
    QString base = override;
    while (base.endsWith('/')) base.chop(1);
    return base;
}
bool stopped(const std::atomic<bool> *cancel) { return cancel && *cancel; }
QString randomValue()
{
    QByteArray bytes(32, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), bytes.size()) != 1) return {};
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
bool saveStore(const QString &file, const QJsonObject &store)
{
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)
        || !out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return false;
    const QByteArray bytes = QJsonDocument(store).toJson(QJsonDocument::Compact);
    return out.write(bytes) == bytes.size() && out.commit();
}
bool lockStore(QLockFile &lock, const QString &file, const std::atomic<bool> *cancel, bool wait = true)
{
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) return false;
    QElapsedTimer timer;
    timer.start();
    do {
        if (stopped(cancel)) return false;
        if (lock.tryLock()) return true;
        if (!wait) return false;
        QThread::msleep(25);
    } while (timer.elapsed() < 90000);
    return false;
}
QJsonObject record(const QJsonObject &store, const QString &id)
{
    return store.value("accounts").toObject().value(id).toObject();
}
void put(QJsonObject &store, const QString &id, const QJsonObject &account)
{
    QJsonObject accounts = store.value("accounts").toObject();
    accounts.insert(id, account);
    store.insert("accounts", accounts);
}
void clearTokens(QJsonObject &account)
{
    for (const char *key : {"access_token", "refresh_token", "id_token", "expires_at", "scopes", "signing_out"}) account.remove(key);
}
bool granted(const QJsonObject &a)
{
    return a.value("scopes").toArray().contains(planScope) && a.value("scopes").toArray().contains(QStringLiteral("resource.invoke"));
}
bool signedInRecord(const QJsonObject &a)
{
    return !a.value("signing_out").toBool() && granted(a) && (!a.value("refresh_token").toString().isEmpty()
        || (!a.value("access_token").toString().isEmpty() && a.value("expires_at").toInteger() > QDateTime::currentSecsSinceEpoch()));
}
QString applyTokens(QJsonObject &account, const QJsonObject &tokens, bool initial)
{
    if (tokens.value("access_token").toString().isEmpty()
        || tokens.value("token_type").toString().compare(QLatin1String("Bearer"), Qt::CaseInsensitive) != 0
        || tokens.value("expires_in").toInteger() <= 0)
        return QStringLiteral("OpenAI returned an incomplete token response. Sign in again.");
    if (initial) { account.remove("scopes"); account.remove("signing_out"); }
    if (tokens.contains("scope")) account.insert("scopes", QJsonArray::fromStringList(tokens.value("scope").toString().split(' ', Qt::SkipEmptyParts)));
    if (!granted(account)) return QStringLiteral("ChatGPT plan permission was not granted. Sign in and allow plan usage.");
    if (initial && tokens.value("refresh_token").toString().isEmpty()) return QStringLiteral("OpenAI did not grant a renewable session. Sign in again.");
    for (const char *key : {"access_token", "refresh_token", "id_token"})
        if (!tokens.value(key).toString().isEmpty()) account.insert(key, tokens.value(key));
    account.insert("expires_at", QDateTime::currentSecsSinceEpoch() + tokens.value("expires_in").toInteger());
    return {};
}
Reply retryRequest(const QString &url, const QByteArray &body, const std::atomic<bool> *cancel)
{
    Reply r;
    for (int attempt = 0; attempt < 3 && !stopped(cancel); ++attempt) {
        if (attempt) {
            for (int i = 0; i < attempt * 10 && !stopped(cancel); ++i) QThread::msleep(100);
        }
        r = request(url, body, {}, true, cancel, 20000);
        if (r.error.isEmpty() || (r.status > 0 && r.status < 500 && r.status != 429)) break;
    }
    return r;
}
QString loadModels(const QString &file, const QString &id, const std::atomic<bool> *cancel)
{
    QString error;
    QString token = ChatGptAuth::accessToken(file, id, &error, cancel);
    if (token.isEmpty()) return error;
    auto r = request(ChatGptAuth::apiBase() + QStringLiteral("/models"), {}, token, false, cancel);
    if (r.status == 401 && !stopped(cancel)) {
        token = ChatGptAuth::accessToken(file, id, &error, cancel, token);
        if (token.isEmpty()) return error;
        r = request(ChatGptAuth::apiBase() + QStringLiteral("/models"), {}, token, false, cancel);
    }
    if (!r.error.isEmpty()) return r.error + QStringLiteral(" Refresh models after checking your account access.");
    if (!r.json().value("models").isArray()) return QStringLiteral("ChatGPT returned an invalid model catalog.");
    QJsonArray models;
    for (const auto &v : r.json().value("models").toArray()) {
        const auto m = v.toObject();
        if (m.value("visibility").toString() == QLatin1String("list") && !m.value("slug").toString().isEmpty())
            models.append(QJsonObject{{"name", m.value("slug")}, {"label", m.value("display_name").toString(m.value("slug").toString())}});
    }
    QLockFile lock(file + QStringLiteral(".lock"));
    if (!lockStore(lock, file, cancel)) return QStringLiteral("Could not lock ChatGPT settings.");
    auto store = ChatGptAuth::readStore(file);
    auto a = record(store, id);
    if (!signedInRecord(a) || stopped(cancel)) return QStringLiteral("ChatGPT session ended.");
    a.insert("models", models);
    put(store, id, a);
    return saveStore(file, store) ? QString() : QStringLiteral("Could not save ChatGPT models.");
}
}

QString ChatGptAuth::authBase() { return endpoint("MVPLAYER_CHATGPT_AUTH", QStringLiteral("https://auth.openai.com")); }
QString ChatGptAuth::apiBase() { return endpoint("MVPLAYER_CHATGPT_API", resource); }
QString ChatGptAuth::defaultFile()
{
#ifdef Q_OS_WIN
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("mvplayer"), QStringLiteral("mvplayer"));
#else
    QSettings settings(QStringLiteral("mvplayer"), QStringLiteral("mvplayer"));
#endif
    return QFileInfo(settings.fileName()).absoluteDir().filePath(QStringLiteral("chatgpt.json"));
}
QJsonObject ChatGptAuth::readStore(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.read(4 * 1024 * 1024)).object();
}
bool ChatGptAuth::hasSession(const QString &file, const QString &id) { return signedInRecord(accountRecord(file, id)); }
QJsonObject ChatGptAuth::accountRecord(const QString &file, const QString &id) { return record(readStore(file), id); }

QString ChatGptAuth::accessToken(const QString &file, const QString &id, QString *error,
                                 const std::atomic<bool> *cancel, const QString &rejectedToken)
{
    error->clear();
    if (id.isEmpty() || file.isEmpty()) { *error = QStringLiteral("Sign in to ChatGPT in Settings → Judge."); return {}; }
    QLockFile lock(file + QStringLiteral(".lock"));
    if (!lockStore(lock, file, cancel)) { *error = QStringLiteral("Could not lock ChatGPT credentials."); return {}; }
    auto store = readStore(file);
    auto a = record(store, id);
    if (!signedInRecord(a)) { *error = QStringLiteral("ChatGPT is signed out or its permission has expired. Sign in again."); return {}; }
    const QString token = a.value("access_token").toString();
    if (!token.isEmpty() && a.value("expires_at").toInteger() > QDateTime::currentSecsSinceEpoch() + 60
        && (rejectedToken.isEmpty() || token != rejectedToken)) return token;
    if (a.value("refresh_token").toString().isEmpty()) { *error = QStringLiteral("ChatGPT session expired. Sign in again."); return {}; }
    const auto r = retryRequest(authBase() + QStringLiteral("/api/accounts/oauth/token"),
        form({{"grant_type", "refresh_token"}, {"client_id", id}, {"refresh_token", a.value("refresh_token")}, {"resource", resource}}), cancel);
    if (!r.error.isEmpty()) {
        if (r.status == 400 || r.status == 401 || r.status == 403) {
            clearTokens(a);
            put(store, id, a);
            saveStore(file, store);
            *error = QStringLiteral("ChatGPT session could not be renewed. Sign in again.");
        } else *error = r.error;
        return {};
    }
    *error = applyTokens(a, r.json(), false);
    if (!error->isEmpty()) {
        clearTokens(a);
        put(store, id, a);
        saveStore(file, store);
        return {};
    }
    put(store, id, a);
    if (!saveStore(file, store)) { *error = QStringLiteral("Could not save renewed ChatGPT credentials."); return {}; }
    return a.value("access_token").toString();
}

QJsonObject ChatGptAuth::verifyIdentity(const QString &jwt, const QJsonObject &jwks,
                                        const QString &client, const QString &nonce, QString *error)
{
    *error = QStringLiteral("ChatGPT identity verification failed. Sign in again.");
    const auto parts = jwt.toLatin1().split('.');
    if (parts.size() != 3 || jwt.size() > 128 * 1024) return {};
    auto decode = [](const QByteArray &s) { return QByteArray::fromBase64(s, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors); };
    const auto header = QJsonDocument::fromJson(decode(parts[0])).object();
    const QString alg = header.value("alg").toString(), kid = header.value("kid").toString();
    if ((alg != QLatin1String("RS256") && alg != QLatin1String("ES256")) || kid.isEmpty() || header.contains("crit")) return {};
    QJsonObject jwk;
    for (const auto &v : jwks.value("keys").toArray()) {
        const auto k = v.toObject();
        if (k.value("kid").toString() == kid) { if (!jwk.isEmpty()) return {}; jwk = k; }
    }
    if (jwk.isEmpty() || (jwk.contains("use") && jwk.value("use").toString() != QLatin1String("sig"))
        || (jwk.contains("alg") && jwk.value("alg").toString() != alg)) return {};
    using PKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using Big = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
    using Builder = std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)>;
    using Params = std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)>;
    Builder builder(OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
    if (!builder) return {};
    QByteArray signature = decode(parts[2]);
    QByteArray point;
    Big n(nullptr, BN_free), e(nullptr, BN_free);
    const bool rsa = alg == QLatin1String("RS256");
    if (rsa) {
        if (jwk.value("kty").toString() != QLatin1String("RSA")) return {};
        auto nb = decode(jwk.value("n").toString().toLatin1()), eb = decode(jwk.value("e").toString().toLatin1());
        n.reset(BN_bin2bn(reinterpret_cast<const unsigned char *>(nb.constData()), nb.size(), nullptr));
        e.reset(BN_bin2bn(reinterpret_cast<const unsigned char *>(eb.constData()), eb.size(), nullptr));
        if (!n || !e || BN_num_bits(n.get()) < 2048
            || !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, n.get())
            || !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, e.get())) return {};
    } else {
        if (jwk.value("kty").toString() != QLatin1String("EC") || jwk.value("crv").toString() != QLatin1String("P-256") || signature.size() != 64) return {};
        const auto x = decode(jwk.value("x").toString().toLatin1()), y = decode(jwk.value("y").toString().toLatin1());
        if (x.size() != 32 || y.size() != 32) return {};
        point = QByteArray(1, char(4)) + x + y;
        if (!OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0)
            || !OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size())) return {};
        std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(ECDSA_SIG_new(), ECDSA_SIG_free);
        Big r(BN_bin2bn(reinterpret_cast<const unsigned char *>(signature.constData()), 32, nullptr), BN_free);
        Big s(BN_bin2bn(reinterpret_cast<const unsigned char *>(signature.constData()) + 32, 32, nullptr), BN_free);
        if (!sig || !r || !s || ECDSA_SIG_set0(sig.get(), r.get(), s.get()) != 1) return {};
        r.release(); s.release();
        const int size = i2d_ECDSA_SIG(sig.get(), nullptr);
        if (size <= 0) return {};
        signature.resize(size);
        auto *p = reinterpret_cast<unsigned char *>(signature.data());
        if (i2d_ECDSA_SIG(sig.get(), &p) != size) return {};
    }
    Params params(OSSL_PARAM_BLD_to_param(builder.get()), OSSL_PARAM_free);
    Context ctx(EVP_PKEY_CTX_new_from_name(nullptr, rsa ? "RSA" : "EC", nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *rawKey = nullptr;
    if (!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) <= 0
        || EVP_PKEY_fromdata(ctx.get(), &rawKey, EVP_PKEY_PUBLIC_KEY, params.get()) <= 0) return {};
    PKey key(rawKey, EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    const QByteArray signedBytes = parts[0] + '.' + parts[1];
    if (!md || EVP_DigestVerifyInit(md.get(), nullptr, EVP_sha256(), nullptr, key.get()) <= 0
        || EVP_DigestVerify(md.get(), reinterpret_cast<const unsigned char *>(signature.constData()), signature.size(),
                            reinterpret_cast<const unsigned char *>(signedBytes.constData()), signedBytes.size()) != 1) return {};
    const auto claims = QJsonDocument::fromJson(decode(parts[1])).object();
    const auto aud = claims.value("aud");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (claims.value("iss").toString() != authBase() || claims.value("sub").toString().isEmpty()
        || !(aud.toString() == client || (aud.isArray() && aud.toArray().contains(client)))
        || (aud.isArray() && aud.toArray().size() > 1 && claims.value("azp").toString() != client)
        || (claims.contains("azp") && claims.value("azp").toString() != client)
        || claims.value("exp").toInteger() <= now - 5 || claims.value("iat").toInteger() <= 0
        || claims.value("iat").toInteger() > now + 5 || claims.value("nbf").toInteger() > now + 5
        || claims.value("nonce").toString() != nonce || nonce.isEmpty()) return {};
    error->clear();
    return claims;
}

ChatGptAuth::ChatGptAuth(const QString &file, QObject *parent) : QObject(parent), m_file(file)
{
    m_pool.setMaxThreadCount(1);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] { m_server.close(); finish(tr("ChatGPT sign-in timed out. Try again."), {}); });
    connect(&m_server, &QTcpServer::newConnection, this, &ChatGptAuth::callback);
    reload();
}
ChatGptAuth::~ChatGptAuth() { m_cancel = true; m_server.close(); m_pool.waitForDone(); }
void ChatGptAuth::reload() { m_store = readStore(m_file); }
QString ChatGptAuth::account() const { return m_store.value("selected").toString(); }
bool ChatGptAuth::signedIn() const { return signedInRecord(record(m_store, account())); }
QString ChatGptAuth::model() const { return record(m_store, account()).value("model").toString(); }
QVariantList ChatGptAuth::models() const { return record(m_store, account()).value("models").toArray().toVariantList(); }
QVariantList ChatGptAuth::accounts() const
{
    QVariantList out;
    const auto all = m_store.value("accounts").toObject();
    for (auto it = all.begin(); it != all.end(); ++it) {
        const auto a = it.value().toObject();
        out.append(QVariantMap{{"id", it.key()}, {"label", a.value("email").toString(tr("ChatGPT account")) + QStringLiteral(" · ") + it.key()},
                               {"signedIn", signedInRecord(a)}});
    }
    return out;
}
void ChatGptAuth::finish(const QString &error, const QString &success)
{
    m_server.close(); m_timeout.stop();
    m_state.clear(); m_nonce.clear(); m_verifier.clear();
    m_busy = false;
    m_status = error.isEmpty() ? success : error;
    reload();
    emit changed();
    emit finished(error.isEmpty());
}
void ChatGptAuth::run(std::function<OperationResult(const std::atomic<bool> *)> work, const QString &success)
{
    m_busy = true; m_cancel = false;
    emit changed();
    m_pool.start([this, work = std::move(work), success] {
        const auto result = work(&m_cancel);
        QMetaObject::invokeMethod(this, [this, result, success] { finish(result.error, result.notice.isEmpty() ? success : result.notice); }, Qt::QueuedConnection);
    });
}
void ChatGptAuth::setModel(const QString &model)
{
    if (m_busy) return;
    bool found = false;
    for (const auto &m : models()) if (m.toMap().value("name").toString() == model) found = true;
    if (!found) return;
    QLockFile lock(m_file + QStringLiteral(".lock"));
    if (!lockStore(lock, m_file, nullptr, false)) { m_status = tr("ChatGPT settings are busy. Try again."); emit changed(); return; }
    auto store = readStore(m_file);
    auto a = record(store, account());
    if (a.isEmpty()) return;
    a.insert("model", model);
    put(store, account(), a);
    if (!saveStore(m_file, store)) m_status = tr("Could not save ChatGPT model.");
    reload(); emit changed();
}
void ChatGptAuth::selectAccount(const QString &id)
{
    if (m_busy || id == account()) return;
    QLockFile lock(m_file + QStringLiteral(".lock"));
    if (!lockStore(lock, m_file, nullptr, false)) { m_status = tr("ChatGPT settings are busy. Try again."); emit changed(); return; }
    auto store = readStore(m_file);
    if (record(store, id).isEmpty()) return;
    store.insert("selected", id);
    if (!saveStore(m_file, store)) { m_status = tr("Could not save ChatGPT account selection."); emit changed(); return; }
    reload(); m_status.clear(); emit changed();
    lock.unlock();
}
void ChatGptAuth::refreshModels()
{
    if (m_busy || !signedIn()) return;
    const QString file = m_file, id = account();
    run([file, id](const std::atomic<bool> *cancel) { return loadModels(file, id, cancel); }, tr("ChatGPT models refreshed. Requests use your ChatGPT plan."));
}
void ChatGptAuth::signIn(const QString &id)
{
    if (m_busy) return;
    QLockFile lock(m_file + QStringLiteral(".lock"));
    if (!lockStore(lock, m_file, nullptr, false)) { finish(tr("ChatGPT settings are busy. Try again."), {}); return; }
    auto store = readStore(m_file);
    if (QFileInfo::exists(m_file) && store.isEmpty()) { finish(tr("Could not read ChatGPT settings; the file was left intact."), {}); return; }
    if (!id.isEmpty() && record(store, id).isEmpty()) { finish(tr("Select a saved ChatGPT account or add another."), {}); return; }
    if (store.value("host_id").toString().isEmpty()) {
        store.insert("host_id", QStringLiteral("urn:uuid:") + QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!saveStore(m_file, store)) { finish(tr("Could not save the ChatGPT host registration."), {}); return; }
    }
    m_state = randomValue(); m_nonce = randomValue(); m_verifier = randomValue();
    if (m_state.isEmpty() || m_nonce.isEmpty() || m_verifier.isEmpty() || authBase().isEmpty()) { finish(tr("Could not prepare ChatGPT sign-in."), {}); return; }
    if (!m_server.listen(QHostAddress::LocalHost, 0)) { finish(tr("Could not listen for the ChatGPT browser callback."), {}); return; }
    m_pendingClient = id;
    m_pendingSubject = record(store, id).value("subject").toString();
    m_redirect = QStringLiteral("http://127.0.0.1:%1/auth/callback").arg(m_server.serverPort());
    QUrl url(authBase() + QStringLiteral("/api/accounts/authorize"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("client_id"), id.isEmpty() ? QStringLiteral("dynamic_agent_client") : id);
    if (id.isEmpty()) q.addQueryItem(QStringLiteral("agent_name_hint"), QStringLiteral("MV Player"));
    q.addQueryItem(QStringLiteral("ext_agent_host_id"), store.value("host_id").toString());
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("redirect_uri"), m_redirect);
    q.addQueryItem(QStringLiteral("scope"), QStringLiteral("openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"));
    q.addQueryItem(QStringLiteral("resource"), resource);
    q.addQueryItem(QStringLiteral("state"), m_state);
    q.addQueryItem(QStringLiteral("nonce"), m_nonce);
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    // Deliberately omit optional ID-token hints: no token in a browser URL or
    // in the CLI's fallback link. The browser can select the returning account.
    url.setQuery(q);
    m_busy = true; m_status = tr("Finish signing in in your browser.");
    m_timeout.start(5 * 60 * 1000);
    emit changed(); emit openBrowser(url);
}
void ChatGptAuth::cancelSignIn()
{
    if (m_server.isListening()) { m_server.close(); finish(tr("ChatGPT sign-in cancelled."), {}); }
}
void ChatGptAuth::callback()
{
    while (m_server.hasPendingConnections()) {
        auto *socket = m_server.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        QTimer::singleShot(5000, socket, [socket] { socket->abort(); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
            *buffer += socket->readAll();
            if (buffer->size() > 16384) { socket->abort(); return; }
            if (!buffer->contains("\r\n\r\n")) return;
            const auto parts = buffer->split('\n').value(0).trimmed().split(' ');
            const QUrl url = QUrl::fromEncoded(parts.value(1));
            const QUrlQuery q(url);
            bool unique = true;
            QSet<QString> names;
            for (const auto &pair : q.queryItems()) { if (names.contains(pair.first)) unique = false; names.insert(pair.first); }
            const bool valid = m_server.isListening() && parts.value(0) == "GET" && url.isRelative()
                && url.path() == QLatin1String("/auth/callback") && unique
                && q.queryItemValue(QStringLiteral("state")) == m_state;
            const QByteArray html = valid ? QByteArray("Return to MV Player to see the sign-in result.") : QByteArray("Invalid sign-in callback. Return to MV Player and try again.");
            socket->write(QByteArray(valid ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 400 Bad Request\r\n")
                + "Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                + QByteArray::number(html.size()) + "\r\n\r\n" + html);
            socket->disconnectFromHost();
            if (!valid) return;
            m_server.close(); m_timeout.stop();
            if (q.hasQueryItem(QStringLiteral("error"))) { finish(tr("ChatGPT sign-in was declined or failed. Try again."), {}); return; }
            const QString issued = q.queryItemValue(QStringLiteral("client_id"));
            const QString client = m_pendingClient.isEmpty() ? issued : m_pendingClient;
            const QString code = q.queryItemValue(QStringLiteral("code"));
            if (code.isEmpty() || client.isEmpty() || client == QLatin1String("dynamic_agent_client")
                || (!m_pendingClient.isEmpty() && !issued.isEmpty() && issued != m_pendingClient)
                || (q.hasQueryItem(QStringLiteral("iss")) && q.queryItemValue(QStringLiteral("iss")) != authBase())) {
                finish(tr("ChatGPT returned an invalid registration callback."), {}); return;
            }
            const QString file = m_file, nonce = m_nonce, verifier = m_verifier, redirect = m_redirect, subject = m_pendingSubject;
            m_status = tr("Verifying your ChatGPT account…");
            run([file, client, code, nonce, verifier, redirect, subject](const std::atomic<bool> *cancel) -> OperationResult {
                // Authorization codes are one-use: a failed exchange starts a
                // fresh browser attempt, never an automatic code replay.
                const auto tokens = request(authBase() + QStringLiteral("/api/accounts/oauth/token"),
                    form({{"grant_type", "authorization_code"}, {"client_id", client}, {"code", code},
                          {"code_verifier", verifier}, {"redirect_uri", redirect}, {"resource", resource}}), {}, true, cancel);
                if (!tokens.error.isEmpty()) return tokens.error + QStringLiteral(" Start sign-in again.");
                const auto keys = request(authBase() + QStringLiteral("/.well-known/jwks.json"), {}, {}, false, cancel);
                if (!keys.error.isEmpty()) return keys.error;
                QString error;
                const auto claims = verifyIdentity(tokens.json().value("id_token").toString(), keys.json(), client, nonce, &error);
                if (!error.isEmpty()) return error;
                if (!subject.isEmpty() && subject != claims.value("sub").toString()) return QStringLiteral("The account differs from the one selected. Add it as another account.");
                QLockFile lock(file + QStringLiteral(".lock"));
                if (!lockStore(lock, file, cancel)) return QStringLiteral("Could not lock ChatGPT credentials.");
                auto store = readStore(file);
                if (store.value("host_id").toString().isEmpty()) return QStringLiteral("ChatGPT settings could not be read; the file was left intact.");
                auto a = record(store, client);
                if (!a.isEmpty() && a.value("subject") != claims.value("sub")) return QStringLiteral("The registration belongs to a different account.");
                a.insert("client_id", client); a.insert("subject", claims.value("sub")); a.insert("email", claims.value("email"));
                error = applyTokens(a, tokens.json(), true);
                if (!error.isEmpty() || stopped(cancel)) return error.isEmpty() ? QStringLiteral("Cancelled.") : error;
                put(store, client, a); store.insert("selected", client);
                if (!saveStore(file, store)) return QStringLiteral("Could not save ChatGPT credentials.");
                lock.unlock();
                error = loadModels(file, client, cancel);
                return OperationResult({}, error.isEmpty() ? QString() : QStringLiteral("Signed in. ") + error);
            }, tr("Signed in to ChatGPT. Choose a model, then enable Advice or Judge when ready."));
        });
    }
}
void ChatGptAuth::signOut()
{
    if (m_busy || account().isEmpty()) return;
    const QString file = m_file, id = account();
    run([file, id](const std::atomic<bool> *cancel) -> QString {
        QLockFile lock(file + QStringLiteral(".lock"));
        if (!lockStore(lock, file, cancel)) return QStringLiteral("Could not lock ChatGPT credentials.");
        auto store = readStore(file);
        auto a = record(store, id);
        if (a.isEmpty()) return QStringLiteral("ChatGPT account could not be read; the file was left intact.");
        a.insert("signing_out", true);
        put(store, id, a);
        if (!saveStore(file, store)) return QStringLiteral("Could not stop the local ChatGPT session.");
        bool revoked = a.value("refresh_token").toString().isEmpty();
        if (!revoked) {
            const auto discovery = request(authBase() + QStringLiteral("/.well-known/openid-configuration"), {}, {}, false, cancel);
            const QUrl url(discovery.json().value("revocation_endpoint").toString()), base(authBase());
            // Trust discovery only on the configured issuer's origin.
            if (discovery.error.isEmpty() && discovery.json().value("issuer").toString() == authBase()
                && url.scheme() == base.scheme() && url.host() == base.host() && url.port() == base.port() && url.userInfo().isEmpty()) {
                const auto r = retryRequest(url.toString(), form({{"token", a.value("refresh_token")}, {"token_type_hint", "refresh_token"}, {"client_id", id}}), cancel);
                revoked = r.error.isEmpty();
            }
        }
        clearTokens(a); put(store, id, a);
        if (!saveStore(file, store)) return QStringLiteral("Could not clear local ChatGPT credentials.");
        return revoked ? QString() : QStringLiteral("Signed out locally. Remote revocation was not confirmed; disconnect MV Player in ChatGPT Settings.");
    }, tr("Signed out of ChatGPT."));
}
