// Layer styles, drawn while compositing: drop shadow and outer glow go under the layer, colour
// and gradient overlays, inner glow, inner shadow and bevel change the layer's own colours, and the
// stroke goes on top. Each works on the layer's alpha over the area being drawn plus a margin
// (styleReach), so a small redraw gives exactly the pixels a full one does.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "composite_internal.hpp"
#include "wetwaresuite/raster/parallel.hpp"

namespace wetwaresuite::raster {

namespace {

constexpr double kPi = 3.14159265358979323846;

int blurRadius(double size) { return std::max(1, static_cast<int>(std::lround(size / 2))); }
int blurReach(double size) { return 3 * blurRadius(size) + 1; }

// Three box passes (close to a Gaussian) over a float field, rows then columns.
void blurField(std::vector<float>& f, int w, int h, int r) {
    std::vector<float> tmp(f.size());
    auto line = [r](const float* in, float* out, int n, std::ptrdiff_t stride) {
        double acc = 0;
        const double inv = 1.0 / (2 * r + 1);
        auto get = [&](int i) { return i < 0 || i >= n ? 0.0 : static_cast<double>(in[i * stride]); };
        for (int i = -r; i <= r; ++i) acc += get(i);
        for (int i = 0; i < n; ++i) {
            out[i * stride] = static_cast<float>(acc * inv);
            acc += get(i + r + 1) - get(i - r);
        }
    };
    for (int pass = 0; pass < 3; ++pass) {
        parallelRows(h, static_cast<std::size_t>(w), [&](int b, int e) {
            for (int y = b; y < e; ++y) line(f.data() + static_cast<std::size_t>(y) * w, tmp.data() + static_cast<std::size_t>(y) * w, w, 1);
        });
        parallelRows(w, static_cast<std::size_t>(h), [&](int b, int e) {
            for (int x = b; x < e; ++x) line(tmp.data() + x, f.data() + x, h, w);
        });
    }
}

// Exact Euclidean distance from every cell to the nearest cell where `feature` is set
// (Felzenszwalb & Huttenlocher), in px.
std::vector<float> distanceTo(const std::vector<std::uint8_t>& feature, int w, int h) {
    constexpr float kInf = 1e20f;
    std::vector<float> d(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = feature[i] ? 0.0f : kInf;
    auto pass = [kInf](float* f, int n, std::ptrdiff_t stride, std::vector<float>& src, std::vector<float>& out, std::vector<int>& v,
                       std::vector<float>& z) {
        for (int i = 0; i < n; ++i) src[static_cast<std::size_t>(i)] = f[i * stride];
        int k = 0;
        v[0] = 0;
        z[0] = -kInf;
        z[1] = kInf;
        for (int q = 1; q < n; ++q) {
            float s;
            while (true) {
                const int p = v[static_cast<std::size_t>(k)];
                s = ((src[static_cast<std::size_t>(q)] + static_cast<float>(q) * q) - (src[static_cast<std::size_t>(p)] + static_cast<float>(p) * p)) /
                    (2.0f * static_cast<float>(q - p));
                if (s <= z[static_cast<std::size_t>(k)] && k > 0) {
                    --k;
                    continue;
                }
                break;
            }
            ++k;
            v[static_cast<std::size_t>(k)] = q;
            z[static_cast<std::size_t>(k)] = s;
            z[static_cast<std::size_t>(k) + 1] = kInf;
        }
        k = 0;
        for (int q = 0; q < n; ++q) {
            while (z[static_cast<std::size_t>(k) + 1] < static_cast<float>(q)) ++k;
            const int p = v[static_cast<std::size_t>(k)];
            out[static_cast<std::size_t>(q)] = static_cast<float>(q - p) * (q - p) + src[static_cast<std::size_t>(p)];
        }
        for (int i = 0; i < n; ++i) f[i * stride] = out[static_cast<std::size_t>(i)];
    };
    const int n = std::max(w, h);
    parallelRows(w, static_cast<std::size_t>(h), [&](int b, int e) {
        std::vector<float> src(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1);
        std::vector<int> v(static_cast<std::size_t>(n));
        for (int x = b; x < e; ++x) pass(d.data() + x, h, w, src, out, v, z);
    });
    parallelRows(h, static_cast<std::size_t>(w), [&](int b, int e) {
        std::vector<float> src(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1);
        std::vector<int> v(static_cast<std::size_t>(n));
        for (int y = b; y < e; ++y) pass(d.data() + static_cast<std::size_t>(y) * w, w, 1, src, out, v, z);
    });
    for (auto& x : d) x = x >= kInf * 0.5f ? kInf : std::sqrt(x);
    return d;
}

struct Field {
    int w = 0, h = 0, margin = 0;
    std::vector<float> v;
    // value at area pixel (col, row) shifted by (dx, dy); 0 outside
    float at(int col, int row, int dx = 0, int dy = 0) const {
        const int x = col + margin - dx, y = row + margin - dy;
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        return v[static_cast<std::size_t>(y) * w + x];
    }
};

Field blurred(const Field& a, double size, double spread) {
    Field f = a;
    blurField(f.v, f.w, f.h, blurRadius(size));
    const float s = static_cast<float>(std::clamp(spread, 0.0, 0.99));
    if (s > 0) {
        for (auto& x : f.v) x = std::min(1.0f, x / (1 - s));
    }
    return f;
}

// Blends one colour, shaped by alpha(col, row) in 0..1, onto out.
template <typename AlphaFn>
void paintColor(Image& out, Color c, double opacity, Blend mode, double layerOpacity, const std::uint8_t* clip, AlphaFn alpha) {
    const int w = out.width(), h = out.height();
    Image img(w, h);
    const float k = static_cast<float>(opacity * c.a / 255.0);
    bool any = false;
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            float a = alpha(col, row) * k;
            if (clip) a *= clip[static_cast<std::size_t>(row) * w + col] / 255.0f;
            if (a <= 0) continue;
            any = true;
            std::uint8_t* p = img.pixel(col, row);
            p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255 + 0.5f);
        }
    }
    if (any) blendOnto(out, img, 0, 0, mode, layerOpacity);
}

