// Turning a drawing into a sheet: every entity becomes paper-space strokes,
// fills and text (mm, y up from the paper's bottom-left corner), which the
// SVG, PDF and raster writers then draw. Arcs stay exact cubic Béziers until
// a raster needs pixels.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "achclwss/blueprint/blueprint.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/paint.hpp"
#include "internal.hpp"

namespace achclwss::blueprint {

using detail::num;

namespace {

// ---- paper-space primitives -----------------------------------------------

struct Seg {
    bool curve = false;
    Vec2 c1, c2, p;
};

struct Sub {
    Vec2 start;
    std::vector<Seg> segs;
    bool closed = false;
};

struct Prim {
    enum Kind { Stroke, Fill, Text } kind = Stroke;
    std::vector<Sub> path;
    Color color{0, 0, 0, 255};
    double width = 0.25;  // mm
    std::vector<double> dash;
    Vec2 pos;
    double height = 0, rotation = 0;
    HAlign halign = HAlign::Left;
    VAlign valign = VAlign::Baseline;
    std::string text;
    std::string layer;
};

Sub polySub(const std::vector<Vec2>& pts, bool closed) {
    Sub s;
    if (pts.empty()) return s;
    s.start = pts[0];
    for (std::size_t i = 1; i < pts.size(); ++i) s.segs.push_back({false, {}, {}, pts[i]});
    s.closed = closed;
    return s;
}

// Counter-clockwise arc from a0 to a1 as cubic Béziers (at most 90 degrees each).
Sub arcSub(Vec2 c, double r, double a0, double a1, bool full) {
    double sweep = full ? 360.0 : normalizeAngle(a1 - a0);
    if (!full && sweep <= 1e-9) sweep = 360.0;
    const int n = std::max(1, static_cast<int>(std::ceil(sweep / 90.0 - 1e-9)));
    const double step = sweep / n;
    const double kappa = 4.0 / 3.0 * std::tan(radians(step) / 4);
    Sub s;
    s.start = c + direction(a0) * r;
    for (int i = 0; i < n; ++i) {
        double t0 = a0 + i * step, t1 = t0 + step;
        Vec2 p0 = c + direction(t0) * r, p3 = c + direction(t1) * r;
        s.segs.push_back({true, p0 + perp(direction(t0)) * (r * kappa), p3 - perp(direction(t1)) * (r * kappa), p3});
    }
    s.closed = full;
    return s;
}

// Parallel hatch lines across a polygon (paper mm), anchored on the paper origin so neighbours line up.
std::vector<Sub> hatchLines(const std::vector<Vec2>& poly, double angle, double spacing) {
    std::vector<Sub> out;
    if (poly.size() < 3 || !(spacing > 0)) return out;
    Vec2 u = direction(angle), n = perp(u);
    double smin = dot(poly[0], n), smax = smin;
    for (auto p : poly) {
        smin = std::min(smin, dot(p, n));
        smax = std::max(smax, dot(p, n));
    }
    const double first = std::ceil(smin / spacing), last = std::floor(smax / spacing);
    if (last - first > 5000) return out;
    std::vector<double> ts;
    for (double k = first; k <= last; k += 1) {
        const double s = k * spacing;
        ts.clear();
        for (std::size_t i = 0; i < poly.size(); ++i) {
            Vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
            double da = dot(a, n) - s, db = dot(b, n) - s;
            if ((da < 0) == (db < 0)) continue;
            Vec2 p = a + (b - a) * (da / (da - db));
            ts.push_back(dot(p, u));
        }
        std::sort(ts.begin(), ts.end());
        for (std::size_t i = 0; i + 1 < ts.size(); i += 2) {
            if (ts[i + 1] - ts[i] < 1e-9) continue;
            out.push_back(polySub({n * s + u * ts[i], n * s + u * ts[i + 1]}, false));
        }
    }
    return out;
}

struct Pen {
    Color color;
    double width;
    std::vector<double> dash;
    std::string layer;
};

// One simple entity, mapped to paper by `map` (with `k` paper mm per unit), as primitives.
void entityPrims(std::vector<Prim>& out, const Entity& e, const Pen& pen, const std::function<Vec2(Vec2)>& map, double k) {
    Prim p;
    p.color = e.color.value_or(pen.color);
    p.width = pen.width;
    p.dash = pen.dash;
    p.layer = pen.layer;
    auto mapped = [&](const std::vector<Vec2>& pts) {
        std::vector<Vec2> m;
        m.reserve(pts.size());
        for (auto q : pts) m.push_back(map(q));
        return m;
    };
    switch (e.type) {
        case EntityType::Line:
        case EntityType::Polyline:
            if (e.pts.size() < 2) return;
            p.path.push_back(polySub(mapped(e.pts), e.closed && e.pts.size() > 2));
            break;
        case EntityType::Circle:
            if (!(e.r > 0)) return;
            p.path.push_back(arcSub(map(e.c), e.r * k, 0, 360, true));
            break;
        case EntityType::Arc:
            if (!(e.r > 0)) return;
            p.path.push_back(arcSub(map(e.c), e.r * k, e.a0, e.a1, false));
            break;
        case EntityType::Point: {
            Vec2 c = map(e.c);
            p.path.push_back(polySub({c - Vec2{0.8, 0}, c + Vec2{0.8, 0}}, false));
            p.path.push_back(polySub({c - Vec2{0, 0.8}, c + Vec2{0, 0.8}}, false));
            p.dash.clear();
            break;
        }
        case EntityType::Text:
            if (e.text.empty() || !(e.height > 0)) return;
            p.kind = Prim::Text;
            p.pos = map(e.c);
            p.height = e.height * k;
            p.rotation = e.rotation;
            p.halign = e.halign;
            p.valign = e.valign;
            p.text = e.text;
            break;
        case EntityType::Hatch: {
            if (e.pts.size() < 3) return;
            std::vector<Vec2> poly = mapped(e.pts);
            if (e.pattern == "lines" || e.pattern == "cross") {
                p.width = 0.13;
                p.dash.clear();
                p.path = hatchLines(poly, e.angle, e.spacing);
                if (e.pattern == "cross") {
                    auto more = hatchLines(poly, e.angle + 90, e.spacing);
                    p.path.insert(p.path.end(), more.begin(), more.end());
                }
                if (p.path.empty()) return;
            } else {
                p.kind = Prim::Fill;
                p.path.push_back(polySub(poly, true));
            }
            break;
        }
        default: return;
    }
    out.push_back(std::move(p));
}

std::vector<Prim> sheetPrims(const Drawing& doc, bool withGrid) {
    std::vector<Prim> out;
    const double k = doc.k();
    auto map = [&](Vec2 q) { return doc.toPaper(q); };

    if (withGrid && doc.grid.show && doc.grid.spacing > 0) {
        Vec2 lo = doc.fromPaper({0, 0}), hi = doc.fromPaper({doc.paperWidth(), doc.paperHeight()});
        const double g = doc.grid.spacing;
        const double nx = (hi.x - lo.x) / g, ny = (hi.y - lo.y) / g;
        if (nx < 600 && ny < 600) {
            Prim p;
            p.color = {205, 215, 230, 255};
            p.width = 0.1;
            p.layer = "grid";
            for (double x = std::ceil(lo.x / g) * g; x <= hi.x; x += g) p.path.push_back(polySub({map({x, lo.y}), map({x, hi.y})}, false));
            for (double y = std::ceil(lo.y / g) * g; y <= hi.y; y += g) p.path.push_back(polySub({map({lo.x, y}), map({hi.x, y})}, false));
            out.push_back(std::move(p));
        }
    }

    for (const auto& ent : doc.entities) {
        const Layer* l = doc.layer(ent.layer);
        Layer fallback{ent.layer};
        if (!l) l = &fallback;
        if (!l->visible) continue;
        for (const auto& piece : explode(doc, ent)) {
            const Layer* pl = piece.e.layer == ent.layer ? l : doc.layer(piece.e.layer);
            if (!pl) pl = l;
            Pen pen{pl->color, piece.thin ? std::min(0.18, pl->weight) : pl->weight,
                    piece.thin ? std::vector<double>{} : dashPattern(pl->linetype), pl->name};
            entityPrims(out, piece.e, pen, map, k);
        }
    }

    for (const auto& item : detail::sheetItems(doc)) {
        Pen pen{{0, 0, 0, 255}, item.weight, {}, "sheet"};
        entityPrims(out, item.e, pen, [](Vec2 q) { return q; }, 1.0);
    }
    return out;
}

// Baseline start of a text primitive and its direction vectors.
void textFrame(const Prim& t, Vec2& origin, Vec2& ux, Vec2& uy, double& width) {
    ux = direction(t.rotation);
    uy = perp(ux);
    width = textWidth(t.text) * t.height;
    const double dx = t.halign == HAlign::Left ? 0 : t.halign == HAlign::Center ? -width / 2 : -width;
    const double dy = t.valign == VAlign::Baseline ? 0 : t.valign == VAlign::Middle ? -t.height / 2 : -t.height;
    origin = t.pos + ux * dx + uy * dy;
}

// ---- stroke font for rasters ------------------------------------------------
// Glyphs on a 4 x 6 grid (x, y digits; y up), strokes separated by spaces.

const std::map<unsigned, const char*>& strokeFont() {
    static const std::map<unsigned, const char*> font = {
        {'0', "10 30 41 45 36 16 05 01 10"},
        {'1', "15 26 20|10 30"},
        {'2', "05 16 36 45 44 00 40"},
        {'3', "05 16 36 45 44 33 13|33 42 41 30 10 01"},
        {'4', "30 36 02 42"},
        {'5', "46 06 04 34 43 41 30 10 01"},
        {'6', "45 36 16 05 01 10 30 41 42 33 13 02"},
        {'7', "06 46 10"},
        {'8', "13 04 05 16 36 45 44 33 13 02 01 10 30 41 42 33"},
        {'9', "01 10 30 41 45 36 16 05 04 13 33 44"},
        {'A', "00 26 40|13 33"},
        {'B', "00 06 36 45 44 33 03|33 42 41 30 00"},
        {'C', "45 36 16 05 01 10 30 41"},
        {'D', "00 06 26 44 42 20 00"},
        {'E', "40 00 06 46|03 33"},
        {'F', "00 06 46|03 33"},
        {'G', "45 36 16 05 01 10 30 41 43 23"},
        {'H', "00 06|40 46|03 43"},
        {'I', "10 30|16 36|20 26"},
        {'J', "26 46 41 30 10 01"},
        {'K', "00 06|46 02|13 40"},
        {'L', "06 00 40"},
        {'M', "00 06 23 46 40"},
        {'N', "00 06 40 46"},
        {'O', "10 30 41 45 36 16 05 01 10"},
        {'P', "00 06 36 45 44 33 03"},
        {'Q', "10 30 41 45 36 16 05 01 10|22 40"},
        {'R', "00 06 36 45 44 33 03|23 40"},
        {'S', "45 36 16 05 04 13 33 42 41 30 10 01"},
        {'T', "06 46|26 20"},
        {'U', "06 01 10 30 41 46"},
        {'V', "06 20 46"},
        {'W', "06 10 23 30 46"},
        {'X', "06 40|00 46"},
        {'Y', "06 23 46|23 20"},
        {'Z', "06 46 00 40"},
        {'.', "20 21"},
        {',', "21 10"},
        {':', "21 22|24 25"},
        {';', "24 25|22 10"},
        {'-', "13 33"},
        {'+', "13 33|22 24"},
        {'=', "12 32|14 34"},
        {'/', "00 46"},
        {'\\', "06 40"},
        {'(', "36 24 22 30"},
        {')', "16 24 22 10"},
        {'[', "36 26 20 30"},
        {']', "16 26 20 10"},
        {'\'', "26 24"},
        {'"', "16 14|36 34"},
        {'_', "00 40"},
        {'#', "10 16|30 36|02 42|04 44"},
        {'%', "05 06 16 15 05|00 46|30 31 41 40 30"},
        {'*', "11 35|15 31|13 33"},
        {'<', "45 03 41"},
        {'>', "05 43 01"},
        {'?', "05 16 36 45 44 23 22|20 21"},
        {'!', "26 22|20 21"},
        {'|', "20 26"},
        {'&', "40 05 16 26 35 01 10 20 42"},
        {0xB0, "15 26 35 24 15"},
        {0xB2, "15 26 36 35 13 33"},
        {0xB1, "14 34|23 25|11 31"},
        {0xD7, "11 35|15 31"},
        {0xD8, "10 30 41 45 36 16 05 01 10|00 46"},
    };
    return font;
}

// Strokes of `text` in paper mm, laid out with Helvetica advances so it lines up with SVG/PDF text.
std::vector<std::vector<Vec2>> textStrokes(const Prim& t) {
    std::vector<std::vector<Vec2>> out;
    Vec2 origin, ux, uy;
    double width = 0;
    textFrame(t, origin, ux, uy, width);
    const double unit = t.height / 6;
    double x = 0;
    for (unsigned cp : detail::utf8Decode(t.text)) {
        const double adv = detail::glyphWidth(cp) / 1000.0 / 0.718 * t.height;
        unsigned key = cp;
        double sy = 1;
        if (cp >= 'a' && cp <= 'z') {
            key = cp - 32;
            sy = 0.75;
        }
        if (key == 0xF8) key = 0xD8;
        const double fx = std::clamp(adv / (5 * unit), 0.45, 1.2);
        const double left = x + (adv - 4 * unit * fx) / 2;
        auto it = strokeFont().find(key);
        if (it != strokeFont().end() || cp != ' ') {
            const char* def = it != strokeFont().end() ? it->second : "00 40 46 06 00";  // unknown: a box
            std::vector<Vec2> pts;
            for (const char* q = def; *q; ++q) {
                if (*q == '|') {
                    if (pts.size() > 1) out.push_back(pts);
                    pts.clear();
                } else if (std::isdigit(static_cast<unsigned char>(q[0])) && std::isdigit(static_cast<unsigned char>(q[1]))) {
                    const double gx = q[0] - '0', gy = q[1] - '0';
                    pts.push_back(origin + ux * (left + gx * unit * fx) + uy * (gy * unit * sy));
                    ++q;
                }
            }
            if (pts.size() > 1) out.push_back(pts);
        }
        x += adv;
    }
    return out;
}

// ---- raster ---------------------------------------------------------------

struct Raster {
    raster::Image& img;
    View view;
    double s;  // px per mm
    Vec2 px(Vec2 p) const { return {(p.x - view.x0) * s, (view.y1 - p.y) * s}; }
};

std::vector<Vec2> flatten(const Sub& sub, double s) {
    std::vector<Vec2> pts{sub.start};
    Vec2 cur = sub.start;
    for (const auto& seg : sub.segs) {
        if (!seg.curve) {
            pts.push_back(seg.p);
        } else {
            double len = (distance(cur, seg.c1) + distance(seg.c1, seg.c2) + distance(seg.c2, seg.p)) * s;
            int n = std::clamp(static_cast<int>(len / 3), 4, 256);
            for (int i = 1; i <= n; ++i) {
                double t = static_cast<double>(i) / n, mt = 1 - t;
                pts.push_back(cur * (mt * mt * mt) + seg.c1 * (3 * mt * mt * t) + seg.c2 * (3 * mt * t * t) + seg.p * (t * t * t));
            }
        }
        cur = seg.p;
    }
    if (sub.closed) pts.push_back(sub.start);
    return pts;
}

// Splits a polyline into dashes (pattern in the same units as the points).
std::vector<std::vector<Vec2>> dashes(const std::vector<Vec2>& pts, const std::vector<double>& pattern) {
    if (pattern.empty()) return {pts};
    double total = 0;
    for (double d : pattern) total += d;
    if (!(total > 0)) return {pts};
    std::vector<std::vector<Vec2>> out;
    std::size_t idx = 0;
    double left = pattern[0];
    bool on = true;
    std::vector<Vec2> cur{pts.empty() ? Vec2{} : pts[0]};
    for (std::size_t i = 1; i < pts.size(); ++i) {
        Vec2 a = pts[i - 1], b = pts[i];
        double len = distance(a, b), pos = 0;
        while (len - pos > left) {
            pos += left;
            Vec2 q = a + (b - a) * (pos / len);
            if (on) {
                cur.push_back(q);
                out.push_back(cur);
            }
            cur = {q};
            on = !on;
            idx = (idx + 1) % pattern.size();
            left = pattern[idx];
        }
        left -= len - pos;
        if (on) cur.push_back(b);
        else cur = {b};
    }
    if (on && cur.size() > 1) out.push_back(cur);
    return out;
}

void segmentPx(raster::Image& img, Vec2 a, Vec2 b, double w, Color c) {
    if (w < 1) {
        c.a = static_cast<std::uint8_t>(std::lround(c.a * std::max(0.15, w)));
        w = 1;
    }
    Vec2 d = b - a;
    const double len = length(d);
    Vec2 u = len > 1e-9 ? d / len : Vec2{1, 0};
    Vec2 n = perp(u) * (w / 2);
    Vec2 e = u * (w / 2);  // square caps so joints meet
    raster::fillPolygon(img, {{a.x - e.x + n.x, a.y - e.y + n.y}, {b.x + e.x + n.x, b.y + e.y + n.y},
                              {b.x + e.x - n.x, b.y + e.y - n.y}, {a.x - e.x - n.x, a.y - e.y - n.y}},
                        c);
}

void drawPrim(const Raster& R, const Prim& p) {
    auto toPx = [&](const std::vector<Vec2>& pts) {
        std::vector<Vec2> o;
        o.reserve(pts.size());
        for (auto q : pts) o.push_back(R.px(q));
        return o;
    };
    if (p.kind == Prim::Text) {
        const double w = std::max(0.6, p.height * R.s * 0.09);
        for (const auto& stroke : textStrokes(p)) {
            auto pts = toPx(stroke);
            for (std::size_t i = 1; i < pts.size(); ++i) segmentPx(R.img, pts[i - 1], pts[i], w, p.color);
        }
        return;
    }
    if (p.kind == Prim::Fill) {
        std::vector<raster::Point> all;
        const raster::Point* anchor = nullptr;
        raster::Point first{};
        for (const auto& sub : p.path) {
            auto pts = toPx(flatten(sub, R.s));
            if (pts.size() < 3) continue;
            if (!anchor) {
                for (auto q : pts) all.push_back({q.x, q.y});
                first = {pts[0].x, pts[0].y};
                anchor = &first;
                continue;
            }
            for (auto q : pts) all.push_back({q.x, q.y});
            all.push_back({pts[0].x, pts[0].y});
            all.push_back(*anchor);
        }
        if (all.size() >= 3) raster::fillPolygon(R.img, all, p.color);
        return;
    }
    std::vector<double> pattern;
    for (double d : p.dash) pattern.push_back(d * R.s);
    for (const auto& sub : p.path) {
        for (const auto& piece : dashes(toPx(flatten(sub, R.s)), pattern)) {
            for (std::size_t i = 1; i < piece.size(); ++i) segmentPx(R.img, piece[i - 1], piece[i], p.width * R.s, p.color);
        }
    }
}

// ---- SVG ------------------------------------------------------------------

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += ch;
        }
    }
    return out;
}

