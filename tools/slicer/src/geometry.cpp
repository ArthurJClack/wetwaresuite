#include "wetwaresuite/slicer/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace wetwaresuite::slicer {

namespace {

constexpr double kScale = 1e4;  // integer units per mm (0.1 micron)

struct IPt {
    std::int64_t x = 0, y = 0;
    bool operator==(const IPt& o) const { return x == o.x && y == o.y; }
    bool operator!=(const IPt& o) const { return !(*this == o); }
    bool operator<(const IPt& o) const { return x != o.x ? x < o.x : y < o.y; }
};

struct IPtHash {
    std::size_t operator()(const IPt& p) const {
        return std::hash<std::int64_t>{}(p.x * 73856093LL ^ p.y * 19349663LL);
    }
};

IPt toI(const Vec2& p) { return {std::llround(p.x * kScale), std::llround(p.y * kScale)}; }
Vec2 toD(const IPt& p) { return {static_cast<double>(p.x) / kScale, static_cast<double>(p.y) / kScale}; }

// Exact for |coordinates| < ~1e9 units (100 m).
std::int64_t orient(const IPt& a, const IPt& b, const IPt& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

int sign(std::int64_t v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

struct Seg {
    IPt a, b;
    std::int64_t minx, maxx, miny, maxy;
    std::vector<IPt> cuts;
};

bool within(const Seg& s, const IPt& p) { return p.x >= s.minx && p.x <= s.maxx && p.y >= s.miny && p.y <= s.maxy; }

void intersectSegs(Seg& s, Seg& t) {
    const int d1 = sign(orient(s.a, s.b, t.a)), d2 = sign(orient(s.a, s.b, t.b));
    const int d3 = sign(orient(t.a, t.b, s.a)), d4 = sign(orient(t.a, t.b, s.b));
    if (d1 * d2 < 0 && d3 * d4 < 0) {
        const double o3 = static_cast<double>(orient(t.a, t.b, s.a)), o4 = static_cast<double>(orient(t.a, t.b, s.b));
        const double u = o3 / (o3 - o4);
        IPt p{std::llround(static_cast<double>(s.a.x) + static_cast<double>(s.b.x - s.a.x) * u),
              std::llround(static_cast<double>(s.a.y) + static_cast<double>(s.b.y - s.a.y) * u)};
        s.cuts.push_back(p);
        t.cuts.push_back(p);
        return;
    }
    if (d1 == 0 && within(s, t.a)) s.cuts.push_back(t.a);
    if (d2 == 0 && within(s, t.b)) s.cuts.push_back(t.b);
    if (d3 == 0 && within(t, s.a)) t.cuts.push_back(s.a);
    if (d4 == 0 && within(t, s.b)) t.cuts.push_back(s.b);
}

struct Edge {
    IPt u, v;  // canonical: u < v
    int count; // net number of input edges running u -> v
};

// Inside when the winding is non-zero (minWinding 0), or at least minWinding.
bool keep(int w, int minWinding) { return minWinding == 0 ? w != 0 : w >= minWinding; }

// Exact winding numbers by casting a ray towards +x from a point given in
// doubled coordinates (so edge midpoints are integers). Edges are bucketed
// into horizontal rows. `rotated` works in the frame (x, y) -> (y, -x), which
// turns a +y ray into a +x one.
class WindingIndex {
public:
    WindingIndex(const std::vector<Edge>& edges, bool rotated) {
        edges_.reserve(edges.size());
        for (const auto& e : edges) {
            IPt a = rotated ? IPt{e.u.y, -e.u.x} : e.u, b = rotated ? IPt{e.v.y, -e.v.x} : e.v;
            edges_.push_back({{a.x * 2, a.y * 2}, {b.x * 2, b.y * 2}, e.count});
        }
        if (edges_.empty()) return;
        ymin_ = edges_[0].u.y;
        std::int64_t ymax = ymin_;
        for (const auto& e : edges_) {
            ymin_ = std::min({ymin_, e.u.y, e.v.y});
            ymax = std::max({ymax, e.u.y, e.v.y});
        }
        rows_ = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::sqrt(static_cast<double>(edges_.size())) * 2), 1, 2048);
        rowHeight_ = std::max<std::int64_t>(1, (ymax - ymin_) / rows_ + 1);
        buckets_.resize(static_cast<std::size_t>(rows_));
        for (std::size_t i = 0; i < edges_.size(); ++i) {
            const auto lo = row(std::min(edges_[i].u.y, edges_[i].v.y)), hi = row(std::max(edges_[i].u.y, edges_[i].v.y));
            for (std::size_t r = lo; r <= hi; ++r) buckets_[r].push_back(i);
        }
    }

    // m in the index's frame, doubled.
    int winding(const IPt& m) const {
        if (edges_.empty()) return 0;
        int w = 0;
        for (std::size_t i : buckets_[row(m.y)]) {
            const Edge& e = edges_[i];
            if (e.u.y <= m.y) {
                if (e.v.y > m.y && orient(e.u, e.v, m) > 0) w += e.count;
            } else if (e.v.y <= m.y && orient(e.u, e.v, m) < 0) {
                w -= e.count;
            }
        }
        return w;
    }

private:
    std::size_t row(std::int64_t y) const {
        const std::int64_t r = (y - ymin_) / rowHeight_;
        return static_cast<std::size_t>(std::clamp<std::int64_t>(r, 0, rows_ - 1));
    }

    std::vector<Edge> edges_;
    std::int64_t ymin_ = 0, rows_ = 1, rowHeight_ = 1;
    std::vector<std::vector<std::size_t>> buckets_;
};

// Finds crossings and touches between segments (sweep in x), recording cut points.
void findCuts(std::vector<Seg>& segs) {
    std::vector<std::size_t> order(segs.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return segs[a].minx < segs[b].minx; });
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        Seg& s = segs[order[oi]];
        for (std::size_t oj = oi + 1; oj < order.size(); ++oj) {
            Seg& t = segs[order[oj]];
            if (t.minx > s.maxx) break;
            if (t.miny > s.maxy || t.maxy < s.miny) continue;
            intersectSegs(s, t);
        }
    }
}

