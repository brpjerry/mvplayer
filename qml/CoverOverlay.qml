import QtQuick

// The playing item's cover, large, over the whole window. A double click
// on the now-playing panel's cover opens it; a double click, Escape or a
// click on the backdrop closes it.
Item {
    id: root

    property url source
    property bool open: false

    function show() { if (source != "") open = true }
    function hide() { open = false }

    anchors.fill: parent
    opacity: open ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    // The picture goes with its source: nothing playing, nothing to show.
    onSourceChanged: if (source == "") open = false

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
    }
    MouseArea {
        id: backdrop
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true // nothing beneath lights up under the pointer
        onWheel: (wheel) => wheel.accepted = true
        onClicked: (mouse) => {
            if (!picture.contains(picture.mapFromItem(backdrop, mouse.x, mouse.y)))
                root.hide()
        }
        onDoubleClicked: root.hide()
    }
    RoundedImage {
        id: picture
        anchors.centerIn: parent
        // The picture's own shape, as large as the window leaves room for.
        readonly property real aspect: front.implicitWidth > 0 && front.implicitHeight > 0
                                       ? front.implicitWidth / front.implicitHeight : 1
        width: Math.max(1, Math.min(root.width - 96, (root.height - 96) * aspect))
        height: width / aspect
        radius: Theme.radius
        source: root.source
        scale: root.open ? 1 : 0.92
        Behavior on scale { NumberAnimation { duration: Theme.slow; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.emphasized } }
    }
}
