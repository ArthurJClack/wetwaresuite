// Shape outlines, connector routing (straight, orthogonal around obstacles,
// curved), moving and resizing with children, ink smoothing and text
// measurement.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>

#include "internal.hpp"

namespace wetwaresuite::board {

using namespace detail;

namespace detail {

pdf::Font pdfFont(const std::string& font, bool bold, bool italic) {
    pdf::Font f;
    f.family = font == "serif" ? pdf::Family::Serif : (font == "mono" ? pdf::Family::Mono : pdf::Family::Sans);
    f.bold = bold;
    f.italic = italic;
    return f;
}

std::uint64_t hashOf(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string fmtNum(double v) {
    if (std::abs(v - std::round(v)) < 1e-6) return std::to_string(static_cast<long long>(std::llround(v)));
    std::ostringstream o;
    o.precision(6);
    o << v;
    return o.str();
}

double length(const std::vector<Pt>& pts) {
    double L = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) L += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    return L;
}

Pt pointAt(const std::vector<Pt>& pts, double distance, Pt* direction) {
    if (pts.empty()) return {};
    if (pts.size() == 1) return pts[0];
    double left = std::max(0.0, distance);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const double seg = std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        if (left <= seg || i + 1 == pts.size()) {
            const double t = seg > 0 ? std::min(1.0, left / seg) : 0;
            if (direction && seg > 0) *direction = {(pts[i].x - pts[i - 1].x) / seg, (pts[i].y - pts[i - 1].y) / seg};
            return {pts[i - 1].x + (pts[i].x - pts[i - 1].x) * t, pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t};
        }
        left -= seg;
    }
    return pts.back();
}

std::vector<Pt> flattenCubic(Pt a, Pt b, Pt c, Pt d, int steps) {
    std::vector<Pt> out;
    out.reserve(static_cast<std::size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps, u = 1 - t;
        out.push_back({u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
                       u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y});
    }
    return out;
}

}  // namespace detail

// ---- shapes -----------------------------------------------------------------------------

namespace {

void arc(std::vector<Pt>& out, double cx, double cy, double rx, double ry, double a0, double a1, int steps) {
    for (int i = 0; i <= steps; ++i) {
        const double a = a0 + (a1 - a0) * i / steps;
        out.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
}

std::vector<Pt> roundedRect(double x, double y, double w, double h, double r) {
    r = std::max(0.0, std::min({r, w / 2, h / 2}));
    if (r <= 0.01) return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    std::vector<Pt> p;
    const int n = 8;
    arc(p, x + w - r, y + r, r, r, -kPi / 2, 0, n);
    arc(p, x + w - r, y + h - r, r, r, 0, kPi / 2, n);
    arc(p, x + r, y + h - r, r, r, kPi / 2, kPi, n);
    arc(p, x + r, y + r, r, r, kPi, 1.5 * kPi, n);
    return p;
}

}  // namespace

ShapeGeom shapeGeometry(const Element& e, double radius) {
    ShapeGeom g;
    const double x = e.x, y = e.y, w = std::max(0.0, e.w), h = std::max(0.0, e.h);
    auto body = [&](std::vector<Pt> pts) { g.body.push_back({std::move(pts), true}); };
    switch (e.kind) {
        case Kind::Rect:
        case Kind::Frame:
        case Kind::Image: body({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}); break;
        case Kind::Subroutine: {
            body({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}});
            const double i = std::min(10.0, w * 0.1);
            g.lines.push_back({{{x + i, y}, {x + i, y + h}}, false});
            g.lines.push_back({{{x + w - i, y}, {x + w - i, y + h}}, false});
            break;
        }
        case Kind::Rounded: body(roundedRect(x, y, w, h, radius)); break;
        case Kind::Stadium: body(roundedRect(x, y, w, h, std::min(w, h) / 2)); break;
        case Kind::Ellipse: {
            std::vector<Pt> p;
            arc(p, x + w / 2, y + h / 2, w / 2, h / 2, 0, 2 * kPi, 64);
            p.pop_back();
            body(p);
            break;
        }
        case Kind::Diamond: body({{x + w / 2, y}, {x + w, y + h / 2}, {x + w / 2, y + h}, {x, y + h / 2}}); break;
        case Kind::Hexagon: {
            const double i = std::min(h * 0.35, w * 0.25);
            body({{x + i, y}, {x + w - i, y}, {x + w, y + h / 2}, {x + w - i, y + h}, {x + i, y + h}, {x, y + h / 2}});
            break;
        }
        case Kind::Parallelogram: {
            const double s = std::min(w * 0.2, h * 0.6);
            body({{x + s, y}, {x + w, y}, {x + w - s, y + h}, {x, y + h}});
            break;
        }
        case Kind::Trapezoid: {
            const double s = std::min(w * 0.2, h * 0.6);
            body({{x + s, y}, {x + w - s, y}, {x + w, y + h}, {x, y + h}});
            break;
        }
        case Kind::Triangle: body({{x + w / 2, y}, {x + w, y + h}, {x, y + h}}); break;
        case Kind::Cylinder: {
            const double ry = std::min(h * 0.18, std::max(6.0, w * 0.12));
            const double cx = x + w / 2, rx = w / 2;
            std::vector<Pt> p;
            arc(p, cx, y + ry, rx, ry, kPi, 2 * kPi, 24);          // back of the top rim
            arc(p, cx, y + h - ry, rx, ry, 0, kPi, 24);            // front of the bottom
            body(p);
            std::vector<Pt> rim;
            arc(rim, cx, y + ry, rx, ry, kPi, 0, 24);              // front of the top rim
            g.lines.push_back({rim, false});
            break;
        }
        case Kind::Cloud: {
            std::vector<Pt> p;
            const double cx = x + w / 2, cy = y + h / 2;
            const int n = 120;
            for (int i = 0; i < n; ++i) {
                const double a = 2 * kPi * i / n;
                const double k = 0.86 + 0.14 * std::abs(std::sin(4.5 * a + 0.4));
                p.push_back({cx + w / 2 * k * std::cos(a), cy + h / 2 * k * std::sin(a)});
            }
            body(p);
            break;
        }
        case Kind::Note: {
            const double f = std::min(w, h) * 0.14;
            body({{x, y}, {x + w, y}, {x + w, y + h - f}, {x + w - f, y + h}, {x, y + h}});
            g.lines.push_back({{{x + w - f, y + h}, {x + w - f, y + h - f}, {x + w, y + h - f}}, false});
            break;
        }
        case Kind::Text:
        case Kind::Group:
        case Kind::Connector:
        case Kind::Ink: break;
    }
    return g;
}

