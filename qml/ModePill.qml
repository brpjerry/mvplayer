import QtQuick
import MvPlayer.Core

// Videos | Music: which library the window shows (App.mode). A pill whose
// thumb slides under the side chosen.
Item {
    id: root

    readonly property int segment: 40
    readonly property bool music: App.mode === "music"

    implicitWidth: 2 * segment + 4
    implicitHeight: 36

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Theme.hover
        border.width: 1
        border.color: Theme.line
    }
    Rectangle {
        id: thumb
        x: 2 + (root.music ? root.segment : 0)
        y: 2
        width: root.segment
        height: parent.height - 4
        radius: height / 2
        color: Theme.accent
        Behavior on x { NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized } }
    }
    Row {
        anchors.fill: parent
        anchors.margins: 2
        Repeater {
            model: [
                { key: "video", icon: Icons.movie, tip: "Videos" },
                { key: "music", icon: Icons.music, tip: "Music" }
            ]
            Item {
                id: side
                required property var modelData
                readonly property bool on: App.mode === modelData.key
                width: root.segment
                height: parent.height
                Icon {
                    anchors.centerIn: parent
                    path: side.modelData.icon
                    size: 20
                    color: side.on ? Theme.accentInk : sideMouse.containsMouse ? Theme.text : Theme.textDim
                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                }
                MouseArea {
                    id: sideMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: App.mode = side.modelData.key
                }
                Tooltip {
                    text: side.modelData.tip
                    shown: sideMouse.containsMouse && !side.on
                    below: true
                }
            }
        }
    }
}
