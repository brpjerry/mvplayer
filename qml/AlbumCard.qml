import QtQuick

// One album in the grid: cover with hover affordances, title, album artist.
Item {
    id: root

    required property int index
    required property string key
    required property string album
    required property string albumArtist
    required property int year
    required property int trackCount
    required property int videoCount  // tracks that have a music video
    required property int lookupCount // tracks that are looked up for one
    required property string durationText
    required property url cover

    property bool current: false   // a track of this album is loaded in the player
    property bool playing: false   // ...and not paused
    readonly property Item thumbnail: coverItem

    signal activated()
    signal menuRequested()

    readonly property bool hovered: mouse.containsMouse
    readonly property string shownTitle: album.length > 0 ? album : "Unknown album"

    Item {
        id: body
        anchors.fill: parent
        anchors.margins: 10
        scale: mouse.pressed ? 0.975 : 1
        Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }

        // A stand-in while there is no cover (or none to be had).
        Rectangle {
            anchors.fill: coverItem
            radius: Theme.radius
            color: Theme.raised
            Icon {
                anchors.centerIn: parent
                path: Icons.music
                size: 48
                color: Theme.textFaint
            }
        }
        RoundedImage {
            id: coverItem
            width: parent.width
            height: width
            source: root.cover
            placeholder: "transparent"
            zoom: root.hovered ? 1.06 : 1.0
            dim: root.hovered ? 0.35 : 0.0
            Behavior on zoom { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutCubic } }
            Behavior on dim { NumberAnimation { duration: Theme.normal } }
        }

        // Selection ring for the album that plays
        Rectangle {
            anchors.fill: coverItem
            anchors.margins: -4
            radius: Theme.radius + 4
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
            opacity: root.current ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.normal } }
        }

        // Play affordance
        Rectangle {
            anchors.centerIn: coverItem
            width: 52
            height: 52
            radius: 26
            color: Theme.accent
            opacity: root.hovered ? 1 : 0
            scale: root.hovered ? 1 : 0.7
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.normal } }
            Behavior on scale { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutBack; easing.overshoot: 1.4 } }
            Icon {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: 2
                path: Icons.play
                size: 28
                color: Theme.accentInk
            }
        }

        // How long the album plays
        Rectangle {
            anchors.left: coverItem.left
            anchors.bottom: coverItem.bottom
            anchors.margins: 8
            width: durationLabel.implicitWidth + 10
            height: 18
            radius: 4
            color: Theme.scrim
            Text {
                id: durationLabel
                anchors.centerIn: parent
                text: root.durationText
                color: Theme.scrimText
                font.pixelSize: 11
                font.weight: Font.Medium
                font.features: { "tnum": 1 }
            }
        }
        // Of an album that is looked up: how many of its tracks have a video
        Rectangle {
            visible: root.lookupCount > 0
            anchors.right: coverItem.right
            anchors.bottom: coverItem.bottom
            anchors.margins: 8
            width: videosRow.implicitWidth + 12
            height: 18
            radius: 4
            color: Theme.scrim
            Row {
                id: videosRow
                anchors.centerIn: parent
                spacing: 4
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    path: Icons.movie
                    size: 12
                    color: Theme.scrimText
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.videoCount + " / " + root.trackCount
                    color: Theme.scrimText
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    font.features: { "tnum": 1 }
                }
            }
        }

        // Caption
        EqBars {
            id: bars
            anchors.left: parent.left
            anchors.top: coverItem.bottom
            anchors.topMargin: 13
            visible: root.current
            playing: root.playing
            width: 13
            height: 12
        }
        Text {
            id: titleLabel
            anchors.left: parent.left
            anchors.leftMargin: root.current ? 21 : 0
            anchors.right: parent.right
            anchors.top: coverItem.bottom
            anchors.topMargin: 10
            text: root.shownTitle
            color: root.current ? Theme.accentHi : Theme.text
            font.pixelSize: 14
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Behavior on color { ColorAnimation { duration: Theme.normal } }
            Behavior on anchors.leftMargin { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
        }
        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: titleLabel.bottom
            anchors.topMargin: 3
            text: root.albumArtist.replace(/; /g, ", ") + (root.year > 0 ? "  ·  " + root.year : "")
            color: Theme.textDim
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: body
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: (event) => event.button === Qt.RightButton ? root.menuRequested() : root.activated()
    }
}