// ---- text -------------------------------------------------------------------------------

double textWidth(const std::string& line, const std::string& font, bool bold, bool italic, double size) {
    return pdf::textWidth(pdfFont(font, bold, italic), line, size);
}

std::vector<std::string> wrapText(const std::string& text, const std::string& font, bool bold, bool italic, double size, double maxWidth) {
    std::vector<std::string> out;
    const pdf::Font f = pdfFont(font, bold, italic);
    for (const auto& para : splitLines(text)) {
        if (maxWidth <= 0 || pdf::textWidth(f, para, size) <= maxWidth) {
            out.push_back(para);
            continue;
        }
        std::istringstream words(para);
        std::string word, line;
        while (words >> word) {
            const std::string tryLine = line.empty() ? word : line + " " + word;
            if (pdf::textWidth(f, tryLine, size) <= maxWidth || line.empty()) {
                line = tryLine;
            } else {
                out.push_back(line);
                line = word;
            }
            // a word longer than the box breaks by characters
            while (pdf::textWidth(f, line, size) > maxWidth && line.size() > 1) {
                const auto cps = pdf::utf8Decode(line);
                std::string head;
                std::size_t k = 0;
                for (; k < cps.size(); ++k) {
                    const std::string next = head + pdf::utf8Encode(cps[k]);
                    if (pdf::textWidth(f, next, size) > maxWidth && k > 0) break;
                    head = next;
                }
                if (k >= cps.size()) break;
                out.push_back(head);
                std::string rest;
                for (; k < cps.size(); ++k) rest += pdf::utf8Encode(cps[k]);
                line = rest;
            }
        }
        out.push_back(line);
    }
    return out;
}

