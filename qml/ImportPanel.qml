import QtQuick
import QtQuick.Controls.Basic
import MvPlayer.Core

// Drop-down showing what the importer is doing and which tracks have no video.
Popup {
    id: root

    width: 420
    // Tall enough for its content, up to most of the window.
    height: Math.min(Math.max(240, Overlay.overlay ? Overlay.overlay.height - 190 : 560), list.contentHeight + 2)
    padding: 1
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

    // Tracks without a video: thousands in a large library. The list makes
    // rows only for what is on screen, and while open is read again only when
    // their number changes (the importer reports activity many times a track),
    // keeping the reader's place.
    property var unmatched: []
    property string unmatchedKey: ""
    function countsKey() {
        const c = App.trackCounts
        return [c.not_found || 0, c.failed || 0, c.skipped || 0].join("/")
    }
    function refresh() {
        unmatchedKey = countsKey()
        unmatched = App.unmatchedTracks()
        list.returnToBounds()
    }
    onAboutToShow: {
        refresh()
        list.contentY = list.originY // each opening starts at the top
    }
    Connections {
        target: App
        enabled: root.visible
        function onActivityChanged() {
            if (root.countsKey() !== root.unmatchedKey)
                root.refresh()
        }
    }

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal }
            NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; to: 0; duration: Theme.fast }
            NumberAnimation { property: "scale"; to: 0.98; duration: Theme.fast }
        }
    }
    transformOrigin: Popup.TopRight

    background: Rectangle {
        radius: Theme.radius + 2
        color: Theme.raised
        border.width: 1
        border.color: Theme.line
    }

    contentItem: ListView {
        id: list
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.unmatched

        // The header grows upwards as jobs come and go: if its top was in
        // view, it stays in view.
        property bool atTop: true
        onContentYChanged: atTop = contentY <= originY + 1
        onOriginYChanged: if (atTop) contentY = originY

        KineticWheel {
            view: list
            touchpadGain: App.touchpadGain
            wheelStep: App.wheelStep * 0.6
            deceleration: App.flickDeceleration
        }

        header: Column {
            width: list.width
            topPadding: 16

            // Header
            Item {
                width: parent.width
                height: 34
                Text {
                    x: 18
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Library import"
                    color: Theme.text
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    IconButton {
                        icon: Icons.refresh
                        size: 30
                        iconSize: 17
                        tooltip: "Rescan music library"
                        tooltipBelow: true
                        onClicked: App.rescan()
                    }
                }
            }

            // Summary
            Text {
                x: 18
                width: parent.width - 36
                wrapMode: Text.WordWrap
                color: Theme.textDim
                font.pixelSize: 12
                lineHeight: 1.3
                text: {
                    const c = App.trackCounts
                    const total = c.total || 0
                    if (!App.configured)
                        return "No music library selected."
                    if (total === 0)
                        return App.scanning ? "Scanning your music folders…" : "No tracks found in your music folders."
                    const parts = [(c.done || 0) + " of " + total + " tracks have a video"]
                    if (c.pending) parts.push(c.pending + " waiting")
                    if (c.not_found) parts.push(c.not_found + " without a video")
                    if (c.skipped) parts.push(c.skipped + " skipped")
                    if (c.failed) parts.push(c.failed + " failed")
                    return parts.join(" · ")
                }
            }

            // Paused by the circuit breaker
            Item {
                width: parent.width
                height: App.importPaused ? pauseBox.height + 14 : 0
                visible: App.importPaused
                Rectangle {
                    id: pauseBox
                    x: 14
                    y: 12
                    width: parent.width - 28
                    height: pauseText.implicitHeight + 46
                    radius: Theme.radiusSmall
                    color: Theme.hover
                    Text {
                        id: pauseText
                        x: 12
                        y: 10
                        width: parent.width - 24
                        wrapMode: Text.WordWrap
                        maximumLineCount: 6
                        elide: Text.ElideRight
                        lineHeight: 1.25
                        color: Theme.textDim
                        font.pixelSize: 12
                        // What YouTube asks of a guest here is a sign-in:
                        // nothing to solve, and the user's to give.
                        text: App.statusText + (App.pauseKind === "signin"
                                ? ". YouTube serves this connection no more videos without a sign-in. Importing can carry on with your account; YouTube may restrict an account that downloads a great deal.\n"
                            : App.pauseKind === "cookies"
                                ? ". YouTube serves this connection no more videos without a sign-in, and " + (App.hasCookies
                                    ? "your account's cookies have expired. Export cookies.txt again and replace it in Settings.\n"
                                    : "no account is set up. Add your account's cookies.txt in Settings to carry on signed in.\n")
                                : ". YouTube is refusing requests, so importing waits instead of failing track after track. It resumes by itself.\n")
                            + App.pauseReason
                    }
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 6
                        spacing: 8
                        FlatButton {
                            visible: App.pauseKind === "signin"
                            implicitHeight: 26
                            text: "Carry on signed in"
                            onClicked: App.accountOnBotCheck = true
                        }
                        FlatButton {
                            implicitHeight: 26
                            text: "Resume now"
                            onClicked: App.resumeImport()
                        }
                    }
                }
            }

            Item { width: 1; height: 10 }

            // Active and recent jobs
            Repeater {
                model: App.jobs
                Item {
                    id: job
                    required property string title
                    required property string artist
                    required property string stage
                    required property real progress
                    required property bool finished
                    required property string outcome
                    required property string detail
                    width: list.width
                    height: 50

                    Item {
                        id: state
                        x: 18
                        width: 18
                        height: 18
                        anchors.verticalCenter: parent.verticalCenter
                        Spinner { anchors.centerIn: parent; size: 16; visible: !job.finished }
                        Icon {
                            anchors.centerIn: parent
                            visible: job.finished
                            size: 17
                            path: job.outcome === "done" ? Icons.check : job.outcome === "failed" ? Icons.close : Icons.info
                            color: job.outcome === "done" ? Theme.good : job.outcome === "failed" ? Theme.bad : Theme.textFaint
                        }
                    }
                    Column {
                        anchors.left: state.right
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 18
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text {
                            width: parent.width
                            text: job.title + (job.artist ? "  ·  " + job.artist : "")
                            color: Theme.text
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            visible: job.finished || job.progress < 0
                            text: job.finished && job.outcome !== "done" && job.detail ? job.stage + " — " + job.detail : job.stage
                            color: Theme.textFaint
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        // Download progress
                        Item {
                            width: parent.width
                            height: 14
                            visible: !job.finished && job.progress >= 0
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 44
                                height: 3
                                radius: 1.5
                                color: Theme.pressed
                                Rectangle {
                                    width: parent.width * Math.max(0, Math.min(1, job.progress))
                                    height: parent.height
                                    radius: 1.5
                                    color: Theme.accent
                                    Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
                                }
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: Math.round(Math.max(0, job.progress) * 100) + "%"
                                color: Theme.textFaint
                                font.pixelSize: 11
                                font.features: { "tnum": 1 }
                            }
                        }
                    }
                }
            }

            // Tracks without a video
            Item {
                width: parent.width
                height: 40
                visible: root.unmatched.length > 0
                Text {
                    x: 18
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 8
                    text: "NO VIDEO"
                    color: Theme.textFaint
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }
                FlatButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                    implicitHeight: 28
                    text: "Try again"
                    enabled: !App.busy
                    onClicked: App.retryUnmatched()
                }
            }
        }

        delegate: Item {
            id: row
            required property var modelData
            width: list.width
            height: 42
            Column {
                x: 18
                width: parent.width - 36
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                Text {
                    width: parent.width
                    text: row.modelData.title + (row.modelData.artist ? "  ·  " + row.modelData.artist : "")
                    color: Theme.textDim
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: (row.modelData.state === "skipped" ? "Skipped: " : row.modelData.state === "failed" ? "Failed: " : "")
                          + (row.modelData.message || "no matching video")
                    color: Theme.textFaint
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
        }

        footer: Item { width: 1; height: 14 }
    }
}
