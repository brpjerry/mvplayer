import QtQuick
import QtQuick.Dialogs
import MvPlayer.Core

// First-run screen: pick the music library to build the MV library from.
Item {
    id: root

    FolderDialog {
        id: dialog
        title: "Select the base directory of your music"
        currentFolder: App.pathToUrl(App.baseDir)
        onAccepted: App.baseDir = App.urlToPath(selectedFolder)
    }
    readonly property bool hasBase: App.baseDir.length > 0

    Column {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -20
        width: Math.min(460, parent.width - 80)
        spacing: 14

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 64
            height: 64
            radius: 18
            color: Theme.accentSoft
            Icon {
                anchors.centerIn: parent
                path: Icons.music
                size: 32
                color: Theme.accent
            }
        }
        Item { width: 1; height: 2 }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.hasBase ? "Tick the folders to read" : "Add your music library"
            color: Theme.text
            font.pixelSize: 22
            font.weight: Font.DemiBold
            font.letterSpacing: -0.4
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: 1.35
            text: root.hasBase ? "Only the music in the folders you tick is read, everything under each included. Your music folders are only read, never changed; the choice can be changed in Settings."
                               : "MV Player finds the music video for each track, downloads it at the best quality available and pairs it with your own audio. Your music folders are only read, never changed; which of them are read is chosen next."
            color: Theme.textDim
            font.pixelSize: 14
        }
        Item { width: 1; height: 6 }
        FlatButton {
            anchors.horizontalCenter: parent.horizontalCenter
            implicitHeight: 40
            text: "Select base directory"
            icon: Icons.folder
            primary: !root.hasBase
            onClicked: dialog.open()
        }
        Rectangle {
            visible: root.hasBase
            width: parent.width
            height: tree.height + 16
            radius: Theme.radius
            color: Theme.surface
            border.width: 1
            border.color: Theme.line
            FolderTree {
                id: tree
                x: 12
                y: 8
                width: parent.width - 24
                maxHeight: Math.max(120, root.height - 420)
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Videos are saved to " + App.displayPath(App.mvDir)
            color: Theme.textFaint
            font.pixelSize: 12
        }
    }
}