namespace detail {

Box textArea(const Element& e, const Resolved& s) {
    const double x = e.x, y = e.y, w = e.w, h = e.h;
    Box b;
    auto set = [&](double x0, double y0, double x1, double y1) {
        b.x0 = x0;
        b.y0 = y0;
        b.x1 = std::max(x0, x1);
        b.y1 = std::max(y0, y1);
        b.empty = false;
    };
    const double pad = 10;
    switch (e.kind) {
        case Kind::Text: set(x, y, x + w, y + h); break;
        case Kind::Diamond: set(x + w * 0.17, y + h * 0.22, x + w * 0.83, y + h * 0.78); break;
        case Kind::Ellipse:
        case Kind::Cloud: set(x + w * 0.14, y + h * 0.15, x + w * 0.86, y + h * 0.85); break;
        case Kind::Triangle: set(x + w * 0.22, y + h * 0.45, x + w * 0.78, y + h - pad * 0.6); break;
        case Kind::Hexagon: {
            const double i = std::min(h * 0.35, w * 0.25);
            set(x + i * 0.6, y + pad * 0.6, x + w - i * 0.6, y + h - pad * 0.6);
            break;
        }
        case Kind::Parallelogram:
        case Kind::Trapezoid: {
            const double i = std::min(w * 0.2, h * 0.6);
            set(x + i * 0.8, y + pad * 0.6, x + w - i * 0.8, y + h - pad * 0.6);
            break;
        }
        case Kind::Cylinder: {
            const double ry = std::min(h * 0.18, std::max(6.0, w * 0.12));
            set(x + pad, y + ry * 2 + 2, x + w - pad, y + h - ry * 0.5);
            break;
        }
        case Kind::Subroutine: set(x + pad + std::min(10.0, w * 0.1), y + pad * 0.6, x + w - pad - std::min(10.0, w * 0.1), y + h - pad * 0.6); break;
        case Kind::Note: set(x + 14, y + 12, x + w - 14, y + h - 12); break;
        case Kind::Frame: set(x + 12, y + 8, x + w - 12, y + 8 + s.fontSize * kLineHeight); break;
        case Kind::Image: set(x, y + h + 4, x + w, y + h + 4 + s.fontSize * kLineHeight * 2); break;
        default: set(x + pad, y + pad * 0.6, x + w - pad, y + h - pad * 0.6); break;
    }
    return b;
}

std::vector<PlacedLine> placeText(const Element& e, const Resolved& s, const Box& area, bool wrap) {
    std::vector<PlacedLine> out;
    if (e.text.empty()) return out;
    const std::vector<std::string> lines = wrap ? wrapText(e.text, s.font, s.bold, s.italic, s.fontSize, area.w() + 0.5) : splitLines(e.text);
    const double lh = s.fontSize * kLineHeight;
    const double total = lh * static_cast<double>(lines.size());
    double top = area.y0;
    if (s.valign == "middle") top = area.cy() - total / 2;
    else if (s.valign == "bottom") top = area.y1 - total;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const double tw = textWidth(lines[i], s.font, s.bold, s.italic, s.fontSize);
        double x = area.x0;
        if (s.align == "center") x = area.cx() - tw / 2;
        else if (s.align == "right") x = area.x1 - tw;
        // baseline: a line box of 1.25 em with the ascender at ~0.95 em
        out.push_back({x, top + lh * static_cast<double>(i) + s.fontSize * 1.03, lines[i]});
    }
    return out;
}

}  // namespace detail

Pt fitText(Kind kind, const std::string& text, const Resolved& s, double minW, double minH) {
    if (text.empty()) return {minW, minH};
    double natural = 0;
    for (const auto& l : splitLines(text)) natural = std::max(natural, textWidth(l, s.font, s.bold, s.italic, s.fontSize));
    double fx = 1, fy = 1, padX = 20, padY = 14;
    switch (kind) {
        case Kind::Diamond: fx = 1.55; fy = 1.9; break;
        case Kind::Ellipse:
        case Kind::Cloud: fx = 1.38; fy = 1.45; break;
        case Kind::Triangle: fx = 1.9; fy = 2.4; break;
        case Kind::Hexagon:
        case Kind::Parallelogram:
        case Kind::Trapezoid: fx = 1.2; padX = 34; break;
        case Kind::Cylinder: padY = 34; break;
        case Kind::Text: padX = 0; padY = 0; break;
        case Kind::Note: padX = 28; padY = 24; break;
        default: break;
    }
    const double maxW = kind == Kind::Note ? std::max(minW, 160.0) : std::max(minW, 260.0);  // sticky notes stay square-ish
    double w = std::clamp(natural * fx + padX, minW, maxW);
    if (kind == Kind::Text) w = std::max(minW, natural + 1);
    const double inner = kind == Kind::Text ? w : std::max(10.0, (w - padX) / fx);
    const auto lines = wrapText(text, s.font, s.bold, s.italic, s.fontSize, inner + 0.5);
    const double h = std::max(minH, static_cast<double>(lines.size()) * s.fontSize * kLineHeight * fy + padY);
    return {w, h};
}

// ---- boxes and movement -----------------------------------------------------------------

Box elementBox(const Board& board, const Element& e) {
    Box b;
    if (e.kind == Kind::Connector) {
        for (const auto& p : connectorPath(board, e)) b.add(p.x, p.y);
        return b;
    }
    if (e.kind == Kind::Ink) {
        for (const auto& p : e.ink) b.add(p.x, p.y);
        return b.grown(e.inkWidth / 2);
    }
    b.add(e.x, e.y);
    b.add(e.x + std::max(0.0, e.w), e.y + std::max(0.0, e.h));
    return b;
}

Box contentBounds(const Board& board, bool visibleOnly) {
    Box b;
    for (const auto& e : board.elements) {
        if (visibleOnly) {
            const Layer* l = board.layer(e.layer);
            if (l && !l->visible) continue;
        }
        Box eb = elementBox(board, e);
        if (e.kind == Kind::Image && !e.text.empty()) eb.y1 += 40;
        b.add(eb);
    }
    return b;
}

void moveBy(Board& board, Element& e, double dx, double dy) {
    std::vector<std::string> ids{e.id};
    for (const auto& d : board.descendants(e.id)) ids.push_back(d);
    for (const auto& id : ids) {
        Element* m = board.get(id);
        if (!m) continue;
        m->x += dx;
        m->y += dy;
        for (auto& p : m->ink) {
            p.x += dx;
            p.y += dy;
        }
        if (m->kind == Kind::Connector) {
            m->start = {m->start.x + dx, m->start.y + dy};
            m->end = {m->end.x + dx, m->end.y + dy};
            for (auto& w : m->waypoints) w = {w.x + dx, w.y + dy};
        }
    }
}

