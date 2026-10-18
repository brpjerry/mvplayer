import QtQuick
import MvPlayer.Core

// Right of the album grid in music mode: the queue, the track that plays
// (with the way to its video, where it has one) and its cover. A panel like
// the review panel on the other side, with the same way of coming and going.
Rectangle {
    id: root

    property bool active: false
    readonly property int panelWidth: 320
    property var queue: []        // the player's queue (track maps)
    property int index: -1        // which of them plays
    property var current: null    // the playing item, or null
    property bool playing: false

    signal playRequested(int index)
    signal videoRequested(var videoId)

    width: active ? panelWidth : 0
    Behavior on width { NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized } }
    visible: width > 0
    clip: true
    color: Theme.surface

    // What plays: a track of the music library, or a video that plays
    // on from the video library across the mode switch.
    readonly property bool hasItem: current !== null
    readonly property bool hasTrack: hasItem && current.kind === "track"
    readonly property var track: hasItem ? current : null
    readonly property alias queueView: list
    onIndexChanged: list.currentIndex = index

    function formatLine(t) {
        let s = (t.codec || "").toUpperCase()
        if (t.bits > 0 && t.sampleRate > 0)
            s += " " + t.bits + "/" + Math.round(t.sampleRate / 1000)
        else if (t.sampleRate > 0)
            s += " " + Math.round(t.sampleRate / 1000) + " kHz"
        if (t.lossless)
            s += "  ·  lossless"
        else if (t.bitrate > 0)
            s += "  ·  " + t.bitrate + " kbit/s"
        if (t.discNo > 0)
            s += "  ·  Disc " + t.discNo
        if (t.trackNo > 0)
            s += "  ·  Track " + t.trackNo
        return s
    }
    function videoLine(v) {
        const parts = []
        if (v.quality) parts.push(v.quality)
        if (v.audioDetail) parts.push(v.audioDetail)
        return parts.join("  ·  ")
    }
    // Why the track has no video, in a few words (docs/import-rules.html).
    function stateLine(t) {
        if (t.lookup === 0)
            return "Not looked up: its folder is not a video folder (A12)"
        switch (t.state) {
        case "pending": return "Waiting to be looked up"
        case "not_found": return "No music video found"
        case "review": return "A video waits for your verdict"
        case "failed": return "The last lookup could not finish; it is tried again"
        case "skipped": return "Not looked up: skipped by the rules (A2–A4)"
        }
        return ""
    }

    Rectangle {
        width: 1
        height: parent.height
        color: Theme.line
    }

    Item {
        id: body
        x: 1
        width: root.panelWidth - 1
        height: parent.height

        // ---- Queue ---------------------------------------------------------
        Item {
            id: queueHeader
            x: 20
            y: 22
            width: parent.width - 40
            height: 22
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "Queue"
                color: Theme.text
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: list.count > 0 ? list.count + (root.hasTrack ? (list.count === 1 ? " track" : " tracks") : (list.count === 1 ? " video" : " videos")) : ""
                color: Theme.textFaint
                font.pixelSize: 12
                font.features: { "tnum": 1 }
            }
        }
        ListView {
            id: list
            anchors.top: queueHeader.bottom
            anchors.topMargin: 8
            anchors.bottom: details.top
            anchors.bottomMargin: 10
            width: parent.width
            clip: true
            model: root.hasItem ? root.queue : []
            // Set, not bound: the view writes currentIndex itself when a
            // model arrives (to 0), which would break a binding.
            Component.onCompleted: currentIndex = root.index
            boundsBehavior: Flickable.StopAtBounds
            onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
            // A queue that arrives with its index already set (a video
            // playing on across the mode switch) scrolls to it as well.
            onCountChanged: { currentIndex = root.index; keepCurrentShown() }
            // The panel slides in, so the list has its size only at the
            // end: the row is brought into view once the size has settled.
            onWidthChanged: keepCurrentShown()
            onHeightChanged: keepCurrentShown()
            function keepCurrentShown() { settle.restart() }
            Timer {
                id: settle
                interval: 450
                onTriggered: if (list.currentIndex >= 0 && list.currentIndex < list.count) list.positionViewAtIndex(list.currentIndex, ListView.Contain)
            }

            delegate: Item {
                id: row
                required property int index
                required property var modelData
                readonly property bool now: index === root.index
                readonly property bool played: index < root.index
                width: list.width
                height: 38

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    radius: Theme.radiusSmall
                    color: row.now ? Theme.accentSoft : rowMouse.pressed ? Theme.pressed : Theme.hover
                    opacity: row.now || rowMouse.containsMouse ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                }
                Rectangle {
                    visible: row.now
                    x: 8
                    width: 3
                    height: 18
                    radius: 1.5
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.accent
                }
                Text {
                    id: number
                    x: 20
                    width: 26
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignRight
                    text: row.modelData.trackNo > 0 ? row.modelData.trackNo : row.index + 1
                    color: row.now ? Theme.accent : Theme.textFaint
                    font.pixelSize: 12
                    font.features: { "tnum": 1 }
                }
                Text {
                    anchors.left: number.right
                    anchors.leftMargin: 10
                    anchors.right: trail.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.title
                    color: row.now ? Theme.accentHi : row.played ? Theme.textDim : Theme.text
                    font.pixelSize: 13
                    font.weight: row.now ? Font.DemiBold : Font.Normal
                    elide: Text.ElideRight
                }
                Row {
                    id: trail
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Icon {
                        visible: root.hasTrack && row.modelData.videoId > 0
                        anchors.verticalCenter: parent.verticalCenter
                        path: Icons.movie
                        size: 14
                        color: Theme.textFaint
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.durationText || Theme.formatTime(row.modelData.duration || 0)
                        color: Theme.textFaint
                        font.pixelSize: 12
                        font.features: { "tnum": 1 }
                    }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.playRequested(row.index)
                }
            }

            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: "Click an album to play it"
                color: Theme.textFaint
                font.pixelSize: 13
            }
        }

        // ---- The track -----------------------------------------------------
        Column {
            id: details
            anchors.bottom: coverItem.top
            anchors.bottomMargin: 18
            x: 20
            width: parent.width - 40
            spacing: 3
            visible: root.hasItem
            Text {
                width: parent.width
                text: root.track ? root.track.title : ""
                color: Theme.text
                font.pixelSize: 15
                lineHeightMode: Text.FixedHeight
                lineHeight: 20 // a CJK fallback font is taller; the lines keep their places
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                wrapMode: Text.Wrap
                maximumLineCount: 2
            }
            Text {
                width: parent.width
                text: root.track ? root.track.artist.replace(/; /g, ", ") : ""
                color: Theme.textDim
                font.pixelSize: 13
                lineHeightMode: Text.FixedHeight
                lineHeight: 18
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: root.track ? [root.track.album, root.track.year > 0 ? root.track.year : ""].filter(Boolean).join("  ·  ") : ""
                visible: text.length > 0
                color: Theme.textDim
                font.pixelSize: 12
                lineHeightMode: Text.FixedHeight
                lineHeight: 17
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: root.track ? (root.hasTrack ? root.formatLine(root.track) : root.videoLine(root.track)) : ""
                color: Theme.textFaint
                font.pixelSize: 11
                lineHeightMode: Text.FixedHeight
                lineHeight: 15
                elide: Text.ElideRight
            }
            Item { width: 1; height: 6 }
            // Its music video, where it has one; else why not.
            FlatButton {
                visible: root.hasTrack && root.track !== null && root.track.videoId > 0
                icon: Icons.movie
                text: "Music video"
                onClicked: root.videoRequested(root.track.videoId)
            }
            Text {
                visible: root.hasTrack && root.track !== null && !(root.track.videoId > 0)
                width: parent.width
                text: root.hasTrack && root.track !== null ? root.stateLine(root.track) : ""
                color: Theme.textFaint
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        // ---- Cover ---------------------------------------------------------
        Rectangle {
            anchors.fill: coverItem
            radius: Theme.radius
            color: Theme.raised
            Icon {
                anchors.centerIn: parent
                path: root.hasItem && !root.hasTrack ? Icons.movie : Icons.music
                size: 56
                color: Theme.textFaint
            }
        }
        RoundedImage {
            id: coverItem
            // As wide as the panel allows, but in a short window it gives
            // way so the queue keeps at least a few rows.
            readonly property real room: body.height - (queueHeader.y + queueHeader.height + 8) - 120 - 10
                                         - (details.visible ? details.height + 18 : 0) - 16
            width: Math.max(96, Math.min(parent.width - 32, root.hasTrack ? room : room * 16 / 9))
            height: root.hasTrack ? width : width * 9 / 16
            x: (parent.width - width) / 2
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            source: root.track ? root.track.thumb : ""
            placeholder: "transparent"
        }
    }
}
