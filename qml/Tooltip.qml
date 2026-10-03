import QtQuick

// Small label that fades in above (or below) its parent after a short hover.
Item {
    id: root

    property string text
    property bool shown: false
    property bool below: false
    property int delay: 500

    anchors.horizontalCenter: parent.horizontalCenter
    y: below ? parent.height + 8 : -height - 8
    width: label.implicitWidth + 16
    height: label.implicitHeight + 10
    z: 1000
    opacity: 0
    visible: opacity > 0

    states: State {
        name: "shown"
        when: root.shown && root.text.length > 0
        PropertyChanges { root.opacity: 1 }
    }
    transitions: [
        Transition {
            to: "shown"
            SequentialAnimation {
                PauseAnimation { duration: root.delay }
                NumberAnimation { property: "opacity"; duration: Theme.fast }
            }
        },
        Transition {
            NumberAnimation { property: "opacity"; duration: Theme.fast }
        }
    ]

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusSmall
        color: Theme.pressed
    }
    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: Theme.text
        font.pixelSize: 12
    }
}