std::string hex6(Color c) { return c.hex().substr(0, 7); }

std::string svgPath(const std::vector<Sub>& path, double H) {
    std::string d;
    auto pt = [&](Vec2 p) { return num(p.x) + " " + num(H - p.y); };
    for (const auto& sub : path) {
        d += (d.empty() ? "M" : " M") + pt(sub.start);
        for (const auto& seg : sub.segs) {
            if (seg.curve) d += " C" + pt(seg.c1) + " " + pt(seg.c2) + " " + pt(seg.p);
            else d += " L" + pt(seg.p);
        }
        if (sub.closed) d += " Z";
    }
    return d;
}

// ---- PDF ------------------------------------------------------------------

std::string pdfNum(double v) { return num(v, 3); }

// WinAnsi bytes for a PDF string literal, escaped.
std::string pdfString(const std::string& text) {
    std::string out = "(";
    for (unsigned cp : detail::utf8Decode(text)) {
        unsigned b = cp;
        if (cp > 0xFF) b = '?';
        if (b == '(' || b == ')' || b == '\\') {
            out += '\\';
            out += static_cast<char>(b);
        } else if (b < 32 || b > 126) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", b);
            out += buf;
        } else {
            out += static_cast<char>(b);
        }
    }
    return out + ")";
}

std::string pdfPath(const std::vector<Sub>& path) {
    std::string d;
    auto pt = [&](Vec2 p) { return pdfNum(p.x) + " " + pdfNum(p.y); };
    for (const auto& sub : path) {
        d += pt(sub.start) + " m\n";
        for (const auto& seg : sub.segs) {
            if (seg.curve) d += pt(seg.c1) + " " + pt(seg.c2) + " " + pt(seg.p) + " c\n";
            else d += pt(seg.p) + " l\n";
        }
        if (sub.closed) d += "h\n";
    }
    return d;
}

