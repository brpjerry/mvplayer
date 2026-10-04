#include "ui/WindowFrame.h"

#include <QGuiApplication>
#include <QQuickWindow>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#endif

WindowFrame::~WindowFrame()
{
    if (m_window)
        qApp->removeNativeEventFilter(this);
}

bool WindowFrame::custom() const
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

void WindowFrame::attach(QQuickWindow *window)
{
#ifdef Q_OS_WIN
    m_window = window;
    qApp->installNativeEventFilter(this);
    applyStyle();
    updateInset();
    connect(window, &QWindow::visibilityChanged, this, [this] {
        applyStyle();
        updateInset();
    });
    connect(window, &QWindow::screenChanged, this, &WindowFrame::updateInset);
#else
    Q_UNUSED(window);
#endif
}

// The window is created frameless (Main.qml), which keeps Qt's idea of its
// geometry simple: no frame margins. Putting the frame styles back makes the
// system treat it as a normal resizable window again; WM_NCCALCSIZE below
// then gives the whole of it to the application.
void WindowFrame::applyStyle()
{
#ifdef Q_OS_WIN
    // Qt manages the styles of a full-screen window itself.
    if (!m_window || m_window->visibility() == QWindow::FullScreen || m_window->visibility() == QWindow::Hidden)
        return;
    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR wanted = style | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    if (wanted == style)
        return;
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, wanted);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    // Windows 11: rounded corners (DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND). Ignored on Windows 10.
    const DWORD round = 2;
    ::DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));
#endif
}

void WindowFrame::updateInset()
{
#ifdef Q_OS_WIN
    qreal inset = 0;
    if (m_window && m_window->visibility() == QWindow::Maximized) {
        const UINT dpi = ::GetDpiForWindow(reinterpret_cast<HWND>(m_window->winId()));
        inset = (::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi))
                / m_window->devicePixelRatio();
    }
    if (qFuzzyCompare(inset + 1, m_inset + 1))
        return;
    m_inset = inset;
    emit maximizedInsetChanged();
#endif
}

bool WindowFrame::nativeEventFilter(const QByteArray &, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    const MSG *msg = static_cast<const MSG *>(message);
    if (!m_window || !m_window->handle() || msg->hwnd != reinterpret_cast<HWND>(m_window->winId()))
        return false;

    switch (msg->message) {
    case WM_NCCALCSIZE:
        // No title bar or borders: the client area is the whole window.
        if (msg->wParam) {
            *result = 0;
            return true;
        }
        break;
    case WM_NCHITTEST: {
        // With no border left, the outer few pixels of the window resize it.
        if (m_window->visibility() != QWindow::Windowed)
            break;
        RECT r;
        ::GetWindowRect(msg->hwnd, &r);
        const int x = GET_X_LPARAM(msg->lParam), y = GET_Y_LPARAM(msg->lParam);
        const int border = qRound(6 * m_window->devicePixelRatio());
        const bool left = x < r.left + border, right = x >= r.right - border;
        const bool top = y < r.top + border, bottom = y >= r.bottom - border;
        if (!left && !right && !top && !bottom)
            break;
        *result = top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP)
                : bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM)
                : left ? HTLEFT : HTRIGHT;
        return true;
    }
    }
#else
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}
