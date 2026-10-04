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

#include "core/AudioAlign.h"
#include "core/ImportManager.h"
#include "core/Muxer.h"
#include "core/TagReader.h"
#include "core/Util.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
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
    });
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("Optional: align <track> <video> | mux <track> <video> <out.mkv> | check-video <video> [keyframes]"));
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

    if (!parser.isSet(QStringLiteral("music-dir")))
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
        const QHash<QString, int> c = db.trackStateCounts();
        out << "done: " << db.allVideos().size() << " videos; tracks:";
        for (auto it = c.begin(); it != c.end(); ++it)
            out << " " << it.key() << "=" << it.value();
        out << Qt::endl;
        QCoreApplication::exit(0);
    }, Qt::QueuedConnection);

    quitOnTerminationSignals([] { QCoreApplication::exit(130); });

    mgr.start();
    if (parser.isSet(QStringLiteral("retry")))
        mgr.retryUnmatched();
    const int rc = app.exec();
    mgr.stop();
    return rc;
}
