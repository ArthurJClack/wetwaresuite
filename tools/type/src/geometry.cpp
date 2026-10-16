// Contour geometry: segments, flattening, bounds, directions, quadratic and
// cubic conversion, SVG path data in and out, basic shapes.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "internal.hpp"

namespace achclwss::typeface {

Affine Affine::operator*(const Affine& m) const {
    return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d, a * m.e + c * m.f + e, b * m.e + d * m.f + f};
}

Affine Affine::rotate(double degrees) {
    const double r = degrees * kPi / 180, cs = std::cos(r), sn = std::sin(r);
    return {cs, sn, -sn, cs, 0, 0};
}

Affine Affine::skewX(double degrees) { return {1, 0, std::tan(degrees * kPi / 180), 1, 0, 0}; }

void Box::add(Vec p) {
    if (empty) {
        x0 = x1 = p.x;
        y0 = y1 = p.y;
        empty = false;
        return;
    }
    x0 = std::min(x0, p.x);
    x1 = std::max(x1, p.x);
    y0 = std::min(y0, p.y);
    y1 = std::max(y1, p.y);
}

void Box::add(const Box& b) {
    if (b.empty) return;
    add(Vec{b.x0, b.y0});
    add(Vec{b.x1, b.y1});
}

// ---- segments ----

std::vector<Segment> segments(const Contour& contour) {
    std::vector<Segment> out;
    const auto& pts = contour.points;
    const std::size_t n = pts.size();
    if (n < 2) return out;
    std::size_t start = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (pts[i].on()) {
            start = i;
            break;
        }
    }
    std::vector<Point> ring;
    ring.reserve(n + 1);
    if (start == n) {
        // only off-curve points: TrueType implies an on-curve point between each pair
        ring.push_back({(pts[n - 1].x + pts[0].x) / 2, (pts[n - 1].y + pts[0].y) / 2, PointType::On});
        for (const auto& p : pts) ring.push_back(p);
    } else {
        for (std::size_t i = 0; i < n; ++i) ring.push_back(pts[(start + i) % n]);
    }
    const Vec first{ring[0].x, ring[0].y};
    Vec cur = first;
    std::vector<Point> offs;
    auto flush = [&](Vec to) {
        if (offs.empty()) {
            out.push_back({1, {cur, to}});
        } else if (offs.size() == 2 && offs[0].type == PointType::Cubic && offs[1].type == PointType::Cubic) {
            out.push_back({3, {cur, {offs[0].x, offs[0].y}, {offs[1].x, offs[1].y}, to}});
        } else {
            for (std::size_t k = 0; k < offs.size(); ++k) {
                Vec c{offs[k].x, offs[k].y};
                Vec e = k + 1 < offs.size() ? Vec{(offs[k].x + offs[k + 1].x) / 2, (offs[k].y + offs[k + 1].y) / 2} : to;
                out.push_back({2, {cur, c, e}});
                cur = e;
            }
        }
        cur = to;
        offs.clear();
    };
    for (std::size_t i = 1; i < ring.size(); ++i) {
        if (ring[i].on()) flush({ring[i].x, ring[i].y});
        else offs.push_back(ring[i]);
    }
    flush(first);
    // drop a zero-length closing line
    if (out.size() > 1 && out.back().order == 1 && out.back().p[0] == out.back().p[1]) out.pop_back();
    return out;
}

Contour fromSegments(const std::vector<Segment>& segs) {
    Contour c;
    for (const auto& s : segs) {
        c.points.push_back({s.p[0].x, s.p[0].y, PointType::On});
        if (s.order == 2) c.points.push_back({s.p[1].x, s.p[1].y, PointType::Quad});
        if (s.order == 3) {
            c.points.push_back({s.p[1].x, s.p[1].y, PointType::Cubic});
            c.points.push_back({s.p[2].x, s.p[2].y, PointType::Cubic});
        }
    }
    return c;
}

