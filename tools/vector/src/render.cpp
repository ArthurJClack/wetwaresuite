// Rasterising a vector document. Shapes become coverage masks (an active-edge
// scanline filler with 5 subsamples per row, nonzero or even-odd), which are
// painted with a colour, gradient or pattern. Shapes with opacity, a blend
// mode or effects, and groups with a clip or mask, are drawn on their own
// layer first and then composited, like Illustrator's isolated groups.
#include <algorithm>
#include <cmath>
#include <map>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/filters.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "internal.hpp"

namespace wetwaresuite::vector {

namespace detail {

Cover rasterize(const Outline& o, bool nonzero, int W, int H) {
    Cover c;
    struct E {
        double x0, y0, y1, dxdy;
        int dir;
    };
    std::vector<E> edges;
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const auto& ct : o) {
        const auto& pts = ct.points;
        const std::size_t n = pts.size();
        if (n < 2) continue;
        for (std::size_t i = 0; i < n; ++i) {
            const Point a = pts[i], b = pts[(i + 1) % n];
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) continue;
            minX = std::min(minX, a.x), maxX = std::max(maxX, a.x), minY = std::min(minY, a.y), maxY = std::max(maxY, a.y);
            if (a.y == b.y) continue;
            const bool down = b.y > a.y;
            const Point t = down ? a : b, u = down ? b : a;
            edges.push_back({t.x, t.y, u.y, (u.x - t.x) / (u.y - t.y), down ? 1 : -1});
        }
    }
    if (edges.empty()) return c;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::max(-1e9, minX))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::max(-1e9, minY))));
    const int x1 = std::min(W, static_cast<int>(std::ceil(std::min(1e9, maxX))) + 1);
    const int y1 = std::min(H, static_cast<int>(std::ceil(std::min(1e9, maxY))) + 1);
    if (x1 <= x0 || y1 <= y0) return c;
    c.x0 = x0, c.y0 = y0, c.w = x1 - x0, c.h = y1 - y0;
    c.a.assign(static_cast<std::size_t>(c.w) * c.h, 0.0f);
    std::sort(edges.begin(), edges.end(), [](const E& a, const E& b) { return a.y0 < b.y0; });
    constexpr int kSub = 5;
    std::vector<std::size_t> active;
    std::vector<std::pair<double, int>> xs;
    std::size_t next = 0;
    for (int y = y0; y < y1; ++y) {
        float* row = c.a.data() + static_cast<std::size_t>(y - y0) * c.w - x0;
        for (int s = 0; s < kSub; ++s) {
            const double sy = y + (s + 0.5) / kSub;
            while (next < edges.size() && edges[next].y0 <= sy) active.push_back(next++);
            xs.clear();
            std::size_t keep = 0;
            for (std::size_t k = 0; k < active.size(); ++k) {
                const E& e = edges[active[k]];
                if (e.y1 <= sy) continue;  // finished
                active[keep++] = active[k];
                if (e.y0 <= sy) xs.push_back({e.x0 + (sy - e.y0) * e.dxdy, e.dir});
            }
            active.resize(keep);
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
                wind += nonzero ? xs[i].second : 1;
                const bool inside = nonzero ? wind != 0 : (wind & 1) != 0;
                if (!inside) continue;
                const double l = std::max<double>(xs[i].first, x0), r = std::min<double>(xs[i + 1].first, x1);
                if (r <= l) continue;
                const int il = static_cast<int>(std::floor(l)), ir = static_cast<int>(std::floor(r));
                if (il == ir) {
                    row[il] += static_cast<float>((r - l) / kSub);
                    continue;
                }
                row[il] += static_cast<float>((il + 1 - l) / kSub);
                for (int x = il + 1; x < ir; ++x) row[x] += 1.0f / kSub;
                if (ir < x1) row[ir] += static_cast<float>((r - ir) / kSub);
            }
        }
        for (int x = x0; x < x1; ++x) row[x] = std::min(row[x], 1.0f);
    }
    return c;
}

}  // namespace detail

