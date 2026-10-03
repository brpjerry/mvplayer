#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include <vector>

class QQuickWindow;

// Measures real presented-frame intervals of a window. Drives the optional
// on-screen FPS counter and lets tests check animation smoothness.
class FrameStats : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool visible READ visible WRITE setVisible NOTIFY visibleChanged)
    Q_PROPERTY(int fps READ fps NOTIFY updated)
    Q_PROPERTY(double worstMs READ worstMs NOTIFY updated)
    Q_PROPERTY(double refreshRate READ refreshRate NOTIFY updated)

public:
    explicit FrameStats(QObject *parent = nullptr);

    void attach(QQuickWindow *window);

    bool visible() const { return m_visible; }
    void setVisible(bool v);
    int fps() const { return m_fps; }
    double worstMs() const { return m_worstMs; }
    double refreshRate() const { return m_refresh; }

    // Records every frame interval between begin() and end().
    Q_INVOKABLE void begin();
    Q_INVOKABLE QVariantMap end();

signals:
    void visibleChanged();
    void updated();

private:
    void onFrameSwapped(); // render thread
    void publish();

    QQuickWindow *m_window = nullptr;
    QMutex m_mutex;
    QElapsedTimer m_clock;
    qint64 m_lastNs = 0;
    std::vector<double> m_recent;   // intervals since the last publish (ms)
    std::vector<double> m_recorded; // intervals of the current recording (ms)
    bool m_recording = false;

    QTimer m_timer;
    bool m_visible = false;
    int m_fps = 0;
    double m_worstMs = 0;
    double m_refresh = 60;
};
