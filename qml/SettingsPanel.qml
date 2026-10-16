import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import MvPlayer.Core

// Library folders, looks, playback, import behaviour, the channels you
// allowed and denied, and the judge: one tab each.
Popup {
    id: root

    width: 560
    // As tall as its content, as far as the window allows: then it scrolls.
    height: Math.min(tabBar.y + tabBar.height + body.implicitHeight + 2, (Overlay.overlay ? Overlay.overlay.height : 800) - 32)
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

    // The tabs, by key; the one on show stays across openings.
    readonly property var tabs: [
        { key: "library", label: "Library" },
        { key: "appearance", label: "Appearance" },
        { key: "playback", label: "Playback" },
        { key: "import", label: "Import" },
        { key: "channels", label: "Channels" },
        { key: "judge", label: "Judge" }
    ]
    property string tab: "library"
    readonly property int tabIndex: Math.max(0, tabs.findIndex((t) => t.key === tab))
    function showTab(key) {
        if (!tabs.some((t) => t.key === key))
            return false
        if (key !== tab) {
            tab = key
            flick.contentY = 0
        }
        return true
    }

    readonly property var accentPresets: [
        "#8b7dff", "#5b9dff", "#3ec9d6", "#4fd68f", "#c5d94a", "#f5b84a", "#ff8f4d", "#ff6f91", "#f06bd8"
    ]
    readonly property bool customAccent: App.accent !== "auto" && accentPresets.indexOf(App.accent) < 0

    FolderDialog {
        id: musicDialog
        title: "Select the base directory of your music"
        currentFolder: App.pathToUrl(App.baseDir)
        onAccepted: App.baseDir = App.urlToPath(selectedFolder)
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
    property string keyError: ""
    onOpened: App.folderTree.refresh()
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

    // A heading with a line of explanation, and whatever sits on its right.
    component Lead: Item {
        id: lead
        property string label
        property string hint
        property color hintColor: Theme.textDim
        property Item side: null
        width: parent.width
        height: leadText.implicitHeight + 34
        Column {
            id: leadText
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.rightMargin: lead.side ? lead.side.width + 20 : 0
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 6
            spacing: 3
            Text {
                text: lead.label
                color: Theme.text
                font.pixelSize: 14
            }
            Text {
                width: parent.width
                text: lead.hint
                color: lead.hintColor
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }
    }

    component Divider: Column {
        width: parent.width
        Item { width: 1; height: 8 }
        Rectangle { width: parent.width; height: 1; color: Theme.line }
        Item { width: 1; height: 8 }
    }

    component Field: Rectangle {
        property alias input: field
        property alias text: field.text
        width: 84
        height: 34
        radius: Theme.radiusSmall
        color: Theme.hover
        border.width: 1
        border.color: field.activeFocus ? Theme.accent : "transparent"
        TextInput {
            id: field
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

        // The tabs, with a mark that slides to the one on show.
        Item {
            id: tabBar
            x: 26
            y: header.y + header.height + 6
            width: parent.width - 52
            height: 40
            Row {
                id: tabRow
                spacing: 4
                Repeater {
                    model: root.tabs
                    Item {
                        id: tabItem
                        required property var modelData
                        required property int index
                        readonly property bool current: index === root.tabIndex
                        width: tabLabel.implicitWidth + 24
                        height: tabBar.height - 2
                        Rectangle {
                            anchors.fill: parent
                            anchors.bottomMargin: 4
                            radius: Theme.radiusSmall
                            color: Theme.hover
                            opacity: tabMouse.containsMouse && !tabItem.current ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                        }
                        Text {
                            id: tabLabel
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: -2
                            text: tabItem.modelData.label
                            color: tabItem.current ? Theme.text : Theme.textDim
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            Behavior on color { ColorAnimation { duration: Theme.normal } }
                        }
                        MouseArea {
                            id: tabMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.showTab(tabItem.modelData.key)
                        }
                    }
                }
            }
            Rectangle {
                readonly property Item on: tabRow.children[root.tabIndex] || null
                x: on ? on.x + 12 : 0
                width: on ? on.width - 24 : 0
                y: parent.height - 2
                height: 2
                radius: 1
                color: Theme.accent
                Behavior on x { NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized } }
                Behavior on width { NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized } }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.line
            }
        }

        Flickable {
            id: flick
            anchors.fill: parent
            anchors.topMargin: tabBar.y + tabBar.height
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
                topPadding: 12
                bottomPadding: 22

                // ---- Library ------------------------------------------------
                Column {
                    width: parent.width - 2 * body.padding
                    visible: root.tab === "library"

                    // Music library: a base directory, and the folders ticked under it.
                    Item {
                        width: parent.width
                        height: 64
                        Icon {
                            id: musicIcon
                            anchors.verticalCenter: parent.verticalCenter
                            path: Icons.music
                            size: 20
                            color: Theme.textDim
                        }
                        Column {
                            anchors.left: musicIcon.right
                            anchors.leftMargin: 14
                            anchors.right: baseButton.left
                            anchors.rightMargin: 16
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                text: "Music library"
                                color: Theme.text
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                text: App.baseDir.length > 0 ? App.displayPath(App.baseDir) : "No base directory yet"
                                color: App.baseDir.length > 0 ? Theme.textDim : Theme.textFaint
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                            }
                        }
                        FlatButton {
                            id: baseButton
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Select base directory"
                            icon: Icons.folder
                            primary: App.baseDir.length === 0
                            onClicked: musicDialog.open()
                        }
                    }
                    FolderTree {
                        visible: App.baseDir.length > 0
                        width: parent.width
                        maxHeight: 320
                    }
                    Text {
                        visible: App.baseDir.length > 0
                        width: parent.width
                        topPadding: 8
                        wrapMode: Text.WordWrap
                        lineHeight: 1.3
                        text: App.musicDirs.length === 0
                              ? "Every music file under the base is read for the player. Tick the folders whose tracks are looked up for videos; none is until one is ticked."
                              : "Every music file under the base is read for the player. The tracks in the ticked folders are looked up for videos, everything under each included. A folder ticked off keeps its videos."
                        color: Theme.textFaint
                        font.pixelSize: 12
                    }
                    Item { width: 1; height: 10 }
                    FolderRow {
                        label: "Music video library"
                        hint: "Not set"
                        icon: Icons.movie
                        path: App.mvDir
                        onChange: mvDialog.open()
                    }

                    Divider {}

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

                // ---- Appearance ---------------------------------------------
                Column {
                    width: parent.width - 2 * body.padding
                    visible: root.tab === "appearance"

                    // Follow the system, or force dark / light
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
                    Item { width: 1; height: 8 }
                }

                // ---- Playback -----------------------------------------------
                Column {
                    width: parent.width - 2 * body.padding
                    visible: root.tab === "playback"

                    SwitchRow {
                        label: "Enlarge a video when it starts"
                        hint: "Off: a video starts in its thumbnail, and a click on it enlarges it."
                        checked: App.autoExpand
                        onToggled: (c) => App.autoExpand = c
                    }

                    Divider {}

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
                            Field {
                                id: langField
                                text: App.subtitleLangs
                                input.onEditingFinished: {
                                    App.subtitleLangs = text
                                    text = Qt.binding(() => App.subtitleLangs)
                                }
                            }
                            FlatButton {
                                text: App.fetchingSubtitles ? "Fetching…" : "Fetch for my videos"
                                enabled: App.configured && !App.busy && App.subtitleLangs.length > 0
                                onClicked: { langField.input.focus = false; App.fetchSubtitles() }
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
                }

                // ---- Import -------------------------------------------------
                Column {
                    width: parent.width - 2 * body.padding
                    visible: root.tab === "import"

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
                    // Somebody's re-upload of a song comes from a small channel.
                    Item {
                        width: parent.width
                        height: 76
                        Column {
                            anchors.left: parent.left
                            anchors.right: smallChannels.left
                            anchors.rightMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                text: "Ignore small channels"
                                color: Theme.text
                                font.pixelSize: 14
                            }
                            Text {
                                width: parent.width
                                text: "Uploads from channels with fewer subscribers than this are not taken. Verified channels, channels you allowed, and uploads listed under several channels, are."
                                color: Theme.textFaint
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                        }
                        Row {
                            id: smallChannels
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 12
                            Field {
                                anchors.verticalCenter: parent.verticalCenter
                                opacity: App.ignoreSmallChannels ? 1 : 0.45
                                input.horizontalAlignment: TextInput.AlignRight
                                input.enabled: App.ignoreSmallChannels
                                input.font.features: { "tnum": 1 }
                                input.inputMethodHints: Qt.ImhDigitsOnly
                                input.validator: IntValidator { bottom: 1; top: 1000000000 }
                                text: App.minSubscribers
                                input.onEditingFinished: {
                                    App.minSubscribers = parseInt(text) || App.minSubscribers
                                    text = Qt.binding(() => App.minSubscribers)
                                    input.focus = false
                                }
                            }
                            FlatSwitch {
                                anchors.verticalCenter: parent.verticalCenter
                                checked: App.ignoreSmallChannels
                                onToggled: (c) => App.ignoreSmallChannels = c
                            }
                        }
                    }

                    Divider {}

                    // Keeping everything a lookup touches, for a replay (README).
                    Item {
                        width: parent.width
                        height: keepText.implicitHeight + 24
                        Column {
                            id: keepText
                            anchors.left: parent.left
                            anchors.right: keepSwitch.left
                            anchors.rightMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                text: "Keep everything for a replay"
                                color: Theme.text
                                font.pixelSize: 14
                            }
                            Text {
                                width: parent.width
                                text: "Every search YouTube answers, every page, audio, preview and video a lookup brings, and every question to the judge with its answer are kept for good under the videos folder (.mvplayer/capture), so that a lookup can be run again later exactly as it went, or with new words for the judge. Takes room: tens of megabytes a lookup."
                                color: Theme.textFaint
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                            Text {
                                id: captureSize
                                width: parent.width
                                function refresh() { text = "Kept so far: " + App.captureSizeText() }
                                Component.onCompleted: refresh()
                                color: Theme.textFaint
                                font.pixelSize: 12
                                Connections {
                                    target: App
                                    function onSettingsChanged() { captureSize.refresh() }
                                }
                            }
                        }
                        FlatSwitch {
                            id: keepSwitch
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            checked: App.keepEverything
                            onToggled: (c) => App.keepEverything = c
                        }
                    }

                    Divider {}

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
                }

                // ---- Channels -----------------------------------------------
                Column {
                    width: parent.width - 2 * body.padding
                    visible: root.tab === "channels"
                    spacing: 3

                    Item { width: 1; height: 8 }
                    Text {
                        text: "Channels you allowed and denied"
                        color: Theme.text
                        font.pixelSize: 14
                    }
                    Text {
                        width: parent.width
                        text: "An allowed channel is an artist's channel for every track and is never ignored as a small one. A denied channel's uploads are never taken. Taken off here, a channel is judged like any other again; nothing in your library changes with that."
                        color: Theme.textFaint
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                    Item { width: 1; height: 8 }
                    Text {
                        visible: App.listedChannels.length === 0
                        width: parent.width
                        text: "None yet. On a video under review, rest the pointer on ✓ or ✗ for a second: a second button comes up for its channel as a whole."
                        color: Theme.textDim
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                    Repeater {
                        model: App.listedChannels
                        Rectangle {
                            id: listed
                            required property var modelData
                            width: parent.width
                            height: 34
                            radius: Theme.radiusSmall
                            color: Theme.hover
                            Icon {
                                id: listedIcon
                                x: 10
                                anchors.verticalCenter: parent.verticalCenter
                                path: listed.modelData.list === "allow" ? Icons.check : Icons.close
                                size: 15
                                color: listed.modelData.list === "allow" ? "#3fbf7f" : "#ff5d5d"
                            }
                            Text {
                                anchors.left: listedIcon.right
                                anchors.leftMargin: 8
                                anchors.right: unlist.left
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: (listed.modelData.name || listed.modelData.id)
                                      + (listed.modelData.handle ? "  ·  " + listed.modelData.handle : "")
                                      + (listed.modelData.list === "allow" ? "  ·  allowed" : "  ·  denied")
                                color: Theme.text
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                            IconButton {
                                id: unlist
                                anchors.right: parent.right
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                icon: Icons.close
                                iconSize: 14
                                tooltip: "Take it off the list"
                                onClicked: App.unlistChannel(listed.modelData.id)
                            }
                        }
                    }
                    Item { width: 1; height: 8 }
                }

                // ---- Judge --------------------------------------------------
                Column {
                    width: parent.width - 2 * body.padding
                    visible: root.tab === "judge"

                    Column {
                        width: parent.width
                        spacing: 10
                        topPadding: 16
                        Text { text: "Judge provider"; color: Theme.text; font.pixelSize: 14 }
                        Row {
                            spacing: 10
                            FlatButton { text: "Claude API key"; primary: App.judgeProvider === "claude"; onClicked: App.judgeProvider = "claude" }
                            FlatButton { text: "ChatGPT account"; primary: App.judgeProvider === "chatgpt"; onClicked: App.judgeProvider = "chatgpt" }
                        }
                    }
                    Column {
                        width: parent.width
                        visible: App.judgeProvider === "chatgpt"
                        spacing: 10
                        topPadding: 16
                        Text {
                            width: parent.width
                            text: App.chatGpt.status || (App.chatGpt.signedIn
                                ? "Signed in. Judge requests use your ChatGPT plan."
                                : "Sign in with your ChatGPT account and allow MV Player to use your plan for judge answers.")
                            color: Theme.textDim
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                        Column {
                            width: parent.width
                            spacing: 6
                            Repeater {
                                model: App.chatGpt.accounts
                                FlatButton {
                                    required property var modelData
                                    text: modelData.label + (modelData.signedIn ? "" : " · signed out")
                                    primary: App.chatGpt.account === modelData.id
                                    enabled: !App.chatGpt.busy
                                    onClicked: { App.chatGpt.selectAccount(modelData.id); App.chatGpt.refreshModels() }
                                }
                            }
                        }
                        Flow {
                            width: parent.width
                            spacing: 10
                            FlatButton {
                                text: "Continue with ChatGPT"
                                enabled: !App.chatGpt.busy
                                onClicked: App.chatGpt.signIn(App.chatGpt.account)
                            }
                            FlatButton {
                                text: "Add another account"
                                visible: App.chatGpt.accounts.length > 0
                                enabled: !App.chatGpt.busy
                                onClicked: App.chatGpt.signIn("")
                            }
                            FlatButton {
                                text: "Cancel sign-in"
                                visible: App.chatGpt.signingIn
                                onClicked: App.chatGpt.cancelSignIn()
                            }
                            FlatButton {
                                text: "Sign out"
                                visible: App.chatGpt.signedIn
                                enabled: !App.chatGpt.busy
                                onClicked: App.chatGpt.signOut()
                            }
                            FlatButton {
                                text: "Refresh models"
                                visible: App.chatGpt.signedIn
                                enabled: !App.chatGpt.busy
                                onClicked: App.chatGpt.refreshModels()
                            }
                            FlatButton {
                                text: "ChatGPT usage"
                                onClicked: Qt.openUrlExternally("https://chatgpt.com/settings/usage")
                            }
                        }
                    }

                    // Claude, through the API: the key, and whether it works.
                    Item {
                        width: parent.width
                        visible: App.judgeProvider === "claude"
                        height: visible ? keyText.implicitHeight + 34 : 0
                        Column {
                            id: keyText
                            anchors.left: parent.left
                            anchors.right: keyButtons.left
                            anchors.rightMargin: 20
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 6
                            spacing: 3
                            Text {
                                text: "Claude API key"
                                color: Theme.text
                                font.pixelSize: 14
                            }
                            Text {
                                width: parent.width
                                text: root.keyError.length > 0 ? root.keyError
                                    : App.judgeChecking ? "Asking the API about the key…"
                                    : App.judgeStatus.length > 0 ? App.judgeStatus
                                    : App.judgeKeySet ? "Key " + App.judgeKeyHint + " is stored, for this user only."
                                    : "Videos under review can be judged by Claude. Paste a key from console.anthropic.com; it is stored for this user only and used for nothing else."
                                color: root.keyError.length > 0 || (!App.judgeChecking && (App.judgeState === "bad" || App.judgeState === "error")) ? "#ff5d5d"
                                    : !App.judgeChecking && App.judgeState === "ok" ? Theme.accentHi : Theme.textDim
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                        }
                        Row {
                            id: keyButtons
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            spacing: 10
                            FlatButton {
                                visible: App.judgeKeySet
                                text: "Remove"
                                onClicked: { root.keyError = ""; App.removeJudgeKey() }
                            }
                            FlatButton {
                                visible: App.judgeKeySet
                                text: App.judgeChecking ? "Checking…" : "Check key"
                                enabled: !App.judgeChecking
                                onClicked: { root.keyError = ""; App.checkJudgeKey() }
                            }
                        }
                    }
                    // A new key, or another in place of the one stored.
                    Item {
                        width: parent.width
                        visible: App.judgeProvider === "claude"
                        height: visible ? 46 : 0
                        Row {
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 6
                            spacing: 10
                            Field {
                                id: keyField
                                width: 330
                                input.echoMode: TextInput.Password
                                input.passwordMaskDelay: 0
                                input.onAccepted: keyField.store()
                                function store() {
                                    root.keyError = App.setJudgeKey(text)
                                    if (root.keyError.length === 0) {
                                        text = ""
                                        input.focus = false
                                    }
                                }
                                Text {
                                    x: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: keyField.text.length === 0
                                    text: App.judgeKeySet ? "Paste another key" : "sk-ant-…"
                                    color: Theme.textFaint
                                    font.pixelSize: 13
                                }
                            }
                            FlatButton {
                                text: App.judgeKeySet ? "Replace" : "Add key"
                                primary: !App.judgeKeySet
                                enabled: keyField.text.length > 0
                                onClicked: keyField.store()
                            }
                        }
                    }

                    Divider {}

                    // What the judge does (docs/import-rules-judge.html, X4):
                    // nothing, advice on cards under review, or the lookup itself.
                    Column {
                        width: parent.width
                        spacing: 8
                        Text {
                            text: "What the judge does"
                            color: Theme.text
                            font.pixelSize: 14
                        }
                        Text {
                            width: parent.width
                            text: App.judgeMode === "judge"
                                ? "Judge: it runs each lookup — writes the search, picks what to listen to, and gives the verdict. A verdict it is at least 85% sure of stands by itself; anything less, a cut, an interrupted video or one the audio check doubts is left for you, with its advice on the card."
                                : App.judgeMode === "advice"
                                ? "Advice: lookups run by the rules as before; each card under review shows whether the judge would keep the video for the candidate on show, and how sure it is. Nothing is decided by it."
                                : "Off: the rules decide, and cards carry no advice."
                            color: Theme.textDim
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                        Row {
                            spacing: 10
                            Repeater {
                                model: [{ key: "off", label: "Off" }, { key: "advice", label: "Advice" }, { key: "judge", label: "Judge" }]
                                FlatButton {
                                    text: modelData.label
                                    primary: App.judgeMode === modelData.key
                                    enabled: App.judgeReady || modelData.key === "off"
                                    onClicked: App.judgeMode = modelData.key
                                }
                            }
                        }
                        Text {
                            visible: !App.judgeReady
                            text: App.judgeProvider === "chatgpt" ? "Sign in and choose an available model first." : "Add a key first."
                            color: Theme.textFaint
                            font.pixelSize: 12
                        }
                    }

                    Divider {}

                    Column {
                        width: parent.width
                        spacing: 8
                        Text {
                            text: "Which model"
                            color: Theme.text
                            font.pixelSize: 14
                        }
                        Text {
                            width: parent.width
                            text: App.judgeProvider === "chatgpt" ? "Models available to the selected ChatGPT account. Usage counts toward your ChatGPT plan; these models have not been measured against your verdicts here."
                                : "Sonnet 5.5 agreed with your own verdicts on 87% of the review queue it was measured on, Opus 5.5 on 89%, Haiku 5.5 on 73%; what each costs is the price of a verdict with four uploads and their frames."
                            color: Theme.textDim
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                        Flow {
                            width: parent.width
                            spacing: 10
                            Repeater {
                                model: App.judgeModels
                                FlatButton {
                                    text: modelData.label + (modelData.cost ? " · " + modelData.cost : "")
                                    primary: App.judgeModel === modelData.name
                                    onClicked: App.judgeModel = modelData.name
                                }
                            }
                        }
                        Text {
                            topPadding: 8
                            text: "How many questions at once"
                            color: Theme.text
                            font.pixelSize: 14
                        }
                        Text {
                            width: parent.width
                            text: "For the advice on cards (\"Review all\" and the panel's own questions). A question is half waiting for the API and half this machine's work, so three at once run about three times as fast; the key's rate limit is the ceiling. One unless you raise it."
                            color: Theme.textDim
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                        Flow {
                            width: parent.width
                            spacing: 10
                            Repeater {
                                model: [1, 2, 3, 4, 6, 8]
                                FlatButton {
                                    text: "" + modelData
                                    primary: App.judgeThreads === modelData
                                    onClicked: App.judgeThreads = modelData
                                }
                            }
                        }
                        Text {
                            visible: App.judgeAlone + App.judgeLeft > 0
                            text: "In judge mode so far: " + App.judgeAlone + (App.judgeAlone === 1 ? " lookup" : " lookups") + " settled by the judge alone, " + App.judgeLeft + " left for your verdict."
                            color: Theme.textDim
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }
                    }
                    Item { width: 1; height: 8 }
                }
            }
        }
    }
}
