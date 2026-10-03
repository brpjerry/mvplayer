import QtQuick
import QtQuick.Dialogs
import MvPlayer.Core

// First-run screen: pick the music library to build the MV library from.
Item {
    id: root

    FolderDialog {
        id: dialog
        title: "Choose your music library"
        onAccepted: App.addMusicDir(App.urlToPath(selectedFolder))
    }

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
            text: "Add your music library"
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
            text: "MV Player finds the music video for each track, downloads it at the best quality available and pairs it with your own audio. Your music folders are only read, never changed; more can be added in Settings."
            color: Theme.textDim
            font.pixelSize: 14
        }
        Item { width: 1; height: 6 }
        FlatButton {
            anchors.horizontalCenter: parent.horizontalCenter
            implicitHeight: 40
            text: "Choose music folder"
            icon: Icons.folder
            primary: true
            onClicked: dialog.open()
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Videos are saved to " + App.displayPath(App.mvDir)
            color: Theme.textFaint
            font.pixelSize: 12
        }
    }
}
