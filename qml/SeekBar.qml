import QtQuick

// Seek bar: click or drag anywhere; shows the time under the pointer.
Item {
    id: root

    property real position: 0
    property real duration: 0
    readonly property bool dragging: mouse.pressed
    readonly property bool engaged: mouse.containsMouse || mouse.pressed

    // exact=false while scrubbing (fast keyframe seeks), true on release
    signal seekRequested(real seconds, bool exact)

    implicitHeight: 20

    // While dragging, and briefly afterwards until the player catches up,
    // show where the user put the handle rather than the reported position.
    property real dragFraction: 0
    property bool holding: false
    readonly property real fraction: (dragging || holding) ? dragFraction
                                   : duration > 0 ? Math.max(0, Math.min(1, position / duration)) : 0

    function fractionAt(x) {
        return Math.max(0, Math.min(1, x / width))
    }

    Timer { id: holdTimer; interval: 350; onTriggered: root.holding = false }

    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: root.engaged ? 6 : 4
        radius: height / 2
        color: Theme.pressed
        Behavior on height { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }

        Rectangle {
            width: Math.max(height, parent.width * root.fraction)
            height: parent.height
            radius: height / 2
            color: root.engaged ? Theme.accentHi : Theme.accent
            visible: root.duration > 0
            Behavior on color { ColorAnimation { duration: Theme.fast } }
        }
    }

    Rectangle {
        id: handle
        x: Math.max(0, Math.min(root.width - width, root.width * root.fraction - width / 2))
        anchors.verticalCenter: parent.verticalCenter
        width: 14
        height: 14
        radius: 7
        color: Theme.text
        scale: root.engaged && root.duration > 0 ? (mouse.pressed ? 1.15 : 1) : 0
        Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
    }

    // Time under the pointer
    Rectangle {
        visible: root.engaged && root.duration > 0
        x: Math.max(0, Math.min(root.width - width, mouse.mouseX - width / 2))
        y: -height - 6
        width: hoverTime.implicitWidth + 14
        height: 22
        radius: Theme.radiusSmall
        color: Theme.pressed
        Text {
            id: hoverTime
            anchors.centerIn: parent
            text: Theme.formatTime(root.fractionAt(mouse.mouseX) * root.duration)
            color: Theme.text
            font.pixelSize: 11
            font.features: { "tnum": 1 }
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        anchors.topMargin: -6
        anchors.bottomMargin: -6
        hoverEnabled: true
        enabled: root.duration > 0
        cursorShape: Qt.PointingHandCursor
        preventStealing: true

        property real lastSent: -1
        function scrub(exact) {
            root.dragFraction = root.fractionAt(mouseX)
            const t = root.dragFraction * root.duration
            if (exact || Math.abs(t - lastSent) > 0.25) {
                lastSent = t
                root.seekRequested(t, exact)
            }
        }
        onPressed: { lastSent = -1; scrub(false) }
        onPositionChanged: if (pressed) scrub(false)
        onReleased: {
            scrub(true)
            root.holding = true
            holdTimer.restart()
        }
        onWheel: (wheel) => {
            const step = (wheel.angleDelta.y > 0 ? 5 : -5)
            root.seekRequested(Math.max(0, Math.min(root.duration, root.position + step)), true)
        }
    }
}
