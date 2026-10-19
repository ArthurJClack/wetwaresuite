#include "wetwaresuite/raster/paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace wetwaresuite::raster {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Pixel-space bounding box clipped to the image.
struct Box {
    int x0, y0, x1, y1;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// Blends `color` with per-pixel coverage `mask` (bbox-local) onto target.
void applyMask(Image& target, const Box& box, const std::vector<float>& mask, Color color, double opacity, bool erase) {
    const float op = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    const float ca = color.a / 255.0f;
    for (int y = box.y0; y < box.y1; ++y) {
        for (int x = box.x0; x < box.x1; ++x) {
            float m = mask[static_cast<std::size_t>(y - box.y0) * box.width() + (x - box.x0)] * op;
            if (m <= 0) continue;
            auto* d = target.pixel(x, y);
            if (erase) {
                d[3] = static_cast<std::uint8_t>(d[3] * (1 - m) + 0.5f);
                continue;
            }
            float sa = m * ca;
            float da = d[3] / 255.0f;
            float outA = sa + da * (1 - sa);
            if (outA <= 0) continue;
            const std::uint8_t src[3] = {color.r, color.g, color.b};
            for (int c = 0; c < 3; ++c) {
                float v = (src[c] * sa + d[c] * da * (1 - sa)) / outA;
                d[c] = static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
            }
            d[3] = static_cast<std::uint8_t>(outA * 255.0f + 0.5f);
        }
    }
}

// Bilinear read of a tip mask at u, v in 0..1 (outside = 0).
float tipAt(const Mask& m, double u, double v) {
    if (u < 0 || v < 0 || u >= 1 || v >= 1 || m.empty()) return 0;
    const double fx = u * m.width - 0.5, fy = v * m.height - 0.5;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const double tx = fx - x0, ty = fy - y0;
    auto px = [&](int x, int y) {
        x = std::clamp(x, 0, m.width - 1);
        y = std::clamp(y, 0, m.height - 1);
        return m.at(x, y) / 255.0;
    };
    const double top = px(x0, y0) * (1 - tx) + px(x0 + 1, y0) * tx;
    const double bot = px(x0, y0 + 1) * (1 - tx) + px(x0 + 1, y0 + 1) * tx;
    return static_cast<float>(top * (1 - ty) + bot * ty);
}

std::uint32_t nextRandom(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

PixelRect dabsRect(const std::vector<Dab>& dabs, int width, int height) {
    if (dabs.empty()) return {};
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& d : dabs) {
        x0 = std::min(x0, d.x - d.radius - 1);
        y0 = std::min(y0, d.y - d.radius - 1);
        x1 = std::max(x1, d.x + d.radius + 1);
        y1 = std::max(y1, d.y + d.radius + 1);
    }
    auto toInt = [](double v) { return static_cast<int>(std::clamp(v, -1e9, 1e9)); };
    PixelRect r{std::max(0, toInt(std::floor(x0))), std::max(0, toInt(std::floor(y0))), std::min(width, toInt(std::ceil(x1))),
                std::min(height, toInt(std::ceil(y1)))};
    return r.empty() ? PixelRect{} : r;
}

}  // namespace

std::vector<StrokePoint> smooth(const std::vector<StrokePoint>& points, double amount) {
    if (amount <= 0 || points.size() < 3) return points;
    const double k = std::clamp(amount, 0.0, 0.95);
    std::vector<StrokePoint> out;
    out.reserve(points.size() + 1);
    StrokePoint cur = points.front();
    out.push_back(cur);
    for (std::size_t i = 1; i < points.size(); ++i) {
        cur.x = cur.x * k + points[i].x * (1 - k);
        cur.y = cur.y * k + points[i].y * (1 - k);
        cur.pressure = cur.pressure * k + points[i].pressure * (1 - k);
        out.push_back(cur);
    }
    out.push_back(points.back());  // end where the pen lifted
    return out;
}

