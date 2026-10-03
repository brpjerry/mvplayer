#pragma once

#include <QByteArray>
#include <QColor>

// Derives a UI accent colour from a (tiny, downscaled) video frame.
namespace AccentColor {

// Finds the dominant hue among the colourful pixels of an RGBA8888 buffer.
// `hue` is 0..1, `saturation` 0..1. Returns false for frames that are
// essentially black, white or grey.
bool dominantHue(const QByteArray &rgba, double *hue, double *saturation);

// A colour of that hue with the brightness every accent shares, so text and
// controls drawn with it stay equally legible whatever the hue.
QColor fromHue(double hue, double saturation);

// The accent for one frame: its dominant hue, or for a colourless frame a
// neutral running from light grey (black frame) to white (bright frame).
QColor fromFrame(const QByteArray &rgba);

} // namespace AccentColor