Seg makeSeg(const IPt& a, const IPt& b) { return {a, b, std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.y, b.y), std::max(a.y, b.y), {}}; }

// Splits every segment at its cuts. Returns true if anything was split.
bool splitAtCuts(std::vector<Seg>& segs) {
    std::vector<Seg> out;
    out.reserve(segs.size());
    bool changed = false;
    for (auto& s : segs) {
        auto& c = s.cuts;
        c.erase(std::remove_if(c.begin(), c.end(), [&](const IPt& p) { return p == s.a || p == s.b; }), c.end());
        if (c.empty()) {
            out.push_back(makeSeg(s.a, s.b));
            continue;
        }
        changed = true;
        const double dx = static_cast<double>(s.b.x - s.a.x), dy = static_cast<double>(s.b.y - s.a.y);
        auto param = [&](const IPt& p) { return static_cast<double>(p.x - s.a.x) * dx + static_cast<double>(p.y - s.a.y) * dy; };
        std::sort(c.begin(), c.end(), [&](const IPt& p, const IPt& q) { return param(p) < param(q); });
        c.erase(std::unique(c.begin(), c.end()), c.end());
        IPt prev = s.a;
        for (const auto& p : c) {
            if (p != prev) out.push_back(makeSeg(prev, p));
            prev = p;
        }
        if (prev != s.b) out.push_back(makeSeg(prev, s.b));
    }
    segs.swap(out);
    return changed;
}

double turnAngle(const IPt& a, const IPt& b, const IPt& c) {
    const double x1 = static_cast<double>(b.x - a.x), y1 = static_cast<double>(b.y - a.y);
    const double x2 = static_cast<double>(c.x - b.x), y2 = static_cast<double>(c.y - b.y);
    return std::atan2(x1 * y2 - y1 * x2, x1 * x2 + y1 * y2);
}

Loop cleanLoop(const std::vector<IPt>& pts) {
    // drop collinear points (exactly, on the integer grid)
    std::vector<IPt> p = pts;
    bool changed = true;
    while (changed && p.size() >= 3) {
        changed = false;
        std::vector<IPt> out;
        const std::size_t n = p.size();
        for (std::size_t i = 0; i < n; ++i) {
            const IPt& a = out.empty() ? p[(i + n - 1) % n] : out.back();
            const IPt& b = p[i];
            const IPt& c = p[(i + 1) % n];
            if (b == a || orient(a, b, c) == 0) {
                // collinear: keep only if it's a spike reversal we must not straighten (never needed for region bounds)
                changed = true;
                continue;
            }
            out.push_back(b);
        }
        p.swap(out);
    }
    Loop loop;
    loop.reserve(p.size());
    for (const auto& q : p) loop.push_back(toD(q));
    return loop;
}

}  // namespace

