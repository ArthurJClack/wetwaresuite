// Strokes as filled areas: every segment becomes a quad, every join a wedge or
// disc, every cap and arrowhead its own piece. The pieces all wind the same
// way, so filling them nonzero (or merging them with a nonzero boolean) gives
// the stroke's exact area, whatever the width profile or dash pattern.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::vector {

namespace {

constexpr double kPi = 3.14159265358979323846;

Point add(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
Point sub(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
Point mul(Point a, double k) { return {a.x * k, a.y * k}; }
double dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }
double cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }
double dist(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }
Point unit(Point a) {
    const double l = std::hypot(a.x, a.y);
    return l > 1e-12 ? mul(a, 1 / l) : Point{0, 0};
}
Point normal(Point u) { return {-u.y, u.x}; }

void piece(Outline& out, std::vector<Point> pts) {
    if (pts.size() < 3) return;
    if (signedArea(pts) < 0) std::reverse(pts.begin(), pts.end());
    out.push_back({std::move(pts), true});
}

void disc(Outline& out, Point c, double r) {
    if (r <= 1e-9) return;
    const int n = std::clamp(static_cast<int>(std::ceil(r * 1.5)) + 8, 8, 96);
    std::vector<Point> pts;
    pts.reserve(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        const double t = 2 * kPi * k / n;
        pts.push_back({c.x + r * std::cos(t), c.y + r * std::sin(t)});
    }
    piece(out, std::move(pts));
}

double profileAt(const std::string& profile, double t) {
    t = std::clamp(t, 0.0, 1.0);
    double f = 1;
    if (profile == "taper") f = std::sin(kPi * t);
    else if (profile == "taper_start") f = t;
    else if (profile == "taper_end") f = 1 - t;
    else if (profile == "bulge") f = 0.4 + 0.6 * std::sin(kPi * t);
    else if (profile == "pinch") f = 1 - 0.6 * std::sin(kPi * t);
    return std::max(f, 0.0);
}

struct Pt {
    Point p;
    double t;  // fraction of the way along the whole line, for the width profile
};

// The "on" stretches of a dash pattern.
std::vector<std::vector<Pt>> dashes(const std::vector<Pt>& line, bool closed, const std::vector<double>& pattern, double offset) {
    std::vector<double> pat;
    for (double v : pattern) pat.push_back(std::max(0.0, v));
    if (pat.size() % 2) pat.insert(pat.end(), pat.begin(), pat.end());
    double period = 0;
    for (double v : pat) period += v;
    std::vector<std::vector<Pt>> out;
    if (period <= 1e-9) {
        out.push_back(line);
        return out;
    }
    std::vector<Pt> pts = line;
    if (closed && !pts.empty()) pts.push_back({line.front().p, 1.0});
    // where in the pattern the line starts
    double phase = std::fmod(offset, period);
    if (phase < 0) phase += period;
    std::size_t k = 0;
    while (phase >= pat[k]) {
        phase -= pat[k];
        k = (k + 1) % pat.size();
    }
    double left = pat[k] - phase;  // of the current dash or gap
    bool on = k % 2 == 0;
    std::vector<Pt> cur;
    if (on && !pts.empty()) cur.push_back(pts[0]);
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        Pt a = pts[i];
        const Pt b = pts[i + 1];
        double segLeft = dist(a.p, b.p);
        while (segLeft > left) {
            const double f = left / segLeft;
            const Pt m{add(a.p, mul(sub(b.p, a.p), f)), a.t + (b.t - a.t) * f};
            if (on) {
                cur.push_back(m);
                if (cur.size() >= 2) out.push_back(cur);
                cur.clear();
            } else {
                cur = {m};
            }
            segLeft -= left;
            a = m;
            k = (k + 1) % pat.size();
            left = pat[k];
            on = !on;
        }
        left -= segLeft;
        if (on) cur.push_back(b);
    }
    if (on && cur.size() >= 2) out.push_back(cur);
    return out;
}

