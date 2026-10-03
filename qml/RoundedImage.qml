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
    readonly property bool ready: img.status === Image.Ready
    // Width the picture is decoded at; lets others request the same cached copy.
    readonly property int decodeWidth: img.sourceSize.width

    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: root.placeholder
        visible: effect.opacity < 1
    }

    Image {
        id: img
        visible: false
        source: root.source
        asynchronous: true
        cache: true
        // Decode at the size it is shown (in device pixels, stepped so that
        // resizing does not reload constantly), not at the file's full size.
        sourceSize.width: Math.ceil(Math.max(root.width, 64) * Screen.devicePixelRatio * 1.1 / 64) * 64
        smooth: true
    }

    ShaderEffect {
        id: effect
        anchors.fill: parent
        opacity: root.ready ? 1 : 0
        visible: opacity > 0

        property variant source: img
        property vector2d size: Qt.vector2d(width, height)
        property real radius: Math.min(root.radius, width / 2, height / 2)
        property real zoom: root.zoom
        property real dim: root.dim
        property vector2d uvScale: {
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
