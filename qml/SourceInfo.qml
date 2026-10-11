import QtQuick
import MvPlayer.Core

// What a video stands for: the music file it was found for, with every tag
// in that file, and the video's page on YouTube. The body of the popup in the
// now-playing corner of the control bar, and of the review panel.
Item {
    id: root

    property var videoId: -1
    // Kept up to date while active: shown, or about to be.
    property bool active: false
    property var info: ({})
    property int fileIndex: 0
    readonly property var files: info.files || []
    readonly property var file: files.length > 0 ? files[Math.min(fileIndex, files.length - 1)] : null
    // The height all of it wants, before the tags have to scroll.
    readonly property real contentHeight: header.height + tags.height + footer.height

    // Queued for a lookup by today's rules, from the button below.
    property int queued: 0
    // The user wants another video in this one's place.
    signal replaceRequested(var videoId)

    function refresh() {
        info = videoId >= 0 ? App.videoSources(videoId) : ({})
        fileIndex = 0
        queued = 0
        flick.contentY = 0
    }
    onActiveChanged: if (active) refresh()
    onVideoIdChanged: if (active) refresh()
    // Under review it shows the music file of the candidate track on show:
    // another one comes on show while it is up.
    Connections {
        target: App
        enabled: root.active
        function onCandidatesChanged() {
            root.info = root.videoId >= 0 ? App.videoSources(root.videoId) : ({})
            root.fileIndex = 0
        }
    }
    onFileIndexChanged: flick.contentY = 0

    // Only the tags scroll. Icons stay out of the scrolling part: a vector
    // icon scrolled out of a clipped view inside a popup can still be drawn,
    // outside the popup.

    // Which file, where it is and what is in it
    Column {
        id: header
        width: parent.width
        topPadding: 14
        bottomPadding: 12

        // With several files of the recording, steps between them.
        Item {
            width: parent.width
            height: 34
            Text {
                id: fileHeading
                x: 18
                anchors.verticalCenter: parent.verticalCenter
                text: "Music file"
                color: Theme.text
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            // How long the track is: against the video's length, below,
            // it shows a video that is only a cut of the song.
            Text {
                anchors.left: fileHeading.right
                anchors.leftMargin: 10
                anchors.baseline: fileHeading.baseline
                text: root.file ? root.file.length : ""
                color: Theme.text
                font.pixelSize: 13
                font.features: { "tnum": 1 }
            }
            Row {
                visible: root.files.length > 1
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                IconButton {
                    size: 28
                    iconSize: 16
                    icon: Icons.back
                    enabled: root.fileIndex > 0
                    onClicked: root.fileIndex -= 1
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: (root.fileIndex + 1) + " of " + root.files.length
                    color: Theme.textDim
                    font.pixelSize: 12
                    font.features: { "tnum": 1 }
                }
                IconButton {
                    size: 28
                    iconSize: 16
                    icon: Icons.back
                    iconRotation: 180
                    enabled: root.fileIndex < root.files.length - 1
                    onClicked: root.fileIndex += 1
                }
            }
        }

        Text {
            visible: root.file === null
            x: 18
            width: parent.width - 36
            wrapMode: Text.WordWrap
            text: "No music file in your library has this video."
            color: Theme.textDim
            font.pixelSize: 12
        }

        Column {
            visible: root.file !== null
            x: 18
            width: parent.width - 36
            spacing: 3
            Text {
                width: parent.width
                text: root.file ? root.file.path : ""
                color: Theme.textDim
                font.pixelSize: 12
                elide: Text.ElideMiddle
            }
            Row {
                visible: root.file !== null && root.file.absent
                spacing: 5
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    path: Icons.musicOff
                    size: 13
                    color: Theme.warn
                }
                Text {
                    text: "No longer in your music folders"
                    color: Theme.warn
                    font.pixelSize: 12
                }
            }
            Text {
                width: parent.width
                text: root.file ? root.file.format : ""
                color: Theme.textFaint
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
    }

    // Every tag in the file, by the name the file gives it
    Flickable {
        id: flick
        anchors.top: header.bottom
        anchors.bottom: footer.top
        width: parent.width
        clip: true
        contentHeight: tags.height
        boundsBehavior: Flickable.StopAtBounds

        KineticWheel {
            view: flick
            touchpadGain: App.touchpadGain
            wheelStep: App.wheelStep * 0.6
            deceleration: App.flickDeceleration
        }

        Column {
            id: tags
            x: 18
            width: flick.width - 36
            bottomPadding: 14
            spacing: 7
            Repeater {
                model: root.file ? root.file.tags : []
                Row {
                    id: tag
                    required property var modelData
                    width: parent.width
                    spacing: 10
                    Text {
                        id: keyLabel
                        // (Narrower where the whole is: the value gets the room.)
                        width: Math.min(116, Math.round(parent.width * 0.34))
                        topPadding: 2
                        text: tag.modelData.key
                        color: Theme.textFaint
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.4
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width - keyLabel.width - parent.spacing
                        text: tag.modelData.value
                        color: Theme.text
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    // The video's page on YouTube, in view however long the tags are
    Column {
        id: footer
        anchors.bottom: parent.bottom
        width: parent.width
        bottomPadding: 16

        Rectangle {
            x: 18
            width: parent.width - 36
            height: 1
            color: Theme.line
        }
        Item { width: 1; height: 12 }

        Column {
            x: 18
            width: parent.width - 36
            spacing: 4
            Item {
                width: parent.width
                height: 34
                Text {
                    id: ytHeading
                    anchors.verticalCenter: parent.verticalCenter
                    text: "YouTube"
                    color: Theme.text
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.left: ytHeading.right
                    anchors.leftMargin: 10
                    anchors.baseline: ytHeading.baseline
                    text: root.info.videoLength || ""
                    color: Theme.text
                    font.pixelSize: 13
                    font.features: { "tnum": 1 }
                }
                // Beside the heading where there is room (the popup); in
                // the panel, on a row of their own below.
                Actions {
                    visible: root.wide
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Text {
                width: parent.width
                text: root.info.ytTitle || ""
                color: Theme.text
                font.pixelSize: 12
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            // Whose upload it is, and when it was published.
            Text {
                width: parent.width
                visible: text.length > 0
                text: [root.info.ytChannel, root.info.ytDate].filter(s => s).join("  ·  ")
                color: Theme.textDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            Item {
                width: parent.width
                height: link.implicitHeight + 6
                Row {
                    id: link
                    y: 6
                    spacing: 5
                    Text {
                        id: linkText
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.parent.width - 18)
                        text: (root.info.ytUrl || "").replace(/^https:\/\/www\./, "")
                        color: linkMouse.containsMouse ? Theme.accentHi : Theme.accent
                        font.pixelSize: 12
                        font.underline: linkMouse.containsMouse
                        elide: Text.ElideRight
                    }
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        path: Icons.openInNew
                        size: 13
                        color: linkText.color
                    }
                }
                MouseArea {
                    id: linkMouse
                    x: link.x
                    y: link.y
                    width: link.width
                    height: link.height
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Qt.openUrlExternally(root.info.ytUrl)
                }
            }
            Item { visible: !root.wide; width: 1; height: 8 }
            Actions { visible: !root.wide }
        }
    }

    // Wide enough for the buttons to share the YouTube heading's row.
    readonly property bool wide: width >= 400

    // What can be done about the video: looked up again by today's rules
    // (kept, sent to review or replaced as they decide), or another upload
    // put in its place, chosen by hand.
    component Actions: Row {
        spacing: 8
        readonly property bool hasTrack: root.files.length > 0 && !root.files.every(f => f.absent)
        FlatButton {
            icon: Icons.refresh
            text: root.queued > 0 ? "Queued" : "Re-import"
            enabled: root.queued === 0 && parent.hasTrack
            onClicked: root.queued = Math.max(1, App.reimportVideo(root.videoId))
        }
        FlatButton {
            icon: Icons.swap
            text: "Replace…"
            enabled: parent.hasTrack && !App.replacing
            onClicked: root.replaceRequested(root.videoId)
        }
    }
}
