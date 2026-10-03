import QtQuick

// Three bouncing bars marking the video that is playing.
Item {
    id: root

    property bool playing: true
    property color color: Theme.accent

    implicitWidth: 14
    implicitHeight: 14

    Repeater {
        model: [
            { low: 0.35, high: 1.0, time: 420 },
            { low: 0.25, high: 0.8, time: 560 },
            { low: 0.4, high: 0.95, time: 480 }
        ]
        Rectangle {
            id: bar
            required property var modelData
            required property int index
            x: index * 5
            width: 3
            height: root.height
            radius: 1.5
            color: root.color
            transformOrigin: Item.Bottom
            scale: 1
            transform: Scale {
                id: stretch
                origin.y: bar.height
                yScale: bar.modelData.low
            }

            SequentialAnimation {
                running: root.playing && root.visible
                loops: Animation.Infinite
                onRunningChanged: if (!running) stretch.yScale = bar.modelData.low
                NumberAnimation { target: stretch; property: "yScale"; to: bar.modelData.high; duration: bar.modelData.time; easing.type: Easing.InOutSine }
                NumberAnimation { target: stretch; property: "yScale"; to: bar.modelData.low; duration: bar.modelData.time; easing.type: Easing.InOutSine }
            }
        }
    }
}