std::vector<Dab> strokeDabs(const Brush& brush, const std::vector<StrokePoint>& points, int width, int height) {
    std::vector<Dab> dabs;
    if (points.empty()) return dabs;
    auto radiusAt = [&](double p) { return std::max(0.5, (brush.pressureSize ? brush.size * std::clamp(p, 0.05, 1.0) : brush.size) / 2); };
    auto alphaAt = [&](double p) { return brush.flow * (brush.pressureOpacity ? std::clamp(p, 0.0, 1.0) : 1.0); };
    dabs.push_back({points[0].x, points[0].y, radiusAt(points[0].pressure), alphaAt(points[0].pressure)});
    double carry = 0;  // distance travelled since the last dab
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto& a = points[i - 1];
        const auto& b = points[i];
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0) continue;
        double pos = 0;
        while (true) {
            double t = pos / len;
            double p = a.pressure + (b.pressure - a.pressure) * t;
            const double step = std::max(0.5, 2 * radiusAt(p) * std::max(0.02, brush.spacing));
            const double next = pos + (step - carry);
            if (next > len) {
                carry += len - pos;
                break;
            }
            pos = next;
            carry = 0;
            t = pos / len;
            p = a.pressure + (b.pressure - a.pressure) * t;
            dabs.push_back({a.x + dx * t, a.y + dy * t, radiusAt(p), alphaAt(p)});
        }
    }
    if (brush.scatter > 0) {
        std::uint32_t state = brush.seed * 2654435761u + 0x9e3779b9u;
        if (state == 0) state = 1;
        const double reach = std::clamp(brush.scatter, 0.0, 4.0) * brush.size;
        for (auto& d : dabs) {
            const double ang = nextRandom(state) / 4294967296.0 * 2 * kPi;
            const double dist = std::sqrt(nextRandom(state) / 4294967296.0) * reach;
            d.x += std::cos(ang) * dist;
            d.y += std::sin(ang) * dist;
        }
    }
    const Symmetry& sym = brush.symmetry;
    if (!sym.on()) return dabs;
    const double cx = sym.cx < 0 ? width / 2.0 : sym.cx, cy = sym.cy < 0 ? height / 2.0 : sym.cy;
    // every copy as (flip x, flip y, turn): each copy is a whole stroke, painted one after another
    struct Copy {
        bool fx, fy;
        double turn;
    };
    std::vector<Copy> copies{{false, false, 0}};
    if (sym.mirrorX) copies.push_back({true, false, 0});
    if (sym.mirrorY) {
        const std::size_t n = copies.size();
        for (std::size_t i = 0; i < n; ++i) copies.push_back({copies[i].fx, true, 0});
    }
    const int turns = std::clamp(sym.radial, 1, 64);
    std::vector<Copy> all;
    for (int k = 0; k < turns; ++k) {
        for (const auto& c : copies) all.push_back({c.fx, c.fy, 2 * kPi * k / turns});
    }
    std::vector<Dab> out;
    out.reserve(dabs.size() * all.size());
    for (const auto& c : all) {
        const double cs = std::cos(c.turn), sn = std::sin(c.turn);
        for (const auto& d : dabs) {
            double x = d.x - cx, y = d.y - cy;
            if (c.fx) x = -x;
            if (c.fy) y = -y;
            out.push_back({cx + x * cs - y * sn, cy + x * sn + y * cs, d.radius, d.alpha});
        }
    }
    return out;
}

void addDab(Coverage& cover, const Brush& brush, const Dab& d) {
    const PixelRect& box = cover.rect;
    const double radius = std::max(0.5, d.radius);
    const int x0 = std::max(box.x0, static_cast<int>(std::floor(d.x - radius - 1)));
    const int x1 = std::min(box.x1, static_cast<int>(std::ceil(d.x + radius + 1)));
    const int y0 = std::max(box.y0, static_cast<int>(std::floor(d.y - radius - 1)));
    const int y1 = std::min(box.y1, static_cast<int>(std::ceil(d.y + radius + 1)));
    const double h = std::clamp(brush.hardness, 0.0, 0.999);
    const Mask* tip = brush.tip && !brush.tip->empty() ? brush.tip.get() : nullptr;
    // a tip keeps its proportions: its longer side spans the brush diameter
    const double tipLong = tip ? std::max(tip->width, tip->height) : 1;
    const double tipW = tip ? 2 * radius * tip->width / tipLong : 1, tipH = tip ? 2 * radius * tip->height / tipLong : 1;
    for (int y = y0; y < y1; ++y) {
        float* row = cover.v.data() + static_cast<std::size_t>(y - box.y0) * box.width() - box.x0;
        for (int x = x0; x < x1; ++x) {
            const double dx = x + 0.5 - d.x, dy = y + 0.5 - d.y;
            double a;
            if (tip) {
                a = tipAt(*tip, dx / tipW + 0.5, dy / tipH + 0.5);
            } else {
                const double dist = std::sqrt(dx * dx + dy * dy);
                const double edge = std::clamp(radius - dist + 0.5, 0.0, 1.0);  // antialiased rim
                if (edge <= 0) continue;
                const double t = dist / radius;
                double fall = t <= h ? 1.0 : std::max(0.0, 1.0 - (t - h) / (1.0 - h));
                fall = fall * fall * (3 - 2 * fall);  // smoothstep
                a = brush.hardness >= 0.99 ? edge : std::min(edge, fall);
            }
            a *= d.alpha;
            if (a <= 0) continue;
            float& m = row[x];
            m = m + (1 - m) * static_cast<float>(a);
        }
    }
}

