import QtQuick

// What a video that waits for review carries on its picture: accept and
// reject, and — when several uploads could be the track's video — arrows to
// step through them. Laid over the thumbnail in the grid, and over the video
// while it plays there.
Item {
    id: root

    property int option: 1    // which of the options is shown, from 1
    property int options: 1
    // A video already in the library with this one's title: {album, ytTitle}.
    property var sameTitle: ({})

    signal approved()
    signal rejected()
    signal stepped(int delta) // show the next (+1) or previous (-1) option

    // Other uploads that could be this track's video: step through them.
    Repeater {
        model: root.options > 1 ? [-1, 1] : []
        Rectangle {
            id: arrow
            required property int modelData
            x: modelData < 0 ? 8 : root.width - width - 8
            y: (root.height - height) / 2
            width: 34
            height: 34
            radius: 17
            color: arrowMouse.containsMouse ? Theme.accent : Theme.scrim
            scale: arrowMouse.pressed ? 0.92 : 1
            Behavior on color { ColorAnimation { duration: Theme.fast } }
            Behavior on scale { NumberAnimation { duration: Theme.fast } }
            Icon {
                anchors.centerIn: parent
                path: Icons.back
                rotation: arrow.modelData < 0 ? 0 : 180
                size: 20
                color: arrowMouse.containsMouse ? Theme.accentInk : Theme.scrimText
            }
            MouseArea {
                id: arrowMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.stepped(arrow.modelData)
            }
            Tooltip {
                text: arrow.modelData < 0 ? "Previous option" : "Next option"
                shown: arrowMouse.containsMouse
            }
        }
    }
    Rectangle {
        visible: root.options > 1
        x: 8
        y: 8
        width: optionLabel.implicitWidth + 12
        height: 20
        radius: 4
        color: Theme.scrim
        Text {
            id: optionLabel
            anchors.centerIn: parent
            text: "Option " + root.option + " of " + root.options
            color: Theme.scrimText
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
    }

    // For information: the song may have its video already.
    Rectangle {
        id: sameBadge
        readonly property bool wanted: root.sameTitle !== undefined && root.sameTitle !== null && root.sameTitle.ytTitle !== undefined
        visible: wanted
        x: 8
        y: root.height - height - 8
        width: Math.min(sameRow.implicitWidth + 14, root.width - 76)
        height: 22
        radius: 4
        color: Theme.scrim
        Row {
            id: sameRow
            x: 7
            anchors.verticalCenter: parent.verticalCenter
            spacing: 5
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                path: Icons.check
                size: 13
                color: Theme.accentHi
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, root.width - 76 - 32)
                text: !sameBadge.wanted ? "" : root.sameTitle.album ? "Same title in library · " + root.sameTitle.album : "Same title in library"
                color: Theme.scrimText
                font.pixelSize: 11
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }
        MouseArea {
            id: sameMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }
        Tooltip {
            // From the badge's left edge: centred on it, it would leave the grid.
            anchors.horizontalCenter: undefined
            x: 0
            text: sameBadge.wanted ? "In the library: " + root.sameTitle.ytTitle : ""
            shown: sameMouse.containsMouse
        }
    }

    // The verdict: accept it into the library, or turn it down.
    Row {
        x: root.width - width - 8
        y: 8
        spacing: 6
        Repeater {
            model: [
                { accept: true, icon: Icons.check, tint: "#3fbf7f", tip: "This is the track's video" },
                { accept: false, icon: Icons.close, tint: "#ff5d5d", tip: "Not this track's video: delete it" }
            ]
            Rectangle {
                id: verdict
                required property var modelData
                width: 34
                height: 34
                radius: 17
                color: verdictMouse.containsMouse ? modelData.tint : Theme.scrim
                border.width: 1
                border.color: modelData.tint
                scale: verdictMouse.pressed ? 0.92 : 1
                Behavior on color { ColorAnimation { duration: Theme.fast } }
                Behavior on scale { NumberAnimation { duration: Theme.fast } }
                Icon {
                    anchors.centerIn: parent
                    path: verdict.modelData.icon
                    size: 20
                    color: verdictMouse.containsMouse ? "#ffffff" : verdict.modelData.tint
                }
                MouseArea {
                    id: verdictMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: verdict.modelData.accept ? root.approved() : root.rejected()
                }
                Tooltip {
                    // Below: the buttons are at the top of the card, and
                    // above them the first row's would leave the grid.
                    below: true
                    // And ending at the button's right edge, for the last column.
                    anchors.horizontalCenter: undefined
                    x: parent.width - width
                    text: verdict.modelData.tip
                    shown: verdictMouse.containsMouse
                }
            }
        }
    }
}