using detail::Cover;
using detail::rasterize;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Source-over one straight-alpha colour onto a pixel, with extra coverage `k` (0..1).
inline void over(std::uint8_t* d, Color c, float k) {
    const float sa = c.a / 255.0f * k;
    if (sa <= 0) return;
    const float da = d[3] / 255.0f, oa = sa + da * (1 - sa);
    if (oa <= 0) return;
    const float ws = sa / oa, wd = da * (1 - sa) / oa;
    d[0] = static_cast<std::uint8_t>(std::lround(c.r * ws + d[0] * wd));
    d[1] = static_cast<std::uint8_t>(std::lround(c.g * ws + d[1] * wd));
    d[2] = static_cast<std::uint8_t>(std::lround(c.b * ws + d[2] * wd));
    d[3] = static_cast<std::uint8_t>(std::lround(oa * 255));
}

// What colour a pixel (device coordinates of its centre) gets.
struct Paint {
    enum Kind { Solid, Gradient, Pattern } kind = Solid;
    Color color;
    const vector::Gradient* gradient = nullptr;
    const vector::Pattern* pattern = nullptr;
    Matrix toLocal;  // device -> the shape's own coordinates
    Bounds box;      // the shape's own box

    Color at(double x, double y) const {
        if (kind == Solid) return color;
        if (kind == Gradient) {
            const Point l = toLocal.apply({x, y});
            const double u = (l.x - box.x0) / std::max(box.width(), 1e-9), v = (l.y - box.y0) / std::max(box.height(), 1e-9);
            const vector::Gradient& g = *gradient;
            if (g.freeform) return g.atPoint(u, v);
            if (g.radial) return g.at(std::hypot(u - g.cx, v - g.cy) / std::max(g.r, 1e-9));
            const double dx = g.x2 - g.x1, dy = g.y2 - g.y1;
            return g.at(((u - g.x1) * dx + (v - g.y1) * dy) / std::max(dx * dx + dy * dy, 1e-12));
        }
        // patterns: 2 x 2 samples for smooth edges
        int r = 0, gg = 0, b = 0, a = 0;
        for (int sy = 0; sy < 2; ++sy) {
            for (int sx = 0; sx < 2; ++sx) {
                const Point l = toLocal.apply({x + (sx - 0.5) * 0.5, y + (sy - 0.5) * 0.5});
                const Color c = patternAt(l.x - box.x0, l.y - box.y0);
                r += c.r * c.a, gg += c.g * c.a, b += c.b * c.a, a += c.a;
            }
        }
        if (a == 0) return {0, 0, 0, 0};
        return {static_cast<std::uint8_t>(r / a), static_cast<std::uint8_t>(gg / a), static_cast<std::uint8_t>(b / a), static_cast<std::uint8_t>(a / 4)};
    }

    Color patternAt(double px, double py) const {
        const vector::Pattern& p = *pattern;
        const double t = -p.angle * kPi / 180, cs = std::cos(t), sn = std::sin(t);
        const double u = (px * cs - py * sn) / std::max(p.size, 1e-6), v = (px * sn + py * cs) / std::max(p.size, 1e-6);
        const double fu = u - std::floor(u), fv = v - std::floor(v), w = std::clamp(p.weight, 0.02, 0.98);
        bool mark = false;
        if (p.type == "stripes") mark = fv < w;
        else if (p.type == "dots") mark = std::hypot(fu - 0.5, fv - 0.5) < w * 0.5;
        else if (p.type == "grid") mark = fu < w * 0.4 || fv < w * 0.4;
        else if (p.type == "checker") mark = ((static_cast<long long>(std::floor(u)) + static_cast<long long>(std::floor(v))) & 1) == 0;
        else if (p.type == "hatch") {
            const double d1 = (u + v) - std::floor(u + v), d2 = (u - v) - std::floor(u - v);
            mark = d1 < w * 0.4 || d2 < w * 0.4;
        }
        if (mark) return p.color;
        return p.background.value_or(Color{0, 0, 0, 0});
    }
};

