// RGBA8 images, layers and compositing: the pixel engine shared by photo,
// paint, convert and anything else that touches raster pixels. 8-bit straight
// alpha keeps memory and code small; precision-critical work can move to
// float later without changing the command layer.
#pragma once

#include <cstdint>
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

enum class Blend { Normal, Multiply, Screen, Overlay, Add, Darken, Lighten, Difference };

const std::vector<std::string>& blendNames();
std::optional<Blend> parseBlend(const std::string& name);
std::string toString(Blend blend);

struct Layer {
    std::string name;
    Image image;
    double opacity = 1.0;
    Blend blend = Blend::Normal;
    bool visible = true;
};

// Blends `src` onto `dst` at (x, y) with the given mode and opacity.
void blendOnto(Image& dst, const Image& src, int x, int y, Blend mode, double opacity);

// Flattens visible layers, bottom first, into one image.
Image composite(const std::vector<Layer>& layers, int width, int height);
// Flattens only the width x height area starting at (x, y), e.g. to refresh the part of a view that changed.
Image compositeRegion(const std::vector<Layer>& layers, int x, int y, int width, int height);

}  // namespace wetwaresuite::raster
