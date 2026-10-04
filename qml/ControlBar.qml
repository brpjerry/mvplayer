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
    readonly property bool hovered: hover.hovered || seek.dragging

    signal playPauseRequested()
    signal nextRequested()
    signal previousRequested()
    signal shuffleToggled()
    signal repeatCycled()
    signal fullscreenToggled()
    signal nowPlayingClicked()

    implicitHeight: 88
    color: overlay ? Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, 0.92) : Theme.surface

    readonly property bool hasMedia: current !== null
    // What a video under review is playing right now; empty otherwise.
    readonly property string reviewLabel: audioSwitch.reviewing ? chipLabel.text : ""

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
            dim: npMouse.containsMouse ? 0.3 : 0
            Behavior on dim { NumberAnimation { duration: Theme.fast } }
        }
        Column {
            anchors.left: art.right
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            Text {
                width: parent.width
                text: root.current ? root.current.title : ""
                color: npMouse.containsMouse ? Theme.accentHi : Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Behavior on color { ColorAnimation { duration: Theme.fast } }
            }
            Text {
                width: parent.width
                text: root.current ? root.current.artist.replace(/; /g, ", ") : ""
                color: Theme.textDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
        MouseArea {
            id: npMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.nowPlayingClicked()
        }
    }

    // ---- Transport + seek -------------------------------------------------
    Item {
        id: center
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
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

        // Videos with library audio muxed in keep YouTube's track as well.
        Item {
            id: audioSwitch
            anchors.verticalCenter: parent.verticalCenter
            readonly property bool available: root.hasMedia && root.mpv.audioTracks.length > 1
            // A video under review plays a third stream that changes source
            // every ten seconds: YouTube's audio first, then the library's,
            // wherever the track reaches.
            readonly property bool reviewing: root.mpv.audioTracks.length > 2 && root.mpv.audioTrack === 3
                                              && root.current !== null && root.current.review === true
            readonly property bool reviewLibrary: reviewing && Math.floor(root.mpv.position / 10) % 2 === 1
                                                  && root.mpv.position >= root.current.reviewStart
                                                  && root.mpv.position < root.current.reviewEnd
            readonly property bool library: reviewing ? reviewLibrary : root.mpv.audioTrack <= 1
            width: available ? chipLabel.implicitWidth + 22 : 0
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
                Text {
                    id: chipLabel
                    anchors.centerIn: parent
                    text: audioSwitch.reviewing ? (audioSwitch.reviewLibrary ? "Library audio (FLAC)" : "YouTube audio")
                        : audioSwitch.library ? (root.current && root.current.audioDetail ? root.current.audioDetail : "Library audio")
                                              : "YouTube audio"
                    color: audioSwitch.library ? Theme.accentHi : Theme.textDim
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.2
                }
            }
            MouseArea {
                id: chipMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (root.mpv.audioTracks.length > 2)
                        root.mpv.audioTrack = root.mpv.audioTrack % 3 + 1 // review stream, library, YouTube
                    else
                        root.mpv.audioTrack = audioSwitch.library ? 2 : 1
                }
            }
            Tooltip {
                text: audioSwitch.reviewing ? "Alternating every 10 s between YouTube's audio and your library's · click for one of them"
                    : audioSwitch.library ? "Your library's audio · click for YouTube's" : "YouTube's audio · click for your library's"
                shown: chipMouse.containsMouse
            }
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
