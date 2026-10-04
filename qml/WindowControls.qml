import QtQuick
import QtQuick.Window
import MvPlayer.Core

// Minimise, maximise and close, for platforms where the application draws
// its own title bar (see WindowFrame).
Row {
    id: root

    readonly property Window win: Window.window
    readonly property bool maximized: win && win.visibility === Window.Maximized

    visible: WindowFrame.custom
    spacing: 2

    IconButton {
        icon: Icons.windowMinimize
        iconSize: 18
        tooltip: "Minimise"
        tooltipBelow: true
        onClicked: root.win.showMinimized()
    }
    IconButton {
        icon: root.maximized ? Icons.windowRestore : Icons.windowMaximize
        iconSize: 16
        tooltip: root.maximized ? "Restore" : "Maximise"
        tooltipBelow: true
        onClicked: root.maximized ? root.win.showNormal() : root.win.showMaximized()
    }
    IconButton {
        icon: Icons.close
        iconSize: 18
        hoverColor: "#ff5d5d"
        tooltip: "Close"
        tooltipBelow: true
        onClicked: root.win.close()
    }
}
