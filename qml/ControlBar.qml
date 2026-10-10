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
    // The key that changes between the library's audio and YouTube's, as the
    // chip shows it; the window binds it.
    readonly property string audioKey: "A"
    function switchAudio() {
        if (audioSwitch.available)
            mpv.audioTrack = audioSwitch.library ? 2 : 1
    }
    // How long the track of a video under review is: the seek bar has the
    // video's length, and a video much shorter than its track is a cut.
    readonly property string trackLength: current !== null && current.review === true ? App.trackLength(current.videoId) : ""

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
                width: parent.width
                text: root.current ? root.current.title : ""
                color: textMouse.containsMouse || sources.visible ? Theme.accentHi : Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Behavior on color { ColorAnimation { duration: Theme.fast } }
            }
            Row {
                width: parent.width
                Text {
                    width: Math.min(implicitWidth, parent.width - (trackLengthLabel.visible ? trackLengthLabel.implicitWidth : 0))
                    text: root.current ? root.current.artist.replace(/; /g, ", ") : ""
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
            readonly property bool available: root.hasMedia && root.mpv.audioTracks.length > 1
            readonly property bool reviewing: available && root.current !== null && root.current.review === true
            readonly property bool library: root.mpv.audioTrack <= 1
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
                        readonly property string detail: root.current && root.current.audioDetail ? root.current.audioDetail : ""
                        text: !audioSwitch.library ? "YouTube audio"
                            : audioSwitch.reviewing ? "Library audio" + (detail ? " (" + detail + ")" : "")
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
                text: audioSwitch.library ? "Your library's audio · click or press " + root.audioKey + " for YouTube's"
                                          : "YouTube's audio · click or press " + root.audioKey + " for your library's"
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
