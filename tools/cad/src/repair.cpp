// Coplanar region merging for boolean results: group flat triangles into
// regions, trace each region's outline, drop vertices that only sit on a
// straight edge between two regions, and ear-clip the outlines again.
#include "repair.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace achclwss::cad {

using mesh::Mesh;
using mesh::Vec2;
using mesh::Vec3;

namespace {

std::uint64_t key(std::uint32_t a, std::uint32_t b) { return (static_cast<std::uint64_t>(a) << 32) | b; }

double orient(const Vec2& a, const Vec2& b, const Vec2& c) { return cross(b - a, c - a); }

bool segmentsCross(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
    if (a == c || a == d || b == c || b == d) return false;
    const double d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return true;
    auto on = [](const Vec2& p, const Vec2& q, const Vec2& r) {
        return std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x) && std::min(p.y, q.y) <= r.y && r.y <= std::max(p.y, q.y);
    };
    return (d1 == 0 && on(c, d, a)) || (d2 == 0 && on(c, d, b)) || (d3 == 0 && on(a, b, c)) || (d4 == 0 && on(a, b, d));
}

double ringArea(const std::vector<Vec2>& pts, const std::vector<std::uint32_t>& ring) {
    double a = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) a += cross(pts[ring[i]], pts[ring[(i + 1) % ring.size()]]);
    return a / 2;
}

bool insideRing(const std::vector<Vec2>& pts, const std::vector<std::uint32_t>& ring, const Vec2& p) {
    bool in = false;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const Vec2 &a = pts[ring[i]], &b = pts[ring[j]];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

// Splices each hole into the outer ring through a bridge to a visible vertex.
bool bridgeHoles(const std::vector<Vec2>& pts, std::vector<std::uint32_t>& outer, std::vector<std::vector<std::uint32_t>> holes) {
    auto maxX = [&](const std::vector<std::uint32_t>& r) {
        double x = -1e300;
        for (auto i : r) x = std::max(x, pts[i].x);
        return x;
    };
    std::sort(holes.begin(), holes.end(), [&](const auto& a, const auto& b) { return maxX(a) > maxX(b); });
    for (std::size_t h = 0; h < holes.size(); ++h) {
        const auto& hole = holes[h];
        std::size_t m = 0;
        for (std::size_t i = 1; i < hole.size(); ++i) {
            if (pts[hole[i]].x > pts[hole[m]].x) m = i;
        }
        const Vec2 M = pts[hole[m]];
        std::vector<std::size_t> order(outer.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        auto dist = [&](std::size_t i) {
            const Vec2 d = pts[outer[i]] - M;
            // prefer vertices to the right, and ones that aren't already a bridge end
            const bool used = std::count(outer.begin(), outer.end(), outer[i]) > 1;
            return dot(d, d) + (pts[outer[i]].x <= M.x ? 1e300 : 0.0) + (used ? 1e200 : 0.0);
        };
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return dist(a) < dist(b); });
        auto blocks = [&](const std::vector<std::uint32_t>& r, const Vec2& P) {
            for (std::size_t i = 0; i < r.size(); ++i) {
                if (segmentsCross(M, P, pts[r[i]], pts[r[(i + 1) % r.size()]])) return true;
            }
            return false;
        };
        bool found = false;
        std::size_t best = 0;
        for (std::size_t i : order) {
            const Vec2 P = pts[outer[i]];
            bool blocked = blocks(outer, P);
            for (std::size_t k = h; k < holes.size() && !blocked; ++k) blocked = blocks(holes[k], P);
            if (!blocked) {
                best = i;
                found = true;
                break;
            }
        }
        if (!found) return false;
        std::vector<std::uint32_t> merged(outer.begin(), outer.begin() + static_cast<std::ptrdiff_t>(best) + 1);
        for (std::size_t k = 0; k <= hole.size(); ++k) merged.push_back(hole[(m + k) % hole.size()]);
        merged.insert(merged.end(), outer.begin() + static_cast<std::ptrdiff_t>(best), outer.end());
        outer = std::move(merged);
    }
    return true;
}

