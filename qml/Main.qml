import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import MvPlayer.Core

ApplicationWindow {
    id: window

    width: 1440
    height: 900
    minimumWidth: 960
    minimumHeight: 600
    visible: true
    color: Theme.bg
    // Windows: no system title bar; the top bar carries the window controls.
    flags: WindowFrame.custom ? Qt.Window | Qt.FramelessWindowHint : Qt.Window
    title: player.current ? player.current.title + " · " + player.current.artist.replace(/; /g, ", ") + " — MV Player"
                          : "MV Player"

    // "grid": library; the video (if any) plays inside its own thumbnail.
    // "player": the video replaces the grid.
    property string view: "grid"
    readonly property bool fullscreen: visibility === Window.FullScreen
    property int visibilityBeforeFullscreen: Window.Windowed
    readonly property MpvItem mpv: playerView.mpv

    function setFullscreen(on) {
        if (on === fullscreen)
            return
        if (on) {
            if (!player.current)
                return
            visibilityBeforeFullscreen = visibility
            if (view !== "player")
                showPlayer()
            showFullScreen()
        } else {
            visibility = visibilityBeforeFullscreen === Window.Maximized ? Window.Maximized : Window.Windowed
        }
    }
    function showPlayer() {
        if (!player.current)
            return
        releaseTextFocus()
        view = "player"
        playerView.showFull()
    }
    function showGrid() {
        if (fullscreen)
            setFullscreen(false)
        view = "grid"
        if (player.current)
            playerView.collapse()
    }
    // True while something is playing or about to (a video is being opened).
    readonly property bool playingNow: player.current !== null && (!mpv.paused || playerView.loading)

    function togglePause() {
        if (!player.current || playerView.loading)
            return
        playerView.flashState(!mpv.paused)
        mpv.togglePause()
    }

    // Another option for the same track comes on show; if the one it replaces
    // was playing, the new one plays.
    function stepReview(id, delta) {
        const wasPlaying = player.current && player.current.videoId === id
        const shown = App.videos.stepReview(id, delta)
        if (wasPlaying && shown !== id)
            player.playRow(App.videos.rowOfVideo(shown))
        return shown
    }

    // An accepted video leaves the review list; it stops rather than play on
    // out of sight, and its file is tidied up meanwhile.
    function approveVideo(id) {
        if (player.current && player.current.videoId === id)
            player.stop()
        App.approveVideo(id)
    }

    // A rejected video is deleted: it cannot go on playing.
    function rejectVideo(id) {
        if (player.current && player.current.videoId === id)
            player.stop()
        App.rejectVideo(id)
    }

    // ---- Playback queue ----------------------------------------------------
    QtObject {
        id: player

        property var queue: []      // snapshot of the grid when playback started
        property int index: -1
        property var current: null
        property bool shuffle: false
        property int repeatMode: 0  // 0 off, 1 all, 2 one
        property var history: []    // indices played, for "previous" while shuffling

        function load(i) {
            index = i
            current = queue[i]
            playerView.beginLoad(current.path)
        }

        // Start playing the grid's `row`; the video grows out of its thumbnail.
        function playRow(row) {
            const list = App.videos.snapshot()
            if (row < 0 || row >= list.length)
                return
            // The video that is already loaded: just bring it forward.
            if (current && list[row].videoId === current.videoId) {
                window.showPlayer()
                return
            }
            const decodeWidth = grid.thumbDecodeWidth(row)
            queue = list
            history = []
            load(row)
            // A video under review plays in its thumbnail, next to the
            // others and with its verdict buttons; a click on it enlarges it.
            if (list[row].review) {
                window.view = "grid"
                playerView.openCollapsed(decodeWidth)
                return
            }
            window.releaseTextFocus()
            window.view = "player"
            playerView.openFrom(decodeWidth)
        }

        // `automatic`: called because the current video ended.
        function next(automatic) {
            if (queue.length === 0)
                return false
            if (automatic && repeatMode === 2) {
                load(index)
                return true
            }
            let n = index + 1
            if (shuffle && queue.length > 1) {
                do {
                    n = Math.floor(Math.random() * queue.length)
                } while (n === index)
            } else if (n >= queue.length) {
                if (automatic && repeatMode !== 1)
                    return false
                n = 0
            }
            history.push(index)
            load(n)
            return true
        }

        function previous() {
            if (queue.length === 0)
                return
            if (window.mpv.position > 3 || queue.length === 1) {
                window.mpv.seek(0, true)
                return
            }
            if (history.length > 0) {
                load(history.pop())
                return
            }
            load(index > 0 ? index - 1 : queue.length - 1)
        }

        function stop() {
            window.mpv.stop()
            if (window.fullscreen)
                window.setFullscreen(false)
            playerView.close()
            window.view = "grid"
            current = null
            index = -1
        }
    }

    Connections {
        target: window.mpv
        function onEndReached() {
            if (!player.next(true))
                player.stop()
        }
        function onLoadFailed(reason) {
            console.warn("playback failed:", reason)
            if (!player.next(true))
                player.stop()
        }
    }

    Binding { target: window.mpv; property: "volume"; value: App.volume }
    Binding { target: window.mpv; property: "muted"; value: App.muted }
    // Subtitles belong to the full view: over a thumbnail they are specks.
    Binding { target: window.mpv; property: "subtitlesVisible"; value: App.subtitlesOn && (window.view === "player" || window.fullscreen) }
    Binding { target: window.mpv; property: "subtitleLangs"; value: App.subtitleLangs.replace(/ /g, "") }
    Binding { target: IdleInhibitor; property: "active"; value: window.playingNow }

    // ---- Accent colour -----------------------------------------------------
    // Either the colour chosen in Settings, or ("auto") one that follows the
    // picture of the playing video, changing slowly.
    readonly property bool autoAccent: App.accent === "auto"
    Binding { target: window.mpv; property: "colorSampling"; value: window.autoAccent }
    Binding {
        target: Theme
        property: "accentSource"
        value: !window.autoAccent ? App.accent
             : player.current && window.mpv.frameColorValid ? window.mpv.frameColor : Theme.defaultAccent
    }
    Binding { target: Theme; property: "accentFade"; value: window.autoAccent ? 800 : 300 }

    // ---- Dark / light ------------------------------------------------------
    Binding {
        target: Theme
        property: "dark"
        value: App.themeMode === "auto" ? App.systemDark : App.themeMode !== "light"
    }

    // ---- Layout ------------------------------------------------------------
    Item {
        id: frame
        anchors.fill: parent
        // A maximised window without its system frame overlaps the screen edges.
        anchors.margins: window.visibility === Window.Maximized ? WindowFrame.maximizedInset : 0

        Sidebar {
            id: sidebar
            width: 248
            anchors.top: parent.top
            anchors.bottom: controlBar.top
            onNavigated: if (window.view === "player") window.showGrid()
            onSettingsRequested: settings.open()
        }

        Item {
            id: mainArea
            anchors.left: sidebar.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: controlBar.top

            TopBar {
                id: topBar
                width: parent.width
                playerView: window.view === "player"
                current: player.current
                onBackRequested: window.showGrid()
                onSearchTextChanged: {
                    App.videos.searchText = searchText
                    if (searchText.length > 0 && window.view === "player")
                        window.showGrid()
                }
            }

            Item {
                id: pane
                anchors.top: topBar.bottom
                anchors.bottom: parent.bottom
                width: parent.width

                VideoGrid {
                    id: grid
                    anchors.fill: parent
                    // Recedes as the player grows over it.
                    opacity: 1 - playerView.cover
                    scale: 1 - 0.03 * playerView.cover
                    visible: opacity > 0 && App.configured
                    searchText: topBar.searchText
                    currentVideoId: player.current ? player.current.videoId : -1
                    currentPlaying: window.playingNow
                    onActivated: (row) => player.playRow(row)
                    onApproved: (id) => window.approveVideo(id)
                    onRejected: (id) => window.rejectVideo(id)
                    onStepped: (id, delta) => window.stepReview(id, delta)
                }

                Welcome {
                    anchors.fill: parent
                    visible: !App.configured
                }
            }
        }

        PlayerView {
            id: playerView
            x: window.fullscreen ? 0 : sidebar.width
            y: window.fullscreen ? 0 : topBar.height
            width: window.fullscreen ? window.width : mainArea.width
            height: window.fullscreen ? window.height : pane.height
            current: player.current
            fullscreen: window.fullscreen
            // The grid occupies the same rectangle, so its coordinates carry over.
            anchorRect: grid.currentThumbRect
            anchorAvailable: grid.currentThumbAvailable && grid.visible
            onExpandRequested: window.showPlayer()
            onCloseRequested: player.stop()
            onReviewApproved: window.approveVideo(player.current.videoId)
            onReviewRejected: window.rejectVideo(player.current.videoId)
            onReviewStepped: (delta) => window.stepReview(player.current.videoId, delta)
            onTogglePauseRequested: window.togglePause()
            onToggleFullscreenRequested: window.setFullscreen(!window.fullscreen)
        }

        ControlBar {
            id: controlBar
            width: parent.width
            readonly property bool revealed: !window.fullscreen || playerView.pointerActive || hovered || window.mpv.paused
            y: parent.height - height + (revealed ? 0 : height)
            opacity: revealed ? 1 : 0
            Behavior on y { enabled: window.fullscreen; NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
            Behavior on opacity { enabled: window.fullscreen; NumberAnimation { duration: Theme.fast } }

            mpv: window.mpv
            current: player.current
            playing: window.playingNow
            overlay: window.fullscreen
            fullscreen: window.fullscreen
            shuffle: player.shuffle
            repeatMode: player.repeatMode
            canStep: player.queue.length > 1
            onPlayPauseRequested: window.togglePause()
            onNextRequested: player.next(false)
            onPreviousRequested: player.previous()
            onShuffleToggled: player.shuffle = !player.shuffle
            onRepeatCycled: player.repeatMode = (player.repeatMode + 1) % 3
            onFullscreenToggled: window.setFullscreen(!window.fullscreen)
            onNowPlayingClicked: window.view === "player" ? window.showGrid() : window.showPlayer()
        }
    }

    SettingsPanel { id: settings }

    // Frame rate counter (F12)
    Rectangle {
        visible: FrameStats.visible
        x: (window.fullscreen ? 0 : sidebar.width) + 14
        y: controlBar.y - height - 14
        z: 100
        width: fpsLabel.implicitWidth + 16
        height: 24
        radius: Theme.radiusSmall
        color: "#cc0c0d10"
        Text {
            id: fpsLabel
            anchors.centerIn: parent
            text: FrameStats.fps + " fps · worst " + FrameStats.worstMs.toFixed(1) + " ms · " + Math.round(FrameStats.refreshRate) + " Hz"
            color: FrameStats.fps > 0 && FrameStats.fps < FrameStats.refreshRate * 0.9 ? Theme.warn : Theme.good
            font.pixelSize: 11
            font.features: { "tnum": 1 }
        }
    }

    // Pressing anywhere outside a text field takes keyboard focus away from
    // it, so the single-key shortcuts below work again.
    readonly property bool typing: activeFocusItem instanceof TextInput
    function releaseTextFocus() {
        if (window.typing)
            window.contentItem.forceActiveFocus()
    }
    PointHandler {
        parent: window.contentItem
        onActiveChanged: {
            if (!active || !window.typing)
                return
            const field = window.activeFocusItem
            const p = field.mapFromItem(null, point.scenePressPosition.x, point.scenePressPosition.y)
            if (!field.contains(p))
                window.releaseTextFocus()
        }
    }

    // ---- Keyboard ----------------------------------------------------------

    Shortcut { sequence: "Space"; enabled: !window.typing; onActivated: window.togglePause() }
    Shortcut { sequence: "Left"; enabled: !window.typing; onActivated: window.mpv.seekRelative(-5) }
    Shortcut { sequence: "Right"; enabled: !window.typing; onActivated: window.mpv.seekRelative(5) }
    Shortcut { sequence: "Shift+Left"; enabled: !window.typing; onActivated: window.mpv.seekRelative(-30) }
    Shortcut { sequence: "Shift+Right"; enabled: !window.typing; onActivated: window.mpv.seekRelative(30) }
    Shortcut { sequence: "Up"; enabled: !window.typing; onActivated: App.volume = Math.min(1, App.volume + 0.05) }
    Shortcut { sequence: "Down"; enabled: !window.typing; onActivated: App.volume = Math.max(0, App.volume - 0.05) }
    Shortcut { sequence: "M"; enabled: !window.typing; onActivated: App.muted = !App.muted }
    Shortcut { sequence: "F"; enabled: !window.typing; onActivated: window.setFullscreen(!window.fullscreen) }
    Shortcut { sequence: "N"; enabled: !window.typing; onActivated: player.next(false) }
    Shortcut { sequence: "P"; enabled: !window.typing; onActivated: player.previous() }
    Shortcut { sequences: ["Ctrl+F", "/"]; enabled: !window.typing; onActivated: { if (window.view === "player") window.showGrid(); topBar.focusSearch() } }
    Shortcut {
        sequence: "Escape"
        enabled: !window.typing
        onActivated: {
            if (window.fullscreen)
                window.setFullscreen(false)
            else if (window.view === "player")
                window.showGrid()
        }
    }
    Shortcut { sequence: "F11"; onActivated: window.setFullscreen(!window.fullscreen) }
    Shortcut { sequence: "F12"; onActivated: FrameStats.visible = !FrameStats.visible }
    Shortcut { sequence: "Ctrl+,"; onActivated: settings.open() }
    Shortcut { sequence: "Ctrl+Q"; onActivated: Qt.quit() }

    // ---- Automation (--ipc) ------------------------------------------------
    NumberAnimation { id: scrollTest; target: grid.view; property: "contentY"; easing.type: Easing.InOutSine }

    function ipc(line) {
        const sp = line.indexOf(" ")
        const cmd = sp < 0 ? line : line.slice(0, sp)
        const arg = sp < 0 ? "" : line.slice(sp + 1)
        switch (cmd) {
        case "play": {
            const row = parseInt(arg)
            player.playRow(row)
            return "ok"
        }
        case "back": showGrid(); return "ok"
        case "expand": showPlayer(); return "ok"
        case "toggle": togglePause(); return "ok"
        case "next": player.next(false); return "ok"
        case "previous": player.previous(); return "ok"
        case "stop": player.stop(); return "ok"
        case "seek": mpv.seek(parseFloat(arg), true); return "ok"
        case "audio": mpv.audioTrack = parseInt(arg); return "ok"
        case "search": topBar.searchText = arg; return "ok"
        case "facet": {
            const s = arg.indexOf(" ")
            App.videos.setFacet(s < 0 ? arg : arg.slice(0, s), s < 0 ? "" : arg.slice(s + 1))
            return "ok"
        }
        case "sort": App.videos.sortMode = arg; return "ok"
        case "fullscreen": setFullscreen(arg !== "off"); return "ok"
        case "settings": arg === "off" ? settings.close() : settings.open(); return "ok"
        case "retry": App.retryUnmatched(); return "ok"
        case "rescan": App.rescan(); return "ok"
        case "cookies": return (arg === "" ? (App.removeCookies(), "") : App.importCookies(arg)) || "ok"
        case "check-quality": App.checkQuality(); return "ok"
        case "check-cookies": App.checkCookies(); return "ok"
        case "fetch-subtitles": App.fetchSubtitles(); return "ok"
        case "subtitles": // subtitles on|off, or the languages to fetch: subtitles en,ja
            if (arg === "on" || arg === "off") App.subtitlesOn = arg === "on"
            else App.subtitleLangs = arg
            return "ok"
        case "step": { // step <video id> <delta>
            const sp = arg.split(" ")
            return "" + window.stepReview(parseInt(sp[0]), parseInt(sp[1] || "1"))
        }
        case "approve": window.approveVideo(parseInt(arg)); return "ok"
        case "reject": window.rejectVideo(parseInt(arg)); return "ok"
        case "grab": // grab <file>: a picture of the settings panel when open, else of the window
            (settings.opened ? settings.contentItem : frame).grabToImage(r => r.saveToFile(arg))
            return "ok"
        case "ytdlp-update": YtDlpUpdater.update(); return "ok"
        case "ytdlp": return JSON.stringify({ version: YtDlpUpdater.version, busy: YtDlpUpdater.busy, status: YtDlpUpdater.status })
        case "fps": FrameStats.visible = arg !== "off"; return "ok"
        case "accent": App.accent = arg; return "ok"
        case "theme": App.themeMode = arg; return "ok"
        case "add-music": App.addMusicDir(arg); return "ok"
        case "remove-music": App.removeMusicDir(arg); return "ok"
        case "sidebar": App.sidebarFacet = arg; return "ok"
        case "wheel": { // wheel <pixelDy> <angleDy> <phase>: feed the grid's scroll handler
            const w = arg.split(" ")
            grid.wheel.scroll(parseFloat(w[0]), parseFloat(w[1]), parseInt(w[2]))
            return "" + grid.view.contentY
        }
        case "scroll": {
            const parts = arg.split(" ")
            scrollTest.stop()
            scrollTest.from = grid.view.contentY
            scrollTest.to = parseFloat(parts[0])
            scrollTest.duration = parts.length > 1 ? parseInt(parts[1]) : 0
            scrollTest.start()
            return "ok"
        }
        case "stats-begin": FrameStats.begin(); return "ok"
        case "stats-end": return JSON.stringify(FrameStats.end())
        case "state":
            return JSON.stringify({
                view: view, fullscreen: fullscreen, shown: App.videos.count, total: App.videoCount,
                playing: player.current ? player.current.title : null, index: player.index,
                position: mpv.position, duration: mpv.duration, paused: mpv.paused, active: mpv.active, muted: mpv.muted,
                hwdec: mpv.hwdec, videoSize: mpv.videoSize.width + "x" + mpv.videoSize.height,
                audioTrack: mpv.audioTrack, audioTracks: mpv.audioTracks.length,
                busy: App.busy, status: App.statusText, counts: App.trackCounts,
                subtitleLangs: App.subtitleLangs, subtitlesOn: App.subtitlesOn, hasSubtitles: window.mpv.hasSubtitles,
                fetchingSubtitles: App.fetchingSubtitles, cookies: App.hasCookies, checkingCookies: App.checkingCookies, cookiesState: App.cookiesState,
                cookiesStatus: App.cookiesStatus, checkingQuality: App.checkingQuality, reviewCount: App.reviewCount,
                facet: App.videos.facetType, reviewLabel: controlBar.reviewLabel,
                accent: "" + Theme.accent, accentMode: App.accent, theme: App.themeMode, dark: Theme.dark,
                systemDark: App.systemDark, bg: "" + Theme.bg, musicDirs: App.musicDirs, frameColor: "" + mpv.frameColor,
                frameColorValid: mpv.frameColorValid, sidebar: App.sidebarFacet,
                anchor: playerView.anchorAvailable ? [Math.round(playerView.anchorRect.x), Math.round(playerView.anchorRect.y), Math.round(playerView.anchorRect.width)] : null,
                contentY: grid.view.contentY, flicking: grid.view.flicking,
                expand: playerView.expand, contentHeight: grid.view.contentHeight, gridHeight: grid.view.height,
                window: width + "x" + height, dpr: Screen.devicePixelRatio
            })
        case "quit": Qt.quit(); return "ok"
        }
        return "unknown command"
    }
}
