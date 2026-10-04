import QtQuick

// One music video in the grid: thumbnail with hover affordances, title, artist.
Item {
    id: root

    required property int index
    required property var videoId
    required property string title
    required property string artist
    required property int year
    required property string durationText
    required property url thumb
    required property string quality
    required property string audioSource
    required property bool review      // waits for the user's verdict
    required property string ytTitle
    required property int reviewOption   // which of the options for this track is shown, from 1
    required property int reviewOptions

    property bool current: false   // this video is loaded in the player
    property bool playing: false   // ...and not paused
    readonly property Item thumbnail: thumbItem

    signal activated()
    signal approved()
    signal rejected()
    signal stepped(int delta)          // show the next (+1) or previous (-1) option

    readonly property bool hovered: mouse.containsMouse

    Item {
        id: body
        anchors.fill: parent
        anchors.margins: 10
        scale: mouse.pressed ? 0.975 : 1
        Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }

        RoundedImage {
            id: thumbItem
            width: parent.width
            height: Math.round(width * 9 / 16)
            source: root.thumb
            zoom: root.hovered ? 1.06 : 1.0
            dim: root.hovered ? 0.35 : 0.0
            Behavior on zoom { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutCubic } }
            Behavior on dim { NumberAnimation { duration: Theme.normal } }
        }

        // Selection ring for the video that is loaded
        Rectangle {
            anchors.fill: thumbItem
            anchors.margins: -4
            radius: Theme.radius + 4
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
            opacity: root.current ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.normal } }
        }

        // Play affordance
        Rectangle {
            anchors.centerIn: thumbItem
            width: 52
            height: 52
            radius: 26
            color: Theme.accent
            opacity: root.hovered ? 1 : 0
            scale: root.hovered ? 1 : 0.7
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.normal } }
            Behavior on scale { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutBack; easing.overshoot: 1.4 } }
            Icon {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: 2
                path: Icons.play
                size: 28
                color: Theme.accentInk
            }
        }

        // Badges
        Row {
            anchors.left: thumbItem.left
            anchors.bottom: thumbItem.bottom
            anchors.margins: 8
            spacing: 4
            Repeater {
                model: {
                    const list = []
                    if (root.quality === "4K" || root.quality === "8K") list.push(root.quality)
                    if (root.audioSource === "library" && !root.review) list.push("LOSSLESS")
                    return list
                }
                Rectangle {
                    required property string modelData
                    width: badgeText.implicitWidth + 10
                    height: 18
                    radius: 4
                    color: Theme.scrim
                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: parent.modelData
                        color: Theme.scrimText
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.4
                    }
                }
            }
        }
        Rectangle {
            anchors.right: thumbItem.right
            anchors.bottom: thumbItem.bottom
            anchors.margins: 8
            width: durationLabel.implicitWidth + 10
            height: 18
            radius: 4
            color: Theme.scrim
            Text {
                id: durationLabel
                anchors.centerIn: parent
                text: root.durationText
                color: Theme.scrimText
                font.pixelSize: 11
                font.weight: Font.Medium
                font.features: { "tnum": 1 }
            }
        }

        // Caption
        EqBars {
            id: bars
            anchors.left: parent.left
            anchors.top: thumbItem.bottom
            anchors.topMargin: 13
            visible: root.current
            playing: root.playing
            width: 13
            height: 12
        }
        Text {
            id: titleLabel
            anchors.left: parent.left
            anchors.leftMargin: root.current ? 21 : 0
            anchors.right: parent.right
            anchors.top: thumbItem.bottom
            anchors.topMargin: 10
            text: root.title
            color: root.current ? Theme.accentHi : Theme.text
            font.pixelSize: 14
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Behavior on color { ColorAnimation { duration: Theme.normal } }
            Behavior on anchors.leftMargin { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
        }
        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: titleLabel.bottom
            anchors.topMargin: 3
            // Under review, what matters is which upload this is.
            text: root.review ? root.ytTitle : root.artist.replace(/; /g, ", ") + (root.year > 0 ? "  ·  " + root.year : "")
            color: Theme.textDim
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: body
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.activated()
    }

    // Other uploads that could be this track's video: step through them.
    Repeater {
        model: root.review && root.reviewOptions > 1 ? [-1, 1] : []
        Rectangle {
            id: arrow
            required property int modelData
            x: body.x + (modelData < 0 ? 8 : thumbItem.width - width - 8)
            y: body.y + (thumbItem.height - height) / 2
            width: 34
            height: 34
            radius: 17
            color: arrowMouse.containsMouse ? Theme.accent : Theme.scrim
            scale: arrowMouse.pressed ? 0.92 : 1
            Behavior on color { ColorAnimation { duration: Theme.fast } }
            Behavior on scale { NumberAnimation { duration: Theme.fast } }
            Icon {
                anchors.centerIn: parent
                path: Icons.back
                rotation: arrow.modelData < 0 ? 0 : 180
                size: 20
                color: arrowMouse.containsMouse ? Theme.accentInk : Theme.scrimText
            }
            MouseArea {
                id: arrowMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.stepped(arrow.modelData)
            }
            Tooltip {
                text: arrow.modelData < 0 ? "Previous option" : "Next option"
                shown: arrowMouse.containsMouse
            }
        }
    }
    Rectangle {
        visible: root.review && root.reviewOptions > 1
        x: body.x + 8
        y: body.y + 8
        width: optionLabel.implicitWidth + 12
        height: 20
        radius: 4
        color: Theme.scrim
        Text {
            id: optionLabel
            anchors.centerIn: parent
            text: "Option " + root.reviewOption + " of " + root.reviewOptions
            color: Theme.scrimText
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
    }

    // The verdict: accept it into the library, or turn it down.
    Row {
        visible: root.review
        x: body.x + thumbItem.width - width - 8
        y: body.y + 8
        spacing: 6
        Repeater {
            model: [
                { accept: true, icon: Icons.check, tint: "#3fbf7f", tip: "This is the track's video" },
                { accept: false, icon: Icons.close, tint: "#ff5d5d", tip: "Not this track's video: delete it" }
            ]
            Rectangle {
                id: verdict
                required property var modelData
                width: 34
                height: 34
                radius: 17
                color: verdictMouse.containsMouse ? modelData.tint : Theme.scrim
                border.width: 1
                border.color: modelData.tint
                scale: verdictMouse.pressed ? 0.92 : 1
                Behavior on color { ColorAnimation { duration: Theme.fast } }
                Behavior on scale { NumberAnimation { duration: Theme.fast } }
                Icon {
                    anchors.centerIn: parent
                    path: verdict.modelData.icon
                    size: 20
                    color: verdictMouse.containsMouse ? "#ffffff" : verdict.modelData.tint
                }
                MouseArea {
                    id: verdictMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: verdict.modelData.accept ? root.approved() : root.rejected()
                }
                Tooltip {
                    text: verdict.modelData.tip
                    shown: verdictMouse.containsMouse
                }
            }
        }
    }
}