// Mixes a colour effect into a pixel's own colour with weight t.
void tint(std::uint8_t* p, const float col[3], Blend mode, float t) {
    if (t <= 0) return;
    t = std::min(1.0f, t);
    const float b[3] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
    float m[3];
    blendColor(mode, b, col, m);
    for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(std::clamp(b[c] + (m[c] - b[c]) * t, 0.0f, 1.0f) * 255 + 0.5f);
}

void unit(Color c, float out[3]) {
    out[0] = c.r / 255.0f;
    out[1] = c.g / 255.0f;
    out[2] = c.b / 255.0f;
}

// The settings of one effect, pointing into a LayerStyle.
struct StyleField {
    const char* key;
    enum Type { Number, Colour, Mode, Flag, Where } type;
    void* at;
};

bool* effectOn(LayerStyle& s, const std::string& e) {
    if (e == "drop_shadow") return &s.dropShadow.on;
    if (e == "inner_shadow") return &s.innerShadow.on;
    if (e == "outer_glow") return &s.outerGlow.on;
    if (e == "inner_glow") return &s.innerGlow.on;
    if (e == "bevel") return &s.bevel.on;
    if (e == "color_overlay") return &s.colorOverlay.on;
    if (e == "gradient_overlay") return &s.gradientOverlay.on;
    if (e == "stroke") return &s.stroke.on;
    return nullptr;
}

std::vector<StyleField> fieldsOf(LayerStyle& s, const std::string& e) {
    using F = StyleField;
    auto shadow = [](ShadowStyle& x) {
        return std::vector<F>{{"color", F::Colour, &x.color},   {"opacity", F::Number, &x.opacity}, {"angle", F::Number, &x.angle},
                              {"distance", F::Number, &x.distance}, {"size", F::Number, &x.size},   {"spread", F::Number, &x.spread},
                              {"blend", F::Mode, &x.blend}};
    };
    auto glow = [](GlowStyle& x) {
        return std::vector<F>{{"color", F::Colour, &x.color}, {"opacity", F::Number, &x.opacity}, {"size", F::Number, &x.size},
                              {"spread", F::Number, &x.spread}, {"blend", F::Mode, &x.blend}};
    };
    auto overlay = [](OverlayStyle& x, bool gradient) {
        std::vector<F> v{{"color", F::Colour, &x.color}, {"opacity", F::Number, &x.opacity}, {"blend", F::Mode, &x.blend}};
        if (gradient) {
            v.push_back({"to", F::Colour, &x.to});
            v.push_back({"angle", F::Number, &x.angle});
            v.push_back({"radial", F::Flag, &x.radial});
        }
        return v;
    };
    if (e == "drop_shadow") return shadow(s.dropShadow);
    if (e == "inner_shadow") return shadow(s.innerShadow);
    if (e == "outer_glow") return glow(s.outerGlow);
    if (e == "inner_glow") return glow(s.innerGlow);
    if (e == "color_overlay") return overlay(s.colorOverlay, false);
    if (e == "gradient_overlay") return overlay(s.gradientOverlay, true);
    if (e == "bevel") {
        BevelStyle& b = s.bevel;
        return {{"size", F::Number, &b.size},         {"depth", F::Number, &b.depth},         {"angle", F::Number, &b.angle},
                {"altitude", F::Number, &b.altitude}, {"highlight", F::Number, &b.highlight}, {"shadow", F::Number, &b.shadow},
                {"down", F::Flag, &b.down}};
    }
    if (e == "stroke") {
        return {{"color", F::Colour, &s.stroke.color}, {"size", F::Number, &s.stroke.size}, {"opacity", F::Number, &s.stroke.opacity},
                {"position", F::Where, &s.stroke.position}};
    }
    return {};
}

