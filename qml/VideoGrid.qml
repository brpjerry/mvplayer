import QtQuick
import MvPlayer.Core

// Thumbnail grid of the (filtered) library.
Item {
    id: root

    property var currentVideoId: -1
    property bool currentPlaying: false
    property string searchText

    // row in App.videos
    signal activated(int row)

    readonly property alias view: grid
    readonly property alias wheel: wheel

    // The card of the video that is loaded in the player, while that card
    // exists (it is on screen or close to it), and where its thumbnail is in
    // this item's coordinates. The player draws the video right there.
    property Item currentCard: null
    readonly property bool currentThumbAvailable: currentCard !== null
    readonly property rect currentThumbRect: currentCard
        ? Qt.rect(grid.x + currentCard.x - grid.contentX + currentCard.thumbnail.parent.x + currentCard.thumbnail.x,
                  grid.y + currentCard.y - grid.contentY + currentCard.thumbnail.parent.y + currentCard.thumbnail.y,
                  currentCard.thumbnail.width, currentCard.thumbnail.height)
        : Qt.rect(0, 0, 0, 0)

    function claimCurrent(card) {
        currentCard = card
    }
    function releaseCurrent(card) {
        if (currentCard === card)
            currentCard = null
    }

    function thumbDecodeWidth(row) {
        const item = grid.itemAtIndex(row)
        return item ? item.thumbnail.decodeWidth : 0
    }

    GridView {
        id: grid
        anchors.fill: parent
        anchors.leftMargin: 18
        anchors.rightMargin: 18

        readonly property int columns: Math.max(1, Math.floor(width / 290))
        cellWidth: Math.floor(width / columns)
        cellHeight: Math.round((cellWidth - 20) * 9 / 16) + 74

        model: App.videos
        topMargin: 2
        bottomMargin: 24
        clip: true
        cacheBuffer: 800
        boundsBehavior: Flickable.StopAtBounds
        maximumFlickVelocity: 16000
        pixelAligned: true

        delegate: VideoCard {
            id: card
            width: grid.cellWidth
            height: grid.cellHeight
            current: videoId === root.currentVideoId
            playing: current && root.currentPlaying
            onActivated: root.activated(index)
            onCurrentChanged: current ? root.claimCurrent(card) : root.releaseCurrent(card)
            Component.onCompleted: if (current) root.claimCurrent(card)
            Component.onDestruction: root.releaseCurrent(card)
        }

        // Items animate as filters and imports reshape the grid.
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
            // An item displaced mid-appearance must still finish appearing.
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

        readonly property bool filtering: root.searchText.trim().length > 0 || App.videos.facetType !== "all"

        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 56
            height: 56
            Spinner {
                anchors.centerIn: parent
                size: 30
                visible: !parent.parent.filtering && App.busy
            }
            Icon {
                anchors.centerIn: parent
                visible: parent.parent.filtering || !App.busy
                path: parent.parent.filtering ? Icons.search : Icons.movie
                size: 40
                color: Theme.textFaint
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: parent.filtering ? "No videos match"
                : App.busy ? "Looking for music videos…"
                : "No music videos yet"
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
            text: parent.filtering ? (root.searchText.trim().length > 0 ? "Nothing in this view matches “" + root.searchText.trim() + "”." : "Nothing here yet.")
                : App.busy ? "Videos appear here as soon as each one is found, checked against your track and downloaded."
                : "None of the tracks in your music folders has a music video that could be found."
            color: Theme.textDim
            font.pixelSize: 13
        }
    }
}
