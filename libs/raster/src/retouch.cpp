#include "wetwaresuite/raster/retouch.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdlib>

#include "wetwaresuite/raster/ops.hpp"

namespace wetwaresuite::raster {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline float lum(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }
inline std::uint8_t to8(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

// Runs fn(x, y, pixel, k) for every covered pixel, k = coverage * brush opacity.
template <typename Fn>
void eachCovered(Image& target, const Brush& brush, const std::vector<StrokePoint>& points, Fn fn) {
    const Coverage c = strokeCoverage(target.width(), target.height(), brush, points);
    const float op = static_cast<float>(std::clamp(brush.opacity, 0.0, 1.0));
    for (int y = c.rect.y0; y < c.rect.y1; ++y) {
        for (int x = c.rect.x0; x < c.rect.x1; ++x) {
            const float k = c.at(x, y) * op;
            if (k > 0) fn(x, y, target.pixel(x, y), k);
        }
    }
}

// Straight-alpha mix of two pixels done in premultiplied space.
inline void mixInto(std::uint8_t* d, const std::uint8_t* s, float k) {
    const float da = d[3] / 255.0f, sa = s[3] / 255.0f;
    const float a = da + (sa - da) * k;
    if (a <= 0) {
        d[0] = d[1] = d[2] = d[3] = 0;
        return;
    }
    for (int c = 0; c < 3; ++c) d[c] = to8((d[c] * da + (s[c] * sa - d[c] * da) * k) / a);
    d[3] = to8(a * 255);
}

}  // namespace

void cloneStroke(Image& target, const Image& source, int dx, int dy, const Brush& brush, const std::vector<StrokePoint>& points) {
    eachCovered(target, brush, points, [&](int x, int y, std::uint8_t* d, float k) {
        const int sx = x + dx, sy = y + dy;
        if (!source.contains(sx, sy)) return;
        mixInto(d, source.pixel(sx, sy), k);
    });
}

void toneStroke(Image& target, ToneTool tool, ToneRange range, double exposure, bool saturate, const Brush& brush,
                const std::vector<StrokePoint>& points) {
    const float e = static_cast<float>(std::clamp(exposure, 0.0, 1.0));
    eachCovered(target, brush, points, [&](int, int, std::uint8_t* d, float k) {
        float v[3] = {d[0] / 255.0f, d[1] / 255.0f, d[2] / 255.0f};
        const float L = lum(v[0], v[1], v[2]);
        const float s = e * k;
        if (tool == ToneTool::Sponge) {
            for (float& c : v) c += (saturate ? c - L : L - c) * s;
        } else {
            float w = range == ToneRange::Shadows ? (1 - L) * (1 - L) : range == ToneRange::Highlights ? L * L : 1 - (2 * L - 1) * (2 * L - 1);
            w = 0.15f + 0.85f * w;  // every tone moves a little, the chosen range the most
            for (float& c : v) c = tool == ToneTool::Dodge ? c + (1 - c) * s * w : c - c * s * w;
        }
        for (int c = 0; c < 3; ++c) d[c] = to8(v[c] * 255);
    });
}

void smudgeStroke(Image& target, double strength, bool fingerPaint, const Brush& brush, const std::vector<StrokePoint>& points) {
    const std::vector<Dab> dabs = strokeDabs(brush, points, target.width(), target.height());
    if (dabs.empty() || target.empty()) return;
    double maxR = 0;
    for (const auto& d : dabs) maxR = std::max(maxR, d.radius);
    const int R = static_cast<int>(std::ceil(maxR)) + 1, side = 2 * R + 1;
    const float keep = static_cast<float>(std::clamp(strength, 0.0, 1.0));
    const float op = static_cast<float>(std::clamp(brush.opacity, 0.0, 1.0));
    // the colours the brush carries, premultiplied, by offset from the dab centre
    std::vector<float> carry(static_cast<std::size_t>(side) * side * 4, 0.0f);
    auto load = [&](int cx, int cy) {
        for (int oy = -R; oy <= R; ++oy) {
            for (int ox = -R; ox <= R; ++ox) {
                float* c = &carry[(static_cast<std::size_t>(oy + R) * side + (ox + R)) * 4];
                const int x = std::clamp(cx + ox, 0, target.width() - 1), y = std::clamp(cy + oy, 0, target.height() - 1);
                const auto* p = target.pixel(x, y);
                const float a = p[3] / 255.0f;
                if (fingerPaint) {
                    const float ba = brush.color.a / 255.0f;
                    c[0] = brush.color.r * ba, c[1] = brush.color.g * ba, c[2] = brush.color.b * ba, c[3] = ba;
                } else {
                    c[0] = p[0] * a, c[1] = p[1] * a, c[2] = p[2] * a, c[3] = a;
                }
            }
        }
    };
    Brush one = brush;
    one.texture.reset();
    load(static_cast<int>(std::floor(dabs[0].x)), static_cast<int>(std::floor(dabs[0].y)));
    for (std::size_t i = 0; i < dabs.size(); ++i) {
        const Dab& d = dabs[i];
        const int cx = static_cast<int>(std::floor(d.x)), cy = static_cast<int>(std::floor(d.y));
        if (brush.symmetry.on() && i > 0) {  // a new mirrored copy starts where it lands, not where the last one ended
            const Dab& prev = dabs[i - 1];
            if (std::hypot(d.x - prev.x, d.y - prev.y) > 4 * maxR + 2) load(cx, cy);
        }
        Coverage cov;
        cov.rect = {std::max(0, cx - R), std::max(0, cy - R), std::min(target.width(), cx + R + 1), std::min(target.height(), cy + R + 1)};
        if (cov.rect.empty()) continue;
        cov.v.assign(static_cast<std::size_t>(cov.rect.width()) * cov.rect.height(), 0.0f);
        addDab(cov, one, d);
        for (int y = cov.rect.y0; y < cov.rect.y1; ++y) {
            for (int x = cov.rect.x0; x < cov.rect.x1; ++x) {
                float* c = &carry[(static_cast<std::size_t>(y - cy + R) * side + (x - cx + R)) * 4];
                auto* p = target.pixel(x, y);
                const float k = cov.at(x, y) * op;
                const float a = p[3] / 255.0f;
                float px[4] = {p[0] * a, p[1] * a, p[2] * a, a};
                if (k > 0) {
                    for (int ch = 0; ch < 4; ++ch) px[ch] += (c[ch] - px[ch]) * k;
                    if (px[3] <= 1e-6f) {
                        p[0] = p[1] = p[2] = p[3] = 0;
                    } else {
                        for (int ch = 0; ch < 3; ++ch) p[ch] = to8(px[ch] / px[3]);
                        p[3] = to8(px[3] * 255);
                    }
                }
                // the brush picks up some of what it now sits on
                for (int ch = 0; ch < 4; ++ch) c[ch] = c[ch] * keep + px[ch] * (1 - keep);
            }
        }
    }
}

void blurStroke(Image& target, bool sharpen, double strength, const Brush& brush, const std::vector<StrokePoint>& points) {
    const PixelRect r = strokeBounds(target, brush, points);
    if (r.empty()) return;
    const int radius = std::clamp(static_cast<int>(std::lround(brush.size / 10)), 1, 12);
    const int x0 = std::max(0, r.x0 - 3 * radius), y0 = std::max(0, r.y0 - 3 * radius);
    const int x1 = std::min(target.width(), r.x1 + 3 * radius), y1 = std::min(target.height(), r.y1 + 3 * radius);
    Image soft = crop(target, x0, y0, x1 - x0, y1 - y0);
    boxBlur(soft, radius);
    const float s = static_cast<float>(std::clamp(strength, 0.0, 1.0));
    eachCovered(target, brush, points, [&](int x, int y, std::uint8_t* d, float k) {
        const auto* b = soft.pixel(x - x0, y - y0);
        if (sharpen) {
            for (int c = 0; c < 3; ++c) d[c] = to8(d[c] + (d[c] - b[c]) * 1.5f * s * k);
        } else {
            mixInto(d, b, s * k);
        }
    });
}

void replaceColorStroke(Image& target, int tolerance, const Brush& brush, const std::vector<StrokePoint>& points) {
    if (points.empty() || target.empty()) return;
    const int px = std::clamp(static_cast<int>(points[0].x), 0, target.width() - 1);
    const int py = std::clamp(static_cast<int>(points[0].y), 0, target.height() - 1);
    const Color seed = target.at(px, py);
    const float tol = static_cast<float>(std::clamp(tolerance, 0, 255));
    const float src[3] = {brush.color.r / 255.0f, brush.color.g / 255.0f, brush.color.b / 255.0f};
    eachCovered(target, brush, points, [&](int, int, std::uint8_t* d, float k) {
        const float diff = static_cast<float>(std::max({std::abs(d[0] - seed.r), std::abs(d[1] - seed.g), std::abs(d[2] - seed.b)}));
        const float w = diff <= tol ? 1.0f : diff <= tol + 16 ? 1 - (diff - tol) / 16 : 0.0f;  // soft edge past the tolerance
        if (w <= 0) return;
        const float b[3] = {d[0] / 255.0f, d[1] / 255.0f, d[2] / 255.0f};
        float out[3];
        blendColor(Blend::Color, b, src, out);
        for (int c = 0; c < 3; ++c) d[c] = to8((b[c] + (out[c] - b[c]) * k * w) * 255);
    });
}

const std::vector<std::string>& patternNames() {
    static const std::vector<std::string> n = {"checker", "stripes", "diagonal", "dots", "grid", "bricks", "noise", "hexagons"};
    return n;
}

Image builtinPattern(const std::string& name, int size, Color a, Color b) {
    const int s = std::clamp(size, 2, 1024);
    const int w = name == "bricks" ? 2 * s : s, h = name == "hexagons" ? static_cast<int>(std::lround(s * std::sqrt(3.0))) : s;
    Image img(w, h, b);
    std::uint32_t state = 12345;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double u = (x + 0.5) / s, v = (y + 0.5) / s;
            bool on = false;
            if (name == "checker") on = (u < 0.5) != (v < 0.5);
            else if (name == "stripes") on = u < 0.5;
            else if (name == "diagonal") on = std::fmod(u + v, 1.0) < 0.5;
            else if (name == "dots") on = std::hypot(u - 0.5, v - 0.5) < 0.3;
            else if (name == "grid") on = x < std::max(1, s / 8) || y < std::max(1, s / 8);
            else if (name == "bricks") {
                const int row = y < s / 2 ? 0 : 1;
                const int bx = (x + row * s / 2) % s;
                on = !(y % (s / 2) < std::max(1, s / 16) || bx < std::max(1, s / 16));
            } else if (name == "hexagons") {
                // distance to the nearest of the hexagon lattice centres
                const double hx = x + 0.5, hy = y + 0.5, r3 = std::sqrt(3.0) * s;
                double best = 1e18;
                const double cs[5][2] = {{0, 0}, {double(s), 0}, {s / 2.0, r3 / 2}, {0, r3}, {double(s), r3}};
                for (const auto& c : cs) best = std::min(best, std::hypot(hx - c[0], hy - c[1]));
                on = best < s * 0.42;
            } else {  // noise
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                const double t = (state & 0xffff) / 65535.0;
                const auto mixc = [&](std::uint8_t p, std::uint8_t q) { return static_cast<std::uint8_t>(std::lround(p + (q - p) * t)); };
                img.set(x, y, {mixc(b.r, a.r), mixc(b.g, a.g), mixc(b.b, a.b), mixc(b.a, a.a)});
                continue;
            }
            if (on) img.set(x, y, a);
        }
    }
    return img;
}

