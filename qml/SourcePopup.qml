import QtQuick
import QtQuick.Controls.Basic
import MvPlayer.Core

// What the playing video stands for: the music file it was found for, with
// every tag in that file, and the video's page on YouTube. Opens from the
// now-playing corner of the control bar. (Under review the same is in the
// review panel instead, beside the sidebar.)
Popup {
    id: root

    property var videoId: -1
    // The user wants another video in this one's place.
    signal replaceRequested(var videoId)

    onVideoIdChanged: if (videoId < 0) close()

    width: 440
    // Tall enough for its content, up to most of the window; past that the
    // tags scroll.
    height: Math.min(body.contentHeight + 2, Overlay.overlay ? Overlay.overlay.height - 140 : 600)
    padding: 1
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    focus: true // Escape closes it only while it has the focus
    transformOrigin: Popup.BottomLeft

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

    background: Rectangle {
        radius: Theme.radius + 2
        color: Theme.raised
        border.width: 1
        border.color: Theme.line
    }

    contentItem: SourceInfo {
        id: body
        videoId: root.videoId
        active: root.visible
        onReplaceRequested: (videoId) => {
            root.replaceRequested(videoId)
            root.close()
        }
    }
}
