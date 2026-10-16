// Texture and sprite algorithms behind forge's image commands. Pure functions
// on raster images: nothing here knows about commands or documents.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::forge {

using raster::Color;
using raster::Image;

// ---- normal maps ----

enum class HeightSource { Luma, Red, Green, Blue, Alpha };

struct NormalMapOptions {
    double strength = 2.0;          // slope multiplier; higher = bumpier
    bool invertY = false;           // false = OpenGL (green up), true = DirectX (green down)
    bool wrap = true;               // sample across edges as if tiled (keeps tileable textures tileable)
    HeightSource source = HeightSource::Luma;
};

// Tangent-space normal map from a height image (white = high) with a 3x3 Sobel
// filter. A flat image gives (128, 128, 255) everywhere.
Image normalMap(const Image& height, const NormalMapOptions& options = {});

// ---- tiling ----

// Makes a texture tile: blends the image with a copy offset by half its size,
// so the edges come from the seamless offset copy and the middle from the
// original. `blend` is the width of the transition at each edge as a fraction
// of the image size (0.02..0.5).
Image makeSeamless(const Image& img, double blend = 0.25);
// The image repeated `tilesX` x `tilesY` times, to check that it tiles.
Image tile(const Image& img, int tilesX = 3, int tilesY = 3);
// Average colour difference across the wrap-around edges (0 = perfectly tileable).
double seamError(const Image& img);

// ---- pixel art ----

enum class QuantizeMethod { MedianCut, KMeans };

// Palette of up to `colors` colours for the image's visible pixels (alpha > 0).
std::vector<Color> buildPalette(const Image& img, int colors, QuantizeMethod method = QuantizeMethod::MedianCut);
// Maps every visible pixel to its nearest palette colour (alpha is kept),
// optionally with Floyd-Steinberg error diffusion.
void applyPalette(Image& img, const std::vector<Color>& palette, bool dither = false);
// Distinct RGB colours among visible pixels.
std::size_t countColors(const Image& img);

// Nearest-neighbour enlargement by an integer factor (crisp pixel art).
Image upscale(const Image& img, int factor);
// Colours transparent pixels next to visible ones (alpha >= threshold) with
// `color`, `thickness` px thick; `diagonal` includes corner neighbours.
// `expand` grows the canvas so the outline is never clipped.
Image outline(const Image& img, Color color, int thickness = 1, bool diagonal = false, int threshold = 128, bool expand = false);

// ---- engine prep ----

std::int64_t nextPowerOfTwo(std::int64_t v);
std::int64_t nearestPowerOfTwo(std::int64_t v);  // ties go up

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool overlaps(const Rect& o) const { return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h; }
};

// Bounds of pixels with alpha > threshold; empty when the image is fully transparent.
Rect opaqueBounds(const Image& img, int threshold = 0);

// One channel of an image as greyscale 0..255 (Luma = Rec. 601 weights).
std::vector<std::uint8_t> channel(const Image& img, HeightSource which);

// ---- packing ----

struct PackItem {
    int w = 0, h = 0;
};

struct PackResult {
    std::vector<Rect> rects;  // where each item goes, in input order (w, h as given)
    int width = 0, height = 0;
};

// Shelf packing: tallest first, left to right in rows of at most `maxWidth`
// (0 = chosen so the sheet comes out roughly square). `padding` px separate
// items from each other and from the sheet's edges. `powerOfTwo` rounds the
// sheet size up (and picks a power-of-two row width).
PackResult packShelf(const std::vector<PackItem>& items, int padding = 0, bool powerOfTwo = false, int maxWidth = 0);
// Uniform grid of cells the size of the largest item; `columns` 0 = about square.
// Every rect is a whole cell.
PackResult packGrid(const std::vector<PackItem>& items, int columns = 0, int padding = 0, bool powerOfTwo = false);

// Copies src into dst at (x, y), replacing pixels (no blending).
void blit(Image& dst, const Image& src, int x, int y);

// Orders names like a person would: walk_2 before walk_10.
bool naturalLess(const std::string& a, const std::string& b);

}  // namespace wetwaresuite::forge