static Region resolveAtLeast(const std::vector<Loop>& loops, int minWinding) {
    const int rule = minWinding;
    // 1. integer segments
    std::vector<Seg> segs;
    for (const auto& loop : loops) {
        if (loop.size() < 3) continue;
        std::vector<IPt> pts;
        pts.reserve(loop.size());
        for (const auto& v : loop) {
            IPt p = toI(v);
            if (pts.empty() || pts.back() != p) pts.push_back(p);
        }
        while (pts.size() > 1 && pts.front() == pts.back()) pts.pop_back();
        if (pts.size() < 3) continue;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const IPt& a = pts[i];
            const IPt& b = pts[(i + 1) % pts.size()];
            segs.push_back({a, b, std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.y, b.y), std::max(a.y, b.y), {}});
        }
    }
    if (segs.empty()) return {};

    // 2. split where segments cross or touch; rounding a crossing to the grid
    //    can create new ones, so repeat a few times
    for (int pass = 0; pass < 4; ++pass) {
        findCuts(segs);
        if (!splitAtCuts(segs)) break;
    }

    // 3. pieces, merged when coincident
    std::vector<Edge> pieces;
    pieces.reserve(segs.size());
    for (const auto& sg : segs) {
        if (sg.a == sg.b) continue;
        if (sg.a < sg.b) pieces.push_back({sg.a, sg.b, 1});
        else pieces.push_back({sg.b, sg.a, -1});
    }
    std::sort(pieces.begin(), pieces.end(), [](const Edge& a, const Edge& b) { return a.u != b.u ? a.u < b.u : a.v < b.v; });
    std::vector<Edge> edges;
    for (const auto& e : pieces) {
        if (!edges.empty() && edges.back().u == e.u && edges.back().v == e.v) edges.back().count += e.count;
        else edges.push_back(e);
    }
    edges.erase(std::remove_if(edges.begin(), edges.end(), [](const Edge& e) { return e.count == 0; }), edges.end());

    // 4. keep the pieces with the region on exactly one side, oriented
    //    region-on-left. The winding just right of (or above, for horizontal
    //    pieces) each midpoint is exact; the other side differs by the count.
    WindingIndex alongX(edges, false), alongY(edges, true);
    struct Directed {
        IPt a, b;
    };
    std::vector<Directed> kept;
    for (const auto& e : edges) {
        const IPt m{e.u.x + e.v.x, e.u.y + e.v.y};
        int left = 0, right = 0;
        if (e.u.y < e.v.y) {
            right = alongX.winding(m);
            left = right + e.count;
        } else if (e.u.y > e.v.y) {
            left = alongX.winding(m);
            right = left - e.count;
        } else {
            left = alongY.winding({m.y, -m.x});
            right = left - e.count;
        }
        const bool inL = keep(left, rule), inR = keep(right, rule);
        if (inL && !inR) kept.push_back({e.u, e.v});
        else if (inR && !inL) kept.push_back({e.v, e.u});
    }

    // 5. link into loops, taking the sharpest right turn at junctions
    std::unordered_map<IPt, std::vector<std::size_t>, IPtHash> outgoing;
    outgoing.reserve(kept.size() * 2);
    for (std::size_t i = 0; i < kept.size(); ++i) outgoing[kept[i].a].push_back(i);
    std::vector<char> used(kept.size(), 0);
    Region out;
    for (std::size_t start = 0; start < kept.size(); ++start) {
        if (used[start]) continue;
        used[start] = 1;
        std::vector<IPt> pts{kept[start].a};
        std::size_t cur = start;
        bool closed = false;
        for (std::size_t guard = 0; guard <= kept.size(); ++guard) {
            const IPt p = kept[cur].b;
            if (p == kept[start].a) {
                closed = true;
                break;
            }
            pts.push_back(p);
            auto it = outgoing.find(p);
            std::size_t best = kept.size();
            double bestTurn = 10;
            if (it != outgoing.end()) {
                for (std::size_t cand : it->second) {
                    if (used[cand]) continue;
                    const double t = turnAngle(kept[cur].a, p, kept[cand].b);
                    if (t < bestTurn) {
                        bestTurn = t;
                        best = cand;
                    }
                }
            }
            if (best == kept.size()) {
                // a chain broken by grid rounding: jump a few units to the nearest free piece
                std::int64_t bd = 64;
                for (std::size_t cand = 0; cand < kept.size(); ++cand) {
                    if (used[cand]) continue;
                    const std::int64_t dx = kept[cand].a.x - p.x, dy = kept[cand].a.y - p.y;
                    if (dx * dx + dy * dy < bd) bd = dx * dx + dy * dy, best = cand;
                }
                const std::int64_t sx = kept[start].a.x - p.x, sy = kept[start].a.y - p.y;
                if (best == kept.size() || sx * sx + sy * sy <= bd) break;
            }
            used[best] = 1;
            cur = best;
        }
        if (pts.size() < 3) continue;
        (void)closed;  // chains broken by snapping are closed anyway
        Loop loop = cleanLoop(pts);
        if (loop.size() >= 3 && std::abs(mesh::loopArea(loop)) > 1e-6) out.push_back(std::move(loop));
    }
    return out;
}