void resizeTo(Board& board, Element& e, double x, double y, double w, double h) {
    w = std::max(1.0, w);
    h = std::max(1.0, h);
    if (e.kind == Kind::Ink || e.kind == Kind::Group) {
        const Box old = elementBox(board, e);
        const double sx = old.w() > 0 ? w / old.w() : 1, sy = old.h() > 0 ? h / old.h() : 1;
        auto map = [&](double& px, double& py) {
            px = x + (px - old.x0) * sx;
            py = y + (py - old.y0) * sy;
        };
        if (e.kind == Kind::Ink) {
            for (auto& p : e.ink) map(p.x, p.y);
        } else {
            for (const auto& id : board.descendants(e.id)) {
                Element* c = board.get(id);
                if (!c || c->kind == Kind::Group) continue;
                if (c->kind == Kind::Ink) {
                    for (auto& p : c->ink) map(p.x, p.y);
                } else if (c->kind == Kind::Connector) {
                    map(c->start.x, c->start.y);
                    map(c->end.x, c->end.y);
                } else {
                    map(c->x, c->y);
                    c->w *= sx;
                    c->h *= sy;
                }
            }
        }
    }
    e.x = x;
    e.y = y;
    e.w = w;
    e.h = h;
}

void refreshGroups(Board& board, bool fitFrames, double padding) {
    // innermost first: repeat a few times so nested groups settle
    for (int pass = 0; pass < 6; ++pass) {
        bool changed = false;
        for (auto& e : board.elements) {
            if (e.kind != Kind::Group && !(fitFrames && e.kind == Kind::Frame)) continue;
            Box b;
            for (const auto& c : board.children(e.id)) {
                if (const Element* ce = board.get(c)) b.add(elementBox(board, *ce));
            }
            if (b.empty) continue;
            if (e.kind == Kind::Frame) {
                const Resolved s = resolve(board, e);
                const double title = e.text.empty() ? 0 : s.fontSize * kLineHeight + 8;
                b.x0 -= padding;
                b.x1 += padding;
                b.y0 -= padding + title;
                b.y1 += padding;
            }
            if (std::abs(b.x0 - e.x) + std::abs(b.y0 - e.y) + std::abs(b.w() - e.w) + std::abs(b.h() - e.h) > 1e-6) {
                e.x = b.x0;
                e.y = b.y0;
                e.w = b.w();
                e.h = b.h();
                changed = true;
            }
        }
        if (!changed) break;
    }
}

std::vector<InkPoint> smoothInk(const std::vector<InkPoint>& raw, double amount) {
    amount = std::clamp(amount, 0.0, 0.95);
    if (raw.size() < 3 || amount <= 0) return raw;
    // drop near-duplicates
    std::vector<InkPoint> pts{raw.front()};
    for (std::size_t i = 1; i < raw.size(); ++i) {
        if (std::hypot(raw[i].x - pts.back().x, raw[i].y - pts.back().y) > 0.5 || i + 1 == raw.size()) pts.push_back(raw[i]);
    }
    // exponential smoothing forwards and backwards (no lag)
    std::vector<InkPoint> f = pts, b = pts;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        f[i].x = f[i - 1].x * amount + pts[i].x * (1 - amount);
        f[i].y = f[i - 1].y * amount + pts[i].y * (1 - amount);
    }
    for (std::size_t i = pts.size() - 1; i-- > 0;) {
        b[i].x = b[i + 1].x * amount + pts[i].x * (1 - amount);
        b[i].y = b[i + 1].y * amount + pts[i].y * (1 - amount);
    }
    std::vector<InkPoint> s = pts;
    for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
        s[i].x = (f[i].x + b[i].x) / 2;
        s[i].y = (f[i].y + b[i].y) / 2;
    }
    // Chaikin corner cutting, twice
    for (int it = 0; it < 2; ++it) {
        std::vector<InkPoint> c{s.front()};
        for (std::size_t i = 0; i + 1 < s.size(); ++i) {
            const auto& p = s[i];
            const auto& q = s[i + 1];
            c.push_back({p.x * 0.75 + q.x * 0.25, p.y * 0.75 + q.y * 0.25, p.pressure * 0.75 + q.pressure * 0.25});
            c.push_back({p.x * 0.25 + q.x * 0.75, p.y * 0.25 + q.y * 0.75, p.pressure * 0.25 + q.pressure * 0.75});
        }
        c.push_back(s.back());
        s = std::move(c);
    }
    return s;
}

// ---- connectors -------------------------------------------------------------------------

