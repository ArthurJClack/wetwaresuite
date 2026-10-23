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

namespace {

// Photoshop's menu order.
const std::vector<std::pair<std::string, Blend>>& blendTable() {
    static const std::vector<std::pair<std::string, Blend>> t = {
        {"normal", Blend::Normal},         {"dissolve", Blend::Dissolve},       {"darken", Blend::Darken},
        {"multiply", Blend::Multiply},     {"color_burn", Blend::ColorBurn},    {"linear_burn", Blend::LinearBurn},
        {"darker_color", Blend::DarkerColor}, {"lighten", Blend::Lighten},      {"screen", Blend::Screen},
        {"color_dodge", Blend::ColorDodge}, {"add", Blend::Add},                {"lighter_color", Blend::LighterColor},
        {"overlay", Blend::Overlay},       {"soft_light", Blend::SoftLight},    {"hard_light", Blend::HardLight},
        {"vivid_light", Blend::VividLight}, {"linear_light", Blend::LinearLight}, {"pin_light", Blend::PinLight},
        {"hard_mix", Blend::HardMix},      {"difference", Blend::Difference},   {"exclusion", Blend::Exclusion},
        {"subtract", Blend::Subtract},     {"divide", Blend::Divide},           {"hue", Blend::Hue},
        {"saturation", Blend::Saturation}, {"color", Blend::Color},             {"luminosity", Blend::Luminosity},
    };
    return t;
}

}  // namespace

const std::vector<std::string>& blendNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& [n, b] : blendTable()) v.push_back(n);
        return v;
    }();
    return names;
}

std::optional<Blend> parseBlend(const std::string& raw) {
    std::string name;
    for (char c : raw) {
        if (c == '-' || c == ' ') name += '_';
        else name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (auto at = name.find("colour"); at != std::string::npos) name.replace(at, 6, "color");
    if (name == "linear_dodge" || name == "plus") return Blend::Add;
    for (const auto& [n, b] : blendTable()) {
        if (n == name) return b;
    }
    return std::nullopt;
}

std::string toString(Blend blend) {
    for (const auto& [n, b] : blendTable()) {
        if (b == blend) return n;
    }
    return "normal";
}

const std::vector<std::string>& layerKindNames() {
    static const std::vector<std::string> names = {"pixel", "group", "adjustment", "text", "shape"};
    return names;
}

std::string toString(LayerKind kind) { return layerKindNames()[static_cast<std::size_t>(kind)]; }

std::optional<LayerKind> parseLayerKind(const std::string& name) {
    const auto& names = layerKindNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return static_cast<LayerKind>(i);
    }
    return std::nullopt;
}

const std::vector<std::string>& styleEffectNames() {
    static const std::vector<std::string> names = {"drop_shadow", "inner_shadow", "outer_glow", "inner_glow",
                                                   "bevel",       "color_overlay", "gradient_overlay", "stroke"};
    return names;
}

int groupStart(const std::vector<Layer>& layers, int header) {
    if (header < 0 || header >= static_cast<int>(layers.size())) return header;
    const Layer& h = layers[static_cast<std::size_t>(header)];
    if (h.kind != LayerKind::Group) return header;
    int s = header;
    while (s > 0 && layers[static_cast<std::size_t>(s - 1)].depth > h.depth) --s;
    return s;
}

void normalizeDepths(std::vector<Layer>& layers) {
    int allowed = 0;  // the deepest the next layer down may be
    for (std::size_t i = layers.size(); i-- > 0;) {
        Layer& l = layers[i];
        l.depth = std::clamp(l.depth, 0, allowed);
        allowed = l.kind == LayerKind::Group ? l.depth + 1 : l.depth;
    }
}

namespace {

// Blend of two channels in 0..1 (backdrop b, source s), for the separable modes.
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
        case Blend::ColorBurn:
            if (b >= 1) return 1;
            if (s <= 0) return 0;
            return 1 - std::min(1.0f, (1 - b) / s);
        case Blend::LinearBurn: return std::max(0.0f, b + s - 1);
        case Blend::ColorDodge:
            if (b <= 0) return 0;
            if (s >= 1) return 1;
            return std::min(1.0f, b / (1 - s));
        case Blend::SoftLight: {
            if (s <= 0.5f) return b - (1 - 2 * s) * b * (1 - b);
            const float d = b <= 0.25f ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
            return b + (2 * s - 1) * (d - b);
        }
        case Blend::HardLight: return s <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
        case Blend::VividLight:
            if (s <= 0.5f) return mix(Blend::ColorBurn, b, 2 * s);
            return mix(Blend::ColorDodge, b, 2 * (s - 0.5f));
        case Blend::LinearLight: return std::clamp(b + 2 * s - 1, 0.0f, 1.0f);
        case Blend::PinLight: return s <= 0.5f ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
        case Blend::HardMix: return b + s >= 1 ? 1.0f : 0.0f;
        case Blend::Exclusion: return b + s - 2 * b * s;
        case Blend::Subtract: return std::max(0.0f, b - s);
        case Blend::Divide:
            if (s <= 0) return b <= 0 ? 0.0f : 1.0f;
            return std::min(1.0f, b / s);
        default: return s;
    }
}

