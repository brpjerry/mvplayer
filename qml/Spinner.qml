import QtQuick
import QtQuick.Shapes

// Indeterminate progress ring. Rotation runs on the render thread.
Item {
    id: root

    property real size: 16
    property color color: Theme.accent
    property bool running: visible

    implicitWidth: size
    implicitHeight: size

    Shape {
        id: ring
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: root.color
            strokeWidth: Math.max(1.5, root.size / 8)
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.size / 2
                centerY: root.size / 2
                radiusX: root.size / 2 - 1.5
                radiusY: root.size / 2 - 1.5
                startAngle: 0
                sweepAngle: 270
            }
        }

        RotationAnimator on rotation {
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
            running: root.running
        }
    }
}