namespace {

struct End {
    Pt p;
    Pt dir;              // outward from the shape (zero for free ends)
    bool attached = false;
    Box box;
    const Element* el = nullptr;
};

Pt norm(Pt v) {
    const double l = std::hypot(v.x, v.y);
    return l > 1e-9 ? Pt{v.x / l, v.y / l} : Pt{0, 0};
}

// The outermost crossing of the ray centre -> centre + dir * far with the element's outline.
Pt boundaryPoint(const Element& e, Pt dir) {
    const Box b{e.x, e.y, e.x + e.w, e.y + e.h, false};
    const Pt c{b.cx(), b.cy()};
    dir = norm(dir);
    if (dir.x == 0 && dir.y == 0) return c;
    ShapeGeom g = shapeGeometry(e);
    if (g.body.empty()) g.body.push_back({{{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}}, true});
    const double far = (e.w + e.h) * 2 + 10;
    const Pt q{c.x + dir.x * far, c.y + dir.y * far};
    double best = -1;
    for (const auto& contour : g.body) {
        const auto& p = contour.pts;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const Pt a = p[i], d = p[(i + 1) % p.size()];
            const double rx = q.x - c.x, ry = q.y - c.y, sx = d.x - a.x, sy = d.y - a.y;
            const double den = rx * sy - ry * sx;
            if (std::abs(den) < 1e-12) continue;
            const double t = ((a.x - c.x) * sy - (a.y - c.y) * sx) / den;
            const double u = ((a.x - c.x) * ry - (a.y - c.y) * rx) / den;
            if (t >= 0 && t <= 1 && u >= -1e-9 && u <= 1 + 1e-9) best = std::max(best, t);
        }
    }
    if (best < 0) return c;
    return {c.x + (q.x - c.x) * best, c.y + (q.y - c.y) * best};
}

Pt sideDir(const std::string& side) {
    if (side == "top") return {0, -1};
    if (side == "bottom") return {0, 1};
    if (side == "left") return {-1, 0};
    if (side == "right") return {1, 0};
    return {0, 0};
}

// "top", "right", ..., "center", or "fx,fy" fractions of the box.
bool anchorAt(const Element& e, const std::string& anchor, End& end) {
    const Box b{e.x, e.y, e.x + e.w, e.y + e.h, false};
    if (anchor == "center") {
        end.p = {b.cx(), b.cy()};
        end.dir = {0, 0};
        return true;
    }
    const Pt d = sideDir(anchor);
    if (d.x != 0 || d.y != 0) {
        end.p = boundaryPoint(e, d);
        end.dir = d;
        return true;
    }
    const auto comma = anchor.find(',');
    if (comma != std::string::npos) {
        try {
            const double fx = std::stod(anchor.substr(0, comma)), fy = std::stod(anchor.substr(comma + 1));
            end.p = {b.x0 + b.w() * fx, b.y0 + b.h() * fy};
            const double dl = fx, dr = 1 - fx, dt = fy, db = 1 - fy;
            const double m = std::min({dl, dr, dt, db});
            end.dir = m == dl ? Pt{-1, 0} : m == dr ? Pt{1, 0} : m == dt ? Pt{0, -1} : Pt{0, 1};
            return true;
        } catch (const std::exception&) {
        }
    }
    return false;
}

std::string autoSide(const Box& a, const Box& b) {
    const double dx = b.cx() - a.cx(), dy = b.cy() - a.cy();
    const double gapX = std::abs(dx) - (a.w() + b.w()) / 2, gapY = std::abs(dy) - (a.h() + b.h()) / 2;
    if (gapX > gapY) return dx > 0 ? "right" : "left";
    return dy > 0 ? "bottom" : "top";
}

std::string opposite(const std::string& s) {
    if (s == "top") return "bottom";
    if (s == "bottom") return "top";
    if (s == "left") return "right";
    return "left";
}

Box nodeBox(const Board& board, const Element& e) {
    if (e.kind == Kind::Group) return elementBox(board, e);
    return Box{e.x, e.y, e.x + e.w, e.y + e.h, false};
}

int dirIndex(Pt d) {
    if (d.x > 0.5) return 0;
    if (d.y > 0.5) return 1;
    if (d.x < -0.5) return 2;
    return 3;
}

std::vector<Pt> simplify(const std::vector<Pt>& pts) {
    std::vector<Pt> out;
    for (const auto& p : pts) {
        if (!out.empty() && std::abs(out.back().x - p.x) < 1e-6 && std::abs(out.back().y - p.y) < 1e-6) continue;
        if (out.size() >= 2) {
            const Pt a = out[out.size() - 2], b = out.back();
            const double cross = (b.x - a.x) * (p.y - b.y) - (b.y - a.y) * (p.x - b.x);
            const double dot = (b.x - a.x) * (p.x - b.x) + (b.y - a.y) * (p.y - b.y);
            if (std::abs(cross) < 1e-6 && dot >= 0) {
                out.back() = p;
                continue;
            }
        }
        out.push_back(p);
    }
    return out;
}