void composite(raster::Image& dst, const Cover& c, const Paint& paint, float opacity) {
    for (int y = 0; y < c.h; ++y) {
        const float* row = c.a.data() + static_cast<std::size_t>(y) * c.w;
        const int py = c.y0 + y;
        for (int x = 0; x < c.w; ++x) {
            const float k = row[x] * opacity;
            if (k <= 0) continue;
            const int px = c.x0 + x;
            over(dst.pixel(px, py), paint.kind == Paint::Solid ? paint.color : paint.at(px + 0.5, py + 0.5), k);
        }
    }
}

// a * b (inside) or a * (1 - b) (outside), in a's box
void maskCover(Cover& a, const Cover& b, bool inside) {
    for (int y = 0; y < a.h; ++y) {
        for (int x = 0; x < a.w; ++x) {
            float& v = a.a[static_cast<std::size_t>(y) * a.w + x];
            const float m = b.at(a.x0 + x, a.y0 + y);
            v *= inside ? m : 1 - m;
        }
    }
}

Outline transformed(Outline o, const Matrix& m) {
    for (auto& c : o) {
        for (auto& p : c.points) p = m.apply(p);
    }
    return o;
}

class Renderer {
public:
    Renderer(const VectorDoc& doc, int W, int H) : doc_(doc), W_(W), H_(H) {}

    void drawList(raster::Image& dst, const std::vector<Shape>& list, const std::string& parent, const Matrix& m, int depth) {
        for (std::size_t i : kids(list, parent)) drawItem(dst, list, i, m, depth);
    }

    void drawItem(raster::Image& dst, const std::vector<Shape>& list, std::size_t i, const Matrix& m, int depth, bool force = false) {
        if (depth > 24) return;
        const Shape& s = list[i];
        if ((!s.visible && !force) || s.opacity <= 0) return;
        const raster::Blend blend = raster::parseBlend(s.blend).value_or(raster::Blend::Normal);
        const bool group = s.kind == ShapeKind::Group;
        const bool isolate = s.opacity < 1 || blend != raster::Blend::Normal || !s.effects.empty() || (group && (!s.clip.empty() || !s.mask.empty()));
        raster::Image layer;
        if (isolate) layer = raster::Image(W_, H_);
        raster::Image& target = isolate ? layer : dst;
        if (group) {
            int clipIdx = -1, maskIdx = -1;
            for (std::size_t k : kids(list, s.id)) {
                if (list[k].id == s.clip) clipIdx = static_cast<int>(k);
                else if (list[k].id == s.mask) maskIdx = static_cast<int>(k);
                else drawItem(target, list, k, m, depth + 1);
            }
            if (clipIdx >= 0) {
                Outline o;
                gather(list, static_cast<std::size_t>(clipIdx), m, o, depth + 1);
                const Shape& cs = list[static_cast<std::size_t>(clipIdx)];
                applyCover(target, rasterize(o, cs.kind == ShapeKind::Text || cs.fillRule == "nonzero", W_, H_));
            }
            if (maskIdx >= 0) {
                raster::Image mask(W_, H_);
                drawItem(mask, list, static_cast<std::size_t>(maskIdx), m, depth + 1, true);
                applyLuminance(target, mask);
            }
        } else if (s.kind == ShapeKind::Instance) {
            if (const Symbol* sym = doc_.findSymbol(s.symbol)) drawList(target, sym->shapes, "", m * shapeMatrix(s) * Matrix::translate(s.cx, s.cy), depth + 1);
        } else {
            drawLeaf(target, s, m);
        }
        if (!isolate) return;
        if (!s.effects.empty()) effects(dst, layer, s, m.scaleFactor());
        raster::blendOnto(dst, layer, 0, 0, blend, s.opacity);
    }

private:
    const VectorDoc& doc_;
    int W_, H_;
    std::map<const std::vector<Shape>*, std::map<std::string, std::vector<std::size_t>>> kids_;