Vec evalSegment(const Segment& s, double t) {
    const double u = 1 - t;
    if (s.order == 1) return s.p[0] + (s.p[1] - s.p[0]) * t;
    if (s.order == 2) return s.p[0] * (u * u) + s.p[1] * (2 * u * t) + s.p[2] * (t * t);
    return s.p[0] * (u * u * u) + s.p[1] * (3 * u * u * t) + s.p[2] * (3 * u * t * t) + s.p[3] * (t * t * t);
}

int flattenSteps(const Segment& s, double tolerance) {
    tolerance = std::max(tolerance, 1e-4);
    double n = 1;
    if (s.order == 2) {
        n = std::sqrt(len(s.p[0] - s.p[1] * 2 + s.p[2]) / (4 * tolerance));
    } else if (s.order == 3) {
        const double m = std::max(len(s.p[0] - s.p[1] * 2 + s.p[2]), len(s.p[1] - s.p[2] * 2 + s.p[3]));
        n = std::sqrt(0.75 * m / tolerance);
    }
    return std::clamp(static_cast<int>(std::ceil(n)), 1, 500);
}

Polygon flatten(const Contour& contour, double tolerance) {
    Polygon out;
    for (const auto& s : segments(contour)) {
        out.push_back(s.p[0]);
        if (s.order > 1) {
            const int n = flattenSteps(s, tolerance);
            for (int i = 1; i < n; ++i) out.push_back(evalSegment(s, static_cast<double>(i) / n));
        }
    }
    return out;
}

std::vector<Polygon> flatten(const std::vector<Contour>& contours, double tolerance) {
    std::vector<Polygon> out;
    out.reserve(contours.size());
    for (const auto& c : contours) out.push_back(flatten(c, tolerance));
    return out;
}

namespace {

// Roots in (0, 1) of a*t^2 + b*t + c.
void quadRoots(double a, double b, double c, std::vector<double>& out) {
    if (std::abs(a) < 1e-12) {
        if (std::abs(b) > 1e-12) out.push_back(-c / b);
        return;
    }
    const double disc = b * b - 4 * a * c;
    if (disc < 0) return;
    const double sq = std::sqrt(disc);
    out.push_back((-b + sq) / (2 * a));
    out.push_back((-b - sq) / (2 * a));
}

}  // namespace

Box bounds(const std::vector<Contour>& contours) {
    Box b;
    for (const auto& c : contours) {
        for (const auto& s : segments(c)) {
            b.add(s.p[0]);
            b.add(s.end());
            std::vector<double> ts;
            if (s.order == 2) {
                for (int axis = 0; axis < 2; ++axis) {
                    const double p0 = axis ? s.p[0].y : s.p[0].x, p1 = axis ? s.p[1].y : s.p[1].x, p2 = axis ? s.p[2].y : s.p[2].x;
                    const double den = p0 - 2 * p1 + p2;
                    if (std::abs(den) > 1e-12) ts.push_back((p0 - p1) / den);
                }
            } else if (s.order == 3) {
                for (int axis = 0; axis < 2; ++axis) {
                    double p[4];
                    for (int k = 0; k < 4; ++k) p[k] = axis ? s.p[k].y : s.p[k].x;
                    quadRoots(3 * (-p[0] + 3 * p[1] - 3 * p[2] + p[3]), 6 * (p[0] - 2 * p[1] + p[2]), 3 * (p[1] - p[0]), ts);
                }
            }
            for (double t : ts) {
                if (t > 0 && t < 1) b.add(evalSegment(s, t));
            }
        }
    }
    return b;
}

void transform(Contour& contour, const Affine& m) {
    for (auto& p : contour.points) {
        Vec q = m.apply({p.x, p.y});
        p.x = q.x;
        p.y = q.y;
    }
    if (m.a * m.d - m.b * m.c < 0) reverse(contour);  // a mirror flips the direction back
}

void transform(std::vector<Contour>& contours, const Affine& m) {
    for (auto& c : contours) transform(c, m);
}

