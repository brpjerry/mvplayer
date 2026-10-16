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
            if (!player.current || (player.current.kind === "track" && !player.videoUp))
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
        if (!player.current || (player.current.kind === "track" && !player.videoUp))
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

    // Another video under review that a candidate track of this one is a
    // candidate of as well: its card comes into view.
    function revealVideo(id) {
        if (window.view !== "grid")
            showGrid()
        return grid.reveal(id)
    }

    // A track's music video, asked for from the music library: the video
    // library comes up on its card and it plays, as a click on the card
    // would. A grid narrowed to a tag or a search that leaves it out is
    // widened first.
    function playVideo(id) {
        App.mode = "video"
        showSection("library")
        let row = revealVideo(id)
        if (row < 0) {
            topBar.searchText = ""
            sidebar.select("all", "")
            row = revealVideo(id)
        }
        if (row >= 0)
            player.playRow(row)
    }

    // An accepted video leaves the review list; it stops rather than play on
    // out of sight, and its file is tidied up meanwhile. Among the orphans
    // the verdict is on an orphan, and a kept one leaves that list alike.
    // "Accept all" (X4): the judge's confident answers as verdicts. The
    // playing video may be among them.
    function acceptJudged() {
        if (player.current && player.current.review === true)
            player.stop()
        App.acceptJudged()
    }

    function approveVideo(id) {
        if (player.current && player.current.videoId === id)
            player.stop()
        if (App.videos.section === "orphans")
            App.keepOrphan(id)
        else
            App.approveVideo(id)
    }

    // A rejected video is deleted: it cannot go on playing. An orphan is
    // not deleted while the music folders are read.
    function rejectVideo(id) {
        const orphan = App.videos.section === "orphans"
        if (orphan && App.scanning)
            return
        if (player.current && player.current.videoId === id)
            player.stop()
        if (orphan)
            App.deleteOrphan(id)
        else
            App.rejectVideo(id)
    }

    // Another section of the grid. The playing video was chosen from the
    // one on show, so it stops, and the grid comes forward.
    function showSection(section) {
        if (section === App.videos.section)
            return
        if (player.current)
            player.stop()
        App.videos.showSection(section)
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
        // A track of the music library whose music video is playing in its
        // place (App.videoFollows): the player layer is up over the albums.
        property bool videoUp: false
        // Gapless: while a track plays from its file, the one that follows
        // is queued in mpv (pendingNext), which goes on to it by itself;
        // onFileStarted then takes the queue there without a load.
        property int pendingNext: -1
        property int loads: 0   // how many times a file was loaded afresh (tests)

        // What next(true) would choose, without choosing it.
        function upcoming() {
            if (queue.length === 0)
                return -1
            if (repeatMode === 2)
                return index
            if (shuffle && queue.length > 1) {
                let n
                do {
                    n = Math.floor(Math.random() * queue.length)
                } while (n === index)
                return n
            }
            const n = index + 1
            if (n < queue.length)
                return n
            return repeatMode === 1 ? 0 : -1
        }
        // A track played from its file, as this one is.
        function fromFile(item) {
            return item && item.kind === "track" && !(App.videoFollows && item.videoId > 0 && item.videoPath)
        }
        function armNext() {
            const n = fromFile(current) && !videoUp ? upcoming() : -1
            if (n >= 0 && fromFile(queue[n])) {
                pendingNext = n
                window.mpv.queueNext(queue[n].path)
            } else {
                pendingNext = -1
                window.mpv.clearNext()
            }
        }
        onShuffleChanged: if (current) armNext()
        onRepeatModeChanged: if (current) armNext()

        function load(i) {
            pendingNext = -1
            loads += 1
            index = i
            current = queue[i]
            if (current.kind === "track") {
                if (App.videoFollows && current.videoId > 0 && current.videoPath) {
                    // Its video, grown over the albums, with the track's
                    // audio where the file has it.
                    videoUp = true
                    mpv.firstAudioTrack = App.firstAudioTrack(current.videoId)
                    playerView.beginLoad(current.videoPath)
                    if (window.view !== "player") {
                        window.releaseTextFocus()
                        window.view = "player"
                        playerView.openFrom(0)
                    }
                    return
                }
                // From its file, with no picture: the player layer stays
                // closed and the grid stays.
                videoUp = false
                if (window.fullscreen)
                    window.setFullscreen(false)
                window.view = "grid"
                playerView.close()
                window.mpv.load(current.path)
                armNext()
                return
            }
            videoUp = false
            // (Under review it starts on the audio of its candidate on show.)
            mpv.firstAudioTrack = App.firstAudioTrack(current.videoId)
            playerView.beginLoad(current.path)
        }

        // Start playing an album of the music library from one of its
        // tracks; the rest of the album follows, in order.
        function playAlbum(row, trackIndex) {
            const list = App.albumTracks(row)
            if (list.length === 0)
                return
            queue = list
            history = []
            load(Math.max(0, Math.min(trackIndex || 0, list.length - 1)))
        }

        // Start playing the grid's `row`; the video grows out of its thumbnail.
        function playRow(row) {
            const list = App.videos.snapshot()
            if (row < 0 || row >= list.length)
                return
            // The video that is already loaded: just bring it forward. A
            // track whose video is up in its place (App.videoFollows) goes
            // on as that video, in the grid's queue, without a reload.
            if (current && list[row].videoId === current.videoId && (current.kind !== "track" || videoUp)) {
                queue = list
                history = []
                index = row
                current = list[row]
                videoUp = false
                window.showPlayer()
                return
            }
            const decodeWidth = grid.thumbDecodeWidth(row)
            queue = list
            history = []
            load(row)
            // A video under review plays in its thumbnail, next to the
            // others and with its verdict buttons; a click on it enlarges it.
            // So do the orphans among themselves, and every video when that
            // is how the user wants it.
            if (list[row].review || App.videos.section === "orphans" || !App.autoExpand) {
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
            videoUp = false
            window.mpv.stop()
            if (window.fullscreen)
                window.setFullscreen(false)
            playerView.close()
            window.view = "grid"
            current = null
            index = -1
        }
    }

    // Switching library: a video grown over the grid gives way; what
    // plays, plays on.
    Connections {
        target: App
        function onModeChanged() {
            if (window.view === "player")
                window.showGrid()
        }
    }

    Connections {
        target: window.mpv
        function onEndReached() {
            if (!player.next(true))
                player.stop()
        }
        // mpv went on to the track queued after the playing one.
        function onFileStarted(path) {
            const n = player.pendingNext
            if (n < 0 || n >= player.queue.length || player.queue[n].path !== path)
                return
            if (n !== player.index) {
                player.history.push(player.index)
                player.index = n
                player.current = player.queue[n]
            }
            player.pendingNext = -1
            player.armNext()
        }
        function onLoadFailed(reason) {
            console.warn("playback failed:", reason)
            if (!player.next(true))
                player.stop()
        }
        function onSeeked() { MediaSession.seeked() }
    }

    Binding { target: window.mpv; property: "volume"; value: App.volume }
    Binding { target: window.mpv; property: "muted"; value: App.muted }
    // Subtitles belong to the full view: over a thumbnail they are specks.
    Binding { target: window.mpv; property: "subtitlesVisible"; value: App.subtitlesOn && (window.view === "player" || window.fullscreen) }
    Binding { target: window.mpv; property: "subtitleLangs"; value: App.subtitleLangs.replace(/ /g, "") }
    Binding { target: window.mpv; property: "subtitleOutline"; value: App.subtitleOutline }
    Binding { target: window.mpv; property: "subtitleShadow"; value: App.subtitleShadow }
    Binding { target: IdleInhibitor; property: "active"; value: window.playingNow }

    // ---- Desktop media controls (media keys, panels) ----------------------
    Binding { target: MediaSession; property: "track"; value: player.current ?? ({}) }
    Binding { target: MediaSession; property: "playing"; value: window.playingNow }
    Binding { target: MediaSession; property: "position"; value: window.mpv.position }
    Binding { target: MediaSession; property: "volume"; value: App.volume }
    Binding { target: MediaSession; property: "shuffle"; value: player.shuffle }
    Binding { target: MediaSession; property: "repeatMode"; value: player.repeatMode }
    Binding { target: MediaSession; property: "canStep"; value: player.queue.length > 1 }
    Binding { target: MediaSession; property: "fullscreen"; value: window.fullscreen }
    Connections {
        target: MediaSession
        function onPlayRequested() { if (window.mpv.paused) window.togglePause() }
        function onPauseRequested() { if (!window.mpv.paused) window.togglePause() }
        function onPlayPauseRequested() { window.togglePause() }
        function onStopRequested() { player.stop() }
        function onNextRequested() { player.next(false) }
        function onPreviousRequested() { player.previous() }
        function onSeekRequested(seconds) { window.mpv.seek(seconds, true) }
        function onVolumeRequested(volume) { App.volume = volume }
        function onShuffleRequested(shuffle) { player.shuffle = shuffle }
        function onRepeatModeRequested(mode) { player.repeatMode = mode }
        function onFullscreenRequested(fullscreen) { window.setFullscreen(fullscreen) }
        function onRaiseRequested() { window.raise(); window.requestActivate() }
    }

    // ---- Accent colour -----------------------------------------------------
    // Either the colour chosen in Settings, or ("auto") one that follows the
    // picture of the playing video, changing slowly.
    readonly property bool autoAccent: App.accent === "auto"
    Binding { target: window.mpv; property: "colorSampling"; value: window.autoAccent }
    // A track played from its file has no picture to sample: its album's
    // cover gives the colour instead.
    readonly property string trackAccent: autoAccent && player.current && player.current.kind === "track" && !player.videoUp
                                          ? App.coverAccent(player.current.thumb) : ""
    Binding {
        target: Theme
        property: "accentSource"
        value: !window.autoAccent ? App.accent
             : window.trackAccent !== "" ? window.trackAccent
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

        // The music file and YouTube page of the video under review, with the
        // arrows between its candidates: a panel in place of the control
        // bar's popup while the review queue is on show.
        ReviewPanel {
            id: reviewPanel
            anchors.left: sidebar.right
            anchors.top: parent.top
            anchors.bottom: controlBar.top
            active: App.videos.section === "review" && !window.fullscreen
            playing: player.current
            onStepRequested: (delta) => controlBar.stepCandidate(delta)
            onReplaceRequested: (videoId) => replaceDialog.openFor(videoId)
            onRevealRequested: (videoId) => window.revealVideo(videoId)
        }

        Item {
            id: mainArea
            anchors.left: reviewPanel.right
            anchors.right: nowPlaying.left
            anchors.top: parent.top
            anchors.bottom: controlBar.top

            TopBar {
                id: topBar
                width: parent.width
                playerView: window.view === "player"
                current: player.current
                onBackRequested: window.showGrid()
                onSectionRequested: (section) => window.showSection(section)
                onAcceptJudgedRequested: window.acceptJudged()
                onSearchTextChanged: {
                    App.videos.searchText = searchText
                    App.albums.searchText = searchText
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
                    visible: opacity > 0 && App.configured && App.mode === "video"
                    searchText: topBar.searchText
                    currentVideoId: player.current ? player.current.videoId : -1
                    currentPlaying: window.playingNow
                    onActivated: (row) => player.playRow(row)
                    onApproved: (id) => window.approveVideo(id)
                    onRejected: (id) => window.rejectVideo(id)
                    onChannelRequested: (id, allow) => channelDialog.openFor(id, allow)
                }

                AlbumGrid {
                    id: albumGrid
                    anchors.fill: parent
                    visible: App.mode === "music" && App.configured
                    searchText: topBar.searchText
                    currentKey: player.current && player.current.kind === "track" ? player.current.albumKey : ""
                    currentPlaying: window.playingNow
                    onActivated: (row) => player.playAlbum(row, 0)
                }

                Welcome {
                    anchors.fill: parent
                    visible: !App.configured
                }
            }
        }

        // In music mode, right of the grid: the queue, the playing track
        // and its cover.
        NowPlayingPanel {
            id: nowPlaying
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: controlBar.top
            active: App.mode === "music" && !window.fullscreen && App.musicPanel
            queue: player.queue
            index: player.index
            current: player.current
            playing: window.playingNow
            onPlayRequested: (i) => player.load(i)
            onVideoRequested: (videoId) => window.playVideo(videoId)
        }

        PlayerView {
            id: playerView
            x: window.fullscreen ? 0 : mainArea.x
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
            onReviewChannelRequested: (allow) => channelDialog.openFor(player.current.videoId, allow)
            candidates: controlBar.candidates.length
            candidateTitle: controlBar.candidate ? controlBar.candidate.title : ""
            onTogglePauseRequested: window.togglePause()
            onToggleFullscreenRequested: window.setFullscreen(!window.fullscreen)
        }

        ControlBar {
            id: controlBar
            onReplaceRequested: (videoId) => replaceDialog.openFor(videoId)
            onRevealRequested: (videoId) => window.revealVideo(videoId)
            width: parent.width
            readonly property bool revealed: !window.fullscreen || playerView.pointerActive || hovered || window.mpv.paused
            y: parent.height - height + (revealed ? 0 : height)
            opacity: revealed ? 1 : 0
            Behavior on y { enabled: window.fullscreen; NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
            Behavior on opacity { enabled: window.fullscreen; NumberAnimation { duration: Theme.fast } }

            mpv: window.mpv
            current: player.current
            playing: window.playingNow
            sourcesInPanel: reviewPanel.active
            overlay: window.fullscreen
            fullscreen: window.fullscreen
            musicMode: App.mode === "music"
            videoUp: player.videoUp
            shuffle: player.shuffle
            repeatMode: player.repeatMode
            canStep: player.queue.length > 1
            onPlayPauseRequested: window.togglePause()
            onNextRequested: player.next(false)
            onPreviousRequested: player.previous()
            onShuffleToggled: player.shuffle = !player.shuffle
            onRepeatCycled: player.repeatMode = (player.repeatMode + 1) % 3
            onFullscreenToggled: window.setFullscreen(!window.fullscreen)
            onNowPlayingClicked: {
                if (player.current && player.current.kind === "track" && !player.videoUp) {
                    // The album of the playing track, in the music library.
                    App.mode = "music"
                    albumGrid.reveal(player.current.albumKey)
                } else {
                    window.view === "player" ? window.showGrid() : window.showPlayer()
                }
            }
        }
    }

    SettingsPanel { id: settings }
    ReplaceDialog { id: replaceDialog }
    ChannelDialog {
        id: channelDialog
        // A video that plays is let go of before its channel's videos change.
        onConfirmed: (ids) => {
            if (player.current && ids.indexOf(player.current.videoId) >= 0)
                player.stop()
        }
    }

    // Frame rate counter (F12)
    Rectangle {
        visible: FrameStats.visible
        x: (window.fullscreen ? 0 : mainArea.x) + 14
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
    Shortcut { sequence: "Ctrl+M"; enabled: !window.typing; onActivated: App.mode = App.mode === "music" ? "video" : "music" }
    Shortcut { sequence: controlBar.audioKey; enabled: !window.typing; onActivated: controlBar.switchAudio() }
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
        case "repeat": if (arg) player.repeatMode = parseInt(arg); return "" + player.repeatMode // repeat [0|1|2]: off, all, one
        case "shuffle": if (arg) player.shuffle = arg !== "off"; return player.shuffle ? "on" : "off" // shuffle [on|off]
        case "audio": mpv.audioTrack = parseInt(arg); return "ok"
        case "switch-audio": controlBar.switchAudio(); return "ok" // as the key on the chip does
        case "search": topBar.searchText = arg; return "ok"
        case "facet": {
            const s = arg.indexOf(" ")
            App.videos.setFacet(s < 0 ? arg : arg.slice(0, s), s < 0 ? "" : arg.slice(s + 1))
            return "ok"
        }
        case "section": showSection(arg); return "ok" // library | review | orphans
        case "sort": App.videos.sortMode = arg; return "ok"
        case "sort-direction": App.videos.sortDescending = arg === "desc"; return "ok" // sort-direction asc|desc
        case "order": return JSON.stringify(App.videos.snapshot().slice(0, parseInt(arg) || 20).map(v => v.title)) // order [n]: the grid's first titles
        case "mode": if (arg) App.mode = arg; return App.mode // mode [video|music]
        case "video-follows": App.videoFollows = arg !== "off"; return "ok" // video-follows on|off
        case "music-panel": if (arg) App.musicPanel = arg !== "off"; return App.musicPanel ? "on" : "off" // music-panel [on|off]: the panel right of the albums
        case "thumb-columns": if (arg) App.thumbColumns = parseInt(arg); return "" + App.thumbColumns // thumb-columns [n]: videos across
        case "cover-columns": if (arg) App.coverColumns = parseInt(arg); return "" + App.coverColumns // cover-columns [n]: albums across
        case "albums": return JSON.stringify(App.albums.snapshot().slice(0, parseInt(arg) || 20).map(a => a.albumArtist + " — " + a.album)) // albums [n]: the album grid's first albums
        case "album-tracks": return JSON.stringify(App.albumTracks(parseInt(arg)).map(t => t.title)) // album-tracks <row>
        case "play-album": { // play-album <row> [track]
            const parts = arg.split(" ")
            player.playAlbum(parseInt(parts[0]), parseInt(parts[1]) || 0)
            return "ok"
        }
        case "queue": return JSON.stringify({ index: player.index, kind: player.current ? (player.current.kind || "video") : null, titles: player.queue.map(q => q.title) })
        case "reveal-album": return "" + albumGrid.reveal(App.albums.get(parseInt(arg)).key) // reveal-album <row>
        case "album-menu": albumGrid.openMenu(parseInt(arg), null); return "ok" // album-menu <row>: the card's right-click menu
        case "lookup-album": { const a = App.albums.get(parseInt(arg)); return "" + App.lookupAlbum(a.albumArtist, a.album) } // lookup-album <row>: how many tracks it brings in
        case "unlookup-album": { const a = App.albums.get(parseInt(arg)); App.removeLookupAlbum(a.albumArtist, a.album); return "ok" }
        case "reimport-album": { const a = App.albums.get(parseInt(arg)); return "" + App.reimportAlbum(a.albumArtist, a.album) } // reimport-album <row>: lookups queued
        case "lookup-albums": return JSON.stringify(App.lookupAlbums.map(a => a.albumArtist + " — " + a.album))
        case "album-sort": App.albums.sortMode = arg; return "ok"
        case "album-facet": {
            const s = arg.indexOf(" ")
            App.albums.setFacet(s < 0 ? arg : arg.slice(0, s), s < 0 ? "" : arg.slice(s + 1))
            return "ok"
        }
        case "fullscreen": setFullscreen(arg !== "off"); return "ok"
        case "sources": controlBar.toggleSources(); return "ok"
        case "imports": { // imports on|off: the import panel; answers how long opening or closing took, in ms
            const t0 = Date.now()
            arg === "off" ? topBar.importPanel.close() : topBar.importPanel.open()
            return "" + (Date.now() - t0)
        }
        case "settings": arg === "off" ? settings.close() : settings.open(); return "ok"
        case "settings-scroll": settings.scrollTo(parseInt(arg)); return "ok" // to this many pixels down
        case "settings-tab": return settings.showTab(arg) ? "ok" : "no such tab" // library | appearance | playback | import | channels | judge
        // The Claude API key for judging: judge-key <key>, judge-key (removes it), judge-check
        case "judge-key": return (arg === "" ? (App.removeJudgeKey(), "") : App.setJudgeKey(arg)) || "ok"
        case "judge-check": App.checkJudgeKey(); return "ok"
        case "review-all": App.askJudgeAll(); return "ok" // advice mode: the judge on every card
        case "accept-all": if (arg === "go") { window.acceptJudged(); return "ok" } return JSON.stringify(App.judgePlan()) // X4: what it would do, or do it
        case "retry": App.retryUnmatched(); return "ok"
        case "rescan": App.rescan(); return "ok"
        case "cookies": return (arg === "" ? (App.removeCookies(), "") : App.importCookies(arg)) || "ok"
        case "check-quality": App.checkQuality(); return "ok"
        case "check-cookies": App.checkCookies(); return "ok"
        case "sign-in-on-bot-check": App.accountOnBotCheck = arg !== "off"; return "ok"
        case "small-channels": // small-channels on|off, or the limit: small-channels 5000
            if (arg === "on" || arg === "off") App.ignoreSmallChannels = arg === "on"
            else App.minSubscribers = parseInt(arg)
            return "ok"
        // Channels: channel-actions <video id> (whether the two buttons are offered), channel-dialog <video id> allow|deny,
        // channel-confirm, channel-cancel, channel-state (the dialog as it stands), channels (the lists), unlist-channel <id>
        case "channel-actions": return JSON.stringify(App.channelActions(parseInt(arg)))
        case "channel-dialog": {
            const parts = arg.split(" ")
            channelDialog.openFor(parseInt(parts[0]), parts[1] !== "deny")
            return channelDialog.change.id !== undefined ? "ok" : "no channel"
        }
        case "channel-confirm": if (!channelDialog.opened) return "closed"; channelDialog.confirm(); return "ok"
        case "channel-cancel": channelDialog.close(); return "ok"
        case "channel-state": return JSON.stringify({ opened: channelDialog.opened, allow: channelDialog.allow, judging: channelDialog.judging,
            channel: channelDialog.change, judged: channelDialog.judged, accepted: channelDialog.accepted })
        case "channels": return JSON.stringify(App.listedChannels)
        case "unlist-channel": App.unlistChannel(arg); return "ok"
        case "resume": App.resumeImport(); return "ok"
        case "fetch-subtitles": App.fetchSubtitles(); return "ok"
        case "delete-untracked": return "" + App.deleteUntracked()
        case "reimport": return "" + App.reimportVideo(parseInt(arg)) // reimport <video id>
        case "reimport-facet": { // reimport-facet <type> <value>: the section on show
            const s = arg.indexOf(" ")
            return "" + App.reimportFacet(s < 0 ? arg : arg.slice(0, s), s < 0 ? "" : arg.slice(s + 1))
        }
        case "reimport-path": return "" + App.reimportPath(arg)
        case "replace-dialog": replaceDialog.openFor(parseInt(arg)); return "ok" // replace-dialog <video id>
        case "replace-search": return App.replaceSearch(parseInt(arg)) ? "ok" : "busy"
        case "replace-check": { // replace-check <video id> <link or id>
            const s = arg.indexOf(" ")
            return App.replaceCheck(parseInt(arg.slice(0, s)), arg.slice(s + 1)) || "ok"
        }
        case "replace": { // replace <video id> <youtube id>
            const s = arg.indexOf(" ")
            return App.replaceWith(parseInt(arg.slice(0, s)), arg.slice(s + 1)) ? "ok" : "busy"
        }
        case "reimport-menu": { // reimport-menu <value>: the sidebar's row menu for a listed tag value ("" for All Videos)
            const f = App.facets.find(x => x.key === App.sidebarFacet)
            const it = f ? (f.items[App.videos.section] || []).find(i => i.name === arg) : null
            sidebar.askReimport(null, arg === "" ? "all" : App.sidebarFacet, arg, arg === "" ? App.videoCount : (it ? it.count : 0))
            return "ok"
        }
        case "auto-expand": App.autoExpand = arg !== "off"; return "ok"
        case "subtitle-style": { // subtitle-style <outline> <shadow>
            const st = arg.split(" ")
            App.subtitleOutline = parseFloat(st[0]); App.subtitleShadow = parseFloat(st[1] || "0")
            return "ok"
        }
        case "subtitles": // subtitles on|off, or the languages to fetch: subtitles en,ja
            if (arg === "on" || arg === "off") App.subtitlesOn = arg === "on"
            else App.subtitleLangs = arg
            return "ok"
        case "candidate": controlBar.stepCandidate(parseInt(arg || "1")); return "ok" // the next (1) or previous (-1) candidate track of the playing video
        case "candidates": return JSON.stringify(App.candidates(parseInt(arg)))
        case "elsewhere": return "" + controlBar.showElsewhere() // as the button in the panel does: the video it went to
        // The review panel's own arrows and "+N" button, for the video it is about
        case "panel-candidate": reviewPanel.stepCandidate(parseInt(arg || "1")); return "ok"
        case "panel-elsewhere": return "" + reviewPanel.showElsewhere()
        case "reveal": return "" + window.revealVideo(parseInt(arg))
        case "approve": window.approveVideo(parseInt(arg)); return "ok"
        case "reject": window.rejectVideo(parseInt(arg)); return "ok"
        case "undo": if (!App.undoAvailable) return "nothing to undo"; App.undoVerdict(); return "ok" // the last verdict taken back
        case "grab": // grab <file>: a picture of the settings panel when open, else of the window (grab-window: with popups)
            (settings.opened ? settings.contentItem : frame).grabToImage(r => r.saveToFile(arg))
            return "ok"
        case "ytdlp-update": YtDlpUpdater.update(); return "ok"
        case "ytdlp": return JSON.stringify({ version: YtDlpUpdater.version, busy: YtDlpUpdater.busy, status: YtDlpUpdater.status })
        case "fps": FrameStats.visible = arg !== "off"; return "ok"
        case "accent": App.accent = arg; return "ok"
        case "theme": App.themeMode = arg; return "ok"
        case "add-music": App.addMusicDir(arg); return "ok" // ticks a folder (widening the base if it lies outside)
        case "remove-music": App.removeMusicDir(arg); return "ok" // unticks one
        case "base-dir": App.baseDir = arg; return "ok" // the base directory the folder tree shows
        case "check-folder": App.setFolderChecked(arg, true); return "ok"
        case "uncheck-folder": App.setFolderChecked(arg, false); return "ok"
        case "folder-expand": { // folder-expand <path>: opens or closes a folder of the tree
            const row = App.folderTree.rowOf(arg)
            if (row < 0) return "not shown"
            App.folderTree.toggleExpanded(row); return "ok"
        }
        case "folder-tree": { // the rows on show: [{path, depth, expanded, hasChildren, checkState}]
            const rows = []
            const m = App.folderTree
            for (let i = 0; i < m.count; ++i) {
                const ix = m.index(i, 0)
                rows.push({ path: m.data(ix, Qt.UserRole + 1), depth: m.data(ix, Qt.UserRole + 3), expanded: m.data(ix, Qt.UserRole + 4),
                            hasChildren: m.data(ix, Qt.UserRole + 5), checkState: m.data(ix, Qt.UserRole + 6) })
            }
            return JSON.stringify(rows)
        }
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
                mode: App.mode, albumCount: App.albumCount, albumsShown: App.albums.count, libraryTracks: App.libraryTrackCount,
                albumSort: App.albums.sortMode, albumDescending: App.albums.sortDescending, albumFacet: App.albums.facetType,
                queueLength: player.queue.length, queueKind: player.current ? (player.current.kind || "video") : null,
                pendingNext: player.pendingNext, loads: player.loads, trackAccent: window.trackAccent,
                musicPanel: nowPlaying.active, musicPanelWidth: nowPlaying.width, musicPanelOn: App.musicPanel,
                lookupAlbums: App.lookupAlbums.length, albumMenu: albumGrid.menuOpen,
                videoFollows: App.videoFollows, videoUp: player.videoUp,
                thumbColumns: App.thumbColumns, coverColumns: App.coverColumns, gridColumns: grid.view.columns, albumColumns: albumGrid.view.columns,
                playing: player.current ? player.current.title : null, index: player.index,
                position: mpv.position, duration: mpv.duration, paused: mpv.paused, active: mpv.active, muted: mpv.muted,
                hwdec: mpv.hwdec, videoSize: mpv.videoSize.width + "x" + mpv.videoSize.height,
                audioTrack: mpv.audioTrack, audioTracks: mpv.audioTracks.length, libraryHere: controlBar.libraryHere, librarySpans: controlBar.libSpans.length,
                busy: App.busy, scanning: App.scanning, status: App.statusText, counts: App.trackCounts,
                untracked: App.untrackedCount, autoExpand: App.autoExpand, subtitleOutline: App.subtitleOutline, subtitleShadow: App.subtitleShadow,
                subtitleLangs: App.subtitleLangs, subtitlesOn: App.subtitlesOn, hasSubtitles: window.mpv.hasSubtitles,
                fetchingSubtitles: App.fetchingSubtitles, cookies: App.hasCookies, checkingCookies: App.checkingCookies, cookiesState: App.cookiesState,
                cookiesStatus: App.cookiesStatus, checkingQuality: App.checkingQuality, reviewCount: App.reviewCount,
                paused: App.importPaused, pauseKind: App.pauseKind, accountOnBotCheck: App.accountOnBotCheck,
                ignoreSmallChannels: App.ignoreSmallChannels, minSubscribers: App.minSubscribers,
                channelDialog: channelDialog.opened,
                facet: App.videos.facetType, facetValue: App.videos.facetValue, section: App.videos.section,
                sortMode: App.videos.sortMode, sortDescending: App.videos.sortDescending,
                orphanCount: App.orphanCount, reviewLabel: controlBar.reviewLabel, trackLength: controlBar.trackLength,
                candidates: controlBar.candidates.length, candidate: controlBar.shown,
                candidateTitle: controlBar.candidate ? controlBar.candidate.title : null,
                elsewhere: controlBar.candidate ? controlBar.candidate.elsewhere.length : 0, nowTitle: controlBar.nowTitle,
                panel: reviewPanel.active, panelWidth: reviewPanel.width, panelVideo: reviewPanel.videoId, panelPlaying: reviewPanel.isPlaying,
                panelCandidates: reviewPanel.candidates.length, panelCandidate: reviewPanel.shown, panelTitle: reviewPanel.title,
                panelFile: reviewPanel.file ? reviewPanel.file.path : null, panelYt: reviewPanel.ytTitle,
                undo: App.undoAvailable, undoLabel: App.undoLabel,
                settingsOpen: settings.opened, settingsTab: settings.tab,
                judgeKey: App.judgeKeySet, judgeKeyHint: App.judgeKeyHint, judgeChecking: App.judgeChecking, judgeState: App.judgeState, judgeStatus: App.judgeStatus, adviceLeft: App.adviceLeft, judgeThreads: App.judgeThreads, keepEverything: App.keepEverything,
                replacing: App.replacing, replaceStage: App.replaceStage, replaceDialog: replaceDialog.opened, replaceOptions: replaceDialog.options.length, replaceMode: replaceDialog.mode,
                accent: "" + Theme.accent, accentMode: App.accent, theme: App.themeMode, dark: Theme.dark,
                systemDark: App.systemDark, bg: "" + Theme.bg, baseDir: App.baseDir, musicDirs: App.musicDirs, frameColor: "" + mpv.frameColor,
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