Region resolve(const std::vector<Loop>& loops, FillRule rule) {
    return resolveAtLeast(loops, rule == FillRule::NonZero ? 0 : rule == FillRule::Positive ? 1 : 2);
}

Region intersectAll(const std::vector<const Region*>& regions) {
    std::vector<Loop> all;
    int count = 0;
    for (const Region* r : regions) {
        if (!r || r->empty()) return {};
        all.insert(all.end(), r->begin(), r->end());
        ++count;
    }
    if (count == 0) return {};
    if (count == 1) return *regions.front();
    return resolveAtLeast(all, count);
}

Region unite(const Region& a, const Region& b) {
    Region all = a;
    all.insert(all.end(), b.begin(), b.end());
    return resolve(all, FillRule::Positive);
}

Region intersect(const Region& a, const Region& b) {
    if (a.empty() || b.empty()) return {};
    Region all = a;
    all.insert(all.end(), b.begin(), b.end());
    return resolve(all, FillRule::AtLeastTwo);
}

Region subtract(const Region& a, const Region& b) {
    if (a.empty()) return {};
    if (b.empty()) return a;
    Region all = a;
    for (const auto& loop : b) all.emplace_back(loop.rbegin(), loop.rend());
    return resolve(all, FillRule::Positive);
}

namespace {

double segDistance(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 ab = b - a;
    const double l2 = dot(ab, ab);
    double t = l2 > 0 ? dot(p - a, ab) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return length(p - (a + ab * t));
}

void orientCcw(Loop& l, bool ccw) {
    if ((mesh::loopArea(l) > 0) != ccw) std::reverse(l.begin(), l.end());
}

}  // namespace

// The exact (Minkowski) offset: every point within |delta| of the boundary
// lies in a strip along one edge, on the side the offset moves into, or in
// a wedge at a corner that bends away from that side. Adding those strips and
// wedges to the region (growing), or cutting them out of it (shrinking), in a
// single positive-winding pass gives the offset with sharp corners where the
// strips overlap and round ones at the wedges.
Region offset(const Region& region, double delta) {
    if (region.empty()) return {};
    if (delta == 0) return region;
    const double d = std::abs(delta);
    const bool grow = delta > 0;
    std::vector<Loop> parts = region;
    const double step = mesh::kPi / (d < 1 ? 6 : d < 5 ? 12 : 24);
    for (const auto& loop : region) {
        const std::size_t n = loop.size();
        if (n < 3) continue;
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2& a = loop[(i + n - 1) % n];
            const Vec2& b = loop[i];
            const Vec2& c = loop[(i + 1) % n];
            const Vec2 e2 = c - b;
            const double l2 = length(e2);
            if (l2 <= 0) continue;
            // the region is on the left of every edge; offsets move to the right (grow) or left (shrink)
            const Vec2 s2 = grow ? Vec2{e2.y / l2, -e2.x / l2} : Vec2{-e2.y / l2, e2.x / l2};
            Loop strip = {b, c, c + s2 * d, b + s2 * d};
            orientCcw(strip, grow);
            parts.push_back(std::move(strip));

            const Vec2 e1 = b - a;
            const double l1 = length(e1);
            if (l1 <= 0) continue;
            const double turn = cross(e1, e2) / (l1 * l2);
            if (grow ? turn <= 1e-9 : turn >= -1e-9) continue;  // the strips already meet on this side
            const Vec2 s1 = grow ? Vec2{e1.y / l1, -e1.x / l1} : Vec2{-e1.y / l1, e1.x / l1};
            double a1 = std::atan2(s1.y, s1.x), sweep = std::atan2(cross(s1, s2), dot(s1, s2));
            const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / step)));
            Loop wedge = {b};
            for (int k = 0; k <= steps; ++k) {
                const double ang = a1 + sweep * k / steps;
                wedge.push_back(b + Vec2{std::cos(ang), std::sin(ang)} * d);
            }
            orientCcw(wedge, grow);
            parts.push_back(std::move(wedge));
        }
    }
    return resolve(parts, FillRule::Positive);
}

