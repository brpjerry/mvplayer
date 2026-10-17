import QtQuick

// Seek bar: click or drag anywhere; shows the time under the pointer.
Item {
    id: root

    property real position: 0
    property real duration: 0
    property var spans: []   // [[start, end], …] in seconds: where the library audio is the track's, drawn on the track
    readonly property bool dragging: mouse.pressed
    readonly property bool engaged: mouse.containsMouse || mouse.pressed
    // The time under the pointer while hovered (-1 when not): the clock
    // beside the bar shows it, where a bubble would cover the transport.
    readonly property real hoverSeconds: engaged && duration > 0 && !dragging ? fractionAt(mouse.mouseX) * duration : -1

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
        Repeater {
            model: root.duration > 0 ? root.spans : []
            Rectangle {
                required property var modelData
                x: parent.width * Math.max(0, Math.min(1, modelData[0] / root.duration))
                width: Math.max(2, parent.width * Math.max(0, Math.min(1, (modelData[1] - modelData[0]) / root.duration)))
                height: parent.height
                radius: height / 2
                color: Qt.rgba(1, 1, 1, 0.22)
            }
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
