#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

struct ProcResult {
    bool started = false;
    bool cancelled = false;
    bool timedOut = false;
    int exitCode = -1;
    QByteArray out;
    QByteArray err;

    bool ok() const { return started && !cancelled && !timedOut && exitCode == 0; }
    QString errorText() const;
};

struct ProcOptions {
    const std::atomic<bool> *cancel = nullptr;
    // Called for each complete stdout line. When set, stdout is not accumulated.
    std::function<void(const QByteArray &)> onLine;
    int timeoutMs = -1;
    QString workingDir;
};

// Folder comparisons follow the file system: Windows ignores case.
#ifdef Q_OS_WIN
inline constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
inline constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif

// Windows has no package manager to supply yt-dlp, so the app keeps its own
// copy (and yt-dlp's JS runtime) in this per-user folder. Empty elsewhere.
QString toolsDir();

// The program to start for a helper such as "yt-dlp". On Windows that is the
// copy in toolsDir() or beside the application when there is one; otherwise
// the bare name, which is looked up on PATH.
QString toolPath(const QString &name);

// Runs a child process to completion on the calling thread (no event loop needed).
ProcResult runProcess(const QString &program, const QStringList &args, const ProcOptions &opts = {});

// Makes a string safe to use as a single path component on Linux, Windows and macOS.
QString sanitizeFileName(const QString &name, int maxLen = 120);

// Lower-cased, NFKC-normalised text for case/width-insensitive matching.
QString foldText(const QString &s);

QString formatDuration(double seconds);

// Calls `quit` on the main thread when the process is asked to stop (Ctrl+C,
// session logout, `kill`), so running imports are cancelled and cleaned up
// instead of being cut off. Call once, after the application object exists.
void quitOnTerminationSignals(std::function<void()> quit);

// Windows: lets a program started from a terminal print there (a GUI program
// has no console of its own) and switches the terminal to UTF-8. Call first
// thing in main(). Does nothing elsewhere.
void initConsole();

// Text encoded for stdout/stderr.
QByteArray consoleText(const QString &s);
