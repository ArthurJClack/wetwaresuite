// Boolean operations on outlines (union, subtract, intersect, exclude).
//
// Every edge of both outlines is split where it crosses another. Each piece is
// then kept when the result's inside is on exactly one side of it (tested a
// hair to its left and right with an even-odd count against both inputs), and
// turned so the inside is on its left. The kept pieces join into loops.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::vector {

namespace {

struct Edge {
    Point a, b;
    int src;  // 0 = first outline, 1 = second
};

double cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }
Point sub(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
double dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }

std::vector<Edge> edgesOf(const Outline& o, int src) {
    std::vector<Edge> out;
    for (const auto& c : o) {
        const auto& p = c.points;
        if (p.size() < 3) continue;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const Point a = p[i], b = p[(i + 1) % p.size()];
            if (std::hypot(b.x - a.x, b.y - a.y) > 1e-9) out.push_back({a, b, src});
        }
    }
    return out;
}

// Even-odd: is p inside the edges that came from `src`?
bool inside(const std::vector<Edge>& edges, int src, Point p) {
    bool in = false;
    for (const auto& e : edges) {
        if (e.src != src) continue;
        if ((e.a.y > p.y) != (e.b.y > p.y)) {
            const double x = e.a.x + (p.y - e.a.y) / (e.b.y - e.a.y) * (e.b.x - e.a.x);
            if (x > p.x) in = !in;
        }
    }
    return in;
}

bool apply(BoolOp op, bool a, bool b) {
    switch (op) {
        case BoolOp::Union: return a || b;
        case BoolOp::Subtract: return a && !b;
        case BoolOp::Intersect: return a && b;
        case BoolOp::Exclude: return a != b;
    }
    return false;
}

struct Key {
    long long x, y;
    bool operator<(const Key& o) const { return x != o.x ? x < o.x : y < o.y; }
    bool operator==(const Key& o) const { return x == o.x && y == o.y; }
};

}  // namespace

