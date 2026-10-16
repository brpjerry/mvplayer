import QtQuick
import MvPlayer.Core

// The music library as album covers, in the video grid's manner: the same
// margins and columns, a square cover in place of the 16:9 thumbnail.
Item {
    id: root

    property string searchText: ""
    property string currentKey: ""     // the album of the track that plays
    property bool currentPlaying: false

    signal activated(int row)
    signal menuRequested(int row, Item item)

    readonly property alias view: grid
    readonly property alias wheel: wheel

    // Brings an album's card into view; returns its row, or -1.
    function reveal(key) {
        const row = App.albums.rowOfKey(key)
        if (row >= 0)
            grid.positionViewAtIndex(row, GridView.Center)
        return row
    }

    GridView {
        id: grid
        anchors.fill: parent
        anchors.leftMargin: 18
        anchors.rightMargin: 18

        readonly property int columns: Math.max(1, Math.floor(width / 290))
        cellWidth: Math.floor(width / columns)
        cellHeight: (cellWidth - 20) + 74

        model: App.albums
        topMargin: 2
        bottomMargin: 24
        clip: true
        cacheBuffer: 800
        boundsBehavior: Flickable.StopAtBounds
        maximumFlickVelocity: 16000
        pixelAligned: true

        delegate: AlbumCard {
            id: card
            width: grid.cellWidth
            height: grid.cellHeight
            current: key === root.currentKey
            playing: current && root.currentPlaying
            onActivated: root.activated(index)
            onMenuRequested: root.menuRequested(index, card)
        }

        add: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 240 }
                NumberAnimation { property: "scale"; from: 0.92; to: 1; duration: Theme.slow; easing.type: Easing.OutCubic }
            }
        }
        remove: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; to: 0; duration: 140 }
                NumberAnimation { property: "scale"; to: 0.94; duration: 140 }
            }
        }
        displaced: Transition {
            NumberAnimation { properties: "x,y"; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
            NumberAnimation { property: "opacity"; to: 1; duration: Theme.normal }
            NumberAnimation { property: "scale"; to: 1; duration: Theme.normal }
        }
        populate: Transition {
            id: populateTransition
            SequentialAnimation {
                PropertyAction { property: "opacity"; value: 0 }
                PauseAnimation { duration: Math.min(populateTransition.ViewTransition.index, 24) * 18 }
                ParallelAnimation {
                    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 260 }
                    NumberAnimation { property: "y"; from: populateTransition.ViewTransition.destination.y + 14; duration: Theme.slow; easing.type: Easing.OutCubic }
                }
            }
        }

        KineticWheel {
            id: wheel
            view: grid
            touchpadGain: App.touchpadGain
            wheelStep: App.wheelStep
            deceleration: App.flickDeceleration
        }
    }

    // Scroll indicator
    Rectangle {
        anchors.right: parent.right
        anchors.rightMargin: 5
        visible: grid.contentHeight > grid.height
        y: grid.visibleArea.yPosition * grid.height
        width: 4
        height: Math.max(32, grid.visibleArea.heightRatio * grid.height)
        radius: 2
        color: Theme.textFaint
        opacity: grid.moving || wheel.active ? 0.8 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.slow } }
    }

    // Empty states
    Column {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -30
        spacing: 10
        opacity: grid.count === 0 && App.configured ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }

        readonly property bool filtering: root.searchText.trim().length > 0 || App.albums.facetType !== "all"

        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 56
            height: 56
            Spinner {
                anchors.centerIn: parent
                size: 30
                visible: !parent.parent.filtering && App.scanning
            }
            Icon {
                anchors.centerIn: parent
                visible: parent.parent.filtering || !App.scanning
                path: parent.parent.filtering ? Icons.search : Icons.music
                size: 40
                color: Theme.textFaint
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: parent.filtering ? "No albums match"
                : App.scanning ? "Reading your music…"
                : "No music yet"
            color: Theme.text
            font.pixelSize: 17
            font.weight: Font.DemiBold
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            width: Math.min(420, root.width - 80)
            wrapMode: Text.WordWrap
            lineHeight: 1.3
            text: parent.filtering ? (root.searchText.trim().length > 0 ? "No album matches “" + root.searchText.trim() + "”." : "Nothing here yet.")
                : App.scanning ? "Albums appear here as the files under the base directory are read."
                : "No music files were found under the base directory."
            color: Theme.textDim
            font.pixelSize: 13
        }
    }
}
