#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>

class QQuickWindow;

// Keeps the mouse pointer smooth on displays with a variable refresh rate.
// With G-Sync in windowed mode the display follows the rate this window
// presents at, which is low when little is moving (an idle library, a 25 fps
// video), and the pointer is only drawn that often. While the pointer is
// moving over the window this presents every frame; once it rests, the window
// goes back to drawing only what changes. Windows only.
class PointerPacer : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    void attach(QQuickWindow *window);
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void frameDone();

    QQuickWindow *m_window = nullptr;
    QElapsedTimer m_sinceMove;
    QPointF m_last;
    bool m_pacing = false;
};
