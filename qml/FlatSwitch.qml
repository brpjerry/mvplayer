import QtQuick

// Toggle switch.
Item {
    id: root

    property bool checked: false
    signal toggled(bool checked)

    implicitWidth: 40
    implicitHeight: 22

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.checked ? Theme.accent : Theme.pressed
        Behavior on color { ColorAnimation { duration: Theme.normal } }
    }
    Rectangle {
        y: 3
        x: root.checked ? parent.width - width - 3 : 3
        width: 16
        height: 16
        radius: 8
        color: root.checked ? Theme.accentInk : Theme.textDim
        Behavior on x { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Behavior on color { ColorAnimation { duration: Theme.normal } }
    }
    MouseArea {
        anchors.fill: parent
        anchors.margins: -6
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled(!root.checked)
    }
}
