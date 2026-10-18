// Geometry, adjustments and filters on whole images. Everything returns a new
// image or edits in place; nothing here knows about commands or documents.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

// Nearest: blocky, fastest. Bilinear: smooth, the default. Area: averages whole source pixels
// (best for shrinking a lot; falls back to bilinear when enlarging). Lanczos: Lanczos-3, the
// sharpest for both shrinking and enlarging, a little slower.
enum class Filter { Nearest, Bilinear, Area, Lanczos };
const std::vector<std::string>& filterNames();  // "nearest", "bilinear", "area", "lanczos"
std::optional<Filter> parseFilter(const std::string& name);

// Bilinear downscales by more than 2x average whole source areas so they don't alias.
Image resize(const Image& img, int width, int height, Filter filter = Filter::Bilinear);
Image crop(const Image& img, int x, int y, int width, int height);  // clipped to the image
Image rotate90(const Image& img, int quarterTurnsClockwise);
Image flipHorizontal(const Image& img);
Image flipVertical(const Image& img);
// Free transform: every output pixel (x, y) is looked up at inverse * (x+0.5, y+0.5, 1) in src
// (a 3x3 matrix, row-major, so affine and perspective both work). Smooth samples bilinearly with
// premultiplied alpha; otherwise nearest. Outside src is transparent.
using Matrix3 = std::array<double, 9>;
Image warp(const Image& src, const Matrix3& inverse, int width, int height, bool smooth = true);
Matrix3 multiply(const Matrix3& a, const Matrix3& b);
bool invert(const Matrix3& m, Matrix3& out);
// The perspective map taking the four `from` corners (x,y pairs) onto the four `to` corners.
bool homography(const std::array<double, 8>& from, const std::array<double, 8>& to, Matrix3& out);
// Grows or shrinks the canvas without scaling; new area is transparent. anchorX/Y in 0..1.
Image resizeCanvas(const Image& img, int width, int height, double anchorX = 0.5, double anchorY = 0.5);

// Tonal and colour adjustments. Amounts are documented per kind in adjustmentKinds().
enum class Adjust { Brightness, Contrast, Saturation, Hue, Exposure, Gamma, Temperature, Vibrance };
const std::vector<std::string>& adjustmentKinds();
std::optional<Adjust> parseAdjust(const std::string& name);
void adjust(Image& img, Adjust kind, double amount);

void levels(Image& img, double black, double white, double gamma);  // black/white in 0..255
void autoContrast(Image& img);  // stretches each channel's 0.5%..99.5% range to full

enum class Effect { Grayscale, Invert, Sepia, Threshold, Posterize, Blur, Sharpen, Pixelate, Noise };
const std::vector<std::string>& effectKinds();
std::optional<Effect> parseEffect(const std::string& name);
// amount: blur/sharpen radius in px, threshold level 0..255, posterize levels, pixelate block size,
// noise strength 0..100; ignored for grayscale, invert and sepia.
void applyEffect(Image& img, Effect kind, double amount);

void boxBlur(Image& img, int radius);  // three passes approximate a Gaussian
void unsharpMask(Image& img, double radius, double amount);

}  // namespace wetwaresuite::raster
