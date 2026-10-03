#include "ui/IpcServer.h"

#include <QDebug>
#include <QLocalSocket>

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
                while (sock->canReadLine()) {
                    const QString line = QString::fromUtf8(sock->readLine()).trimmed();
                    if (line.isEmpty())
                        continue;
                    const QString reply = m_handler(line);
                    sock->write(reply.toUtf8() + '\n');
                    sock->flush();
                }
            });
        }
    });
}
