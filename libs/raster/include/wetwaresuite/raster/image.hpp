// RGBA8 images, layers and compositing: the pixel engine shared by photo,
// paint, convert and anything else that touches raster pixels. 8-bit straight
// alpha keeps memory and code small; precision-critical work can move to
// float later without changing the command layer.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wetwaresuite::raster {

struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;

    // "#rgb", "#rgba", "#rrggbb", "#rrggbbaa", "rgb(1,2,3)" or a name like "red", "transparent".
    static std::optional<Color> parse(const std::string& text);
    std::string hex() const;  // "#rrggbbaa"
    bool operator==(const Color&) const = default;
};

// A rectangle of pixels, x0/y0 inclusive, x1/y1 exclusive.
struct PixelRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    void add(const PixelRect& r);  // grows to cover r too
};

class Image {
public:
    Image() = default;
    Image(int width, int height, Color fill = {0, 0, 0, 0});

    int width() const { return w_; }
    int height() const { return h_; }
    bool empty() const { return w_ == 0 || h_ == 0; }

    std::uint8_t* data() { return px_.data(); }
    const std::uint8_t* data() const { return px_.data(); }
    std::uint8_t* pixel(int x, int y) { return px_.data() + (static_cast<std::size_t>(y) * w_ + x) * 4; }
    const std::uint8_t* pixel(int x, int y) const { return px_.data() + (static_cast<std::size_t>(y) * w_ + x) * 4; }
    Color at(int x, int y) const;
    void set(int x, int y, Color c);
    bool contains(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }

    void fill(Color c);
    std::size_t bytes() const { return px_.size(); }

private:
    int w_ = 0;
    int h_ = 0;
    std::vector<std::uint8_t> px_;
};

// One byte per pixel, 0 = out, 255 = fully in: selections and layer masks. Empty means "none".
struct Mask {
    int width = 0, height = 0;
    std::vector<std::uint8_t> v;

    Mask() = default;
    Mask(int w, int h, std::uint8_t fill = 0) : width(w), height(h), v(static_cast<std::size_t>(w) * h, fill) {}
    bool empty() const { return v.empty(); }
    std::uint8_t at(int x, int y) const { return v[static_cast<std::size_t>(y) * width + x]; }
    std::uint8_t* row(int y) { return v.data() + static_cast<std::size_t>(y) * width; }
    const std::uint8_t* row(int y) const { return v.data() + static_cast<std::size_t>(y) * width; }
    bool operator==(const Mask&) const = default;
};

// Photoshop's 27 layer blend modes. The first eight keep their old numbers (files and scripts use
// the names, not the numbers, so the order only matters inside the library).
enum class Blend {
    Normal, Multiply, Screen, Overlay, Add, Darken, Lighten, Difference,
    Dissolve, ColorBurn, LinearBurn, DarkerColor, ColorDodge, LighterColor, SoftLight, HardLight,
    VividLight, LinearLight, PinLight, HardMix, Exclusion, Subtract, Divide, Hue, Saturation, Color, Luminosity
};

// "normal", "dissolve", "darken", "multiply", ... in the order Photoshop's menu lists them.
const std::vector<std::string>& blendNames();
// Also takes "linear_dodge" (= add), "colour", dashes and spaces ("soft-light", "Soft Light").
std::optional<Blend> parseBlend(const std::string& name);
std::string toString(Blend blend);
// One blended pixel, channels 0..1: the mode's result for backdrop b and source s (both opaque).
void blendColor(Blend mode, const float b[3], const float s[3], float out[3]);

// Pixel layers hold pixels; groups hold the layers right below them (see LayerInfo::depth);
// adjustment layers change the look of everything below them without touching any pixels;
// text and shape layers keep their settings so they can be edited, plus the pixels they render to.
enum class LayerKind { Pixel, Group, Adjustment, Text, Shape };
const std::vector<std::string>& layerKindNames();  // "pixel", "group", "adjustment", "text", "shape"
std::string toString(LayerKind kind);
std::optional<LayerKind> parseLayerKind(const std::string& name);

