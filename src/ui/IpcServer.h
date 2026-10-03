#pragma once

#include <QLocalServer>
#include <QObject>

#include <functional>

// Optional local control socket (--ipc <name>) used for UI automation:
// each line received is handed to the handler and its result is written back.
class IpcServer : public QObject
{
    Q_OBJECT
public:
    using Handler = std::function<QString(const QString &)>;

    IpcServer(const QString &name, Handler handler, QObject *parent = nullptr);

private:
    QLocalServer m_server;
    Handler m_handler;
};
