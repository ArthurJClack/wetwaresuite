// Feature editing helpers: numbered vertices, the nearest edge, moving,
// splitting and merging geometries, intersection tests, and cutting lines
// by polygons.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "wetwaresuite/maps/maps.hpp"

namespace wetwaresuite::maps {

namespace {

// Every chain of vertices in a geometry, in numbering order.
struct Chain {
    std::vector<Coord>* coords;
    bool closed;
    std::size_t minSize;
};

std::vector<Chain> chains(Geometry& g) {
    std::vector<Chain> out;
    if (!g.points.empty()) out.push_back({&g.points, false, 1});
    for (auto& l : g.lines) out.push_back({&l, false, 2});
    for (auto& p : g.polygons) {
        for (auto& r : p) out.push_back({&r, true, 3});
    }
    return out;
}

// Local metres around a reference latitude (equirectangular): fine for edges of a few km.
struct Local {
    double lon0, lat0, kx, ky;
    explicit Local(double lon, double lat)
        : lon0(lon), lat0(lat), kx(kDeg * kAuthalicRadius * std::cos(std::clamp(lat, -89.9, 89.9) * kDeg)), ky(kDeg * kAuthalicRadius) {}
    double x(const Coord& c) const {
        double d = c.x - lon0;
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        return d * kx;
    }
    double y(const Coord& c) const { return (c.y - lat0) * ky; }
};

Coord interpolate(const Coord& a, const Coord& b, double t) {
    Coord c;
    c.x = a.x + (b.x - a.x) * t;
    c.y = a.y + (b.y - a.y) * t;
    c.z = std::isnan(a.z) || std::isnan(b.z) ? (std::isnan(a.z) ? b.z : a.z) : a.z + (b.z - a.z) * t;
    c.t = std::isnan(a.t) || std::isnan(b.t) ? kNaN : a.t + (b.t - a.t) * t;
    return c;
}

bool samePlace(const Coord& a, const Coord& b) { return std::abs(a.x - b.x) < 1e-10 && std::abs(a.y - b.y) < 1e-10; }

struct Seg {
    Coord a, b;
};

std::vector<Seg> segments(const Geometry& g) {
    std::vector<Seg> out;
    for (const auto& p : g.points) out.push_back({p, p});
    for (const auto& l : g.lines) {
        if (l.size() == 1) out.push_back({l[0], l[0]});
        for (std::size_t i = 0; i + 1 < l.size(); ++i) out.push_back({l[i], l[i + 1]});
    }
    for (const auto& p : g.polygons) {
        for (const auto& r : p) {
            for (std::size_t i = 0; i < r.size(); ++i) out.push_back({r[i], r[(i + 1) % r.size()]});
        }
    }
    return out;
}

double cross(const Coord& o, const Coord& a, const Coord& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

bool onSegment(const Coord& p, const Coord& a, const Coord& b) {
    return std::min(a.x, b.x) - 1e-12 <= p.x && p.x <= std::max(a.x, b.x) + 1e-12 && std::min(a.y, b.y) - 1e-12 <= p.y && p.y <= std::max(a.y, b.y) + 1e-12;
}

bool segmentsTouch(const Seg& s, const Seg& t) {
    const double d1 = cross(t.a, t.b, s.a), d2 = cross(t.a, t.b, s.b), d3 = cross(s.a, s.b, t.a), d4 = cross(s.a, s.b, t.b);
    const double eps = 1e-18;
    if (((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps))) return true;
    if (std::abs(d1) <= eps && onSegment(s.a, t.a, t.b)) return true;
    if (std::abs(d2) <= eps && onSegment(s.b, t.a, t.b)) return true;
    if (std::abs(d3) <= eps && onSegment(t.a, s.a, s.b)) return true;
    if (std::abs(d4) <= eps && onSegment(t.b, s.a, s.b)) return true;
    return false;
}

}  // namespace

std::vector<Coord> vertices(const Geometry& g0) {
    Geometry& g = const_cast<Geometry&>(g0);  // chains() only reads here
    std::vector<Coord> out;
    for (const auto& c : chains(g)) out.insert(out.end(), c.coords->begin(), c.coords->end());
    return out;
}

Coord* vertexAt(Geometry& g, std::size_t index) {
    for (auto& c : chains(g)) {
        if (index < c.coords->size()) return &(*c.coords)[index];
        index -= c.coords->size();
    }
    return nullptr;
}

bool insertVertex(Geometry& g, std::size_t after, const Coord& v) {
    for (auto& c : chains(g)) {
        if (after < c.coords->size()) {
            c.coords->insert(c.coords->begin() + static_cast<std::ptrdiff_t>(after) + 1, v);
            return true;
        }
        after -= c.coords->size();
    }
    return false;
}

bool removeVertex(Geometry& g, std::size_t index, std::string& error) {
    for (auto& c : chains(g)) {
        if (index < c.coords->size()) {
            if (c.coords->size() <= c.minSize) {
                error = c.minSize == 1 ? "that is the feature's only point: delete the feature instead (maps.feature_delete)"
                        : c.minSize == 2 ? "a line needs at least 2 points"
                                         : "a polygon ring needs at least 3 points";
                return false;
            }
            c.coords->erase(c.coords->begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
        index -= c.coords->size();
    }
    error = "there is no vertex " + std::to_string(index);
    return false;
}

EdgeHit nearestEdge(const Geometry& g0, double lon, double lat) {
    Geometry& g = const_cast<Geometry&>(g0);
    EdgeHit best;
    const Local loc(lon, lat);
    const Coord q{lon, lat};
    std::size_t base = 0;
    double bestD2 = std::numeric_limits<double>::infinity();
    for (const auto& c : chains(g)) {
        const auto& cs = *c.coords;
        if (c.minSize == 1) {  // points have no edges
            base += cs.size();
            continue;
        }
        const std::size_t n = c.closed ? cs.size() : cs.size() - 1;
        double along = 0;
        for (std::size_t i = 0; i < n && cs.size() > 1; ++i) {
            const Coord& a = cs[i];
            const Coord& b = cs[(i + 1) % cs.size()];
            const double ax = loc.x(a), ay = loc.y(a), bx = loc.x(b), by = loc.y(b), qx = loc.x(q), qy = loc.y(q);
            const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
            double t = len2 > 0 ? ((qx - ax) * dx + (qy - ay) * dy) / len2 : 0;
            t = std::clamp(t, 0.0, 1.0);
            const double px = ax + t * dx, py = ay + t * dy;
            const double d2 = (px - qx) * (px - qx) + (py - qy) * (py - qy);
            const double segLen = geodesicDistance(a.x, a.y, b.x, b.y);
            if (d2 < bestD2) {
                bestD2 = d2;
                best.found = true;
                best.vertex = base + i;
                best.point = interpolate(a, b, t);
                best.along = along + segLen * t;
            }
            along += segLen;
        }
        base += cs.size();
    }
    if (best.found) best.distance = geodesicDistance(lon, lat, best.point.x, best.point.y);
    return best;
}

void translate(Geometry& g, double dx, double dy) {
    const BBox b = bboxOf(g);
    if (!b.valid()) return;
    const Crs crs = Crs::localTm((b.minX + b.maxX) / 2);
    g.forEach([&](Coord& c) {
        Coord p = crs.forward(c);
        p.x += dx;
        p.y += dy;
        const Coord q = crs.inverse(p);
        c.x = q.x;
        c.y = q.y;
    });
}

std::pair<Geometry, Geometry> splitAt(const Geometry& g, const EdgeHit& hit) {
    if (g.type != GeomType::Line || !hit.found) throw std::runtime_error("only lines can be split");
    std::size_t index = hit.vertex, li = 0;
    while (li < g.lines.size() && index >= g.lines[li].size()) index -= g.lines[li++].size();
    if (li >= g.lines.size()) throw std::runtime_error("the split point isn't on the line");
    const Line& l = g.lines[li];
    Line first(l.begin(), l.begin() + static_cast<std::ptrdiff_t>(index) + 1), second;
    if (!samePlace(first.back(), hit.point)) first.push_back(hit.point);
    second.push_back(hit.point);
    for (std::size_t k = index + 1; k < l.size(); ++k) {
        if (k == index + 1 && samePlace(l[k], hit.point)) continue;
        second.push_back(l[k]);
    }
    Geometry a, b;
    a.type = b.type = GeomType::Line;
    for (std::size_t k = 0; k < li; ++k) a.lines.push_back(g.lines[k]);
    if (first.size() >= 2) a.lines.push_back(std::move(first));
    if (second.size() >= 2) b.lines.push_back(std::move(second));
    for (std::size_t k = li + 1; k < g.lines.size(); ++k) b.lines.push_back(g.lines[k]);
    if (a.lines.empty()) a.type = GeomType::None;
    if (b.lines.empty()) b.type = GeomType::None;
    return {std::move(a), std::move(b)};
}

Geometry mergeGeometries(const std::vector<const Geometry*>& gs, double snap) {
    Geometry out;
    for (const Geometry* g : gs) {
        if (g->empty()) continue;
        if (out.type == GeomType::None) out.type = g->type;
        else if (out.type != g->type) throw std::runtime_error("can't merge different kinds of geometry (points, lines, polygons) into one feature");
        out.points.insert(out.points.end(), g->points.begin(), g->points.end());
        out.polygons.insert(out.polygons.end(), g->polygons.begin(), g->polygons.end());
        for (const auto& l : g->lines) {
            if (l.size() >= 2) out.lines.push_back(l);
        }
    }
    if (out.type != GeomType::Line || out.lines.size() < 2) return out;
    // chain lines whose ends meet into as few lines as possible
    std::vector<Line> pool = std::move(out.lines), chainsOut;
    auto close = [snap](const Coord& a, const Coord& b) { return geodesicDistance(a.x, a.y, b.x, b.y) <= snap; };
    while (!pool.empty()) {
        Line cur = std::move(pool.front());
        pool.erase(pool.begin());
        bool grew = true;
        while (grew) {
            grew = false;
            for (std::size_t i = 0; i < pool.size(); ++i) {
                Line& l = pool[i];
                if (close(cur.back(), l.front())) {
                    cur.insert(cur.end(), l.begin() + 1, l.end());
                } else if (close(cur.back(), l.back())) {
                    cur.insert(cur.end(), l.rbegin() + 1, l.rend());
                } else if (close(cur.front(), l.back())) {
                    cur.insert(cur.begin(), l.begin(), l.end() - 1);
                } else if (close(cur.front(), l.front())) {
                    cur.insert(cur.begin(), l.rbegin(), l.rend() - 1);
                } else {
                    continue;
                }
                pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(i));
                grew = true;
                break;
            }
        }
        chainsOut.push_back(std::move(cur));
    }
    out.lines = std::move(chainsOut);
    return out;
}

bool intersects(const Geometry& a, const Geometry& b) {
    if (a.empty() || b.empty() || !bboxOf(a).intersects(bboxOf(b))) return false;
    // one inside the other
    bool inside = false;
    a.forEach([&](const Coord& c) { inside = inside || pointInGeometry(b, c.x, c.y); });
    if (inside) return true;
    b.forEach([&](const Coord& c) { inside = inside || pointInGeometry(a, c.x, c.y); });
    if (inside) return true;
    const auto sa = segments(a), sb = segments(b);
    for (const auto& s : sa) {
        const BBox bs{std::min(s.a.x, s.b.x), std::min(s.a.y, s.b.y), std::max(s.a.x, s.b.x), std::max(s.a.y, s.b.y)};
        for (const auto& t : sb) {
            if (std::max(t.a.x, t.b.x) < bs.minX || std::min(t.a.x, t.b.x) > bs.maxX || std::max(t.a.y, t.b.y) < bs.minY || std::min(t.a.y, t.b.y) > bs.maxY) continue;
            if (segmentsTouch(s, t)) return true;
        }
    }
    return false;
}

double distanceBetween(const Geometry& a, const Geometry& b) {
    if (a.empty() || b.empty()) return std::numeric_limits<double>::infinity();
    if (intersects(a, b)) return 0;
    double best = std::numeric_limits<double>::infinity();
    a.forEach([&](const Coord& c) { best = std::min(best, nearestPoint(b, c.x, c.y).distance); });
    b.forEach([&](const Coord& c) { best = std::min(best, nearestPoint(a, c.x, c.y).distance); });
    return best;
}

Geometry clipLinesByPolygons(const Geometry& g, const std::vector<const Geometry*>& polygons, bool keepInside) {
    std::vector<Seg> edges;
    for (const Geometry* p : polygons) {
        for (const auto& poly : p->polygons) {
            for (const auto& r : poly) {
                for (std::size_t i = 0; i < r.size(); ++i) edges.push_back({r[i], r[(i + 1) % r.size()]});
            }
        }
    }
    auto in = [&](const Coord& c) {
        for (const Geometry* p : polygons) {
            if (pointInGeometry(*p, c.x, c.y)) return true;
        }
        return false;
    };
    Geometry out;
    out.type = GeomType::Line;
    for (const auto& l : g.lines) {
        Line cur;
        auto flush = [&] {
            if (cur.size() >= 2) out.lines.push_back(cur);
            cur.clear();
        };
        for (std::size_t i = 0; i + 1 < l.size(); ++i) {
            const Coord& a = l[i];
            const Coord& b = l[i + 1];
            std::vector<double> ts{0.0, 1.0};
            for (const auto& e : edges) {
                const double dx = b.x - a.x, dy = b.y - a.y, ex = e.b.x - e.a.x, ey = e.b.y - e.a.y;
                const double den = dx * ey - dy * ex;
                if (std::abs(den) < 1e-20) continue;
                const double t = ((e.a.x - a.x) * ey - (e.a.y - a.y) * ex) / den;
                const double u = ((e.a.x - a.x) * dy - (e.a.y - a.y) * dx) / den;
                if (t > 0 && t < 1 && u >= 0 && u <= 1) ts.push_back(t);
            }
            std::sort(ts.begin(), ts.end());
            for (std::size_t k = 0; k + 1 < ts.size(); ++k) {
                if (ts[k + 1] - ts[k] < 1e-12) continue;
                const Coord mid = interpolate(a, b, (ts[k] + ts[k + 1]) / 2);
                if (in(mid) == keepInside) {
                    const Coord p0 = interpolate(a, b, ts[k]), p1 = interpolate(a, b, ts[k + 1]);
                    if (cur.empty() || !samePlace(cur.back(), p0)) {
                        flush();
                        cur.push_back(p0);
                    }
                    cur.push_back(p1);
                } else {
                    flush();
                }
            }
        }
        flush();
    }
    if (out.lines.empty()) out.type = GeomType::None;
    return out;
}

}  // namespace wetwaresuite::maps
