import QtQuick
import MvPlayer.Core

// Import activity indicator in the top bar. Expands to show progress text
// while the library is being scanned or videos are being fetched.
Item {
    id: root

    property bool open: false
    signal clicked()

    readonly property bool busy: App.busy
    readonly property string label: App.statusText

    implicitHeight: 36
    implicitWidth: busy && label.length > 0 ? text.implicitWidth + 50 : 36
    clip: true

    Behavior on implicitWidth {
        NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
    }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.open ? Theme.accentSoft : mouse.containsMouse ? Theme.hover : root.busy ? Theme.raised : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }

    Spinner {
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        size: 16
        opacity: root.busy && !App.importPaused ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    }
    // Waiting out a block by YouTube
    Icon {
        x: 8
        anchors.verticalCenter: parent.verticalCenter
        path: Icons.pause
        size: 20
        color: Theme.warn
        opacity: App.importPaused ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    }
    Icon {
        x: 8
        anchors.verticalCenter: parent.verticalCenter
        path: Icons.download
        size: 20
        color: root.open ? Theme.accent : mouse.containsMouse ? Theme.text : Theme.textDim
        opacity: root.busy ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }

    Text {
        id: text
        x: 36
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: App.importPaused ? Theme.warn : Theme.textDim
        font.pixelSize: 12
        font.features: { "tnum": 1 }
        opacity: root.busy ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }

    Tooltip {
        text: "Import activity"
        shown: mouse.containsMouse && !root.open
        below: true
    }
}
