#pragma once

#include <QObject>

// Keeps the screen from blanking or locking while a video is playing.
class IdleInhibitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    using QObject::QObject;
    ~IdleInhibitor() override;

    bool active() const { return m_active; }
    void setActive(bool active);

signals:
    void activeChanged();

private:
    void inhibit();
    void release();

    bool m_active = false;
    bool m_held = false;
    quint32 m_cookie = 0;
};
