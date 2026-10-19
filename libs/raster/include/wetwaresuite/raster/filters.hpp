// The Filter menu: blurs (gaussian, motion, radial spin/zoom), sharpening
// (unsharp mask with threshold, high pass), noise (add, median, despeckle,
// dust & scratches), stylize (emboss, find edges, oil paint), render (clouds),
// distortion (displace, lens correction, warp grid, liquify) and the masks
// behind "select subject" and "refine edge". Everything edits in place or
// returns a new image; nothing here knows about documents.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

// Gaussian blur, radius ~ the standard deviation in px (three box passes, fast at any size).
void gaussianBlur(Image& img, double radius);
// Blur along a line: angle in degrees (0 = horizontal), distance = streak length in px.
void motionBlur(Image& img, double angle, double distance);
// Radial blur around (cx, cy): spin turns by up to `amount` degrees, zoom streaks outwards
// (amount 0..100, % of the distance to the centre).
void spinBlur(Image& img, double cx, double cy, double amount);
void zoomBlur(Image& img, double cx, double cy, double amount);

// Unsharp mask: amount in % (100 = double the detail), radius px; only differences above
// `threshold` (0..255) are sharpened, so smooth skin and sky stay smooth.
void unsharpMaskThreshold(Image& img, double radius, double amount, int threshold);
// High pass: keeps the detail finer than radius on mid gray (for overlay sharpening).
void highPass(Image& img, double radius);

// Median: each pixel becomes the middle value of its neighbourhood (removes specks, keeps edges).
void medianFilter(Image& img, int radius);
// Dust & scratches: median, but only where the pixel differs from it by more than threshold.
void dustAndScratches(Image& img, int radius, int threshold);
// Adds noise: amount 0..100; gaussian (else uniform); monochrome = the same for r, g, b.
void addNoise(Image& img, double amount, bool gaussian, bool monochrome, std::uint32_t seed);

// Emboss: a gray relief lit from `angle` degrees, height px, amount %.
void emboss(Image& img, double angle, double height, double amount);
// Find edges: dark lines where colours change, white elsewhere.
void findEdges(Image& img);
// Oil paint (Kuwahara): flat strokes the size of radius, edges kept.
void oilPaint(Image& img, int radius);

// Clouds: soft fractal noise between two colours; scale = the size of the biggest puffs in px.
void renderClouds(Image& img, Color a, Color b, double scale, std::uint32_t seed);

// Displace: map's red moves pixels sideways, green up and down (128 = stay), by up to scale px.
Image displace(const Image& img, const Image& map, double scaleX, double scaleY, bool wrap);
// Lens correction: distortion -100..100 (negative fixes barrel, positive fixes pincushion),
// vignette -100..100 (positive brightens dark corners), chromatic -10..10 px of red/blue fringe fix.
Image lensCorrect(const Image& img, double distortion, double vignette, double chromatic);
// Warp grid: (cols + 1) x (rows + 1) control points spread evenly over the image; `moves` holds
// how far each one moves (dx, dy pairs, row by row from the top-left). The picture bends smoothly.
Image warpGrid(const Image& img, int cols, int rows, const std::vector<double>& moves);

// Liquify: a brush that pushes pixels around. Push drags them along the stroke, bloat swells,
// pucker shrinks, twirl turns them (clockwise, or counter-clockwise for negative pressure).
enum class LiquifyMode { Push, Bloat, Pucker, TwirlRight, TwirlLeft };
// points are x,y pairs; size = brush diameter px; pressure 0..1.
Image liquify(const Image& img, LiquifyMode mode, const std::vector<double>& points, double size, double pressure);

// "Select subject": a soft mask of the main thing in the picture, guessed from how unlike the
// border colours it is and where it sits (works for objects on plain or simple backgrounds).
Mask subjectMask(const Image& img, int tolerance);
// Refine edge: snaps a mask's edge to the picture's edges (a guided filter), radius px, then
// smooth (px), contrast 0..100 (hardens the edge) and shift -100..100 (grows or shrinks it).
Mask refineEdge(const Mask& mask, const Image& guide, int radius, double smooth, double contrast, double shift);

}  // namespace wetwaresuite::raster
