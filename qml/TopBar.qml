import QtQuick
import QtQuick.Controls.Basic
import MvPlayer.Core

// Heading on the left; import activity, sort order and search on the right.
Item {
    id: root

    property bool playerView: false
    property var current: null        // video shown in the player, if any
    property alias searchText: searchField.text

    function focusSearch() {
        searchField.focusInput()
    }
    signal backRequested()

    implicitHeight: 64

    readonly property string browseTitle: {
        const t = App.videos.facetType
        if (t === "all") return "All Videos"
        if (t === "recent") return "Recently Added"
        return App.videos.facetValue
    }
    readonly property string browseSubtitle: {
        const n = App.videos.count
        const noun = n === 1 ? " video" : " videos"
        if (searchField.text.trim().length > 0)
            return n + noun + " matching “" + searchField.text.trim() + "”"
        return n + noun
    }

    // Back button, only in the player view
    IconButton {
        id: back
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        icon: Icons.back
        tooltip: "Back to library (Esc)"
        tooltipBelow: true
        opacity: root.playerView ? 1 : 0
        visible: opacity > 0
        onClicked: root.backRequested()
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    }

    // The two headings cross-fade and slide as the view changes.
    Item {
        id: headings
        anchors.left: parent.left
        anchors.leftMargin: root.playerView ? 62 : 28
        anchors.right: tools.left
        anchors.rightMargin: 24
        height: parent.height
        clip: true

        Behavior on anchors.leftMargin {
            NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
        }

        Column {
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: root.playerView ? -10 : 0
            width: parent.width
            spacing: 2
            opacity: root.playerView ? 0 : 1
            Behavior on opacity { NumberAnimation { duration: Theme.normal } }
            Behavior on anchors.verticalCenterOffset { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutCubic } }

            Text {
                width: parent.width
                text: root.browseTitle
                color: Theme.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
                font.letterSpacing: -0.3
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: root.browseSubtitle
                color: Theme.textFaint
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        Column {
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: root.playerView ? 0 : 10
            width: parent.width
            spacing: 3
            opacity: root.playerView ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.normal } }
            Behavior on anchors.verticalCenterOffset { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutCubic } }

            Text {
                width: parent.width
                text: root.current ? root.current.title : ""
                color: Theme.text
                font.pixelSize: 17
                font.weight: Font.DemiBold
                font.letterSpacing: -0.2
                elide: Text.ElideRight
            }
            Row {
                spacing: 8
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        const v = root.current
                        if (!v) return ""
                        const parts = [v.artist.replace(/; /g, ", ")]
                        if (v.album && v.album !== v.title) parts.push(v.album)
                        if (v.year > 0) parts.push(v.year)
                        return parts.join("  ·  ")
                    }
                    color: Theme.textDim
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, headings.width - chips.width - 8)
                }
                Row {
                    id: chips
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Repeater {
                        model: {
                            const v = root.current
                            if (!v) return []
                            const list = []
                            if (v.quality) list.push({ text: v.quality, strong: false })
                            if (v.audioSource === "library") list.push({ text: v.audioDetail, strong: true })
                            return list
                        }
                        Rectangle {
                            required property var modelData
                            width: chip.implicitWidth + 12
                            height: 18
                            radius: 4
                            color: modelData.strong ? Theme.accentSoft : Theme.hover
                            Text {
                                id: chip
                                anchors.centerIn: parent
                                text: parent.modelData.text
                                color: parent.modelData.strong ? Theme.accentHi : Theme.textDim
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.3
                            }
                        }
                    }
                }
            }
        }
    }

    Row {
        id: tools
        anchors.right: parent.right
        anchors.rightMargin: 24
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        ImportPill {
            id: pill
            anchors.verticalCenter: parent.verticalCenter
            open: importPanel.visible
            onClicked: importPanel.visible ? importPanel.close() : importPanel.open()

            ImportPanel {
                id: importPanel
                x: pill.width - width
                y: pill.height + 10
            }
        }

        IconButton {
            id: sortButton
            anchors.verticalCenter: parent.verticalCenter
            icon: Icons.sort
            checked: sortMenu.visible
            tooltip: sortMenu.visible ? "" : "Sort order"
            tooltipBelow: true
            enabled: App.videos.facetType !== "recent"
            onClicked: sortMenu.visible ? sortMenu.close() : sortMenu.open()

            Popup {
                id: sortMenu
                x: sortButton.width - width
                y: sortButton.height + 10
                width: 190
                padding: 6
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                transformOrigin: Popup.TopRight
                enter: Transition {
                    ParallelAnimation {
                        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
                        NumberAnimation { property: "scale"; from: 0.94; to: 1; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
                    }
                }
                exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: Theme.fast } }
                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.raised
                    border.width: 1
                    border.color: Theme.line
                }
                contentItem: Column {
                    Repeater {
                        model: [
                            { key: "title", label: "Title" },
                            { key: "artist", label: "Artist" },
                            { key: "year", label: "Newest release" },
                            { key: "added", label: "Recently added" }
                        ]
                        Item {
                            id: option
                            required property var modelData
                            readonly property bool current: App.videos.sortMode === modelData.key
                            width: sortMenu.availableWidth
                            height: 34
                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.radiusSmall
                                color: Theme.hover
                                opacity: optionMouse.containsMouse ? 1 : 0
                                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                            }
                            Text {
                                x: 12
                                anchors.verticalCenter: parent.verticalCenter
                                text: option.modelData.label
                                color: option.current ? Theme.accent : Theme.text
                                font.pixelSize: 13
                                font.weight: option.current ? Font.DemiBold : Font.Normal
                            }
                            Icon {
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                visible: option.current
                                path: Icons.check
                                size: 16
                                color: Theme.accent
                            }
                            MouseArea {
                                id: optionMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    App.videos.sortMode = option.modelData.key
                                    sortMenu.close()
                                }
                            }
                        }
                    }
                }
            }
        }

        SearchBox {
            id: searchField
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
