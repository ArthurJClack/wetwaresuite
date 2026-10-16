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

void dab(std::vector<float>& mask, const Box& box, double cx, double cy, double radius, double hardness, double alpha) {
    radius = std::max(0.5, radius);
    const int x0 = std::max(box.x0, static_cast<int>(std::floor(cx - radius - 1)));
    const int x1 = std::min(box.x1, static_cast<int>(std::ceil(cx + radius + 1)));
    const int y0 = std::max(box.y0, static_cast<int>(std::floor(cy - radius - 1)));
    const int y1 = std::min(box.y1, static_cast<int>(std::ceil(cy + radius + 1)));
    const double h = std::clamp(hardness, 0.0, 0.999);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            double dist = std::sqrt(dx * dx + dy * dy);
            double edge = std::clamp(radius - dist + 0.5, 0.0, 1.0);  // antialiased rim
            if (edge <= 0) continue;
            double t = dist / radius;
            double fall = t <= h ? 1.0 : std::max(0.0, 1.0 - (t - h) / (1.0 - h));
            fall = fall * fall * (3 - 2 * fall);  // smoothstep
            double a = (hardness >= 0.99 ? edge : std::min(edge, fall)) * alpha;
            float& m = mask[static_cast<std::size_t>(y - box.y0) * box.width() + (x - box.x0)];
            m = m + (1 - m) * static_cast<float>(a);
        }
    }
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

PixelRect strokeBounds(const Image& target, const Brush& brush, const std::vector<StrokePoint>& points) {
    if (points.empty() || target.empty()) return {};
    const double maxR = brush.size / 2 + 2;
    double minX = points[0].x, maxX = minX, minY = points[0].y, maxY = minY;
    for (const auto& p : points) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    auto toInt = [](double v) { return static_cast<int>(std::clamp(v, -1e9, 1e9)); };
    PixelRect r{std::max(0, toInt(std::floor(minX - maxR))), std::max(0, toInt(std::floor(minY - maxR))),
                std::min(target.width(), toInt(std::ceil(maxX + maxR))), std::min(target.height(), toInt(std::ceil(maxY + maxR)))};
    return r.empty() ? PixelRect{} : r;
}

void stroke(Image& target, const Brush& brush, const std::vector<StrokePoint>& points) {
    const PixelRect r = strokeBounds(target, brush, points);
    if (r.empty()) return;
    Box box{r.x0, r.y0, r.x1, r.y1};
    std::vector<float> mask(static_cast<std::size_t>(box.width()) * box.height(), 0.0f);

    auto radiusAt = [&](double p) { return (brush.pressureSize ? brush.size * std::clamp(p, 0.05, 1.0) : brush.size) / 2; };
    auto alphaAt = [&](double p) { return brush.flow * (brush.pressureOpacity ? std::clamp(p, 0.0, 1.0) : 1.0); };

    dab(mask, box, points[0].x, points[0].y, radiusAt(points[0].pressure), brush.hardness, alphaAt(points[0].pressure));
    double carry = 0;  // distance travelled since the last dab
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto& a = points[i - 1];
        const auto& b = points[i];
        double dx = b.x - a.x, dy = b.y - a.y;
        double len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0) continue;
        double pos = 0;
        while (true) {
            double t = pos / len;
            double p = a.pressure + (b.pressure - a.pressure) * t;
            double step = std::max(0.5, 2 * radiusAt(p) * std::max(0.02, brush.spacing));
            double next = pos + (step - carry);
            if (next > len) {
                carry += len - pos;
                break;
            }
            pos = next;
            carry = 0;
            t = pos / len;
            p = a.pressure + (b.pressure - a.pressure) * t;
            dab(mask, box, a.x + dx * t, a.y + dy * t, radiusAt(p), brush.hardness, alphaAt(p));
        }
    }
    applyMask(target, box, mask, brush.color, brush.opacity, brush.erase);
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

void fillPolygon(Image& target, const std::vector<Point>& pts, Color color) {
    if (pts.size() < 3 || target.empty()) return;
    double minX = pts[0].x, maxX = minX, minY = pts[0].y, maxY = minY;
    for (const auto& p : pts) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    Box box{std::max(0, static_cast<int>(std::floor(minX))), std::max(0, static_cast<int>(std::floor(minY))),
            std::min(target.width(), static_cast<int>(std::ceil(maxX)) + 1),
            std::min(target.height(), static_cast<int>(std::ceil(maxY)) + 1)};
    if (box.empty()) return;
    std::vector<float> mask(static_cast<std::size_t>(box.width()) * box.height(), 0.0f);
    constexpr int kSub = 5;  // vertical subsamples per pixel row
    std::vector<double> xs;
    for (int y = box.y0; y < box.y1; ++y) {
        float* row = mask.data() + static_cast<std::size_t>(y - box.y0) * box.width();
        for (int s = 0; s < kSub; ++s) {
            double sy = y + (s + 0.5) / kSub;
            xs.clear();
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const Point& a = pts[i];
                const Point& b = pts[(i + 1) % pts.size()];
                if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
                    xs.push_back(a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
                double l = std::max<double>(xs[i], box.x0), r = std::min<double>(xs[i + 1], box.x1);
                if (r <= l) continue;
                int il = static_cast<int>(std::floor(l)), ir = static_cast<int>(std::floor(r));
                if (il == ir) {
                    row[il - box.x0] += static_cast<float>((r - l) / kSub);
                    continue;
                }
                row[il - box.x0] += static_cast<float>((il + 1 - l) / kSub);
                for (int x = il + 1; x < ir; ++x) row[x - box.x0] += 1.0f / kSub;
                if (ir < box.x1) row[ir - box.x0] += static_cast<float>((r - ir) / kSub);
            }
        }
    }
    for (auto& m : mask) m = std::min(m, 1.0f);
    applyMask(target, box, mask, color, 1.0, false);
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