std::string rgb(Color c) {
    return pdfNum(c.r / 255.0) + " " + pdfNum(c.g / 255.0) + " " + pdfNum(c.b / 255.0);
}

std::string ext(const std::filesystem::path& path) {
    std::string e = path.extension().string();
    for (auto& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return e;
}

}  // namespace

// ---- sheet frame and title block --------------------------------------------

namespace detail {

std::vector<SheetItem> sheetItems(const Drawing& doc) {
    std::vector<SheetItem> out;
    if (!doc.title.show) return out;
    const double W = doc.paperWidth(), H = doc.paperHeight(), m = 10;
    auto line = [&](Vec2 a, Vec2 b, double w) {
        Entity e;
        e.type = EntityType::Line;
        e.pts = {a, b};
        out.push_back({e, w});
    };
    auto text = [&](Vec2 at, const std::string& s, double h, double maxWidth) {
        if (s.empty()) return;
        Entity e;
        e.type = EntityType::Text;
        e.c = at;
        e.text = s;
        e.height = std::min(h, maxWidth / std::max(1e-9, textWidth(s)));
        out.push_back({e, 0.25});
    };
    Entity frame;
    frame.type = EntityType::Polyline;
    frame.closed = true;
    frame.pts = {{m, m}, {W - m, m}, {W - m, H - m}, {m, H - m}};
    out.push_back({frame, 0.5});

    const double tw = std::min(180.0, W - 2 * m), x1 = W - m, x0 = x1 - tw, y0 = m, ymid = m + 15, y1 = m + 32;
    Entity box;
    box.type = EntityType::Polyline;
    box.closed = true;
    box.pts = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    out.push_back({box, 0.35});
    line({x0, ymid}, {x1, ymid}, 0.25);

    text({x0 + 2, y1 - 4}, "TITLE", 1.8, tw);
    text({x0 + 2, ymid + 4}, doc.title.title, 5, tw - 4);

    const double widths[4] = {0.4, 0.25, 0.175, 0.175};
    const char* labels[4] = {"DRAWN BY", "DATE", "SCALE", "SHEET"};
    const std::string values[4] = {doc.title.author, doc.title.date, doc.title.scale.empty() ? doc.scaleText() : doc.title.scale,
                                   doc.title.sheet};
    double cx = x0;
    for (int i = 0; i < 4; ++i) {
        const double cw = tw * widths[i];
        if (i > 0) line({cx, y0}, {cx, ymid}, 0.25);
        text({cx + 2, ymid - 4}, labels[i], 1.8, cw - 4);
        text({cx + 2, y0 + 3.5}, values[static_cast<std::size_t>(i)], 3.5, cw - 4);
        cx += cw;
    }
    return out;
}

}  // namespace detail

