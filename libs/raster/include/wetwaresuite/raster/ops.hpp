// Geometry, adjustments and filters on whole images. Everything returns a new
// image or edits in place; nothing here knows about commands or documents.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

enum class Filter { Nearest, Bilinear };

// Bilinear downscales by more than 2x average whole source areas so they don't alias.
Image resize(const Image& img, int width, int height, Filter filter = Filter::Bilinear);
Image crop(const Image& img, int x, int y, int width, int height);  // clipped to the image
Image rotate90(const Image& img, int quarterTurnsClockwise);
Image flipHorizontal(const Image& img);
Image flipVertical(const Image& img);
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
