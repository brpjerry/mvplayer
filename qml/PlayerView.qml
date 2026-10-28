import QtQuick
import MvPlayer.Core

// The video surface. Expanded, it fills its area. Collapsed, it sits exactly
// on its own thumbnail in the library grid and keeps playing there, following
// the thumbnail as the grid scrolls. Opening a video is the same motion in
// reverse: the thumbnail grows into the player.
//
// The video box never changes size while it moves: it is transformed, so the
// video's render target is not reallocated mid-animation.
Item {
    id: root

    property var current: null       // video map
    property bool fullscreen: false
    readonly property alias mpv: mpvItem
    readonly property bool expanded: expand > 0.999
    readonly property bool collapsed: expand < 0.001
    readonly property bool cursorIdle: fullscreen && !pointerActive

    // 1 = video fills the view, 0 = video sits in `restRect`
    property real expand: 0
    property bool shown: false       // false: nothing loaded, layer faded out

    signal expandRequested()
    signal closeRequested()
    signal togglePauseRequested()
    signal toggleFullscreenRequested()
    // The verdict on the video under review (or the orphan, among the
    // orphans) that is playing in its thumbnail.
    signal reviewApproved()
    signal reviewRejected()
    signal reviewChannelRequested(bool allow)
    // Its candidate tracks: how many, and the title of the one on show.
    property int candidates: 1
    property string candidateTitle

    // ---- Geometry ----------------------------------------------------------
    readonly property real videoAspect: {
        const s = mpvItem.videoSize
        return s.width > 0 && s.height > 0 ? s.width / s.height : 16 / 9
    }
    readonly property real boxWidth: Math.round(Math.min(width, height * videoAspect))
    readonly property real boxHeight: Math.round(Math.min(height, width / videoAspect))

    // Where the video rests when collapsed: its thumbnail in the grid, in
    // this item's coordinates. Not available while that thumbnail is scrolled
    // far away or filtered out; the video is then hidden (audio continues).
    property rect anchorRect
    property bool anchorAvailable: false
    readonly property rect fallbackRect: Qt.rect(width * 0.04, height * 0.04, width * 0.92, height * 0.92)
    readonly property rect restRect: anchorAvailable ? anchorRect : fallbackRect

    clip: true

    // ---- Motion ------------------------------------------------------------
    NumberAnimation {
        id: motion
        target: root
        property: "expand"
        duration: 380
        easing.type: Easing.BezierSpline
        easing.bezierCurve: Theme.emphasized
    }
    function animateTo(value) {
        motion.stop()
        motion.from = root.expand
        motion.to = value
        motion.start()
    }

    // Grow out of the thumbnail. `decodeWidth` is the size the grid decoded
    // that thumbnail at, so the same cached picture can be shown immediately.
    function openFrom(decodeWidth) {
        motion.stop()
        posterHint = decodeWidth || 0
        if (anchorAvailable) {
            // Start exactly on top of the thumbnail, fully opaque.
            expand = 0
            presenceFade.enabled = false
            shown = true
            presenceFade.enabled = true
        }
        shown = true
        posterShown = true
        animateTo(1)
    }
    // Start playing where the thumbnail is, without growing: a video under
    // review is judged from the list.
    function openCollapsed(decodeWidth) {
        motion.stop()
        posterHint = decodeWidth || 0
        expand = 0
        if (anchorAvailable) {
            presenceFade.enabled = false
            shown = true
            presenceFade.enabled = true
        }
        shown = true
        posterShown = true
    }
    function showFull() {
        shown = true
        animateTo(1)
    }
    function collapse() {
        animateTo(0)
    }
    function close() {
        cancelLoad()
        shown = false
    }

    // Fades the whole layer in and out as playback starts and stops.
    property real presence: shown ? 1 : 0
    Behavior on presence { id: presenceFade; NumberAnimation { duration: Theme.normal } }
    // How much of the area behind is covered by the player (0..1).
    readonly property real cover: expand * presence

    // ---- Loading -----------------------------------------------------------
    // The thumbnail ("poster") covers the surface from the moment a video is
    // chosen until its first frame is up. The file itself is only opened once
    // the transition has finished: mpv sets up decoding and shaders for each
    // new file on the render thread, and doing that mid-animation would drop
    // frames. The previous video is frozen meanwhile.
    property bool posterShown: false
    property int posterHint: 0
    property string pendingPath: ""
    property bool awaitingFrame: false // opened, its first frame not up yet
    // Until then the frozen previous video is no pause to anyone outside.
    readonly property bool loading: pendingPath !== "" || awaitingFrame
    readonly property bool settling: motion.running || posterFade.running

    // `at`: a track going on as its video. A function that gives the
    // second of the video to open at, asked when the file is opened: the
    // moment of the song by then. The sound is not stopped first: it fades
    // out while the layer grows, and the file is opened once the fade has
    // been heard (mpv has about a fifth of a second in the speaker's
    // buffer, and a new file drops what is buffered). Its own sound fades
    // in on its first frame.
    property var pendingAt: null
    function beginLoad(path, at) {
        reveal.stop()
        posterShown = true
        fadeOut.stop()
        if (at) {
            pendingAt = at
            fadeOut.start()
        } else {
            pendingAt = null
            if (mpvItem.active)
                mpvItem.paused = true
        }
        pendingPath = path
        Qt.callLater(flushLoad)
    }
    function flushLoad() {
        if (pendingPath === "" || settling || fadeOut.running)
            return
        const path = pendingPath
        const at = pendingAt
        awaitingFrame = true
        pendingPath = ""
        pendingAt = null
        mpvItem.load(path, at ? at() : -1)
    }
    function cancelLoad() {
        pendingPath = ""
        pendingAt = null
        awaitingFrame = false
        fadeOut.stop()
        fadeIn.stop()
        mpvItem.fade = 1
    }
    onSettlingChanged: flushLoad()
    SequentialAnimation {
        id: fadeOut
        NumberAnimation { target: mpvItem; property: "fade"; to: 0; duration: 200 }
        PauseAnimation { duration: 250 } // the fade through the buffer
        onFinished: root.flushLoad()
    }
    NumberAnimation { id: fadeIn; target: mpvItem; property: "fade"; to: 1; duration: 200 }

    Timer {
        id: reveal
        interval: 60 // give mpv a couple of frames to draw before uncovering it
        onTriggered: root.posterShown = false
    }
    Connections {
        target: mpvItem
        function onFirstFrame() {
            root.awaitingFrame = false
            reveal.restart()
            // Faded out for a change of file (or left part way by a load
            // that cut one short): back up for the new one.
            if (mpvItem.fade < 1)
                fadeIn.restart()
        }
    }

    // ---- Pointer activity (fullscreen auto-hide) ---------------------------
    property bool pointerActive: true
    Timer {
        id: pointerTimer
        interval: 1200
        onTriggered: root.pointerActive = false
    }
    // Qt re-delivers hover events whenever the scene under a resting cursor
    // changes (every video frame), so only a real change of position counts.
    property point lastPointer
    function pointerMoved(pos) {
        if (pos !== undefined) {
            if (Math.abs(pos.x - lastPointer.x) < 1 && Math.abs(pos.y - lastPointer.y) < 1)
                return
            lastPointer = pos
        }
        pointerActive = true
        pointerTimer.restart()
    }
    onFullscreenChanged: pointerMoved()

    // Backdrop behind the video in the expanded state
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: root.cover
        visible: opacity > 0
    }

    Item {
        id: box
        width: root.boxWidth
        height: root.boxHeight
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        // Stays in the scene even when nothing is playing (fully transparent):
        // the video surface and mpv's GL state are then set up at startup
        // instead of stalling the first transition.
        // With no thumbnail to rest on, it fades as it collapses.
        opacity: root.presence * (root.anchorAvailable ? 1 : Math.min(1, root.expand * 1.6))
        enabled: root.shown && (root.anchorAvailable || root.expand > 0)

        readonly property real t: 1 - root.expand
        // Fit inside the rest rectangle, centred (the two only differ in
        // shape for videos that are not 16:9).
        readonly property real restScale: Math.min(root.restRect.width / Math.max(1, width),
                                                   root.restRect.height / Math.max(1, height))
        readonly property real restX: root.restRect.x + (root.restRect.width - width * restScale) / 2
        readonly property real restY: root.restRect.y + (root.restRect.height - height * restScale) / 2
        readonly property real k: 1 + (restScale - 1) * t

        transform: [
            Scale { xScale: box.k; yScale: box.k },
            Translate {
                x: box.t * (box.restX - box.x)
                y: box.t * (box.restY - box.y)
            }
        ]

        // Rounded corners while away from full size. Radius is given in the
        // box's own (unscaled) units so it stays constant on screen.
        layer.enabled: box.t > 0.001 && root.presence > 0
        layer.effect: ShaderEffect {
            property vector2d size: Qt.vector2d(box.width, box.height)
            property real radius: Math.min(1, box.t * 4) * Theme.radius / box.k
            property vector2d uvScale: Qt.vector2d(1, 1)
            property real zoom: 1
            property real dim: 0
            fragmentShader: "qrc:/shaders/rounded.frag.qsb"
        }

        Rectangle { anchors.fill: parent; color: "black" }

        MpvItem {
            id: mpvItem
            anchors.fill: parent
        }

        Item {
            id: poster
            anchors.fill: parent
            opacity: root.posterShown ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { id: posterFade; duration: 220 } }

            // The grid's copy of the thumbnail is already decoded: instant.
            Image {
                anchors.fill: parent
                visible: root.posterHint > 0
                source: root.posterHint > 0 && root.current ? root.current.thumb : ""
                sourceSize.width: root.posterHint
                fillMode: Image.PreserveAspectCrop
                asynchronous: false
            }
            // The full-size picture sharpens it once loaded.
            Image {
                anchors.fill: parent
                source: root.current ? root.current.thumb : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.normal } }
            }
        }

        // Affordances while playing inside the thumbnail
        Rectangle {
            anchors.fill: parent
            color: "#0c0d10"
            opacity: root.collapsed && boxMouse.containsMouse ? 0.45 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        }
        Item {
            // Counter-scale so controls keep their size on the shrunken box.
            anchors.fill: parent
            visible: root.collapsed
            opacity: boxMouse.containsMouse || closeMini.hovered ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }

            Icon {
                anchors.centerIn: parent
                path: Icons.expand
                size: 28 / box.k
                color: Theme.scrimText
            }
        }

        MouseArea {
            id: boxMouse
            anchors.fill: parent
            // Out of the scene while there is nothing to click: at rest the
            // box lies over the whole grid, and the hover Qt re-delivers on
            // every frame would go to it instead of the card underneath.
            visible: box.enabled
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: root.collapsed ? Qt.PointingHandCursor
                       : root.cursorIdle ? Qt.BlankCursor : Qt.ArrowCursor
            onPositionChanged: (mouse) => root.pointerMoved(mapToItem(root, mouse.x, mouse.y))
            onClicked: {
                if (root.collapsed)
                    root.expandRequested()
                else if (root.expanded)
                    pauseClick.restart()
            }
            // A click pauses once no second click has followed it: the
            // first click of a double click going full screen left the
            // picture stopped for a moment, and the sound cut in and out.
            onDoubleClicked: {
                pauseClick.stop()
                if (root.expanded)
                    root.toggleFullscreenRequested()
            }
            Timer {
                id: pauseClick
                interval: Qt.styleHints.mouseDoubleClickInterval
                onTriggered: if (root.expanded) root.togglePauseRequested()
            }
        }

        IconButton {
            id: closeMini
            visible: box.enabled && root.collapsed && !verdict.judged
            opacity: boxMouse.containsMouse || hovered ? 1 : 0
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 6 / box.k
            transformOrigin: Item.TopRight
            scale: 1 / box.k
            size: 28
            iconSize: 16
            icon: Icons.close
            color: Theme.scrimText
            hoverColor: Theme.text
            onClicked: root.closeRequested()
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        }
    }

    // The thumbnail's review controls are underneath the video now: the same
    // ones, on top of it. Over the thumbnail's rectangle, not the video's: a
    // video that is not 16:9 covers only part of it.
    ReviewControls {
        id: verdict
        // Under review, or an orphan shown among the orphans.
        readonly property bool judged: root.current !== null
            && (root.current.review === true || (root.current.orphan === true && App.videos.section === "orphans"))
        readonly property bool wanted: root.collapsed && judged
        visible: wanted && box.enabled
        x: root.restRect.x
        y: root.restRect.y
        width: root.restRect.width
        height: root.restRect.height
        candidates: wanted && root.current.review === true ? root.candidates : 1
        candidateTitle: wanted && root.current.review === true ? root.candidateTitle : ""
        sameTitle: wanted ? root.current.sameTitle : ({})
        orphan: wanted && root.current.review !== true
        canDelete: !App.scanning
        videoId: wanted ? root.current.videoId : -1
        onApproved: root.reviewApproved()
        onRejected: root.reviewRejected()
        onAllowChannelRequested: root.reviewChannelRequested(true)
        onDenyChannelRequested: root.reviewChannelRequested(false)
    }

    // Pause / play flash in the middle of the video
    Rectangle {
        id: flash
        anchors.centerIn: parent
        width: 84
        height: 84
        radius: 42
        color: "#b30c0d10"
        opacity: 0
        visible: opacity > 0
        property string icon: Icons.pause
        Icon {
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: flash.icon === Icons.play ? 3 : 0
            path: flash.icon
            size: 44
            color: Theme.scrimText
        }
        ParallelAnimation {
            id: flashAnim
            NumberAnimation { target: flash; property: "opacity"; from: 0.95; to: 0; duration: 520; easing.type: Easing.InCubic }
            NumberAnimation { target: flash; property: "scale"; from: 0.8; to: 1.25; duration: 520; easing.type: Easing.OutCubic }
        }
    }
    function flashState(paused) {
        if (!expanded || !shown)
            return
        flash.icon = paused ? Icons.pause : Icons.play
        flashAnim.restart()
    }

    // Mouse movement over the letterbox bars also counts as activity.
    HoverHandler {
        enabled: root.fullscreen
        onPointChanged: root.pointerMoved(point.position)
    }
}
