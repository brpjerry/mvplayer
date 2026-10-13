#pragma once

#include "core/AudioAlign.h"
#include "core/Types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>

// The words the judge is given and the shapes of its answers
// (docs/import-rules-judge.html, K): generated into JudgePrompts.cpp.
namespace JudgeWords {
extern const char *const kQueryWords;
extern const char *const kPickWords;
extern const char *const kVerdictWords;
extern const char *const kQuerySchema;
extern const char *const kPickSchema;
extern const char *const kVerdictSchema;
} // namespace JudgeWords

// Judge providers, asked through their APIs: the account's key, kept in a file of this
// user's alone, and whether it works; and the three questions of
// docs/import-rules-judge.html (the query, the pick, the verdict), asked from
// any thread.
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
    // The key as the file has it (any thread; empty when there is none).
    static QString readKey(const QString &keyFile);

    // Asks the API which models the key may use: checked() says.
    void check();
    bool checking() const { return m_checking; }
    // After a check: "ok", "bad" (the key is refused) or "error" (the
    // request did not get through); empty before one.
    QString state() const { return m_state; }
    QString status() const { return m_status; }

    // MVPLAYER_CLAUDE_API names another server (the tests' stand-in).
    static QString apiBase();

    // ---- The models (X4) ----------------------------------------------------
    // The names the settings use: "haiku", "sonnet", "opus".
    static QStringList models();
    static QString modelId(const QString &name);    // "claude-sonnet-5-5"
    static QString modelLabel(const QString &name); // "Sonnet 5.5"
    // What an answer cost, in dollars, at the prices of October 2026 (N).
    static double cost(const QString &modelId, const QJsonObject &usage);
    // The confidence from which the judge's verdict stands alone (N, W1, W3).
    static constexpr double kGate = 0.85;
    static constexpr int kMaxQueries = 2; // Q3
    static constexpr int kMaxPicks = 4;   // S3
    static constexpr int kRetries = 2;    // V5: malformed answers before the lookup is postponed

    // ---- One question --------------------------------------------------------
    struct Answer {
        QJsonObject json;   // the answer, in the fields of the schema
        QString error;      // non-empty: no answer (why)
        bool refused = false; // the key, or the request: the lookup is postponed (V5)
        QJsonObject usage;  // tokens, as the API counts them
        double cost = 0;
        double seconds = 0;
        bool cached = false;
        QString model;
        QString provider = QStringLiteral("claude");
    };
    // Blocking; any thread. `words`: the system prompt; `content`: the facts
    // of the case (text and image blocks); `schemaJson`: the shape of the
    // answer. Answers are kept a day under `cacheDir` (Q), so a lookup
    // repeated within the day asks nothing twice.
    static Answer ask(const QString &key, const QString &modelId, const QString &words, const QJsonArray &content,
                      const QString &schemaJson, int maxTokens, const QString &cacheDir, const std::atomic<bool> *cancel,
                      const QString &provider = QStringLiteral("claude"), const QString &authFile = QString(),
                      const QString &accountId = QString());
    // Keeping everything (README): every question and its answer under
    // `dir`/judge, for good; a replay answers from there where the same
    // question (words, facts and frames alike) was asked before, and asks
    // the API where not. Empty: off.
    static void setCapture(const QString &dir);
    static void setReplay(const QString &dir);
    static QString captureDir();
    static QString replayDir();

    // ---- The picture (L4.1) ------------------------------------------------
    struct Picture {
        bool valid = false;
        QString error;
        QStringList frames;        // JPEG files, 768 px wide
        QVector<double> positions; // where in the video each was taken, 0..1
        double changed = 0;        // share of the picture that ever changes between the frames
        bool edgeOnly = false;     // and that change sits only along the top or bottom edge
    };
    static constexpr int kFrameWidth = 768;
    static constexpr int kCheckWidth = 192;   // the width the frames are compared at
    static constexpr int kCheckLevels = 24;   // grey levels a pixel must move to count as changed
    // Five frames from the file at 10, 35, 50, 65 and 90% of it, and the
    // picture check, written under `outDir`.
    static Picture frames(const QString &file, double durationSec, const QString &outDir, const std::atomic<bool> *cancel);
    static QString pictureText(const Picture &p);

    // ---- The facts of a case, as V1 and S2 list them -----------------------
    struct Upload {
        QString id;
        QString title;
        QString channel;
        QString handle;
        bool verified = false;
        qint64 subscribers = -2; // -2: unknown
        QString date;            // YYYY-MM-DD, or empty
        QString description;     // the first 600 characters of its description, one line; empty: not known
        qint64 views = -1;       // -1: unknown
        bool allowed = false;    // a channel you allowed (G7)
        double duration = 0;
        int width = 0, height = 0;
        int query = 0, rank = 0; // which query it came from, and where (S2)
        QString audio;           // the audio check, in words (V1); empty: not measured
        Picture picture;
        QString pictureNote;     // when no frames: why
    };
    static QString trackLine(const TrackInfo &t);
    static QString audioText(const AudioAlign::Result &r);
    // `sound`: the "Sound:" line for a talk-titled track (Q2), or empty.
    static QJsonArray queryContent(const TrackInfo &t, const QString &sound = QString());
    static QJsonArray pickContent(const TrackInfo &t, const QVector<Upload> &results);
    static QJsonArray verdictContent(const TrackInfo &t, const QVector<TrackInfo> &others, const QVector<Upload> &uploads);
    // The judge's answer about upload `index` (from 1) in a verdict answer.
    static QJsonObject optionAt(const QJsonObject &verdict, int index);

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