// The non-separable modes (W3C compositing spec): hue, saturation, colour and luminosity.
inline float lum(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

void clipColor(float c[3]) {
    const float l = lum(c);
    const float n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
    for (int i = 0; i < 3; ++i) {
        if (n < 0 && l - n > 0) c[i] = l + (c[i] - l) * l / (l - n);
        if (x > 1 && x - l > 0) c[i] = l + (c[i] - l) * (1 - l) / (x - l);
    }
}

void setLum(const float c[3], float l, float out[3]) {
    const float d = l - lum(c);
    for (int i = 0; i < 3; ++i) out[i] = c[i] + d;
    clipColor(out);
}

float sat(const float c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

void setSat(const float c[3], float s, float out[3]) {
    int mx = 0, mn = 0;
    for (int i = 1; i < 3; ++i) {
        if (c[i] > c[mx]) mx = i;
        if (c[i] < c[mn]) mn = i;
    }
    if (mx == mn) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    const int md = 3 - mx - mn;
    const float range = c[mx] - c[mn];
    out[md] = (c[md] - c[mn]) * s / range;
    out[mx] = s;
    out[mn] = 0;
}

}  // namespace

void blendColor(Blend mode, const float b[3], const float s[3], float out[3]) {
    switch (mode) {
        case Blend::Hue: {
            float t[3];
            setSat(s, sat(b), t);
            setLum(t, lum(b), out);
            return;
        }
        case Blend::Saturation: {
            float t[3];
            setSat(b, sat(s), t);
            setLum(t, lum(b), out);
            return;
        }
        case Blend::Color: setLum(s, lum(b), out); return;
        case Blend::Luminosity: setLum(b, lum(s), out); return;
        case Blend::DarkerColor:
            for (int i = 0; i < 3; ++i) out[i] = lum(s) < lum(b) ? s[i] : b[i];
            return;
        case Blend::LighterColor:
            for (int i = 0; i < 3; ++i) out[i] = lum(s) > lum(b) ? s[i] : b[i];
            return;
        case Blend::Dissolve:
            for (int i = 0; i < 3; ++i) out[i] = s[i];
            return;
        default:
            for (int i = 0; i < 3; ++i) out[i] = mix(mode, b[i], s[i]);
    }
}

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

// A stable per-pixel number in [0, 1) for dissolve, so redraws and redo give the same speckle.
inline float dissolveNoise(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xffffff) / 16777216.0f;
}

// The modes added after the first eight: scalar only (they're rarer, and some mix all three channels).
void blendRowGeneric(Blend mode, std::uint8_t* __restrict d, const std::uint8_t* __restrict s, int n, float op, int sx, int sy) {
    constexpr float k = 1.0f / 255.0f;
    for (int i = 0; i < n; ++i, d += 4, s += 4) {
        float sa = s[3] * k * op;
        if (mode == Blend::Dissolve) sa = sa > dissolveNoise(sx + i, sy) ? 1.0f : 0.0f;
        if (!(sa > 0)) continue;
        const float da = d[3] * k;
        const float outA = sa + da * (1 - sa);
        const float div = outA > 0 ? outA : 1;
        const float sc[3] = {s[0] * k, s[1] * k, s[2] * k}, dc[3] = {d[0] * k, d[1] * k, d[2] * k};
        float m[3];
        blendColor(mode, dc, sc, m);
        for (int c = 0; c < 3; ++c) {
            const float blended = (1 - da) * sc[c] + da * m[c];
            const float out = (sa * blended + da * dc[c] * (1 - sa)) / div;
            d[c] = static_cast<std::uint8_t>(std::clamp(out, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        d[3] = static_cast<std::uint8_t>(outA * 255.0f + 0.5f);
    }
}

void blendRectGeneric(Blend mode, Image& dst, const Image& src, int ox, int oy, int x0, int y0, int x1, int y1, float op) {
    parallelRows(y1 - y0, static_cast<std::size_t>(x1 - x0) * 8, [&](int b, int e) {
        for (int y = y0 + b; y < y0 + e; ++y) {
            blendRowGeneric(mode, dst.pixel(x0, y), src.pixel(x0 - ox, y - oy), x1 - x0, op, x0 - ox, y - oy);
        }
    });
}

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
        default: blendRectGeneric(mode, dst, src, ox, oy, x0, y0, x1, y1, op); return;
    }
}

}  // namespace wetwaresuite::raster
