#include "ui/TaskbarButtons.h"
#include "ui/MediaSession.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QTimer>
#include <QWindow>

#include <shobjidl.h>

#include <cwchar>
#include <iterator>

namespace {

// In the order they are shown, which is also their id.
enum Button { Previous, PlayPause, Next, ButtonCount };

// The control bar's glyphs (Icons.qml), on the same 24-unit grid.
QPainterPath glyph(Button button, bool playing)
{
    QPainterPath path;
    switch (button) {
    case Previous:
        path.addRect(6, 6, 2, 12);
        path.addPolygon(QPolygonF({{9.5, 12}, {18, 18}, {18, 6}}));
        break;
    case PlayPause:
        if (playing) {
            path.addRect(6, 5, 4, 14);
            path.addRect(14, 5, 4, 14);
        } else {
            path.addPolygon(QPolygonF({{8, 5}, {8, 19}, {19, 12}}));
        }
        break;
    case Next:
        path.addPolygon(QPolygonF({{6, 18}, {14.5, 12}, {6, 6}}));
        path.addRect(16, 6, 2, 12);
        break;
    case ButtonCount:
        break;
    }
    return path;
}

HICON icon(Button button, bool playing, int size, const QColor &color)
{
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(size / 24.0, size / 24.0);
    painter.fillPath(glyph(button, playing), color);
    painter.end();
    return image.toHICON();
}

// The taskbar's own setting, apart from the apps' light or dark.
bool lightTaskbar()
{
    const QSettings personalize(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                                QSettings::NativeFormat);
    return personalize.value(QStringLiteral("SystemUsesLightTheme")).toInt() != 0;
}

} // namespace

TaskbarButtons::TaskbarButtons(MediaSession *session, QWindow *window)
    : QObject(window)
    , m_session(session)
    , m_window(window)
{
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comHere = SUCCEEDED(init); // RPC_E_CHANGED_MODE: initialised already, multithreaded
    if (FAILED(::CoCreateInstance(__uuidof(TaskbarList), nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITaskbarList3),
                                  reinterpret_cast<void **>(&m_taskbar)))) {
        m_taskbar = nullptr;
        return;
    }
    m_buttonCreated = ::RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (FAILED(m_taskbar->HrInit()) || !m_buttonCreated) {
        m_taskbar->Release();
        m_taskbar = nullptr;
        return;
    }
    qApp->installNativeEventFilter(this);
    connect(session, &MediaSession::trackChanged, this, &TaskbarButtons::schedule);
    connect(session, &MediaSession::playingChanged, this, &TaskbarButtons::schedule);
    connect(session, &MediaSession::canStepChanged, this, &TaskbarButtons::schedule);
    // Shown already, the window may have its taskbar button before the
    // message saying so gets through.
    if (window->isVisible())
        sync(true);
}

TaskbarButtons::~TaskbarButtons()
{
    if (m_taskbar) {
        qApp->removeNativeEventFilter(this);
        m_taskbar->Release();
    }
    if (m_comHere)
        ::CoUninitialize();
}

// A new video changes several things at once; the taskbar hears of them
// together, once control returns to the event loop.
void TaskbarButtons::schedule()
{
    if (m_pending)
        return;
    m_pending = true;
    QTimer::singleShot(0, this, [this] { sync(false); });
}

// Puts the buttons on the window's taskbar button as the session has them.
// A taskbar button starts without any; they can be added to it once, and
// are changed in place after that. With the window in the tray there is no
// taskbar button, and nothing to change.
void TaskbarButtons::sync(bool fresh)
{
    m_pending = false;
    if (!m_window->handle())
        return;
    const bool loaded = m_session->hasTrack();
    const bool playing = m_session->playing();
    const THUMBBUTTONFLAGS step = !loaded ? THBF_HIDDEN : m_session->canStep() ? THBF_ENABLED : THBF_DISABLED;
    const struct {
        const wchar_t *tip;
        THUMBBUTTONFLAGS flags;
    } spec[ButtonCount] = {
        {L"Previous", step},
        {playing ? L"Pause" : L"Play", loaded ? THBF_ENABLED : THBF_HIDDEN},
        {L"Next", step},
    };
    // Icons are made at the size of a small icon, as the taskbar shows them.
    const int size = ::GetSystemMetrics(SM_CXSMICON);
    const QColor color = lightTaskbar() ? Qt::black : Qt::white;
    THUMBBUTTON buttons[ButtonCount] = {};
    for (int i = 0; i < ButtonCount; ++i) {
        buttons[i].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        buttons[i].iId = UINT(i);
        buttons[i].hIcon = icon(Button(i), playing, size, color);
        buttons[i].dwFlags = spec[i].flags;
        std::wcsncpy(buttons[i].szTip, spec[i].tip, std::size(buttons[i].szTip) - 1);
    }
    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    // Added already: the taskbar button was there before it was announced.
    if (!fresh || FAILED(m_taskbar->ThumbBarAddButtons(hwnd, ButtonCount, buttons)))
        m_taskbar->ThumbBarUpdateButtons(hwnd, ButtonCount, buttons);
    // The taskbar keeps copies.
    for (const THUMBBUTTON &button : buttons)
        ::DestroyIcon(button.hIcon);
}

bool TaskbarButtons::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType != "windows_generic_MSG")
        return false;
    const MSG *msg = static_cast<const MSG *>(message);
    const bool clicked = msg->message == WM_COMMAND && HIWORD(msg->wParam) == THBN_CLICKED;
    // The taskbar turned light or dark.
    const bool recolour = msg->message == WM_SETTINGCHANGE && msg->lParam
                          && std::wcscmp(reinterpret_cast<const wchar_t *>(msg->lParam), L"ImmersiveColorSet") == 0;
    if ((msg->message != m_buttonCreated && !clicked && !recolour)
        || !m_window->handle() || msg->hwnd != reinterpret_cast<HWND>(m_window->winId()))
        return false;

    if (msg->message == m_buttonCreated) {
        // The window shown again out of the tray, or Explorer restarted.
        sync(true);
        return false;
    }
    if (recolour) {
        schedule();
        return false; // Qt follows it too
    }
    if (m_session->hasTrack()) {
        switch (LOWORD(msg->wParam)) {
        case Previous:
            if (m_session->canStep())
                emit m_session->previousRequested();
            break;
        case PlayPause:
            emit m_session->playPauseRequested();
            break;
        case Next:
            if (m_session->canStep())
                emit m_session->nextRequested();
            break;
        }
    }
    *result = 0;
    return true;
}
