import QtQuick
import QtQuick.Controls.Basic
import MvPlayer.Core

// Thumbnail grid of the (filtered) library.
Item {
    id: root

    property var currentVideoId: -1
    property bool currentPlaying: false
    property string searchText
    // The width the grid has once the panels beside it have slid in or
    // out: set from outside, so that it is taken in one step and not
    // frame by frame with a slide (which laid the cards out afresh at
    // every width, twice a frame as the anchors caught up).
    property real room: width

    // row in App.videos
    signal activated(int row)
    // the verdict on a video that waits for review, or on an orphan among
    // the orphans
    signal approved(var videoId)
    signal rejected(var videoId)
    // Allow (true) or deny the channel of this video under review.
    signal channelRequested(var videoId, bool allow)
    // Edit the tags of these videos' tracks (one song, or a batch).
    signal editRequested(var videoIds, string subject)

    readonly property alias view: grid
    readonly property alias wheel: wheel
    readonly property bool menuOpen: menu.opened

    // Videos picked with Ctrl+click, by id: a right click on one of them
    // edits the tags of all of their tracks. A plain click, or Escape, lets
    // them go. Only a library video has a track to edit: one under review
    // or an orphan is not picked.
    property var selected: []
    function isSelected(videoId) { return selected.indexOf(videoId) >= 0 }
    function toggleSelected(row) {
        const v = App.videos.get(row)
        if (!v || v.review || v.orphan)
            return
        selected = isSelected(v.videoId) ? selected.filter(id => id !== v.videoId) : selected.concat([v.videoId])
    }
    function clearSelection() { selected = [] }

    // A right click on a card: edit the tags of its track, or of the tracks
    // of every video selected where it is one of them.
    function openMenu(row, item) {
        const v = App.videos.get(row)
        if (!v || v.videoId === undefined)
            return
        const p = item ? item.mapToItem(root, item.width / 2 - 20, item.height / 2 - 40) : Qt.point(root.width / 2 - 130, 60)
        menu.video = v
        menu.editIds = !v.review && !v.orphan && isSelected(v.videoId) && selected.length > 1 ? selected : [v.videoId]
        menu.x = Math.max(8, Math.min(p.x, root.width - menu.width - 8))
        menu.y = Math.max(8, Math.min(p.y, root.height - menu.height - 8))
        menu.open()
    }
    // The open menu's entry, as a click on it (tests).
    function pickMenu() { if (menu.opened && menu.canEdit) menu.pick() }

    // After a verdict the cards close up and the next card's button lands
    // under the pointer: a second click would be a second verdict, so the
    // buttons sleep for a moment.
    property bool verdictCooldown: false
    Timer {
        id: cooldown
        interval: 500
        onTriggered: root.verdictCooldown = false
    }
    function verdictGiven() {
        root.verdictCooldown = true
        cooldown.restart()
    }

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

    // Brings a video's card into view, as a button that names it asks.
    function reveal(videoId) {
        const row = App.videos.rowOfVideo(videoId)
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
    function thumbDecodeWidth(row) {
        const item = grid.itemAtIndex(row)
        return item ? item.thumbnail.decodeWidth : 0
    }

    // Another section is another list: it starts at the top rather than
    // at the rows the last one was scrolled to.
    Connections {
        target: App.videos
        function onSectionChanged() {
            grid.rowOffset = 0
            grid.keepRows()
        }
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
        onFitWidthChanged: if (visible && App.mode === "video") shownWidth = fitWidth
        onEnabledChanged: if (enabled && visible) shownWidth = fitWidth
        x: 18
        y: 0
        width: shownWidth
        height: parent.height

        // As many across as keep a card within the chosen width (Settings
        // → Appearance): the cards share the row and grow with it, and
        // one more goes across once they would pass that width.
        readonly property int columns: Math.max(1, Math.ceil(width / App.thumbWidth))
        cellWidth: Math.floor(width / columns)
        cellHeight: Math.round((cellWidth - 20) * 9 / 16) + 74

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
            orphanView: App.videos.section === "orphans"
            judgeOn: App.judgeMode !== "off"
            canDelete: !App.scanning
            selected: root.isSelected(videoId)
            onActivated: { root.clearSelection(); root.activated(index) }
            onMenuRequested: root.openMenu(index, card)
            onToggleRequested: root.toggleSelected(index)
            verdictsEnabled: !root.verdictCooldown
            onApproved: { root.verdictGiven(); root.approved(videoId) }
            onRejected: { root.verdictGiven(); root.rejected(videoId) }
            onAllowChannelRequested: root.channelRequested(videoId, true)
            onDenyChannelRequested: root.channelRequested(videoId, false)
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

    Popup {
        id: menu
        property var video: ({})
        property var editIds: []
        readonly property bool canEdit: !(video.review || video.orphan)
        function pick() {
            const ids = editIds
            const subject = ids.length > 1 ? ids.length + " songs" : (video.title || "")
            close()
            root.editRequested(ids, subject)
        }
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
        contentItem: Column {
            Item {
                width: menu.availableWidth
                height: 34
                enabled: menu.canEdit
                opacity: enabled ? 1 : 0.4
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusSmall
                    color: Theme.hover
                    opacity: editMouse.containsMouse ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                }
                Icon {
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    path: Icons.edit
                    size: 16
                    color: Theme.textDim
                }
                Text {
                    x: 34
                    width: parent.width - 44
                    anchors.verticalCenter: parent.verticalCenter
                    text: menu.editIds.length > 1 ? "Edit tags of " + menu.editIds.length + " songs" : "Edit tags"
                    color: Theme.text
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                MouseArea {
                    id: editMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: menu.pick()
                }
            }
            Text {
                width: menu.availableWidth
                leftPadding: 10
                rightPadding: 10
                topPadding: 2
                bottomPadding: 8
                text: !menu.canEdit ? (menu.video.review ? "A video under review has no track of its own yet: accept it first."
                                                         : "No track in your music folders has this video.")
                    : menu.editIds.length > 1 ? "The tags their music files share. Titles and track numbers are each file's own."
                    : "The tags of its music file, written into the file. The video keeps its file and is shown under the new tags."
                color: Theme.textFaint
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
            // The video's file, in the file manager
            Item {
                width: menu.availableWidth
                height: 34
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusSmall
                    color: Theme.hover
                    opacity: locationMouse.containsMouse ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                }
                Icon {
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    path: Icons.folder
                    size: 16
                    color: Theme.textDim
                }
                Text {
                    x: 34
                    width: parent.width - 44
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Open file location"
                    color: Theme.text
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                MouseArea {
                    id: locationMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        const path = menu.video.path
                        menu.close()
                        App.openFileLocation(path)
                    }
                }
            }
            Text {
                width: menu.availableWidth
                leftPadding: 10
                rightPadding: 10
                topPadding: 2
                bottomPadding: 8
                text: "The video's file, in the file manager."
                color: Theme.textFaint
                font.pixelSize: 11
                wrapMode: Text.WordWrap
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

        readonly property bool filtering: root.searchText.trim().length > 0 || App.videos.facetType !== "all"
        // Review and Orphans empty are not a failed search.
        readonly property string sectionEmpty: !filtering && App.videos.section !== "library" ? App.videos.section : ""

        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 56
            height: 56
            Spinner {
                anchors.centerIn: parent
                size: 30
                visible: !parent.parent.filtering && parent.parent.sectionEmpty === "" && App.busy
            }
            Icon {
                anchors.centerIn: parent
                visible: parent.parent.filtering || parent.parent.sectionEmpty !== "" || !App.busy
                path: parent.parent.filtering ? Icons.search : parent.parent.sectionEmpty === "review" ? Icons.review : Icons.movie
                size: 40
                color: Theme.textFaint
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: parent.filtering ? "No videos match"
                : parent.sectionEmpty === "review" ? "Nothing to review"
                : parent.sectionEmpty === "orphans" ? "No orphaned videos"
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
                : parent.sectionEmpty === "review" ? "Videos that wait for your verdict appear here."
                : parent.sectionEmpty === "orphans" ? "Videos whose tracks are gone from the music folders appear here."
                : App.busy ? "Videos appear here as soon as each one is found, checked against your track and downloaded."
                : "None of the tracks in your music folders has a music video that could be found."
            color: Theme.textDim
            font.pixelSize: 13
        }
    }
}
