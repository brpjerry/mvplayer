// mvplayer-import: headless front end to the import pipeline.
//
//   mvplayer-import --music-dir ~/Music --mv-dir ~/Videos/MVs
//       Scans the audio library, imports every missing video, then exits.
//   mvplayer-import align <track> <video-or-audio>
//       Prints how the track lines up with the other file's soundtrack.
//   mvplayer-import mux <track> <video> <out.mkv>
//       Runs the audio replacement on a local video file.
//   mvplayer-import check-video <video> [keyframes]
//       Reports whether the picture is a still image.
//   mvplayer-import tags <file> [KEY=value ...]
//       Prints a music file's tags; with assignments, writes them first.

#include "core/ArtistChannels.h"
#include "core/AudioAlign.h"
#include "core/AudioPrint.h"
#include "core/ImportManager.h"
#include "core/Judge.h"
#include "core/ChatGptAuth.h"
#include <QProcess>
#include <QTimer>
#include "core/Muxer.h"
#include "core/MusicBrainz.h"
#include "core/Subtitles.h"
#include "core/LibraryScanner.h"
#include "core/TagReader.h"
#include "core/TagWriter.h"
#include "core/TalkCheck.h"
#include "core/Util.h"
#include "core/YtDlp.h"

#include <QCommandLineParser>
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>

// Qt may route log output to the system journal; a CLI wants it on stderr.
static void logToStderr(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *tag = type == QtWarningMsg ? "warning: "
                    : type == QtCriticalMsg || type == QtFatalMsg ? "error: " : "";
    std::fprintf(stderr, "%s%s\n", tag, consoleText(msg).constData());
}

