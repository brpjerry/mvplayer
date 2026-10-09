#include "core/Util.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <io.h>
#include <tlhelp32.h>
#endif

#ifdef Q_OS_WIN
namespace {

// A job object holds a child and everything it starts (yt-dlp runs ffmpeg and
// a JS runtime), so they can be stopped together. The system also ends them
// when the handle closes, which covers this process dying without cleaning up.
struct ProcessJob {
    ProcessJob()
    {
        handle = ::CreateJobObjectW(nullptr, nullptr);
        if (!handle)
            return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    ~ProcessJob()
    {
        if (handle)
            ::CloseHandle(handle);
    }
    void add(DWORD pid)
    {
        if (HANDLE process = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid)) {
            if (handle)
                ::AssignProcessToJobObject(handle, process);
            ::CloseHandle(process);
        }
    }
    void terminate()
    {
        if (handle)
            ::TerminateJobObject(handle, 1);
    }
    HANDLE handle = nullptr;
};

// The child is created suspended so that it joins its job before it can start
// anything; this lets it run.
void resumeProcess(DWORD pid)
{
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL more = ::Thread32First(snapshot, &entry); more; more = ::Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid)
            continue;
        if (HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID)) {
            ::ResumeThread(thread);
            ::CloseHandle(thread);
        }
    }
    ::CloseHandle(snapshot);
}

} // namespace
#endif

QString toolsDir()
{
#ifdef Q_OS_WIN
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/mvplayer/tools");
#else
    return {};
#endif
}

QString toolPath(const QString &name)
{
#ifdef Q_OS_WIN
    for (const QString &dir : {toolsDir(), QCoreApplication::applicationDirPath()}) {
        const QString file = dir + QLatin1Char('/') + name + QStringLiteral(".exe");
        if (QFile::exists(file))
            return QDir::toNativeSeparators(file);
    }
#endif
    return name;
}

QString ProcResult::errorText() const
{
    if (!started)
        return QStringLiteral("could not start process");
    if (cancelled)
        return QStringLiteral("cancelled");
    if (timedOut)
        return QStringLiteral("timed out");
    if (stalled)
        return QStringLiteral("the download was too slow and was given up");
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
#ifdef Q_OS_WIN
    ProcessJob job;
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_SUSPENDED; });
#endif
    p.start(QIODevice::ReadOnly);
    if (!p.waitForStarted(10000))
        return res;
#ifdef Q_OS_WIN
    job.add(DWORD(p.processId()));
    resumeProcess(DWORD(p.processId()));
#endif
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
        if (opts.abortIf && opts.abortIf()) {
            res.stalled = true;
            break;
        }
    }
    if (res.cancelled || res.timedOut || res.stalled) {
#ifdef Q_OS_UNIX
        const pid_t group = pid_t(p.processId());
        if (group > 0)
            ::kill(-group, SIGTERM);
        if (!p.waitForFinished(2000)) {
            if (group > 0)
                ::kill(-group, SIGKILL);
            p.waitForFinished(2000);
        }
#elif defined(Q_OS_WIN)
        // Console programs have no window to ask politely; end the whole job.
        job.terminate();
        p.waitForFinished(2000);
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

bool replaceFile(const QString &from, const QString &to)
{
    // Removing `to` first and renaming afterwards loses both files when the
    // machine goes down before the new data is written out (with laptop
    // power settings that can be a minute or two): the removal is on disk,
    // the new file's contents are not, and what remains is an empty file.
#ifdef Q_OS_WIN
    const std::wstring src = QDir::toNativeSeparators(from).toStdWString();
    const std::wstring dst = QDir::toNativeSeparators(to).toStdWString();
    const HANDLE h = ::CreateFileW(src.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const bool flushed = ::FlushFileBuffers(h);
    ::CloseHandle(h);
    return flushed && ::MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    const QByteArray src = QFile::encodeName(from);
    const int fd = ::open(src.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced || ::rename(src.constData(), QFile::encodeName(to).constData()) != 0)
        return false;
    // And the rename itself.
    const int dir = ::open(QFile::encodeName(QFileInfo(to).absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
        ::fsync(dir);
        ::close(dir);
    }
    return true;
#endif
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
    // Windows reserves these names, with or without an extension.
    static const QStringList devices = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"), QStringLiteral("nul"),
        QStringLiteral("com1"), QStringLiteral("com2"), QStringLiteral("com3"), QStringLiteral("com4"),
        QStringLiteral("com5"), QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"),
        QStringLiteral("lpt4"), QStringLiteral("lpt5"), QStringLiteral("lpt6"), QStringLiteral("lpt7"),
        QStringLiteral("lpt8"), QStringLiteral("lpt9"),
    };
    if (devices.contains(out.section(QLatin1Char('.'), 0, 0).trimmed().toLower()))
        out.prepend(QLatin1Char('_'));
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

#ifdef Q_OS_WIN
namespace {
std::function<void()> g_quit;

// Runs on a thread of its own. For anything but Ctrl+C the system ends the
// process as soon as this returns, so give the main thread time to clean up.
BOOL WINAPI onConsoleEvent(DWORD type)
{
    QMetaObject::invokeMethod(QCoreApplication::instance(), [] { g_quit(); }, Qt::QueuedConnection);
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        ::Sleep(4000);
    return TRUE;
}

UINT g_consoleCodePage = 0;
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
#elif defined(Q_OS_WIN)
    g_quit = std::move(quit);
    ::SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#else
    Q_UNUSED(quit);
#endif
}

void initConsole()
{
#ifdef Q_OS_WIN
    // Output that is already going to a file or pipe stays as it is.
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (_fileno(stdout) < 0)
            std::freopen("CONOUT$", "w", stdout);
        if (_fileno(stderr) < 0)
            std::freopen("CONOUT$", "w", stderr);
    }
    g_consoleCodePage = ::GetConsoleOutputCP();
    if (g_consoleCodePage != 0 && g_consoleCodePage != CP_UTF8 && ::SetConsoleOutputCP(CP_UTF8))
        std::atexit([] { ::SetConsoleOutputCP(g_consoleCodePage); });
#endif
}

QByteArray consoleText(const QString &s)
{
#ifdef Q_OS_WIN
    return s.toUtf8();
#else
    return s.toLocal8Bit();
#endif
}
