import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import MvPlayer.Core

// Library folders and import behaviour.
Popup {
    id: root

    width: 560
    // As tall as its content, as far as the window allows: then it scrolls.
    height: Math.min(header.y + header.height + body.implicitHeight + 2, (Overlay.overlay ? Overlay.overlay.height : 800) - 32)
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

    readonly property var accentPresets: [
        "#8b7dff", "#5b9dff", "#3ec9d6", "#4fd68f", "#c5d94a", "#f5b84a", "#ff8f4d", "#ff6f91", "#f06bd8"
    ]
    readonly property bool customAccent: App.accent !== "auto" && accentPresets.indexOf(App.accent) < 0

    FolderDialog {
        id: musicDialog
        title: "Add a music folder"
        currentFolder: App.pathToUrl(App.musicDirs.length > 0 ? App.musicDirs[App.musicDirs.length - 1] : "")
        onAccepted: App.addMusicDir(App.urlToPath(selectedFolder))
    }
    FolderDialog {
        id: mvDialog
        title: "Choose where music videos are stored"
        currentFolder: App.pathToUrl(App.mvDir)
        onAccepted: App.mvDir = App.urlToPath(selectedFolder)
    }

    FolderDialog {
        id: reimportFolderDialog
        title: "Re-import the music in a folder"
        currentFolder: App.pathToUrl(App.musicDirs.length > 0 ? App.musicDirs[0] : "")
        onAccepted: reimport.queued(App.reimportPath(App.urlToPath(selectedFolder)), App.urlToPath(selectedFolder))
    }
    FileDialog {
        id: reimportFileDialog
        title: "Re-import a music file"
        currentFolder: App.pathToUrl(App.musicDirs.length > 0 ? App.musicDirs[0] : "")
        onAccepted: reimport.queued(App.reimportPath(App.urlToPath(selectedFile)), App.urlToPath(selectedFile))
    }
    FileDialog {
        id: cookiesDialog
        title: "Choose the cookies.txt exported from your browser"
        nameFilters: ["Cookie files (*.txt)", "All files (*)"]
        onAccepted: root.cookiesError = App.importCookies(App.urlToPath(selectedFile))
    }
    property string cookiesError: ""
    // For automation: how far down the settings are shown.
    function scrollTo(y) { flick.contentY = Math.max(0, Math.min(y, flick.contentHeight - flick.height)) }

    component FolderRow: Item {
        id: folderRow
        property string label
        property string hint
        property string path
        property string icon
        signal change()
        width: parent.width
        height: 64

        Icon {
            id: rowIcon
            anchors.verticalCenter: parent.verticalCenter
            path: folderRow.icon
            size: 20
            color: Theme.textDim
        }
        Column {
            anchors.left: rowIcon.right
            anchors.leftMargin: 14
            anchors.right: changeButton.left
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            Text {
                text: folderRow.label
                color: Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                text: folderRow.path.length > 0 ? App.displayPath(folderRow.path) : folderRow.hint
                color: folderRow.path.length > 0 ? Theme.textDim : Theme.textFaint
                font.pixelSize: 12
                elide: Text.ElideMiddle
            }
        }
        FlatButton {
            id: changeButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: folderRow.path.length > 0 ? "Change" : "Choose"
            primary: folderRow.path.length === 0
            onClicked: folderRow.change()
        }
    }

    component SliderRow: Item {
        id: sliderRow
        property string label
        property string hint
        property real from: 0
        property real to: 1
        property real value: 0
        property string valueText: value.toFixed(1)
        signal moved(real value)
        width: parent.width
        height: sliderText.implicitHeight + 28

        Column {
            id: sliderText
            anchors.left: parent.left
            anchors.right: track.left
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            Text {
                text: sliderRow.label
                color: Theme.text
                font.pixelSize: 14
            }
            Text {
                width: parent.width
                text: sliderRow.hint
                color: Theme.textDim
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }
        Item {
            id: track
            anchors.right: valueLabel.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 150
            height: 28
            readonly property real fraction: Math.max(0, Math.min(1, (sliderRow.value - sliderRow.from) / (sliderRow.to - sliderRow.from)))
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: Theme.hover
                Rectangle {
                    width: parent.width * track.fraction
                    height: parent.height
                    radius: 2
                    color: Theme.accent
                }
            }
            Rectangle {
                x: (track.width - width) * track.fraction
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                radius: 7
                color: trackMouse.pressed || trackMouse.containsMouse ? Theme.accentHi : Theme.text
            }
            MouseArea {
                id: trackMouse
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
                // Dragging here must not scroll the panel.
                preventStealing: true
                function pick(mx) {
                    const f = Math.max(0, Math.min(1, (mx - 4 - 7) / (track.width - 14)))
                    sliderRow.moved(Math.round((sliderRow.from + f * (sliderRow.to - sliderRow.from)) * 10) / 10)
                }
                onPressed: (m) => pick(m.x)
                onPositionChanged: (m) => { if (pressed) pick(m.x) }
            }
        }
        Text {
            id: valueLabel
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 30
            horizontalAlignment: Text.AlignRight
            text: sliderRow.valueText
            color: Theme.textDim
            font.pixelSize: 12
            font.features: { "tnum": 1 }
        }
    }

    component SwitchRow: Item {
        id: switchRow
        property string label
        property string hint
        property bool checked
        signal toggled(bool checked)
        width: parent.width
        height: 58

        Column {
            anchors.left: parent.left
            anchors.right: toggle.left
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            Text {
                text: switchRow.label
                color: Theme.text
                font.pixelSize: 14
            }
            Text {
                width: parent.width
                text: switchRow.hint
                color: Theme.textFaint
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }
        FlatSwitch {
            id: toggle
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            checked: switchRow.checked
            onToggled: (c) => switchRow.toggled(c)
        }
    }

    contentItem: Item {
        // Stays put while the settings scroll under it.
        Item {
            id: header
            x: 26
            y: 22
            width: parent.width - 52
            height: 40
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Settings"
                color: Theme.text
                font.pixelSize: 19
                font.weight: Font.DemiBold
                font.letterSpacing: -0.3
            }
            IconButton {
                anchors.right: parent.right
                anchors.rightMargin: -8
                anchors.verticalCenter: parent.verticalCenter
                icon: Icons.close
                onClicked: root.close()
            }
        }

        Flickable {
            id: flick
            anchors.fill: parent
            anchors.topMargin: header.y + header.height
            clip: true
            contentHeight: body.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            KineticWheel {
                view: flick
                touchpadGain: App.touchpadGain
                wheelStep: App.wheelStep * 0.6
                deceleration: App.flickDeceleration
            }

            // Where there is more below.
            Rectangle {
                parent: flick
                anchors.right: parent.right
                anchors.rightMargin: 5
                y: flick.visibleArea.yPosition * flick.height + 4
                width: 4
                height: Math.max(24, flick.visibleArea.heightRatio * flick.height - 8)
                radius: 2
                color: Theme.textFaint
                opacity: flick.contentHeight > flick.height + 1 ? (flick.moving ? 0.9 : 0.45) : 0
                Behavior on opacity { NumberAnimation { duration: Theme.normal } }
            }

            Column {
                id: body
                width: flick.width
                padding: 26
                topPadding: 0
                bottomPadding: 22
                spacing: 2

                Column {
                    width: parent.width - 2 * body.padding

                    // Music library: any number of folders
                    Item {
                        width: parent.width
                        height: 44
                        Icon {
                            id: musicIcon
                            anchors.verticalCenter: parent.verticalCenter
                            path: Icons.music
                            size: 20
                            color: Theme.textDim
                        }
                        Text {
                            anchors.left: musicIcon.right
                            anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Music library"
                            color: Theme.text
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        FlatButton {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Add folder"
                            icon: Icons.add
                            primary: App.musicDirs.length === 0
                            onClicked: musicDialog.open()
                        }
                    }
                    Text {
                        visible: App.musicDirs.length === 0
                        x: 34
                        height: 30
                        verticalAlignment: Text.AlignVCenter
                        text: "No folders yet"
                        color: Theme.textFaint
                        font.pixelSize: 12
                    }
                    Repeater {
                        model: App.musicDirs
                        Item {
                            id: folder
                            required property string modelData
                            width: parent.width
                            height: 32
                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 26
                                anchors.rightMargin: -8
                                radius: Theme.radiusSmall
                                color: Theme.hover
                                opacity: folderHover.hovered ? 1 : 0
                                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                            }
                            HoverHandler { id: folderHover }
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 34
                                anchors.right: removeButton.left
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: App.displayPath(folder.modelData)
                                color: Theme.textDim
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                            }
                            IconButton {
                                id: removeButton
                                anchors.right: parent.right
                                anchors.rightMargin: -4
                                anchors.verticalCenter: parent.verticalCenter
                                size: 26
                                iconSize: 15
                                icon: Icons.close
                                opacity: folderHover.hovered || hovered ? 1 : 0.4
                                tooltip: "Remove folder (its videos are kept)"
                                onClicked: App.removeMusicDir(folder.modelData)
                                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                            }
                        }
                    }
                    Item { width: 1; height: 10 }
                    FolderRow {
                        label: "Music video library"
                        hint: "Not set"
                        icon: Icons.movie
                        path: App.mvDir
                        onChange: mvDialog.open()
                    }

                    Item { width: 1; height: 8 }
                    Rectangle { width: parent.width; height: 1; color: Theme.line }
                    Item { width: 1; height: 8 }

                    // Appearance: follow the system, or force dark / light
                    Item {
                        width: parent.width
                        height: 52

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Appearance"
                            color: Theme.text
                            font.pixelSize: 14
                        }
                        Rectangle {
                            id: modeSwitch
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: 228
                            height: 32
                            radius: 16
                            color: Theme.hover

                            readonly property var modes: [
                                { key: "auto", label: "Auto" },
                                { key: "dark", label: "Dark" },
                                { key: "light", label: "Light" }
                            ]
                            readonly property int current: Math.max(0, modes.findIndex((m) => m.key === App.themeMode))

                            // Sliding highlight
                            Rectangle {
                                x: 3 + modeSwitch.current * (modeSwitch.width - 6) / 3
                                y: 3
                                width: (modeSwitch.width - 6) / 3
                                height: parent.height - 6
                                radius: height / 2
                                color: Theme.accent
                                Behavior on x { NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized } }
                            }
                            Row {
                                anchors.fill: parent
                                anchors.margins: 3
                                Repeater {
                                    model: modeSwitch.modes
                                    Item {
                                        id: segment
                                        required property var modelData
                                        required property int index
                                        width: (modeSwitch.width - 6) / 3
                                        height: parent.height
                                        Text {
                                            anchors.centerIn: parent
                                            text: segment.modelData.label
                                            color: segment.index === modeSwitch.current ? Theme.accentInk : Theme.textDim
                                            font.pixelSize: 12
                                            font.weight: Font.DemiBold
                                            Behavior on color { ColorAnimation { duration: Theme.normal } }
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: App.themeMode = segment.modelData.key
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Accent colour: follow the video, a preset, or any colour
                    Item {
                        width: parent.width
                        height: 106

                        Text {
                            y: 8
                            text: "Accent colour"
                            color: Theme.text
                            font.pixelSize: 14
                        }
                        Text {
                            anchors.right: parent.right
                            y: 10
                            text: App.accent === "auto" ? "Follows the video that is playing"
                                : root.customAccent ? "Custom" : ""
                            color: Theme.textFaint
                            font.pixelSize: 12
                        }
                        Row {
                            y: 40
                            spacing: 9

                            Item {
                                id: autoChip
                                readonly property bool selected: App.accent === "auto"
                                width: autoLabel.implicitWidth + 26
                                height: 28
                                Rectangle {
                                    anchors.fill: parent
                                    radius: 14
                                    color: autoChip.selected ? Theme.accentSoft : autoMouse.containsMouse ? Theme.pressed : Theme.hover
                                    border.width: autoChip.selected ? 2 : 0
                                    border.color: Theme.accent
                                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                                }
                                Text {
                                    id: autoLabel
                                    anchors.centerIn: parent
                                    text: "Auto"
                                    color: autoChip.selected ? Theme.accentHi : Theme.text
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                }
                                MouseArea {
                                    id: autoMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: App.accent = "auto"
                                }
                            }

                            Repeater {
                                model: root.accentPresets
                                Item {
                                    id: swatch
                                    required property string modelData
                                    readonly property bool selected: App.accent === modelData
                                    width: 28
                                    height: 28
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: 14
                                        color: "transparent"
                                        border.width: 2
                                        border.color: swatch.modelData
                                        opacity: swatch.selected ? 1 : 0
                                        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 18
                                        height: 18
                                        radius: 9
                                        color: swatch.modelData
                                        scale: swatchMouse.pressed ? 0.9 : swatchMouse.containsMouse && !swatch.selected ? 1.2 : 1
                                        Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
                                    }
                                    MouseArea {
                                        id: swatchMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: App.accent = swatch.modelData
                                    }
                                }
                            }

                        }

                        // Any other hue: click or drag along the strip
                        Item {
                            id: hueStrip
                            y: 80
                            width: parent.width
                            height: 14

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width
                                height: hueMouse.containsMouse || hueMouse.pressed ? 8 : 6
                                radius: height / 2
                                Behavior on height { NumberAnimation { duration: Theme.fast } }
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0 / 6; color: Theme.accentForHue(0 / 6) }
                                    GradientStop { position: 1 / 6; color: Theme.accentForHue(1 / 6) }
                                    GradientStop { position: 2 / 6; color: Theme.accentForHue(2 / 6) }
                                    GradientStop { position: 3 / 6; color: Theme.accentForHue(3 / 6) }
                                    GradientStop { position: 4 / 6; color: Theme.accentForHue(4 / 6) }
                                    GradientStop { position: 5 / 6; color: Theme.accentForHue(5 / 6) }
                                    GradientStop { position: 6 / 6; color: Theme.accentForHue(0.999) }
                                }
                            }
                            Rectangle {
                                visible: root.customAccent
                                x: Math.max(0, Math.min(parent.width - width, Math.max(0, Theme.accentSource.hslHue) * parent.width - width / 2))
                                anchors.verticalCenter: parent.verticalCenter
                                width: 14
                                height: 14
                                radius: 7
                                color: Theme.accentSource
                                border.width: 2
                                border.color: Theme.text
                            }
                            MouseArea {
                                id: hueMouse
                                anchors.fill: parent
                                anchors.margins: -6
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                preventStealing: true
                                function pick() {
                                    const hue = Math.max(0, Math.min(0.999, (mouseX - 6) / hueStrip.width))
                                    App.accent = "" + Theme.accentForHue(hue)
                                }
                                onPressed: pick()
                                onPositionChanged: if (pressed) pick()
                            }
                        }
                    }

                    Item { width: 1; height: 6 }
                    Rectangle { width: parent.width; height: 1; color: Theme.line }
                    Item { width: 1; height: 8 }

                    SwitchRow {
                        label: "Enlarge a video when it starts"
                        hint: "Off: a video starts in its thumbnail, and a click on it enlarges it."
                        checked: App.autoExpand
                        onToggled: (c) => App.autoExpand = c
                    }
                    SwitchRow {
                        label: "Use my library's audio"
                        hint: "When your track is higher quality than YouTube's, it is synced to the video and used as the main audio. A video it cannot be synced to is not imported."
                        checked: App.replaceAudio
                        onToggled: (c) => App.replaceAudio = c
                    }
                    SwitchRow {
                        label: "Skip still-image videos"
                        hint: "Ignore uploads that are just the cover art set to music."
                        checked: App.skipStillImages
                        onToggled: (c) => App.skipStillImages = c
                    }
                    SwitchRow {
                        label: "Allow unofficial uploads"
                        hint: "Also accept videos from channels that do not look like the artist's or label's."
                        checked: App.allowUnofficial
                        onToggled: (c) => App.allowUnofficial = c
                    }

                    Item { width: 1; height: 12 }

                    Row {
                        spacing: 10
                        FlatButton {
                            text: "Rescan library"
                            icon: Icons.refresh
                            enabled: App.configured
                            onClicked: App.rescan()
                        }
                        FlatButton {
                            text: "Retry tracks without a video"
                            enabled: App.configured && !App.busy
                            onClicked: App.retryUnmatched()
                        }
                    }

                    // Tracks looked up again by today's rules, by folder or by file.
                    Item {
                        id: reimport
                        property string note: ""
                        function queued(n, path) {
                            note = n > 0 ? n + (n === 1 ? " track" : " tracks") + " queued from " + App.displayPath(path)
                                         : "No music file of your library is at " + App.displayPath(path)
                        }
                        width: parent.width
                        height: reimportText.implicitHeight + 34
                        Connections {
                            target: root
                            function onClosed() { reimport.note = "" }
                        }

                        Column {
                            id: reimportText
                            anchors.left: parent.left
                            anchors.right: reimportButtons.left
                            anchors.rightMargin: 20
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 6
                            spacing: 3
                            Text {
                                text: "Re-import"
                                color: Theme.text
                                font.pixelSize: 14
                            }
                            Text {
                                width: parent.width
                                text: reimport.note.length > 0 ? reimport.note
                                    : "Looks the tracks in a folder, or one file, up again by today's rules. A video that still fits is kept without a download; one that no longer does goes to review, or is replaced."
                                color: reimport.note.length > 0 ? Theme.text : Theme.textDim
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                        }
                        Row {
                            id: reimportButtons
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            spacing: 10
                            FlatButton {
                                text: "Folder…"
                                icon: Icons.folder
                                enabled: App.configured
                                onClicked: reimportFolderDialog.open()
                            }
                            FlatButton {
                                text: "File…"
                                enabled: App.configured
                                onClicked: reimportFileDialog.open()
                            }
                        }
                    }

                    // Videos whose tracks are in none of the music folders any more.
                    Item {
                        id: untracked
                        property bool confirming: false
                        visible: App.untrackedCount > 0
                        width: parent.width
                        height: untrackedText.implicitHeight + 34
                        Connections {
                            target: root
                            function onClosed() { untracked.confirming = false }
                        }

                        Column {
                            id: untrackedText
                            anchors.left: parent.left
                            anchors.right: untrackedButtons.left
                            anchors.rightMargin: 20
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 6
                            spacing: 3
                            Text {
                                text: App.untrackedCount + (App.untrackedCount === 1 ? " untracked video" : " untracked videos")
                                color: Theme.text
                                font.pixelSize: 14
                            }
                            Text {
                                width: parent.width
                                text: untracked.confirming
                                    ? "This deletes the files of " + App.untrackedCount + (App.untrackedCount === 1 ? " video" : " videos") + " for good."
                                    : "Their tracks are in none of your music folders any more. They stay in the library until deleted, and are taken up again if the tracks come back."
                                color: untracked.confirming ? "#ff5d5d" : Theme.textDim
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                        }
                        Row {
                            id: untrackedButtons
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            spacing: 10
                            FlatButton {
                                visible: untracked.confirming
                                text: "Keep"
                                onClicked: untracked.confirming = false
                            }
                            FlatButton {
                                text: untracked.confirming ? "Delete " + App.untrackedCount : "Delete untracked videos"
                                // Not while folders are read or tracks looked up: the count is still moving.
                                enabled: !App.busy
                                onClicked: {
                                    if (!untracked.confirming) { untracked.confirming = true; return }
                                    untracked.confirming = false
                                    App.deleteUntracked()
                                }
                            }
                        }
                    }

                    // Subtitles: fetched with each video in these languages.
                Item {
                    width: parent.width
                    height: subtitleText.implicitHeight + 34

                    Column {
                        id: subtitleText
                        anchors.left: parent.left
                        anchors.right: subtitleControls.left
                        anchors.rightMargin: 20
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 6
                        spacing: 3
                        Text {
                            text: "Subtitles"
                            color: Theme.text
                            font.pixelSize: 14
                        }
                        Text {
                            width: parent.width
                            text: App.fetchingSubtitles ? App.statusText
                                : "Languages to fetch with each video where its uploader provides them, like “en” or “en, ja”. Empty for none."
                            color: Theme.textDim
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }
                    Row {
                        id: subtitleControls
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 10
                        spacing: 10
                        Rectangle {
                            width: 84
                            height: 34
                            radius: Theme.radiusSmall
                            color: Theme.hover
                            border.width: 1
                            border.color: langField.activeFocus ? Theme.accent : "transparent"
                            TextInput {
                                id: langField
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                verticalAlignment: TextInput.AlignVCenter
                                clip: true
                                color: Theme.text
                                selectionColor: Theme.accent
                                selectedTextColor: Theme.accentInk
                                font.pixelSize: 13
                                selectByMouse: true
                                text: App.subtitleLangs
                                onEditingFinished: {
                                    App.subtitleLangs = text
                                    text = Qt.binding(() => App.subtitleLangs)
                                }
                            }
                        }
                        FlatButton {
                            text: App.fetchingSubtitles ? "Fetching…" : "Fetch for my videos"
                            enabled: App.configured && !App.busy && App.subtitleLangs.length > 0
                            onClicked: { langField.focus = false; App.fetchSubtitles() }
                        }
                    }
                }

                    SliderRow {
                        label: "Subtitle outline"
                        hint: "Thickness of the dark edge around plain subtitles. Subtitles with their own styling keep it."
                        from: 0
                        to: 5
                        value: App.subtitleOutline
                        valueText: App.subtitleOutline === 0 ? "Off" : App.subtitleOutline.toFixed(1)
                        onMoved: (v) => App.subtitleOutline = v
                    }
                    SliderRow {
                        label: "Subtitle drop shadow"
                        hint: "How far the shadow falls behind plain subtitles."
                        from: 0
                        to: 5
                        value: App.subtitleShadow
                        valueText: App.subtitleShadow === 0 ? "Off" : App.subtitleShadow.toFixed(1)
                        onMoved: (v) => App.subtitleShadow = v
                    }

                // YouTube Premium: the account's cookies bring audio at a higher bitrate.
                    Item {
                        width: parent.width
                        height: premiumText.implicitHeight + 34

                        Column {
                            id: premiumText
                            anchors.left: parent.left
                            anchors.right: premiumButtons.left
                            anchors.rightMargin: 20
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 6
                            spacing: 3
                            Text {
                                text: "YouTube Premium"
                                color: Theme.text
                                font.pixelSize: 14
                            }
                            Text {
                                width: parent.width
                                text: root.cookiesError.length > 0 ? root.cookiesError
                                    : App.checkingCookies ? "Asking YouTube about the cookies…"
                                    : App.cookiesStatus.length > 0 ? App.cookiesStatus
                                    : App.hasCookies ? "Cookies added " + App.cookiesAdded + ". New videos get the account's higher-bitrate audio."
                                    : "Add a cookies.txt exported from a browser signed in to a Premium account to get its higher-bitrate audio."
                                color: root.cookiesError.length > 0 || (!App.checkingCookies && (App.cookiesState === "expired" || App.cookiesState === "unknown")) ? "#ff5d5d"
                                    : !App.checkingCookies && App.cookiesState === "premium" ? Theme.accentHi : Theme.textDim
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                        }
                        Row {
                            id: premiumButtons
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            spacing: 10
                            FlatButton {
                                visible: App.hasCookies
                                text: "Remove"
                                onClicked: { root.cookiesError = ""; App.removeCookies() }
                            }
                            FlatButton {
                                text: App.hasCookies ? "Replace" : "Add cookies.txt"
                                onClicked: cookiesDialog.open()
                            }
                        }
                    }
                    Item {
                        visible: App.hasCookies
                        width: parent.width
                        height: 46
                        Row {
                            anchors.bottom: parent.bottom
                            spacing: 10
                            FlatButton {
                                text: App.checkingCookies ? "Checking cookies…" : "Check cookies"
                                enabled: !App.checkingCookies
                                onClicked: { root.cookiesError = ""; App.checkCookies() }
                            }
                            FlatButton {
                                text: App.checkingQuality ? "Checking for better quality…" : "Check videos for better quality"
                                enabled: App.configured && !App.checkingQuality
                                onClicked: App.checkQuality()
                            }
                        }
                    }
                    Item { visible: App.hasCookies; width: 1; height: 6 }
                    SwitchRow {
                        visible: App.hasCookies
                        label: "Sign in when YouTube refuses guests"
                        hint: "When YouTube asks a guest to “confirm you’re not a bot”, download with the account. Heavy use can get an account restricted."
                        checked: App.accountOnBotCheck
                        onToggled: (c) => App.accountOnBotCheck = c
                    }

                    // Windows only: elsewhere yt-dlp is updated with the system.
                    Item {
                        visible: YtDlpUpdater.supported
                        width: parent.width
                        height: 70

                        Column {
                            anchors.left: parent.left
                            anchors.right: ytdlpButton.left
                            anchors.rightMargin: 20
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 6
                            spacing: 3
                            Text {
                                text: YtDlpUpdater.version.length > 0 ? "yt-dlp " + YtDlpUpdater.version : "yt-dlp is not installed"
                                color: Theme.text
                                font.pixelSize: 14
                            }
                            Text {
                                width: parent.width
                                text: YtDlpUpdater.status.length > 0 ? YtDlpUpdater.status
                                      : "Finds and downloads the videos. Update it when imports start failing."
                                color: Theme.textDim
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                        }
                        FlatButton {
                            id: ytdlpButton
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            text: YtDlpUpdater.version.length > 0 ? "Update yt-dlp" : "Download yt-dlp"
                            primary: YtDlpUpdater.version.length === 0 && !YtDlpUpdater.busy
                            // The running copy cannot be replaced.
                            enabled: !YtDlpUpdater.busy && !App.busy
                            onClicked: YtDlpUpdater.update()
                        }
                    }
                }
            }
        }
    }
}
