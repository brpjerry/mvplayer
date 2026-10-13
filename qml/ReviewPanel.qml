import QtQuick
import MvPlayer.Core

// Beside the sidebar while the review queue is on show: what the control
// bar's popup shows elsewhere, as a panel. It is about the video under
// review that plays, or until one does, the first in the queue: the
// candidate track on show, with the arrows between candidates and the
// "+N" of one that is a candidate elsewhere too, its music file with every
// tag, and the video's page on YouTube.
Rectangle {
    id: root

    // On show: the review queue is, and the window is not fullscreen.
    property bool active: false
    readonly property int panelWidth: 320
    // The video that plays, if any (a video map).
    property var playing: null

    signal replaceRequested(var videoId)
    signal revealRequested(var videoId)
    // The playing video's candidates are stepped through by the control
    // bar, whose audio goes with them.
    signal stepRequested(int delta)

    width: active ? panelWidth : 0
    Behavior on width { NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized } }
    visible: width > 0
    clip: true
    color: Theme.surface

    // ---- Which video ------------------------------------------------------
    readonly property var playingId: playing !== null && playing.review === true ? playing.videoId : -1
    property var firstId: -1
    readonly property var videoId: playingId >= 0 ? playingId : firstId
    readonly property bool isPlaying: videoId >= 0 && videoId === playingId

    function refreshFirst() {
        firstId = active && App.videos.section === "review" && App.videos.count > 0 ? App.videos.get(0).videoId : -1
    }
    onActiveChanged: { refreshFirst(); refreshCandidates() }
    Connections {
        target: App.videos
        enabled: root.active
        // (Rows that come, go or are reordered all change the count's signal.)
        function onCountChanged() { root.refreshFirst() }
        function onSectionChanged() { root.refreshFirst() }
    }

    // ---- Its candidate tracks ---------------------------------------------
    property var candidates: []
    property int shown: 0
    readonly property var candidate: candidates.length > 0 ? candidates[Math.min(shown, candidates.length - 1)] : null
    function refreshCandidates() {
        const list = active && videoId >= 0 ? App.candidates(videoId) : []
        shown = list.length > 0 ? Math.max(0, Math.min(App.shownCandidate(videoId), list.length - 1)) : 0
        candidates = list
    }
    onVideoIdChanged: { elsewhereAt = 0; refreshCandidates() }
    Connections {
        target: App
        enabled: root.active
        function onCandidatesChanged() { root.refreshCandidates() }
    }
    function stepCandidate(delta) {
        const n = candidates.length
        if (n < 2)
            return
        if (isPlaying) {
            stepRequested(delta)
            return
        }
        elsewhereAt = 0
        App.showCandidate(videoId, ((shown + delta) % n + n) % n)
    }
    // The candidate on show is a candidate of other videos too: goes to the
    // next of those, and says which.
    property int elsewhereAt: 0
    function showElsewhere() {
        if (candidate === null || candidate.elsewhere.length === 0)
            return -1
        const id = candidate.elsewhere[elsewhereAt % candidate.elsewhere.length]
        elsewhereAt = (elsewhereAt + 1) % candidate.elsewhere.length
        revealRequested(id)
        return id
    }

    readonly property string title: candidate !== null ? candidate.title : ""
    readonly property string artist: (candidate !== null ? candidate.artist : "").replace(/; /g, ", ")
    // What the body shows, for the window's state report.
    readonly property var file: body.file
    readonly property string ytTitle: body.info.ytTitle || ""

    // ---- The candidate on show --------------------------------------------
    Column {
        id: head
        width: root.panelWidth
        bottomPadding: 6

        Item {
            width: parent.width
            height: 64

            WindowDrag { anchors.fill: parent }

            Text {
                x: 20
                anchors.verticalCenter: parent.verticalCenter
                text: root.videoId < 0 ? "To review" : root.isPlaying ? "Playing" : "First in the queue"
                color: Theme.textFaint
                font.pixelSize: 10
                font.weight: Font.DemiBold
                font.letterSpacing: 0.6
                font.capitalization: Font.AllUppercase
            }

            // Several tracks could have the video: the arrows show the
            // next one and the one before. On the playing video its audio
            // goes with them.
            Row {
                id: candidateSteps
                visible: root.candidates.length > 1
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 26
                    iconSize: 16
                    icon: Icons.back
                    tooltip: "Previous candidate track"
                    onClicked: root.stepCandidate(-1)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: (root.shown + 1) + "/" + root.candidates.length
                    color: Theme.textDim
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.features: { "tnum": 1 }
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 26
                    iconSize: 16
                    icon: Icons.back
                    iconRotation: 180
                    tooltip: "Next candidate track"
                    onClicked: root.stepCandidate(1)
                }
            }
        }

        Column {
            x: 20
            width: parent.width - 40
            visible: root.videoId >= 0
            spacing: 3
            Text {
                width: parent.width
                text: root.title
                color: Theme.text
                font.pixelSize: 15
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Item {
                width: parent.width
                height: 18
                Text {
                    width: parent.width - (elsewhere.visible ? elsewhere.width + 8 : 0)
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.artist
                    color: Theme.textDim
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                // The candidate on show is a candidate of other videos as
                // well: says so, and a click brings the next of them into view.
                Rectangle {
                    id: elsewhere
                    readonly property int count: root.candidate !== null ? root.candidate.elsewhere.length : 0
                    visible: count > 0
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: elsewhereRow.implicitWidth + 12
                    height: 18
                    radius: 9
                    color: elsewhereMouse.containsMouse ? Theme.accent : Theme.accentSoft
                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                    Row {
                        id: elsewhereRow
                        anchors.centerIn: parent
                        spacing: 4
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            path: Icons.movie
                            size: 11
                            color: elsewhereMouse.containsMouse ? Theme.accentInk : Theme.accentHi
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "+" + elsewhere.count
                            color: elsewhereMouse.containsMouse ? Theme.accentInk : Theme.accentHi
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                        }
                    }
                    MouseArea {
                        id: elsewhereMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.showElsewhere()
                    }
                    Tooltip {
                        text: elsewhere.count === 1 ? "This track is a candidate of another video too · click to go to it"
                                                    : "This track is a candidate of " + elsewhere.count + " other videos too · click to go to the next"
                        shown: elsewhereMouse.containsMouse
                    }
                }
            }
            // The judge's advice on this candidate (docs/import-rules-judge.html,
            // X2): keep, not this one, or unsure, with how sure it is, and
            // under the chip what it made of the upload, in its words. In
            // advice mode it is asked when the card first shows.
            Item {
                id: advice
                property var answer: ({ asked: false })
                // Account and model settings must not start library work.
                // Showing a card, changing candidates or enabling advice does.
                property string activeJudgeMode: App.judgeMode
                onActiveJudgeModeChanged: refresh()
                readonly property bool unsure: answer.asked && !answer.error && answer.probability > 0.4 && answer.probability < 0.6
                readonly property int percent: answer.asked ? Math.round((answer.keep ? answer.probability : 1 - answer.probability) * 100) : 0
                function refresh() {
                    if (root.videoId < 0 || root.candidate === null || App.judgeMode === "off") {
                        answer = { asked: false }
                        return
                    }
                    answer = App.judgeAdvice(root.videoId, root.candidate.trackId)
                    if (!answer.asked && App.judgeMode === "advice")
                        App.askJudge(root.videoId, root.candidate.trackId, false)
                }
                Connections {
                    target: App
                    function onAdviceChanged(videoId) { if (videoId === root.videoId) advice.refresh() }
                }
                Connections {
                    target: root
                    function onCandidateChanged() { advice.refresh() }
                }
                Component.onCompleted: refresh()
                visible: App.judgeMode !== "off" && root.videoId >= 0
                width: parent.width
                height: visible ? (adviceDetails.visible ? 24 + adviceDetails.implicitHeight : 22) : 0
                Rectangle {
                    id: adviceChip
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.topMargin: 2
                    width: adviceText.implicitWidth + 14
                    height: 18
                    radius: 9
                    color: !advice.answer.asked || advice.answer.error ? Theme.raised
                        : advice.unsure ? Theme.raised
                        : advice.answer.keep ? Theme.accentSoft : Qt.rgba(1, 0.36, 0.36, 0.18)
                    Text {
                        id: adviceText
                        anchors.centerIn: parent
                        text: !advice.answer.asked ? "Judge: asking…"
                            : advice.answer.error ? "Judge: could not answer"
                            : advice.unsure ? "Judge: unsure"
                            : (advice.answer.keep ? "Judge: keep · " : "Judge: not this one · ") + advice.percent + "%"
                        color: !advice.answer.asked || advice.answer.error || advice.unsure ? Theme.textDim
                            : advice.answer.keep ? Theme.accentHi : "#ff7a7a"
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                    }
                }
                // For when the words or the model change.
                Text {
                    visible: advice.answer.asked
                    anchors.left: adviceChip.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: adviceChip.verticalCenter
                    text: "Ask again"
                    color: againMouse.containsMouse ? Theme.text : Theme.textFaint
                    font.pixelSize: 10
                    MouseArea {
                        id: againMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { advice.answer = { asked: false }; App.askJudge(root.videoId, root.candidate.trackId, true) }
                    }
                }
                // What the judge made of the upload: its note in full, the
                // facts it settled on, and where the answer came from.
                Column {
                    id: adviceDetails
                    visible: advice.answer.asked
                    anchors.top: parent.top
                    anchors.topMargin: 24
                    width: parent.width
                    spacing: 2
                    Text {
                        width: parent.width
                        text: advice.answer.asked ? advice.answer.note : ""
                        color: Theme.textDim
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    Text {
                        visible: advice.answer.asked && !advice.answer.error
                        width: parent.width
                        text: advice.answer.asked ? advice.answer.kind + " · " + advice.answer.source + " · " + advice.answer.version + " · " + advice.answer.whole : ""
                        color: Theme.textFaint
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                    Text {
                        width: parent.width
                        text: advice.answer.asked ? (advice.answer.mode === "judge" ? "Given during the lookup" : "Asked from this card") + " · " + advice.answer.model : ""
                        color: Theme.textFaint
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    // ---- The last verdict, which can be taken back ------------------------
    Item {
        id: undo
        anchors.top: head.bottom
        width: root.panelWidth
        height: App.undoAvailable ? undoRow.implicitHeight + 16 : 0
        visible: height > 0
        clip: true
        Behavior on height { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Rectangle {
            x: 12
            y: 4
            width: parent.width - 24
            height: undoRow.implicitHeight + 8
            radius: Theme.radiusSmall
            color: Theme.raised
            Row {
                id: undoRow
                x: 6
                y: 4
                width: parent.width - 12
                spacing: 8
                FlatButton {
                    id: undoButton
                    anchors.verticalCenter: parent.verticalCenter
                    icon: Icons.undo
                    text: "Undo"
                    onClicked: App.undoVerdict()
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - undoButton.width - parent.spacing
                    text: App.undoLabel
                    color: Theme.textDim
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
            }
        }
    }

    // ---- Its music file and YouTube page ----------------------------------
    SourceInfo {
        id: body
        anchors.top: undo.bottom
        anchors.bottom: parent.bottom
        width: root.panelWidth
        visible: root.videoId >= 0
        videoId: root.videoId
        active: root.active && root.videoId >= 0
        onReplaceRequested: (videoId) => root.replaceRequested(videoId)
    }

    Text {
        visible: root.videoId < 0
        x: 20
        width: root.panelWidth - 40
        anchors.top: undo.bottom
        wrapMode: Text.WordWrap
        text: "Nothing waits for review."
        color: Theme.textDim
        font.pixelSize: 12
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.line
    }
}
