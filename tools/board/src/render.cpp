// Drawing a board: elements become a display list (paths, text, images),
// including the hand-drawn "sketchy" strokes and hachure fills, and the same
// list goes to a PNG, an SVG or a vector PDF, so all three look alike.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <variant>

#include "achclwss/pdf/page.hpp"
#include "achclwss/pdfdoc/writer.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/ops.hpp"
#include "achclwss/raster/paint.hpp"
#include "internal.hpp"

namespace achclwss::board {

using namespace detail;
namespace fs = std::filesystem;

namespace {

struct PathOp {
    std::vector<Contour> contours;
    std::optional<Color> fill, stroke;
    double width = 1;
    std::string dash = "solid";
};
struct TextOp {
    double x = 0, y = 0;
    std::string text;
    std::string font = "sans";
    bool bold = false, italic = false;
    double size = 16;
    Color color;
};
struct ImageOp {
    double x = 0, y = 0, w = 0, h = 0;
    std::shared_ptr<raster::Image> img;
    std::string src;
};
struct Op {
    std::variant<PathOp, TextOp, ImageOp> v;
    std::string owner;  // element id, for SVG groups
};

struct Scene {
    Box region;
    Color background;
    bool transparent = false;
    std::vector<Op> ops;
};

Pt norm(Pt v) {
    const double l = std::hypot(v.x, v.y);
    return l > 1e-9 ? Pt{v.x / l, v.y / l} : Pt{1, 0};
}

Color withAlpha(Color c, double a) {
    c.a = static_cast<std::uint8_t>(std::clamp(std::lround(c.a * a), 0L, 255L));
    return c;
}

// ---- hand-drawn strokes ----------------------------------------------------------------------

std::vector<Pt> roughLine(Pt a, Pt b, double roughness, Rng& rng) {
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    if (len < 1e-6) return {a, b};
    const Pt d{(b.x - a.x) / len, (b.y - a.y) / len}, n{-d.y, d.x};
    const double o = roughness * std::clamp(len * 0.015, 0.6, 2.2);
    const double over0 = rng.range(0, roughness * std::min(4.0, len * 0.05)), over1 = rng.range(0, roughness * std::min(4.0, len * 0.05));
    const Pt a1{a.x - d.x * over0 + rng.range(-o, o), a.y - d.y * over0 + rng.range(-o, o)};
    const Pt b1{b.x + d.x * over1 + rng.range(-o, o), b.y + d.y * over1 + rng.range(-o, o)};
    const double bow = rng.range(-1, 1) * roughness * std::min(len * 0.018, 4.0);
    const double t = 0.5 + rng.range(-0.1, 0.1);
    const Pt m{a1.x + (b1.x - a1.x) * t + n.x * bow * 2, a1.y + (b1.y - a1.y) * t + n.y * bow * 2};
    std::vector<Pt> out;
    const int steps = std::clamp(static_cast<int>(len / 12), 3, 16);
    for (int i = 0; i <= steps; ++i) {
        const double u = static_cast<double>(i) / steps, v = 1 - u;
        out.push_back({v * v * a1.x + 2 * v * u * m.x + u * u * b1.x, v * v * a1.y + 2 * v * u * m.y + u * u * b1.y});
    }
    return out;
}

std::vector<Pt> resample(const std::vector<Pt>& pts, double step) {
    std::vector<Pt> out;
    const double L = length(pts);
    if (L <= 0) return pts;
    const int n = std::max(2, static_cast<int>(L / step));
    for (int i = 0; i <= n; ++i) out.push_back(pointAt(pts, L * i / n));
    return out;
}

std::vector<Pt> roughCurve(const std::vector<Pt>& pts, double roughness, Rng& rng, bool closed) {
    std::vector<Pt> src = pts;
    if (closed && !src.empty()) src.push_back(src.front());
    std::vector<Pt> r = resample(src, 4);
    const double L = length(src);
    if (r.size() < 2) return r;
    if (closed) {  // overlap the start a little, like a pen going round
        const double extra = std::min(L * 0.08, 30.0);
        const auto ext = resample(std::vector<Pt>(src.begin(), src.begin() + std::min<std::size_t>(src.size(), 4)), 4);
        for (const auto& p : ext) {
            if (length(std::vector<Pt>{r.front(), p}) > extra) break;
            r.push_back(p);
        }
    }
    const double amp = roughness * 1.1;
    const double f1 = std::max(1.0, L / 140), ph1 = rng.range(0, 2 * kPi), ph2 = rng.range(0, 2 * kPi);
    std::vector<Pt> out;
    const std::size_t n = r.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Pt a = r[i > 0 ? i - 1 : 0], b = r[i + 1 < n ? i + 1 : n - 1];
        const Pt d = norm({b.x - a.x, b.y - a.y});
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        const double off = amp * (0.7 * std::sin(2 * kPi * f1 * t + ph1) + 0.3 * std::sin(2 * kPi * f1 * 2.3 * t + ph2)) + rng.range(-0.15, 0.15) * roughness;
        out.push_back({r[i].x - d.y * off, r[i].y + d.x * off});
    }
    return out;
}

// Two wobbly passes over each run of a contour: straight edges overshoot their corners, curves wander.
std::vector<std::vector<Pt>> roughen(const Contour& c, double roughness, Rng& rng, int passes = 2) {
    std::vector<std::vector<Pt>> out;
    const auto& p = c.pts;
    if (p.size() < 2) return out;
    // split at sharp corners
    const std::size_t n = p.size();
    std::vector<std::size_t> corners;
    for (std::size_t i = 0; i < n; ++i) {
        if (!c.closed && (i == 0 || i + 1 == n)) {
            corners.push_back(i);
            continue;
        }
        const Pt a = p[(i + n - 1) % n], b = p[i], d = p[(i + 1) % n];
        const Pt u = norm({b.x - a.x, b.y - a.y}), v = norm({d.x - b.x, d.y - b.y});
        if (u.x * v.x + u.y * v.y < std::cos(35 * kPi / 180)) corners.push_back(i);
    }
    std::vector<std::vector<Pt>> runs;
    if (corners.empty()) {
        runs.push_back(p);
        for (int k = 0; k < passes; ++k) out.push_back(roughCurve(p, roughness, rng, c.closed));
        return out;
    }
    const std::size_t m = corners.size();
    const std::size_t count = c.closed ? m : m - 1;
    for (std::size_t k = 0; k < count; ++k) {
        std::vector<Pt> run;
        const std::size_t s = corners[k], e = corners[(k + 1) % m];
        for (std::size_t i = s;; i = (i + 1) % n) {
            run.push_back(p[i]);
            if (i == e) break;
            if (run.size() > n + 1) break;
        }
        runs.push_back(run);
    }
    for (int k = 0; k < passes; ++k) {
        for (const auto& run : runs) {
            if (run.size() <= 2) out.push_back(roughLine(run.front(), run.back(), roughness, rng));
            else out.push_back(roughCurve(run, roughness, rng, false));
        }
    }
    return out;
}

// Parallel lines at `angle` degrees clipped to the contours (even-odd), `gap` apart.
std::vector<std::pair<Pt, Pt>> hachure(const std::vector<Contour>& contours, double angle, double gap) {
    std::vector<std::pair<Pt, Pt>> out;
    const double a = angle * kPi / 180, ca = std::cos(a), sa = std::sin(a);
    // rotate by -angle so hachures are horizontal
    std::vector<std::vector<Pt>> rot;
    double y0 = 1e300, y1 = -1e300;
    for (const auto& c : contours) {
        std::vector<Pt> r;
        for (const auto& p : c.pts) {
            r.push_back({p.x * ca + p.y * sa, -p.x * sa + p.y * ca});
            y0 = std::min(y0, r.back().y);
            y1 = std::max(y1, r.back().y);
        }
        rot.push_back(r);
    }
    if (!(y1 > y0) || gap <= 0) return out;
    for (double y = y0 + gap / 2; y < y1; y += gap) {
        std::vector<double> xs;
        for (const auto& r : rot) {
            for (std::size_t i = 0; i < r.size(); ++i) {
                const Pt p = r[i], q = r[(i + 1) % r.size()];
                if ((p.y <= y) == (q.y <= y)) continue;
                xs.push_back(p.x + (y - p.y) / (q.y - p.y) * (q.x - p.x));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
            const double xa = xs[i] + 1, xb = xs[i + 1] - 1;
            if (xb <= xa) continue;
            out.push_back({{xa * ca - y * sa, xa * sa + y * ca}, {xb * ca - y * sa, xb * sa + y * ca}});
        }
    }
    return out;
}

// ---- building the display list ---------------------------------------------------------------

struct Builder {
    const Board& board;
    Scene& scene;
    std::string owner;

    void path(std::vector<Contour> contours, std::optional<Color> fill, std::optional<Color> stroke, double width, const std::string& dash = "solid") {
        if (!fill && !stroke) return;
        scene.ops.push_back({PathOp{std::move(contours), fill, stroke, width, dash}, owner});
    }

    void strokeOpen(const std::vector<Pt>& pts, Color c, double width, const std::string& dash = "solid") {
        path({Contour{pts, false}}, std::nullopt, c, width, dash);
    }

    void text(const std::vector<PlacedLine>& lines, const Resolved& s, Color color) {
        for (const auto& l : lines) {
            if (l.text.empty()) continue;
            scene.ops.push_back({TextOp{l.x, l.y, l.text, s.font, s.bold, s.italic, s.fontSize, color}, owner});
        }
    }

    void fillAndStroke(const ShapeGeom& g, const Resolved& s, double alpha, std::uint64_t seed) {
        Rng rng(seed);
        if (s.fill && g.body.size()) {
            const Color fc = withAlpha(*s.fill, alpha);
            if (s.fillStyle == "hachure" || s.fillStyle == "cross") {
                const double gap = std::max(4.0, s.strokeWidth * 4);
                std::vector<std::pair<Pt, Pt>> lines = hachure(g.body, -41, gap);
                if (s.fillStyle == "cross") {
                    auto more = hachure(g.body, 49, gap);
                    lines.insert(lines.end(), more.begin(), more.end());
                }
                const double w = std::max(1.0, s.strokeWidth * 0.6);
                for (const auto& [a, b] : lines) {
                    if (s.sketch) strokeOpen(roughLine(a, b, s.roughness * 0.6, rng), fc, w);
                    else strokeOpen({a, b}, fc, w);
                }
            } else if (s.fillStyle != "none") {
                path(g.body, fc, std::nullopt, 0);
            }
        }
        if (!s.stroke || s.strokeWidth <= 0) return;
        const Color sc = withAlpha(*s.stroke, alpha);
        auto all = g.body;
        all.insert(all.end(), g.lines.begin(), g.lines.end());
        for (const auto& c : all) {
            if (s.sketch && s.roughness > 0) {
                for (auto& run : roughen(c, s.roughness, rng)) strokeOpen(run, sc, s.strokeWidth * 0.9, s.dash);
            } else {
                path({c}, std::nullopt, sc, s.strokeWidth, s.dash);
            }
        }
    }

    void shape(const Element& e, double layerAlpha) {
        const Resolved s = resolve(board, e);
        const double alpha = s.opacity * layerAlpha;
        const std::uint64_t seed = hashOf(e.id);
        if (e.kind == Kind::Image) {
            auto img = loadImageSrc(e.src);
            if (img && !img->empty()) {
                ImageOp op{e.x, e.y, e.w, e.h, img, e.src};
                scene.ops.push_back({op, owner});
            } else {  // placeholder: a crossed box
                const Color c = withAlpha(Color{150, 150, 150, 255}, alpha);
                path({Contour{{{e.x, e.y}, {e.x + e.w, e.y}, {e.x + e.w, e.y + e.h}, {e.x, e.y + e.h}}, true}}, withAlpha(Color{235, 235, 235, 255}, alpha), c, 1);
                strokeOpen({{e.x, e.y}, {e.x + e.w, e.y + e.h}}, c, 1);
                strokeOpen({{e.x + e.w, e.y}, {e.x, e.y + e.h}}, c, 1);
            }
            if (s.stroke && e.style.stroke) {
                Resolved border = s;
                border.fill.reset();
                fillAndStroke(ShapeGeom{shapeGeometry(e).body, {}}, border, alpha, seed);
            }
            if (!e.text.empty()) {
                Resolved ts = s;
                ts.align = "center";
                ts.valign = "top";
                text(placeText(e, ts, textArea(e, ts)), ts, withAlpha(s.text, alpha));
            }
            return;
        }
        if (e.kind != Kind::Text) {
            const ShapeGeom g = shapeGeometry(e, s.radius);
            if (s.shadow && !g.body.empty() && s.fill) {
                std::vector<Contour> sh = g.body;
                for (auto& c : sh) {
                    for (auto& p : c.pts) p = {p.x + 3, p.y + 5};
                }
                path(sh, withAlpha(Color{0, 0, 0, 40}, alpha), std::nullopt, 0);
            }
            fillAndStroke(g, s, alpha, seed);
        } else if (s.fill || s.stroke) {
            Element box = e;
            box.kind = Kind::Rect;
            fillAndStroke(shapeGeometry(box), s, alpha, seed);
        }
        text(placeText(e, s, textArea(e, s)), s, withAlpha(s.text, alpha));
    }

    void arrowHead(const std::string& type, Pt tip, Pt dir, double size, Color c, double width, bool sketch, Rng& rng) {
        const Pt n{-dir.y, dir.x};
        auto line = [&](Pt a, Pt b) {
            if (sketch) strokeOpen(roughLine(a, b, 0.6, rng), c, width);
            else strokeOpen({a, b}, c, width);
        };
        if (type == "arrow") {
            const double s = size * 1.05, sp = 0.45;
            const Pt b{tip.x - dir.x * s, tip.y - dir.y * s};
            line({b.x + n.x * s * sp, b.y + n.y * s * sp}, tip);
            line({b.x - n.x * s * sp, b.y - n.y * s * sp}, tip);
        } else if (type == "triangle") {
            const Pt b{tip.x - dir.x * size, tip.y - dir.y * size};
            path({Contour{{tip, {b.x + n.x * size * 0.42, b.y + n.y * size * 0.42}, {b.x - n.x * size * 0.42, b.y - n.y * size * 0.42}}, true}}, c, c, std::max(1.0, width * 0.5));
        } else if (type == "diamond") {
            const double l = size * 1.4, w = size * 0.42;
            const Pt m{tip.x - dir.x * l / 2, tip.y - dir.y * l / 2}, e{tip.x - dir.x * l, tip.y - dir.y * l};
            path({Contour{{tip, {m.x + n.x * w, m.y + n.y * w}, e, {m.x - n.x * w, m.y - n.y * w}}, true}}, c, c, 1);
        } else if (type == "circle") {
            const double r = size * 0.38;
            const Pt m{tip.x - dir.x * r, tip.y - dir.y * r};
            std::vector<Pt> pts;
            for (int i = 0; i < 24; ++i) pts.push_back({m.x + r * std::cos(i * kPi / 12), m.y + r * std::sin(i * kPi / 12)});
            path({Contour{pts, true}}, c, c, 1);
        } else if (type == "bar") {
            line({tip.x + n.x * size * 0.55, tip.y + n.y * size * 0.55}, {tip.x - n.x * size * 0.55, tip.y - n.y * size * 0.55});
        } else if (type == "cross") {
            const double r = size * 0.4;
            const Pt m{tip.x - dir.x * size * 0.6, tip.y - dir.y * size * 0.6};
            line({m.x - r * (dir.x + n.x), m.y - r * (dir.y + n.y)}, {m.x + r * (dir.x + n.x), m.y + r * (dir.y + n.y)});
            line({m.x - r * (dir.x - n.x), m.y - r * (dir.y - n.y)}, {m.x + r * (dir.x - n.x), m.y + r * (dir.y - n.y)});
        }
    }

    static double headInset(const std::string& type, double size) {
        if (type == "triangle") return size * 0.85;
        if (type == "diamond") return size * 1.35;
        if (type == "circle") return size * 0.7;
        return 0;
    }

    static std::vector<Pt> trimEnd(std::vector<Pt> pts, double d) {
        while (d > 0 && pts.size() >= 2) {
            const Pt a = pts[pts.size() - 2], b = pts.back();
            const double seg = std::hypot(b.x - a.x, b.y - a.y);
            if (seg > d) {
                pts.back() = {b.x + (a.x - b.x) * d / seg, b.y + (a.y - b.y) * d / seg};
                break;
            }
            d -= seg;
            pts.pop_back();
        }
        return pts;
    }

    void connector(const Element& e, double layerAlpha) {
        const Resolved s = resolve(board, e);
        const double alpha = s.opacity * layerAlpha;
        std::vector<Pt> pts = connectorPath(board, e);
        if (pts.size() < 2) return;
        Rng rng(hashOf(e.id));
        const Color c = withAlpha(s.stroke.value_or(Color{0, 0, 0, 255}), alpha);
        const double size = 8 + s.strokeWidth * 2;
        const double L = length(pts);
        auto dirAt = [&](const std::vector<Pt>& p, bool atEnd) {
            const double back = std::min(L * 0.5, size);
            const Pt tip = atEnd ? p.back() : p.front();
            const Pt from = pointAt(p, atEnd ? L - back : back);
            return norm({tip.x - from.x, tip.y - from.y});
        };
        const Pt endDir = dirAt(pts, true), startDir = dirAt(pts, false);
        const Pt endTip = pts.back(), startTip = pts.front();
        std::vector<Pt> line = trimEnd(pts, headInset(e.endArrow, size));
        std::reverse(line.begin(), line.end());
        line = trimEnd(line, headInset(e.startArrow, size));
        std::reverse(line.begin(), line.end());
        if (s.stroke) {
            if (s.sketch && s.roughness > 0) {
                for (auto& run : roughen(Contour{line, false}, s.roughness * 0.8, rng)) strokeOpen(run, c, s.strokeWidth * 0.9, s.dash);
            } else {
                strokeOpen(line, c, s.strokeWidth, s.dash);
            }
            if (e.endArrow != "none") arrowHead(e.endArrow, endTip, endDir, size, c, s.strokeWidth, s.sketch, rng);
            if (e.startArrow != "none") arrowHead(e.startArrow, startTip, startDir, size, c, s.strokeWidth, s.sketch, rng);
        }
        if (!e.text.empty()) {
            const Pt mid = pointAt(pts, L / 2);
            const auto lines = wrapText(e.text, s.font, s.bold, s.italic, s.fontSize, 220);
            double w = 0;
            for (const auto& l : lines) w = std::max(w, textWidth(l, s.font, s.bold, s.italic, s.fontSize));
            const double h = static_cast<double>(lines.size()) * s.fontSize * kLineHeight;
            Element box = e;
            box.x = mid.x - w / 2;
            box.y = mid.y - h / 2;
            box.w = w;
            box.h = h;
            const Color bg = boardBackground(board);
            path({Contour{{{box.x - 5, box.y - 2}, {box.x + w + 5, box.y - 2}, {box.x + w + 5, box.y + h + 2}, {box.x - 5, box.y + h + 2}}, true}},
                 withAlpha(Color{bg.r, bg.g, bg.b, 255}, alpha * 0.92), std::nullopt, 0);
            Resolved ts = s;
            ts.align = "center";
            ts.valign = "top";
            Box area{box.x, box.y, box.x + w, box.y + h, false};
            text(placeText(box, ts, area, false), ts, withAlpha(s.text, alpha));
        }
    }

    void ink(const Element& e, double layerAlpha) {
        const Resolved s = resolve(board, e);
        const Color c = withAlpha(s.stroke.value_or(s.text), s.opacity * layerAlpha);
        const auto& p = e.ink;
        if (p.empty()) return;
        const double w = e.inkWidth;
        auto half = [&](const InkPoint& q) { return std::max(0.3, w * (0.3 + 0.7 * std::clamp(q.pressure, 0.0, 1.0)) / 2); };
        auto disc = [&](Pt m, double r) {
            std::vector<Pt> pts;
            for (int i = 0; i < 16; ++i) pts.push_back({m.x + r * std::cos(i * kPi / 8), m.y + r * std::sin(i * kPi / 8)});
            return Contour{pts, true};
        };
        if (p.size() == 1) {
            path({disc({p[0].x, p[0].y}, half(p[0]))}, c, std::nullopt, 0);
            return;
        }
        // one outline: left side forward, round cap, right side back, round cap
        std::vector<Pt> left, right;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const InkPoint a = p[i > 0 ? i - 1 : 0], b = p[i + 1 < p.size() ? i + 1 : i];
            const Pt d = norm({b.x - a.x, b.y - a.y});
            const double r = half(p[i]);
            left.push_back({p[i].x - d.y * r, p[i].y + d.x * r});
            right.push_back({p[i].x + d.y * r, p[i].y - d.x * r});
        }
        std::vector<Pt> outline = left;
        auto cap = [&](const InkPoint& q, Pt d, bool end) {
            const double r = half(q);
            const double a0 = std::atan2(d.y, d.x) + (end ? kPi / 2 : -kPi / 2);
            for (int i = 1; i < 8; ++i) {
                const double a = a0 - kPi * i / 8;
                outline.push_back({q.x + r * std::cos(a), q.y + r * std::sin(a)});
            }
        };
        const Pt dEnd = norm({p.back().x - p[p.size() - 2].x, p.back().y - p[p.size() - 2].y});
        const Pt dStart = norm({p[1].x - p[0].x, p[1].y - p[0].y});
        cap(p.back(), dEnd, true);
        outline.insert(outline.end(), right.rbegin(), right.rend());
        cap(p.front(), {-dStart.x, -dStart.y}, true);
        path({Contour{outline, true}}, c, std::nullopt, 0);
        // the outline self-overlaps on tight turns; discs along the stroke fill the gaps
        std::vector<Contour> dots;
        for (std::size_t i = 0; i < p.size(); i += 3) dots.push_back(disc({p[i].x, p[i].y}, half(p[i]) * 0.98));
        for (const auto& d : dots) path({d}, c, std::nullopt, 0);
    }

    void element(const Element& e, double layerAlpha) {
        owner = e.id;
        switch (e.kind) {
            case Kind::Group: break;
            case Kind::Connector: connector(e, layerAlpha); break;
            case Kind::Ink: ink(e, layerAlpha); break;
            default: shape(e, layerAlpha); break;
        }
    }
};

std::set<std::string> selection(const Board& board, const RenderOptions& o) {
    std::set<std::string> ids;
    for (const auto& raw : o.only) {
        const Element* e = board.get(raw);
        if (!e) continue;
        ids.insert(e->id);
        for (const auto& d : board.descendants(e->id)) ids.insert(d);
    }
    if (!ids.empty()) {
        for (const auto& e : board.elements) {
            if (e.kind == Kind::Connector && ids.count(e.from) && ids.count(e.to)) ids.insert(e.id);
        }
    }
    return ids;
}

bool layerShown(const Board& board, const Element& e, const RenderOptions& o, double& alpha) {
    const Layer* l = board.layer(e.layer);
    alpha = l ? l->opacity : 1;
    if (!o.layers.empty()) return std::find(o.layers.begin(), o.layers.end(), e.layer) != o.layers.end();
    return !l || l->visible;
}

Scene buildScene(const Board& board, const RenderOptions& o) {
    Scene scene;
    scene.region = renderRegion(board, o);
    scene.background = boardBackground(board);
    scene.transparent = o.transparent;
    const auto only = selection(board, o);
    Builder b{board, scene, {}};
    std::vector<std::string> layerOrder;
    for (const auto& l : board.layers) layerOrder.push_back(l.name);
    for (const auto& e : board.elements) {
        if (std::find(layerOrder.begin(), layerOrder.end(), e.layer) == layerOrder.end()) layerOrder.push_back(e.layer);
    }
    for (const auto& ln : layerOrder) {
        for (int pass = 0; pass < 2; ++pass) {  // frames first, under everything on their layer
            for (const auto& e : board.elements) {
                if (e.layer != ln || (pass == 0) != (e.kind == Kind::Frame)) continue;
                if (!only.empty() && !only.count(e.id)) continue;
                double alpha = 1;
                if (!layerShown(board, e, o, alpha)) continue;
                b.element(e, alpha);
            }
        }
    }
    return scene;
}

// ---- PNG ----------------------------------------------------------------------------------------

struct RasterTarget {
    raster::Image& img;
    Box region;
    double scale;
    Pt map(Pt p) const { return {(p.x - region.x0) * scale, (p.y - region.y0) * scale}; }
};

void fillContours(raster::Image& img, const std::vector<std::vector<Pt>>& contours, Color color) {
    // one even-odd polygon: later contours bridged to the first and back
    std::vector<raster::Point> pts;
    const Pt* anchor = nullptr;
    for (const auto& c : contours) {
        if (c.size() < 3) continue;
        if (!anchor) {
            for (const auto& p : c) pts.push_back({p.x, p.y});
            pts.push_back({c.front().x, c.front().y});
            anchor = &c.front();
            continue;
        }
        for (const auto& p : c) pts.push_back({p.x, p.y});
        pts.push_back({c.front().x, c.front().y});
        pts.push_back({anchor->x, anchor->y});
    }
    if (pts.size() >= 3) raster::fillPolygon(img, pts, color);
}

std::vector<std::vector<Pt>> dashed(const std::vector<Pt>& pts, double on, double off) {
    std::vector<std::vector<Pt>> out;
    const double L = length(pts);
    if (L <= 0) return out;
    for (double d = 0; d < L; d += on + off) {
        std::vector<Pt> seg{pointAt(pts, d)};
        // include the corners inside the dash
        double acc = 0;
        for (std::size_t i = 1; i < pts.size(); ++i) {
            acc += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
            if (acc > d && acc < d + on) seg.push_back(pts[i]);
        }
        seg.push_back(pointAt(pts, std::min(L, d + on)));
        out.push_back(seg);
    }
    return out;
}

void dashPattern(const std::string& dash, double width, double& on, double& off) {
    if (dash == "dotted") {
        on = 0.01;
        off = width * 2 + 3;
    } else {
        on = width * 3 + 6;
        off = width * 2 + 5;
    }
}

void strokePixels(raster::Image& img, const std::vector<Pt>& pts, Color color, double width) {
    if (pts.empty()) return;
    raster::Brush brush;
    brush.color = color;
    brush.hardness = 1.0;
    brush.spacing = 0.06;
    brush.pressureSize = false;
    brush.size = std::max(1.0, width);
    if (width < 1) brush.color.a = static_cast<std::uint8_t>(color.a * std::max(0.05, width) + 0.5);
    std::vector<raster::StrokePoint> sp;
    sp.reserve(pts.size());
    for (const auto& p : pts) sp.push_back({p.x, p.y, 1.0});
    if (sp.size() == 1) sp.push_back(sp.front());
    raster::stroke(img, brush, sp);
}

void drawRaster(const Scene& scene, RasterTarget& t) {
    for (const auto& op : scene.ops) {
        if (const auto* p = std::get_if<PathOp>(&op.v)) {
            std::vector<std::vector<Pt>> mapped;
            for (const auto& c : p->contours) {
                std::vector<Pt> m;
                m.reserve(c.pts.size() + 1);
                for (const auto& q : c.pts) m.push_back(t.map(q));
                mapped.push_back(m);
            }
            if (p->fill && p->fill->a > 0) fillContours(t.img, mapped, *p->fill);
            if (p->stroke && p->stroke->a > 0 && p->width > 0) {
                const double w = p->width * t.scale;
                for (std::size_t k = 0; k < mapped.size(); ++k) {
                    std::vector<Pt> line = mapped[k];
                    if (p->contours[k].closed && !line.empty()) line.push_back(line.front());
                    if (p->dash == "dashed" || p->dash == "dotted") {
                        double on, off;
                        dashPattern(p->dash, p->width, on, off);
                        for (const auto& d : dashed(line, on * t.scale, off * t.scale)) strokePixels(t.img, d, *p->stroke, w);
                    } else {
                        strokePixels(t.img, line, *p->stroke, w);
                    }
                }
            }
        } else if (const auto* x = std::get_if<TextOp>(&op.v)) {
            const Pt at = t.map({x->x, x->y});
            pdf::drawText(t.img, at.x, at.y, x->text, pdfFont(x->font, x->bold, x->italic), x->size * t.scale, x->color);
        } else if (const auto* im = std::get_if<ImageOp>(&op.v)) {
            const Pt a = t.map({im->x, im->y});
            const int w = std::max(1, static_cast<int>(std::lround(im->w * t.scale))), h = std::max(1, static_cast<int>(std::lround(im->h * t.scale)));
            if (w > 20000 || h > 20000) continue;
            raster::Image scaled = raster::resize(*im->img, w, h);
            raster::blendOnto(t.img, scaled, static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)), raster::Blend::Normal, 1.0);
        }
    }
}

