#include "wetwaresuite/raster/image.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "wetwaresuite/raster/parallel.hpp"
#include "wetwaresuite/raster/simd.hpp"
#include "simd_ops.hpp"

namespace wetwaresuite::raster {

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
        {"gray", {128, 128, 128, 255}}, {"grey", {128, 128, 128, 255}},  {"orange", {255, 165, 0, 255}}, {"gold", {255, 215, 0, 255}},
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

void PixelRect::add(const PixelRect& r) {
    if (r.empty()) return;
    if (empty()) {
        *this = r;
        return;
    }
    x0 = std::min(x0, r.x0);
    y0 = std::min(y0, r.y0);
    x1 = std::max(x1, r.x1);
    y1 = std::max(y1, r.y1);
}

Image::Image(int width, int height, Color fill) : w_(std::max(0, width)), h_(std::max(0, height)) {
    px_.resize(static_cast<std::size_t>(w_) * h_ * 4);  // zeroed: transparent needs no second pass
    if (fill.r || fill.g || fill.b || fill.a) this->fill(fill);
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

namespace {

// One row of blendOnto with the mode fixed at compile time so the inner loop has no switch.
template <Blend Mode>
void blendRow(std::uint8_t* __restrict d, const std::uint8_t* __restrict s, int n, float op) {
    constexpr float k = 1.0f / 255.0f;
    for (int i = 0; i < n; ++i, d += 4, s += 4) {
        const float sa = s[3] * k * op;
        const float da = d[3] * k;
        const float outA = sa + da * (1 - sa);
        const float div = outA > 0 ? outA : 1;
        const bool keep = !(sa > 0);  // fully clear source pixels leave the backdrop exactly as it was
        for (int c = 0; c < 3; ++c) {
            const float sc = s[c] * k, dc = d[c] * k;
            // W3C compositing: blended colour where both exist, source colour where backdrop is clear
            const float blended = (1 - da) * sc + da * mix(Mode, dc, sc);
            const float out = (sa * blended + da * dc * (1 - sa)) / div;
            const auto v = static_cast<std::uint8_t>(std::clamp(out, 0.0f, 1.0f) * 255.0f + 0.5f);
            d[c] = keep ? d[c] : v;
        }
        const auto a = static_cast<std::uint8_t>(outA * 255.0f + 0.5f);
        d[3] = keep ? d[3] : a;
    }
}

#if WW_SIMD
// blendRow with one pixel per register: the same operations in the same order, so the same bytes.
template <Blend Mode>
simd::F4 mixV(simd::F4 b, simd::F4 s) {
    using namespace simd;
    if constexpr (Mode == Blend::Normal) return s;
    else if constexpr (Mode == Blend::Multiply) return b * s;
    else if constexpr (Mode == Blend::Screen) return b + s - b * s;
    else if constexpr (Mode == Blend::Overlay) {
        const F4 one = set1(1.0f), two = set1(2.0f);
        return select(lessEq(b, set1(0.5f)), two * b * s, one - two * (one - b) * (one - s));
    } else if constexpr (Mode == Blend::Add) return vmin(set1(1.0f), b + s);
    else if constexpr (Mode == Blend::Darken) return vmin(b, s);
    else if constexpr (Mode == Blend::Lighten) return vmax(b, s);
    else return vabs(b - s);
}

template <Blend Mode>
void blendRowSimd(std::uint8_t* __restrict d, const std::uint8_t* __restrict s, int n, float op) {
    using namespace simd;
    constexpr float k = 1.0f / 255.0f;
    const F4 kv = set1(k), one = set1(1.0f), zero = set1(0.0f), scale = set1(255.0f), half = set1(0.5f);
    for (int i = 0; i < n; ++i, d += 4, s += 4) {
        const float sa = s[3] * k * op;
        if (!(sa > 0)) continue;  // fully clear source pixels leave the backdrop exactly as it was
        const float da = d[3] * k;
        const float outA = sa + da * (1 - sa);
        const float div = outA > 0 ? outA : 1;
        const F4 sc = loadU8(s) * kv, dc = loadU8(d) * kv;
        const F4 sav = set1(sa), dav = set1(da);
        const F4 blended = (one - dav) * sc + dav * mixV<Mode>(dc, sc);
        const F4 out = (sav * blended + dav * dc * (one - sav)) / set1(div);
        const F4 v = withLast(vmin(vmax(out, zero), one), set1(outA));
        storeU8(d, v * scale + half);
    }
}
#endif

template <Blend Mode>
void blendRect(Image& dst, const Image& src, int ox, int oy, int x0, int y0, int x1, int y1, float op) {
    parallelRows(y1 - y0, static_cast<std::size_t>(x1 - x0) * 4, [&](int b, int e) {
        for (int y = y0 + b; y < y0 + e; ++y) {
#if WW_SIMD
            if (simdEnabled()) {
                blendRowSimd<Mode>(dst.pixel(x0, y), src.pixel(x0 - ox, y - oy), x1 - x0, op);
                continue;
            }
#endif
            blendRow<Mode>(dst.pixel(x0, y), src.pixel(x0 - ox, y - oy), x1 - x0, op);
        }
    });
}

}  // namespace

void blendOnto(Image& dst, const Image& src, int ox, int oy, Blend mode, double opacity) {
    const float op = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    if (op <= 0) return;
    const int x0 = std::max(0, ox), y0 = std::max(0, oy);
    const int x1 = std::min(dst.width(), ox + src.width()), y1 = std::min(dst.height(), oy + src.height());
    if (x1 <= x0 || y1 <= y0) return;
    switch (mode) {
        case Blend::Normal: blendRect<Blend::Normal>(dst, src, ox, oy, x0, y0, x1, y1, op); return;
        case Blend::Multiply: blendRect<Blend::Multiply>(dst, src, ox, oy, x0, y0, x1, y1, op); return;
        case Blend::Screen: blendRect<Blend::Screen>(dst, src, ox, oy, x0, y0, x1, y1, op); return;
        case Blend::Overlay: blendRect<Blend::Overlay>(dst, src, ox, oy, x0, y0, x1, y1, op); return;
        case Blend::Add: blendRect<Blend::Add>(dst, src, ox, oy, x0, y0, x1, y1, op); return;
        case Blend::Darken: blendRect<Blend::Darken>(dst, src, ox, oy, x0, y0, x1, y1, op); return;
        case Blend::Lighten: blendRect<Blend::Lighten>(dst, src, ox, oy, x0, y0, x1, y1, op); return;
        case Blend::Difference: blendRect<Blend::Difference>(dst, src, ox, oy, x0, y0, x1, y1, op); return;
    }
}

Image composite(const std::vector<Layer>& layers, int width, int height) {
    return compositeRegion(layers, 0, 0, width, height);
}

namespace {

bool usesMask(const Layer& l) { return !l.mask.empty() && l.maskEnabled; }

// The layer's alpha over the region after its own mask, 0..255, one byte per pixel.
std::vector<std::uint8_t> effectiveAlpha(const Layer& l, int x, int y, int w, int h) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * h, 0);
    for (int row = 0; row < h; ++row) {
        const int sy = y + row;
        if (sy < 0 || sy >= l.image.height()) continue;
        for (int col = 0; col < w; ++col) {
            const int sx = x + col;
            if (sx < 0 || sx >= l.image.width()) continue;
            int v = l.image.pixel(sx, sy)[3];
            if (usesMask(l)) v = sx < l.mask.width && sy < l.mask.height ? v * l.mask.at(sx, sy) / 255 : 0;
            a[static_cast<std::size_t>(row) * w + col] = static_cast<std::uint8_t>(v);
        }
    }
    return a;
}

}  // namespace