// ---- views and rendering ----------------------------------------------------

View sheetView(const Drawing& doc) { return {0, 0, doc.paperWidth(), doc.paperHeight()}; }

View extentsView(const Drawing& doc) {
    Box b = extents(doc);
    if (b.empty) return sheetView(doc);
    Vec2 lo = doc.toPaper({b.x0, b.y0}), hi = doc.toPaper({b.x1, b.y1});
    double w = hi.x - lo.x, h = hi.y - lo.y;
    double pad = std::max(5.0, 0.06 * std::max(w, h));
    View v{lo.x - pad, lo.y - pad, hi.x + pad, hi.y + pad};
    // at least 20 mm across so a single point still shows something sensible
    for (auto [a, z] : {std::pair<double*, double*>{&v.x0, &v.x1}, std::pair<double*, double*>{&v.y0, &v.y1}}) {
        if (*z - *a < 20) {
            double c = (*a + *z) / 2;
            *a = c - 10;
            *z = c + 10;
        }
    }
    return v;
}

raster::Image render(const Drawing& doc, const View& view, int size, double dpi, bool grid) {
    const double vw = std::max(1e-6, view.x1 - view.x0), vh = std::max(1e-6, view.y1 - view.y0);
    double s = size > 0 ? size / std::max(vw, vh) : (dpi > 0 ? dpi : 150) / 25.4;
    const int w = std::clamp(static_cast<int>(std::lround(vw * s)), 1, 20000);
    const int h = std::clamp(static_cast<int>(std::lround(vh * s)), 1, 20000);
    raster::Image img(w, h, Color{255, 255, 255, 255});
    Raster R{img, view, s};
    for (const auto& p : sheetPrims(doc, grid)) drawPrim(R, p);
    return img;
}

