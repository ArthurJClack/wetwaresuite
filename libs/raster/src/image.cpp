#include "achclwss/raster/image.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

namespace achclwss::raster {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::uint8_t clamp8(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

}  // namespace

std::optional<Color> Color::parse(const std::string& raw) {
    std::string text;
    for (char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c))) text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static const std::map<std::string, Color> names = {
        {"black", {0, 0, 0, 255}},      {"white", {255, 255, 255, 255}}, {"red", {255, 0, 0, 255}},
        {"green", {0, 128, 0, 255}},    {"lime", {0, 255, 0, 255}},      {"blue", {0, 0, 255, 255}},
        {"yellow", {255, 255, 0, 255}}, {"cyan", {0, 255, 255, 255}},    {"magenta", {255, 0, 255, 255}},
        {"gray", {128, 128, 128, 255}}, {"grey", {128, 128, 128, 255}},  {"orange", {255, 165, 0, 255}},
        {"purple", {128, 0, 128, 255}}, {"pink", {255, 192, 203, 255}},  {"brown", {165, 42, 42, 255}},
        {"transparent", {0, 0, 0, 0}},
    };
    if (auto it = names.find(text); it != names.end()) return it->second;

    if (!text.empty() && text[0] == '#') {
        std::string h = text.substr(1);
        for (char c : h) {
            if (hexDigit(c) < 0) return std::nullopt;
        }
        auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(hexDigit(h[i]) * 16 + hexDigit(h[i + 1])); };
        auto nib = [&](std::size_t i) { return static_cast<std::uint8_t>(hexDigit(h[i]) * 17); };
        if (h.size() == 3) return Color{nib(0), nib(1), nib(2), 255};
        if (h.size() == 4) return Color{nib(0), nib(1), nib(2), nib(3)};
        if (h.size() == 6) return Color{byte(0), byte(2), byte(4), 255};
        if (h.size() == 8) return Color{byte(0), byte(2), byte(4), byte(6)};
        return std::nullopt;
    }

    bool rgba = text.rfind("rgba(", 0) == 0;
    if (rgba || text.rfind("rgb(", 0) == 0) {
        if (text.back() != ')') return std::nullopt;
        std::string inner = text.substr(rgba ? 5 : 4, text.size() - (rgba ? 6 : 5));
        double v[4] = {0, 0, 0, 1};
        int n = 0;
        std::size_t start = 0;
        while (n < 4 && start <= inner.size()) {
            auto end = inner.find(',', start);
            std::string part = inner.substr(start, end == std::string::npos ? std::string::npos : end - start);
            try {
                v[n++] = std::stod(part);
            } catch (...) {
                return std::nullopt;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (n < 3) return std::nullopt;
        return Color{clamp8(v[0]), clamp8(v[1]), clamp8(v[2]), clamp8(v[3] <= 1.0 ? v[3] * 255 : v[3])};
    }
    return std::nullopt;
}

std::string Color::hex() const {
    char buf[10];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, a);
    return buf;
}

Image::Image(int width, int height, Color fill) : w_(std::max(0, width)), h_(std::max(0, height)) {
    px_.resize(static_cast<std::size_t>(w_) * h_ * 4);
    this->fill(fill);
}

Color Image::at(int x, int y) const {
    const auto* p = pixel(x, y);
    return {p[0], p[1], p[2], p[3]};
}

void Image::set(int x, int y, Color c) {
    auto* p = pixel(x, y);
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

void Image::fill(Color c) {
    for (std::size_t i = 0; i < px_.size(); i += 4) {
        px_[i] = c.r;
        px_[i + 1] = c.g;
        px_[i + 2] = c.b;
        px_[i + 3] = c.a;
    }
}

const std::vector<std::string>& blendNames() {
    static const std::vector<std::string> names = {"normal",  "multiply", "screen",  "overlay",
                                                   "add",     "darken",   "lighten", "difference"};
    return names;
}

std::optional<Blend> parseBlend(const std::string& name) {
    const auto& names = blendNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return static_cast<Blend>(i);
    }
    return std::nullopt;
}

std::string toString(Blend blend) { return blendNames()[static_cast<std::size_t>(blend)]; }

namespace {

// Blend of two channels in 0..1 (backdrop b, source s).
inline float mix(Blend mode, float b, float s) {
    switch (mode) {
        case Blend::Normal: return s;
        case Blend::Multiply: return b * s;
        case Blend::Screen: return b + s - b * s;
        case Blend::Overlay: return b <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
        case Blend::Add: return std::min(1.0f, b + s);
        case Blend::Darken: return std::min(b, s);
        case Blend::Lighten: return std::max(b, s);
        case Blend::Difference: return std::fabs(b - s);
    }
    return s;
}

}  // namespace

void blendOnto(Image& dst, const Image& src, int ox, int oy, Blend mode, double opacity) {
    const float op = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    if (op <= 0) return;
    const int x0 = std::max(0, ox), y0 = std::max(0, oy);
    const int x1 = std::min(dst.width(), ox + src.width()), y1 = std::min(dst.height(), oy + src.height());
    constexpr float k = 1.0f / 255.0f;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const auto* s = src.pixel(x - ox, y - oy);
            const float sa = s[3] * k * op;
            if (sa <= 0) continue;
            auto* d = dst.pixel(x, y);
            const float da = d[3] * k;
            const float outA = sa + da * (1 - sa);
            for (int c = 0; c < 3; ++c) {
                const float sc = s[c] * k, dc = d[c] * k;
                // W3C compositing: blended colour where both exist, source colour where backdrop is clear
                const float blended = (1 - da) * sc + da * mix(mode, dc, sc);
                const float out = (sa * blended + da * dc * (1 - sa)) / (outA > 0 ? outA : 1);
                d[c] = static_cast<std::uint8_t>(std::clamp(out, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            d[3] = static_cast<std::uint8_t>(outA * 255.0f + 0.5f);
        }
    }
}

Image composite(const std::vector<Layer>& layers, int width, int height) {
    Image out(width, height);
    for (const auto& layer : layers) {
        if (layer.visible) blendOnto(out, layer.image, 0, 0, layer.blend, layer.opacity);
    }
    return out;
}

}  // namespace achclwss::raster
