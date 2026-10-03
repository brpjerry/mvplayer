#include "ui/FrameStats.h"

#include <QQuickWindow>
#include <QScreen>

#include <algorithm>
#include <numeric>

FrameStats::FrameStats(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &FrameStats::publish);
}

void FrameStats::attach(QQuickWindow *window)
{
    m_window = window;
    // frameSwapped is emitted on the render thread; keep the handler there so
    // timestamps are not delayed by whatever the GUI thread is doing.
    connect(window, &QQuickWindow::frameSwapped, this, &FrameStats::onFrameSwapped, Qt::DirectConnection);
    m_timer.start();
}

void FrameStats::setVisible(bool v)
{
    if (v == m_visible)
        return;
    m_visible = v;
    emit visibleChanged();
}

void FrameStats::onFrameSwapped()
{
    const qint64 now = m_clock.nsecsElapsed();
    QMutexLocker lock(&m_mutex);
    if (m_lastNs > 0) {
        const double ms = double(now - m_lastNs) / 1e6;
        // Gaps this long are idle time between animations, not slow frames.
        if (ms < 250) {
            m_recent.push_back(ms);
            if (m_recording)
                m_recorded.push_back(ms);
        }
    }
    m_lastNs = now;
}

void FrameStats::publish()
{
    std::vector<double> recent;
    {
        QMutexLocker lock(&m_mutex);
        recent.swap(m_recent);
    }
    if (m_window && m_window->screen())
        m_refresh = m_window->screen()->refreshRate();
    const double total = std::accumulate(recent.begin(), recent.end(), 0.0);
    m_fps = total > 0 ? qRound(recent.size() * 1000.0 / total) : 0;
    m_worstMs = recent.empty() ? 0 : *std::max_element(recent.begin(), recent.end());
    emit updated();
}

void FrameStats::begin()
{
    QMutexLocker lock(&m_mutex);
    m_recorded.clear();
    m_recording = true;
}

QVariantMap FrameStats::end()
{
    std::vector<double> v;
    {
        QMutexLocker lock(&m_mutex);
        m_recording = false;
        v.swap(m_recorded);
    }
    QVariantMap out;
    out.insert(QStringLiteral("frames"), int(v.size()));
    if (v.empty())
        return out;
    std::sort(v.begin(), v.end());
    const double total = std::accumulate(v.begin(), v.end(), 0.0);
    const double refresh = m_window && m_window->screen() ? m_window->screen()->refreshRate() : 60.0;
    const double budget = 1000.0 / refresh;
    const auto late = std::count_if(v.begin(), v.end(), [&](double ms) { return ms > budget * 1.5; });
    out.insert(QStringLiteral("refreshHz"), refresh);
    out.insert(QStringLiteral("avgFps"), v.size() * 1000.0 / total);
    out.insert(QStringLiteral("medianMs"), v[v.size() / 2]);
    out.insert(QStringLiteral("p99Ms"), v[size_t(double(v.size() - 1) * 0.99)]);
    out.insert(QStringLiteral("maxMs"), v.back());
    out.insert(QStringLiteral("lateFrames"), int(late));
    return out;
}
