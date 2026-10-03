#include "core/Util.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QSocketNotifier>

#include <csignal>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif

QString ProcResult::errorText() const
{
    if (!started)
        return QStringLiteral("could not start process");
    if (cancelled)
        return QStringLiteral("cancelled");
    if (timedOut)
        return QStringLiteral("timed out");
    QString e = QString::fromUtf8(err).trimmed();
    // Keep the last line, which is where yt-dlp/ffmpeg put the actual error.
    const int nl = e.lastIndexOf(QLatin1Char('\n'));
    if (nl >= 0)
        e = e.mid(nl + 1);
    if (e.isEmpty())
        e = QStringLiteral("exit code %1").arg(exitCode);
    return e;
}

ProcResult runProcess(const QString &program, const QStringList &args, const ProcOptions &opts)
{
    ProcResult res;
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    if (!opts.workingDir.isEmpty())
        p.setWorkingDirectory(opts.workingDir);
    p.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_UNIX
    // Own process group, so the helpers a tool starts itself (yt-dlp runs
    // ffmpeg and a JS runtime) can be stopped together with it. On Linux the
    // child is also told to stop if this process dies without cleaning up.
    p.setChildProcessModifier([] {
        ::setpgid(0, 0);
#ifdef Q_OS_LINUX
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    });
#endif
    p.start(QIODevice::ReadOnly);
    if (!p.waitForStarted(10000))
        return res;
    res.started = true;

    QElapsedTimer timer;
    timer.start();
    QByteArray lineBuf;

    auto drain = [&] {
        const QByteArray out = p.readAllStandardOutput();
        if (!out.isEmpty()) {
            if (opts.onLine) {
                lineBuf += out;
                int nl;
                while ((nl = lineBuf.indexOf('\n')) >= 0) {
                    opts.onLine(lineBuf.left(nl));
                    lineBuf.remove(0, nl + 1);
                }
            } else {
                res.out += out;
            }
        }
        res.err += p.readAllStandardError();
        // Only the tail of stderr is ever interesting.
        if (res.err.size() > 256 * 1024)
            res.err.remove(0, res.err.size() - 64 * 1024);
    };

    while (p.state() != QProcess::NotRunning) {
        p.waitForFinished(100);
        drain();
        if (opts.cancel && opts.cancel->load()) {
            res.cancelled = true;
            break;
        }
        if (opts.timeoutMs > 0 && timer.elapsed() > opts.timeoutMs) {
            res.timedOut = true;
            break;
        }
    }
    if (res.cancelled || res.timedOut) {
#ifdef Q_OS_UNIX
        const pid_t group = pid_t(p.processId());
        if (group > 0)
            ::kill(-group, SIGTERM);
        if (!p.waitForFinished(2000)) {
            if (group > 0)
                ::kill(-group, SIGKILL);
            p.waitForFinished(2000);
        }
#else
        p.terminate();
        if (!p.waitForFinished(2000)) {
            p.kill();
            p.waitForFinished(2000);
        }
#endif
        return res;
    }
    drain();
    if (opts.onLine && !lineBuf.isEmpty())
        opts.onLine(lineBuf);
    res.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    return res;
}

QString sanitizeFileName(const QString &name, int maxLen)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        const ushort u = c.unicode();
        if (u < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(c))
            out += QLatin1Char('_');
        else
            out += c;
    }
    out = out.simplified();
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    while (out.startsWith(QLatin1Char('.')))
        out.remove(0, 1);
    if (out.size() > maxLen)
        out = out.left(maxLen).trimmed();
    if (out.isEmpty())
        out = QStringLiteral("Unknown");
    return out;
}

QString foldText(const QString &s)
{
    return s.normalized(QString::NormalizationForm_KC).toCaseFolded();
}

QString formatDuration(double seconds)
{
    const int total = qMax(0, int(seconds + 0.5));
    const int h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

#ifdef Q_OS_UNIX
namespace {
int g_signalPipe[2] = {-1, -1};

// Only async-signal-safe work here: wake the event loop through a pipe.
void onTerminationSignal(int)
{
    const char byte = 1;
    [[maybe_unused]] const ssize_t n = ::write(g_signalPipe[1], &byte, 1);
}
} // namespace
#endif

void quitOnTerminationSignals(std::function<void()> quit)
{
#ifdef Q_OS_UNIX
    if (::pipe(g_signalPipe) != 0)
        return;
    ::fcntl(g_signalPipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(g_signalPipe[1], F_SETFD, FD_CLOEXEC);
    auto *notifier = new QSocketNotifier(g_signalPipe[0], QSocketNotifier::Read, QCoreApplication::instance());
    QObject::connect(notifier, &QSocketNotifier::activated, notifier, [quit, notifier] {
        notifier->setEnabled(false);
        quit();
    });
    std::signal(SIGINT, onTerminationSignal);
    std::signal(SIGTERM, onTerminationSignal);
    std::signal(SIGHUP, onTerminationSignal);
#else
    Q_UNUSED(quit);
#endif
}
