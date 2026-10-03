import QtQuick

// Mute button with a slider that is always visible beside it.
Item {
    id: root

    property real volume: 1.0
    property bool muted: false

    signal volumeRequested(real volume)
    signal muteRequested(bool muted)

    implicitWidth: 36 + 6 + 92
    implicitHeight: 36

    readonly property real shown: muted ? 0 : volume

    IconButton {
        id: button
        anchors.verticalCenter: parent.verticalCenter
        icon: root.muted || root.volume <= 0.001 ? Icons.volumeOff
            : root.volume < 0.5 ? Icons.volumeLow : Icons.volumeHigh
        tooltip: root.muted ? "Unmute (M)" : "Mute (M)"
        onClicked: root.muteRequested(!root.muted)
    }

    Item {
        id: slider
        anchors.left: button.right
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        width: 92
        height: 20
        readonly property bool engaged: area.containsMouse || area.pressed

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: Theme.pressed
            Rectangle {
                width: parent.width * root.shown
                height: parent.height
                radius: 2
                color: slider.engaged ? Theme.accentHi : Theme.textDim
                Behavior on color { ColorAnimation { duration: Theme.fast } }
                Behavior on width { enabled: !area.pressed; NumberAnimation { duration: Theme.fast } }
            }
        }
        Rectangle {
            x: Math.max(0, Math.min(parent.width - width, parent.width * root.shown - width / 2))
            anchors.verticalCenter: parent.verticalCenter
            width: 12
            height: 12
            radius: 6
            color: Theme.text
            scale: slider.engaged ? 1 : 0
            Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
        }
        MouseArea {
            id: area
            anchors.fill: parent
            anchors.margins: -6
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            preventStealing: true
            function apply() {
                const v = Math.max(0, Math.min(1, (mouseX - 6) / slider.width))
                if (root.muted && v > 0)
                    root.muteRequested(false)
                root.volumeRequested(v)
            }
            onPressed: apply()
            onPositionChanged: if (pressed) apply()
        }
    }

    // Scroll anywhere over the control to adjust.
    WheelHandler {
        target: null
        onWheel: (event) => {
            const step = event.angleDelta.y > 0 ? 0.05 : -0.05
            root.volumeRequested(Math.max(0, Math.min(1, root.volume + step)))
        }
    }
}