double area(const Region& region) {
    double a = 0;
    for (const auto& loop : region) a += mesh::loopArea(loop);
    return a;
}

bool contains(const Region& region, const Vec2& p) {
    bool inside = false;
    for (const auto& loop : region) {
        const std::size_t n = loop.size();
        for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
            const Vec2& a = loop[i];
            const Vec2& b = loop[j];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
        }
    }
    return inside;
}

Loop simplify(const Loop& loop, double tolerance) {
    if (loop.size() <= 3) return loop;
    Loop out;
    out.reserve(loop.size());
    const std::size_t n = loop.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2& prev = out.empty() ? loop[n - 1] : out.back();
        const Vec2& cur = loop[i];
        const Vec2& next = loop[(i + 1) % n];
        if (length(cur - prev) < tolerance * 0.5) continue;
        if (segDistance(cur, prev, next) < tolerance && length(next - prev) > 0) continue;
        out.push_back(cur);
    }
    if (out.size() < 3) return loop;
    return out;
}

std::vector<Region> islands(const Region& region) {
    std::vector<std::size_t> outers, holes;
    for (std::size_t i = 0; i < region.size(); ++i) (mesh::loopArea(region[i]) > 0 ? outers : holes).push_back(i);
    std::vector<Region> out(outers.size());
    std::vector<double> areas(outers.size());
    for (std::size_t k = 0; k < outers.size(); ++k) {
        out[k].push_back(region[outers[k]]);
        areas[k] = mesh::loopArea(region[outers[k]]);
    }
    for (std::size_t h : holes) {
        std::size_t best = outers.size();
        for (std::size_t k = 0; k < outers.size(); ++k) {
            if (contains(Region{region[outers[k]]}, region[h][0]) && (best == outers.size() || areas[k] < areas[best])) best = k;
        }
        if (best < outers.size()) out[best].push_back(region[h]);
    }
    return out;
}

std::vector<Segment> fillLines(const Region& region, double spacing, double angleDegrees) {
    std::vector<Segment> out;
    if (region.empty() || !(spacing > 0)) return out;
    const double a = angleDegrees * mesh::kPi / 180.0, ca = std::cos(a), sa = std::sin(a);
    // rotate by -angle so the lines become horizontal
    auto toLocal = [&](const Vec2& p) { return Vec2{p.x * ca + p.y * sa, -p.x * sa + p.y * ca}; };
    auto toWorld = [&](const Vec2& p) { return Vec2{p.x * ca - p.y * sa, p.x * sa + p.y * ca}; };
    struct E {
        Vec2 a, b;
    };
    std::vector<E> edges;
    double ymin = 1e300, ymax = -1e300;
    for (const auto& loop : region) {
        for (std::size_t i = 0; i < loop.size(); ++i) {
            Vec2 p = toLocal(loop[i]), q = toLocal(loop[(i + 1) % loop.size()]);
            edges.push_back({p, q});
            ymin = std::min(ymin, p.y);
            ymax = std::max(ymax, p.y);
        }
    }
    std::sort(edges.begin(), edges.end(), [](const E& x, const E& y) { return std::min(x.a.y, x.b.y) < std::min(y.a.y, y.b.y); });
    std::vector<double> xs;
    const auto k0 = static_cast<long long>(std::ceil(ymin / spacing - 1e-9));
    const auto k1 = static_cast<long long>(std::floor(ymax / spacing + 1e-9));
    for (long long k = k0; k <= k1; ++k) {
        double y = static_cast<double>(k) * spacing;
        if (y <= ymin) y = ymin + 1e-7;
        if (y >= ymax) y = ymax - 1e-7;
        xs.clear();
        for (const auto& e : edges) {
            if (std::min(e.a.y, e.b.y) > y) break;
            if ((e.a.y <= y) != (e.b.y <= y)) xs.push_back(e.a.x + (y - e.a.y) * (e.b.x - e.a.x) / (e.b.y - e.a.y));
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
            if (xs[i + 1] - xs[i] < 1e-6) continue;
            out.push_back({toWorld({xs[i], y}), toWorld({xs[i + 1], y})});
        }
    }
    return out;
}

}  // namespace wetwaresuite::slicer