int main(int argc, char **argv)
{
    initConsole();
    qInstallMessageHandler(logToStderr);
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mvplayer-import"));
    QTextStream out(stdout);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Builds a music video library from an audio library."));
    parser.addHelpOption();
    parser.addOptions({
        {QStringLiteral("music-dir"), QStringLiteral("Video folder: an audio library folder whose tracks are looked up (never modified; repeatable)."), QStringLiteral("dir")},
        {QStringLiteral("library-dir"), QStringLiteral("The music library the player lists: its tracks outside every --music-dir are indexed by their tags, not looked up (repeatable; the video folders alone by default)."), QStringLiteral("dir")},
        {QStringLiteral("mv-dir"), QStringLiteral("Music video library to write."), QStringLiteral("dir"),
         QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath(QStringLiteral("MVs"))},
        {QStringLiteral("jobs"), QStringLiteral("Tracks to process in parallel."), QStringLiteral("n"), QStringLiteral("2")},
        {QStringLiteral("allow-unofficial"), QStringLiteral("Accept uploads that do not look official.")},
        {QStringLiteral("keep-youtube-audio"), QStringLiteral("Never replace the video's audio with the library track.")},
        {QStringLiteral("allow-still-images"), QStringLiteral("Accept videos whose picture never changes.")},
        // The judge's modes: docs/import-rules-judge.html.
        {QStringLiteral("judge"), QStringLiteral("The judge: off, advice (on videos under review), or judge (it runs the lookup)."), QStringLiteral("mode"), QStringLiteral("off")},
        {QStringLiteral("judge-model"), QStringLiteral("Claude: haiku, sonnet or opus. ChatGPT: a model slug from --chatgpt-models."), QStringLiteral("model"), QStringLiteral("sonnet")},
        {QStringLiteral("judge-provider"), QStringLiteral("Judge provider: claude or chatgpt."), QStringLiteral("provider"), QStringLiteral("claude")},
        {QStringLiteral("chatgpt-auth-file"), QStringLiteral("ChatGPT account store; the app's by default."), QStringLiteral("file")},
        {QStringLiteral("chatgpt-account"), QStringLiteral("Saved ChatGPT client ID; 'new' with --chatgpt-login adds an account."), QStringLiteral("id")},
        {QStringLiteral("chatgpt-login"), QStringLiteral("Sign in with ChatGPT in the browser, then exit without opening a library.")},
        {QStringLiteral("chatgpt-logout"), QStringLiteral("Sign out of the selected ChatGPT account, then exit without opening a library.")},
        {QStringLiteral("chatgpt-models"), QStringLiteral("Refresh and list the selected ChatGPT account's models, then exit without opening a library.")},
        {QStringLiteral("claude-key-file"), QStringLiteral("The file holding the Claude API key; the app's by default."), QStringLiteral("file")},
        {QStringLiteral("capture"), QStringLiteral("Keep everything under this folder, for good: every yt-dlp exchange, every file a lookup brings, every question to the judge and its answer."), QStringLiteral("dir")},
        {QStringLiteral("replay"), QStringLiteral("Replay from a capture: searches, pages, downloads and the judge's answers come from this folder wherever it has them; the rest is asked as usual."), QStringLiteral("dir")},
        {QStringLiteral("min-subscribers"), QStringLiteral("Ignore uploads from channels with fewer subscribers, unless the channel is verified or one of several the upload is listed under; 0 ignores none."),
         QStringLiteral("n"), QStringLiteral("5000")},
        {QStringLiteral("allow-channel"), QStringLiteral("Allow a channel, by its id (UC…), its handle (@name) or the address of its page: an artist's channel for every track, and never ignored as a small one. Its videos under review are judged again at once (repeatable)."),
         QStringLiteral("channel")},
        {QStringLiteral("deny-channel"), QStringLiteral("Deny a channel: every video the library has from it is deleted, and its uploads are never taken (repeatable)."),
         QStringLiteral("channel")},
        {QStringLiteral("unlist-channel"), QStringLiteral("Take a channel off the allowed or denied ones (repeatable)."), QStringLiteral("channel")},
        {QStringLiteral("retry"), QStringLiteral("Retry tracks that previously failed or had no video.")},
        {QStringLiteral("ytdlp-arg"), QStringLiteral("Extra argument passed to yt-dlp (repeatable)."), QStringLiteral("arg")},
        {QStringLiteral("cookies"), QStringLiteral("cookies.txt of a YouTube Premium account, for its higher audio bitrate."), QStringLiteral("file")},
        {QStringLiteral("sign-in-on-bot-check"), QStringLiteral("Where YouTube refuses a guest (\"Sign in to confirm you're not a bot\"), make the request with the account (needs --cookies).")},
        {QStringLiteral("approve"), QStringLiteral("Accept a video that waits for review, by its YouTube id, for the candidate track shown first; or for the track of a music file, as <id>=<file> (repeatable)."), QStringLiteral("id")},
        {QStringLiteral("reject"), QStringLiteral("Turn down a video that waits for review, by its YouTube id: it is deleted and not offered to any of its candidate tracks again (repeatable)."), QStringLiteral("id")},
        {QStringLiteral("subtitles"), QStringLiteral("Languages of the subtitles to fetch with each video, e.g. \"en,ja\"."), QStringLiteral("languages")},
        {QStringLiteral("fetch-subtitles"), QStringLiteral("Fetch the subtitles that the videos already imported lack (needs --subtitles).")},
        {QStringLiteral("delete-untracked"), QStringLiteral("After reading the music folders, delete the videos that none of their tracks has.")},
        {QStringLiteral("check-quality"), QStringLiteral("Look at the videos already imported again and rebuild those the account is offered in better quality.")},
        // By today's rules (docs/import-rules.html, H).
        {QStringLiteral("reimport"), QStringLiteral("Look the tracks at this path (a music file, or a folder of them) up again: a video that still fits is kept, one that no longer does goes to review or is replaced (repeatable)."), QStringLiteral("path")},
        {QStringLiteral("reimport-before"), QStringLiteral("Likewise every track with a video whose last lookup was before this time (ISO 8601, e.g. 2026-10-05T16:35 or 2026-10-05)."), QStringLiteral("time")},
        {QStringLiteral("lookup-album"), QStringLiteral("Look an album of the music library up wherever its files are, as <album artist>/<album> (repeatable)."), QStringLiteral("album")},
    });
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("Optional: align <track> <video> | mux <track> <video> <out.mkv> | check-video <video> [keyframes] | same-recording <file> <file> | same-version <track title> <album> <video title> [own] | check-cookies [video id] | convert-subs <file.srv3> <out without extension> | talk-check <file>... | rank-check <track title> <artist> <video title> <channel> [verified] | artist-channels <artist> | music-videos <file> | tags <file> [KEY=value ...] [PICTURE=<image>]"));
    parser.process(app);

    const QString authFile = parser.isSet(QStringLiteral("chatgpt-auth-file"))
        ? QFileInfo(parser.value(QStringLiteral("chatgpt-auth-file"))).absoluteFilePath() : ChatGptAuth::defaultFile();
    const auto authStore = ChatGptAuth::readStore(authFile);
    const QString account = parser.isSet(QStringLiteral("chatgpt-account")) ? parser.value(QStringLiteral("chatgpt-account"))
        : authStore.value(QStringLiteral("selected")).toString();
    const bool login = parser.isSet(QStringLiteral("chatgpt-login"));
    const bool logout = parser.isSet(QStringLiteral("chatgpt-logout"));
    const bool models = parser.isSet(QStringLiteral("chatgpt-models"));
    if (int(login) + int(logout) + int(models) > 1) {
        out << "error: choose one ChatGPT account action" << Qt::endl; return 1;
    }
    if (login || logout || models) {
        ChatGptAuth auth(authFile);
        if (!login && (account.isEmpty() || ChatGptAuth::accountRecord(authFile, account).isEmpty())) {
            out << "error: sign in first with --chatgpt-login" << Qt::endl; return 1;
        }
        if (!login) {
            auth.selectAccount(account);
            if (auth.account() != account) { out << "error: " << auth.status() << Qt::endl; return 1; }
        }
        if (models && !auth.signedIn()) { out << "error: sign in first with --chatgpt-login" << Qt::endl; return 1; }
        QObject::connect(&auth, &ChatGptAuth::openBrowser, &app, [&](const QUrl &url) {
            // This URL has no access, refresh or ID token; useful if launching
            // the system browser fails on a headless machine.
            out << "Open this URL to sign in:\n" << url.toString(QUrl::FullyEncoded) << Qt::endl;
#ifdef Q_OS_WIN
            QProcess::startDetached(QStringLiteral("rundll32"), {QStringLiteral("url.dll,FileProtocolHandler"), url.toString(QUrl::FullyEncoded)});
#elif defined(Q_OS_MACOS)
            QProcess::startDetached(QStringLiteral("open"), {url.toString(QUrl::FullyEncoded)});
#else
            QProcess::startDetached(QStringLiteral("xdg-open"), {url.toString(QUrl::FullyEncoded)});
#endif
        });
        QObject::connect(&auth, &ChatGptAuth::finished, &app, [&](bool ok) {
            out << auth.status() << Qt::endl;
            if (ok && models) for (const auto &m : auth.models())
                out << m.toMap().value(QStringLiteral("name")).toString() << "  " << m.toMap().value(QStringLiteral("label")).toString() << Qt::endl;
            if (ok && login) out << "Account: " << auth.account() << Qt::endl;
            app.exit(ok ? 0 : 1);
        });
        QTimer::singleShot(0, &auth, [&] {
            if (login) auth.signIn(account == QLatin1String("new") ? QString() : account);
            else if (logout) auth.signOut();
            else auth.refreshModels();
        });
        return app.exec();
    }

    const QStringList pos = parser.positionalArguments();
    if (pos.value(0) == QLatin1String("tags")) {
        // tags <file> [KEY=value ...]: the file's tags; with assignments,
        // written first (the tag editor, headless; an empty value removes the
        // tag). With --mv-dir the library takes them at once, as the app
        // does; otherwise its next scan does.
        if (pos.size() < 2)
            parser.showHelp(2);
        const QString file = QFileInfo(pos[1]).absoluteFilePath();
        QHash<QString, QString> changes;
        std::optional<TagWriter::Picture> picture;
        for (int i = 2; i < pos.size(); ++i) {
            const int eq = pos[i].indexOf(QLatin1Char('='));
            if (eq <= 0) {
                out << "error: expected KEY=value, got " << pos[i] << Qt::endl;
                return 2;
            }
            const QString key = pos[i].left(eq).toUpper(), value = pos[i].mid(eq + 1);
            if (key == QLatin1String("PICTURE")) {
                // PICTURE=<image file>: the file's one picture, its front cover.
                QFile img(value);
                if (!img.open(QIODevice::ReadOnly)) {
                    out << "error: cannot read " << value << Qt::endl;
                    return 1;
                }
                const QString suffix = QFileInfo(value).suffix().toLower();
                picture = TagWriter::Picture{img.readAll(), suffix == QLatin1String("png") ? QStringLiteral("image/png")
                                             : suffix == QLatin1String("webp") ? QStringLiteral("image/webp") : QStringLiteral("image/jpeg")};
                continue;
            }
            changes.insert(key, value);
        }
        if (!changes.isEmpty() || picture) {
            QString err;
            if (!TagWriter::write(file, changes, picture, &err)) {
                out << "error: " << err << Qt::endl;
                return 1;
            }
            if (parser.isSet(QStringLiteral("mv-dir"))) {
                const QString mvDir = QDir(parser.value(QStringLiteral("mv-dir"))).absolutePath();
                Database db(QDir(ImportManager::dataDir(mvDir)).filePath(QStringLiteral("library.db")), mvDir);
                if (!db.init(&err)) {
                    out << "error: cannot open library database: " << err << Qt::endl;
                    return 1;
                }
                const TrackInfo known = db.tracksByPath().value(file);
                if (known.id <= 0) {
                    out << "library: no such track" << Qt::endl;
                } else {
                    const LibraryScanner::Result r = LibraryScanner::refreshTracks({known.id}, db);
                    out << "library: " << (r.changed ? "updated" : "unchanged") << ", " << r.videosChanged.size() << " video(s) retagged" << Qt::endl;
                }
            }
        }
        TrackInfo t;
        t.path = file;
        if (!TagReader::read(t)) {
            out << "error: not a readable audio file" << Qt::endl;
            return 1;
        }
        for (auto it = t.tags.constBegin(); it != t.tags.constEnd(); ++it)
            out << it.key() << "=" << it.value().toString() << Qt::endl;
        if (const std::optional<TagWriter::Picture> pic = TagWriter::picture(file))
            out << "PICTURE=" << pic->mime << ", " << pic->data.size() << " bytes" << Qt::endl;
        return 0;
    }

    if (pos.value(0) == QLatin1String("align")) {
        if (pos.size() != 3)
            parser.showHelp(2);
        std::vector<int16_t> a, b;
        QString err;
        if (!AudioAlign::decodeMono(pos[1], &a, nullptr, &err) || !AudioAlign::decodeMono(pos[2], &b, nullptr, &err)) {
            out << "error: " << err << Qt::endl;
            return 1;
        }
        out << AudioAlign::align(a, b).summary() << Qt::endl;
        return 0;
    }

    if (pos.value(0) == QLatin1String("same-recording")) {
        if (pos.size() != 3)
            parser.showHelp(2);
        AudioPrint::Print a, b;
        QString err;
        if (!AudioPrint::ofFile(pos[1], &a, nullptr, &err) || !AudioPrint::ofFile(pos[2], &b, nullptr, &err)) {
            out << "error: " << err << Qt::endl;
            return 1;
        }
        out << (AudioPrint::sameRecording(a, b) ? "same" : "different") << ": "
            << QString::number(AudioPrint::distance(a, b) * 100, 'f', 1) << "% of fingerprint bits differ, "
            << a.size() << " and " << b.size() << " items" << Qt::endl;
        return 0;
    }

    if (pos.value(0) == QLatin1String("same-version")) {
        if (pos.size() < 4 || pos.size() > 5)
            parser.showHelp(2);
        TrackInfo t;
        t.title = pos[1];
        t.album = pos[2];
        out << (Matcher::sameVersion(t, pos[3], pos.value(4) == QLatin1String("own")) ? "same" : "different") << Qt::endl;
        return 0;
    }

    if (pos.value(0) == QLatin1String("convert-subs")) {
        if (pos.size() != 3)
            parser.showHelp(2);
        QString err;
        const QString written = Subtitles::convertSrv3(pos[1], pos[2], &err);
        if (written.isEmpty()) {
            out << (err.isEmpty() ? QStringLiteral("no subtitles in it") : QStringLiteral("error: ") + err) << Qt::endl;
            return 1;
        }
        out << "wrote " << written << Qt::endl;
        return 0;
    }

    if (pos.value(0) == QLatin1String("talk-check")) {
        if (pos.size() < 2)
            parser.showHelp(2);
        for (const QString &file : pos.mid(1)) {
            std::vector<int16_t> pcm;
            QString err;
            if (!AudioAlign::decodeMono(file, &pcm, nullptr, &err)) {
                out << "error: " << err << Qt::endl;
                return 1;
            }
            TrackInfo t;
            t.path = file;
            TagReader::read(t);
            const TalkCheck::Result r = TalkCheck::measure(pcm);
            out << (r.talk ? "talk " : r.valid ? "music" : "short") << " pauses " << QString::number(r.pauses, 'f', 2)
                << " beat " << QString::number(r.beat, 'f', 2) << " title " << (Matcher::isTalkTitle(t.title) ? "talk" : "-")
                << "\t" << file << Qt::endl;
        }
        return 0;
    }

    if (pos.value(0) == QLatin1String("rank-check")) {
        if (pos.size() < 5 || pos.size() > 6)
            parser.showHelp(2);
        TrackInfo t;
        t.title = pos[1];
        t.artist = pos[2];
        YtCandidate c;
        c.id = QStringLiteral("x");
        c.title = pos[3];
        c.channel = pos[4];
        c.verified = pos.value(5) == QLatin1String("verified");
        QVector<YtCandidate> list{c};
        Matcher::rank(t, list);
        out << (list[0].rejectReason.isEmpty() ? (list[0].ownChannel ? "the artist's channel" : list[0].trusted ? "trusted" : "not trusted") : "dropped")
            << (list[0].rejectReason.isEmpty() ? QString() : QStringLiteral(": ") + list[0].rejectReason)
            << (Matcher::namesArtist(t, c.title, c.channel) ? ", names the artist" : ", does not name the artist") << Qt::endl;
        return 0;
    }

    if (pos.value(0) == QLatin1String("artist-channels")) {
        if (pos.size() != 2)
            parser.showHelp(2);
        QString err;
        const QStringList ids = ArtistChannels::fromMusicBrainz(pos[1], &err);
        if (!err.isEmpty()) {
            out << "error: " << err << Qt::endl;
            return 1;
        }
        out << (ids.isEmpty() ? QStringLiteral("MusicBrainz lists no YouTube channel") : QStringLiteral("MusicBrainz: ") + ids.join(QStringLiteral(", "))) << Qt::endl;
        return 0;
    }

    if (pos.value(0) == QLatin1String("music-videos")) {
        // What MusicBrainz links as the music video of a file's recording
        // (docs/import-rules.html, B7–B12, B15), asked now, nothing kept.
        if (pos.size() != 2)
            parser.showHelp(2);
        TrackInfo t;
        t.path = pos[1];
        if (!TagReader::read(t)) {
            out << "error: not readable audio: " << pos[1] << Qt::endl;
            return 1;
        }
        // AcoustID hears the file as the library does (B15).
        AudioPrint::Print print;
        QString err, acoustidErr;
        if (!AudioPrint::ofFile(t.path, &print, nullptr, &err)) {
            out << "error: " << err << Qt::endl;
            return 1;
        }
        QStringList ways;
        const QStringList recordings = MusicBrainz::recordingsOf(t, AudioPrint::pack(print), &ways, &err, &acoustidErr);
        const QStringList uploads = err.isEmpty() && !recordings.isEmpty() ? MusicBrainz::musicVideosOf(recordings, &err) : QStringList();
        if (!err.isEmpty()) {
            out << "error: " << err << Qt::endl;
            return 1;
        }
        if (!acoustidErr.isEmpty())
            out << "AcoustID could not be asked: " << acoustidErr << Qt::endl;
        out << "recordings" << (ways.isEmpty() ? QString() : QStringLiteral(" (by %1)").arg(ways.join(QStringLiteral(", ")))) << ": "
            << (recordings.isEmpty() ? QStringLiteral("none") : recordings.join(QStringLiteral(", "))) << Qt::endl
            << "music videos on YouTube: " << (uploads.isEmpty() ? QStringLiteral("none") : uploads.join(QStringLiteral(", "))) << Qt::endl;
        return 0;
    }

    if (pos.value(0) == QLatin1String("check-video")) {
        if (pos.size() < 2 || pos.size() > 3)
            parser.showHelp(2);
        const Muxer::StillCheck c = Muxer::checkStill(pos[1], pos.value(2) == QLatin1String("keyframes"), nullptr);
        if (!c.valid) {
            out << "error: cannot analyse " << pos[1] << Qt::endl;
            return 1;
        }
        out << (c.still ? "still" : "moving") << ": " << qRound(c.movingShare * 100) << "% of " << c.samples - 1
            << " sampled frame pairs changed" << Qt::endl;
        return 0;
    }

    if (pos.value(0) == QLatin1String("mux")) {
        if (pos.size() != 4)
            parser.showHelp(2);
        TrackInfo track;
        track.path = QFileInfo(pos[1]).absoluteFilePath();
        std::vector<int16_t> a, b;
        QString err;
        if (!TagReader::read(track) || !AudioAlign::decodeMono(track.path, &a, nullptr, &err)
            || !AudioAlign::decodeMono(pos[2], &b, nullptr, &err)) {
            out << "error: " << (err.isEmpty() ? QStringLiteral("cannot read track") : err) << Qt::endl;
            return 1;
        }
        QTemporaryDir work(QFileInfo(pos[3]).absolutePath() + QStringLiteral("/mux-XXXXXX"));
        Muxer::Plan plan;
        plan.videoFile = pos[2];
        plan.ytAudioFile = pos[2];
        plan.workDir = work.path();
        plan.outFile = QFileInfo(pos[3]).absoluteFilePath();
        plan.track = track;
        plan.ytId = QStringLiteral("local");
        plan.align = AudioAlign::align(a, b);
        plan.replaceAudio = true;
        out << plan.align.summary() << Qt::endl;
        QString detail;
        Muxer::ReviewSpan span;
        if (!Muxer::mux(plan, nullptr, &detail, &err, &span)) {
            out << "error: " << err << Qt::endl;
            return 1;
        }
        out << "wrote " << plan.outFile << " (" << detail << ")" << Qt::endl;
        // Where the track sits in the video, as the library keeps it.
        out << "spans: " << audioSpansText(span.spans) << Qt::endl;
        return 0;
    }

    if (!parser.isSet(QStringLiteral("music-dir")) && !parser.isSet(QStringLiteral("library-dir"))
        && pos.value(0) != QLatin1String("check-cookies"))
        parser.showHelp(2);

    ImportSettings cfg;
    for (const QString &d : parser.values(QStringLiteral("music-dir")))
        cfg.musicDirs << QDir(d).absolutePath();
    for (const QString &d : parser.values(QStringLiteral("library-dir")))
        cfg.libraryDirs << QDir(d).absolutePath();
    cfg.mvDir = QDir(parser.value(QStringLiteral("mv-dir"))).absolutePath();
    cfg.concurrency = parser.value(QStringLiteral("jobs")).toInt();
    cfg.allowUnofficial = parser.isSet(QStringLiteral("allow-unofficial"));
    cfg.replaceAudio = !parser.isSet(QStringLiteral("keep-youtube-audio"));
    cfg.skipStillImages = !parser.isSet(QStringLiteral("allow-still-images"));
    cfg.judgeMode = parser.value(QStringLiteral("judge"));
    if (cfg.judgeMode != QLatin1String("off") && cfg.judgeMode != QLatin1String("advice") && cfg.judgeMode != QLatin1String("judge")) {
        out << "error: --judge takes off, advice or judge" << Qt::endl;
        return 1;
    }
    cfg.judgeProvider = parser.value(QStringLiteral("judge-provider"));
    if (cfg.judgeProvider != QLatin1String("claude") && cfg.judgeProvider != QLatin1String("chatgpt")) {
        out << "error: --judge-provider takes claude or chatgpt" << Qt::endl; return 1;
    }
    cfg.chatGptAuthFile = authFile;
    cfg.chatGptAccount = account;
    cfg.judgeModel = parser.value(QStringLiteral("judge-model"));
    if (cfg.judgeProvider == QLatin1String("chatgpt") && !parser.isSet(QStringLiteral("judge-model")))
        cfg.judgeModel = ChatGptAuth::accountRecord(authFile, account).value(QStringLiteral("model")).toString();
    if (cfg.judgeProvider == QLatin1String("claude") && !Judge::models().contains(cfg.judgeModel)) {
        out << "error: --judge-model takes haiku, sonnet or opus" << Qt::endl;
        return 1;
    }
    if (parser.isSet(QStringLiteral("capture")))
        cfg.captureDir = QFileInfo(parser.value(QStringLiteral("capture"))).absoluteFilePath();
    if (parser.isSet(QStringLiteral("replay")))
        cfg.replayDir = QFileInfo(parser.value(QStringLiteral("replay"))).absoluteFilePath();
    cfg.judgeKeyFile = parser.isSet(QStringLiteral("claude-key-file"))
        ? QFileInfo(parser.value(QStringLiteral("claude-key-file"))).absoluteFilePath()
        : QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath(QStringLiteral("mvplayer/claude-api-key"));
    if (cfg.judgeProvider == QLatin1String("claude") && cfg.judgeMode == QLatin1String("judge") && Judge::readKey(cfg.judgeKeyFile).isEmpty()) {
        out << "error: --judge judge needs a Claude API key in " << cfg.judgeKeyFile << Qt::endl;
        return 1;
    }
    if (cfg.judgeProvider == QLatin1String("chatgpt") && cfg.judgeMode != QLatin1String("off")
        && (cfg.judgeModel.isEmpty() || !ChatGptAuth::hasSession(authFile, account))) {
        out << "error: sign in with --chatgpt-login, then choose --judge-model from --chatgpt-models" << Qt::endl; return 1;
    }
    cfg.minSubscribers = qMax(0, parser.value(QStringLiteral("min-subscribers")).toInt());
    cfg.ytdlpArgs = parser.values(QStringLiteral("ytdlp-arg"));
    cfg.subtitleLangs = parser.value(QStringLiteral("subtitles")).split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
    if (parser.isSet(QStringLiteral("fetch-subtitles")) && cfg.subtitleLangs.isEmpty()) {
        out << "error: --fetch-subtitles needs --subtitles" << Qt::endl;
        return 1;
    }
    if (parser.isSet(QStringLiteral("cookies"))) {
        cfg.cookiesFile = QFileInfo(parser.value(QStringLiteral("cookies"))).absoluteFilePath();
        if (!YtDlp::looksLikeCookies(cfg.cookiesFile)) {
            out << "error: " << cfg.cookiesFile << " is not a cookies.txt with YouTube cookies" << Qt::endl;
            return 1;
        }
    }
    cfg.accountOnBotCheck = parser.isSet(QStringLiteral("sign-in-on-bot-check"));
    if (cfg.accountOnBotCheck && cfg.cookiesFile.isEmpty()) {
        out << "error: --sign-in-on-bot-check needs --cookies" << Qt::endl;
        return 1;
    }
    if (pos.value(0) == QLatin1String("check-cookies")) {
        if (cfg.cookiesFile.isEmpty()) {
            out << "error: check-cookies needs --cookies" << Qt::endl;
            return 1;
        }
        YtDlp yt(qEnvironmentVariable("MVPLAYER_YTDLP", toolPath(QStringLiteral("yt-dlp"))), cfg.ytdlpArgs, cfg.cookiesFile, nullptr);
        double kbps = 0;
        QString err;
        switch (yt.checkAccount(pos.value(1, QStringLiteral("dQw4w9WgXcQ")), &kbps, &err)) {
        case YtDlp::Account::Premium:
            out << "valid: the account is offered audio at " << qRound(kbps) << " kbit/s" << Qt::endl;
            return 0;
        case YtDlp::Account::Ordinary:
            out << "accepted, but no Premium audio is offered: " << qRound(kbps) << " kbit/s at best" << Qt::endl;
            return 1;
        case YtDlp::Account::Expired:
            out << "expired: " << err << Qt::endl;
            return 1;
        case YtDlp::Account::Unknown:
            out << "error: " << err << Qt::endl;
            return 1;
        }
    }
    QDateTime reimportBefore;
    if (parser.isSet(QStringLiteral("reimport-before"))) {
        const QString given = parser.value(QStringLiteral("reimport-before"));
        reimportBefore = QDateTime::fromString(given, Qt::ISODate);
        if (!reimportBefore.isValid())
            reimportBefore = QDate::fromString(given, Qt::ISODate).startOfDay();
        if (!reimportBefore.isValid()) {
            out << "error: --reimport-before wants a time like 2026-10-05T16:35 or 2026-10-05, not " << given << Qt::endl;
            return 1;
        }
    }
    if (parser.isSet(QStringLiteral("check-quality")) && cfg.cookiesFile.isEmpty()) {
        out << "error: --check-quality needs --cookies" << Qt::endl;
        return 1;
    }
    if (qEnvironmentVariableIsSet("MVPLAYER_PAUSE_SECS"))
        cfg.pauseBaseSecs = qEnvironmentVariableIntValue("MVPLAYER_PAUSE_SECS");

    QDir().mkpath(ImportManager::dataDir(cfg.mvDir));
    Database db(QDir(ImportManager::dataDir(cfg.mvDir)).filePath(QStringLiteral("library.db")), cfg.mvDir);
    QString err;
    if (!db.init(&err)) {
        out << "error: cannot open library database: " << err << Qt::endl;
        return 1;
    }
    // Without the music library named, the video folders are the whole
    // library: refused where it holds tracks outside them, which the scan
    // would take for gone.
    if (cfg.libraryDirs.isEmpty()) {
        int outside = 0;
        for (const QString &p : db.pathsNotLookedUp()) {
            bool under = false;
            for (const QString &d : std::as_const(cfg.musicDirs))
                under |= p.startsWith(d.endsWith(QLatin1Char('/')) ? d : d + QLatin1Char('/'), pathCase);
            outside += under ? 0 : 1;
        }
        if (outside > 0) {
            out << "error: the library holds " << outside << " tracks indexed for the music player outside the folders given;"
                << " name the music library with --library-dir" << Qt::endl;
            return 2;
        }
    }
    for (const QString &a : parser.values(QStringLiteral("lookup-album"))) {
        const int slash = a.lastIndexOf(QLatin1Char('/'));
        if (slash <= 0 || slash == a.size() - 1) {
            out << "error: --lookup-album takes <album artist>/<album>" << Qt::endl;
            return 2;
        }
        db.addLookupAlbum(a.left(slash), a.mid(slash + 1));
        out << "looking up the album " << a.mid(slash + 1) << " by " << a.left(slash) << Qt::endl;
    }

    ImportManager mgr(&db);
    mgr.setSettings(cfg);
    QObject::connect(&mgr, &ImportManager::jobChanged, &app, [&](const JobStatus &s) {
        if (s.finished || s.progress > 0)
            return; // results are logged by the manager; skip download percentages
        out << "[" << s.title << "] " << s.stage << (s.detail.isEmpty() ? QString() : QStringLiteral(": ") + s.detail)
            << Qt::endl;
    });
    QObject::connect(&mgr, &ImportManager::idle, &app, [&] {
        if (parser.isSet(QStringLiteral("delete-untracked"))) {
            const int n = mgr.deleteUntracked();
            out << "deleted " << n << " untracked videos" << Qt::endl;
        }
        const QHash<QString, int> c = db.trackStateCounts();
        const QVector<VideoInfo> videos = db.allVideos();
        const auto waiting = std::count_if(videos.begin(), videos.end(), [](const VideoInfo &v) { return v.review; });
        out << "done: " << videos.size() - waiting << " videos";
        if (waiting > 0)
            out << " and " << waiting << " for review";
        out << "; tracks:";
        for (auto it = c.begin(); it != c.end(); ++it)
            out << " " << it.key() << "=" << it.value();
        out << Qt::endl;
        QCoreApplication::exit(0);
    }, Qt::QueuedConnection);

    quitOnTerminationSignals([] { QCoreApplication::exit(130); });

    mgr.start();
    if (parser.isSet(QStringLiteral("retry")))
        mgr.retryUnmatched();
    if (parser.isSet(QStringLiteral("check-quality")))
        mgr.checkQuality();
    if (parser.isSet(QStringLiteral("fetch-subtitles")))
        mgr.fetchSubtitles();
    for (const QString &path : parser.values(QStringLiteral("reimport"))) {
        const int n = mgr.reimportPath(QFileInfo(path).absoluteFilePath());
        out << "re-importing " << n << " tracks at " << path << Qt::endl;
    }
    if (reimportBefore.isValid()) {
        const int n = mgr.reimport(db.trackIdsImportedBefore(reimportBefore.toSecsSinceEpoch()));
        out << "re-importing " << n << " tracks last looked up before " << reimportBefore.toString(Qt::ISODate) << Qt::endl;
    }
    // A channel by its id, or by its handle where the library has a video
    // of it: a handle alone does not say which channel it is.
    const auto channelNamed = [&](const QString &typed) -> std::optional<Database::ListedChannel> {
        const QString ref = YtDlp::channelRef(typed);
        const Database::ListedChannel c = ref.isEmpty() ? Database::ListedChannel() : db.knownChannel(ref);
        if (c.id.isEmpty()) {
            out << (ref.isEmpty() ? "not a channel's id (UC…), handle (@name) or address: " : "the library has no video of the channel ") << typed
                << (ref.isEmpty() ? "" : ": give its id (UC…)") << Qt::endl;
            return std::nullopt;
        }
        return c;
    };
    for (const QString &typed : parser.values(QStringLiteral("allow-channel"))) {
        if (const auto c = channelNamed(typed))
            mgr.allowChannel(c->id, c->name, c->handle);
    }
    for (const QString &typed : parser.values(QStringLiteral("deny-channel"))) {
        if (const auto c = channelNamed(typed))
            mgr.denyChannel(c->id, c->name, c->handle);
    }
    for (const QString &typed : parser.values(QStringLiteral("unlist-channel"))) {
        if (const auto c = channelNamed(typed))
            mgr.unlistChannel(c->id);
    }
    for (const QString &arg : parser.values(QStringLiteral("approve"))) {
        // (A YouTube id has no "=" in it.)
        const qsizetype eq = arg.indexOf(QLatin1Char('='));
        const auto v = db.videoByYtId(eq < 0 ? arg : arg.left(eq));
        if (!v) {
            out << "no video " << arg.left(eq < 0 ? arg.size() : eq) << " in the library" << Qt::endl;
            continue;
        }
        qint64 trackId = 0;
        if (eq >= 0) {
            const QVector<qint64> ids = db.trackIdsUnder(QFileInfo(arg.mid(eq + 1)).absoluteFilePath());
            if (ids.isEmpty()) {
                out << "no track at " << arg.mid(eq + 1) << Qt::endl;
                continue;
            }
            trackId = ids.first();
        }
        mgr.approveVideo(v->id, trackId);
    }
    for (const QString &id : parser.values(QStringLiteral("reject"))) {
        if (const auto v = db.videoByYtId(id))
            mgr.rejectVideo(v->id);
    }
    const int rc = app.exec();
    mgr.stop();
    return rc;
}
