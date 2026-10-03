import QtQuick

// Wheel and touchpad scrolling for a Flickable, tuned to feel like a browser:
// touchpad movement is amplified and keeps gliding after the fingers lift,
// mouse wheel notches glide instead of jumping.
WheelHandler {
    id: root

    required property Flickable view

    // Browsers move several times the distance the touchpad reports; a plain
    // 1:1 mapping feels slow next to them.
    property real touchpadGain: 4.0
    property real wheelStep: 170 // pixels per mouse wheel notch
    // How quickly a flick glides to a stop, in pixels per second squared.
    property real deceleration: 4800

    readonly property bool active: glide.running || tracking

    property bool tracking: false
    property real goal: 0
    property real velocity: 0      // pixels per second, smoothed
    property real lastEventMs: 0
    property bool systemMomentum: false

    target: null
    onDecelerationChanged: view.flickDeceleration = deceleration
    Component.onCompleted: view.flickDeceleration = deceleration
    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

    property NumberAnimation glide: NumberAnimation {
        target: root.view
        property: "contentY"
        duration: 230
        easing.type: Easing.OutCubic
    }
    property Timer idle: Timer {
        interval: 150
        onTriggered: root.tracking = false
    }

    function clampY(y) {
        const minY = view.originY - view.topMargin
        const maxY = Math.max(minY, view.originY + view.contentHeight - view.height + view.bottomMargin)
        return Math.max(minY, Math.min(maxY, y))
    }

    // One scroll event. `pixelDy` is the precise delta (touchpads), `angleDy`
    // the wheel angle in eighths of a degree, `phase` a Qt.ScrollPhase.
    function scroll(pixelDy, angleDy, phase) {
        const precise = phase !== Qt.NoScrollPhase || pixelDy !== 0
        if (!precise) {
            if (angleDy === 0)
                return
            const from = glide.running ? goal : view.contentY
            goal = clampY(from - angleDy / 120 * wheelStep)
            view.cancelFlick()
            glide.stop()
            glide.from = view.contentY
            glide.to = goal
            glide.start()
            return
        }

        const now = Date.now()
        if (phase === Qt.ScrollBegin) {
            view.cancelFlick()
            glide.stop()
            velocity = 0
            lastEventMs = 0
            systemMomentum = false
        } else if (phase === Qt.ScrollMomentum) {
            systemMomentum = true // the platform sends its own glide (macOS)
        } else if (phase === Qt.ScrollEnd) {
            tracking = false
            // Fingers lifted while moving: carry the motion on.
            if (!systemMomentum && now - lastEventMs < 80 && Math.abs(velocity) > 250)
                view.flick(0, velocity)
            velocity = 0
            return
        }

        const dy = pixelDy * touchpadGain
        if (dy === 0)
            return
        glide.stop()
        view.cancelFlick()
        view.contentY = clampY(view.contentY - dy)
        if (lastEventMs > 0) {
            const dt = Math.max(4, Math.min(100, now - lastEventMs))
            velocity = velocity * 0.6 + (dy / dt * 1000) * 0.4
        }
        lastEventMs = now
        tracking = true
        idle.restart()
    }

    onWheel: (event) => scroll(event.pixelDelta.y, event.angleDelta.y, event.phase)
}