// ---- SVG ----------------------------------------------------------------------------------------

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string svgColor(Color c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string n2(double v) { return fmtNum(std::round(v * 100) / 100); }

std::string svgFont(const std::string& f) {
    if (f == "serif") return "'Times New Roman', Times, 'Liberation Serif', serif";
    if (f == "mono") return "'Courier New', Courier, 'Liberation Mono', monospace";
    return "Helvetica, Arial, 'Liberation Sans', sans-serif";
}

std::string mimeOf(const std::string& path) {
    const std::string l = lower(fs::path(path).extension().string());
    if (l == ".jpg" || l == ".jpeg") return "image/jpeg";
    if (l == ".gif") return "image/gif";
    if (l == ".png") return "image/png";
    return "";
}

std::string dataUri(const ImageOp& im) {
    if (im.src.rfind("data:", 0) == 0) return im.src;
    const std::string mime = mimeOf(im.src);
    if (!mime.empty()) {
        std::ifstream in(im.src, std::ios::binary);
        std::stringstream s;
        s << in.rdbuf();
        if (in || in.eof()) return "data:" + mime + ";base64," + base64Encode(s.str());
    }
    return "data:image/png;base64," + base64Encode(raster::encodePng(*im.img));
}

}  // namespace

Box renderRegion(const Board& board, const RenderOptions& o) {
    if (o.region && !o.region->empty && o.region->w() > 0 && o.region->h() > 0) return *o.region;
    Box b;
    const auto only = selection(board, o);
    if (!only.empty()) {
        for (const auto& id : only) {
            if (const Element* e = board.get(id)) b.add(elementBox(board, *e));
        }
    } else if (!o.layers.empty()) {
        for (const auto& e : board.elements) {
            if (std::find(o.layers.begin(), o.layers.end(), e.layer) != o.layers.end()) b.add(elementBox(board, e));
        }
    } else {
        b = contentBounds(board);
    }
    if (b.empty) return Box{0, 0, 400, 300, false};
    return b.grown(std::max(0.0, o.padding));
}

