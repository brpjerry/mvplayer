import QtQuick
import QtQuick.Controls.Basic
import MvPlayer.Core

// Allowing or denying a channel, asked before it is done: what it would
// change, video by video. Allowed, a channel is an artist's channel for every
// track, and each of its videos under review that waited only on whose
// channel it is on is accepted; which ones is found out by listening, so the
// list fills in as they are judged. Denied, every video the library has from
// it is deleted.
Popup {
    id: root

    property bool allow: true
    // {id, name, handle, allow, videos: [{videoId, ytTitle, thumb, review, tracks}]}
    property var change: ({})
    // What listening found, by video: {accept, track, reason}
    property var judged: ({})
    property bool judging: false
    readonly property var videos: change.videos || []
    readonly property int accepted: {
        let n = 0
        for (const id in judged)
            n += judged[id].accept ? 1 : 0
        return n
    }
    readonly property int reviewCount: {
        let n = 0
        for (const v of videos)
            n += v.review ? 1 : 0
        return n
    }

    // The videos that changed, for whoever plays one of them.
    signal confirmed(var videoIds)

    function openFor(videoId, allowIt) {
        allow = allowIt
        judged = ({})
        change = App.channelChange(videoId, allowIt)
        if (change.id === undefined)
            return
        judging = allowIt && videos.length > 0
        open()
    }
    function confirm() {
        const ids = []
        for (const v of videos)
            ids.push(v.videoId)
        App.applyChannelChange(change.id, allow)
        confirmed(ids)
        close()
    }
    onClosed: App.cancelChannelChange()

    Connections {
        target: App
        // (From the moment it is asked for: `opened` only turns true once
        // the popup has finished coming in.)
        enabled: root.visible
        function onChannelJudged(videoId, accept, track, reason) {
            const next = Object.assign({}, root.judged)
            next[videoId] = { accept: accept, track: track, reason: reason }
            root.judged = next
        }
        function onChannelPreviewDone() { root.judging = false }
    }

    width: 600
    height: Math.min(content.implicitHeight + 2, (Overlay.overlay ? Overlay.overlay.height : 800) - 32)
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true // Escape closes it only while it has the focus
    padding: 1
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    Overlay.modal: Rectangle {
        color: "#990c0d10"
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    }
    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
            NumberAnimation { property: "scale"; from: 0.95; to: 1; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; to: 0; duration: Theme.fast }
            NumberAnimation { property: "scale"; to: 0.97; duration: Theme.fast }
        }
    }
    background: Rectangle {
        radius: Theme.radius + 4
        color: Theme.raised
        border.width: 1
        border.color: Theme.line
    }

    function plural(n, one, many) { return n + " " + (n === 1 ? one : many) }

    contentItem: Column {
        id: content
        width: root.width - 2
        topPadding: 20
        bottomPadding: 20

        // Header
        Item {
            width: parent.width
            height: 32
            Text {
                x: 24
                width: parent.width - 80
                anchors.verticalCenter: parent.verticalCenter
                text: (root.allow ? "Allow “" : "Deny “") + (root.change.name || "") + "”?"
                color: Theme.text
                font.pixelSize: 18
                font.weight: Font.DemiBold
                font.letterSpacing: -0.2
                elide: Text.ElideRight
            }
            IconButton {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                icon: Icons.close
                iconSize: 18
                onClicked: root.close()
            }
        }
        Text {
            x: 24
            width: parent.width - 48
            visible: (root.change.handle || "").length > 0
            text: root.change.handle || ""
            color: Theme.textFaint
            font.pixelSize: 12
            elide: Text.ElideRight
        }
        Item { width: 1; height: 8 }
        Text {
            x: 24
            width: parent.width - 48
            text: root.allow
                  ? "It counts as an artist's channel from now on, for every track, and is never ignored as a small one. "
                    + "Its videos under review that waited only on whose channel this is are accepted; the others stay for your verdict."
                  : "Every video your library has from it is deleted, and its uploads are never taken again. "
                    + "Tracks that had one of its videos are looked up again for another."
            color: Theme.textDim
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
        Item { width: 1; height: 14 }

        // What it changes
        Item {
            x: 24
            width: parent.width - 48
            height: 20
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    if (root.videos.length === 0)
                        return root.allow ? "No video from it waits for review." : "Your library has no video from it."
                    if (!root.allow) {
                        const kept = root.videos.length - root.reviewCount
                        return "Deleted: " + (kept > 0 ? root.plural(kept, "video", "videos") + " in your library" : "")
                            + (kept > 0 && root.reviewCount > 0 ? " and " : "")
                            + (root.reviewCount > 0 ? root.plural(root.reviewCount, "video", "videos") + " under review" : "")
                    }
                    const done = Object.keys(root.judged).length
                    if (root.judging)
                        return "Listening: " + done + " of " + root.videos.length + " judged, " + root.accepted + " would be accepted"
                    return root.accepted + " of " + root.plural(root.videos.length, "video", "videos") + " under review would be accepted"
                }
                color: Theme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            Spinner {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: root.judging
                running: visible
                size: 16
            }
        }
        Item { width: 1; height: 8 }
        ListView {
            id: list
            x: 14
            width: parent.width - 28
            height: Math.min(contentHeight, 4 * 84)
            visible: root.videos.length > 0
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.videos
            spacing: 4
            ScrollBar.vertical: ScrollBar { policy: list.contentHeight > list.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
            delegate: Item {
                id: row
                required property var modelData
                readonly property var verdict: root.judged[modelData.videoId]
                width: list.width
                height: 80
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    color: Theme.surface
                }
                RoundedImage {
                    id: thumb
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 85
                    height: 48
                    radius: Theme.radiusSmall
                    source: row.modelData.thumb || ""
                }
                Column {
                    anchors.left: thumb.right
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text {
                        width: parent.width
                        text: row.modelData.ytTitle
                        color: Theme.text
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: (row.modelData.review ? (row.modelData.tracks.length > 1 ? "Candidates: " : "Candidate: ") : "The video of ")
                              + (row.modelData.tracks.length > 0 ? row.modelData.tracks.join(" · ") : "no track in your music folders")
                        color: Theme.textDim
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: !root.allow ? (row.modelData.review ? "Under review: deleted, and not offered to them again" : "In your library: deleted")
                              : row.verdict === undefined ? "Listening…"
                              : row.verdict.accept ? "Will be accepted for “" + row.verdict.track + "”"
                              : "Stays under review: " + row.verdict.reason
                        color: !root.allow ? "#ff5d5d"
                               : row.verdict === undefined ? Theme.textFaint
                               : row.verdict.accept ? "#3fbf7f" : Theme.textFaint
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }
            }
        }
        Item { width: 1; height: 18 }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 24
            spacing: 10
            FlatButton {
                text: "Cancel"
                onClicked: root.close()
            }
            FlatButton {
                text: root.allow ? "Allow the channel" : "Deny the channel"
                primary: true
                onClicked: root.confirm()
            }
        }
    }
}