namespace {

void outlineInto(const Font& font, const Glyph& g, const Affine& m, int depth, std::vector<Contour>& out) {
    for (auto c : g.contours) {
        if (!m.identity()) transform(c, m);
        out.push_back(std::move(c));
    }
    if (depth > 8) return;
    for (const auto& comp : g.components) {
        const Glyph* base = nullptr;
        if (int i = font.index(comp.base); i >= 0) base = &font.glyphs[static_cast<std::size_t>(i)];
        if (!base || base == &g) continue;
        outlineInto(font, *base, m * componentMatrix(comp), depth + 1, out);
    }
}

}  // namespace

Affine componentMatrix(const Component& c) { return {c.xx, c.xy, c.yx, c.yy, c.dx, c.dy}; }

std::vector<Contour> outline(const Font& font, const Glyph& glyph) {
    std::vector<Contour> out;
    outlineInto(font, glyph, {}, 0, out);
    return out;
}

Box glyphBounds(const Font& font, const Glyph& glyph) { return bounds(outline(font, glyph)); }

double signedArea(const Polygon& p) {
    double a = 0;
    for (std::size_t i = 0, n = p.size(); i < n; ++i) {
        const Vec& u = p[i];
        const Vec& v = p[(i + 1) % n];
        a += u.x * v.y - v.x * u.y;
    }
    return a / 2;
}

double signedArea(const Contour& c) { return signedArea(flatten(c, 0.25)); }

void reverse(Contour& c) {
    if (c.points.size() < 2) return;
    std::reverse(c.points.begin(), c.points.end());
    // keep an on-curve point first: rotate the last on-curve point (the old first) to the front
    auto it = std::find_if(c.points.begin(), c.points.end(), [](const Point& p) { return p.on(); });
    if (it == c.points.end()) return;
    auto last = std::find_if(c.points.rbegin(), c.points.rend(), [](const Point& p) { return p.on(); });
    std::rotate(c.points.begin(), std::prev(last.base()), c.points.end());
}

