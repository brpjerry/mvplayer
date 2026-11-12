import QtQuick

// The scroll bar of a list: a thin bar that shows where the list is, shown
// while it moves or while the pointer is over the strip it runs along. The
// bar can be dragged; a press on the track beside it takes the list there
// (the bar glides to the press, centred on it, and then follows the drag);
// and the wheel over the strip scrolls the list as over the list itself,
// also where the strip stands beside the list rather than on it.
//
// The item is that strip, placed by its user along the list's right edge;
// the bar is drawn in it `inset` from its right edge, and widens while the
// pointer is over the strip.
Item {
    id: root

    required property Flickable view
    // The view's wheel, which glides it and carries the wheel's motion on.
    required property KineticWheel wheel

    // From the strip's right edge to the bar. 6 keeps a strip at the
    // window's edge clear of the 6 px that resize it on Windows (WindowFrame).
    property real inset: 6
    property real thickness: 4     // the bar's width
    property real hoverThickness: thickness * 2
    property real minLength: 32
    property real margin: 0        // kept clear at both ends of the track
    property real restOpacity: 0   // while nothing moves the list
    property real activeOpacity: 0.8

    // The range the view scrolls through, as KineticWheel.clampY and PageKeys
    // take it.
    readonly property real topY: view.originY - view.topMargin
    readonly property real span: Math.max(0, view.originY + view.contentHeight + view.bottomMargin - view.height - topY)
    readonly property bool scrolls: span > 0.5
    readonly property real track: height - 2 * margin
    readonly property real length: Math.min(track, Math.max(minLength, track * view.height / (view.height + span)))
    readonly property real travel: Math.max(0, track - length)
    readonly property real position: scrolls ? Math.max(0, Math.min(1, (view.contentY - topY) / span)) : 0
    readonly property bool hovered: area.containsMouse
    readonly property bool pressed: area.pressed

    visible: scrolls

    // The contentY that puts the bar's top at `barY`.
    function contentYAt(barY) {
        return topY + (travel > 0 ? Math.max(0, Math.min(1, (barY - margin) / travel)) : 0) * span
    }

    Rectangle {
        id: bar
        readonly property bool hot: root.hovered || root.pressed
        width: hot ? root.hoverThickness : root.thickness
        x: root.width - root.inset - width
        y: root.margin + root.position * root.travel
        height: root.length
        radius: width / 2
        color: root.pressed ? Theme.textDim : Theme.textFaint
        opacity: hot ? 1 : root.view.moving || root.wheel.active ? root.activeOpacity : root.restOpacity
        Behavior on width { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.slow } }
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        // Where the bar is held, from its top.
        property real grab: 0
        onPressed: (mouse) => {
            root.wheel.halt()
            if (mouse.y >= bar.y && mouse.y < bar.y + bar.height) {
                grab = mouse.y - bar.y
            } else {
                grab = root.length / 2
                root.wheel.glideTo(root.contentYAt(mouse.y - grab))
            }
        }
        onPositionChanged: (mouse) => {
            if (!pressed)
                return
            root.wheel.halt()
            root.view.contentY = root.contentYAt(mouse.y - grab)
        }

        WheelHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => root.wheel.scroll(event.pixelDelta.y, event.angleDelta.y, event.phase)
        }
    }
}
