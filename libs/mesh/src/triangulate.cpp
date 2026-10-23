// Polygon-with-holes triangulation: holes are bridged into the outer ring
// (nearest visible vertex to the right of each hole's rightmost point), then
// the single ring is ear-clipped. O(n^2) worst case, fine for CAD profiles.
#include <algorithm>
#include <cmath>

#include "internal.hpp"

namespace wetwaresuite::mesh::detail {

double signedArea(const std::vector<Vec2>& poly) {
    double a = 0;
    for (std::size_t i = 0, n = poly.size(); i < n; ++i) a += cross(poly[i], poly[(i + 1) % n]);
    return a * 0.5;
}

namespace {

struct Ring {
    std::vector<std::uint32_t> ids;  // indices into the combined point array
};

double orient(const Vec2& a, const Vec2& b, const Vec2& c) { return cross(b - a, c - a); }

// Proper or touching intersection of segments ab and cd, ignoring shared endpoints.
bool segmentsCross(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
    if (a == c || a == d || b == c || b == d) return false;
    const double d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return true;
    auto onSeg = [](const Vec2& p, const Vec2& q, const Vec2& r) {  // r on pq, given collinear
        return std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x) && std::min(p.y, q.y) <= r.y && r.y <= std::max(p.y, q.y);
    };
    if (d1 == 0 && onSeg(c, d, a)) return true;
    if (d2 == 0 && onSeg(c, d, b)) return true;
    if (d3 == 0 && onSeg(a, b, c)) return true;
    if (d4 == 0 && onSeg(a, b, d)) return true;
    return false;
}

bool ringBlocks(const std::vector<Vec2>& pts, const std::vector<std::uint32_t>& ring, const Vec2& a, const Vec2& b) {
    for (std::size_t i = 0, n = ring.size(); i < n; ++i) {
        if (segmentsCross(a, b, pts[ring[i]], pts[ring[(i + 1) % n]])) return true;
    }
    return false;
}

// Splices `hole` into `outer` with a two-way bridge.
void bridge(const std::vector<Vec2>& pts, std::vector<std::uint32_t>& outer, const std::vector<std::uint32_t>& hole,
            const std::vector<Ring>& pending) {
    std::size_t m = 0;
    for (std::size_t i = 1; i < hole.size(); ++i) {
        const Vec2 &p = pts[hole[i]], &q = pts[hole[m]];
        if (p.x > q.x || (p.x == q.x && p.y < q.y)) m = i;
    }
    const Vec2 M = pts[hole[m]];
    std::vector<std::size_t> order(outer.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto dist = [&](std::size_t i) {
        Vec2 d = pts[outer[i]] - M;
        return dot(d, d) + (pts[outer[i]].x < M.x ? 1e300 : 0.0);  // prefer vertices to the right
    };
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return dist(a) < dist(b); });
    std::size_t best = order.empty() ? 0 : order[0];
    for (std::size_t i : order) {
        const Vec2 P = pts[outer[i]];
        if (P == M) {
            best = i;
            break;
        }
        bool blocked = ringBlocks(pts, outer, M, P) || ringBlocks(pts, hole, M, P);
        for (const auto& r : pending) {
            if (!blocked) blocked = ringBlocks(pts, r.ids, M, P);
        }
        if (!blocked) {
            best = i;
            break;
        }
    }
    std::vector<std::uint32_t> merged;
    merged.reserve(outer.size() + hole.size() + 2);
    merged.insert(merged.end(), outer.begin(), outer.begin() + static_cast<std::ptrdiff_t>(best) + 1);
    for (std::size_t k = 0; k <= hole.size(); ++k) merged.push_back(hole[(m + k) % hole.size()]);
    merged.insert(merged.end(), outer.begin() + static_cast<std::ptrdiff_t>(best), outer.end());
    outer = std::move(merged);
}

bool inTriangle(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c) {
    return orient(a, b, p) >= 0 && orient(b, c, p) >= 0 && orient(c, a, p) >= 0;
}

// Ear clipping leaves a zero-area triangle (a, v, b) when v sits on the straight line from a to b
// (collinear points, as CSG cuts leave along an edge). The face next door has v on that edge, so the
// sliver goes, and the triangle across a-b is split at v instead.
void dropSlivers(const std::vector<Vec2>& pts, std::vector<std::uint32_t>& tris) {
    // "on the line" allows for coordinates rounded in the file (STEP writers keep 12 to 15 digits)
    Vec2 lo{1e300, 1e300}, hi{-1e300, -1e300};
    for (const Vec2& p : pts) lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)}, hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    const double tol = 1e-9 * std::max({std::abs(lo.x), std::abs(lo.y), std::abs(hi.x), std::abs(hi.y), 1e-300});
    for (int round = 0; round < 64; ++round) {
        bool changed = false;
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            // the longest edge is a-b, v the corner opposite it
            int k = 0;
            double longest = -1;
            for (int e = 0; e < 3; ++e) {
                const Vec2 d = pts[tris[t + static_cast<std::size_t>((e + 1) % 3)]] - pts[tris[t + static_cast<std::size_t>(e)]];
                if (dot(d, d) > longest) longest = dot(d, d), k = e;
            }
            const std::uint32_t a = tris[t + static_cast<std::size_t>(k)], b = tris[t + static_cast<std::size_t>((k + 1) % 3)],
                                v = tris[t + static_cast<std::size_t>((k + 2) % 3)];
            const Vec2 &pa = pts[a], &pb = pts[b], &pv = pts[v];
            if (!(longest > 0) || std::abs(orient(pa, pb, pv)) > tol * std::sqrt(longest)) continue;
            const double along = dot(pv - pa, pb - pa);
            if (along <= 0 || along >= longest) continue;  // v not strictly between a and b
            // the neighbour across a-b runs b -> a
            for (std::size_t u = 0; u + 2 < tris.size(); u += 3) {
                if (u == t) continue;
                int j = 0;
                while (j < 3 && !(tris[u + static_cast<std::size_t>(j)] == b && tris[u + static_cast<std::size_t>((j + 1) % 3)] == a)) ++j;
                if (j == 3) continue;
                const std::uint32_t c = tris[u + static_cast<std::size_t>((j + 2) % 3)];
                if (c == v) continue;
                tris[u] = b, tris[u + 1] = v, tris[u + 2] = c;  // (b, v, c): was (b, a, c) with v put on b-a
                tris[t] = v, tris[t + 1] = a, tris[t + 2] = c;
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
}

}  // namespace