// Layer styles (Photoshop's layer effects). They're drawn while compositing, so they follow the
// layer's pixels as you paint. Sizes and distances are px, opacities 0..1, angles degrees.
struct ShadowStyle {  // drop shadow (outside, below) or inner shadow
    bool on = false;
    Color color{0, 0, 0, 255};
    double opacity = 0.75, angle = 120, distance = 5, size = 5, spread = 0;
    Blend blend = Blend::Multiply;
    bool operator==(const ShadowStyle&) const = default;
};
struct GlowStyle {  // outer or inner glow
    bool on = false;
    Color color{255, 255, 190, 255};
    double opacity = 0.75, size = 8, spread = 0;
    Blend blend = Blend::Screen;
    bool operator==(const GlowStyle&) const = default;
};
struct StrokeStyle {
    enum class Position { Outside, Inside, Center };
    bool on = false;
    Color color{0, 0, 0, 255};
    double size = 3, opacity = 1;
    Position position = Position::Outside;
    bool operator==(const StrokeStyle&) const = default;
};
struct OverlayStyle {  // colour overlay, or a gradient overlay from color to `to`
    bool on = false;
    Color color{255, 0, 0, 255};
    Color to{255, 255, 255, 255};
    bool radial = false;
    double opacity = 1, angle = 90;
    Blend blend = Blend::Normal;
    bool operator==(const OverlayStyle&) const = default;
};
struct BevelStyle {  // bevel and emboss (inner bevel)
    bool on = false;
    double size = 5, depth = 1, angle = 120, altitude = 30, highlight = 0.75, shadow = 0.75;
    bool down = false;  // pressed in instead of raised
    bool operator==(const BevelStyle&) const = default;
};
struct LayerStyle {
    ShadowStyle dropShadow, innerShadow;
    GlowStyle outerGlow, innerGlow;
    BevelStyle bevel;
    OverlayStyle colorOverlay, gradientOverlay;
    StrokeStyle stroke;
    bool any() const {
        return dropShadow.on || innerShadow.on || outerGlow.on || innerGlow.on || bevel.on || colorOverlay.on || gradientOverlay.on ||
               stroke.on;
    }
    bool operator==(const LayerStyle&) const = default;
};
// "drop_shadow", "inner_shadow", "outer_glow", "inner_glow", "bevel", "color_overlay", "gradient_overlay", "stroke"
const std::vector<std::string>& styleEffectNames();
// How far (px) past a layer's pixels its style can draw or look, so a change there redraws enough.
int styleReach(const LayerStyle& style);
// The settings each effect has: color, opacity (0..1), angle, distance, size, spread, blend, to,
// radial, position (outside/inside/center), depth, altitude, highlight, shadow, down.
const std::vector<std::string>& styleEffectKeys(const std::string& effect);
// Turns an effect on or off; false for an unknown effect name.
bool setStyleEffect(LayerStyle& style, const std::string& effect, bool on);
// Sets one setting of one effect. False with a readable error for a bad name or value.
bool setStyleValue(LayerStyle& style, const std::string& effect, const std::string& key, const std::string& value, std::string& error);
// The effects that are on, as text ("drop_shadow:color=#000000ff,opacity=0.75,...;stroke:..."), and back.
std::string styleToText(const LayerStyle& style);
bool styleFromText(const std::string& text, LayerStyle& style, std::string& error);

// Everything about a layer except its pixels and mask: cheap to copy, so undo keeps it whole.
struct LayerInfo {
    std::string name;
    double opacity = 1.0;
    Blend blend = Blend::Normal;
    bool visible = true;
    bool maskEnabled = true;   // a disabled mask is kept but ignored
    bool clip = false;         // clipping mask: shows only where the layer below (the group's base) has pixels
    double fill = 1.0;         // fill opacity: fades the layer's own pixels but not its style
    bool lockAlpha = false;    // edits can't change the layer's transparency (paint only where there's paint)
    LayerKind kind = LayerKind::Pixel;
    // Groups are flat, like in PSD files: a group's members are the layers right below its header
    // with a bigger depth. [A depth 1, B depth 1, G depth 0 (group)] is group G holding A and B.
    int depth = 0;
    bool passThrough = true;   // groups: members blend straight into what's below (Photoshop's default)
    bool collapsed = false;    // groups: folded shut in the layers panel
    // adjustment, text and shape layers: their settings (type=..., text=..., ...), as text
    std::map<std::string, std::string> settings;
    LayerStyle style;
    bool operator==(const LayerInfo&) const = default;
};

struct Layer : LayerInfo {
    Image image;  // canvas-sized for pixel, text and shape layers; empty for groups and adjustments
    Mask mask;    // layer mask: hides where dark; empty = no mask

    Layer() = default;
    Layer(std::string name_, Image image_ = {}, double opacity_ = 1.0, Blend blend_ = Blend::Normal, bool visible_ = true)
        : image(std::move(image_)) {
        name = std::move(name_);
        opacity = opacity_;
        blend = blend_;
        visible = visible_;
    }
    bool hasPixels() const { return kind == LayerKind::Pixel || kind == LayerKind::Text || kind == LayerKind::Shape; }
};

// The layers inside group `header` are [groupStart(layers, header), header); for any other layer
// it returns `header` itself. So [groupStart(i), i] is layer i with everything it holds.
int groupStart(const std::vector<Layer>& layers, int header);
// Makes depths consistent after layers moved: no layer deeper than what sits above it allows.
void normalizeDepths(std::vector<Layer>& layers);

// Blends `src` onto `dst` at (x, y) with the given mode and opacity.
void blendOnto(Image& dst, const Image& src, int x, int y, Blend mode, double opacity);

// Flattens visible layers, bottom first, into one image: blend modes, masks, clipping, groups,
// adjustment layers and layer styles all apply.
Image composite(const std::vector<Layer>& layers, int width, int height);
// Flattens only the width x height area starting at (x, y), e.g. to refresh the part of a view that changed.
Image compositeRegion(const std::vector<Layer>& layers, int x, int y, int width, int height);

}  // namespace wetwaresuite::raster
