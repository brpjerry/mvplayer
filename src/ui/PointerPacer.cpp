#include "ui/PointerPacer.h"

#include <QMouseEvent>
#include <QQuickWindow>

namespace {
// How long after the last movement frames keep coming.
constexpr qint64 kHoldMs = 300;
} // namespace

void PointerPacer::attach(QQuickWindow *window)
{
#ifdef Q_OS_WIN
    m_window = window;
    window->installEventFilter(this);
    // frameSwapped comes from the render thread; the next frame is asked for
    // on this one.
    connect(window, &QQuickWindow::frameSwapped, this, &PointerPacer::frameDone, Qt::QueuedConnection);
#else
    Q_UNUSED(window);
#endif
}

bool PointerPacer::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseMove) {
        const QPointF pos = static_cast<QMouseEvent *>(event)->globalPosition();
        if (pos != m_last) {
            m_last = pos;
            m_sinceMove.start();
            if (!m_pacing) {
                m_pacing = true;
                m_window->update();
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

void PointerPacer::frameDone()
{
    if (!m_pacing)
        return;
    if (m_sinceMove.elapsed() < kHoldMs)
        m_window->update();
    else
        m_pacing = false;
}
