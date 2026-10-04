#include "ui/IdleInhibitor.h"

#ifdef MV_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

// Linux: the org.freedesktop.ScreenSaver session service (implemented by
// hypridle, KDE, GNOME, ...). Windows: the calling thread's execution state.
// Other platforms need their own call here (IOPMAssertion on macOS).

IdleInhibitor::~IdleInhibitor()
{
    release();
}

void IdleInhibitor::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active)
        inhibit();
    else
        release();
    emit activeChanged();
}

void IdleInhibitor::inhibit()
{
#ifdef MV_HAVE_DBUS
    if (m_held)
        return;
    QDBusInterface iface(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/org/freedesktop/ScreenSaver"),
                         QStringLiteral("org.freedesktop.ScreenSaver"), QDBusConnection::sessionBus());
    if (!iface.isValid())
        return;
    const QDBusReply<quint32> reply = iface.call(QStringLiteral("Inhibit"), QStringLiteral("mvplayer"),
                                                 QStringLiteral("Playing video"));
    if (reply.isValid()) {
        m_cookie = reply.value();
        m_held = true;
    }
#elif defined(Q_OS_WIN)
    ::SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    m_held = true;
#endif
}

void IdleInhibitor::release()
{
#ifdef MV_HAVE_DBUS
    if (!m_held)
        return;
    QDBusInterface iface(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/org/freedesktop/ScreenSaver"),
                         QStringLiteral("org.freedesktop.ScreenSaver"), QDBusConnection::sessionBus());
    if (iface.isValid())
        iface.call(QStringLiteral("UnInhibit"), m_cookie);
    m_held = false;
#elif defined(Q_OS_WIN)
    if (m_held)
        ::SetThreadExecutionState(ES_CONTINUOUS);
    m_held = false;
#endif
}
