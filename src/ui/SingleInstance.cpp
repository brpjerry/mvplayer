#include "ui/SingleInstance.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QStandardPaths>

QString SingleInstance::nameFor(const QString &configFile, const QString &mvDir)
{
    QString key = QStringLiteral("mvplayer");
    if (!configFile.isEmpty() || !mvDir.isEmpty()) {
        const QString what = (configFile.isEmpty() ? QString() : QFileInfo(configFile).absoluteFilePath())
            + QLatin1Char('\n') + (mvDir.isEmpty() ? QString() : QFileInfo(mvDir).absoluteFilePath());
        key += QLatin1Char('-') + QString::fromLatin1(QCryptographicHash::hash(what.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    }
#ifdef Q_OS_WIN
    return key; // a named pipe, per session
#else
    // A path of the user's own: the runtime directory, else the temporary
    // one with the user's name on it.
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty() || !QFileInfo(dir).isWritable())
        return QDir::tempPath() + QLatin1Char('/') + key + QLatin1Char('-') + qEnvironmentVariable("USER") + QStringLiteral(".sock");
    return dir + QLatin1Char('/') + key + QStringLiteral(".sock");
#endif
}

bool SingleInstance::handOver(const QString &name)
{
    QLocalSocket sock;
    sock.connectToServer(name);
    if (!sock.waitForConnected(1000))
        return false;
    sock.write("raise " + qgetenv("XDG_ACTIVATION_TOKEN") + '\n');
    sock.flush();
    sock.waitForBytesWritten(1000);
    sock.waitForReadyRead(2000); // its "ok", so the request lands before this process ends
    return true;
}

SingleInstance::SingleInstance(const QString &name, QObject *parent)
    : QObject(parent)
{
    // Nobody answered on it (handOver was tried first), so whatever is
    // there is left from an instance that did not get to clean up.
    QLocalServer::removeServer(name);
    if (!m_server.listen(name)) {
        qWarning().noquote() << "single instance: cannot listen on" << name << m_server.errorString();
        return;
    }
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *sock = m_server.nextPendingConnection()) {
            connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
            connect(sock, &QLocalSocket::readyRead, sock, [this, sock] {
                while (sock->canReadLine()) {
                    const QString line = QString::fromUtf8(sock->readLine()).trimmed();
                    if (line.startsWith(QLatin1String("raise"))) {
                        emit raiseRequested(line.mid(5).trimmed());
                        sock->write("ok\n");
                        sock->flush();
                    }
                }
            });
        }
    });
}