std::unique_ptr<Attachment> Drawing::preview() const {
    raster::Image img = render(*this, sheetView(*this), 1024, 0, grid.show);
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(img), "preview.png"});
}

std::string toSvg(const Drawing& doc) {
    const double W = doc.paperWidth(), H = doc.paperHeight();
    std::vector<Prim> prims = sheetPrims(doc, false);
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" width=\""
      << num(W) << "mm\" height=\"" << num(H) << "mm\" viewBox=\"0 0 " << num(W) << " " << num(H) << "\">\n";
    o << "<title>" << xmlEscape(doc.title.title.empty() ? "blueprint" : doc.title.title) << "</title>\n";
    o << "<desc>" << xmlEscape(doc.paper + (doc.landscape ? " landscape" : " portrait") + ", scale " + doc.scaleText() +
                               ", drawing units " + toString(doc.units))
      << "</desc>\n";
    o << "<rect x=\"0\" y=\"0\" width=\"" << num(W) << "\" height=\"" << num(H) << "\" fill=\"#ffffff\"/>\n";

    // one group per layer (in layer order), then the sheet
    std::vector<std::string> order;
    for (const auto& l : doc.layers) order.push_back(l.name);
    for (const auto& p : prims) {
        if (std::find(order.begin(), order.end(), p.layer) == order.end()) order.push_back(p.layer);
    }
    int gid = 0;
    for (const auto& name : order) {
        bool any = false;
        for (const auto& p : prims) any = any || p.layer == name;
        if (!any) continue;
        o << "<g id=\"layer" << ++gid << "\" inkscape:groupmode=\"layer\" inkscape:label=\"" << xmlEscape(name) << "\">\n";
        for (const auto& p : prims) {
            if (p.layer != name) continue;
            if (p.kind == Prim::Text) {
                Vec2 origin, ux, uy;
                double width = 0;
                Prim base = p;
                base.halign = HAlign::Left;
                textFrame(base, origin, ux, uy, width);
                // anchor at the alignment point on the baseline so viewers with other fonts still align
                Vec2 anchor = origin + ux * (p.halign == HAlign::Left ? 0 : p.halign == HAlign::Center ? width / 2 : width);
                const char* anchorName = p.halign == HAlign::Left ? "start" : p.halign == HAlign::Center ? "middle" : "end";
                o << "<text x=\"" << num(anchor.x) << "\" y=\"" << num(H - anchor.y) << "\" font-family=\"Helvetica, Arial, sans-serif\" font-size=\""
                  << num(p.height / 0.718) << "\" fill=\"" << hex6(p.color) << "\" text-anchor=\"" << anchorName << "\"";
                if (std::abs(p.rotation) > 1e-9) {
                    o << " transform=\"rotate(" << num(-p.rotation) << " " << num(anchor.x) << " " << num(H - anchor.y) << ")\"";
                }
                o << ">" << xmlEscape(p.text) << "</text>\n";
            } else if (p.kind == Prim::Fill) {
                o << "<path d=\"" << svgPath(p.path, H) << "\" fill=\"" << hex6(p.color) << "\" fill-rule=\"evenodd\" stroke=\"none\"/>\n";
            } else {
                o << "<path d=\"" << svgPath(p.path, H) << "\" fill=\"none\" stroke=\"" << hex6(p.color) << "\" stroke-width=\""
                  << num(p.width) << "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"";
                if (!p.dash.empty()) {
                    o << " stroke-dasharray=\"";
                    for (std::size_t i = 0; i < p.dash.size(); ++i) o << (i ? " " : "") << num(p.dash[i]);
                    o << "\"";
                }
                o << "/>\n";
            }
        }
        o << "</g>\n";
    }
    o << "</svg>\n";
    return o.str();
}