Outline booleanOp(const Outline& a, const Outline& b, BoolOp op) {
    std::vector<Edge> edges = edgesOf(a, 0);
    const std::vector<Edge> eb = edgesOf(b, 1);
    edges.insert(edges.end(), eb.begin(), eb.end());
    if (edges.empty()) return {};
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const auto& e : edges) {
        minX = std::min({minX, e.a.x, e.b.x}), maxX = std::max({maxX, e.a.x, e.b.x});
        minY = std::min({minY, e.a.y, e.b.y}), maxY = std::max({maxY, e.a.y, e.b.y});
    }
    const double size = std::max({maxX - minX, maxY - minY, 1e-6});
    const double grid = size * 1e-7;  // points closer than this are the same point
    auto key = [&](Point p) { return Key{std::llround(p.x / grid), std::llround(p.y / grid)}; };

    // where each edge is cut, as positions 0..1 along it; a sweep over x skips pairs that can't meet
    std::vector<std::vector<double>> cuts(edges.size());
    std::vector<std::size_t> order(edges.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto lo = [&](std::size_t i) { return std::min(edges[i].a.x, edges[i].b.x); };
    auto hi = [&](std::size_t i) { return std::max(edges[i].a.x, edges[i].b.x); };
    std::sort(order.begin(), order.end(), [&](std::size_t i, std::size_t j) { return lo(i) < lo(j); });
    const double eps = 1e-12;
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        const std::size_t i = order[oi];
        const Edge& e = edges[i];
        for (std::size_t oj = oi + 1; oj < order.size() && lo(order[oj]) <= hi(i) + grid; ++oj) {
            const std::size_t j = order[oj];
            const Edge& f = edges[j];
            if (std::max(e.a.y, e.b.y) < std::min(f.a.y, f.b.y) - grid || std::max(f.a.y, f.b.y) < std::min(e.a.y, e.b.y) - grid) continue;
            const Point r = sub(e.b, e.a), s = sub(f.b, f.a);
            const double den = cross(r, s);
            const double rr = dot(r, r), ss = dot(s, s);
            if (std::fabs(den) <= 1e-12 * std::sqrt(rr * ss)) {
                // parallel: when on one line, each is cut where the other ends
                if (std::fabs(cross(r, sub(f.a, e.a))) > grid * std::sqrt(rr)) continue;
                for (Point q : {f.a, f.b}) {
                    const double t = dot(sub(q, e.a), r) / rr;
                    if (t > eps && t < 1 - eps) cuts[i].push_back(t);
                }
                for (Point q : {e.a, e.b}) {
                    const double t = dot(sub(q, f.a), s) / ss;
                    if (t > eps && t < 1 - eps) cuts[j].push_back(t);
                }
                continue;
            }
            const Point d = sub(f.a, e.a);
            const double t = cross(d, s) / den, u = cross(d, r) / den;
            if (t < -eps || t > 1 + eps || u < -eps || u > 1 + eps) continue;
            if (t > eps && t < 1 - eps) cuts[i].push_back(t);
            if (u > eps && u < 1 - eps) cuts[j].push_back(u);
        }
    }

    // the pieces that bound the result, turned so the inside is on the left (y down: left = clockwise side)
    std::map<std::pair<Key, Key>, std::pair<Point, Point>> kept;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        auto& c = cuts[i];
        c.push_back(0);
        c.push_back(1);
        std::sort(c.begin(), c.end());
        const Edge& e = edges[i];
        for (std::size_t k = 0; k + 1 < c.size(); ++k) {
            const Point p{e.a.x + (e.b.x - e.a.x) * c[k], e.a.y + (e.b.y - e.a.y) * c[k]};
            const Point q{e.a.x + (e.b.x - e.a.x) * c[k + 1], e.a.y + (e.b.y - e.a.y) * c[k + 1]};
            if (key(p) == key(q)) continue;
            const Point m{(p.x + q.x) / 2, (p.y + q.y) / 2};
            const double len = std::hypot(q.x - p.x, q.y - p.y);
            const double off = std::min(len * 0.01, size * 1e-5) + grid * 2;
            const Point n{-(q.y - p.y) / len * off, (q.x - p.x) / len * off};
            const Point left{m.x + n.x, m.y + n.y}, right{m.x - n.x, m.y - n.y};
            const bool inL = apply(op, inside(edges, 0, left), inside(edges, 1, left));
            const bool inR = apply(op, inside(edges, 0, right), inside(edges, 1, right));
            if (inL == inR) continue;
            const Point from = inL ? p : q, to = inL ? q : p;
            kept.emplace(std::make_pair(key(from), key(to)), std::make_pair(from, to));
        }
    }

    // join the pieces into loops
    std::multimap<Key, std::pair<Point, Point>> byStart;
    for (const auto& [k, seg] : kept) byStart.emplace(k.first, seg);
    Outline out;
    while (!byStart.empty()) {
        auto it = byStart.begin();
        Contour c{{it->second.first}, true};
        const Key first = it->first;
        Point prev = it->second.first, cur = it->second.second;
        byStart.erase(it);
        for (std::size_t guard = 0; guard < kept.size() + 1; ++guard) {
            const Key k = key(cur);
            if (k == first) break;
            c.points.push_back(cur);
            auto range = byStart.equal_range(k);
            if (range.first == range.second) break;  // open chain (shouldn't happen): keep what we have
            // where loops touch at a point, take the sharpest turn so each loop stays simple
            auto best = range.first;
            double bestAngle = 1e9;
            const Point din = sub(cur, prev);
            for (auto r = range.first; r != range.second; ++r) {
                const Point dout = sub(r->second.second, r->second.first);
                const double ang = std::atan2(cross(din, dout), dot(din, dout));
                if (ang < bestAngle) bestAngle = ang, best = r;
            }
            prev = cur;
            cur = best->second.second;
            byStart.erase(best);
        }
        // drop points in the middle of straight runs
        std::vector<Point> pts;
        const std::size_t n = c.points.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Point a = c.points[(i + n - 1) % n], p = c.points[i], b = c.points[(i + 1) % n];
            if (std::fabs(cross(sub(p, a), sub(b, p))) > grid * std::hypot(b.x - a.x, b.y - a.y) || dot(sub(p, a), sub(b, p)) < 0) pts.push_back(p);
        }
        if (pts.size() >= 3) out.push_back({std::move(pts), true});
    }
    return out;
}

std::string outlinePathData(const Outline& outline) {
    std::string d;
    char buf[64];
    auto n = [&](double v) {
        std::snprintf(buf, sizeof buf, "%.2f", v);
        std::string s = buf;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s == "-0" ? std::string("0") : s;
    };
    for (const auto& c : outline) {
        if (c.points.empty()) continue;
        for (std::size_t i = 0; i < c.points.size(); ++i) {
            d += (i == 0 ? "M" : "L") + n(c.points[i].x) + " " + n(c.points[i].y);
        }
        if (c.closed) d += "Z";
    }
    return d;
}

}  // namespace wetwaresuite::vector