Coverage strokeCoverage(int width, int height, const Brush& brush, const std::vector<StrokePoint>& points) {
    Coverage c;
    const std::vector<Dab> dabs = strokeDabs(brush, points, width, height);
    c.rect = dabsRect(dabs, width, height);
    if (c.rect.empty()) return c;
    c.v.assign(static_cast<std::size_t>(c.rect.width()) * c.rect.height(), 0.0f);
    for (const auto& d : dabs) addDab(c, brush, d);
    if (brush.texture && !brush.texture->empty()) {
        const Mask& t = *brush.texture;
        const float depth = static_cast<float>(std::clamp(brush.textureDepth, 0.0, 1.0));
        for (int y = c.rect.y0; y < c.rect.y1; ++y) {
            float* row = c.v.data() + static_cast<std::size_t>(y - c.rect.y0) * c.rect.width();
            const std::uint8_t* trow = t.row(y % t.height);
            for (int x = c.rect.x0; x < c.rect.x1; ++x) row[x - c.rect.x0] *= 1 - depth * (1 - trow[x % t.width] / 255.0f);
        }
    }
    return c;
}

std::shared_ptr<const Mask> maskFromImage(const Image& img, bool invert) {
    auto m = std::make_shared<Mask>(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const auto* p = img.pixel(x, y);
            double lum = (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255;
            if (invert) lum = 1 - lum;
            m->v[static_cast<std::size_t>(y) * img.width() + x] = static_cast<std::uint8_t>(std::lround(lum * p[3]));
        }
    }
    return m;
}

PixelRect strokeBounds(const Image& target, const Brush& brush, const std::vector<StrokePoint>& points) {
    if (target.empty()) return {};
    return dabsRect(strokeDabs(brush, points, target.width(), target.height()), target.width(), target.height());
}

void stroke(Image& target, const Brush& brush, const std::vector<StrokePoint>& points) {
    const Coverage c = strokeCoverage(target.width(), target.height(), brush, points);
    if (c.rect.empty()) return;
    applyMask(target, Box{c.rect.x0, c.rect.y0, c.rect.x1, c.rect.y1}, c.v, brush.color, brush.opacity, brush.erase);
}

std::size_t floodFill(Image& target, int sx, int sy, Color color, int tolerance, bool contiguous) {
    if (!target.contains(sx, sy)) return 0;
    const Color seed = target.at(sx, sy);
    if (seed == color) return 0;
    auto matches = [&](int x, int y) {
        const auto* p = target.pixel(x, y);
        return std::abs(p[0] - seed.r) <= tolerance && std::abs(p[1] - seed.g) <= tolerance &&
               std::abs(p[2] - seed.b) <= tolerance && std::abs(p[3] - seed.a) <= tolerance;
    };
    const int w = target.width(), h = target.height();
    std::vector<std::uint8_t> done(static_cast<std::size_t>(w) * h, 0);
    std::size_t filled = 0;

    if (!contiguous) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (matches(x, y)) {
                    target.set(x, y, color);
                    ++filled;
                }
            }
        }
        return filled;
    }

    std::vector<std::pair<int, int>> stack{{sx, sy}};
    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        if (done[static_cast<std::size_t>(y) * w + x] || !matches(x, y)) continue;
        int left = x, right = x;
        while (left > 0 && !done[static_cast<std::size_t>(y) * w + left - 1] && matches(left - 1, y)) --left;
        while (right < w - 1 && !done[static_cast<std::size_t>(y) * w + right + 1] && matches(right + 1, y)) ++right;
        for (int i = left; i <= right; ++i) {
            done[static_cast<std::size_t>(y) * w + i] = 1;
            target.set(i, y, color);
            ++filled;
            if (y > 0 && !done[static_cast<std::size_t>(y - 1) * w + i]) stack.emplace_back(i, y - 1);
            if (y < h - 1 && !done[static_cast<std::size_t>(y + 1) * w + i]) stack.emplace_back(i, y + 1);
        }
    }
    return filled;
}

