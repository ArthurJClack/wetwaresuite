// Outline operations: curve fitting (Schneider), emboldening and overlap removal.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "internal.hpp"

namespace wetwaresuite::typeface {

// ---- curve fitting ----

namespace {

class Fitter {
public:
    Fitter(const std::vector<Vec>& pts, double tolerance) : p_(pts), tol_(tolerance) {}

    std::vector<Segment> out;

    void run(std::size_t first, std::size_t last, Vec t1, Vec t2) { fit(first, last, t1, t2, 0); }

    // Straight within tolerance?
    bool straight(std::size_t first, std::size_t last) const {
        const Vec a = p_[first], b = p_[last];
        const Vec d = b - a;
        const double l = len(d);
        for (std::size_t i = first + 1; i < last; ++i) {
            const double dist = l > 1e-12 ? std::abs(cross(d, p_[i] - a)) / l : len(p_[i] - a);
            if (dist > tol_) return false;
        }
        return true;
    }

private:
    const std::vector<Vec>& p_;
    double tol_;

    static Vec bez(const Vec c[4], double t) {
        const Segment s{3, {c[0], c[1], c[2], c[3]}};
        return evalSegment(s, t);
    }

    std::vector<double> chordParams(std::size_t first, std::size_t last) const {
        std::vector<double> u(last - first + 1, 0.0);
        for (std::size_t i = first + 1; i <= last; ++i) u[i - first] = u[i - first - 1] + len(p_[i] - p_[i - 1]);
        const double total = u.back();
        for (auto& v : u) v = total > 0 ? v / total : 0;
        return u;
    }

    void generate(std::size_t first, std::size_t last, const std::vector<double>& u, Vec t1, Vec t2, Vec c[4]) const {
        const Vec p0 = p_[first], p3 = p_[last];
        double C00 = 0, C01 = 0, C11 = 0, X0 = 0, X1 = 0;
        for (std::size_t i = 0; i < u.size(); ++i) {
            const double t = u[i], s = 1 - t;
            const double b0 = s * s * s, b1 = 3 * s * s * t, b2 = 3 * s * t * t, b3 = t * t * t;
            const Vec a1 = t1 * b1, a2 = t2 * b2;
            C00 += dot(a1, a1);
            C01 += dot(a1, a2);
            C11 += dot(a2, a2);
            const Vec tmp = p_[first + i] - (p0 * (b0 + b1) + p3 * (b2 + b3));
            X0 += dot(a1, tmp);
            X1 += dot(a2, tmp);
        }
        const double det = C00 * C11 - C01 * C01;
        double al = 0, ar = 0;
        if (std::abs(det) > 1e-12) {
            al = (X0 * C11 - X1 * C01) / det;
            ar = (C00 * X1 - C01 * X0) / det;
        }
        const double segLen = len(p3 - p0), eps = 1e-6 * segLen;
        if (al < eps || ar < eps) al = ar = segLen / 3;
        c[0] = p0;
        c[1] = p0 + t1 * al;
        c[2] = p3 + t2 * ar;
        c[3] = p3;
    }

    double maxError(std::size_t first, std::size_t last, const Vec c[4], const std::vector<double>& u, std::size_t& split) const {
        double worst = 0;
        split = (first + last) / 2;
        for (std::size_t i = first + 1; i < last; ++i) {
            const Vec d = bez(c, u[i - first]) - p_[i];
            const double e = dot(d, d);
            if (e >= worst) {
                worst = e;
                split = i;
            }
        }
        return worst;
    }

    static double newton(const Vec c[4], Vec p, double t) {
        const Vec q = bez(c, t);
        const Vec d1[3] = {(c[1] - c[0]) * 3, (c[2] - c[1]) * 3, (c[3] - c[2]) * 3};
        const Vec d2[2] = {(d1[1] - d1[0]) * 2, (d1[2] - d1[1]) * 2};
        const double s = 1 - t;
        const Vec q1 = d1[0] * (s * s) + d1[1] * (2 * s * t) + d1[2] * (t * t);
        const Vec q2 = d2[0] * s + d2[1] * t;
        const double num = dot(q - p, q1), den = dot(q1, q1) + dot(q - p, q2);
        if (std::abs(den) < 1e-12) return t;
        return std::clamp(t - num / den, 0.0, 1.0);
    }

