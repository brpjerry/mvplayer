pragma Singleton
import QtQuick

// Flat palette (dark and light) and shared motion values.
QtObject {
    id: theme

    // Set from outside; every colour below cross-fades when it changes.
    property bool dark: true
    property real lightness: dark ? 0 : 1
    Behavior on lightness { NumberAnimation { duration: 260; easing.type: Easing.InOutSine } }

    readonly property QtObject darkPalette: QtObject {
        readonly property color bg: "#0c0d10"
        readonly property color surface: "#121419"
        readonly property color raised: "#191c22"
        readonly property color hover: "#1f232b"
        readonly property color pressed: "#272c36"
        readonly property color line: "#20242c"
        readonly property color text: "#eef0f4"
        readonly property color textDim: "#9ba2af"
        readonly property color textFaint: "#636a78"
        readonly property color good: "#4fd6a2"
        readonly property color warn: "#f2b866"
        readonly property color bad: "#ff7676"
    }
    readonly property QtObject lightPalette: QtObject {
        readonly property color bg: "#f5f6f8"
        readonly property color surface: "#eceef2"
        readonly property color raised: "#ffffff"
        readonly property color hover: "#e2e5eb"
        readonly property color pressed: "#d3d7df"
        readonly property color line: "#d9dce3"
        readonly property color text: "#15171b"
        readonly property color textDim: "#555c69"
        readonly property color textFaint: "#878e9b"
        readonly property color good: "#1c9a6b"
        readonly property color warn: "#b27610"
        readonly property color bad: "#d24444"
    }

    // Surfaces, from the window background up
    readonly property color bg: mix(darkPalette.bg, lightPalette.bg, lightness)
    readonly property color surface: mix(darkPalette.surface, lightPalette.surface, lightness)
    readonly property color raised: mix(darkPalette.raised, lightPalette.raised, lightness)
    readonly property color hover: mix(darkPalette.hover, lightPalette.hover, lightness)
    readonly property color pressed: mix(darkPalette.pressed, lightPalette.pressed, lightness)
    readonly property color line: mix(darkPalette.line, lightPalette.line, lightness)

    // Text
    readonly property color text: mix(darkPalette.text, lightPalette.text, lightness)
    readonly property color textDim: mix(darkPalette.textDim, lightPalette.textDim, lightness)
    readonly property color textFaint: mix(darkPalette.textFaint, lightPalette.textFaint, lightness)

    readonly property color good: mix(darkPalette.good, lightPalette.good, lightness)
    readonly property color warn: mix(darkPalette.warn, lightPalette.warn, lightness)
    readonly property color bad: mix(darkPalette.bad, lightPalette.bad, lightness)

    // Things drawn over pictures and video look the same in both modes.
    readonly property color scrim: "#d00c0d10"
    readonly property color scrimText: "#eef0f4"

    // Accent. Set `accentSource`; `accent` and its companions glide after it
    // over `accentFade` milliseconds.
    readonly property color defaultAccent: "#8b7dff"
    property color accentSource: defaultAccent
    property int accentFade: 300
    // Accents are chosen for a dark background; on a light one they are
    // deepened, and near-white ones become near-black.
    readonly property color accentTarget: dark ? accentSource : forLightMode(accentSource)
    property color accent: accentTarget
    Behavior on accent { ColorAnimation { duration: theme.accentFade; easing.type: Easing.InOutSine } }

    readonly property color white: "#ffffff"
    readonly property color black: "#000000"
    // Accent-coloured text on the background
    readonly property color accentHi: mix(mix(accent, white, 0.24), mix(accent, black, 0.28), lightness)
    // Accent-tinted surface
    readonly property color accentSoft: mix(bg, accent, 0.2)
    // Text and icons on an accent-filled shape
    readonly property color accentInk: luminance(accentTarget) > 0.22 ? "#0c0d10" : "#ffffff"

    function mix(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1)
    }
    function forLightMode(c) {
        const neutral = Math.max(0, Math.min(1, 1 - c.hslSaturation / 0.35))
        const coloured = c.hslLightness * 0.8
        const inverted = Math.max(0.07, 1.04 - c.hslLightness)
        return Qt.hsla(Math.max(0, c.hslHue), c.hslSaturation, coloured + (inverted - coloured) * neutral, 1)
    }
    // A colour of the given hue (0..1) at the brightness all accents share.
    function accentForHue(hue) {
        let lo = 0.2, hi = 0.95
        for (let i = 0; i < 16; ++i) {
            const mid = (lo + hi) / 2
            if (luminance(Qt.hsla(hue, 0.78, mid, 1)) < 0.33)
                lo = mid
            else
                hi = mid
        }
        return Qt.hsla(hue, 0.78, (lo + hi) / 2, 1)
    }
    function luminance(c) {
        const lin = (v) => v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
        return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b)
    }

    readonly property int radius: 10
    readonly property int radiusSmall: 6

    // Motion
    readonly property int fast: 110
    readonly property int normal: 190
    readonly property int slow: 340
    // "Emphasized decelerate": quick start, long soft landing.
    readonly property list<real> emphasized: [0.2, 0.0, 0.0, 1.0, 1.0, 1.0]

    function formatTime(seconds) {
        if (!(seconds > 0))
            return "0:00"
        const total = Math.floor(seconds)
        const h = Math.floor(total / 3600)
        const m = Math.floor(total / 60) % 60
        const s = total % 60
        const ss = s < 10 ? "0" + s : "" + s
        if (h > 0)
            return h + ":" + (m < 10 ? "0" + m : m) + ":" + ss
        return m + ":" + ss
    }
}
