import QtQuick
import MvPlayer.Core

// Transport bar shared by every view: now playing, transport, seek, volume.
Rectangle {
    id: root

    required property MpvItem mpv
    property var current: null        // video map, or null when stopped
    property bool playing: false
    property bool overlay: false      // floating over fullscreen video
    property bool shuffle: false
    property int repeatMode: 0        // 0 off, 1 all, 2 one
    property bool fullscreen: false
    property bool canStep: false
    // Held while its popup is open, so that it stays in fullscreen.
    readonly property bool hovered: hover.hovered || seek.dragging || sources.visible

    signal replaceRequested(var videoId)
    signal revealRequested(var videoId) // bring that video's card into view

    signal playPauseRequested()
    signal nextRequested()
    signal previousRequested()
    signal shuffleToggled()
    signal repeatCycled()
    signal fullscreenToggled()
    signal nowPlayingClicked()  // the thumbnail; the title and artist open the popup below

    implicitHeight: 96
    color: overlay ? Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, 0.92) : Theme.surface

    readonly property bool hasMedia: current !== null

    // The playing video's music file and YouTube page, in a popup.
    function toggleSources() {
        if (sources.visible)
            sources.close()
        else if (hasMedia)
            sources.open()
    }
    // What a video under review is playing right now; empty otherwise.
    readonly property string reviewLabel: audioSwitch.reviewing ? chipLabel.text : ""

    // A video under review has candidate tracks: the tracks whose video it
    // could be. The panel shows one of them at a time, and the verdict is
    // for that one. The arrows step through them and the audio goes with
    // them: the candidate on show is the one whose audio plays.
    property var candidates: []
    property int shown: 0
    readonly property bool reviewing: current !== null && current.review === true
    readonly property var candidate: candidates.length > 0 ? candidates[Math.min(shown, candidates.length - 1)] : null
    function refreshCandidates() {
        // (Asked of `current` itself: this runs as it changes, ahead of the bindings on it.)
        const list = current !== null && current.review === true ? App.candidates(current.videoId) : []
        shown = list.length > 0 ? Math.max(0, Math.min(App.shownCandidate(current.videoId), list.length - 1)) : 0
        candidates = list
    }
    onCurrentChanged: { elsewhereAt = 0; refreshCandidates() }
    Connections {
        target: App
        function onCandidatesChanged() { root.refreshCandidates() }
    }
    function stepCandidate(delta) {
        const n = candidates.length
        if (n < 2)
            return
        const to = ((shown + delta) % n + n) % n
        const c = candidates[to]
        elsewhereAt = 0
        App.showCandidate(current.videoId, to)
        // (A candidate without audio of its own in the file would keep YouTube's.)
        mpv.audioTrack = c.stream > 0 ? c.stream : current.ytStream + 1
    }
    // The candidate on show is a candidate of other videos too: goes to the
    // next of those, and says which.
    property int elsewhereAt: 0
    function showElsewhere() {
        if (candidate === null || candidate.elsewhere.length === 0)
            return -1
        const id = candidate.elsewhere[elsewhereAt % candidate.elsewhere.length]
        elsewhereAt = (elsewhereAt + 1) % candidate.elsewhere.length
        revealRequested(id)
        return id
    }
    // What the panel says is playing: the candidate on show, under review.
    readonly property string nowTitle: candidate !== null ? candidate.title : current !== null ? current.title : ""
    readonly property string nowArtist: (candidate !== null ? candidate.artist : current !== null ? current.artist : "").replace(/; /g, ", ")

    // The audio the playing video has, in the order the key goes through it:
    // the library's (each candidate's that has one in the file, under
    // review), then YouTube's. `candidate`: whose it is, from 0; -1 for none.
    readonly property var audios: {
        const list = []
        if (!hasMedia)
            return list
        if (reviewing && candidates.length > 0) {
            for (let i = 0; i < candidates.length; ++i) {
                if (candidates[i].stream > 0)
                    list.push({ track: candidates[i].stream, candidate: i, library: true })
            }
            list.push({ track: current.ytStream + 1, candidate: -1, library: false })
        } else {
            for (let i = 0; i < Math.min(2, mpv.audioTracks.length); ++i)
                list.push({ track: i + 1, candidate: -1, library: i === 0 && mpv.audioTracks.length > 1 })
        }
        return list
    }
    readonly property int audioAt: {
        for (let i = 0; i < audios.length; ++i) {
            if (audios[i].track === mpv.audioTrack)
                return i
        }
        return -1
    }
    // The key that goes on to the next of them, as the chip shows it; the
    // window binds it.
    readonly property string audioKey: "A"
    function switchAudio() {
        if (!audioSwitch.available)
            return
        const next = audios[(audioAt + 1) % audios.length]
        mpv.audioTrack = next.track
        if (next.candidate >= 0 && next.candidate !== shown)
            App.showCandidate(current.videoId, next.candidate)
    }
    // How long the track of a video under review is: the seek bar has the
    // video's length, and a video much shorter than its track is a cut.
    readonly property string trackLength: candidate !== null ? candidate.length : ""

    HoverHandler { id: hover }
    // Swallow clicks so they do not reach the video underneath in fullscreen.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.line
        visible: !root.overlay
    }

    // ---- Now playing ------------------------------------------------------
    Item {
        id: nowPlaying
        anchors.left: parent.left
        anchors.leftMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(180, Math.min(340, (root.width - center.width) / 2 - 40))
        height: 52
        opacity: root.hasMedia ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }

        RoundedImage {
            id: art
            anchors.verticalCenter: parent.verticalCenter
            width: 80
            height: 45
            radius: 6
            source: root.current ? root.current.thumb : ""
            dim: artMouse.containsMouse ? 0.3 : 0
            Behavior on dim { NumberAnimation { duration: Theme.fast } }
            MouseArea {
                id: artMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.nowPlayingClicked()
            }
        }
        Column {
            id: trackText
            anchors.left: art.right
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            Text {
                // (Room on the right for the arrows between candidates.)
                width: parent.width - (candidateSteps.visible ? candidateSteps.width + 6 : 0)
                text: root.nowTitle
                color: textMouse.containsMouse || sources.visible ? Theme.accentHi : Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Behavior on color { ColorAnimation { duration: Theme.fast } }
            }
            Row {
                width: parent.width - (elsewhere.visible ? elsewhere.width + 6 : 0)
                Text {
                    width: Math.min(implicitWidth, parent.width - (trackLengthLabel.visible ? trackLengthLabel.implicitWidth : 0))
                    text: root.nowArtist
                    color: Theme.textDim
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Text {
                    id: trackLengthLabel
                    visible: root.trackLength.length > 0
                    text: "  ·  Track " + root.trackLength
                    color: Theme.textDim
                    font.pixelSize: 12
                    font.features: { "tnum": 1 }
                }
            }
        }
        MouseArea {
            id: textMouse
            anchors.fill: trackText
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.toggleSources()
        }

        // Several tracks could have the video under review: the arrows show
        // the next one and the one before, with its audio.
        Row {
            id: candidateSteps
            visible: root.candidates.length > 1
            anchors.right: parent.right
            y: trackText.y - 3
            spacing: 0
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                size: 22
                iconSize: 16
                icon: Icons.back
                tooltip: "Previous candidate track"
                onClicked: root.stepCandidate(-1)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: (root.shown + 1) + "/" + root.candidates.length
                color: Theme.textDim
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.features: { "tnum": 1 }
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                size: 22
                iconSize: 16
                icon: Icons.back
                iconRotation: 180
                tooltip: "Next candidate track"
                onClicked: root.stepCandidate(1)
            }
        }
        // The candidate on show is a candidate of other videos as well: says
        // so, and a click brings the next of them into view.
        Rectangle {
            id: elsewhere
            readonly property int count: root.candidate !== null ? root.candidate.elsewhere.length : 0
            visible: count > 0
            anchors.right: parent.right
            y: trackText.y + trackText.height - height + 2
            width: elsewhereRow.implicitWidth + 12
            height: 18
            radius: 9
            color: elsewhereMouse.containsMouse ? Theme.accent : Theme.accentSoft
            Behavior on color { ColorAnimation { duration: Theme.fast } }
            Row {
                id: elsewhereRow
                anchors.centerIn: parent
                spacing: 4
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    path: Icons.movie
                    size: 11
                    color: elsewhereMouse.containsMouse ? Theme.accentInk : Theme.accentHi
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "+" + elsewhere.count
                    color: elsewhereMouse.containsMouse ? Theme.accentInk : Theme.accentHi
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                }
            }
            MouseArea {
                id: elsewhereMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.showElsewhere()
            }
            Tooltip {
                text: elsewhere.count === 1 ? "This track is a candidate of another video too · click to go to it"
                                            : "This track is a candidate of " + elsewhere.count + " other videos too · click to go to the next"
                shown: elsewhereMouse.containsMouse
            }
        }

        // The music file and the YouTube page behind the video
        SourcePopup {
            id: sources
            y: -height - 30
            videoId: root.current ? root.current.videoId : -1
            onReplaceRequested: (videoId) => root.replaceRequested(videoId)
        }
    }

    // ---- Transport + seek -------------------------------------------------
    Item {
        id: center
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        // As far below the top of the bar as the seek bar is below the buttons.
        anchors.verticalCenterOffset: 4
        width: Math.max(320, Math.min(720, root.width - 2 * 300))
        height: 68

        Row {
            id: transport
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            spacing: 10

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: Icons.shuffle
                iconSize: 18
                checked: root.shuffle
                tooltip: root.shuffle ? "Shuffle on" : "Shuffle"
                onClicked: root.shuffleToggled()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: Icons.previous
                iconSize: 24
                enabled: root.hasMedia
                tooltip: "Previous (P)"
                onClicked: root.previousRequested()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                size: 40
                iconSize: 24
                filled: true
                enabled: root.hasMedia
                icon: root.playing ? Icons.pause : Icons.play
                tooltip: root.playing ? "Pause (Space)" : "Play (Space)"
                onClicked: root.playPauseRequested()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: Icons.next
                iconSize: 24
                enabled: root.hasMedia && root.canStep
                tooltip: "Next (N)"
                onClicked: root.nextRequested()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: root.repeatMode === 2 ? Icons.repeatOne : Icons.repeat
                iconSize: 18
                checked: root.repeatMode > 0
                tooltip: root.repeatMode === 0 ? "Repeat" : root.repeatMode === 1 ? "Repeat all" : "Repeat one"
                onClicked: root.repeatCycled()
            }
        }

        Text {
            id: elapsed
            anchors.left: parent.left
            anchors.verticalCenter: seek.verticalCenter
            width: 44
            horizontalAlignment: Text.AlignRight
            text: Theme.formatTime(seek.dragging ? seek.dragFraction * root.mpv.duration : root.mpv.position)
            color: Theme.textDim
            font.pixelSize: 11
            font.features: { "tnum": 1 }
        }
        SeekBar {
            id: seek
            anchors.left: elapsed.right
            anchors.leftMargin: 12
            anchors.right: total.left
            anchors.rightMargin: 12
            anchors.bottom: parent.bottom
            position: root.mpv.position
            duration: root.hasMedia ? root.mpv.duration : 0
            onSeekRequested: (seconds, exact) => root.mpv.seek(seconds, exact)
        }
        Text {
            id: total
            anchors.right: parent.right
            anchors.verticalCenter: seek.verticalCenter
            width: 44
            text: Theme.formatTime(root.hasMedia ? root.mpv.duration : 0)
            color: Theme.textDim
            font.pixelSize: 11
            font.features: { "tnum": 1 }
        }
    }

    // ---- Audio track, volume, fullscreen ----------------------------------
    Row {
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        // Videos with library audio muxed in keep YouTube's track as well:
        // the chip says which of the two plays, and a click or the key on
        // it changes over. That is how a video under review is judged: the
        // track's audio against the upload's own, at the same place.
        Item {
            id: audioSwitch
            anchors.verticalCenter: parent.verticalCenter
            readonly property bool available: root.hasMedia && root.audios.length > 1
            readonly property bool reviewing: available && root.reviewing
            readonly property var playing: root.audioAt >= 0 ? root.audios[root.audioAt] : null
            readonly property bool library: playing !== null ? playing.library : root.mpv.audioTrack <= 1
            // Under review with several candidates' audio in the file:
            // which of them plays, "1/2".
            readonly property int libraries: root.audios.length - 1
            readonly property string which: reviewing && libraries > 1 && library ? " " + (root.audioAt + 1) + "/" + libraries : ""
            readonly property color ink: library ? Theme.accentHi : Theme.textDim
            width: available ? chipRow.implicitWidth + 20 : 0
            height: 26
            opacity: available ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.normal } }
            Behavior on width { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }

            Item {
                anchors.fill: parent
                clip: true
                Rectangle {
                    anchors.fill: parent
                    radius: 13
                    color: audioSwitch.library ? Theme.accentSoft : Theme.hover
                    border.width: 1
                    border.color: chipMouse.containsMouse ? Theme.accent : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.normal } }
                    Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                }
                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: 7
                    Text {
                        id: chipLabel
                        anchors.verticalCenter: parent.verticalCenter
                        readonly property string detail: {
                            const p = audioSwitch.playing
                            if (p !== null && p.candidate >= 0 && root.candidates[p.candidate].audioDetail)
                                return root.candidates[p.candidate].audioDetail
                            return root.current && root.current.audioDetail ? root.current.audioDetail : ""
                        }
                        text: !audioSwitch.library ? "YouTube audio"
                            : audioSwitch.reviewing ? "Library audio" + audioSwitch.which + (detail ? " (" + detail + ")" : "")
                            : detail ? detail : "Library audio"
                        color: audioSwitch.ink
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.2
                    }
                    // The key that changes over
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(16, keyLabel.implicitWidth + 8)
                        height: 16
                        radius: 4
                        color: "transparent"
                        border.width: 1
                        border.color: Qt.rgba(audioSwitch.ink.r, audioSwitch.ink.g, audioSwitch.ink.b, 0.55)
                        Text {
                            id: keyLabel
                            anchors.centerIn: parent
                            text: root.audioKey
                            color: audioSwitch.ink
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
            MouseArea {
                id: chipMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.switchAudio()
            }
            Tooltip {
                text: {
                    const next = root.audios.length > 0 ? root.audios[(root.audioAt + 1) % root.audios.length] : null
                    const then = next === null ? "" : !next.library ? "YouTube's"
                        : next.candidate >= 0 && audioSwitch.libraries > 1 ? "that of “" + root.candidates[next.candidate].title + "”"
                        : "your library's"
                    const now = !audioSwitch.library ? "YouTube's audio"
                        : audioSwitch.playing !== null && audioSwitch.playing.candidate >= 0 && audioSwitch.libraries > 1
                            ? "The audio of “" + root.candidates[audioSwitch.playing.candidate].title + "” from your library"
                        : "Your library's audio"
                    return now + " · click or press " + root.audioKey + " for " + then
                }
                shown: chipMouse.containsMouse
            }
        }

        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.hasMedia && root.mpv.hasSubtitles
            icon: Icons.subtitles
            iconSize: 20
            checked: App.subtitlesOn
            tooltip: App.subtitlesOn ? "Subtitles on" : "Subtitles off"
            onClicked: App.subtitlesOn = !App.subtitlesOn
        }

        Item { width: 4; height: 1 }

        VolumeControl {
            anchors.verticalCenter: parent.verticalCenter
            volume: App.volume
            muted: App.muted
            onVolumeRequested: (v) => App.volume = v
            onMuteRequested: (m) => App.muted = m
        }

        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            icon: root.fullscreen ? Icons.fullscreenExit : Icons.fullscreen
            iconSize: 22
            enabled: root.hasMedia
            tooltip: root.fullscreen ? "Exit fullscreen (F)" : "Fullscreen (F)"
            onClicked: root.fullscreenToggled()
        }
    }
}