    void emit(const Vec c[4]) {
        // nearly straight cubics become lines
        const Vec d = c[3] - c[0];
        const double l = len(d);
        if (l > 1e-9 && std::abs(cross(d, c[1] - c[0])) / l < tol_ * 0.25 && std::abs(cross(d, c[2] - c[0])) / l < tol_ * 0.25 &&
            dot(c[1] - c[0], d) >= 0 && dot(c[2] - c[3], d) <= 0) {
            out.push_back({1, {c[0], c[3]}});
            return;
        }
        out.push_back({3, {c[0], c[1], c[2], c[3]}});
    }

    void fit(std::size_t first, std::size_t last, Vec t1, Vec t2, int depth) {
        const std::size_t n = last - first + 1;
        if (n == 2 || depth > 40) {
            const double dist = len(p_[last] - p_[first]) / 3;
            Vec c[4] = {p_[first], p_[first] + t1 * dist, p_[last] + t2 * dist, p_[last]};
            if (n == 2) out.push_back({1, {p_[first], p_[last]}});
            else emit(c);
            return;
        }
        if (straight(first, last)) {
            out.push_back({1, {p_[first], p_[last]}});
            return;
        }
        auto u = chordParams(first, last);
        Vec c[4];
        generate(first, last, u, t1, t2, c);
        std::size_t split = 0;
        double err = maxError(first, last, c, u, split);
        const double tol2 = tol_ * tol_;
        if (err < tol2) {
            emit(c);
            return;
        }
        if (err < tol2 * 16) {
            for (int it = 0; it < 6; ++it) {
                for (std::size_t i = 0; i < u.size(); ++i) u[i] = newton(c, p_[first + i], u[i]);
                generate(first, last, u, t1, t2, c);
                err = maxError(first, last, c, u, split);
                if (err < tol2) {
                    emit(c);
                    return;
                }
            }
        }
        split = std::clamp(split, first + 1, last - 1);
        Vec tc = normalize(p_[split - 1] - p_[split + 1]);
        if (len(tc) < 0.5) tc = normalize(p_[split - 1] - p_[split]);
        fit(first, split, t1, tc, depth + 1);
        fit(split, last, -tc, t2, depth + 1);
    }
};

// Index of the point `dist` along the polyline from i, walking by `dir` (cyclic).
std::size_t walk(const std::vector<Vec>& p, std::size_t i, int dir, double dist, std::size_t maxSteps) {
    const std::size_t n = p.size();
    double d = 0;
    std::size_t j = i;
    for (std::size_t s = 0; s < maxSteps; ++s) {
        const std::size_t k = dir > 0 ? (j + 1) % n : (j + n - 1) % n;
        d += len(p[k] - p[j]);
        j = k;
        if (d >= dist) break;
    }
    return j;
}

}  // namespace

Contour fitCurves(const Polygon& input, double tolerance, double cornerDegrees) {
    // drop repeated points
    std::vector<Vec> p;
    for (const auto& v : input) {
        if (p.empty() || len(v - p.back()) > 1e-9) p.push_back(v);
    }
    while (p.size() > 1 && len(p.front() - p.back()) <= 1e-9) p.pop_back();
    const std::size_t n = p.size();
    if (n < 4) return polygonContour(p);
    tolerance = std::max(tolerance, 1e-3);

    // corners: turning angle measured over a short arc on each side, kept at local maxima
    const double reach = tolerance * 3;
    const std::size_t steps = std::max<std::size_t>(1, n / 4);
    std::vector<double> turn(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec a = p[walk(p, i, -1, reach, steps)], b = p[walk(p, i, 1, reach, steps)];
        const Vec u = normalize(p[i] - a), v = normalize(b - p[i]);
        turn[i] = std::acos(std::clamp(dot(u, v), -1.0, 1.0)) * 180 / kPi;
    }
    std::vector<std::size_t> corners;
    for (std::size_t i = 0; i < n; ++i) {
        if (turn[i] < cornerDegrees) continue;
        // a local maximum among neighbours closer than the reach
        bool best = true;
        for (int dir : {-1, 1}) {
            double d = 0;
            std::size_t j = i;
            for (std::size_t s = 0; s < steps && best; ++s) {
                const std::size_t k = dir > 0 ? (j + 1) % n : (j + n - 1) % n;
                d += len(p[k] - p[j]);
                j = k;
                if (d >= reach || j == i) break;
                if (turn[j] > turn[i] || (turn[j] == turn[i] && j < i)) best = false;
            }
        }
        if (best) corners.push_back(i);
    }

    // rotate so a corner (or point 0) comes first, and close the ring with a copy of it
    const std::size_t start = corners.empty() ? 0 : corners[0];
    std::vector<Vec> ring;
    ring.reserve(n + 1);
    for (std::size_t i = 0; i <= n; ++i) ring.push_back(p[(start + i) % n]);
    std::vector<std::size_t> cuts;
    if (corners.empty()) {
        cuts = {0, n / 2, n};
    } else {
        for (auto c : corners) cuts.push_back((c + n - start) % n);
        std::sort(cuts.begin(), cuts.end());
        cuts.push_back(n);
    }
    Fitter fitter(ring, tolerance);
    auto tangentAt = [&](std::size_t i, int dir, std::size_t lo, std::size_t hi) {
        // one-sided tangent from ring[i] toward its neighbours inside [lo, hi]
        double d = 0;
        std::size_t j = i;
        while (true) {
            const std::size_t k = dir > 0 ? j + 1 : j - 1;
            if (k < lo || k > hi) break;
            d += len(ring[k] - ring[j]);
            j = k;
            if (d >= reach) break;
        }
        return normalize(ring[j] - ring[i]);
    };
    for (std::size_t k = 0; k + 1 < cuts.size(); ++k) {
        const std::size_t a = cuts[k], b = cuts[k + 1];
        if (b <= a) continue;
        Vec t1, t2;
        if (corners.empty()) {
            // smooth joins: central tangents
            auto central = [&](std::size_t i) {
                const Vec prev = ring[(i + n - 1) % n], next = ring[(i + 1) % n];
                return normalize(next - prev);
            };
            t1 = central(a % n);
            t2 = -central(b % n);
        } else {
            t1 = tangentAt(a, 1, a, b);
            t2 = tangentAt(b, -1, a, b);
        }
        if (len(t1) < 0.5) t1 = normalize(ring[b] - ring[a]);
        if (len(t2) < 0.5) t2 = normalize(ring[a] - ring[b]);
        fitter.run(a, b, t1, t2);
    }
    if (fitter.out.empty()) return polygonContour(p);
    return fromSegments(fitter.out);
}

// ---- emboldening ----

std::vector<Contour> embolden(const std::vector<Contour>& contours, double dx, double dy) {
    double total = 0;
    for (const auto& c : contours) total += signedArea(c);
    const bool inkRight = total <= 0;  // clockwise outer contours (TrueType)
    std::vector<Contour> out = contours;
    for (auto& c : out) {
        auto& pts = c.points;
        const std::size_t n = pts.size();
        if (n < 2) continue;
        std::vector<Point> moved = pts;
        auto at = [&](std::size_t i) { return Vec{pts[i].x, pts[i].y}; };
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t j = (i + n - 1) % n, k = (i + 1) % n;
            for (std::size_t s = 0; s < n && len(at(j) - at(i)) < 1e-9; ++s) j = (j + n - 1) % n;
            for (std::size_t s = 0; s < n && len(at(k) - at(i)) < 1e-9; ++s) k = (k + 1) % n;
            const Vec in = normalize(at(i) - at(j)), outd = normalize(at(k) - at(i));
            auto normal = [&](Vec v) { return inkRight ? Vec{-v.y, v.x} : Vec{v.y, -v.x}; };
            const Vec n1 = normal(in), n2 = normal(outd);
            Vec m = n1 + n2;
            if (len(m) < 1e-6) m = n1;
            m = normalize(m);
            const double cosHalf = std::max(dot(m, n1), 0.5);  // limits miter spikes to twice the amount
            moved[i].x += m.x / cosHalf * dx;
            moved[i].y += m.y / cosHalf * dy;
        }
        pts = std::move(moved);
    }
    return out;
}