bool pointInPolygon(const Polygon& poly, Vec p) {
    bool inside = false;
    for (std::size_t i = 0, n = poly.size(), j = n - 1; i < n; j = i++) {
        const Vec& a = poly[i];
        const Vec& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

int fixDirections(std::vector<Contour>& contours) {
    const auto polys = flatten(contours, 0.5);
    int changed = 0;
    for (std::size_t i = 0; i < contours.size(); ++i) {
        const Polygon& pi = polys[i];
        if (pi.size() < 3) continue;
        int votes = 0, samples = 0;
        const std::size_t step = std::max<std::size_t>(1, pi.size() / 5);
        for (std::size_t k = 0; k < pi.size() && samples < 5; k += step, ++samples) {
            int depth = 0;
            for (std::size_t j = 0; j < contours.size(); ++j) {
                if (j != i && polys[j].size() >= 3 && pointInPolygon(polys[j], pi[k])) ++depth;
            }
            votes += depth % 2 ? 1 : -1;
        }
        const bool hole = votes > 0;
        const double area = signedArea(pi);
        if ((hole && area < 0) || (!hole && area > 0)) {
            reverse(contours[i]);
            ++changed;
        }
    }
    return changed;
}

// ---- quadratic <-> cubic ----

namespace {

Vec cubicAt(const Vec p[4], double t) {
    Segment s{3, {p[0], p[1], p[2], p[3]}};
    return evalSegment(s, t);
}

Vec cubicDeriv(const Vec p[4], double t) {
    const double u = 1 - t;
    return (p[1] - p[0]) * (3 * u * u) + (p[2] - p[1]) * (6 * u * t) + (p[3] - p[2]) * (3 * t * t);
}

}  // namespace

Contour toQuadratic(const Contour& contour, double tolerance) {
    std::vector<Segment> out;
    bool any = false;
    for (const auto& s : segments(contour)) {
        if (s.order != 3) {
            out.push_back(s);
            continue;
        }
        any = true;
        const double e1 = std::sqrt(3.0) / 36 * len(s.p[3] - s.p[2] * 3 + s.p[1] * 3 - s.p[0]);
        const int n = std::clamp(static_cast<int>(std::ceil(std::cbrt(e1 / std::max(tolerance, 1e-3)))), 1, 64);
        for (int i = 0; i < n; ++i) {
            const double t0 = static_cast<double>(i) / n, t1 = static_cast<double>(i + 1) / n, h = (t1 - t0) / 3;
            const Vec a = i == 0 ? s.p[0] : cubicAt(s.p, t0);
            const Vec d = i == n - 1 ? s.p[3] : cubicAt(s.p, t1);
            const Vec b = a + cubicDeriv(s.p, t0) * h;
            const Vec c = d - cubicDeriv(s.p, t1) * h;
            const Vec q = ((b + c) * 3 - a - d) * 0.25;
            out.push_back({2, {a, q, d}});
        }
    }
    if (!any) return contour;
    return fromSegments(out);
}

Contour toCubic(const Contour& contour) {
    std::vector<Segment> out;
    bool any = false;
    for (const auto& s : segments(contour)) {
        if (s.order != 2) {
            out.push_back(s);
            continue;
        }
        any = true;
        out.push_back({3, {s.p[0], s.p[0] + (s.p[1] - s.p[0]) * (2.0 / 3), s.p[2] + (s.p[1] - s.p[2]) * (2.0 / 3), s.p[2]}});
    }
    if (!any) return contour;
    return fromSegments(out);
}

// ---- SVG path data ----

namespace {

class PathLexer {
public:
    explicit PathLexer(const std::string& d) : d_(d) {}
    void skip() {
        while (i_ < d_.size() && (std::isspace(static_cast<unsigned char>(d_[i_])) || d_[i_] == ',')) ++i_;
    }
    bool done() {
        skip();
        return i_ >= d_.size();
    }
    bool command(char& c) {
        skip();
        if (i_ < d_.size() && std::isalpha(static_cast<unsigned char>(d_[i_])) && d_[i_] != 'e' && d_[i_] != 'E') {
            c = d_[i_++];
            return true;
        }
        return false;
    }
    bool atNumber() {
        skip();
        if (i_ >= d_.size()) return false;
        const char c = d_[i_];
        return std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.';
    }
    bool number(double& v) {
        if (!atNumber()) return false;
        const std::size_t start = i_;
        if (d_[i_] == '-' || d_[i_] == '+') ++i_;
        bool dot = false;
        while (i_ < d_.size() && (std::isdigit(static_cast<unsigned char>(d_[i_])) || (d_[i_] == '.' && !dot))) {
            if (d_[i_] == '.') dot = true;
            ++i_;
        }
        if (i_ < d_.size() && (d_[i_] == 'e' || d_[i_] == 'E')) {
            std::size_t j = i_ + 1;
            if (j < d_.size() && (d_[j] == '-' || d_[j] == '+')) ++j;
            if (j < d_.size() && std::isdigit(static_cast<unsigned char>(d_[j]))) {
                i_ = j;
                while (i_ < d_.size() && std::isdigit(static_cast<unsigned char>(d_[i_]))) ++i_;
            }
        }
        const std::string text = d_.substr(start, i_ - start);
        if (text == "-" || text == "+" || text == "." || text.empty()) return false;
        v = std::strtod(text.c_str(), nullptr);
        return true;
    }
    bool flag(double& v) {
        skip();
        if (i_ < d_.size() && (d_[i_] == '0' || d_[i_] == '1')) {
            v = d_[i_++] - '0';
            return true;
        }
        return false;
    }

private:
    const std::string& d_;
    std::size_t i_ = 0;
};

void arcToCubics(Vec p0, double rx, double ry, double phiDeg, bool large, bool sweep, Vec p1, std::vector<Segment>& out) {
    if (p0 == p1) return;
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx < 1e-12 || ry < 1e-12) {
        out.push_back({1, {p0, p1}});
        return;
    }
    const double phi = phiDeg * kPi / 180, cs = std::cos(phi), sn = std::sin(phi);
    const double dx = (p0.x - p1.x) / 2, dy = (p0.y - p1.y) / 2;
    const double x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
    double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double k = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
    if (large == sweep) k = -k;
    const double cxp = k * rx * y1 / ry, cyp = -k * ry * x1 / rx;
    const double cx = cs * cxp - sn * cyp + (p0.x + p1.x) / 2, cy = sn * cxp + cs * cyp + (p0.y + p1.y) / 2;
    auto angle = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    const double th1 = angle(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
    double dth = angle((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
    if (!sweep && dth > 0) dth -= 2 * kPi;
    if (sweep && dth < 0) dth += 2 * kPi;
    const int n = std::max(1, static_cast<int>(std::ceil(std::abs(dth) / (kPi / 2) - 1e-9)));
    const double step = dth / n, kk = 4.0 / 3 * std::tan(step / 4);
    auto pt = [&](double t) {
        return Vec{cx + rx * std::cos(t) * cs - ry * std::sin(t) * sn, cy + rx * std::cos(t) * sn + ry * std::sin(t) * cs};
    };
    auto dpt = [&](double t) {
        return Vec{-rx * std::sin(t) * cs - ry * std::cos(t) * sn, -rx * std::sin(t) * sn + ry * std::cos(t) * cs};
    };
    Vec a = p0;
    for (int i = 0; i < n; ++i) {
        const double t0 = th1 + step * i, t1 = t0 + step;
        const Vec b = i == n - 1 ? p1 : pt(t1);
        out.push_back({3, {a, a + dpt(t0) * kk, b - dpt(t1) * kk, b}});
        a = b;
    }
}

}  // namespace

std::vector<Contour> parsePathData(const std::string& d, std::string* error) {
    std::vector<Contour> out;
    PathLexer lx(d);
    std::vector<Segment> sub;
    Vec cur, start, lastCubic, lastQuad;
    char prevCmd = 0;
    auto closeSub = [&] {
        if (!sub.empty()) {
            if (!(sub.back().end() == start) && len(sub.back().end() - start) > 1e-9) sub.push_back({1, {cur, start}});
            else sub.back().p[sub.back().order] = start;
            if (sub.size() >= 2) out.push_back(fromSegments(sub));
        }
        sub.clear();
        cur = start;
    };
    char cmd = 0;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        closeSub();
        return out;
    };
    while (!lx.done()) {
        char c;
        if (lx.command(c)) {
            cmd = c;
        } else if (!cmd) {
            return fail("path data must start with a command such as M");
        } else if (cmd == 'M') {
            cmd = 'L';
        } else if (cmd == 'm') {
            cmd = 'l';
        } else if (cmd == 'Z' || cmd == 'z') {
            return fail("numbers after Z");
        }
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        auto rd = [&](double& v) { return lx.number(v); };
        auto pnt = [&](Vec& v) {
            if (!rd(v.x) || !rd(v.y)) return false;
            if (rel) v = v + cur;
            return true;
        };
        if (up == 'Z') {
            closeSub();
            prevCmd = 'Z';
            continue;
        }
        if (up == 'M') {
            Vec p;
            if (!pnt(p)) return fail("M needs x y");
            closeSub();
            cur = start = p;
        } else if (up == 'L') {
            Vec p;
            if (!pnt(p)) return fail("L needs x y");
            sub.push_back({1, {cur, p}});
            cur = p;
        } else if (up == 'H' || up == 'V') {
            double v;
            if (!rd(v)) return fail(std::string(1, cmd) + " needs a number");
            Vec p = cur;
            if (up == 'H') p.x = rel ? cur.x + v : v;
            else p.y = rel ? cur.y + v : v;
            sub.push_back({1, {cur, p}});
            cur = p;
        } else if (up == 'C' || up == 'S') {
            Vec c1, c2, p;
            if (up == 'C') {
                if (!pnt(c1)) return fail("C needs 6 numbers");
            } else {
                c1 = (prevCmd == 'C' || prevCmd == 'S') ? cur * 2 - lastCubic : cur;
            }
            if (!pnt(c2) || !pnt(p)) return fail(std::string(1, cmd) + " needs more numbers");
            sub.push_back({3, {cur, c1, c2, p}});
            lastCubic = c2;
            cur = p;
        } else if (up == 'Q' || up == 'T') {
            Vec c1, p;
            if (up == 'Q') {
                if (!pnt(c1)) return fail("Q needs 4 numbers");
            } else {
                c1 = (prevCmd == 'Q' || prevCmd == 'T') ? cur * 2 - lastQuad : cur;
            }
            if (!pnt(p)) return fail(std::string(1, cmd) + " needs more numbers");
            sub.push_back({2, {cur, c1, p}});
            lastQuad = c1;
            cur = p;
        } else if (up == 'A') {
            double rx, ry, rot, large, sweep;
            Vec p;
            if (!rd(rx) || !rd(ry) || !rd(rot) || !lx.flag(large) || !lx.flag(sweep) || !pnt(p)) return fail("A needs 7 numbers");
            arcToCubics(cur, rx, ry, rot, large != 0, sweep != 0, p, sub);
            cur = p;
        } else {
            return fail(std::string("unknown path command '") + cmd + "'");
        }
        prevCmd = up;
    }
    closeSub();
    return out;
}

std::string fmt(double v) {
    if (std::abs(v - std::round(v)) < 1e-6) return std::to_string(static_cast<long long>(std::llround(v)));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

std::string pathData(const std::vector<Contour>& contours, const Affine& m) {
    std::string out;
    auto p = [&](Vec v) {
        v = m.apply(v);
        return fmt(v.x) + " " + fmt(v.y);
    };
    for (const auto& c : contours) {
        const auto segs = segments(c);
        if (segs.empty()) continue;
        if (!out.empty()) out += ' ';
        out += "M" + p(segs[0].p[0]);
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const auto& s = segs[i];
            if (s.order == 1) {
                if (i + 1 == segs.size()) break;  // Z draws the closing line
                out += "L" + p(s.p[1]);
            } else if (s.order == 2) {
                out += "Q" + p(s.p[1]) + " " + p(s.p[2]);
            } else {
                out += "C" + p(s.p[1]) + " " + p(s.p[2]) + " " + p(s.p[3]);
            }
        }
        out += "Z";
    }
    return out;
}

// ---- shapes ----

Contour polygonContour(const Polygon& points) {
    Contour c;
    for (const auto& p : points) c.points.push_back({p.x, p.y, PointType::On});
    return c;
}

Contour rectContour(double x, double y, double w, double h) {
    return polygonContour({{x, y}, {x, y + h}, {x + w, y + h}, {x + w, y}});
}

Contour ellipseContour(double cx, double cy, double rx, double ry) {
    const double k = 0.5522847498;
    std::vector<Segment> s;
    const Vec l{cx - rx, cy}, t{cx, cy + ry}, r{cx + rx, cy}, b{cx, cy - ry};
    s.push_back({3, {l, {l.x, l.y + ry * k}, {t.x - rx * k, t.y}, t}});
    s.push_back({3, {t, {t.x + rx * k, t.y}, {r.x, r.y + ry * k}, r}});
    s.push_back({3, {r, {r.x, r.y - ry * k}, {b.x + rx * k, b.y}, b}});
    s.push_back({3, {b, {b.x - rx * k, b.y}, {l.x, l.y - ry * k}, l}});
    return fromSegments(s);
}

Contour smoothThrough(const Polygon& pts) {
    const std::size_t n = pts.size();
    if (n < 3) return polygonContour(pts);
    std::vector<Segment> s;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec& p0 = pts[(i + n - 1) % n];
        const Vec& p1 = pts[i];
        const Vec& p2 = pts[(i + 1) % n];
        const Vec& p3 = pts[(i + 2) % n];
        s.push_back({3, {p1, p1 + (p2 - p0) * (1.0 / 6), p2 - (p3 - p1) * (1.0 / 6), p2}});
    }
    return fromSegments(s);
}

}  // namespace achclwss::typeface
