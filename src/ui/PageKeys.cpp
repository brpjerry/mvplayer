#include "ui/PageKeys.h"

#include <QEasingCurve>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QPropertyAnimation>
#include <QQuickItem>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

// Whether a popup is open in the window's overlay. (The overlay holds more
// than popups: the tooltips' bubbles, hidden until they show.)
bool popupOpen(const QQuickItem *overlay)
{
    const QList<QQuickItem *> items = overlay->childItems();
    return std::any_of(items.cbegin(), items.cend(), [](const QQuickItem *item) {
        return item->isVisible() && item->inherits("QQuickPopupItem");
    });
}

// The item a press at `scenePos` would reach first: the topmost visible and
// enabled item there that takes the mouse or the hover, the one drawn on top
// first and children before their parent.
QQuickItem *itemAt(QQuickItem *item, const QPointF &scenePos)
{
    if (!item->isVisible() || !item->isEnabled())
        return nullptr;
    const QPointF local = item->mapFromScene(scenePos);
    if (item->clip() && !item->contains(local))
        return nullptr;
    QList<QQuickItem *> children = item->childItems();
    std::stable_sort(children.begin(), children.end(), [](QQuickItem *a, QQuickItem *b) { return a->z() < b->z(); });
    for (auto it = children.crbegin(); it != children.crend(); ++it)
        if (QQuickItem *hit = itemAt(*it, scenePos))
            return hit;
    // The overlay stops a press only while a popup is open over the window.
    const bool takes = item->inherits("QQuickOverlay") ? popupOpen(item)
                     : item->acceptedMouseButtons() != Qt::NoButton || item->acceptHoverEvents();
    if (takes && item->contains(local))
        return item;
    return nullptr;
}

// How far a view scrolls, from the top to the bottom of its contentY.
std::pair<qreal, qreal> range(const QQuickItem *view)
{
    const qreal top = view->property("originY").toReal() - view->property("topMargin").toReal();
    const qreal bottom = view->property("originY").toReal() + view->property("contentHeight").toReal()
                       - view->height() + view->property("bottomMargin").toReal();
    return {top, std::max(top, bottom)};
}

// The view's own wheel (KineticWheel), by its glideBy(): declared in the
// view, it is a child of the view's content item.
QObject *wheelOf(const QQuickItem *view)
{
    QList<QObject *> objects = view->children();
    if (const auto *content = view->property("contentItem").value<QQuickItem *>())
        objects += content->children();
    for (QObject *object : std::as_const(objects))
        if (object->metaObject()->indexOfMethod("glideBy(QVariant)") >= 0)
            return object;
    return nullptr;
}

bool scrolls(const QQuickItem *view)
{
    if (!view->inherits("QQuickFlickable"))
        return false;
    const auto [top, bottom] = range(view);
    return bottom - top > 0.5;
}

} // namespace

void PageKeys::attach(QQuickWindow *window)
{
    m_window = window;
    window->installEventFilter(this);
}

bool PageKeys::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
    case QEvent::Enter:
        m_pointer = static_cast<QSinglePointEvent *>(event)->scenePosition();
        m_pointerIn = true;
        break;
    case QEvent::Leave:
        m_pointerIn = false;
        break;
    case QEvent::KeyPress: {
        const auto *key = static_cast<QKeyEvent *>(event);
        const int pages = key->key() == Qt::Key_PageDown ? 1 : key->key() == Qt::Key_PageUp ? -1 : 0;
        if (pages == 0 || (key->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier)
            break;
        if (QQuickItem *view = target()) {
            page(view, pages);
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

QQuickItem *PageKeys::target() const
{
    if (m_pointerIn) {
        for (QQuickItem *item = itemAt(m_window->contentItem(), m_pointer); item; item = item->parentItem()) {
            if (scrolls(item))
                return item;
            // In a popup, or outside a modal one, the library behind it
            // is not what is being looked at.
            if (item->inherits("QQuickPopupItem") || item->inherits("QQuickOverlay"))
                return nullptr;
        }
    }
    // Over no list, or outside the window: the library, while no popup is
    // open over it.
    const QList<QQuickItem *> top = m_window->contentItem()->childItems();
    if (std::any_of(top.cbegin(), top.cend(), [](const QQuickItem *item) { return item->inherits("QQuickOverlay") && popupOpen(item); }))
        return nullptr;
    QVariant list;
    QMetaObject::invokeMethod(m_window, "pageList", Q_RETURN_ARG(QVariant, list));
    auto *view = qobject_cast<QQuickItem *>(list.value<QObject *>());
    return view && scrolls(view) ? view : nullptr;
}

void PageKeys::page(QQuickItem *view, int pages)
{
    // A page of a grid is the rows wholly on show, so the one cut off at
    // the bottom comes up to the top; of a list, most of its height, so a
    // line of what was on show stays on show.
    const qreal cell = view->property("cellHeight").toReal();
    const qreal step = cell > 0 ? std::max(1.0, std::floor(view->height() / cell)) * cell
                                : std::round(view->height() * 0.875);
    const qreal dy = pages * step;
    // The view's own wheel glides it, so that a page and a wheel turn go on
    // from each other.
    if (QObject *wheel = wheelOf(view)) {
        QMetaObject::invokeMethod(wheel, "glideBy", Q_ARG(QVariant, dy));
        return;
    }
    // A list without one glides as the wheel would glide it.
    const auto [topY, bottomY] = range(view);
    const bool gliding = m_glide && m_glide->targetObject() == view && m_glide->state() == QAbstractAnimation::Running;
    m_goal = std::clamp((gliding ? m_goal : view->property("contentY").toReal()) + dy, topY, bottomY);
    if (m_glide)
        m_glide->stop(); // deleted as it stops
    QMetaObject::invokeMethod(view, "cancelFlick");
    auto *glide = new QPropertyAnimation(view, "contentY", view);
    glide->setDuration(230);
    glide->setEasingCurve(QEasingCurve::OutCubic);
    glide->setEndValue(m_goal);
    glide->start(QAbstractAnimation::DeleteWhenStopped);
    m_glide = glide;
}