void arrow(Outline& out, const std::string& kind, Point end, Point u, double width, double scale) {
    if (kind.empty() || kind == "none") return;
    const double hw = width / 2, L = std::max(1.0, width * 4 * scale);
    const Point n = normal(u);
    const Point tip = add(end, mul(u, hw));
    if (kind == "triangle") {
        const Point base = sub(tip, mul(u, L));
        piece(out, {tip, add(base, mul(n, L / 2)), sub(base, mul(n, L / 2))});
    } else if (kind == "open") {
        const Point base = sub(tip, mul(u, L));
        for (double side : {1.0, -1.0}) {
            const Point wing = add(base, mul(n, side * L / 2));
            const Point d = unit(sub(wing, tip)), m = normal(d);
            piece(out, {add(tip, mul(m, hw)), add(wing, mul(m, hw)), sub(wing, mul(m, hw)), sub(tip, mul(m, hw))});
            disc(out, wing, hw);
        }
        disc(out, tip, hw);
    } else if (kind == "circle") {
        disc(out, end, L * 0.4);
    } else if (kind == "square") {
        const double h = L * 0.35;
        piece(out, {add(add(end, mul(u, h)), mul(n, h)), add(sub(end, mul(u, h)), mul(n, h)), sub(sub(end, mul(u, h)), mul(n, h)),
                    sub(add(end, mul(u, h)), mul(n, h))});
    } else if (kind == "bar") {
        piece(out, {add(add(end, mul(u, hw)), mul(n, L / 2)), add(sub(end, mul(u, hw)), mul(n, L / 2)), sub(sub(end, mul(u, hw)), mul(n, L / 2)),
                    sub(add(end, mul(u, hw)), mul(n, L / 2))});
    }
}

// The direction a line leaves an end, looking back far enough to skip tiny steps.
Point endDirection(const std::vector<Point>& pts, bool atEnd, double reach) {
    const std::size_t n = pts.size();
    const Point e = atEnd ? pts[n - 1] : pts[0];
    for (std::size_t k = 1; k < n; ++k) {
        const Point q = atEnd ? pts[n - 1 - k] : pts[k];
        if (dist(q, e) >= reach || k + 1 == n) return unit(sub(e, q));
    }
    return {1, 0};
}

void strokeRun(Outline& out, const std::vector<Pt>& pts, bool closed, double hw, const StrokeStyle& st) {
    const std::size_t n = pts.size();
    auto width = [&](const Pt& p) { return hw * profileAt(st.profile, p.t); };
    if (n == 1) return;
    const std::size_t segs = closed ? n : n - 1;
    std::vector<Point> dirs(segs);
    for (std::size_t i = 0; i < segs; ++i) dirs[i] = unit(sub(pts[(i + 1) % n].p, pts[i].p));
    for (std::size_t i = 0; i < segs; ++i) {
        const Pt& a = pts[i];
        const Pt& b = pts[(i + 1) % n];
        if (dist(a.p, b.p) < 1e-9) continue;
        const Point nr = normal(dirs[i]);
        const double ha = width(a), hb = width(closed && i + 1 == n ? Pt{b.p, 1.0} : b);
        piece(out, {add(a.p, mul(nr, ha)), add(b.p, mul(nr, hb)), sub(b.p, mul(nr, hb)), sub(a.p, mul(nr, ha))});
    }
    // joins
    const double cosSmall = std::cos(20 * kPi / 180);
    for (std::size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); ++i) {
        const Point u1 = dirs[(i + segs - 1) % segs], u2 = dirs[i % segs];
        if ((u1.x == 0 && u1.y == 0) || (u2.x == 0 && u2.y == 0)) continue;
        const double c = dot(u1, u2);
        if (std::abs(cross(u1, u2)) < 1e-12 && c > 0) continue;
        const Point p = pts[i].p;
        const double h = width(pts[i]);
        if (h <= 1e-9) continue;
        const Point n1 = normal(u1), n2 = normal(u2);
        const double s = dot(n1, u2) > 0 ? -1.0 : 1.0;  // the outer side
        const bool small = c > cosSmall;
        if (st.join == "round" && !small) {
            disc(out, p, h);
            continue;
        }
        const double denom = 1 + dot(n1, n2);
        const double ratio = denom > 1e-9 ? std::sqrt(2 / denom) : 1e9;
        const Point o1 = add(p, mul(n1, s * h)), o2 = add(p, mul(n2, s * h));
        if ((st.join == "miter" || small) && ratio <= std::max(1.0, st.miterLimit)) {
            const Point tip = add(p, mul(add(n1, n2), s * h / denom));
            piece(out, {p, o1, tip, o2});
        } else {
            piece(out, {p, o1, o2});
        }
    }
    if (closed) return;
    // caps
    for (int end = 0; end < 2; ++end) {
        const Pt& e = end ? pts[n - 1] : pts[0];
        const Point u = end ? dirs[segs - 1] : mul(dirs[0], -1);  // pointing out of the line
        const double h = width(e);
        if (h <= 1e-9) continue;
        if (st.cap == "round") disc(out, e.p, h);
        else if (st.cap == "square") {
            const Point nr = normal(u);
            piece(out, {add(e.p, mul(nr, h)), add(add(e.p, mul(nr, h)), mul(u, h)), add(sub(e.p, mul(nr, h)), mul(u, h)), sub(e.p, mul(nr, h))});
        }
    }
}

}  // namespace