std::string numberText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

}  // namespace

const std::vector<std::string>& styleEffectKeys(const std::string& effect) {
    static const std::map<std::string, std::vector<std::string>> keys = [] {
        std::map<std::string, std::vector<std::string>> m;
        LayerStyle s;
        for (const auto& e : styleEffectNames()) {
            for (const auto& f : fieldsOf(s, e)) m[e].push_back(f.key);
        }
        return m;
    }();
    static const std::vector<std::string> none;
    auto it = keys.find(effect);
    return it == keys.end() ? none : it->second;
}

bool setStyleEffect(LayerStyle& style, const std::string& effect, bool on) {
    bool* flag = effectOn(style, effect);
    if (!flag) return false;
    *flag = on;
    return true;
}

bool setStyleValue(LayerStyle& style, const std::string& effect, const std::string& key, const std::string& value, std::string& error) {
    if (!effectOn(style, effect)) {
        error = "'" + effect + "' isn't a layer effect";
        return false;
    }
    for (const auto& f : fieldsOf(style, effect)) {
        if (key != f.key) continue;
        switch (f.type) {
            case StyleField::Number: {
                char* end = nullptr;
                const double v = std::strtod(value.c_str(), &end);
                if (end == value.c_str()) {
                    error = effect + " " + key + ": '" + value + "' isn't a number";
                    return false;
                }
                *static_cast<double*>(f.at) = v;
                return true;
            }
            case StyleField::Colour: {
                auto c = Color::parse(value);
                if (!c) {
                    error = effect + " " + key + ": '" + value + "' isn't a colour";
                    return false;
                }
                *static_cast<Color*>(f.at) = *c;
                return true;
            }
            case StyleField::Mode: {
                auto b = parseBlend(value);
                if (!b) {
                    error = effect + " blend: '" + value + "' isn't a blend mode";
                    return false;
                }
                *static_cast<Blend*>(f.at) = *b;
                return true;
            }
            case StyleField::Flag: *static_cast<bool*>(f.at) = value == "true" || value == "1" || value == "yes"; return true;
            case StyleField::Where: {
                auto* p = static_cast<StrokeStyle::Position*>(f.at);
                if (value == "outside") *p = StrokeStyle::Position::Outside;
                else if (value == "inside") *p = StrokeStyle::Position::Inside;
                else if (value == "center" || value == "centre") *p = StrokeStyle::Position::Center;
                else {
                    error = "stroke position must be outside, inside or center";
                    return false;
                }
                return true;
            }
        }
    }
    error = effect + " has no setting '" + key + "'";
    return false;
}

std::string styleToText(const LayerStyle& style) {
    LayerStyle s = style;
    std::string out;
    for (const auto& e : styleEffectNames()) {
        if (!*effectOn(s, e)) continue;
        out += (out.empty() ? "" : ";") + e + ":";
        bool first = true;
        for (const auto& f : fieldsOf(s, e)) {
            std::string v;
            switch (f.type) {
                case StyleField::Number: v = numberText(*static_cast<double*>(f.at)); break;
                case StyleField::Colour: v = static_cast<Color*>(f.at)->hex(); break;
                case StyleField::Mode: v = toString(*static_cast<Blend*>(f.at)); break;
                case StyleField::Flag: v = *static_cast<bool*>(f.at) ? "true" : "false"; break;
                case StyleField::Where: {
                    const auto p = *static_cast<StrokeStyle::Position*>(f.at);
                    v = p == StrokeStyle::Position::Outside ? "outside" : p == StrokeStyle::Position::Inside ? "inside" : "center";
                    break;
                }
            }
            out += (first ? "" : ",") + std::string(f.key) + "=" + v;
            first = false;
        }
    }
    return out;
}

