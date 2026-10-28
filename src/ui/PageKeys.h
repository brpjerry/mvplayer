#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>

class QPropertyAnimation;
class QQuickItem;
class QQuickWindow;

// Page Up and Page Down page through the list under the pointer, as the wheel
// scrolls it, or through the library on show while the pointer is over no
// list. No list takes the keyboard (they are scrolled and clicked, never
// focused), so the keys reach the window and are handed on from here.
class PageKeys : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    void attach(QQuickWindow *window);
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QQuickItem *target() const;
    void page(QQuickItem *view, int pages);

    QQuickWindow *m_window = nullptr;
    QPointF m_pointer; // in the window, while m_pointerIn
    bool m_pointerIn = false;
    // The glide of a list that has no wheel of its own (KineticWheel), and
    // where it is going.
    QPointer<QPropertyAnimation> m_glide;
    qreal m_goal = 0;
};