    const std::vector<std::size_t>& kids(const std::vector<Shape>& list, const std::string& parent) {
        auto it = kids_.find(&list);
        if (it == kids_.end()) {
            auto& m = kids_[&list];
            for (std::size_t i = 0; i < list.size(); ++i) m[list[i].parent].push_back(i);
            it = kids_.find(&list);
        }
        static const std::vector<std::size_t> none;
        auto k = it->second.find(parent);
        return k == it->second.end() ? none : k->second;
    }

    // the device outline of a shape and what's inside it (for clipping paths)
    void gather(const std::vector<Shape>& list, std::size_t i, const Matrix& m, Outline& out, int depth) {
        if (depth > 24) return;
        const Shape& s = list[i];
        if (s.kind == ShapeKind::Group) {
            for (std::size_t k : kids(list, s.id)) gather(list, k, m, out, depth + 1);
            return;
        }
        if (s.kind == ShapeKind::Instance) {
            if (const Symbol* sym = doc_.findSymbol(s.symbol)) {
                const Matrix inner = m * shapeMatrix(s) * Matrix::translate(s.cx, s.cy);
                for (std::size_t k : kids(sym->shapes, "")) gather(sym->shapes, k, inner, out, depth + 1);
            }
            return;
        }
        const Matrix full = m * shapeMatrix(s);
        Outline o = transformed(localOutline(s, std::max(0.05, full.scaleFactor())), full);
        out.insert(out.end(), o.begin(), o.end());
    }

    void applyCover(raster::Image& img, const Cover& c) {
        for (int y = 0; y < H_; ++y) {
            for (int x = 0; x < W_; ++x) {
                std::uint8_t* p = img.pixel(x, y);
                if (!p[3]) continue;
                p[3] = static_cast<std::uint8_t>(std::lround(p[3] * c.at(x, y)));
            }
        }
    }

