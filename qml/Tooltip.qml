import QtQuick
import QtQuick.Controls.Basic

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

    // Drawn in the window's overlay, above everything: a tooltip of the top
    // bar reaches down over the video, which is a later sibling of the bar.
    property point origin: Qt.point(0, 0)
    onShownChanged: if (shown) origin = root.mapToItem(bubble.parent, 0, 0)

    Item {
        id: bubble
        parent: root.Overlay.overlay ? root.Overlay.overlay : root
        x: root.origin.x
        y: root.origin.y
        z: 1000
        width: root.width
        height: root.height
        opacity: root.opacity
        visible: root.opacity > 0 && root.visible

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
}