// Orthogonal route from s (leaving along s.dir) to e (arriving against e.dir), around node boxes.
std::vector<Pt> elbowRoute(const Board& board, const Element& c, const End& s, const End& e) {
    const double stub = 20, margin = 12;
    Pt sd = s.dir, ed = e.dir;
    if (sd.x == 0 && sd.y == 0) sd = std::abs(e.p.x - s.p.x) > std::abs(e.p.y - s.p.y) ? Pt{e.p.x > s.p.x ? 1.0 : -1.0, 0} : Pt{0, e.p.y > s.p.y ? 1.0 : -1.0};
    if (ed.x == 0 && ed.y == 0) ed = std::abs(e.p.x - s.p.x) > std::abs(e.p.y - s.p.y) ? Pt{e.p.x > s.p.x ? -1.0 : 1.0, 0} : Pt{0, e.p.y > s.p.y ? -1.0 : 1.0};
    const Pt s1{s.p.x + sd.x * (s.attached ? stub : 0), s.p.y + sd.y * (s.attached ? stub : 0)};
    const Pt e1{e.p.x + ed.x * (e.attached ? stub : 0), e.p.y + ed.y * (e.attached ? stub : 0)};

    Box area;
    area.add(s1.x, s1.y);
    area.add(e1.x, e1.y);
    area = area.grown(240);
    std::vector<Box> obstacles, tight, extraLines;  // tight: kept at their real outline (routes shouldn't hug them)
    for (const auto& o : board.elements) {
        if (!isNode(o.kind) || o.kind == Kind::Frame || o.kind == Kind::Group || o.id == c.id) continue;
        const Layer* l = board.layer(o.layer);
        if (l && !l->visible) continue;
        Box b = nodeBox(board, o).grown(margin);
        if (!b.intersects(area)) continue;
        auto inside = [&](const Box& bx, Pt p) { return p.x > bx.x0 + 1e-6 && p.x < bx.x1 - 1e-6 && p.y > bx.y0 + 1e-6 && p.y < bx.y1 - 1e-6; };
        if (inside(b, s1) || inside(b, e1)) {
            // a neighbour closer than the margin: keep only its real outline as the obstacle
            const Box wide = b;
            b = nodeBox(board, o);
            if (inside(b, s1) || inside(b, e1)) continue;  // an end inside a shape can't avoid it
            tight.push_back(b);
            extraLines.push_back(wide);
        }
        obstacles.push_back(b);
        if (obstacles.size() > 160) break;
    }
    std::vector<double> xs{s1.x, e1.x, (s1.x + e1.x) / 2}, ys{s1.y, e1.y, (s1.y + e1.y) / 2};
    for (const auto& b : extraLines) {
        xs.push_back(b.x0);
        xs.push_back(b.x1);
        ys.push_back(b.y0);
        ys.push_back(b.y1);
    }
    for (const auto& b : obstacles) {
        xs.push_back(b.x0);
        xs.push_back(b.x1);
        ys.push_back(b.y0);
        ys.push_back(b.y1);
    }
    auto uniq = [](std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), v.end());
    };
    uniq(xs);
    uniq(ys);
    const int nx = static_cast<int>(xs.size()), ny = static_cast<int>(ys.size());
    constexpr double eps = 1e-6;
    auto pointBlocked = [&](double x, double y) {
        for (const auto& b : obstacles) {
            if (x > b.x0 + eps && x < b.x1 - eps && y > b.y0 + eps && y < b.y1 - eps) return true;
        }
        return false;
    };
    auto segBlocked = [&](double xa, double ya, double xb, double yb) {
        const double x0 = std::min(xa, xb), x1 = std::max(xa, xb), y0 = std::min(ya, yb), y1 = std::max(ya, yb);
        for (const auto& b : obstacles) {
            if (x1 > b.x0 + eps && x0 < b.x1 - eps && y1 > b.y0 + eps && y0 < b.y1 - eps) return true;
        }
        return false;
    };
    auto indexOf = [](const std::vector<double>& v, double val) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (std::abs(v[i] - val) < 1e-6) return static_cast<int>(i);
        }
        return 0;
    };
    const int si = indexOf(xs, s1.x), sj = indexOf(ys, s1.y), ti = indexOf(xs, e1.x), tj = indexOf(ys, e1.y);
    std::vector<char> blocked(static_cast<std::size_t>(nx * ny));
    for (int i = 0; i < nx; ++i) {
        for (int j = 0; j < ny; ++j) blocked[static_cast<std::size_t>(i * ny + j)] = pointBlocked(xs[i], ys[j]) ? 1 : 0;
    }
    blocked[static_cast<std::size_t>(si * ny + sj)] = 0;
    blocked[static_cast<std::size_t>(ti * ny + tj)] = 0;
    const double bend = 36;
    const int startDir = dirIndex(sd);
    const int finalDir = dirIndex({-ed.x, -ed.y});
    const std::size_t states = static_cast<std::size_t>(nx * ny * 4);
    std::vector<double> dist(states, std::numeric_limits<double>::infinity());
    std::vector<int> prev(states, -1);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    auto stateOf = [&](int i, int j, int d) { return (i * ny + j) * 4 + d; };
    const int start = stateOf(si, sj, startDir);
    dist[static_cast<std::size_t>(start)] = 0;
    pq.push({0, start});
    const int dxs[4] = {1, 0, -1, 0}, dys[4] = {0, 1, 0, -1};
    int goal = -1;
    while (!pq.empty()) {
        auto [d, st] = pq.top();
        pq.pop();
        if (d > dist[static_cast<std::size_t>(st)]) continue;
        const int dir = st % 4, node = st / 4, i = node / ny, j = node % ny;
        if (i == ti && j == tj) {
            const double extra = dir == finalDir ? 0 : bend;
            if (goal < 0 || d + extra < dist[static_cast<std::size_t>(goal)] + (goal % 4 == finalDir ? 0 : bend)) goal = st;
            // keep going: another arrival direction may be cheaper overall
            continue;
        }
        for (int k = 0; k < 4; ++k) {
            if ((k + 2) % 4 == dir) continue;  // no U-turns in place
            const int ni = i + dxs[k], nj = j + dys[k];
            if (ni < 0 || nj < 0 || ni >= nx || nj >= ny) continue;
            if (blocked[static_cast<std::size_t>(ni * ny + nj)]) continue;
            if (segBlocked(xs[i], ys[j], xs[ni], ys[nj])) continue;
            const double len = std::abs(xs[ni] - xs[i]) + std::abs(ys[nj] - ys[j]);
            double hug = 0;
            for (const auto& tb : tight) {
                const bool alongX = ys[j] == ys[nj] && (std::abs(ys[j] - tb.y0) < eps || std::abs(ys[j] - tb.y1) < eps) &&
                                    std::max(xs[i], xs[ni]) > tb.x0 && std::min(xs[i], xs[ni]) < tb.x1;
                const bool alongY = xs[i] == xs[ni] && (std::abs(xs[i] - tb.x0) < eps || std::abs(xs[i] - tb.x1) < eps) &&
                                    std::max(ys[j], ys[nj]) > tb.y0 && std::min(ys[j], ys[nj]) < tb.y1;
                if (alongX || alongY) hug = len * 3;
            }
            const double nd = d + len + hug + (k == dir ? 0 : bend);
            const int ns = stateOf(ni, nj, k);
            if (nd < dist[static_cast<std::size_t>(ns)]) {
                dist[static_cast<std::size_t>(ns)] = nd;
                prev[static_cast<std::size_t>(ns)] = st;
                pq.push({nd, ns});
            }
        }
    }
    std::vector<Pt> pts;
    if (goal >= 0) {
        std::vector<Pt> rev;
        for (int st = goal; st >= 0; st = prev[static_cast<std::size_t>(st)]) {
            const int node = st / 4;
            rev.push_back({xs[node / ny], ys[node % ny]});
        }
        pts.push_back(s.p);
        pts.insert(pts.end(), rev.rbegin(), rev.rend());
        pts.push_back(e.p);
    } else {
        // no clear route: a plain three-segment elbow
        pts = {s.p, s1};
        if (std::abs(sd.x) > 0.5) pts.push_back({(s1.x + e1.x) / 2, s1.y}), pts.push_back({(s1.x + e1.x) / 2, e1.y});
        else pts.push_back({s1.x, (s1.y + e1.y) / 2}), pts.push_back({e1.x, (s1.y + e1.y) / 2});
        pts.push_back(e1);
        pts.push_back(e.p);
    }
    return simplify(pts);
}

