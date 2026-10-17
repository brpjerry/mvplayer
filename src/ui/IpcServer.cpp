#include "ui/IpcServer.h"

#include <QDebug>
#include <QLocalSocket>
#include <QPointer>

IpcServer::IpcServer(const QString &name, Handler handler, QObject *parent)
    : QObject(parent)
    , m_handler(std::move(handler))
{
    QLocalServer::removeServer(name);
    if (!m_server.listen(name)) {
        qWarning().noquote() << "ipc: cannot listen on" << name << m_server.errorString();
        return;
    }
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *sock = m_server.nextPendingConnection()) {
            connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
            connect(sock, &QLocalSocket::readyRead, sock, [this, sock] {
                // A handler may spin the event loop (a window grab waits for
                // the render thread); a client that half-closed its side
                // is gone by then, so the socket is held weakly and the
                // reply dropped if it went.
                QPointer<QLocalSocket> alive(sock);
                while (alive && alive->canReadLine()) {
                    const QString line = QString::fromUtf8(alive->readLine()).trimmed();
                    if (line.isEmpty())
                        continue;
                    const QString reply = m_handler(line);
                    if (!alive)
                        return;
                    alive->write(reply.toUtf8() + '\n');
                    alive->flush();
                }
            });
        }
    });
}