Image tilePattern(const Image& tile, int width, int height, int offsetX, int offsetY, double scale) {
    Image out(width, height);
    if (tile.empty()) return out;
    const Image t = std::fabs(scale - 1) > 1e-6 ? resize(tile, std::max(1, static_cast<int>(std::lround(tile.width() * scale))),
                                                         std::max(1, static_cast<int>(std::lround(tile.height() * scale))))
                                                 : tile;
    const int tw = t.width(), th = t.height();
    for (int y = 0; y < height; ++y) {
        const int ty = ((y - offsetY) % th + th) % th;
        for (int x = 0; x < width; ++x) {
            const int tx = ((x - offsetX) % tw + tw) % tw;
            std::copy_n(t.pixel(tx, ty), 4, out.pixel(x, y));
        }
    }
    return out;
}

const std::vector<std::string>& gradientShapeNames() {
    static const std::vector<std::string> n = {"linear", "radial", "angle", "reflected", "diamond"};
    return n;
}

std::optional<GradientShape> parseGradientShape(const std::string& name) {
    const auto& n = gradientShapeNames();
    for (std::size_t i = 0; i < n.size(); ++i) {
        if (n[i] == name) return static_cast<GradientShape>(i);
    }
    return std::nullopt;
}

const std::vector<std::string>& gradientPresetNames() {
    static const std::vector<std::string> n = {"black_white", "rainbow", "sunset", "ocean", "fire", "chrome",
                                               "copper", "pastel", "violet_orange", "transparent_stripes"};
    return n;
}

