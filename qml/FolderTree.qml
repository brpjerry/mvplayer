import QtQuick
import MvPlayer.Core

// The folders under the base directory, each with a check box: a ticked
// folder's tracks are looked up for videos (every file under the base is
// read for the player), a part-ticked one has ticked folders inside.
// Scrolls on its own once taller than maxHeight.
ListView {
    id: root

    property real maxHeight: 320
    property real rowHeight: 30

    model: App.folderTree
    implicitHeight: Math.min(contentHeight, maxHeight)
    height: implicitHeight
    clip: true
    interactive: contentHeight > height + 1
    boundsBehavior: Flickable.StopAtBounds

    KineticWheel {
        view: root
        enabled: root.interactive
        touchpadGain: App.touchpadGain
        wheelStep: App.wheelStep * 0.5
        deceleration: App.flickDeceleration
    }

    // Where there is more below.
    Rectangle {
        anchors.right: parent.right
        anchors.rightMargin: 1
        y: root.visibleArea.yPosition * root.height + 2
        width: 3
        height: Math.max(20, root.visibleArea.heightRatio * root.height - 4)
        radius: 1.5
        color: Theme.textFaint
        opacity: root.interactive ? (root.moving ? 0.9 : 0.45) : 0
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    }

    delegate: Item {
        id: row
        required property int index
        required property string path
        required property string name
        required property int depth
        required property bool expanded
        required property bool hasChildren
        required property int checkState
        width: root.width
        height: root.rowHeight

        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: -8
            anchors.rightMargin: -8
            radius: Theme.radiusSmall
            color: Theme.hover
            opacity: rowHover.hovered ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        }
        HoverHandler { id: rowHover }

        // Open and close.
        Item {
            id: twisty
            x: row.depth * 18
            width: 20
            height: parent.height
            Icon {
                anchors.centerIn: parent
                visible: row.hasChildren
                path: Icons.chevronDown
                size: 16
                color: twistyMouse.containsMouse ? Theme.text : Theme.textFaint
                rotation: row.expanded ? 0 : -90
                Behavior on rotation { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
            }
            MouseArea {
                id: twistyMouse
                anchors.fill: parent
                enabled: row.hasChildren
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.model.toggleExpanded(row.index)
            }
        }
        FlatCheck {
            id: check
            anchors.left: twisty.right
            anchors.leftMargin: 2
            anchors.verticalCenter: parent.verticalCenter
            checkState: row.checkState
            onToggled: App.setFolderChecked(row.path, row.checkState !== 2)
        }
        Icon {
            id: folderIcon
            anchors.left: check.right
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            path: Icons.folder
            size: 16
            color: row.checkState === 2 ? Theme.accent : Theme.textDim
            Behavior on color { ColorAnimation { duration: Theme.fast } }
        }
        Text {
            anchors.left: folderIcon.right
            anchors.leftMargin: 8
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: row.depth === 0 ? App.displayPath(row.path) : row.name
            color: row.checkState === 2 ? Theme.text : Theme.textDim
            font.pixelSize: 13
            font.weight: row.depth === 0 ? Font.DemiBold : Font.Normal
            elide: Text.ElideMiddle
            MouseArea {
                // The name opens and closes the folder, as the arrow does.
                anchors.fill: parent
                enabled: row.hasChildren
                onClicked: root.model.toggleExpanded(row.index)
            }
        }
    }
}