bool StrokeStyle::plain() const {
    return cap == "round" && join == "round" && dash.empty() && align == "center" && startArrow == "none" && endArrow == "none" &&
           profile == "uniform";
}

double signedArea(const std::vector<Point>& pts) {
    double a = 0;
    for (std::size_t i = 0, n = pts.size(); i < n; ++i) {
        const Point& p = pts[i];
        const Point& q = pts[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2;
}

bool insideOutline(const Outline& o, Point p, bool nonzero) {
    int wind = 0;
    for (const auto& c : o) {
        const auto& pts = c.points;
        for (std::size_t i = 0, n = pts.size(); i < n && n >= 3; ++i) {
            const Point& a = pts[i];
            const Point& b = pts[(i + 1) % n];
            if ((a.y > p.y) != (b.y > p.y)) {
                const double x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (x > p.x) wind += b.y > a.y ? 1 : -1;
            }
        }
    }
    return nonzero ? wind != 0 : (wind & 1) != 0;
}

Outline strokeArea(const Outline& line, double width, const StrokeStyle& style) {
    Outline out;
    if (!(width > 0)) return out;
    const double hw = width / 2;
    for (const auto& c : line) {
        std::vector<Point> pts;
        for (const auto& p : c.points) {
            if (pts.empty() || dist(pts.back(), p) > 1e-9) pts.push_back(p);
        }
        bool closed = c.closed;
        if (closed && pts.size() > 1 && dist(pts.front(), pts.back()) <= 1e-9) pts.pop_back();
        if (closed && pts.size() < 3) closed = false;
        if (pts.size() == 1) {
            if (style.cap == "round") disc(out, pts[0], hw);
            else if (style.cap == "square") piece(out, {{pts[0].x - hw, pts[0].y - hw}, {pts[0].x + hw, pts[0].y - hw}, {pts[0].x + hw, pts[0].y + hw}, {pts[0].x - hw, pts[0].y + hw}});
            continue;
        }
        if (pts.empty()) continue;
        // arc-length fractions for the width profile
        std::vector<Pt> run;
        double total = 0;
        for (std::size_t i = 1; i < pts.size(); ++i) total += dist(pts[i - 1], pts[i]);
        if (closed) total += dist(pts.back(), pts.front());
        double acc = 0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (i) acc += dist(pts[i - 1], pts[i]);
            run.push_back({pts[i], total > 0 ? acc / total : 0});
        }
        if (!closed) {
            arrow(out, style.startArrow, pts.front(), endDirection(pts, false, hw), width, style.arrowScale);
            arrow(out, style.endArrow, pts.back(), endDirection(pts, true, hw), width, style.arrowScale);
        }
        if (style.dash.empty()) {
            strokeRun(out, run, closed, hw, style);
        } else {
            for (const auto& d : dashes(run, closed, style.dash, style.dashOffset)) strokeRun(out, d, false, hw, style);
        }
    }
    return out;
}

Outline outlineStroke(const Outline& line, double width, const StrokeStyle& style) {
    if (style.align == "inside" || style.align == "outside") {
        Outline area;
        for (const auto& c : line) {
            if (c.closed && c.points.size() >= 3) area.push_back(c);
        }
        StrokeStyle twice = style;
        twice.align = "center";
        Outline pieces = strokeArea(line, width * 2, twice);
        if (area.empty()) return booleanOp(pieces, {}, BoolOp::Union, true, false);
        return booleanOp(pieces, area, style.align == "inside" ? BoolOp::Intersect : BoolOp::Subtract, true, false);
    }
    return booleanOp(strokeArea(line, width, style), {}, BoolOp::Union, true, false);
}

Outline offsetArea(const Outline& area, double distance, const std::string& join, double miterLimit) {
    Outline closed;
    for (const auto& c : area) {
        if (c.points.size() >= 3) closed.push_back({c.points, true});
    }
    if (closed.empty() || std::abs(distance) < 1e-9) return closed;
    StrokeStyle st;
    st.cap = "butt";
    st.join = join;
    st.miterLimit = miterLimit;
    const Outline ring = strokeArea(closed, std::abs(distance) * 2, st);
    return booleanOp(closed, ring, distance > 0 ? BoolOp::Union : BoolOp::Subtract, false, true);
}

}  // namespace wetwaresuite::vector
