#include "core/ChatGptAuth.h"
#include "core/ChatGptTransport.h"
#include "core/Judge.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSaveFile>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <future>

static QByteArray compact(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
static QByteArray b64(const QByteArray &b) { return b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals); }
static void save(const QString &file, const QJsonObject &o)
{
    QSaveFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const auto bytes = compact(o);
    QCOMPARE(f.write(bytes), bytes.size());
    QVERIFY(f.commit());
}

class FakeOpenAi : public QTcpServer
{
public:
    EVP_PKEY *key = nullptr;
    QJsonObject jwks;
    QString nonce, challenge, redirect, client = QStringLiteral("oaiapp_first"), subject = QStringLiteral("user1");
    QString mode, authFailure, responseMode;
    QString token = QStringLiteral("access-secret"), refresh = QStringLiteral("refresh-secret");
    QJsonObject lastRequest;
    int exchanges = 0, renewals = 0, responses = 0, revocations = 0;
    QStringList failures;
    FakeOpenAi()
    {
        key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", size_t(2048));
        Q_ASSERT(key);
        auto bn = [&](const char *name) {
            BIGNUM *v = nullptr;
            EVP_PKEY_get_bn_param(key, name, &v);
            QByteArray bytes(BN_num_bytes(v), Qt::Uninitialized);
            BN_bn2bin(v, reinterpret_cast<unsigned char *>(bytes.data()));
            BN_free(v);
            return QString::fromLatin1(b64(bytes));
        };
        jwks = {{"keys", QJsonArray{QJsonObject{{"kty", "RSA"}, {"alg", "RS256"}, {"kid", "test-key"}, {"use", "sig"},
                                              {"n", bn(OSSL_PKEY_PARAM_RSA_N)}, {"e", bn(OSSL_PKEY_PARAM_RSA_E)}}}}};
        if (!listen(QHostAddress::LocalHost)) qFatal("Local test server: %s", qPrintable(errorString()));
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes] {
                    *bytes += socket->readAll();
                    const int end = bytes->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const auto head = bytes->left(end);
                    int size = 0;
                    for (auto line : head.split('\n')) if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
                    if (bytes->size() < end + 4 + size) return;
                    socket->disconnect(socket, &QTcpSocket::readyRead, socket, nullptr);
                    handle(socket, head, bytes->mid(end + 4, size));
                });
            }
        });
    }
    ~FakeOpenAi() override { EVP_PKEY_free(key); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
    void check(bool ok, const QString &message) { if (!ok) failures << message; }
    QJsonObject claims() const
    {
        return {{"iss", base()}, {"aud", client}, {"sub", subject}, {"email", "same@example.test"},
                {"nonce", nonce}, {"iat", QDateTime::currentSecsSinceEpoch()}, {"exp", QDateTime::currentSecsSinceEpoch() + 3600}};
    }
    QString jwt(QJsonObject payload, QJsonObject header = {{"alg", "RS256"}, {"kid", "test-key"}})
    {
        const QByteArray message = b64(compact(header)) + '.' + b64(compact(payload));
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key);
        size_t size = 0;
        EVP_DigestSign(ctx, nullptr, &size, reinterpret_cast<const unsigned char *>(message.constData()), message.size());
        QByteArray sig(size, Qt::Uninitialized);
        EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(sig.data()), &size, reinterpret_cast<const unsigned char *>(message.constData()), message.size());
        sig.resize(size);
        EVP_MD_CTX_free(ctx);
        return QString::fromLatin1(message + '.' + b64(sig));
    }
    void reply(QTcpSocket *s, QByteArray body, int status = 200, bool sse = false)
    {
        s->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: " + (sse ? QByteArray("text/event-stream") : QByteArray("application/json"))
                 + "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n");
        // Packet boundaries deliberately cut through JSON and UTF-8.
        s->write(body.left(body.size() / 2));
        QTimer::singleShot(2, s, [s, body] { s->write(body.mid(body.size() / 2)); s->disconnectFromHost(); });
    }
    void handle(QTcpSocket *s, const QByteArray &head, const QByteArray &body)
    {
        const QByteArray path = head.split(' ').value(1);
        const QUrlQuery form(QString::fromUtf8(body));
        auto val = [&](const QString &k) { return form.queryItemValue(k, QUrl::FullyDecoded); };
        if (path == "/api/accounts/oauth/token") {
            const bool renewing = val("grant_type") == QLatin1String("refresh_token");
            if (renewing) {
                ++renewals;
                check(val("refresh_token") == refresh, "refresh token rotation raced");
                if (authFailure == QLatin1String("refresh")) { reply(s, "{\"error\":\"invalid_grant\"}", 400); return; }
                token = QStringLiteral("renewed-secret-%1").arg(renewals);
                refresh = QStringLiteral("rotated-secret-%1").arg(renewals);
            } else {
                ++exchanges;
                check(val("client_id") == client, "token exchange used wrong client ID");
                check(val("redirect_uri") == redirect, "callback URI changed");
                check(QString::fromLatin1(b64(QCryptographicHash::hash(val("code_verifier").toLatin1(), QCryptographicHash::Sha256))) == challenge, "PKCE mismatch");
                check(val("code") == QLatin1String("code+with/special=characters"), "form encoding damaged authorization code");
            }
            check(val("resource") == QLatin1String("https://api.openai.com/v1"), "wrong resource");
            auto c = claims();
            if (authFailure == QLatin1String("nonce")) c.insert("nonce", "not-the-nonce");
            const QString scope = authFailure == QLatin1String("scope") ? QStringLiteral("openid email")
                : QStringLiteral("openid email profile offline_access resource.invoke chatgpt.tokens.use.direct");
            reply(s, compact({{"access_token", token}, {"refresh_token", refresh}, {"expires_in", 3600}, {"token_type", "Bearer"},
                              {"scope", scope}, {"id_token", jwt(c)}}));
        } else if (path == "/.well-known/jwks.json") reply(s, compact(jwks));
        else if (path == "/.well-known/openid-configuration") reply(s, compact({{"issuer", base()}, {"revocation_endpoint", base() + "/revoke"}}));
        else if (path == "/revoke") {
            ++revocations;
            check(val("token") == refresh && val("token_type_hint") == QLatin1String("refresh_token"), "wrong revocation token");
            reply(s, {}, authFailure == QLatin1String("revoke") ? 400 : 200);
        } else if (path == "/v1/models") {
            if (mode == QLatin1String("models-fail")) { reply(s, "{}", 503); return; }
            check(head.contains("Bearer " + token.toUtf8()), "models missing bearer credential");
            reply(s, compact({{"models", QJsonArray{QJsonObject{{"slug", "test-model"}, {"display_name", "Test model"}, {"visibility", "list"}},
                                                          QJsonObject{{"slug", "hidden-model"}, {"visibility", "hide"}}}}}));
        } else if (path == "/v1/responses") {
            ++responses;
            if (responseMode == QLatin1String("hang")) return;
            lastRequest = QJsonDocument::fromJson(body).object();
            check(head.contains("Bearer " + token.toUtf8()), "inference missing bearer credential");
            check(lastRequest.value("stream").toBool() && lastRequest.value("store") == QJsonValue(false), "incorrect stream/store flags");
            for (const auto *field : {"max_output_tokens", "max_tokens", "system", "temperature", "previous_response_id"})
                check(!lastRequest.contains(field), "unsupported Responses parameter");
            if (responseMode == QLatin1String("401-once") && responses == 1) { reply(s, "{}", 401); return; }
            if (responseMode == QLatin1String("403")) { reply(s, "{}", 403); return; }
            const QJsonObject answer{{"lookup", true}, {"skip_reason", ""}, {"queries", QJsonArray{"artist song MV"}}, {"song_known", "unknown"}};
            const QString text = QString::fromUtf8(compact(responseMode == QLatin1String("schema") ? QJsonObject{{"lookup", "wrong"}} : answer));
            QJsonObject completed{{"status", "completed"}, {"usage", QJsonObject{{"input_tokens", 11}, {"output_tokens", 8}}},
                {"output", QJsonArray{QJsonObject{{"type", "message"}, {"role", "assistant"}, {"status", "completed"},
                    {"content", QJsonArray{QJsonObject{{"type", "output_text"}, {"text", text}}}}}}}};
            QByteArray stream = "event: response.output_text.delta\r\ndata: " + compact({{"type", "response.output_text.delta"}, {"delta", text}}) + "\r\n\r\n";
            stream += "data: " + compact({{"type", "response.output_item.done"}, {"output_index", 0},
                {"item", completed.value("output").toArray()[0]}}) + "\r\n\r\n";
            if (responseMode == QLatin1String("empty-terminal-output")) completed.insert("output", QJsonArray{});
            if (responseMode == QLatin1String("missing-terminal-output")) completed.remove("output");
            if (responseMode != QLatin1String("truncated"))
                stream += "event: response.completed\r\ndata: " + compact({{"type", "response.completed"}, {"response", completed}}) + "\r\n\r\n";
            reply(s, stream, 200, true);
        } else { failures << QString::fromUtf8(path); reply(s, "{}", 404); }
    }
};

class ChatGptTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<FakeOpenAi> server;
    std::unique_ptr<ChatGptAuth> auth;
    QNetworkAccessManager browser;
    QString file;
    QUrl browserUrl;
    bool autoCallback = true;
    void openCallback(const QUrl &url, QString state = QString())
    {
        const QUrlQuery q(url);
        server->nonce = q.queryItemValue("nonce");
        server->challenge = q.queryItemValue("code_challenge");
        server->redirect = q.queryItemValue("redirect_uri", QUrl::FullyDecoded);
        QUrl callback(server->redirect);
        QUrlQuery fields;
        fields.addQueryItem("state", state.isNull() ? q.queryItemValue("state") : state);
        fields.addQueryItem("client_id", server->client);
        fields.addQueryItem("code", "code+with/special=characters");
        callback.setQuery(fields);
        auto *reply = browser.get(QNetworkRequest(callback));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
    void login(const QString &id = {})
    {
        QSignalSpy done(auth.get(), &ChatGptAuth::finished);
        auth->signIn(id);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
        QVERIFY2(done[0][0].toBool(), qPrintable(auth->status()));
        QVERIFY(auth->signedIn());
    }
    Judge::Answer ask(const QString &cache = {})
    {
        QJsonArray content{QJsonObject{{"type", "text"}, {"text", "Track facts 日本語"}},
            QJsonObject{{"type", "image"}, {"source", QJsonObject{{"type", "base64"}, {"media_type", "image/jpeg"}, {"data", "aW1hZ2U="}}}}};
        return Judge::ask({}, "test-model", JudgeWords::kQueryWords, content, JudgeWords::kQuerySchema,
                          1500, cache, nullptr, "chatgpt", file, auth->account());
    }
private slots:
    void init()
    {
        dir = std::make_unique<QTemporaryDir>();
        QVERIFY(dir->isValid());
        file = dir->filePath("chatgpt.json");
        server = std::make_unique<FakeOpenAi>();
        qputenv("MVPLAYER_CHATGPT_AUTH", server->base().toUtf8());
        qputenv("MVPLAYER_CHATGPT_API", (server->base() + "/v1").toUtf8());
        auth = std::make_unique<ChatGptAuth>(file);
        autoCallback = true;
        connect(auth.get(), &ChatGptAuth::openBrowser, this, [this](const QUrl &url) {
            browserUrl = url;
            if (autoCallback) openCallback(url);
        });
    }
    void cleanup()
    {
        auth.reset();
        QVERIFY2(server->failures.isEmpty(), qPrintable(server->failures.join('\n')));
        server.reset();
        Judge::setCapture({}); Judge::setReplay({});
        qunsetenv("MVPLAYER_CHATGPT_AUTH"); qunsetenv("MVPLAYER_CHATGPT_API");
    }
    void loginAndPersistence()
    {
        login();
        QCOMPARE(auth->account(), server->client);
        QCOMPARE(auth->models().size(), 1);
        QVERIFY(auth->model().isEmpty());
        auth->setModel("test-model");
        QCOMPARE(auth->model(), QStringLiteral("test-model"));
        const auto permissions = QFile::permissions(file);
        QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));
        ChatGptAuth reopened(file);
        QVERIFY(reopened.signedIn());
        QCOMPARE(reopened.model(), auth->model());
        const auto old = ChatGptAuth::readStore(file);
        login(auth->account());
        QCOMPARE(ChatGptAuth::readStore(file).value("host_id"), old.value("host_id"));
        QVERIFY(!QUrlQuery(browserUrl).hasQueryItem("agent_name_hint"));
        QVERIFY(!browserUrl.toString().contains(server->token));
    }
    void badCallbacksAndIdentity()
    {
        login();
        const auto before = ChatGptAuth::readStore(file);
        autoCallback = false;
        auth->signIn();
        openCallback(browserUrl, "wrong-state");
        QTest::qWait(30);
        QCOMPARE(server->exchanges, 1);
        auth->cancelSignIn();
        QCOMPARE(ChatGptAuth::readStore(file), before);
        autoCallback = true;
        for (const QString &failure : {QStringLiteral("nonce"), QStringLiteral("scope")}) {
            server->authFailure = failure;
            QSignalSpy done(auth.get(), &ChatGptAuth::finished);
            auth->signIn(auth->account());
            QTRY_COMPARE(done.count(), 1);
            QVERIFY(!done[0][0].toBool());
            QCOMPARE(ChatGptAuth::readStore(file), before);
        }
    }
    void identityClaimsAndSignature()
    {
        server->nonce = "nonce";
        QString error;
        auto good = server->claims();
        QVERIFY(!ChatGptAuth::verifyIdentity(server->jwt(good), server->jwks, server->client, "nonce", &error).isEmpty());
        for (const QString &field : {QStringLiteral("iss"), QStringLiteral("aud"), QStringLiteral("sub"), QStringLiteral("nonce"), QStringLiteral("exp"), QStringLiteral("iat")}) {
            auto bad = good; bad.remove(field);
            QVERIFY(ChatGptAuth::verifyIdentity(server->jwt(bad), server->jwks, server->client, "nonce", &error).isEmpty());
            QVERIFY(!error.isEmpty());
        }
        auto expired = good; expired.insert("exp", QDateTime::currentSecsSinceEpoch() - 60);
        QVERIFY(ChatGptAuth::verifyIdentity(server->jwt(expired), server->jwks, server->client, "nonce", &error).isEmpty());
        QVERIFY(ChatGptAuth::verifyIdentity(server->jwt(good, {{"alg", "none"}, {"kid", "test-key"}}), server->jwks, server->client, "nonce", &error).isEmpty());
        auto token = server->jwt(good); token[token.size() - 10] = token[token.size() - 10] == 'A' ? 'B' : 'A';
        QVERIFY(ChatGptAuth::verifyIdentity(token, server->jwks, server->client, "nonce", &error).isEmpty());
    }
    void accountsStaySeparate()
    {
        login();
        const QString first = auth->account();
        auth->setModel("test-model");
        server->client = "oaiapp_second"; server->subject = "user2";
        login();
        QCOMPARE(auth->accounts().size(), 2);
        QVERIFY(auth->model().isEmpty());
        auth->selectAccount(first);
        QCOMPARE(auth->model(), QStringLiteral("test-model"));
        const auto before = ChatGptAuth::readStore(file);
        server->client = first; // Same registration, wrong verified subject.
        QSignalSpy done(auth.get(), &ChatGptAuth::finished);
        auth->signIn(first);
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(!done[0][0].toBool());
        QCOMPARE(ChatGptAuth::readStore(file), before);
    }
    void rotatingRefreshIsSerialized()
    {
        login();
        auto store = ChatGptAuth::readStore(file);
        auto accounts = store.value("accounts").toObject();
        auto a = accounts.value(auth->account()).toObject();
        a.insert("expires_at", 1); accounts.insert(auth->account(), a); store.insert("accounts", accounts); save(file, store);
        const QString id = auth->account();
        auto work = [this, id] { QString error; return ChatGptAuth::accessToken(file, id, &error, nullptr); };
        auto one = std::async(std::launch::async, work), two = std::async(std::launch::async, work);
        QTRY_VERIFY_WITH_TIMEOUT(one.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(two.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        QCOMPARE(one.get(), server->token); QCOMPARE(two.get(), server->token);
        QCOMPARE(server->renewals, 1);
        QCOMPARE(ChatGptAuth::accountRecord(file, id).value("refresh_token").toString(), server->refresh);
    }
    void judgeUsesAccountAndCaches()
    {
        login();
        Judge::setCapture(dir->filePath("capture"));
        const QString cache = dir->filePath("cache");
        const auto a = ask(cache);
        QVERIFY2(a.error.isEmpty(), qPrintable(a.error));
        QCOMPARE(a.provider, QStringLiteral("chatgpt"));
        QCOMPARE(a.cost, 0.0); QCOMPARE(a.usage.value("input_tokens").toInt(), 11);
        QCOMPARE(server->lastRequest.value("instructions").toString(), QString::fromUtf8(JudgeWords::kQueryWords));
        const auto content = server->lastRequest.value("input").toArray()[0].toObject().value("content").toArray();
        QCOMPARE(content[1].toObject().value("image_url").toString(), QStringLiteral("data:image/jpeg;base64,aW1hZ2U="));
        QCOMPARE(server->lastRequest.value("text").toObject().value("format").toObject().value("schema").toObject(), QJsonDocument::fromJson(JudgeWords::kQuerySchema).object());
        QVERIFY(ask(cache).cached); QCOMPARE(server->responses, 1);
        Judge::setReplay(dir->filePath("capture"));
        QVERIFY(ask().cached); QCOMPARE(server->responses, 1);
        const QDir capture(dir->filePath("capture/judge"));
        for (const QString &f : capture.entryList(QDir::Files)) {
            QFile in(capture.filePath(f)); QVERIFY(in.open(QIODevice::ReadOnly)); const auto bytes = in.readAll();
            QVERIFY(!bytes.contains(server->token.toUtf8())); QVERIFY(!bytes.contains(server->refresh.toUtf8()));
        }
    }
    void unauthorizedRefreshesOnce()
    {
        login(); server->responseMode = "401-once";
        const auto a = ask();
        QVERIFY2(a.error.isEmpty(), qPrintable(a.error));
        QCOMPARE(server->renewals, 1); QCOMPARE(server->responses, 2);
    }
    void judgeReadsStreamedItems_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("empty terminal output") << "empty-terminal-output";
        QTest::newRow("missing terminal output") << "missing-terminal-output";
    }
    void judgeReadsStreamedItems()
    {
        QFETCH(QString, mode);
        login(); server->responseMode = mode;
        const auto a = ask();
        QVERIFY2(a.error.isEmpty(), qPrintable(a.error));
        QVERIFY(a.json.value("lookup").toBool());
        QCOMPARE(a.json.value("queries").toArray(), QJsonArray{"artist song MV"});
        QCOMPARE(a.usage.value("input_tokens").toInt(), 11);
        QCOMPARE(server->responses, 1);
    }
    void failedJudgeReplies_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("missing completion") << "truncated";
        QTest::newRow("schema violation") << "schema";
        QTest::newRow("model access denied") << "403";
    }
    void failedJudgeReplies()
    {
        QFETCH(QString, mode);
        login(); server->responseMode = mode;
        const auto a = ask(dir->filePath("cache"));
        QVERIFY(!a.error.isEmpty()); QVERIFY(a.json.isEmpty());
        QVERIFY(QDir(dir->filePath("cache/judge")).entryList(QDir::Files).isEmpty());
        QCOMPARE(server->responses, mode == QLatin1String("403") ? 1 : Judge::kRetries + 1);
    }
    void sseNeedsSuccessfulCompletion()
    {
        QString error;
        const auto data = compact({{"type", "response.completed"}, {"response", QJsonObject{{"status", "completed"}}}});
        QVERIFY(!ChatGptTransport::completedResponse("data: " + data + "\n\n", &error).isEmpty());
        QVERIFY(ChatGptTransport::completedResponse("data: " + data, &error).isEmpty());
        QVERIFY(ChatGptTransport::completedResponse("data: " + data + "\n", &error).isEmpty());
        QVERIFY(ChatGptTransport::completedResponse("data: " + data + "\n\ndata: {\"type\":\"response.failed\"}\n\n", &error).isEmpty());
        QVERIFY(ChatGptTransport::completedResponse("data: {\"type\":\"response.incomplete\"}\n\n", &error).isEmpty());
    }
    void ssePreservesOutputItems()
    {
        const QJsonObject reasoning{{"type", "reasoning"}, {"id", "reasoning-id"}, {"summary", QJsonArray{}}};
        const QJsonObject message{{"type", "message"}, {"role", "assistant"}, {"status", "completed"},
            {"content", QJsonArray{QJsonObject{{"type", "refusal"}, {"refusal", "Cannot answer."}}}}};
        const auto event = [](const QJsonObject &e) { return "data: " + compact(e) + "\n\n"; };
        // Item indexes, rather than arrival order, determine output order.
        const auto items = event({{"type", "response.output_item.done"}, {"output_index", 1}, {"item", message}})
            + event({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", reasoning}});
        const QJsonObject response{{"status", "completed"}, {"output", QJsonArray{}},
            {"usage", QJsonObject{{"output_tokens", 20}}}};
        const auto terminal = event({{"type", "response.completed"}, {"response", response}});
        QString error;
        const auto result = ChatGptTransport::completedResponse(items + terminal, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.value("output").toArray(), (QJsonArray{reasoning, message}));
        QCOMPARE(result.value("usage"), response.value("usage"));
        auto full = response;
        full.insert("output", QJsonArray{reasoning, message});
        QCOMPARE(ChatGptTransport::completedResponse(items + event({{"type", "response.completed"}, {"response", full}}), &error), full);
        QVERIFY(ChatGptTransport::completedResponse(items, &error).isEmpty());
        QVERIFY(ChatGptTransport::completedResponse(items + terminal + event({{"type", "response.failed"}}), &error).isEmpty());
        QVERIFY(ChatGptTransport::completedResponse(items + event({{"type", "response.incomplete"}}), &error).isEmpty());
        QVERIFY(ChatGptTransport::completedResponse(items + terminal + terminal, &error).isEmpty());
        QVERIFY(ChatGptTransport::completedResponse(items + items + terminal, &error).isEmpty());
        QVERIFY(ChatGptTransport::completedResponse(event({{"type", "response.output_item.done"}, {"item", message}}) + terminal, &error).isEmpty());
    }
    void signOutAndFailedRenewal()
    {
        login();
        QSignalSpy done(auth.get(), &ChatGptAuth::finished);
        auth->signOut(); QTRY_COMPARE(done.count(), 1);
        QVERIFY(done[0][0].toBool()); QVERIFY(!auth->signedIn());
        QCOMPARE(server->revocations, 1);
        const auto account = ChatGptAuth::accountRecord(file, auth->account());
        QVERIFY(!account.contains("access_token")); QVERIFY(!account.contains("refresh_token")); QVERIFY(!account.contains("id_token"));
        QVERIFY(account.contains("subject"));
        QVERIFY(!ask().error.isEmpty()); QCOMPARE(server->responses, 0);
        login(auth->account());
        server->authFailure = "refresh";
        QString error;
        QVERIFY(ChatGptAuth::accessToken(file, auth->account(), &error, nullptr, server->token).isEmpty());
        QVERIFY(!ChatGptAuth::accountRecord(file, auth->account()).contains("refresh_token"));
    }
    void modelCatalogFailureDoesNotUndoLogin()
    {
        server->mode = "models-fail";
        login();
        QVERIFY(auth->models().isEmpty());
        QVERIFY(auth->status().contains("Refresh models"));
    }
    void failedRevocationStillClearsLocalSession()
    {
        login(); server->authFailure = "revoke";
        QSignalSpy done(auth.get(), &ChatGptAuth::finished);
        auth->signOut(); QTRY_COMPARE(done.count(), 1);
        QVERIFY(!auth->signedIn());
        QVERIFY(auth->status().contains("Remote revocation was not confirmed"));
        QVERIFY(!ChatGptAuth::accountRecord(file, auth->account()).contains("refresh_token"));
    }
    void signOutCancelsInFlightAnswer()
    {
        login(); server->responseMode = "hang";
        QTimer::singleShot(30, auth.get(), [this] { auth->signOut(); });
        const auto a = ask();
        QVERIFY(!a.error.isEmpty()); QVERIFY(a.json.isEmpty());
        QTRY_VERIFY(!auth->busy());
        QVERIFY(!auth->signedIn());
        QCOMPARE(server->responses, 1);
    }
    void corruptedStoreIsNotOverwritten()
    {
        login();
        QFile f(file); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("invalid json"); f.close();
        QSignalSpy done(auth.get(), &ChatGptAuth::finished);
        auth->signOut(); QTRY_COMPARE(done.count(), 1);
        QVERIFY(!done[0][0].toBool());
        QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), QByteArray("invalid json"));
    }
    void cancelBeforeRequest()
    {
        login();
        std::atomic<bool> cancel{true};
        const auto r = ChatGptTransport::request(server->base() + "/v1/responses", "{}", server->token, false, &cancel);
        QVERIFY(!r.error.isEmpty()); QCOMPARE(server->responses, 0);
    }
};
QTEST_GUILESS_MAIN(ChatGptTest)
#include "ChatGptTest.moc"
