#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

#include <qt_windows.h>

class MediaSession;
class QWindow;
struct ITaskbarList3;

// Windows: previous, play/pause and next under the window's thumbnail on the
// taskbar, reading from and asking through a MediaSession. They are there
// while a video is loaded, as the app is on MPRIS; next and previous are
// greyed out when they would not change the video. Made a child of the
// window, so it goes with it.
class TaskbarButtons : public QObject, public QAbstractNativeEventFilter
{
public:
    TaskbarButtons(MediaSession *session, QWindow *window);
    ~TaskbarButtons() override;

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void schedule();
    void sync(bool fresh);

    MediaSession *m_session;
    QWindow *m_window;
    ITaskbarList3 *m_taskbar = nullptr;
    UINT m_buttonCreated = 0; // "TaskbarButtonCreated", sent whenever the window gets a taskbar button
    bool m_comHere = false;
    bool m_pending = false;
};
