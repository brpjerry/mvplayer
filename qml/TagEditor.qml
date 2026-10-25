import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import MvPlayer.Core

// Editing the tags of music files, written into the files themselves (the
// one thing the app changes in the music library). Two modes: one song (its
// file, or the files that hold the recording) with every tag, and a batch
// (an album, several albums, several songs) with the tags its files can
// share: a title or a track number is a track's own and is not offered
// there. A field left as it is stays as each file has it; one changed goes
// into every file, and emptied takes the tag out. Where a file has a
// picture embedded, another picture can be put in its place (a file without
// one is left as it is, and the row is not shown when none has one). Videos
// keep their files; the library shows them under the new tags (import-rules
// A10).
Popup {
    id: root

    property var trackIds: []       // the files, by track id
    property bool batch: false
    property string subject: ""     // what is edited: "Northern Lights", "3 albums", "5 songs"
    property var info: ({})         // App.tagEditorInfo(trackIds)
    property var edits: ({})        // KEY -> the new value, for the fields touched
    property url art: ""            // the picture chosen in place of the embedded one; empty: as it is
    readonly property int artFiles: info.artFiles || 0
    readonly property string artPreview: info.artPath ? "image://cover/" + encodeURIComponent(info.artPath) + "?embedded=" + info.artStamp : ""
    property bool writing: false
    property bool saved: false      // the files took the tags (then it closes)
    property var failed: []         // after a write: the files that did not take it, and why
    readonly property int files: info.files || 0
    readonly property int songs: info.songs || 0
    readonly property var fields: (info.fields || []).filter(f => !root.batch || !f.perTrack)
    readonly property int editCount: Object.keys(edits).length + (art != "" ? 1 : 0)
    readonly property var firstTrack: (info.tracks || []).length > 0 ? info.tracks[0] : null

    // Opens for these files: as a batch, or as one song.
    function openFor(ids, batchMode, subjectText) {
        trackIds = ids
        batch = batchMode
        subject = subjectText || ""
        info = App.tagEditorInfo(ids)
        edits = ({})
        art = ""
        failed = []
        writing = false
        saved = false
        open()
    }
    function set(key, value) {
        const next = Object.assign({}, edits)
        next[key] = value
        edits = next
    }
    function revert(key) {
        const next = Object.assign({}, edits)
        delete next[key]
        edits = next
    }
    function isEdited(key) { return edits[key] !== undefined }
    // What a field shows: the edit, else what the files have.
    function shown(field) { return isEdited(field.key) ? edits[field.key] : field.value }
    function save() {
        if (editCount === 0 || writing) {
            if (!writing)
                close()
            return
        }
        failed = []
        const why = App.writeTags(trackIds, edits, art)
        if (why.length > 0)
            failed = [why]
        else
            writing = true
    }

    FileDialog {
        id: artDialog
        title: "Choose a picture"
        nameFilters: ["Pictures (*.jpg *.jpeg *.png *.webp *.bmp)", "All files (*)"]
        currentFolder: root.firstTrack ? App.pathToUrl(root.firstTrack.path.replace(/[\\/][^\\/]*$/, "")) : ""
        onAccepted: root.art = selectedFile
    }

    Connections {
        target: App
        enabled: root.visible
        function onTagsWritten(written, failedList) {
            if (!root.writing)
                return
            root.writing = false
            if (failedList.length === 0) {
                root.saved = true
                root.close()
            } else {
                root.failed = failedList
            }
        }
    }

    width: 560
    height: Math.min(content.implicitHeight + 2, (Overlay.overlay ? Overlay.overlay.height : 800) - 32)
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true // Escape closes it only while it has the focus
    padding: 1
    closePolicy: writing ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside

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

    function plural(n, one, many) { return n + " " + (n === 1 ? one : many) }

    // One tag: its name, what the files have (or "Mixed" where they
    // differ), and the value typed in its place.
    component TagField: Item {
        id: field
        property var model
        readonly property bool edited: root.isEdited(model.key)
        readonly property string value: root.shown(model)
        readonly property bool removes: edited && value.trim().length === 0 && (model.value.length > 0 || model.mixed)
        width: parent.width
        height: 40
        Text {
            id: label
            x: 0
            width: 116
            anchors.verticalCenter: parent.verticalCenter
            text: field.model.label
            color: field.edited ? Theme.text : Theme.textDim
            font.pixelSize: 13
            elide: Text.ElideRight
        }
        Rectangle {
            id: box
            anchors.left: label.right
            anchors.right: revertButton.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            height: 34
            radius: Theme.radiusSmall
            color: Theme.hover
            border.width: 1
            border.color: input.activeFocus ? Theme.accent : field.edited ? Theme.accentSoft : "transparent"
            Behavior on border.color { ColorAnimation { duration: Theme.fast } }
            TextInput {
                id: input
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.text
                selectionColor: Theme.accent
                selectedTextColor: Theme.accentInk
                font.pixelSize: 13
                clip: true
                selectByMouse: true
                enabled: !root.writing
                // Set, not bound: the user's typing is the text.
                Component.onCompleted: text = field.value
                onTextEdited: root.set(field.model.key, text)
                onAccepted: root.save()
                Connections {
                    target: root
                    // Taken back from the button, the field shows the
                    // files' value again; typing leaves it as typed.
                    function onEditsChanged() {
                        const want = root.shown(field.model)
                        if (input.text !== want)
                            input.text = want
                    }
                }
            }
            // What the files have, where the field is empty: nothing, or
            // different values that typing would replace in every file.
            Text {
                anchors.fill: input
                verticalAlignment: Text.AlignVCenter
                visible: input.text.length === 0 && !input.activeFocus
                text: field.removes ? "Removed from every file" : field.model.mixed ? "Mixed · the files differ" : ""
                color: field.removes ? Theme.warn : Theme.textFaint
                font.pixelSize: 13
                font.italic: !field.removes
                elide: Text.ElideRight
            }
        }
        IconButton {
            id: revertButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            size: 30
            iconSize: 16
            icon: Icons.undo
            tooltip: "As the files have it"
            opacity: field.edited ? 1 : 0
            enabled: field.edited && !root.writing
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
            onClicked: root.revert(field.model.key)
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
                width: parent.width - 80
                anchors.verticalCenter: parent.verticalCenter
                text: root.batch ? "Edit tags · " + root.subject : "Edit tags"
                color: Theme.text
                font.pixelSize: 18
                font.weight: Font.DemiBold
                font.letterSpacing: -0.2
                elide: Text.ElideRight
            }
            IconButton {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                icon: Icons.close
                iconSize: 18
                enabled: !root.writing
                onClicked: root.close()
            }
        }
        // What is edited: the song and its file, or how many files share
        // the fields below.
        Text {
            x: 24
            width: parent.width - 48
            text: root.batch
                  ? root.plural(root.songs, "song", "songs") + " in " + root.plural(root.files, "file", "files")
                  : (root.firstTrack ? root.firstTrack.title + (root.firstTrack.artist ? "  —  " + root.firstTrack.artist.replace(/; /g, ", ") : "") : "")
            color: Theme.textDim
            font.pixelSize: 13
            elide: Text.ElideRight
        }
        Text {
            x: 24
            width: parent.width - 48
            visible: text.length > 0
            text: root.batch ? "" : root.firstTrack
                  ? App.displayPath(root.firstTrack.path) + (root.files > 1 ? "  ·  and " + root.plural(root.files - 1, "other file", "other files") + " of this recording" : "")
                  : "No file of it is in your music folders."
            color: Theme.textFaint
            font.pixelSize: 11
            elide: Text.ElideMiddle
        }
        Item { width: 1; height: 8 }
        Text {
            x: 24
            width: parent.width - 48
            text: root.batch
                  ? "The tags an album's files share. A field left as it is stays as each file has it; one you change goes into every file, and one you empty is taken out of them."
                  : "Written into the file" + (root.files > 1 ? "s" : "") + ". A field you empty is taken out."
            color: Theme.textFaint
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
        Item { width: 1; height: 14 }

        // The picture embedded in the files, and another in its place
        Item {
            visible: root.artFiles > 0
            x: 24
            width: parent.width - 48
            height: visible ? 72 : 0
            Text {
                id: artLabel
                width: 116
                anchors.verticalCenter: parent.verticalCenter
                text: "Cover art"
                color: root.art != "" ? Theme.text : Theme.textDim
                font.pixelSize: 13
            }
            Rectangle {
                id: artBox
                anchors.left: artLabel.right
                anchors.verticalCenter: parent.verticalCenter
                width: 64
                height: 64
                radius: Theme.radiusSmall
                color: Theme.hover
                border.width: root.art != "" ? 2 : 0
                border.color: Theme.accent
                clip: true
                Image {
                    id: artImage
                    anchors.fill: parent
                    anchors.margins: root.art != "" ? 2 : 0
                    source: root.art != "" ? root.art : root.artPreview
                    sourceSize.width: 256
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: false
                }
            }
            Column {
                anchors.left: artBox.right
                anchors.leftMargin: 12
                anchors.right: artButtons.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                // Which files carry a picture; replaced, it goes into those.
                readonly property string where: root.artFiles === root.files ? (root.files === 1 ? "the file" : "every file")
                                                                             : root.plural(root.artFiles, "file", "files") + " of " + root.files
                Text {
                    width: parent.width
                    text: root.art != "" ? "Replaces it in " + parent.where : "Embedded in " + parent.where
                    color: Theme.text
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: root.art != "" ? App.displayPath(App.urlToPath(root.art)) : (root.info.artKind || "")
                    color: Theme.textFaint
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                }
            }
            Row {
                id: artButtons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                FlatButton {
                    text: root.art != "" ? "Another…" : "Replace…"
                    enabled: !root.writing
                    onClicked: artDialog.open()
                }
                IconButton {
                    size: 30
                    iconSize: 16
                    icon: Icons.undo
                    tooltip: "As the files have it"
                    opacity: root.art != "" ? 1 : 0
                    enabled: root.art != "" && !root.writing
                    Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                    onClicked: root.art = ""
                }
            }
        }
        Item { visible: root.artFiles > 0; width: 1; height: visible ? 10 : 0 }

        // The fields
        Flickable {
            id: flick
            x: 24
            width: parent.width - 48
            height: Math.min(fieldsColumn.implicitHeight, (Overlay.overlay ? Overlay.overlay.height : 800) - 340)
            contentHeight: fieldsColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
            Column {
                id: fieldsColumn
                width: flick.width - (flick.contentHeight > flick.height ? 12 : 0)
                spacing: 2
                Repeater {
                    model: root.fields
                    TagField {
                        required property var modelData
                        model: modelData
                    }
                }
            }
        }
        Item { width: 1; height: 14 }

        // Videos: nothing happens to their files.
        Row {
            x: 24
            width: parent.width - 48
            spacing: 8
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                path: Icons.movie
                size: 14
                color: Theme.textFaint
            }
            Text {
                width: parent.width - 22
                anchors.verticalCenter: parent.verticalCenter
                text: root.batch ? "Music videos of these songs keep their files and are shown under the new tags."
                                 : "Its music video, if it has one, keeps its file and is shown under the new tags."
                color: Theme.textFaint
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
        // What did not take the tags
        Column {
            visible: root.failed.length > 0
            x: 24
            width: parent.width - 48
            topPadding: 10
            spacing: 2
            Text {
                width: parent.width
                text: root.plural(root.failed.length, "file was", "files were") + " not written:"
                color: Theme.bad
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Repeater {
                model: root.failed.slice(0, 5)
                Text {
                    required property string modelData
                    width: parent.width
                    text: modelData
                    color: Theme.bad
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                }
            }
            Text {
                visible: root.failed.length > 5
                text: "… and " + (root.failed.length - 5) + " more"
                color: Theme.bad
                font.pixelSize: 11
            }
        }
        Item { width: 1; height: 18 }
        Item {
            x: 24
            width: parent.width - 48
            height: 34
            Row {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                visible: root.writing
                Spinner {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 14
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Writing " + root.plural(root.files, "file", "files") + "…"
                    color: Theme.textDim
                    font.pixelSize: 12
                }
            }
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.writing && root.editCount > 0
                text: root.plural(root.editCount, "field", "fields") + " changed"
                color: Theme.textDim
                font.pixelSize: 12
            }
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10
                FlatButton {
                    text: root.failed.length > 0 ? "Close" : "Cancel"
                    enabled: !root.writing
                    onClicked: root.close()
                }
                FlatButton {
                    text: root.batch ? "Save to " + root.plural(root.files, "file", "files") : "Save"
                    primary: true
                    enabled: !root.writing && root.editCount > 0 && root.files > 0
                    onClicked: root.save()
                }
            }
        }
    }
}
