import QtQuick
import QtQuick.Window
import MvPlayer.Core

// Empty space that stands in for the title bar where the application draws
// its own (see WindowFrame): dragging it moves the window, a double click
// maximises or restores it. Put it behind the controls of the area it covers.
Item {
    id: root

    enabled: WindowFrame.custom

    DragHandler {
        target: null
        onActiveChanged: if (active) root.Window.window.startSystemMove()
    }
    TapHandler {
        onDoubleTapped: {
            const w = root.Window.window
            if (w.visibility === Window.Maximized)
                w.showNormal()
            else if (w.visibility === Window.Windowed)
                w.showMaximized()
        }
    }
}