bool styleFromText(const std::string& text, LayerStyle& style, std::string& error) {
    style = {};
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        const std::string part = text.substr(at, end - at);
        at = end + 1;
        const std::size_t colon = part.find(':');
        const std::string effect = part.substr(0, colon);
        if (effect.empty()) continue;
        if (!setStyleEffect(style, effect, true)) {
            error = "'" + effect + "' isn't a layer effect";
            return false;
        }
        if (colon == std::string::npos) continue;
        std::size_t k = colon + 1;
        while (k < part.size()) {
            std::size_t comma = part.find(',', k);
            if (comma == std::string::npos) comma = part.size();
            const std::string kv = part.substr(k, comma - k);
            k = comma + 1;
            const std::size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            if (!setStyleValue(style, effect, kv.substr(0, eq), kv.substr(eq + 1), error)) return false;
        }
    }
    return true;
}

int styleReach(const LayerStyle& s) {
    int m = 0;
    auto shadow = [](const ShadowStyle& sh) { return static_cast<int>(std::ceil(std::fabs(sh.distance))) + blurReach(sh.size); };
    if (s.dropShadow.on) m = std::max(m, shadow(s.dropShadow));
    if (s.innerShadow.on) m = std::max(m, shadow(s.innerShadow));
    if (s.outerGlow.on) m = std::max(m, blurReach(s.outerGlow.size));
    if (s.innerGlow.on) m = std::max(m, blurReach(s.innerGlow.size));
    if (s.bevel.on) m = std::max(m, blurReach(s.bevel.size) + 1);
    if (s.stroke.on) m = std::max(m, static_cast<int>(std::ceil(s.stroke.size)) + 2);
    return m;
}