raster::Image renderImage(const Board& board, const RenderOptions& options) {
    RenderOptions o = options;
    if (!(o.scale > 0)) o.scale = 1;
    const Scene scene = buildScene(board, o);
    double scale = o.scale;
    const double maxSide = 12000;
    if (scene.region.w() * scale > maxSide || scene.region.h() * scale > maxSide) scale = maxSide / std::max(scene.region.w(), scene.region.h());
    const int w = std::max(1, static_cast<int>(std::ceil(scene.region.w() * scale)));
    const int h = std::max(1, static_cast<int>(std::ceil(scene.region.h() * scale)));
    raster::Image img(w, h, o.transparent ? Color{scene.background.r, scene.background.g, scene.background.b, 0} : scene.background);
    RasterTarget t{img, scene.region, scale};
    drawRaster(scene, t);
    return img;
}

std::string toSvg(const Board& board, const RenderOptions& options) {
    RenderOptions o = options;
    if (!(o.scale > 0)) o.scale = 1;
    const Scene scene = buildScene(board, o);
    const Box& r = scene.region;
    std::ostringstream s;
    s << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s << "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" << n2(r.w() * o.scale) << "\" height=\""
      << n2(r.h() * o.scale) << "\" viewBox=\"" << n2(r.x0) << " " << n2(r.y0) << " " << n2(r.w()) << " " << n2(r.h()) << "\">\n";
    if (!board.title.empty()) s << "<title>" << esc(board.title) << "</title>\n";
    if (!o.transparent && scene.background.a > 0)
        s << "<rect x=\"" << n2(r.x0) << "\" y=\"" << n2(r.y0) << "\" width=\"" << n2(r.w()) << "\" height=\"" << n2(r.h()) << "\" fill=\""
          << svgColor(scene.background) << "\"/>\n";
    std::string group;
    for (const auto& op : scene.ops) {
        if (op.owner != group) {
            if (!group.empty()) s << "</g>\n";
            group = op.owner;
            if (!group.empty()) s << "<g id=\"" << esc(group) << "\">\n";
        }
        if (const auto* p = std::get_if<PathOp>(&op.v)) {
            std::string d;
            for (const auto& c : p->contours) {
                for (std::size_t i = 0; i < c.pts.size(); ++i) d += (i ? "L" : "M") + n2(c.pts[i].x) + " " + n2(c.pts[i].y);
                if (c.closed) d += "Z";
            }
            if (d.empty()) continue;
            s << "<path d=\"" << d << "\"";
            if (p->fill) {
                s << " fill=\"" << svgColor(*p->fill) << "\"";
                if (p->fill->a < 255) s << " fill-opacity=\"" << n2(p->fill->a / 255.0) << "\"";
                if (p->contours.size() > 1) s << " fill-rule=\"evenodd\"";
            } else {
                s << " fill=\"none\"";
            }
            if (p->stroke && p->width > 0) {
                s << " stroke=\"" << svgColor(*p->stroke) << "\" stroke-width=\"" << n2(p->width) << "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"";
                if (p->stroke->a < 255) s << " stroke-opacity=\"" << n2(p->stroke->a / 255.0) << "\"";
                if (p->dash == "dashed" || p->dash == "dotted") {
                    double on, off;
                    dashPattern(p->dash, p->width, on, off);
                    s << " stroke-dasharray=\"" << n2(std::max(on, 0.01)) << " " << n2(off) << "\"";
                }
            }
            s << "/>\n";
        } else if (const auto* x = std::get_if<TextOp>(&op.v)) {
            s << "<text x=\"" << n2(x->x) << "\" y=\"" << n2(x->y) << "\" font-family=\"" << svgFont(x->font) << "\" font-size=\"" << n2(x->size) << "\"";
            if (x->bold) s << " font-weight=\"bold\"";
            if (x->italic) s << " font-style=\"italic\"";
            s << " fill=\"" << svgColor(x->color) << "\"";
            if (x->color.a < 255) s << " fill-opacity=\"" << n2(x->color.a / 255.0) << "\"";
            s << " xml:space=\"preserve\">" << esc(x->text) << "</text>\n";
        } else if (const auto* im = std::get_if<ImageOp>(&op.v)) {
            s << "<image x=\"" << n2(im->x) << "\" y=\"" << n2(im->y) << "\" width=\"" << n2(im->w) << "\" height=\"" << n2(im->h)
              << "\" preserveAspectRatio=\"none\" xlink:href=\"" << dataUri(*im) << "\"/>\n";
        }
    }
    if (!group.empty()) s << "</g>\n";
    s << "</svg>\n";
    return s.str();
}

