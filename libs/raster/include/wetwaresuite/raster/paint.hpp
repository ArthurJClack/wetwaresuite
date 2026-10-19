// Brush strokes, flood fill and antialiased shapes.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

struct StrokePoint {
    double x = 0, y = 0;
    double pressure = 1.0;  // 0..1, from a tablet or 1 for a mouse
};

// Painting mirrored or turned copies of every stroke at once.
struct Symmetry {
    bool mirrorX = false;      // mirror left-right, across the vertical line through the centre
    bool mirrorY = false;      // mirror top-bottom
    int radial = 1;            // copies turned evenly around the centre (1 = none, e.g. 6 for a snowflake)
    double cx = -1, cy = -1;   // the centre; negative = the middle of the canvas
    bool on() const { return mirrorX || mirrorY || radial > 1; }
};

struct Brush {
    Color color;
    double size = 10;       // diameter in px at full pressure
    double hardness = 0.8;  // 0 = soft airbrush edge, 1 = hard edge
    double opacity = 1.0;   // cap for the whole stroke
    double flow = 1.0;      // how much each dab adds
    double spacing = 0.15;  // distance between dabs as a fraction of size
    bool erase = false;
    bool pressureSize = true;
    bool pressureOpacity = false;
    double scatter = 0;                 // dabs land up to scatter * size off the path (0..2)
    std::shared_ptr<const Mask> tip;    // tip shape from an image (255 = full paint), stretched over each dab
    std::shared_ptr<const Mask> texture;  // paper grain tiled over the canvas: paint takes where it's bright
    double textureDepth = 1;            // 0..1, how much the texture shows
    Symmetry symmetry;
    std::uint32_t seed = 1;             // scatter is random but repeats for the same seed (so redo matches)
};

// One dab of a stroke: where it lands, its radius and how much paint it puts down (0..1).
struct Dab {
    double x = 0, y = 0, radius = 1, alpha = 1;
};
// Every dab a stroke puts down, in order, with symmetry and scatter applied. width/height is the
// canvas (for the symmetry centre).
std::vector<Dab> strokeDabs(const Brush& brush, const std::vector<StrokePoint>& points, int width, int height);
// How much of a stroke covers each pixel (0..1), in `rect` row by row; built from the dabs,
// the tip, the texture and the hardness. Brush opacity is not applied.
struct Coverage {
    PixelRect rect;
    std::vector<float> v;
    float at(int x, int y) const { return v[static_cast<std::size_t>(y - rect.y0) * rect.width() + (x - rect.x0)]; }
};
Coverage strokeCoverage(int width, int height, const Brush& brush, const std::vector<StrokePoint>& points);
// Adds one dab to a coverage (for tools that work dab by dab, like smudge).
void addDab(Coverage& cover, const Brush& brush, const Dab& dab);
// A shared tip or texture from an image: its brightness times its alpha (white = full paint).
std::shared_ptr<const Mask> maskFromImage(const Image& img, bool invert = false);

// Exponential smoothing for shaky mouse input: 0 = off, 0.9 = very smooth.
std::vector<StrokePoint> smooth(const std::vector<StrokePoint>& points, double amount);

// Paints a stroke. Overlapping dabs build up to the brush opacity, never past it.
void stroke(Image& target, const Brush& brush, const std::vector<StrokePoint>& points);
// The pixels stroke() may change (clipped to the image), so undo can save just that area.
PixelRect strokeBounds(const Image& target, const Brush& brush, const std::vector<StrokePoint>& points);

// Flood fill from (x, y) with colours within tolerance (0..255). contiguous = false
// replaces every matching pixel in the image. Returns the number of pixels filled.
std::size_t floodFill(Image& target, int x, int y, Color color, int tolerance = 32, bool contiguous = true);

struct Point {
    double x = 0, y = 0;
};

// Antialiased filled polygon (even-odd), blended normally.
void fillPolygon(Image& target, const std::vector<Point>& points, Color color);
// Several outlines filled as one shape (letters with holes): nonzero = holes only where an inner
// outline runs the other way (fonts), otherwise even-odd. Coverage 0..1 per pixel, canvas sized.
std::vector<float> polygonCoverage(int width, int height, const std::vector<std::vector<Point>>& contours, bool nonzero);
void fillPolygons(Image& target, const std::vector<std::vector<Point>>& contours, Color color, bool nonzero = true);
void fillRect(Image& target, double x, double y, double w, double h, Color color);
void fillEllipse(Image& target, double cx, double cy, double rx, double ry, Color color);
// Outlines of the same shapes and a line, `width` px wide.
void strokeRect(Image& target, double x, double y, double w, double h, Color color, double width);
void strokeEllipse(Image& target, double cx, double cy, double rx, double ry, Color color, double width);
void drawLine(Image& target, double x1, double y1, double x2, double y2, Color color, double width);

// Points approximating an ellipse, for filling or stroking.
std::vector<Point> ellipsePoints(double cx, double cy, double rx, double ry);

}  // namespace wetwaresuite::raster
