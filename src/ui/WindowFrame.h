#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

class QQuickWindow;

// On Windows the application draws its own title bar: the system's is removed
// and the window controls sit in the top bar. The window keeps its native
// frame styles, so the shadow, rounded corners, snapping and the minimise and
// maximise animations still come from the system. Elsewhere the window
// manager's decorations are left alone and this does nothing.
class WindowFrame : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    // True where the application provides the window controls and drag area.
    Q_PROPERTY(bool custom READ custom CONSTANT)
    // A maximised window hangs over the screen edges by the width of its
    // (invisible) resize frame; the content keeps this far from the edges.
    Q_PROPERTY(qreal maximizedInset READ maximizedInset NOTIFY maximizedInsetChanged)

public:
    using QObject::QObject;
    ~WindowFrame() override;

    bool custom() const;
    qreal maximizedInset() const { return m_inset; }

    void attach(QQuickWindow *window);
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
    void maximizedInsetChanged();

private:
    void applyStyle();
    void updateInset();

    QQuickWindow *m_window = nullptr;
    qreal m_inset = 0;
};
