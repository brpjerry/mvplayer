import QtQuick

// Image cropped to fill a rounded rectangle, drawn in a single shader pass.
// `zoom` scales the picture inside the frame (used for hover effects).
Item {
    id: root

    property url source
    property real radius: Theme.radius
    property real zoom: 1.0
    property real dim: 0.0
    property color placeholder: Theme.raised
    readonly property bool ready: front.status === Image.Ready
    // Width the picture is decoded at; lets others request the same cached copy.
    readonly property int decodeWidth: front.sourceSize.width

    // Decode at the size it is shown (in device pixels, stepped so that
    // resizing does not reload constantly), not at the file's full size.
    readonly property int wantWidth: Math.ceil(Math.max(root.width, 64) * Screen.devicePixelRatio * 1.1 / 64) * 64

    // Two decoders: the picture on show stays up until the next decode (a
    // new size while the grid re-lays itself out, or a new source) is
    // ready, so the frame never drops to the placeholder in between.
    property Image front: imgA
    property Image back: imgB

    function request() {
        if (root.source == "") {
            front.source = ""
            back.source = ""
            return
        }
        if (front.source == root.source && front.sourceSize.width === wantWidth) {
            back.source = ""
            return
        }
        if (back.source == root.source && back.sourceSize.width === wantWidth)
            return
        back.sourceSize.width = wantWidth
        back.source = root.source
        // A copy Qt still holds lands at once, inside the assignment; by
        // then `landed` has swapped and `back` is the other one again.
        if (back.source == root.source && back.status !== Image.Loading)
            swap()
    }
    function swap() {
        const was = front
        front = back
        back = was
        back.source = ""
    }
    function landed(img) {
        if (img === back && back.source == root.source && back.status !== Image.Loading)
            swap()
    }
    onSourceChanged: request()
    onWantWidthChanged: request()
    Component.onCompleted: request()

    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: root.placeholder
        visible: effect.opacity < 1
    }

    Image {
        id: imgA
        visible: false
        asynchronous: true
        cache: true
        smooth: true
        onStatusChanged: root.landed(imgA)
    }
    Image {
        id: imgB
        visible: false
        asynchronous: true
        cache: true
        smooth: true
        onStatusChanged: root.landed(imgB)
    }

    ShaderEffect {
        id: effect
        anchors.fill: parent
        opacity: root.ready ? 1 : 0
        visible: opacity > 0

        property variant source: root.front
        property vector2d size: Qt.vector2d(width, height)
        property real radius: Math.min(root.radius, width / 2, height / 2)
        property real zoom: root.zoom
        property real dim: root.dim
        property vector2d uvScale: {
            const img = root.front
            if (img.implicitWidth <= 0 || img.implicitHeight <= 0 || height <= 0)
                return Qt.vector2d(1, 1)
            const imageAspect = img.implicitWidth / img.implicitHeight
            const itemAspect = width / height
            return imageAspect > itemAspect ? Qt.vector2d(itemAspect / imageAspect, 1)
                                            : Qt.vector2d(1, imageAspect / itemAspect)
        }

        fragmentShader: "qrc:/shaders/rounded.frag.qsb"

        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    }
}