// ---- PDF ----------------------------------------------------------------------------------------

std::string toPdf(const Board& board, const RenderOptions& options) {
    RenderOptions o = options;
    if (!(o.scale > 0)) o.scale = 1;
    const Scene scene = buildScene(board, o);
    const Box& r = scene.region;
    const double k = o.scale;
    const double W = r.w() * k, H = r.h() * k;
    pdfdoc::Writer w;
    const int catalog = w.reserve(), pages = w.reserve(), page = w.reserve();
    std::map<int, int> fonts;           // standard font -> object
    std::map<int, std::string> alphas;  // alpha*100 -> gs name
    std::vector<std::pair<std::string, int>> images;
    auto num = [](double v) { return pdfdoc::num(v); };
    auto rgb = [&](Color c) { return num(c.r / 255.0) + " " + num(c.g / 255.0) + " " + num(c.b / 255.0); };
    auto gs = [&](double a) {
        const int key = static_cast<int>(std::lround(a * 100));
        auto it = alphas.find(key);
        if (it == alphas.end()) it = alphas.emplace(key, "GA" + std::to_string(key)).first;
        return "/" + it->second + " gs ";
    };
    std::ostringstream c;
    c << "1 0 0 -1 0 " << num(H) << " cm " << num(k) << " 0 0 " << num(k) << " " << num(-r.x0 * k) << " " << num(-r.y0 * k) << " cm\n";
    c << "1 J 1 j\n";
    if (!o.transparent && scene.background.a > 0)
        c << rgb(scene.background) << " rg " << num(r.x0) << " " << num(r.y0) << " " << num(r.w()) << " " << num(r.h()) << " re f\n";
    for (const auto& op : scene.ops) {
        if (const auto* p = std::get_if<PathOp>(&op.v)) {
            std::string d;
            for (const auto& ct : p->contours) {
                for (std::size_t i = 0; i < ct.pts.size(); ++i) d += num(ct.pts[i].x) + " " + num(ct.pts[i].y) + (i ? " l " : " m ");
                if (ct.closed && !ct.pts.empty()) d += "h ";
            }
            if (d.empty()) continue;
            c << "q ";
            const bool fill = p->fill && p->fill->a > 0, stroke = p->stroke && p->stroke->a > 0 && p->width > 0;
            if (fill) {
                if (p->fill->a < 255) c << gs(p->fill->a / 255.0);
                c << rgb(*p->fill) << " rg ";
                c << d << (p->contours.size() > 1 ? "f*" : "f") << "\n";
            }
            if (stroke) {
                if (p->stroke->a < 255) c << gs(p->stroke->a / 255.0);
                c << rgb(*p->stroke) << " RG " << num(p->width) << " w ";
                if (p->dash == "dashed" || p->dash == "dotted") {
                    double on, off;
                    dashPattern(p->dash, p->width, on, off);
                    c << "[" << num(std::max(on, 0.01)) << " " << num(off) << "] 0 d ";
                }
                c << d << "S\n";
            }
            c << "Q\n";
        } else if (const auto* x = std::get_if<TextOp>(&op.v)) {
            const std::string fam = x->font == "serif" ? "times" : (x->font == "mono" ? "courier" : "helvetica");
            const pdfdoc::Standard sf = pdfdoc::standardFont(fam, x->bold, x->italic);
            const int fi = static_cast<int>(sf);
            if (!fonts.count(fi)) fonts[fi] = pdfdoc::addStandardFont(w, sf);
            c << "q ";
            if (x->color.a < 255) c << gs(x->color.a / 255.0);
            c << rgb(x->color) << " rg BT /F" << fi << " " << num(x->size) << " Tf 1 0 0 -1 " << num(x->x) << " " << num(x->y) << " Tm "
              << pdfdoc::text(x->text) << " Tj ET Q\n";
        } else if (const auto* im = std::get_if<ImageOp>(&op.v)) {
            const int id = pdfdoc::addImage(w, *im->img);
            const std::string name = "Im" + std::to_string(images.size() + 1);
            images.push_back({name, id});
            c << "q " << num(im->w) << " 0 0 " << num(-im->h) << " " << num(im->x) << " " << num(im->y + im->h) << " cm /" << name << " Do Q\n";
        }
    }
    const int content = w.addStream("", c.str());
    std::string res = "<< /Font <<";
    for (const auto& [fi, id] : fonts) res += " /F" + std::to_string(fi) + " " + std::to_string(id) + " 0 R";
    res += " >> /ExtGState <<";
    for (const auto& [key, name] : alphas) res += " /" + name + " << /ca " + num(key / 100.0) + " /CA " + num(key / 100.0) + " >>";
    res += " >> /XObject <<";
    for (const auto& [name, id] : images) res += " /" + name + " " + std::to_string(id) + " 0 R";
    res += " >> >>";
    w.set(page, "<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 " + num(W) + " " + num(H) + "] /Resources " + res +
                    " /Contents " + std::to_string(content) + " 0 R >>");
    w.set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    w.set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    int info = 0;
    if (!board.title.empty()) info = w.add("<< /Title " + pdfdoc::text(board.title) + " /Producer (ACHCLWSS board) >>");
    return w.finish(catalog, info);
}

}  // namespace achclwss::board
