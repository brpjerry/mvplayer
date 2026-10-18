import QtQuick

// Search box. Grows a little while focused; Escape clears and releases focus.
Item {
    id: root

    property alias text: input.text
    property string placeholder: "Search titles, artists, albums, tags"
    readonly property bool active: input.activeFocus
    // The room the bar can spare: the box gives way down to 140 px
    // before the heading beside it would.
    property real maxWidth: 1e9

    function focusInput() {
        input.forceActiveFocus()
        input.selectAll()
    }
    function clear() {
        input.text = ""
    }

    // No room at all (a narrow window with a panel up): the box folds to
    // its icon until it is focused, then takes the heading's room.
    implicitWidth: active ? Math.max(140, Math.min(320, maxWidth + 150))
                 : Math.max(36, Math.min(input.text.trim().length > 0 ? 320 : 240, maxWidth))
    implicitHeight: 36

    Behavior on implicitWidth {
        NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
    }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.active ? Theme.hover : hover.hovered ? Theme.hover : Theme.raised
        border.width: 1
        border.color: root.active ? Theme.accent : "transparent"

        Behavior on color { ColorAnimation { duration: Theme.fast } }
        Behavior on border.color { ColorAnimation { duration: Theme.normal } }
    }

    HoverHandler { id: hover; cursorShape: Qt.IBeamCursor }
    TapHandler { onTapped: input.forceActiveFocus() }

    Icon {
        id: glass
        x: 12
        anchors.verticalCenter: parent.verticalCenter
        path: Icons.search
        size: 17
        color: root.active ? Theme.accent : Theme.textFaint
        Behavior on color { ColorAnimation { duration: Theme.normal } }
    }

    TextInput {
        id: input
        anchors.left: glass.right
        anchors.leftMargin: 8
        anchors.right: clearButton.left
        anchors.rightMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentInk
        font.pixelSize: 13
        clip: true
        selectByMouse: true
        Keys.onEscapePressed: {
            if (text.length > 0)
                text = ""
            else
                focus = false
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: input.text.trim().length === 0 && root.width >= 120
            text: root.placeholder
            color: Theme.textFaint
            font.pixelSize: 13
            elide: Text.ElideRight
            width: parent.width
        }
    }

    IconButton {
        id: clearButton
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        size: 28
        iconSize: 15
        icon: Icons.close
        opacity: input.text.trim().length > 0 ? 1 : 0
        visible: opacity > 0
        onClicked: {
            input.text = ""
            input.forceActiveFocus()
        }
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
    }
}
