#include "ui/SystemTheme.h"

#include <QGuiApplication>
#include <QStyleHints>

#ifdef MV_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#endif

namespace {

#ifdef MV_HAVE_DBUS
const QString kPortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kPortalSettings = QStringLiteral("org.freedesktop.portal.Settings");
const QString kAppearance = QStringLiteral("org.freedesktop.appearance");
const QString kColorScheme = QStringLiteral("color-scheme");

// The portal wraps its answer in one or two layers of variant.
int schemeFrom(QVariant v)
{
    while (v.canConvert<QDBusVariant>())
        v = v.value<QDBusVariant>().variant();
    return v.toInt();
}
#endif

} // namespace

SystemTheme::SystemTheme(QObject *parent)
    : QObject(parent)
{
#ifdef MV_HAVE_DBUS
    // Linux desktops publish the preference through the settings portal; it
    // is what GTK apps and browsers follow, whatever Qt's own theme says.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kPortalSettings,
                                                           QStringLiteral("Read"));
        call << kAppearance << kColorScheme;
        const QDBusMessage reply = bus.call(call, QDBus::Block, 1000);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
            m_portalScheme = schemeFrom(reply.arguments().constFirst());
        bus.connect(kPortalService, kPortalPath, kPortalSettings, QStringLiteral("SettingChanged"), this,
                    SLOT(onPortalSetting(QString, QString, QDBusVariant)));
    }
#endif
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &SystemTheme::refresh);
    refresh();
}

#ifdef MV_HAVE_DBUS
void SystemTheme::onPortalSetting(const QString &group, const QString &key, const QDBusVariant &value)
{
    if (group != kAppearance || key != kColorScheme)
        return;
    m_portalScheme = schemeFrom(value.variant());
    refresh();
}
#endif

void SystemTheme::refresh()
{
    bool dark = true; // with no preference anywhere, a video player stays dark
    if (m_portalScheme == 1) {
        dark = true;
    } else if (m_portalScheme == 2) {
        dark = false;
    } else {
        const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
        if (scheme != Qt::ColorScheme::Unknown)
            dark = scheme == Qt::ColorScheme::Dark;
    }
    if (dark == m_dark)
        return;
    m_dark = dark;
    emit darkChanged();
}