namespace detail {

void drawStyledLayer(Image& out, const Layer& l, int x, int y, const std::uint8_t* clip) {
    const LayerStyle& st = l.style;
    const int w = out.width(), h = out.height();
    const int M = styleReach(st);
    Field A{w + 2 * M, h + 2 * M, M, {}};
    {
        const std::vector<std::uint8_t> a = layerAlpha(l, x - M, y - M, A.w, A.h);
        A.v.resize(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) A.v[i] = a[i] / 255.0f;
    }
    const double op = std::clamp(l.opacity, 0.0, 1.0);
    auto offset = [](const ShadowStyle& s, int& dx, int& dy) {
        const double a = s.angle * kPi / 180;
        dx = static_cast<int>(std::lround(-std::cos(a) * s.distance));
        dy = static_cast<int>(std::lround(std::sin(a) * s.distance));
    };

    // under the layer
    if (st.dropShadow.on) {
        const Field s = blurred(A, st.dropShadow.size, st.dropShadow.spread);
        int dx, dy;
        offset(st.dropShadow, dx, dy);
        paintColor(out, st.dropShadow.color, st.dropShadow.opacity, st.dropShadow.blend, op, clip,
                   [&](int c, int r) { return s.at(c, r, dx, dy); });
    }
    if (st.outerGlow.on) {
        const Field g = blurred(A, st.outerGlow.size, st.outerGlow.spread);
        paintColor(out, st.outerGlow.color, st.outerGlow.opacity, st.outerGlow.blend, op, clip,
                   [&](int c, int r) { return g.at(c, r) * (1 - A.at(c, r)); });
    }

    // the layer's own pixels, with the inner effects mixed into their colour
    Image content(w, h);
    const float fill = static_cast<float>(std::clamp(l.fill, 0.0, 1.0));
    for (int row = 0; row < h; ++row) {
        const int sy = y + row;
        if (sy < 0 || sy >= l.image.height()) continue;
        for (int col = 0; col < w; ++col) {
            const int sx = x + col;
            if (sx < 0 || sx >= l.image.width()) continue;
            std::uint8_t* d = content.pixel(col, row);
            const std::uint8_t* s = l.image.pixel(sx, sy);
            float a = A.at(col, row) * fill;
            if (clip) a *= clip[static_cast<std::size_t>(row) * w + col] / 255.0f;
            d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = static_cast<std::uint8_t>(a * 255 + 0.5f);
        }
    }
    if (st.colorOverlay.on) {
        float c[3];
        unit(st.colorOverlay.color, c);
        const float t = static_cast<float>(st.colorOverlay.opacity * st.colorOverlay.color.a / 255.0);
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) tint(content.pixel(col, row), c, st.colorOverlay.blend, t);
        }
    }
    if (st.gradientOverlay.on) {
        const OverlayStyle& g = st.gradientOverlay;
        const double cw = std::max(1, l.image.width()), ch = std::max(1, l.image.height());
        const double a = g.angle * kPi / 180, ux = std::cos(a), uy = -std::sin(a);
        const double ext = (std::fabs(ux) * cw + std::fabs(uy) * ch) / 2, rad = std::hypot(cw, ch) / 2;
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) {
                const double px = x + col + 0.5 - cw / 2, py = y + row + 0.5 - ch / 2;
                const double t = std::clamp(g.radial ? std::hypot(px, py) / rad : 0.5 + (px * ux + py * uy) / (2 * ext), 0.0, 1.0);
                const float c[3] = {static_cast<float>((g.color.r + (g.to.r - g.color.r) * t) / 255),
                                    static_cast<float>((g.color.g + (g.to.g - g.color.g) * t) / 255),
                                    static_cast<float>((g.color.b + (g.to.b - g.color.b) * t) / 255)};
                tint(content.pixel(col, row), c, g.blend, static_cast<float>(g.opacity));
            }
        }
    }
    if (st.innerGlow.on) {
        const Field g = blurred(A, st.innerGlow.size, st.innerGlow.spread);
        float c[3];
        unit(st.innerGlow.color, c);
        const float k = static_cast<float>(st.innerGlow.opacity * st.innerGlow.color.a / 255.0);
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) tint(content.pixel(col, row), c, st.innerGlow.blend, (1 - g.at(col, row)) * A.at(col, row) * k);
        }
    }
    if (st.innerShadow.on) {
        const Field s = blurred(A, st.innerShadow.size, st.innerShadow.spread);
        int dx, dy;
        offset(st.innerShadow, dx, dy);
        float c[3];
        unit(st.innerShadow.color, c);
        const float k = static_cast<float>(st.innerShadow.opacity * st.innerShadow.color.a / 255.0);
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) {
                tint(content.pixel(col, row), c, st.innerShadow.blend, (1 - s.at(col, row, dx, dy)) * A.at(col, row) * k);
            }
        }
    }
    if (st.bevel.on) {
        const BevelStyle& b = st.bevel;
        const Field hgt = blurred(A, b.size, 0);
        const double az = b.angle * kPi / 180, alt = std::clamp(b.altitude, 0.0, 90.0) * kPi / 180;
        const double lx = std::cos(alt) * std::cos(az), ly = -std::cos(alt) * std::sin(az), lz = std::sin(alt);
        const double k = b.depth * std::max(1.0, b.size) * 2 * (b.down ? -1 : 1);
        const float white[3] = {1, 1, 1}, black[3] = {0, 0, 0};
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) {
                const float inside = A.at(col, row);
                if (inside <= 0) continue;
                const double gx = (hgt.at(col + 1, row) - hgt.at(col - 1, row)) / 2 * k;
                const double gy = (hgt.at(col, row + 1) - hgt.at(col, row - 1)) / 2 * k;
                const double len = std::sqrt(gx * gx + gy * gy + 1);
                const double shade = (-gx * lx - gy * ly + lz) / len - lz;
                std::uint8_t* p = content.pixel(col, row);
                if (shade > 0) tint(p, white, Blend::Screen, static_cast<float>(shade * b.highlight * 2) * inside);
                else tint(p, black, Blend::Multiply, static_cast<float>(-shade * b.shadow * 2) * inside);
            }
        }
    }
    blendOnto(out, content, 0, 0, l.blend, op);

    // on top
    if (st.stroke.on) {
        const StrokeStyle& s = st.stroke;
        std::vector<std::uint8_t> in(A.v.size()), outside(A.v.size());
        for (std::size_t i = 0; i < A.v.size(); ++i) {
            in[i] = A.v[i] >= 0.5f;
            outside[i] = !in[i];
        }
        const double outer = s.position == StrokeStyle::Position::Outside ? s.size : s.position == StrokeStyle::Position::Center ? s.size / 2 : 0;
        const double inner = s.position == StrokeStyle::Position::Inside ? s.size : s.position == StrokeStyle::Position::Center ? s.size / 2 : 0;
        Field dOut{A.w, A.h, M, outer > 0 ? distanceTo(in, A.w, A.h) : std::vector<float>{}};
        Field dIn{A.w, A.h, M, inner > 0 ? distanceTo(outside, A.w, A.h) : std::vector<float>{}};
        paintColor(out, s.color, s.opacity, Blend::Normal, op, clip, [&](int c, int r) {
            const float a = A.at(c, r);
            float ring = 0;
            if (outer > 0) ring = std::max(ring, std::clamp(static_cast<float>(outer + 0.5 - dOut.at(c, r)), 0.0f, 1.0f) * (1 - a));
            if (inner > 0) ring = std::max(ring, std::clamp(static_cast<float>(inner + 0.5 - dIn.at(c, r)), 0.0f, 1.0f) * a);
            return ring;
        });
    }
}

}  // namespace detail

}  // namespace wetwaresuite::raster
