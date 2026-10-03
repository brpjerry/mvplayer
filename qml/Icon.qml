import QtQuick
import QtQuick.Shapes

// A single-colour vector icon drawn from 24×24 SVG path data.
Item {
    id: root

    property string path
    property color color: Theme.text
    property real size: 20

    implicitWidth: size
    implicitHeight: size

    Shape {
        anchors.centerIn: parent
        width: 24
        height: 24
        scale: root.size / 24
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: root.color
            strokeColor: "transparent"
            fillRule: ShapePath.WindingFill
            PathSvg { path: root.path }
        }
    }
}