// Ear clipping of one counter-clockwise ring; false when it gets stuck.
bool earClip(const std::vector<Vec2>& pts, const std::vector<std::uint32_t>& ring, std::vector<std::uint32_t>& out) {
    const std::size_t n = ring.size();
    if (n < 3) return false;
    std::vector<std::size_t> prev(n), next(n);
    for (std::size_t i = 0; i < n; ++i) {
        prev[i] = (i + n - 1) % n;
        next[i] = (i + 1) % n;
    }
    auto P = [&](std::size_t i) -> const Vec2& { return pts[ring[i]]; };
    auto isEar = [&](std::size_t i) {
        const std::size_t a = prev[i], c = next[i];
        const Vec2 &pa = P(a), &pb = P(i), &pc = P(c);
        const double o = orient(pa, pb, pc);
        if (!(o > 1e-12 * length(pb - pa) * length(pc - pb))) return false;
        for (std::size_t j = next[c]; j != a; j = next[j]) {
            const Vec2& q = P(j);
            if (q == pa || q == pb || q == pc) continue;
            if (orient(P(prev[j]), q, P(next[j])) > 0) continue;  // only reflex vertices can poke in
            if (orient(pa, pb, q) >= 0 && orient(pb, pc, q) >= 0 && orient(pc, pa, q) >= 0) return false;
        }
        return true;
    };
    std::size_t remaining = n, cur = 0, stall = 0;
    while (remaining > 3) {
        if (isEar(cur)) {
            out.insert(out.end(), {ring[prev[cur]], ring[cur], ring[next[cur]]});
            next[prev[cur]] = next[cur];
            prev[next[cur]] = prev[cur];
            cur = prev[cur];
            --remaining;
            stall = 0;
            continue;
        }
        cur = next[cur];
        if (++stall > remaining) return false;
    }
    if (!(orient(P(prev[cur]), P(cur), P(next[cur])) > 0)) return false;
    out.insert(out.end(), {ring[prev[cur]], ring[cur], ring[next[cur]]});
    return true;
}

bool noFolds(const Mesh& m) {
    std::unordered_map<std::uint64_t, std::size_t> owner;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        for (int k = 0; k < 3; ++k) owner[key(m.indices[t * 3 + k], m.indices[t * 3 + (k + 1) % 3])] = t;
    }
    for (const auto& [e, t] : owner) {
        auto it = owner.find(key(static_cast<std::uint32_t>(e & 0xffffffffu), static_cast<std::uint32_t>(e >> 32)));
        if (it == owner.end()) return false;
        const Vec3 n1 = m.faceNormal(t), n2 = m.faceNormal(it->second);
        if (dot(n1, n2) < -0.999 * length(n1) * length(n2)) return false;
    }
    return true;
}

}  // namespace

