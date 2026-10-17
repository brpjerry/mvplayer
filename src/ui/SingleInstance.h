#pragma once

#include <QLocalServer>
#include <QObject>

// One running app per library: a second start hands over to the first,
// which brings its window up (out of the tray, if it is there), and exits.
// The channel is a local socket named for the settings file and library
// given on the command line, so a test run with its own does not reach the
// user's instance.
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    // The socket for this configuration.
    static QString nameFor(const QString &configFile, const QString &mvDir);
    // Asks the instance listening on `name` to bring its window up; true if
    // one was there and took the request. A Wayland activation token this
    // process was started with goes along, so the window may take focus.
    static bool handOver(const QString &name);

    explicit SingleInstance(const QString &name, QObject *parent = nullptr);

signals:
    // A second start asked for the window; the token is empty when none.
    void raiseRequested(const QString &token);

private:
    QLocalServer m_server;
};
