#pragma once

#include <QObject>

#ifdef MV_HAVE_DBUS
#include <QDBusVariant>
#endif

// Whether the operating system asks applications for a dark appearance.
// Follows changes while the app runs.
class SystemTheme : public QObject
{
    Q_OBJECT
public:
    explicit SystemTheme(QObject *parent = nullptr);

    bool dark() const { return m_dark; }

signals:
    void darkChanged();

private slots:
#ifdef MV_HAVE_DBUS
    void onPortalSetting(const QString &group, const QString &key, const QDBusVariant &value);
#endif

private:
    void refresh();

    int m_portalScheme = 0; // freedesktop: 0 no preference, 1 dark, 2 light
    bool m_dark = true;
};