    static void applyLuminance(raster::Image& img, const raster::Image& mask) {
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                std::uint8_t* p = img.pixel(x, y);
                if (!p[3]) continue;
                const std::uint8_t* m = mask.pixel(x, y);
                const double lum = (0.2126 * m[0] + 0.7152 * m[1] + 0.0722 * m[2]) / 255 * (m[3] / 255.0);
                p[3] = static_cast<std::uint8_t>(std::lround(p[3] * lum));
            }
        }
    }

    Paint fillPaint(const Shape& s, const Matrix& full, const std::optional<Gradient>& grad, Color flat, bool usePattern) const {
        Paint p;
        p.color = flat;
        if ((usePattern && s.pattern) || (grad && (!grad->stops.empty() || !grad->points.empty()))) {
            if (!full.invert(p.toLocal)) return p;
            p.box = bounds(localOutline(s, 1.0));
            if (usePattern && s.pattern) {
                p.kind = Paint::Pattern;
                p.pattern = &*s.pattern;
            } else {
                p.kind = Paint::Gradient;
                p.gradient = &*grad;
            }
        }
        return p;
    }

    void strokeWith(raster::Image& dst, const Outline& o, double width, const StrokeStyle& st, bool fillNonzero, const Paint& paint, float opacity) {
        bool aligned = st.align != "center";
        if (aligned) {
            aligned = false;
            for (const auto& c : o) aligned = aligned || (c.closed && c.points.size() >= 3);
        }
        if (!aligned) {
            composite(dst, rasterize(strokeArea(o, width, st), true, W_, H_), paint, opacity);
            return;
        }
        StrokeStyle twice = st;
        twice.align = "center";
        Cover c = rasterize(strokeArea(o, width * 2, twice), true, W_, H_);
        Outline closed;
        for (const auto& ct : o) {
            if (ct.closed) closed.push_back(ct);
        }
        maskCover(c, rasterize(closed, fillNonzero, W_, H_), st.align == "inside");
        composite(dst, c, paint, opacity);
    }

    void drawLeaf(raster::Image& dst, const Shape& s, const Matrix& m) {
        const Matrix full = m * shapeMatrix(s);
        const double k = full.scaleFactor();
        const Outline o = transformed(localOutline(s, std::max(0.05, k)), full);
        if (o.empty()) return;
        const bool nonzero = s.kind == ShapeKind::Text || s.fillRule == "nonzero";
        const bool canFill = s.kind != ShapeKind::Line;
        const double mk = m.scaleFactor();
        auto item = [&](const AppearanceItem& a) {
            const Outline shifted = (a.dx != 0 || a.dy != 0) ? transformed(o, Matrix::translate(a.dx * mk, a.dy * mk)) : o;
            Paint p;
            p.color = a.color;
            const raster::Blend b = raster::parseBlend(a.blend).value_or(raster::Blend::Normal);
            raster::Image layer;
            raster::Image& target = b == raster::Blend::Normal ? dst : (layer = raster::Image(W_, H_), layer);
            const float op = b == raster::Blend::Normal ? static_cast<float>(a.opacity) : 1.0f;
            if (a.stroke) strokeWith(target, shifted, a.width * k, a.style, nonzero, p, op);
            else if (canFill) composite(target, rasterize(shifted, nonzero, W_, H_), p, op);
            if (b != raster::Blend::Normal) raster::blendOnto(dst, layer, 0, 0, b, a.opacity);
        };
        for (const auto& a : s.appearance) {
            if (a.under) item(a);
        }
        const bool base = s.gradient.has_value() || (s.fill && s.fill->a > 0);
        if (canFill && (base || s.pattern)) {  // the colour or gradient, then the pattern's marks over it
            const Cover c = rasterize(o, nonzero, W_, H_);
            if (base) composite(dst, c, fillPaint(s, full, s.gradient, s.fill.value_or(Color{0, 0, 0, 0}), false), 1.0f);
            if (s.pattern) composite(dst, c, fillPaint(s, full, std::nullopt, Color{}, true), 1.0f);
        }
        if (((s.stroke && s.stroke->a > 0) || s.strokeGradient) && s.strokeWidth > 0) {
            strokeWith(dst, o, s.strokeWidth * k, s.line, nonzero, fillPaint(s, full, s.strokeGradient, s.stroke.value_or(Color{0, 0, 0, 255}), false), 1.0f);
        }
        for (const auto& a : s.appearance) {
            if (!a.under) item(a);
        }
    }

    // Shadows and glows outside the shape go under it; inner ones over it, inside its edge.
    void effects(raster::Image& dst, raster::Image& layer, const Shape& s, double k) {
        (void)dst;
        for (const auto& e : s.effects) {
            if (e.type == "blur") raster::gaussianBlur(layer, e.radius * k);
        }
        raster::Image under(W_, H_);
        bool anyUnder = false;
        for (const auto& e : s.effects) {
            const bool outer = e.type == "drop_shadow" || e.type == "outer_glow";
            const bool inner = e.type == "inner_shadow" || e.type == "inner_glow";
            if (!outer && !inner) continue;
            raster::Image fx(W_, H_);
            const int ox = e.type == "drop_shadow" || e.type == "inner_shadow" ? static_cast<int>(std::lround(e.dx * k)) : 0;
            const int oy = e.type == "drop_shadow" || e.type == "inner_shadow" ? static_cast<int>(std::lround(e.dy * k)) : 0;
            for (int y = 0; y < H_; ++y) {
                for (int x = 0; x < W_; ++x) {
                    const int sx = x - ox, sy = y - oy;
                    double a = 0;
                    if (sx >= 0 && sy >= 0 && sx < W_ && sy < H_) a = layer.pixel(sx, sy)[3] / 255.0;
                    if (inner) a = 1 - a;
                    std::uint8_t* p = fx.pixel(x, y);
                    p[0] = e.color.r, p[1] = e.color.g, p[2] = e.color.b;
                    p[3] = static_cast<std::uint8_t>(std::lround(a * 255));
                }
            }
            raster::gaussianBlur(fx, std::max(0.0, e.radius * k));
            if (outer) {
                raster::blendOnto(under, fx, 0, 0, raster::Blend::Normal, e.opacity);
                anyUnder = true;
                continue;
            }
            for (int y = 0; y < H_; ++y) {  // inside the shape only, keeping its own alpha
                for (int x = 0; x < W_; ++x) {
                    std::uint8_t* p = layer.pixel(x, y);
                    if (!p[3]) continue;
                    const float ia = fx.pixel(x, y)[3] / 255.0f * static_cast<float>(e.opacity) * (e.color.a / 255.0f);
                    for (int ch = 0; ch < 3; ++ch) p[ch] = static_cast<std::uint8_t>(std::lround(p[ch] * (1 - ia) + fx.pixel(x, y)[ch] * ia));
                }
            }
        }
        if (anyUnder) {
            raster::blendOnto(under, layer, 0, 0, raster::Blend::Normal, 1.0);
            layer = std::move(under);
        }
    }
};

}  // namespace

