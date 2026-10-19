// Retouching brushes (clone stamp, dodge/burn/sponge, smudge, blur/sharpen,
// colour replacement), patterns and multi-colour gradients. Brushes use the
// same dabs as paint strokes (size, hardness, spacing, flow, scatter, tip,
// texture, symmetry), and only touch strokeBounds(), so undo stays small.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::raster {

// Clone stamp: under the brush, pixel (x, y) takes source(x + dx, y + dy). `source` is usually a
// copy of the layer (or the flattened picture) from before the stroke, the same size as target.
void cloneStroke(Image& target, const Image& source, int dx, int dy, const Brush& brush, const std::vector<StrokePoint>& points);

enum class ToneTool { Dodge, Burn, Sponge };
enum class ToneRange { Shadows, Midtones, Highlights };
// Dodge lightens, burn darkens (mostly in `range`), sponge saturates or (saturate=false) desaturates.
// exposure 0..1 is the strength.
void toneStroke(Image& target, ToneTool tool, ToneRange range, double exposure, bool saturate, const Brush& brush,
                const std::vector<StrokePoint>& points);

// Smudge: drags the colours under the brush along the stroke. strength 0..1 (1 = carries the first
// colours all the way). fingerPaint starts with the brush colour instead of what's under it.
void smudgeStroke(Image& target, double strength, bool fingerPaint, const Brush& brush, const std::vector<StrokePoint>& points);

// Blur or sharpen under the brush; strength 0..1.
void blurStroke(Image& target, bool sharpen, double strength, const Brush& brush, const std::vector<StrokePoint>& points);

// Colour replacement: pixels like the one under the first point (within tolerance 0..255) take the
// hue and saturation of brush.color, keeping their brightness.
void replaceColorStroke(Image& target, int tolerance, const Brush& brush, const std::vector<StrokePoint>& points);

// Patterns: a tile from a built-in name (checker, stripes, diagonal, dots, grid, bricks, noise,
// hexagons), `size` px, in two colours.
const std::vector<std::string>& patternNames();
Image builtinPattern(const std::string& name, int size, Color a, Color b);
// The tile repeated over a width x height image, shifted by (offsetX, offsetY), scaled by `scale`.
Image tilePattern(const Image& tile, int width, int height, int offsetX = 0, int offsetY = 0, double scale = 1.0);

// Gradients with any number of colour stops (positions 0..1).
struct GradientStop {
    double pos = 0;
    Color color;
};
enum class GradientShape { Linear, Radial, Angle, Reflected, Diamond };
const std::vector<std::string>& gradientShapeNames();  // linear, radial, angle, reflected, diamond
std::optional<GradientShape> parseGradientShape(const std::string& name);
// Built-in gradients: black_white, rainbow, sunset, ocean, fire, chrome, copper, pastel, violet_orange,
// transparent_stripes.
const std::vector<std::string>& gradientPresetNames();
std::vector<GradientStop> gradientPreset(const std::string& name);
// Colours spread evenly ("#000000,#ff0000,#ffffff"), or with positions ("#000000@0,#ff0000@30,#fff@100").
bool parseGradientStops(const std::string& text, std::vector<GradientStop>& out, std::string& error);
// Draws the gradient from (x1, y1) to (x2, y2) over img with a blend mode and opacity (0..1).
void drawGradient(Image& img, std::vector<GradientStop> stops, GradientShape shape, double x1, double y1, double x2, double y2,
                  double opacity = 1.0, Blend blend = Blend::Normal, bool reverse = false);

}  // namespace wetwaresuite::raster
