import QtQuick
import QtQuick.Controls.Basic
import MvPlayer.Core

// The music library as album covers, in the video grid's manner: the same
// margins and columns, a square cover in place of the 16:9 thumbnail.
Item {
    id: root

    property string searchText: ""
    property string currentKey: ""     // the album of the track that plays
    property bool currentPlaying: false
    // The width the grid has once the panels beside it have slid in or
    // out: set from outside, so that it is taken in one step and not
    // frame by frame with a slide (which laid the cards out afresh at
    // every width, twice a frame as the anchors caught up).
    property real room: width

    signal activated(int row)

    readonly property alias view: grid
    readonly property alias wheel: wheel
    readonly property bool menuOpen: menu.opened

    // A right click on a card: play the album, ask for its videos (A12), or
    // look its tracks up again by today's rules (H).
    function openMenu(row, item) {
        const a = App.albums.get(row)
        if (!a || !a.key)
            return
        const p = item ? item.mapToItem(root, item.width / 2 - 20, item.height / 2 - 40) : Qt.point(root.width / 2 - 130, 60)
        menu.row = row
        menu.album = a
        menu.x = Math.max(8, Math.min(p.x, root.width - menu.width - 8))
        menu.y = Math.max(8, Math.min(p.y, root.height - menu.height - 8))
        menu.open()
    }

    // Brings an album's card into view; returns its row, or -1.
    function reveal(key) {
        const row = App.albums.rowOfKey(key)
        if (row >= 0)
            grid.positionViewAtIndex(row, GridView.Center)
            // Kept as the rows on show even while a relayout settles (the
            // grid just shown, the panel sliding): the settle would have
            // put the old rows back.
            grid.settled.stop()
            grid.settling = false
            grid.rowOffset = (grid.contentY - grid.originY + grid.topMargin) / grid.cellHeight
        return row
    }

    GridView {
        id: grid
        // Hidden, the view keeps its width and so its layout; the width
        // it has when shown again is taken then, in one step, with the
        // rows kept (below). Any change while hidden would make it flow
        // its cards again around the first one on show, and the rows
        // would drift.
        readonly property int fitWidth: root.room - 36
        property int shownWidth: fitWidth
        // A grid fading out with the mode keeps its layout (the mode is
        // read itself: the width changes before `enabled` follows it);
        // it takes the width again when it is the mode's grid once more.
        onFitWidthChanged: if (visible && App.mode === "music") shownWidth = fitWidth
        onEnabledChanged: if (enabled && visible) shownWidth = fitWidth
        x: 18
        y: 0
        width: shownWidth
        height: parent.height

        // As many across as keep a card within the chosen width (Settings
        // → Appearance): the cards share the row and grow with it, and
        // one more goes across once they would pass that width.
        readonly property int columns: Math.max(1, Math.ceil(width / App.coverWidth))
        cellWidth: Math.floor(width / columns)
        cellHeight: (cellWidth - 20) + 74

        // The scroll offset kept in rows: the cards grow or shrink with the
        // width (a panel coming or going, a resize, the grid shown again
        // at another width), and the rows on show stay where they are
        // instead of drifting by part of a row. While the view lays its
        // cards out again it moves the offset on its own, to keep the
        // first card it had on show; those moves are not the user's and
        // are not kept, and the rows are put back once it has settled.
        property real rowOffset: 0
        property bool settling: false
        readonly property alias settled: settled
        Timer {
            id: settled
            interval: 60
            onTriggered: {
                grid.keepRows()
                grid.settling = false
            }
        }
        function relayout() {
            settling = true
            settled.restart()
            relaid.restart()
            keepRows()
        }
        // A cell size changed while cards were still moving (a sort's
        // displaced transition, 340 ms) left them where the old layout
        // sent them: once the transition is over the layout is forced.
        Timer {
            id: relaid
            interval: Theme.slow + 80
            onTriggered: grid.unstick()
        }
        // Cards a cut-off transition left in the old layout's places are
        // not moved by a later layout (the view believes them placed):
        // found by their column, the view is filled afresh, quietly.
        function unstick() {
            const first = indexAt(cellWidth / 2, contentY + cellHeight / 2)
            if (first < 0)
                return
            let stuck = false
            for (let i = first; i < Math.min(count, first + columns * 6) && !stuck; i++) {
                const it = itemAtIndex(i)
                if (it && Math.abs(it.x - (i % columns) * cellWidth) > 1)
                    stuck = true
            }
            if (!stuck)
                return
            const m = model
            const appear = populate
            populate = null
            model = null
            model = m
            keepRows()
            Qt.callLater(() => { populate = appear })
        }
        // Within the content: a narrower filter and a mode round trip
        // put the kept rows past the end, and the grid showed nothing.
        function keepRows() {
            if (!(visible && cellHeight > 0))
                return
            const top = originY - topMargin
            const bottom = Math.max(top, originY + contentHeight + bottomMargin - height)
            contentY = Math.max(top, Math.min(bottom, top + rowOffset * cellHeight))
        }
        onContentYChanged: if (visible && !settling && cellHeight > 0) rowOffset = (contentY - originY + topMargin) / cellHeight
        onCellHeightChanged: relayout()
        onWidthChanged: relayout()
        onVisibleChanged: {
            if (!visible)
                return
            shownWidth = fitWidth
            relayout()
        }

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
            onMenuRequested: root.openMenu(index, card)
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

    Popup {
        id: menu
        property int row: -1
        property var album: ({})
        readonly property int tracks: album.trackCount || 0
        readonly property int lookedUp: album.lookupCount || 0
        readonly property int videos: album.videoCount || 0
        width: 268
        padding: 6
        // Any press outside closes it and goes no further: nothing under
        // it is activated until it is gone.
        modal: true
        dim: false
        focus: true // Escape closes it only while it has the focus
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        transformOrigin: Popup.TopLeft
        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
                NumberAnimation { property: "scale"; from: 0.95; to: 1; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
            }
        }
        exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: Theme.fast } }
        background: Rectangle {
            radius: Theme.radius
            color: Theme.raised
            border.width: 1
            border.color: Theme.line
        }
        function pick(key) {
            if (key === "play")
                root.activated(row)
            else if (key === "lookup")
                App.lookupAlbum(album.albumArtist, album.album)
            else if (key === "reimport")
                App.reimportAlbum(album.albumArtist, album.album)
            close()
        }
        contentItem: Column {
            Repeater {
                model: [
                    { key: "play", icon: Icons.play, text: "Play", show: true, why: "" },
                    { key: "lookup", icon: Icons.movie, text: "Find videos for this album", show: menu.lookedUp < menu.tracks,
                      why: "Its tracks are looked up wherever their files are, from now on. Settings → Library lists such albums." },
                    { key: "reimport", icon: Icons.refresh,
                      text: menu.videos > 0 ? "Re-import " + menu.videos + (menu.videos === 1 ? " video" : " videos") : "Look its tracks up again",
                      show: menu.lookedUp > 0,
                      why: "By today's rules: a video that still fits is kept, one that no longer does goes to review or is replaced." }
                ]
                Column {
                    id: entry
                    required property var modelData
                    visible: modelData.show
                    width: menu.availableWidth
                    Item {
                        width: parent.width
                        height: 34
                        enabled: entry.modelData.key === "play" || App.configured
                        opacity: enabled ? 1 : 0.4
                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusSmall
                            color: Theme.hover
                            opacity: entryMouse.containsMouse ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                        }
                        Icon {
                            x: 10
                            anchors.verticalCenter: parent.verticalCenter
                            path: entry.modelData.icon
                            size: 16
                            color: Theme.textDim
                        }
                        Text {
                            x: 34
                            width: parent.width - 44
                            anchors.verticalCenter: parent.verticalCenter
                            text: entry.modelData.text
                            color: Theme.text
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            id: entryMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: menu.pick(entry.modelData.key)
                        }
                    }
                    Text {
                        visible: entry.modelData.why.length > 0
                        width: parent.width
                        leftPadding: 10
                        rightPadding: 10
                        topPadding: 2
                        bottomPadding: 8
                        text: entry.modelData.why
                        color: Theme.textFaint
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                }
            }
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