Image compositeRegion(const std::vector<Layer>& layers, int x, int y, int width, int height) {
    Image out(width, height);
    int base = -1;  // the layer clipping masks above clip to
    std::vector<std::uint8_t> baseAlpha;
    bool baseAlphaReady = false;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& layer = layers[i];
        if (!layer.clip || base < 0) {
            base = static_cast<int>(i);
            baseAlphaReady = false;
        }
        const bool clipped = layer.clip && base != static_cast<int>(i);
        if (!layer.visible || (clipped && !layers[static_cast<std::size_t>(base)].visible)) continue;
        if (!usesMask(layer) && !clipped) {
            blendOnto(out, layer.image, -x, -y, layer.blend, layer.opacity);
            continue;
        }
        // masked or clipped: a copy of the region with its alpha cut down
        Image part(width, height);
        std::vector<std::uint8_t> alpha = effectiveAlpha(layer, x, y, width, height);
        if (clipped && !baseAlphaReady) {
            baseAlpha = effectiveAlpha(layers[static_cast<std::size_t>(base)], x, y, width, height);
            baseAlphaReady = true;
        }
        for (int row = 0; row < height; ++row) {
            const int sy = y + row;
            if (sy < 0 || sy >= layer.image.height()) continue;
            for (int col = 0; col < width; ++col) {
                const int sx = x + col;
                if (sx < 0 || sx >= layer.image.width()) continue;
                const std::size_t k = static_cast<std::size_t>(row) * width + col;
                int a = alpha[k];
                if (clipped) a = a * baseAlpha[k] / 255;
                std::uint8_t* d = part.pixel(col, row);
                const std::uint8_t* src = layer.image.pixel(sx, sy);
                d[0] = src[0], d[1] = src[1], d[2] = src[2], d[3] = static_cast<std::uint8_t>(a);
            }
        }
        blendOnto(out, part, 0, 0, layer.blend, layer.opacity);
    }
    return out;
}

}  // namespace wetwaresuite::raster
