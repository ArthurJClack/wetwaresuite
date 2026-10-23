// Selections and masks: making them (rectangle, ellipse, lasso, magic wand,
// colour range), combining, growing, feathering, and using them to limit an
// edit to part of the picture. Both are a Mask: one byte per pixel.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

enum class MaskOp { Replace, Add, Subtract, Intersect };
const std::vector<std::string>& maskOpNames();  // "replace", "add", "subtract", "intersect"
std::optional<MaskOp> parseMaskOp(const std::string& name);

Mask rectMask(int width, int height, int x, int y, int w, int h);
// Ellipse filling the box; soft edge one pixel wide when antialias is on.
Mask ellipseMask(int width, int height, double x, double y, double w, double h, bool antialias = true);
// Polygon from x,y pairs (closed, even-odd), edge coverage measured on a 4x4 grid when antialias is on.
Mask polygonMask(int width, int height, const std::vector<double>& points, bool antialias = true);
// Pixels like the one at (x, y): every channel within `tolerance` (0..255). Contiguous only
// takes the ones joined to it.
Mask wandMask(const Image& img, int x, int y, int tolerance, bool contiguous = true);
// Pixels near `color`: fully in within tolerance, fading out over `fuzz` more.
Mask colorRangeMask(const Image& img, Color color, int tolerance, int fuzz = 0);
// The image's alpha as a mask.
Mask alphaMask(const Image& img);

// Puts `shape` into `base` (both the same size; an empty base counts as nothing selected).
void combine(Mask& base, const Mask& shape, MaskOp op);
void invert(Mask& m);
void grow(Mask& m, int radius);    // spreads out by radius px (negative shrinks)
void feather(Mask& m, double radius);  // softens the edge
Mask border(const Mask& m, int width);  // a band `width` px wide along the edge
bool anySelected(const Mask& m);
PixelRect bounds(const Mask& m);  // of pixels > 0; empty when none
double coverage(const Mask& m);    // 0..1 of the area

// Limits an edit to the mask: where it's 0 `edited` goes back to `before`, where it's 255 the edit
// stays, in between they mix. Only the rect is touched (all of it when rect is empty).
void keepInside(Image& edited, const Image& before, const Mask& m, PixelRect rect = {});
// Multiplies the image's alpha by the mask.
void applyMask(Image& img, const Mask& m);

// For moving masks through image geometry ops: the mask in the alpha channel and back.
Image maskToImage(const Mask& m);
Mask imageToMask(const Image& img);

// Tone curves: a 256-entry lookup table through the given (input, output) points, 0..255,
// a smooth curve that never overshoots between points.
std::array<std::uint8_t, 256> curveTable(const std::vector<std::pair<double, double>>& points);
// Applies tables to red, green, blue (alpha untouched).
void applyCurves(Image& img, const std::array<std::uint8_t, 256>& r, const std::array<std::uint8_t, 256>& g,
                 const std::array<std::uint8_t, 256>& b);
// Colour balance: shifts per tonal range, each -100..100 for cyan-red, magenta-green, yellow-blue.
struct ColorShift {
    double cyanRed = 0, magentaGreen = 0, yellowBlue = 0;
};
void colorBalance(Image& img, ColorShift shadows, ColorShift midtones, ColorShift highlights, bool preserveLuminosity = true);

}  // namespace wetwaresuite::raster
