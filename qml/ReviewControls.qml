import QtQuick
import MvPlayer.Core

// What a video that waits for review carries on its picture: accept and
// reject, and how many tracks could have it when that is more than one. Laid
// over the thumbnail in the grid, and over the video while it plays there.
// An orphan carries the same verdict: keep or delete.
// Resting on accept or reject for a second brings up a second button under
// it, for the upload's channel as a whole: allow it (a tick with a doubled
// arm), or deny it (a cross with one). Only where the channel is not an
// artist's already.
Item {
    id: root

    // The candidate tracks of the video: how many, and the title of the one
    // on show, which accepting is for.
    property int candidates: 1
    property string candidateTitle
    // The judge's advice for the candidate on show (import-rules-judge X2):
    // {asked, keep, probability, error}; shown while the judge is on.
    property var advice: ({})
    property bool judgeOn: false
    // A video already in the library with this one's title: {album, ytTitle}.
    property var sameTitle: ({})
    // An orphan's verdict: keep it, or delete it. Deleting waits while the
    // music folders are read, which settles whether its tracks are gone.
    property bool orphan: false
    property bool canDelete: true
    readonly property bool canReject: !orphan || canDelete
    // The video, for asking whether its channel can be allowed or denied.
    property var videoId: -1
    // How long the pointer rests on a verdict before the channel's button
    // shows, and how long that stays once the pointer has left both.
    property int dwell: 1000
    property int linger: 400

    signal approved()
    signal rejected()
    signal allowChannelRequested()
    signal denyChannelRequested()

    // The judge's advice: how sure it is, in the colour of its answer.
    Rectangle {
        id: adviceBadge
        readonly property bool asked: root.judgeOn && root.advice !== undefined && root.advice !== null && root.advice.asked === true
        readonly property bool error: asked && root.advice.error === true
        readonly property bool unsure: asked && !error && root.advice.probability > 0.4 && root.advice.probability < 0.6
        readonly property int percent: asked && !error ? Math.round((root.advice.keep ? root.advice.probability : 1 - root.advice.probability) * 100) : 0
        visible: asked
        x: 8
        y: 8
        width: adviceLabel.implicitWidth + 12
        height: 20
        radius: 4
        color: !asked || error || unsure ? Theme.scrim : root.advice.keep ? Theme.accent : "#c23b3b"
        Text {
            id: adviceLabel
            anchors.centerIn: parent
            text: !adviceBadge.asked ? "" : adviceBadge.error || adviceBadge.unsure ? "Judge ?" : "Judge " + adviceBadge.percent + "%"
            color: !adviceBadge.asked || adviceBadge.error || adviceBadge.unsure ? Theme.scrimText : root.advice.keep ? Theme.accentInk : "white"
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
    }

    // Several tracks could have this video: the panel below steps through them.
    Rectangle {
        visible: root.candidates > 1
        x: adviceBadge.visible ? adviceBadge.x + adviceBadge.width + 6 : 8
        y: 8
        width: candidatesLabel.implicitWidth + 12
        height: 20
        radius: 4
        color: Theme.scrim
        Text {
            id: candidatesLabel
            anchors.centerIn: parent
            text: root.candidates + " candidate tracks"
            color: Theme.scrimText
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
    }

    // For information: the song may have its video already.
    Rectangle {
        id: sameBadge
        readonly property bool wanted: root.sameTitle !== undefined && root.sameTitle !== null && root.sameTitle.ytTitle !== undefined
        visible: wanted
        x: 8
        y: root.height - height - 8
        width: Math.min(sameRow.implicitWidth + 14, root.width - 76)
        height: 22
        radius: 4
        color: Theme.scrim
        Row {
            id: sameRow
            x: 7
            anchors.verticalCenter: parent.verticalCenter
            spacing: 5
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                path: Icons.check
                size: 13
                color: Theme.accentHi
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, root.width - 76 - 32)
                text: !sameBadge.wanted ? "" : root.sameTitle.album ? "Same title in library · " + root.sameTitle.album : "Same title in library"
                color: Theme.scrimText
                font.pixelSize: 11
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }
        MouseArea {
            id: sameMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }
        Tooltip {
            // From the badge's left edge: centred on it, it would leave the grid.
            anchors.horizontalCenter: undefined
            x: 0
            text: sameBadge.wanted ? "In the library: " + root.sameTitle.ytTitle : ""
            shown: sameMouse.containsMouse
        }
    }

    // The verdict: accept it into the library, or turn it down.
    Row {
        x: root.width - width - 8
        y: 8
        spacing: 6
        Repeater {
            model: [
                { accept: true, icon: Icons.check, tint: "#3fbf7f" },
                { accept: false, icon: Icons.close, tint: "#ff5d5d" }
            ]
            Rectangle {
                id: verdict
                required property var modelData
                readonly property bool usable: modelData.accept || root.canReject
                opacity: usable ? 1 : 0.45
                width: 34
                height: 34
                radius: 17
                color: verdictMouse.containsMouse && usable ? modelData.tint : Theme.scrim
                border.width: 1
                border.color: modelData.tint
                scale: verdictMouse.pressed && usable ? 0.92 : 1
                Behavior on color { ColorAnimation { duration: Theme.fast } }
                Behavior on scale { NumberAnimation { duration: Theme.fast } }
                Icon {
                    anchors.centerIn: parent
                    path: verdict.modelData.icon
                    size: 20
                    color: verdictMouse.containsMouse && verdict.usable ? "#ffffff" : verdict.modelData.tint
                }
                MouseArea {
                    id: verdictMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: verdict.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: {
                        if (verdict.modelData.accept)
                            root.approved()
                        else if (root.canReject)
                            root.rejected()
                    }
                }

                // The channel's button: after a second on this one.
                property bool channelShown: false
                property string channelName: ""
                readonly property bool onEither: verdictMouse.containsMouse || channelMouse.containsMouse
                onVisibleChanged: if (!visible) channelShown = false
                Timer {
                    interval: root.dwell
                    running: verdictMouse.containsMouse && !verdict.channelShown && !root.orphan && root.visible
                    onTriggered: {
                        const channel = App.channelActions(root.videoId)
                        if (channel.offered === true) {
                            verdict.channelName = channel.name
                            verdict.channelShown = true
                        }
                    }
                }
                Timer {
                    interval: root.linger
                    running: verdict.channelShown && !verdict.onEither
                    onTriggered: verdict.channelShown = false
                }
                Rectangle {
                    id: channelButton
                    y: parent.height + 6
                    width: 34
                    height: 34
                    radius: 17
                    visible: opacity > 0
                    opacity: verdict.channelShown ? 1 : 0
                    color: channelMouse.containsMouse ? verdict.modelData.tint : Theme.scrim
                    border.width: 1
                    border.color: verdict.modelData.tint
                    scale: channelMouse.pressed ? 0.92 : 1
                    Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                    Behavior on scale { NumberAnimation { duration: Theme.fast } }
                    // The verdict's own icon with a stroke of it doubled.
                    Icon {
                        anchors.centerIn: parent
                        path: verdict.modelData.accept ? Icons.checkAll : Icons.closeAll
                        size: 20
                        color: channelMouse.containsMouse ? "#ffffff" : verdict.modelData.tint
                    }
                    MouseArea {
                        id: channelMouse
                        anchors.fill: parent
                        enabled: verdict.channelShown
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            verdict.channelShown = false
                            if (verdict.modelData.accept)
                                root.allowChannelRequested()
                            else
                                root.denyChannelRequested()
                        }
                    }
                    Tooltip {
                        below: true
                        anchors.horizontalCenter: undefined
                        x: parent.width - width
                        text: verdict.modelData.accept
                              ? "Allow the channel: “" + verdict.channelName + "” is an artist's channel"
                              : "Deny the channel: delete every video from “" + verdict.channelName + "”"
                        shown: channelMouse.containsMouse
                    }
                }
                Tooltip {
                    // Below: the buttons are at the top of the card, and
                    // above them the first row's would leave the grid.
                    below: true
                    // And ending at the button's right edge, for the last column.
                    anchors.horizontalCenter: undefined
                    x: parent.width - width
                    text: verdict.modelData.accept
                          ? (root.orphan ? "Keep it: its tracks are gone, but it stays"
                             : root.candidateTitle ? "This is the video of “" + root.candidateTitle + "”" : "This is the track's video")
                          : (!root.orphan ? (root.candidates > 1 ? "The video of none of its candidates: delete it" : "Not this track's video: delete it")
                             : root.canDelete ? "Delete it" : "Wait until the music folders have been read")
                    // (It would lie over the channel's button, which is where it shows.)
                    shown: verdictMouse.containsMouse && !verdict.channelShown
                }
            }
        }
    }
}
