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