std::vector<Pt> catmullRom(const std::vector<Pt>& p, int steps) {
    if (p.size() < 3) return p;
    std::vector<Pt> out;
    for (std::size_t i = 0; i + 1 < p.size(); ++i) {
        const Pt p0 = i == 0 ? p[0] : p[i - 1], p1 = p[i], p2 = p[i + 1], p3 = i + 2 < p.size() ? p[i + 2] : p[i + 1];
        const Pt b{p1.x + (p2.x - p0.x) / 6, p1.y + (p2.y - p0.y) / 6};
        const Pt c{p2.x - (p3.x - p1.x) / 6, p2.y - (p3.y - p1.y) / 6};
        auto seg = flattenCubic(p1, b, c, p2, steps);
        if (!out.empty()) seg.erase(seg.begin());
        out.insert(out.end(), seg.begin(), seg.end());
    }
    return out;
}

}  // namespace

std::vector<Pt> connectorPath(const Board& board, const Element& c) {
    const Element* a = c.from.empty() ? nullptr : board.get(c.from);
    const Element* b = c.to.empty() ? nullptr : board.get(c.to);
    if (a && !isNode(a->kind)) a = nullptr;
    if (b && !isNode(b->kind)) b = nullptr;
    End s, e;
    s.p = c.start;
    e.p = c.end;
    if (a) {
        s.attached = true;
        s.el = a;
        s.box = nodeBox(board, *a);
    }
    if (b) {
        e.attached = true;
        e.el = b;
        e.box = nodeBox(board, *b);
    }
    const std::string route = c.route;

    // a loop back to the same shape
    if (a && b && a == b) {
        const Box& bx = s.box;
        const double d = 28;
        std::vector<Pt> pts{{bx.x1, bx.cy() - bx.h() * 0.2}, {bx.x1 + d, bx.cy() - bx.h() * 0.2}, {bx.x1 + d, bx.y0 - d},
                            {bx.cx() + bx.w() * 0.2, bx.y0 - d}, {bx.cx() + bx.w() * 0.2, bx.y0}};
        return route == "curved" ? catmullRom(pts, 8) : pts;
    }

    auto elementFor = [&](const End& end) {
        Element tmp = *end.el;
        tmp.x = end.box.x0;
        tmp.y = end.box.y0;
        tmp.w = end.box.w();
        tmp.h = end.box.h();
        if (tmp.kind == Kind::Group) tmp.kind = Kind::Rect;
        return tmp;
    };

    if (route == "straight" || (route != "elbow" && route != "curved")) {
        // aim each attached end at the other end (or the nearest waypoint)
        Pt sTarget = c.waypoints.empty() ? (b ? Pt{e.box.cx(), e.box.cy()} : e.p) : c.waypoints.front();
        Pt eTarget = c.waypoints.empty() ? (a ? Pt{s.box.cx(), s.box.cy()} : s.p) : c.waypoints.back();
        if (a) {
            const Element ea = elementFor(s);
            if (c.fromAnchor != "auto" && anchorAt(ea, c.fromAnchor, s)) {
            } else {
                s.p = boundaryPoint(ea, {sTarget.x - s.box.cx(), sTarget.y - s.box.cy()});
            }
        }
        if (b) {
            const Element eb = elementFor(e);
            if (c.toAnchor != "auto" && anchorAt(eb, c.toAnchor, e)) {
            } else {
                e.p = boundaryPoint(eb, {eTarget.x - e.box.cx(), eTarget.y - e.box.cy()});
            }
        }
        // explicit anchors on one end: re-aim the other at it
        if (a && b && c.waypoints.empty()) {
            if (c.fromAnchor != "auto" && c.toAnchor == "auto") e.p = boundaryPoint(elementFor(e), {s.p.x - e.box.cx(), s.p.y - e.box.cy()});
            if (c.toAnchor != "auto" && c.fromAnchor == "auto") s.p = boundaryPoint(elementFor(s), {e.p.x - s.box.cx(), e.p.y - s.box.cy()});
        }
        std::vector<Pt> pts{s.p};
        pts.insert(pts.end(), c.waypoints.begin(), c.waypoints.end());
        pts.push_back(e.p);
        return pts;
    }

    // elbow and curved leave and enter through a side
    std::string sideA = c.fromAnchor, sideB = c.toAnchor;
    const Box boxA = a ? s.box : Box{s.p.x, s.p.y, s.p.x, s.p.y, false};
    const Box boxB = b ? e.box : Box{e.p.x, e.p.y, e.p.x, e.p.y, false};
    if (sideA == "auto") sideA = autoSide(boxA, boxB);
    if (sideB == "auto") sideB = c.fromAnchor == "auto" || !a ? opposite(autoSide(boxA, boxB)) : autoSide(boxB, boxA);
    if (a && !anchorAt(elementFor(s), sideA, s)) anchorAt(elementFor(s), "right", s);
    if (b && !anchorAt(elementFor(e), sideB, e)) anchorAt(elementFor(e), "left", e);
    if (!a) s.dir = {0, 0};
    if (!b) e.dir = {0, 0};

    if (route == "elbow") return elbowRoute(board, c, s, e);

    // curved
    if (!c.waypoints.empty()) {
        std::vector<Pt> pts{s.p};
        pts.insert(pts.end(), c.waypoints.begin(), c.waypoints.end());
        pts.push_back(e.p);
        return catmullRom(pts, 12);
    }
    const double d = std::hypot(e.p.x - s.p.x, e.p.y - s.p.y);
    const double k = std::clamp(d * 0.45, 20.0, 240.0);
    Pt sd = s.dir, ed = e.dir;
    if (sd.x == 0 && sd.y == 0) sd = norm({e.p.x - s.p.x, e.p.y - s.p.y});
    if (ed.x == 0 && ed.y == 0) ed = norm({s.p.x - e.p.x, s.p.y - e.p.y});
    return flattenCubic(s.p, {s.p.x + sd.x * k, s.p.y + sd.y * k}, {e.p.x + ed.x * k, e.p.y + ed.y * k}, e.p, 32);
}

}  // namespace wetwaresuite::board
