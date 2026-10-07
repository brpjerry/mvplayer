import QtQuick

// One selectable row in the sidebar.
Item {
    id: root

    property string label
    property string icon
    property int count: -1
    property bool selected: false

    signal clicked()
    signal rightClicked()

    implicitHeight: 34
    width: parent ? parent.width : 0

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        radius: Theme.radiusSmall
        color: root.selected ? Theme.accentSoft : mouse.pressed ? Theme.pressed : Theme.hover
        opacity: root.selected || mouse.containsMouse ? 1 : 0

        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        Behavior on color { ColorAnimation { duration: Theme.normal } }
    }

    // Accent marker that grows in when the row becomes selected.
    Rectangle {
        x: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: root.selected ? 16 : 0
        radius: 1.5
        color: Theme.accent
        opacity: root.selected ? 1 : 0

        Behavior on height { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
    }

    Icon {
        id: glyph
        visible: root.icon.length > 0
        x: 20
        anchors.verticalCenter: parent.verticalCenter
        path: root.icon
        size: 18
        color: root.selected ? Theme.accent : Theme.textDim

        Behavior on color { ColorAnimation { duration: Theme.normal } }
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: glyph.visible ? 48 : 22
        anchors.right: badge.visible ? badge.left : parent.right
        anchors.rightMargin: badge.visible ? 8 : 18
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: root.selected ? Theme.text : mouse.containsMouse ? Theme.text : Theme.textDim
        font.pixelSize: 13
        font.weight: root.selected ? Font.DemiBold : Font.Normal
        elide: Text.ElideRight

        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }

    Text {
        id: badge
        visible: root.count >= 0
        anchors.right: parent.right
        anchors.rightMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        text: root.count
        color: root.selected ? Theme.accent : Theme.textFaint
        font.pixelSize: 11
        font.features: { "tnum": 1 }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (event) => event.button === Qt.RightButton ? root.rightClicked() : root.clicked()
    }
}