// ---- overlap removal ----

namespace {

struct Edge {
    Vec a, b;
    int poly = 0;
};

// Nonzero winding number of `edges` around p.
int winding(const std::vector<Edge>& edges, Vec p) {
    int w = 0;
    for (const auto& e : edges) {
        if (e.a.y <= p.y) {
            if (e.b.y > p.y && cross(e.b - e.a, p - e.a) > 0) ++w;
        } else if (e.b.y <= p.y && cross(e.b - e.a, p - e.a) < 0) {
            --w;
        }
    }
    return w;
}

struct Key {
    long long x, y;
    bool operator<(const Key& o) const { return x != o.x ? x < o.x : y < o.y; }
    bool operator==(const Key& o) const { return x == o.x && y == o.y; }
};
Key keyOf(Vec v) { return {std::llround(v.x * 1e5), std::llround(v.y * 1e5)}; }

}  // namespace

std::vector<Contour> removeOverlaps(const std::vector<Contour>& contours, double tolerance) {
    tolerance = std::max(tolerance, 0.01);
    const auto polys = flatten(contours, tolerance / 4);
    std::vector<Edge> edges;
    std::vector<std::size_t> firstEdge(polys.size() + 1, 0);
    for (std::size_t i = 0; i < polys.size(); ++i) {
        firstEdge[i] = edges.size();
        const auto& p = polys[i];
        if (p.size() >= 3) {
            for (std::size_t k = 0; k < p.size(); ++k) {
                const Vec a = p[k], b = p[(k + 1) % p.size()];
                if (len(b - a) > 1e-9) edges.push_back({a, b, static_cast<int>(i)});
            }
        }
    }
    firstEdge[polys.size()] = edges.size();

    // split points along each edge, as (t, point)
    std::vector<std::vector<std::pair<double, Vec>>> splits(edges.size());
    std::vector<bool> touched(polys.size(), false);
    auto addSplit = [&](std::size_t e, double t, Vec p) {
        if (t <= 1e-9 || t >= 1 - 1e-9) return false;
        splits[e].push_back({t, p});
        return true;
    };
    std::vector<Box> boxes(edges.size());
    for (std::size_t i = 0; i < edges.size(); ++i) {
        boxes[i].add(edges[i].a);
        boxes[i].add(edges[i].b);
    }
    for (std::size_t i = 0; i < edges.size(); ++i) {
        for (std::size_t j = i + 1; j < edges.size(); ++j) {
            const Box& bi = boxes[i];
            const Box& bj = boxes[j];
            if (bi.x1 < bj.x0 - 1e-9 || bj.x1 < bi.x0 - 1e-9 || bi.y1 < bj.y0 - 1e-9 || bj.y1 < bi.y0 - 1e-9) continue;
            const Edge& e = edges[i];
            const Edge& f = edges[j];
            const Vec r = e.b - e.a, s = f.b - f.a;
            const double den = cross(r, s);
            const double scale = len(r) * len(s);
            if (std::abs(den) <= 1e-12 * scale) {
                // parallel: split each at the other's endpoints when they are collinear and inside
                if (std::abs(cross(r, f.a - e.a)) > 1e-7 * len(r)) continue;
                const double rr = dot(r, r), ss = dot(s, s);
                const double t0 = dot(f.a - e.a, r) / rr, t1 = dot(f.b - e.a, r) / rr;
                addSplit(i, t0, f.a);
                addSplit(i, t1, f.b);
                addSplit(j, dot(e.a - f.a, s) / ss, e.a);
                addSplit(j, dot(e.b - f.a, s) / ss, e.b);
                if (std::min(1.0, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1)) > 1e-9) {
                    touched[static_cast<std::size_t>(e.poly)] = touched[static_cast<std::size_t>(f.poly)] = true;
                }
                continue;
            }
            const double t = cross(f.a - e.a, s) / den, u = cross(f.a - e.a, r) / den;
            if (t < -1e-9 || t > 1 + 1e-9 || u < -1e-9 || u > 1 + 1e-9) continue;
            const bool tEnd = t <= 1e-9 || t >= 1 - 1e-9, uEnd = u <= 1e-9 || u >= 1 - 1e-9;
            if (tEnd && uEnd) continue;  // shared vertex
            Vec p = e.a + r * t;
            if (uEnd) p = u <= 1e-9 ? f.a : f.b;
            if (tEnd) p = t <= 1e-9 ? e.a : e.b;
            const bool a = addSplit(i, t, p), b = addSplit(j, u, p);
            if (a || b) touched[static_cast<std::size_t>(e.poly)] = touched[static_cast<std::size_t>(f.poly)] = true;
        }
    }

    std::vector<Contour> out;
    const double nudge = std::max(1e-3, tolerance * 0.02);
    // untouched contours: kept as they are when they lie on the boundary of the filled area
    for (std::size_t i = 0; i < polys.size(); ++i) {
        if (touched[i] || polys[i].size() < 3) continue;
        const std::size_t e0 = firstEdge[i];
        if (e0 >= firstEdge[i + 1]) continue;
        const Edge& e = edges[e0];
        const Vec mid = (e.a + e.b) * 0.5, nrm = normalize(Vec{-(e.b - e.a).y, (e.b - e.a).x}) * nudge;
        const bool left = winding(edges, mid + nrm) != 0, right = winding(edges, mid - nrm) != 0;
        if (left != right) out.push_back(contours[i]);
    }

    // touched contours: split edges, keep boundary pieces with the ink on their right
    std::vector<Edge> kept;
    std::set<std::pair<Key, Key>> seen;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (!touched[static_cast<std::size_t>(edges[i].poly)]) continue;
        auto& sp = splits[i];
        std::sort(sp.begin(), sp.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        std::vector<Vec> chain = {edges[i].a};
        for (const auto& [t, p] : sp) {
            if (!(keyOf(p) == keyOf(chain.back()))) chain.push_back(p);
        }
        if (!(keyOf(edges[i].b) == keyOf(chain.back()))) chain.push_back(edges[i].b);
        for (std::size_t k = 0; k + 1 < chain.size(); ++k) {
            Vec a = chain[k], b = chain[k + 1];
            const Vec mid = (a + b) * 0.5, nrm = normalize(Vec{-(b - a).y, (b - a).x}) * nudge;
            const bool left = winding(edges, mid + nrm) != 0, right = winding(edges, mid - nrm) != 0;
            if (left == right) continue;
            if (left) std::swap(a, b);  // ink on the right: TrueType direction
            if (!seen.insert({keyOf(a), keyOf(b)}).second) continue;
            kept.push_back({a, b, 0});
        }
    }
    // link pieces into loops
    std::multimap<Key, std::size_t> from;
    for (std::size_t i = 0; i < kept.size(); ++i) from.insert({keyOf(kept[i].a), i});
    std::vector<bool> used(kept.size(), false);
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (used[i]) continue;
        Polygon loop;
        std::size_t cur = i;
        const Key startKey = keyOf(kept[i].a);
        bool closed = false;
        for (std::size_t guard = 0; guard <= kept.size(); ++guard) {
            used[cur] = true;
            loop.push_back(kept[cur].a);
            const Key endKey = keyOf(kept[cur].b);
            if (endKey == startKey) {
                closed = true;
                break;
            }
            // the next unused piece from here; with several, the one turning most to the right
            std::size_t next = kept.size();
            double bestTurn = 1e9;
            const Vec dir = normalize(kept[cur].b - kept[cur].a);
            auto range = from.equal_range(endKey);
            for (auto it = range.first; it != range.second; ++it) {
                if (used[it->second]) continue;
                const Vec nd = normalize(kept[it->second].b - kept[it->second].a);
                const double turn = std::atan2(cross(dir, nd), dot(dir, nd));  // negative = right turn
                if (turn < bestTurn) {
                    bestTurn = turn;
                    next = it->second;
                }
            }
            if (next == kept.size()) break;
            cur = next;
        }
        if (!closed || loop.size() < 3 || std::abs(signedArea(loop)) < tolerance * tolerance) continue;
        // drop collinear points before fitting
        Polygon clean;
        for (std::size_t k = 0; k < loop.size(); ++k) {
            const Vec a = loop[(k + loop.size() - 1) % loop.size()], b = loop[k], c = loop[(k + 1) % loop.size()];
            if (std::abs(cross(b - a, c - b)) > 1e-9 * len(b - a) * len(c - b) + 1e-12 || dot(b - a, c - b) < 0) clean.push_back(b);
        }
        if (clean.size() < 3) continue;
        out.push_back(fitCurves(clean, tolerance, 50));
    }
    fixDirections(out);
    return out;
}

}  // namespace wetwaresuite::typeface