Color Gradient::at(double t) const {
    if (stops.empty()) return points.empty() ? Color{} : points.front().color;
    t = std::clamp(t, 0.0, 1.0);
    if (t <= stops.front().offset) return stops.front().color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        const GradientStop& a = stops[i - 1];
        const GradientStop& b = stops[i];
        if (t > b.offset) continue;
        const double k = b.offset > a.offset ? (t - a.offset) / (b.offset - a.offset) : 1;
        auto mixc = [&](std::uint8_t p, std::uint8_t q) { return static_cast<std::uint8_t>(std::lround(p + (q - p) * k)); };
        return {mixc(a.color.r, b.color.r), mixc(a.color.g, b.color.g), mixc(a.color.b, b.color.b), mixc(a.color.a, b.color.a)};
    }
    return stops.back().color;
}

Color Gradient::atPoint(double u, double v) const {
    if (points.empty()) return at(u);
    double r = 0, g = 0, b = 0, a = 0, total = 0;
    for (const auto& p : points) {
        const double d2 = (u - p.x) * (u - p.x) + (v - p.y) * (v - p.y);
        if (d2 < 1e-10) return p.color;
        const double w = 1 / (d2 * d2);  // inverse distance, power 4: each colour holds its area
        r += p.color.r * w, g += p.color.g * w, b += p.color.b * w, a += p.color.a * w;
        total += w;
    }
    auto c8 = [&](double v8) { return static_cast<std::uint8_t>(std::clamp(std::lround(v8 / total), 0L, 255L)); };
    return {c8(r), c8(g), c8(b), c8(a)};
}

raster::Image renderArea(const VectorDoc& doc, double x, double y, double w, double h, double scale, bool background) {
    if (!(scale > 0)) scale = 1.0;
    const int W = std::max(1, static_cast<int>(std::lround(w * scale)));
    const int H = std::max(1, static_cast<int>(std::lround(h * scale)));
    raster::Image img(W, H, background ? doc.background : Color{0, 0, 0, 0});
    Renderer r(doc, W, H);
    r.drawList(img, doc.shapes, "", Matrix::scale(scale, scale) * Matrix::translate(-x, -y), 0);
    return img;
}

raster::Image render(const VectorDoc& doc, double scale) { return renderArea(doc, 0, 0, doc.width, doc.height, scale, true); }

raster::Image renderShape(const VectorDoc& doc, int index, double x, double y, double w, double h, double scale) {
    const int W = std::max(1, static_cast<int>(std::lround(w * scale)));
    const int H = std::max(1, static_cast<int>(std::lround(h * scale)));
    raster::Image img(W, H);
    if (index < 0 || index >= static_cast<int>(doc.shapes.size())) return img;
    Renderer r(doc, W, H);
    r.drawItem(img, doc.shapes, static_cast<std::size_t>(index), Matrix::scale(scale, scale) * Matrix::translate(-x, -y), 0, true);
    return img;
}

}  // namespace wetwaresuite::vector
