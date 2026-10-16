import QtQuick

// Check box with three looks: off, partly on (a bar) and on (a tick).
Item {
    id: root

    // 0 off, 1 partly, 2 on (FolderTreeModel.Unchecked / Partial / Checked).
    property int checkState: 0
    readonly property bool on: checkState > 0
    signal toggled()

    implicitWidth: 18
    implicitHeight: 18

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: root.on ? Theme.accent : (mouse.containsMouse ? Theme.pressed : Theme.hover)
        border.width: root.on ? 0 : 1
        border.color: mouse.containsMouse ? Theme.textDim : Theme.textFaint
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }
    Icon {
        anchors.centerIn: parent
        visible: root.checkState === 2
        path: Icons.check
        size: 14
        color: Theme.accentInk
    }
    Rectangle {
        anchors.centerIn: parent
        visible: root.checkState === 1
        width: 10
        height: 2
        radius: 1
        color: Theme.accentInk
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        anchors.margins: -6
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled()
    }
}