std::optional<Mesh> mergeCoplanar(const Mesh& in) {
    if (in.empty() || !mesh::isWatertight(in)) return std::nullopt;
    const std::size_t T = in.triangleCount();
    const double diag = mesh::bounds(in).diagonal();
    const double tol = std::max(diag * 1e-8, 1e-12);

    std::unordered_map<std::uint64_t, std::size_t> edgeTri;
    edgeTri.reserve(T * 3);
    std::vector<double> area(T);
    for (std::size_t t = 0; t < T; ++t) {
        for (int k = 0; k < 3; ++k) edgeTri[key(in.indices[t * 3 + k], in.indices[t * 3 + (k + 1) % 3])] = t;
        area[t] = length(in.faceNormal(t));
    }
    std::vector<std::size_t> order(T);
    for (std::size_t t = 0; t < T; ++t) order[t] = t;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return area[a] > area[b]; });

    struct Region {
        Vec3 n;
        double d = 0;
        std::vector<std::size_t> tris;
    };
    std::vector<Region> regions;
    std::vector<int> region(T, -1);
    auto onPlane = [&](const Region& r, std::size_t t) {
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(dot(r.n, in.corner(t, k)) - r.d) > tol) return false;
        }
        return true;
    };
    for (std::size_t seed : order) {
        if (region[seed] >= 0) continue;
        if (!(area[seed] > 0)) return std::nullopt;
        Region r;
        r.n = in.faceNormal(seed) / area[seed];
        r.d = dot(r.n, in.corner(seed, 0));
        const int id = static_cast<int>(regions.size());
        region[seed] = id;
        std::vector<std::size_t> queue{seed};
        while (!queue.empty()) {
            const std::size_t t = queue.back();
            queue.pop_back();
            r.tris.push_back(t);
            for (int k = 0; k < 3; ++k) {
                auto it = edgeTri.find(key(in.indices[t * 3 + (k + 1) % 3], in.indices[t * 3 + k]));
                if (it == edgeTri.end()) return std::nullopt;
                const std::size_t nb = it->second;
                if (region[nb] < 0 && onPlane(r, nb)) {
                    region[nb] = id;
                    queue.push_back(nb);
                }
            }
        }
        regions.push_back(std::move(r));
    }
    if (regions.size() == T) return std::nullopt;  // nothing to merge

    // which regions each vertex touches
    std::vector<std::vector<int>> vertexRegions(in.vertexCount());
    for (std::size_t t = 0; t < T; ++t) {
        for (int k = 0; k < 3; ++k) {
            auto& list = vertexRegions[in.indices[t * 3 + k]];
            if (std::find(list.begin(), list.end(), region[t]) == list.end()) list.push_back(region[t]);
        }
    }

    // outlines: directed edges that don't cancel inside the region
    std::vector<std::vector<std::vector<std::uint32_t>>> loops(regions.size());
    std::vector<char> removable(in.vertexCount(), 0);
    for (std::size_t ri = 0; ri < regions.size(); ++ri) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> count;
        for (std::size_t t : regions[ri].tris) {
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t a = in.indices[t * 3 + k], b = in.indices[t * 3 + (k + 1) % 3];
                auto back = count.find({b, a});
                if (back != count.end() && back->second > 0) {
                    if (--back->second == 0) count.erase(back);
                } else {
                    ++count[{a, b}];
                }
            }
        }
        std::unordered_map<std::uint32_t, std::uint32_t> nextOf;
        for (const auto& [e, c] : count) {
            if (c != 1 || !nextOf.emplace(e.first, e.second).second) return std::nullopt;  // pinched outline
        }
        while (!nextOf.empty()) {
            std::vector<std::uint32_t> loop;
            std::uint32_t v = nextOf.begin()->first;
            while (true) {
                auto it = nextOf.find(v);
                if (it == nextOf.end()) break;
                loop.push_back(v);
                v = it->second;
                nextOf.erase(it);
            }
            if (loop.size() < 3 || v != loop.front()) return std::nullopt;
            loops[ri].push_back(std::move(loop));
        }
        // a vertex between exactly two regions, on a straight line, carries nothing
        for (const auto& loop : loops[ri]) {
            const std::size_t n = loop.size();
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t v = loop[i];
                if (vertexRegions[v].size() != 2 || vertexRegions[v][0] != static_cast<int>(ri)) continue;
                const Vec3 &u = in.positions[loop[(i + n - 1) % n]], &p = in.positions[v], &w = in.positions[loop[(i + 1) % n]];
                const Vec3 d = w - u;
                const double len = length(d);
                if (len <= 0) continue;
                const double off = length(cross(p - u, d)) / len, t = dot(p - u, d) / (len * len);
                if (off <= tol && t > 0 && t < 1) removable[v] = 1;
            }
        }
    }

    // Re-triangulates region ri from its outline into out; false if it can't.
    auto rebuild = [&](std::size_t ri, const std::vector<char>& remove, Mesh& out) -> bool {
        const Region& r = regions[ri];
        std::vector<std::vector<std::uint32_t>> rings;  // indices into in.positions
        for (const auto& loop : loops[ri]) {
            std::vector<std::uint32_t> kept;
            for (auto v : loop) {
                if (!remove[v]) kept.push_back(v);
            }
            if (kept.size() < 3) return false;
            rings.push_back(std::move(kept));
        }
        if (rings.size() == 1 && rings[0].size() == 3) {
            out.addTriangle(rings[0][0], rings[0][1], rings[0][2]);
            return true;
        }
        // flatten into the region's plane, counter-clockwise seen along n
        const Vec3 helper = std::fabs(r.n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 ax = normalize(cross(helper, r.n)), ay = cross(r.n, ax);
        std::vector<Vec2> pts;
        std::vector<std::uint32_t> global;
        std::vector<std::vector<std::uint32_t>> local;
        for (const auto& ring : rings) {
            std::vector<std::uint32_t> l;
            for (auto v : ring) {
                l.push_back(static_cast<std::uint32_t>(pts.size()));
                pts.push_back({dot(in.positions[v], ax), dot(in.positions[v], ay)});
                global.push_back(v);
            }
            local.push_back(std::move(l));
        }
        std::vector<std::size_t> outers, holes;
        for (std::size_t i = 0; i < local.size(); ++i) (ringArea(pts, local[i]) > 0 ? outers : holes).push_back(i);
        std::vector<std::vector<std::size_t>> holesOf(local.size());
        for (std::size_t h : holes) {
            std::size_t best = local.size();
            double bestArea = 1e300;
            for (std::size_t o : outers) {
                const double a = ringArea(pts, local[o]);
                if (a < bestArea && insideRing(pts, local[o], pts[local[h][0]])) {
                    best = o;
                    bestArea = a;
                }
            }
            if (best == local.size()) return false;
            holesOf[best].push_back(h);
        }
        std::vector<std::uint32_t> tris;
        for (std::size_t o : outers) {
            std::vector<std::uint32_t> ring = local[o];
            std::vector<std::vector<std::uint32_t>> hs;
            for (std::size_t h : holesOf[o]) hs.push_back(local[h]);
            if (!bridgeHoles(pts, ring, hs) || !earClip(pts, ring, tris)) return false;
        }
        for (std::size_t i = 0; i + 2 < tris.size(); i += 3) out.addTriangle(global[tris[i]], global[tris[i + 1]], global[tris[i + 2]]);
        return true;
    };

    // A region that won't re-triangulate keeps its triangles, and its
    // vertices stay so its neighbours still meet it; then try again.
    std::vector<char> keep(regions.size(), 0);
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::vector<char> remove = removable;
        for (std::size_t ri = 0; ri < regions.size(); ++ri) {
            if (!keep[ri]) continue;
            for (std::size_t t : regions[ri].tris) {
                for (int k = 0; k < 3; ++k) remove[in.indices[t * 3 + k]] = 0;
            }
        }
        Mesh out;
        out.positions = in.positions;
        bool failed = false;
        for (std::size_t ri = 0; ri < regions.size() && !failed; ++ri) {
            if (keep[ri]) {
                for (std::size_t t : regions[ri].tris) out.addTriangle(in.indices[t * 3], in.indices[t * 3 + 1], in.indices[t * 3 + 2]);
            } else if (!rebuild(ri, remove, out)) {
                keep[ri] = 1;
                failed = true;
            }
        }
        if (failed) continue;
        mesh::cleanup(out);
        const double v0 = mesh::volume(in), v1 = mesh::volume(out);
        if (!mesh::isWatertight(out) || std::fabs(v1 - v0) > 1e-7 * std::fabs(v0) + tol * tol || !noFolds(out)) return std::nullopt;
        return out;
    }
    return std::nullopt;
}

}  // namespace achclwss::cad
