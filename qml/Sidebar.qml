import QtQuick
import QtQuick.Controls.Basic
import MvPlayer.Core

// Library navigation: fixed entries, then the values of one tag at a time.
// A selector picks which tag is listed and a filter box narrows the list.
// In the review queue and among the orphans the same entries narrow those,
// and the tags listed are those of the videos there.
Rectangle {
    id: root

    signal settingsRequested()
    signal navigated()

    color: Theme.surface

    function select(type, value) {
        App.videos.setFacet(type, value)
        root.navigated()
    }

    // A right click on a row: its videos looked up again by today's rules.
    function askReimport(item, type, value, count) {
        const p = item ? item.mapToItem(root, 0, item.height) : Qt.point(0, fixed.y + fixed.height)
        rowMenu.facetType = type
        rowMenu.facetValue = value
        rowMenu.count = count
        rowMenu.x = Math.min(p.x + 20, root.width - rowMenu.width - 8)
        rowMenu.y = p.y - 4
        rowMenu.open()
    }

    Popup {
        id: rowMenu
        property string facetType: "all"
        property string facetValue: ""
        property int count: 0
        width: 232
        padding: 6
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        transformOrigin: Popup.TopLeft
        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
                NumberAnimation { property: "scale"; from: 0.95; to: 1; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
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
            Item {
                width: rowMenu.availableWidth
                height: 34
                enabled: App.configured && rowMenu.count > 0
                opacity: enabled ? 1 : 0.4
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusSmall
                    color: Theme.hover
                    opacity: reimportMouse.containsMouse ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                }
                Icon {
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    path: Icons.refresh
                    size: 16
                    color: Theme.textDim
                }
                Text {
                    x: 34
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Re-import " + rowMenu.count + (rowMenu.count === 1 ? " video" : " videos")
                    color: Theme.text
                    font.pixelSize: 13
                }
                MouseArea {
                    id: reimportMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        App.reimportFacet(rowMenu.facetType, rowMenu.facetValue)
                        rowMenu.close()
                    }
                }
            }
            Text {
                width: rowMenu.availableWidth
                leftPadding: 10
                rightPadding: 10
                topPadding: 4
                bottomPadding: 6
                text: "Each is looked up again by today's rules: kept, sent to review or replaced as they decide."
                color: Theme.textFaint
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
    }

    // The tag whose values are listed. Follows App.sidebarFacet, but only
    // switches in the middle of the cross-fade below.
    property string shownKey: App.sidebarFacet
    // Likewise the section of the grid whose values are listed.
    property string shownSection: App.videos.section
    readonly property var facet: {
        const all = App.facets
        for (let i = 0; i < all.length; ++i) {
            if (all[i].key === shownKey)
                return all[i]
        }
        return all.length > 0 ? all[0] : { key: "", title: "", items: {} }
    }
    readonly property var visibleItems: {
        const q = fold(filter.text.trim())
        const items = facet.items[shownSection] || []
        if (q.length === 0)
            return items
        return items.filter((item) => fold(item.name).indexOf(q) >= 0)
    }
    function fold(s) {
        return s.normalize("NFKC").toLowerCase()
    }

    Connections {
        target: App
        function onAppearanceChanged() {
            if (App.sidebarFacet !== root.shownKey)
                swap.restart()
        }
        // The list is rebuilt whenever a video is imported; stay where we were.
        function onFacetsChanged() {
            const y = list.contentY
            Qt.callLater(() => { list.contentY = Math.max(0, Math.min(y, list.contentHeight - list.height)) })
        }
    }
    Connections {
        target: App.videos
        function onSectionChanged() {
            if (App.videos.section !== root.shownSection)
                swap.restart()
        }
    }
    SequentialAnimation {
        id: swap
        NumberAnimation { target: listArea; property: "opacity"; to: 0; duration: 90 }
        ScriptAction {
            script: {
                if (root.shownKey !== App.sidebarFacet)
                    filter.clear()
                root.shownKey = App.sidebarFacet
                root.shownSection = App.videos.section
                list.contentY = 0
            }
        }
        ParallelAnimation {
            NumberAnimation { target: listArea; property: "opacity"; to: 1; duration: Theme.normal }
            NumberAnimation { target: listShift; property: "y"; from: 10; to: 0; duration: Theme.slow; easing.type: Easing.OutCubic }
        }
    }

    // Wordmark
    Item {
        id: header
        width: parent.width
        height: 64

        WindowDrag { anchors.fill: parent }

        Rectangle {
            id: logo
            x: 20
            anchors.verticalCenter: parent.verticalCenter
            width: 28
            height: 28
            radius: 8
            color: Theme.accent
            Icon {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: 1
                path: Icons.play
                size: 18
                color: Theme.accentInk
            }
        }
        Text {
            anchors.left: logo.right
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: "MV Player"
            color: Theme.text
            font.pixelSize: 16
            font.weight: Font.DemiBold
            font.letterSpacing: -0.2
        }
    }

    Column {
        id: fixed
        anchors.top: header.bottom
        width: parent.width

        SidebarItem {
            id: allRow
            label: "All Videos"
            icon: Icons.grid
            count: App.videos.section === "review" ? App.reviewCount : App.videos.section === "orphans" ? App.orphanCount : App.videoCount
            selected: App.videos.facetType === "all"
            onClicked: root.select("all", "")
            onRightClicked: root.askReimport(allRow, "all", "", count)
        }
        SidebarItem {
            label: "Recently Added"
            icon: Icons.clock
            selected: App.videos.facetType === "recent"
            onClicked: root.select("recent", "")
        }

        Item { width: 1; height: 14 }

        // Tag selector
        Item {
            id: selector
            width: parent.width
            height: 36

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                radius: Theme.radiusSmall
                color: menu.visible || selectorMouse.containsMouse ? Theme.hover : Theme.raised
                Behavior on color { ColorAnimation { duration: Theme.fast } }
            }
            Text {
                x: 24
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    const all = App.facets
                    for (let i = 0; i < all.length; ++i) {
                        if (all[i].key === App.sidebarFacet)
                            return all[i].title
                    }
                    return ""
                }
                color: Theme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            Icon {
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                path: Icons.chevronDown
                size: 18
                color: Theme.textDim
                rotation: menu.visible ? 180 : 0
                Behavior on rotation { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
            }
            MouseArea {
                id: selectorMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: menu.visible ? menu.close() : menu.open()
            }

            Popup {
                id: menu
                x: 12
                y: selector.height + 6
                width: selector.width - 24
                padding: 6
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                transformOrigin: Popup.Top
                enter: Transition {
                    ParallelAnimation {
                        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
                        NumberAnimation { property: "scale"; from: 0.95; to: 1; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
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
                        model: App.facets
                        Item {
                            id: option
                            required property var modelData
                            readonly property bool current: App.sidebarFacet === modelData.key
                            width: menu.availableWidth
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
                                text: option.modelData.title
                                color: option.current ? Theme.accentHi : Theme.text
                                font.pixelSize: 13
                                font.weight: option.current ? Font.DemiBold : Font.Normal
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                text: (option.modelData.items[App.videos.section] || []).length
                                color: option.current ? Theme.accent : Theme.textFaint
                                font.pixelSize: 11
                                font.features: { "tnum": 1 }
                            }
                            MouseArea {
                                id: optionMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    App.sidebarFacet = option.modelData.key
                                    menu.close()
                                }
                            }
                        }
                    }
                }
            }
        }

        Item { width: 1; height: 8 }

        // Filter for the listed tag values
        Item {
            width: parent.width
            height: 32
            SearchBox {
                id: filter
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                placeholder: "Filter " + root.facet.title.toLowerCase()
            }
        }

        Item { width: 1; height: 8 }
    }

    Item {
        id: listArea
        anchors.top: fixed.bottom
        anchors.bottom: footer.top
        width: parent.width
        clip: true

        ListView {
            id: list
            width: parent.width
            height: parent.height
            transform: Translate { id: listShift }
            model: root.visibleItems
            boundsBehavior: Flickable.StopAtBounds
            bottomMargin: 10

            delegate: SidebarItem {
                id: row
                required property var modelData
                label: modelData.name
                count: modelData.count
                selected: App.videos.facetType === root.shownKey && App.videos.facetValue === modelData.name
                onClicked: root.select(root.shownKey, modelData.name)
                onRightClicked: root.askReimport(row, root.shownKey, modelData.name, modelData.count)
            }

            KineticWheel {
                id: wheel
                view: list
                touchpadGain: App.touchpadGain
                wheelStep: App.wheelStep * 0.6
                deceleration: App.flickDeceleration
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 14
            visible: list.count === 0 && filter.text.trim().length > 0
            text: "No match"
            color: Theme.textFaint
            font.pixelSize: 12
        }

        // Thin scroll indicator
        Rectangle {
            visible: list.contentHeight > list.height
            anchors.right: parent.right
            anchors.rightMargin: 2
            y: list.visibleArea.yPosition * list.height
            width: 3
            height: Math.max(24, list.visibleArea.heightRatio * list.height)
            radius: 1.5
            color: Theme.textFaint
            opacity: list.moving || wheel.active ? 0.7 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.slow } }
        }
    }

    Item {
        id: footer
        anchors.bottom: parent.bottom
        width: parent.width
        height: 50

        Rectangle { width: parent.width; height: 1; color: Theme.line }
        SidebarItem {
            anchors.verticalCenter: parent.verticalCenter
            label: "Settings"
            icon: Icons.settings
            onClicked: root.settingsRequested()
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.line
    }
}