std::vector<GradientStop> gradientPreset(const std::string& name) {
    auto c = [](const char* hex) { return *Color::parse(hex); };
    if (name == "black_white") return {{0, c("#000000")}, {1, c("#ffffff")}};
    if (name == "rainbow") {
        return {{0, c("#ff0000")}, {1 / 6.0, c("#ffff00")}, {2 / 6.0, c("#00ff00")}, {3 / 6.0, c("#00ffff")},
                {4 / 6.0, c("#0000ff")}, {5 / 6.0, c("#ff00ff")}, {1, c("#ff0000")}};
    }
    if (name == "sunset") return {{0, c("#1a0b3d")}, {0.45, c("#b0306a")}, {0.75, c("#ff7a3d")}, {1, c("#ffd36b")}};
    if (name == "ocean") return {{0, c("#021b3a")}, {0.5, c("#0b6fa4")}, {1, c("#7fe3e8")}};
    if (name == "fire") return {{0, c("#000000")}, {0.35, c("#a30000")}, {0.7, c("#ff8a00")}, {1, c("#fff4b0")}};
    if (name == "chrome") {
        return {{0, c("#2a2f3a")}, {0.3, c("#e8eef5")}, {0.5, c("#5a6270")}, {0.52, c("#3d2b1a")}, {0.7, c("#c9a27a")}, {1, c("#ffffff")}};
    }
    if (name == "copper") return {{0, c("#4a2410")}, {0.4, c("#c8763c")}, {0.6, c("#f2c39b")}, {1, c("#6b3418")}};
    if (name == "pastel") return {{0, c("#ffd1dc")}, {0.33, c("#fff5ba")}, {0.66, c("#c1f0dc")}, {1, c("#c7d2fe")}};
    if (name == "violet_orange") return {{0, c("#2e0854")}, {0.5, c("#ff3d7f")}, {1, c("#ffa53d")}};
    if (name == "transparent_stripes") {
        std::vector<GradientStop> s;
        for (int i = 0; i <= 10; ++i) {
            s.push_back({i / 10.0 - (i ? 0.0001 : 0), i % 2 ? c("#00000000") : c("#000000")});
            if (i < 10) s.push_back({i / 10.0 + 0.0999, i % 2 ? c("#00000000") : c("#000000")});
        }
        return s;
    }
    return {};
}

