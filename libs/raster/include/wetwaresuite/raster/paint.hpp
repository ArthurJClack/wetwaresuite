// Brush strokes, flood fill and antialiased shapes.
#pragma once

#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

struct StrokePoint {
    double x = 0, y = 0;
    double pressure = 1.0;  // 0..1, from a tablet or 1 for a mouse
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
};

// Exponential smoothing for shaky mouse input: 0 = off, 0.9 = very smooth.
std::vector<StrokePoint> smooth(const std::vector<StrokePoint>& points, double amount);

// Paints a stroke. Overlapping dabs build up to the brush opacity, never past it.
void stroke(Image& target, const Brush& brush, const std::vector<StrokePoint>& points);

// Flood fill from (x, y) with colours within tolerance (0..255). contiguous = false
// replaces every matching pixel in the image. Returns the number of pixels filled.
std::size_t floodFill(Image& target, int x, int y, Color color, int tolerance = 32, bool contiguous = true);

struct Point {
    double x = 0, y = 0;
};

// Antialiased filled polygon (even-odd), blended normally.
void fillPolygon(Image& target, const std::vector<Point>& points, Color color);
void fillRect(Image& target, double x, double y, double w, double h, Color color);
void fillEllipse(Image& target, double cx, double cy, double rx, double ry, Color color);
// Outlines of the same shapes and a line, `width` px wide.
void strokeRect(Image& target, double x, double y, double w, double h, Color color, double width);
void strokeEllipse(Image& target, double cx, double cy, double rx, double ry, Color color, double width);
void drawLine(Image& target, double x1, double y1, double x2, double y2, Color color, double width);

// Points approximating an ellipse, for filling or stroking.
std::vector<Point> ellipsePoints(double cx, double cy, double rx, double ry);

}  // namespace wetwaresuite::raster
