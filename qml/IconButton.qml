import QtQuick

// Round, flat icon button with hover and press feedback.
Item {
    id: root

    property string icon
    property real iconSize: 20
    property real size: 36
    property color color: Theme.textDim
    property color hoverColor: Theme.text
    property bool filled: false        // solid accent disc (primary action)
    property bool checked: false       // accent-coloured icon
    property string tooltip
    property bool tooltipBelow: false
    readonly property bool hovered: mouse.containsMouse
    readonly property bool pressed: mouse.pressed

    signal clicked()

    implicitWidth: size
    implicitHeight: size
    opacity: enabled ? 1 : 0.35

    Rectangle {
        id: disc
        anchors.centerIn: parent
        width: root.size
        height: root.size
        radius: root.size / 2
        color: root.filled ? (mouse.containsMouse ? Theme.accentHi : Theme.accent)
                           : (mouse.pressed ? Theme.pressed : Theme.hover)
        opacity: root.filled ? 1 : (mouse.containsMouse ? 1 : 0)
        scale: mouse.pressed ? 0.92 : 1

        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        Behavior on color { ColorAnimation { duration: Theme.fast } }
        Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
    }

    Icon {
        anchors.centerIn: parent
        path: root.icon
        size: root.iconSize
        scale: mouse.pressed ? 0.92 : 1
        color: root.filled ? Theme.accentInk
             : root.checked ? Theme.accent
             : mouse.containsMouse ? root.hoverColor : root.color

        Behavior on color { ColorAnimation { duration: Theme.fast } }
        Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }

    Tooltip {
        text: root.tooltip
        shown: mouse.containsMouse && !mouse.pressed
        below: root.tooltipBelow
    }
}