bool parseGradientStops(const std::string& text, std::vector<GradientStop>& out, std::string& error) {
    out.clear();
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : text + ",") {
        if (ch == ',' || ch == ';') {
            std::size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
            if (a != std::string::npos) parts.push_back(cur.substr(a, b - a + 1));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (parts.size() < 2) {
        error = "a gradient needs at least two colours, e.g. #000000,#ff0000,#ffffff (or with positions #000@0,#f00@40,#fff@100)";
        return false;
    }
    bool anyPos = false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::string col = parts[i];
        double pos = static_cast<double>(i) / (parts.size() - 1);
        const auto at = col.find('@');
        if (at != std::string::npos) {
            pos = std::atof(col.c_str() + at + 1) / 100;
            col = col.substr(0, at);
            anyPos = true;
        }
        auto c = Color::parse(col);
        if (!c) {
            error = "'" + col + "' isn't a colour";
            return false;
        }
        out.push_back({std::clamp(pos, 0.0, 1.0), *c});
    }
    if (anyPos) std::stable_sort(out.begin(), out.end(), [](const GradientStop& a, const GradientStop& b) { return a.pos < b.pos; });
    return true;
}

void drawGradient(Image& img, std::vector<GradientStop> stops, GradientShape shape, double x1, double y1, double x2, double y2,
                  double opacity, Blend blend, bool reverse) {
    if (img.empty() || stops.empty()) return;
    if (reverse) {
        for (auto& s : stops) s.pos = 1 - s.pos;
        std::reverse(stops.begin(), stops.end());
    }
    // a 1024-step table, mixed in premultiplied space so fades to transparent stay clean
    constexpr int kSteps = 1024;
    std::vector<std::array<std::uint8_t, 4>> lut(kSteps);
    for (int i = 0; i < kSteps; ++i) {
        const double t = i / double(kSteps - 1);
        std::size_t j = 0;
        while (j + 1 < stops.size() && stops[j + 1].pos < t) ++j;
        const GradientStop& a = stops[j];
        const GradientStop& b = j + 1 < stops.size() ? stops[j + 1] : stops[j];
        const double span = b.pos - a.pos;
        const double f = t <= a.pos ? 0 : t >= b.pos ? 1 : span > 0 ? (t - a.pos) / span : 1;
        const double aa = a.color.a / 255.0, ba = b.color.a / 255.0, outA = aa + (ba - aa) * f;
        auto ch = [&](std::uint8_t p, std::uint8_t q) {
            return outA <= 0 ? std::uint8_t{0} : to8(static_cast<float>((p * aa + (q * ba - p * aa) * f) / outA));
        };
        lut[static_cast<std::size_t>(i)] = {ch(a.color.r, b.color.r), ch(a.color.g, b.color.g), ch(a.color.b, b.color.b), to8(static_cast<float>(outA * 255))};
    }
    const double dx = x2 - x1, dy = y2 - y1;
    const double len2 = dx * dx + dy * dy, len = std::sqrt(len2);
    const double base = std::atan2(dy, dx);
    Image layer(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const double px = x + 0.5 - x1, py = y + 0.5 - y1;
            double t;
            switch (shape) {
                case GradientShape::Radial: t = len > 0 ? std::hypot(px, py) / len : 1; break;
                case GradientShape::Angle: {
                    double a = std::atan2(py, px) - base;
                    a = std::fmod(a + 4 * kPi, 2 * kPi);
                    t = a / (2 * kPi);
                    break;
                }
                case GradientShape::Reflected: t = len2 > 0 ? std::fabs((px * dx + py * dy) / len2) : 1; break;
                case GradientShape::Diamond: {
                    // distance along plus across the drag direction: a diamond
                    const double u = len > 0 ? std::fabs((px * dx + py * dy) / len2) : 1;
                    const double v = len > 0 ? std::fabs((-px * dy + py * dx) / len2) : 1;
                    t = u + v;
                    break;
                }
                default: t = len2 > 0 ? (px * dx + py * dy) / len2 : 1; break;
            }
            t = std::clamp(t, 0.0, 1.0);
            const auto& c = lut[static_cast<std::size_t>(std::lround(t * (kSteps - 1)))];
            std::copy(c.begin(), c.end(), layer.pixel(x, y));
        }
    }
    blendOnto(img, layer, 0, 0, blend, std::clamp(opacity, 0.0, 1.0));
}

}  // namespace wetwaresuite::raster