namespace {

// Coverage inside `box` only (row stride box.width()), so small shapes on huge canvases stay cheap.
std::vector<float> coverBox(int width, int height, const std::vector<std::vector<Point>>& contours, bool nonzero, Box& box) {
    std::vector<float> mask;
    box = {0, 0, 0, 0};
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    for (const auto& c : contours) {
        for (const auto& p : c) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
    }
    if (maxX < minX) return mask;
    box = Box{std::max(0, static_cast<int>(std::floor(std::max(-1e9, minX)))), std::max(0, static_cast<int>(std::floor(std::max(-1e9, minY)))),
            std::min(width, static_cast<int>(std::ceil(std::min(1e9, maxX))) + 1), std::min(height, static_cast<int>(std::ceil(std::min(1e9, maxY))) + 1)};
    if (box.empty()) return mask;
    mask.assign(static_cast<std::size_t>(box.width()) * box.height(), 0.0f);
    constexpr int kSub = 5;  // vertical subsamples per pixel row
    std::vector<std::pair<double, int>> xs;  // crossing x and its direction
    for (int y = box.y0; y < box.y1; ++y) {
        float* row = mask.data() + static_cast<std::size_t>(y - box.y0) * box.width() - box.x0;
        for (int s = 0; s < kSub; ++s) {
            const double sy = y + (s + 0.5) / kSub;
            xs.clear();
            for (const auto& pts : contours) {
                if (pts.size() < 3) continue;
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    const Point& a = pts[i];
                    const Point& b = pts[(i + 1) % pts.size()];
                    if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
                        xs.push_back({a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x), b.y > a.y ? 1 : -1});
                    }
                }
            }
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
                wind += nonzero ? xs[i].second : 1;
                const bool inside = nonzero ? wind != 0 : (wind & 1) != 0;
                if (!inside) continue;
                const double l = std::max<double>(xs[i].first, box.x0), r = std::min<double>(xs[i + 1].first, box.x1);
                if (r <= l) continue;
                const int il = static_cast<int>(std::floor(l)), ir = static_cast<int>(std::floor(r));
                if (il == ir) {
                    row[il] += static_cast<float>((r - l) / kSub);
                    continue;
                }
                row[il] += static_cast<float>((il + 1 - l) / kSub);
                for (int x = il + 1; x < ir; ++x) row[x] += 1.0f / kSub;
                if (ir < box.x1) row[ir] += static_cast<float>((r - ir) / kSub);
            }
        }
        for (int x = box.x0; x < box.x1; ++x) row[x] = std::min(row[x], 1.0f);
    }
    return mask;
}

}  // namespace

std::vector<float> polygonCoverage(int width, int height, const std::vector<std::vector<Point>>& contours, bool nonzero) {
    std::vector<float> out(static_cast<std::size_t>(std::max(0, width)) * std::max(0, height), 0.0f);
    Box box{};
    const std::vector<float> local = coverBox(width, height, contours, nonzero, box);
    for (int y = box.y0; y < box.y1; ++y) {
        std::copy_n(local.data() + static_cast<std::size_t>(y - box.y0) * box.width(), box.width(), out.data() + static_cast<std::size_t>(y) * width + box.x0);
    }
    return out;
}

void fillPolygons(Image& target, const std::vector<std::vector<Point>>& contours, Color color, bool nonzero) {
    if (target.empty()) return;
    Box box{};
    const std::vector<float> mask = coverBox(target.width(), target.height(), contours, nonzero, box);
    if (!box.empty()) applyMask(target, box, mask, color, 1.0, false);
}

void fillPolygon(Image& target, const std::vector<Point>& pts, Color color) {
    if (pts.size() < 3) return;
    fillPolygons(target, {pts}, color, false);
}

void fillRect(Image& target, double x, double y, double w, double h, Color color) {
    fillPolygon(target, {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, color);
}

std::vector<Point> ellipsePoints(double cx, double cy, double rx, double ry) {
    int n = std::clamp(static_cast<int>(std::max(rx, ry) * 1.5), 16, 720);
    std::vector<Point> pts;
    pts.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double t = 2 * kPi * i / n;
        pts.push_back({cx + rx * std::cos(t), cy + ry * std::sin(t)});
    }
    return pts;
}

void fillEllipse(Image& target, double cx, double cy, double rx, double ry, Color color) {
    fillPolygon(target, ellipsePoints(cx, cy, rx, ry), color);
}

namespace {

void polyline(Image& target, const std::vector<Point>& pts, bool closed, Color color, double width) {
    Brush b;
    b.color = color;
    b.size = std::max(1.0, width);
    b.hardness = 1.0;
    b.spacing = 0.05;
    b.pressureSize = false;
    std::vector<StrokePoint> sp;
    for (const auto& p : pts) sp.push_back({p.x, p.y, 1.0});
    if (closed && !pts.empty()) sp.push_back({pts[0].x, pts[0].y, 1.0});
    stroke(target, b, sp);
}

}  // namespace

void strokeRect(Image& target, double x, double y, double w, double h, Color color, double width) {
    polyline(target, {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, true, color, width);
}

void strokeEllipse(Image& target, double cx, double cy, double rx, double ry, Color color, double width) {
    polyline(target, ellipsePoints(cx, cy, rx, ry), true, color, width);
}

void drawLine(Image& target, double x1, double y1, double x2, double y2, Color color, double width) {
    polyline(target, {{x1, y1}, {x2, y2}}, false, color, width);
}

}  // namespace wetwaresuite::raster
