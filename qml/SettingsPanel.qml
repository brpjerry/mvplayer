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

    FileDialog {
        id: cookiesDialog
        title: "Choose the cookies.txt exported from your browser"
        nameFilters: ["Cookie files (*.txt)", "All files (*)"]
        onAccepted: root.cookiesError = App.importCookies(App.urlToPath(selectedFile))
    }
    property string cookiesError: ""

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
