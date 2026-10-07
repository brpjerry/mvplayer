import QtQuick
import QtQuick.Controls.Basic
import MvPlayer.Core

// Another video in place of the one a track has, chosen by hand: from up to
// three uploads that are the track by ear (any channel), or one the user
// names. One that does not sound like the track is taken only after a
// second thought.
Popup {
    id: root

    property var videoId: -1
    property var info: ({})
    // searching | options | checking | confirm
    property string mode: "searching"
    property var options: []
    property string error: ""
    property string entryError: ""
    // The upload the user named, as checked: {id, title, channel, matches, reason}
    property var checked: ({})

    function openFor(id) {
        videoId = id
        info = App.videoSources(id)
        options = []
        error = ""
        entryError = ""
        checked = ({})
        entry.text = ""
        mode = "searching"
        open()
        App.replaceSearch(id)
    }
    function choose(ytId) {
        App.replaceWith(videoId, ytId)
        close()
    }
    function checkEntry() {
        const why = App.replaceCheck(videoId, entry.text)
        if (why.length > 0) {
            entryError = why
            return
        }
        entryError = ""
        mode = "checking"
    }

    Connections {
        target: App
        function onReplaceOptionsReady(id, list, why) {
            if (id !== root.videoId || !root.opened)
                return
            root.options = list
            root.error = why
            if (root.mode === "searching")
                root.mode = "options"
        }
        function onReplaceCheckDone(id, result) {
            if (id !== root.videoId || !root.opened)
                return
            root.checked = result
            if (result.error) {
                root.entryError = result.error
                root.mode = "options"
            } else if (result.matches) {
                root.choose(result.id)
            } else {
                root.mode = "confirm"
            }
        }
    }

    width: 560
    height: Math.min(content.implicitHeight + 2, (Overlay.overlay ? Overlay.overlay.height : 800) - 32)
    anchors.centerIn: Overlay.overlay
    modal: true
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

    function durationText(seconds) {
        const s = Math.round(seconds)
        const m = Math.floor(s / 60)
        return m + ":" + (s % 60 < 10 ? "0" : "") + (s % 60)
    }

    // One upload the user can take
    component OptionCard: Item {
        id: card
        property var option
        width: parent.width
        height: 84
        Rectangle {
            anchors.fill: parent
            radius: Theme.radius
            color: cardMouse.containsMouse ? Theme.hover : Theme.surface
            Behavior on color { ColorAnimation { duration: Theme.fast } }
        }
        RoundedImage {
            id: thumb
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 112
            height: 63
            radius: Theme.radiusSmall
            source: card.option.thumbnail || ""
        }
        Column {
            anchors.left: thumb.right
            anchors.leftMargin: 14
            anchors.right: chooseButton.left
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            Text {
                width: parent.width
                text: card.option.title
                color: Theme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: card.option.channel + " · " + root.durationText(card.option.duration)
                      + (card.option.artistChannel ? " · the artist's channel" : "")
                color: card.option.artistChannel ? Theme.accent : Theme.textDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: card.option.summary
                color: Theme.textFaint
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
        FlatButton {
            id: chooseButton
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: "Use this"
            primary: true
            onClicked: root.choose(card.option.id)
        }
        MouseArea {
            id: cardMouse
            anchors.fill: parent
            anchors.rightMargin: chooseButton.width + 20
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.choose(card.option.id)
        }
    }

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
                anchors.verticalCenter: parent.verticalCenter
                text: "Replace video"
                color: Theme.text
                font.pixelSize: 18
                font.weight: Font.DemiBold
                font.letterSpacing: -0.2
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
            text: "Now “" + (root.info.ytTitle || "") + "”" + (root.info.ytChannel ? " — " + root.info.ytChannel : "")
            color: Theme.textDim
            font.pixelSize: 12
            elide: Text.ElideRight
        }
        Item { width: 1; height: 18 }

        // ---- Confirm: the named upload does not sound like the track
        Column {
            visible: root.mode === "confirm"
            x: 24
            width: parent.width - 48
            spacing: 12
            Row {
                spacing: 8
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    path: Icons.info
                    size: 18
                    color: Theme.warn
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Are you sure?"
                    color: Theme.text
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
            }
            Text {
                width: parent.width
                text: "“" + (root.checked.title || root.checked.id) + "”" + (root.checked.channel ? " by " + root.checked.channel : "")
                      + " does not sound like this track: " + (root.checked.reason || "")
                      + ". Taken all the same, it keeps its own audio; your track's cannot be put in."
                color: Theme.textDim
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            Row {
                anchors.right: parent.right
                spacing: 10
                FlatButton {
                    text: "Back"
                    onClicked: root.mode = "options"
                }
                FlatButton {
                    text: "Replace anyway"
                    primary: true
                    onClicked: root.choose(root.checked.id)
                }
            }
            Item { width: 1; height: 4 }
        }

        // ---- Search and entry
        Column {
            visible: root.mode !== "confirm"
            width: parent.width

            // A link the user has
            Text {
                x: 24
                text: "A video you know"
                color: Theme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            Item { width: 1; height: 8 }
            Item {
                x: 24
                width: parent.width - 48
                height: 36
                Rectangle {
                    id: entryBox
                    anchors.left: parent.left
                    anchors.right: checkButton.left
                    anchors.rightMargin: 10
                    height: parent.height
                    radius: Theme.radiusSmall
                    color: Theme.hover
                    border.width: 1
                    border.color: entry.activeFocus ? Theme.accent : root.entryError.length > 0 ? Theme.bad : "transparent"
                    Icon {
                        id: linkIcon
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        path: Icons.link
                        size: 16
                        color: Theme.textFaint
                    }
                    TextInput {
                        id: entry
                        anchors.fill: parent
                        anchors.leftMargin: 34
                        anchors.rightMargin: 10
                        verticalAlignment: TextInput.AlignVCenter
                        color: Theme.text
                        font.pixelSize: 13
                        clip: true
                        selectByMouse: true
                        enabled: root.mode !== "checking"
                        onAccepted: root.checkEntry()
                        onTextEdited: root.entryError = ""
                    }
                    Text {
                        anchors.fill: entry
                        verticalAlignment: Text.AlignVCenter
                        visible: entry.text.length === 0
                        text: "YouTube link or video id"
                        color: Theme.textFaint
                        font.pixelSize: 13
                    }
                }
                FlatButton {
                    id: checkButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.mode === "checking" ? "Checking…" : "Use this"
                    enabled: root.mode !== "checking" && entry.text.trim().length > 0 && !App.replacing
                    onClicked: root.checkEntry()
                }
            }
            Text {
                visible: root.entryError.length > 0 || root.mode === "checking"
                x: 24
                width: parent.width - 48
                topPadding: 6
                text: root.mode === "checking" ? "Listening to it: " + (App.replaceStage || "…") : root.entryError
                color: root.mode === "checking" ? Theme.textDim : Theme.bad
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }

            Item { width: 1; height: 18 }
            Rectangle { x: 24; width: parent.width - 48; height: 1; color: Theme.line }
            Item { width: 1; height: 16 }

            // Found by ear
            Item {
                x: 24
                width: parent.width - 48
                height: 20
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Uploads that sound like the track"
                    color: Theme.text
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    visible: root.mode === "searching"
                    Spinner {
                        anchors.verticalCenter: parent.verticalCenter
                        size: 14
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: App.replaceStage || "Searching"
                        color: Theme.textDim
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, 300)
                    }
                }
            }
            Text {
                x: 24
                width: parent.width - 48
                topPadding: 4
                text: "From any channel, not only the artist's own. Each was downloaded and compared with your track."
                color: Theme.textFaint
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
            Item { width: 1; height: 12 }
            Column {
                x: 24
                width: parent.width - 48
                spacing: 8
                Repeater {
                    model: root.options
                    OptionCard {
                        required property var modelData
                        option: modelData
                    }
                }
            }
            Text {
                visible: root.mode === "options" && root.options.length === 0
                x: 24
                width: parent.width - 48
                text: root.error.length > 0 ? "Nothing to offer: " + root.error : "Nothing to offer."
                color: Theme.textDim
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }
    }
}