std::string toPdf(const Drawing& doc) {
    const double W = doc.paperWidth(), H = doc.paperHeight();
    const double pt = 72.0 / 25.4;
    std::string content = pdfNum(pt) + " 0 0 " + pdfNum(pt) + " 0 0 cm\n1 J 1 j\n";
    for (const auto& p : sheetPrims(doc, false)) {
        if (p.kind == Prim::Text) {
            Vec2 origin, ux, uy;
            double width = 0;
            textFrame(p, origin, ux, uy, width);
            content += "BT\n/F1 " + pdfNum(p.height / 0.718) + " Tf\n" + rgb(p.color) + " rg\n" + pdfNum(ux.x) + " " + pdfNum(ux.y) +
                       " " + pdfNum(uy.x) + " " + pdfNum(uy.y) + " " + pdfNum(origin.x) + " " + pdfNum(origin.y) + " Tm\n" +
                       pdfString(p.text) + " Tj\nET\n";
        } else if (p.kind == Prim::Fill) {
            content += rgb(p.color) + " rg\n" + pdfPath(p.path) + "f*\n";
        } else {
            content += rgb(p.color) + " RG\n" + pdfNum(p.width) + " w\n[";
            for (std::size_t i = 0; i < p.dash.size(); ++i) content += (i ? " " : "") + pdfNum(p.dash[i]);
            content += "] 0 d\n" + pdfPath(p.path) + "S\n";
        }
    }

    auto info = [](const std::string& s) { return pdfString(s); };
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + pdfNum(W * pt) + " " + pdfNum(H * pt) +
            "] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        "<< /Title " + info(doc.title.title.empty() ? "blueprint" : doc.title.title) + " /Author " + info(doc.title.author) +
            " /Subject " + info("scale " + doc.scaleText() + " on " + doc.paper) + " /Creator (ACHCLWSS blueprint) /Producer (ACHCLWSS) >>",
    };
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (auto off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R /Info 6 0 R >>\nstartxref\n" + std::to_string(xref) +
           "\n%%EOF\n";
    return out;
}

void exportFile(const Drawing& doc, const std::filesystem::path& path, double dpi, int quality) {
    const std::string e = ext(path);
    std::string bytes;
    if (e == "svg") {
        bytes = toSvg(doc);
    } else if (e == "pdf") {
        bytes = toPdf(doc);
    } else if (e == "dxf") {
        bytes = toDxf(doc);
    } else if (raster::canWrite(path)) {
        raster::Image img = render(doc, sheetView(doc), 0, dpi, false);
        raster::save(img, path, quality);
        return;
    } else {
        throw std::runtime_error("can't export ." + e + " (use .dxf, .svg, .pdf, .png, .jpg, .bmp, .tga or .ppm)");
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << bytes;
    if (!out) throw std::runtime_error("failed writing " + path.string());
}

}  // namespace achclwss::blueprint
