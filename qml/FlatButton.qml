import QtQuick

// Flat text button; `primary` uses the accent colour.
Item {
    id: root

    property string text
    property string icon
    property bool primary: false

    signal clicked()

    implicitWidth: row.implicitWidth + 28
    implicitHeight: 34
    opacity: enabled ? 1 : 0.4

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusSmall
        color: root.primary ? (mouse.pressed ? Theme.accent : mouse.containsMouse ? Theme.accentHi : Theme.accent)
                            : (mouse.pressed ? Theme.pressed : mouse.containsMouse ? Theme.pressed : Theme.hover)
        scale: mouse.pressed ? 0.97 : 1
        Behavior on color { ColorAnimation { duration: Theme.fast } }
        Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 8
        Icon {
            visible: root.icon.length > 0
            anchors.verticalCenter: parent.verticalCenter
            path: root.icon
            size: 16
            color: root.primary ? Theme.accentInk : Theme.text
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: root.primary ? Theme.accentInk : Theme.text
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
