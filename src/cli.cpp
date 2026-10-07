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

#include "core/ArtistChannels.h"
#include "core/AudioAlign.h"
#include "core/AudioPrint.h"
#include "core/ImportManager.h"
#include "core/Muxer.h"
#include "core/Subtitles.h"
#include "core/TagReader.h"
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
        {QStringLiteral("music-dir"), QStringLiteral("Audio library folder to read (never modified; repeatable)."), QStringLiteral("dir")},
        {QStringLiteral("mv-dir"), QStringLiteral("Music video library to write."), QStringLiteral("dir"),
         QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath(QStringLiteral("MVs"))},
        {QStringLiteral("jobs"), QStringLiteral("Tracks to process in parallel."), QStringLiteral("n"), QStringLiteral("2")},
        {QStringLiteral("allow-unofficial"), QStringLiteral("Accept uploads that do not look official.")},
        {QStringLiteral("keep-youtube-audio"), QStringLiteral("Never replace the video's audio with the library track.")},
        {QStringLiteral("allow-still-images"), QStringLiteral("Accept videos whose picture never changes.")},
        {QStringLiteral("retry"), QStringLiteral("Retry tracks that previously failed or had no video.")},
        {QStringLiteral("ytdlp-arg"), QStringLiteral("Extra argument passed to yt-dlp (repeatable)."), QStringLiteral("arg")},
        {QStringLiteral("cookies"), QStringLiteral("cookies.txt of a YouTube Premium account, for its higher audio bitrate."), QStringLiteral("file")},
        {QStringLiteral("approve"), QStringLiteral("Accept a video that waits for review, by its YouTube id (repeatable)."), QStringLiteral("id")},
        {QStringLiteral("reject"), QStringLiteral("Turn down a video that waits for review, by its YouTube id: it is deleted and its tracks have no video (repeatable)."), QStringLiteral("id")},
        {QStringLiteral("subtitles"), QStringLiteral("Languages of the subtitles to fetch with each video, e.g. \"en,ja\"."), QStringLiteral("languages")},
        {QStringLiteral("fetch-subtitles"), QStringLiteral("Fetch the subtitles that the videos already imported lack (needs --subtitles).")},
        {QStringLiteral("delete-untracked"), QStringLiteral("After reading the music folders, delete the videos that none of their tracks has.")},
        {QStringLiteral("check-quality"), QStringLiteral("Look at the videos already imported again and rebuild those the account is offered in better quality.")},
        {QStringLiteral("reimport"), QStringLiteral("Look the tracks at this path (a music file, or a folder of them) up again by today's rules: a video that still fits is kept, one that no longer does goes to review or is replaced (repeatable)."), QStringLiteral("path")},
        {QStringLiteral("reimport-before"), QStringLiteral("Likewise every track with a video whose last lookup was before this time (ISO 8601, e.g. 2026-10-05T16:35 or 2026-10-05)."), QStringLiteral("time")},
    });
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("Optional: align <track> <video> | mux <track> <video> <out.mkv> | check-video <video> [keyframes] | same-recording <file> <file> | same-version <track title> <album> <video title> [own] | check-cookies [video id] | convert-subs <file.srv3> <out without extension> | talk-check <file>... | rank-check <track title> <artist> <video title> <channel> [verified] | artist-channels <artist>"));
    parser.process(app);

    const QStringList pos = parser.positionalArguments();
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
        if (!Muxer::mux(plan, nullptr, &detail, &err)) {
            out << "error: " << err << Qt::endl;
            return 1;
        }
        out << "wrote " << plan.outFile << " (" << detail << ")" << Qt::endl;
        return 0;
    }

    if (!parser.isSet(QStringLiteral("music-dir")) && pos.value(0) != QLatin1String("check-cookies"))
        parser.showHelp(2);

    ImportSettings cfg;
    for (const QString &d : parser.values(QStringLiteral("music-dir")))
        cfg.musicDirs << QDir(d).absolutePath();
    cfg.mvDir = QDir(parser.value(QStringLiteral("mv-dir"))).absolutePath();
    cfg.concurrency = parser.value(QStringLiteral("jobs")).toInt();
    cfg.allowUnofficial = parser.isSet(QStringLiteral("allow-unofficial"));
    cfg.replaceAudio = !parser.isSet(QStringLiteral("keep-youtube-audio"));
    cfg.skipStillImages = !parser.isSet(QStringLiteral("allow-still-images"));
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
    for (const QString &id : parser.values(QStringLiteral("approve"))) {
        if (const auto v = db.videoByYtId(id))
            mgr.approveVideo(v->id);
    }
    for (const QString &id : parser.values(QStringLiteral("reject"))) {
        if (const auto v = db.videoByYtId(id))
            mgr.rejectVideo(v->id);
    }
    const int rc = app.exec();
    mgr.stop();
    return rc;
}