std::vector<std::uint32_t> triangulate(const std::vector<Vec2>& outerIn, const std::vector<std::vector<Vec2>>& holesIn) {
    std::vector<Vec2> pts(outerIn);
    for (const auto& h : holesIn) pts.insert(pts.end(), h.begin(), h.end());

    std::vector<std::uint32_t> outer(outerIn.size());
    for (std::size_t i = 0; i < outer.size(); ++i) outer[i] = static_cast<std::uint32_t>(i);
    if (signedArea(outerIn) < 0) std::reverse(outer.begin(), outer.end());

    std::vector<Ring> holes;
    std::uint32_t base = static_cast<std::uint32_t>(outerIn.size());
    for (const auto& h : holesIn) {
        Ring r;
        for (std::size_t i = 0; i < h.size(); ++i) r.ids.push_back(base + static_cast<std::uint32_t>(i));
        base += static_cast<std::uint32_t>(h.size());
        if (h.size() < 3) continue;
        if (signedArea(h) > 0) std::reverse(r.ids.begin(), r.ids.end());  // holes wind clockwise
        holes.push_back(std::move(r));
    }
    auto maxX = [&](const Ring& r) {
        double x = -1e300;
        for (auto i : r.ids) x = std::max(x, pts[i].x);
        return x;
    };
    std::sort(holes.begin(), holes.end(), [&](const Ring& a, const Ring& b) { return maxX(a) > maxX(b); });
    for (std::size_t h = 0; h < holes.size(); ++h) {
        std::vector<Ring> pending(holes.begin() + static_cast<std::ptrdiff_t>(h) + 1, holes.end());
        bridge(pts, outer, holes[h].ids, pending);
    }

    // Ear clipping over a doubly linked ring.
    std::vector<std::uint32_t> out;
    const std::size_t n = outer.size();
    if (n < 3) return out;
    out.reserve((n - 2) * 3);
    std::vector<std::size_t> prev(n), next(n);
    for (std::size_t i = 0; i < n; ++i) {
        prev[i] = (i + n - 1) % n;
        next[i] = (i + 1) % n;
    }
    auto P = [&](std::size_t i) -> const Vec2& { return pts[outer[i]]; };
    auto isEar = [&](std::size_t i) {
        const std::size_t a = prev[i], c = next[i];
        const Vec2 &pa = P(a), &pb = P(i), &pc = P(c);
        if (orient(pa, pb, pc) <= 0) return false;
        for (std::size_t j = next[c]; j != a; j = next[j]) {
            const Vec2& q = P(j);
            if (q == pa || q == pb || q == pc) continue;
            if (orient(P(prev[j]), q, P(next[j])) > 0) continue;  // only reflex vertices can poke in
            if (inTriangle(q, pa, pb, pc)) return false;
        }
        return true;
    };
    std::size_t remaining = n, cur = 0, stall = 0;
    while (remaining > 3) {
        if (isEar(cur)) {
            out.insert(out.end(), {outer[prev[cur]], outer[cur], outer[next[cur]]});
            next[prev[cur]] = next[cur];
            prev[next[cur]] = prev[cur];
            cur = prev[cur];
            --remaining;
            stall = 0;
            continue;
        }
        cur = next[cur];
        if (++stall > remaining) {
            // No clean ear (collinear or self-touching input): clip the flattest corner to make progress.
            std::size_t pick = cur;
            double bestAbs = 1e300;
            std::size_t j = cur;
            for (std::size_t k = 0; k < remaining; ++k, j = next[j]) {
                const double o = orient(P(prev[j]), P(j), P(next[j]));
                if (o >= 0 && o < bestAbs) {
                    bestAbs = o;
                    pick = j;
                }
            }
            out.insert(out.end(), {outer[prev[pick]], outer[pick], outer[next[pick]]});
            next[prev[pick]] = next[pick];
            prev[next[pick]] = prev[pick];
            cur = prev[pick];
            --remaining;
            stall = 0;
        }
    }
    out.insert(out.end(), {outer[prev[cur]], outer[cur], outer[next[cur]]});
    dropSlivers(pts, out);
    return out;
}

}  // namespace wetwaresuite::mesh::detail
