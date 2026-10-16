// BSP-tree CSG. Trees live in flat vectors (indices, not pointers) and every
// traversal uses an explicit stack, so deep trees never overflow the call stack.
#include "achclwss/mesh/csg.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "internal.hpp"

namespace achclwss::mesh {

namespace {

struct Plane {
    Vec3 n;
    double w = 0;
};

struct Poly {
    std::vector<Vec3> v;
    Plane plane;
    std::uint32_t src = 0;  // input face this fragment came from
};

struct Node {
    Plane plane;
    int front = -1, back = -1;  // -1: open space in front, solid behind
};

enum : int { kCoplanar = 0, kFront = 1, kBack = 2, kSpanning = 3 };

bool lexLess(const Vec3& a, const Vec3& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}

// A solid as a BSP tree of its face planes. Only the planes are kept: the
// output polygons come from clipping the original faces, which fragments
// them far less than collecting the tree's own split polygons.
struct Tree {
    std::vector<Node> nodes;
};

class Csg {
public:
    explicit Csg(double eps) : eps_(eps) {}

    void split(const Plane& pl, Poly&& p, std::vector<Poly>& cf, std::vector<Poly>& cb, std::vector<Poly>& f,
               std::vector<Poly>& b) const {
        const std::size_t n = p.v.size();
        int polyType = 0;
        types_.resize(n);
        dists_.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = dot(pl.n, p.v[i]) - pl.w;
            dists_[i] = t;
            const int type = t < -eps_ ? kBack : t > eps_ ? kFront : kCoplanar;
            polyType |= type;
            types_[i] = type;
        }
        switch (polyType) {
            case kCoplanar: (dot(pl.n, p.plane.n) > 0 ? cf : cb).push_back(std::move(p)); return;
            case kFront: f.push_back(std::move(p)); return;
            case kBack: b.push_back(std::move(p)); return;
            default: break;
        }
        Poly fp{{}, p.plane, p.src}, bp{{}, p.plane, p.src};
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const int ti = types_[i], tj = types_[j];
            const Vec3 &vi = p.v[i], &vj = p.v[j];
            if (ti != kBack) fp.v.push_back(vi);
            if (ti != kFront) bp.v.push_back(vi);
            if ((ti | tj) == kSpanning) {
                // Interpolate in a canonical direction so both polygons sharing this edge get the same point.
                const bool fwd = lexLess(vi, vj);
                const Vec3 &a = fwd ? vi : vj, &c = fwd ? vj : vi;
                const double da = fwd ? dists_[i] : dists_[j], dc = fwd ? dists_[j] : dists_[i];
                const Vec3 x = lerp(a, c, da / (da - dc));
                fp.v.push_back(x);
                bp.v.push_back(x);
            }
        }
        if (fp.v.size() >= 3) f.push_back(std::move(fp));
        if (bp.v.size() >= 3) b.push_back(std::move(bp));
    }

    Tree build(std::vector<Poly> polys) const {
        Tree t;
        if (polys.empty()) return t;
        t.nodes.emplace_back();
        std::vector<std::pair<int, std::vector<Poly>>> stack;
        stack.emplace_back(0, std::move(polys));
        std::vector<Poly> coplanar;
        while (!stack.empty()) {
            auto [idx, list] = std::move(stack.back());
            stack.pop_back();
            const auto u = static_cast<std::size_t>(idx);
            t.nodes[u].plane = list[0].plane;
            const Plane pl = t.nodes[u].plane;
            std::vector<Poly> f, b;
            coplanar.clear();
            for (auto& p : list) split(pl, std::move(p), coplanar, coplanar, f, b);
            if (!f.empty()) {
                t.nodes.emplace_back();
                t.nodes[u].front = static_cast<int>(t.nodes.size() - 1);
                stack.emplace_back(t.nodes[u].front, std::move(f));
            }
            if (!b.empty()) {
                t.nodes.emplace_back();
                t.nodes[u].back = static_cast<int>(t.nodes.size() - 1);
                stack.emplace_back(t.nodes[u].back, std::move(b));
            }
        }
        return t;
    }

    // Removes the parts of `polys` inside the solid t (csg.js clipPolygons).
    std::vector<Poly> clip(const Tree& t, std::vector<Poly> polys) const {
        if (t.nodes.empty() || polys.empty()) return polys;
        std::vector<Poly> out;
        std::vector<std::pair<int, std::vector<Poly>>> stack;
        stack.emplace_back(0, std::move(polys));
        while (!stack.empty()) {
            auto [idx, list] = std::move(stack.back());
            stack.pop_back();
            const Node& node = t.nodes[static_cast<std::size_t>(idx)];
            std::vector<Poly> f, b;
            for (auto& p : list) split(node.plane, std::move(p), f, b, f, b);
            if (node.front >= 0) stack.emplace_back(node.front, std::move(f));
            else for (auto& p : f) out.push_back(std::move(p));
            if (node.back >= 0) stack.emplace_back(node.back, std::move(b));
        }
        return out;
    }

    // The complement solid.
    static Tree inverted(Tree t) {
        for (auto& node : t.nodes) {
            node.plane.n = -node.plane.n;
            node.plane.w = -node.plane.w;
            std::swap(node.front, node.back);
        }
        return t;
    }

private:
    double eps_;
    mutable std::vector<int> types_;
    mutable std::vector<double> dists_;
};

std::vector<Poly> flipped(std::vector<Poly> polys) {
    for (auto& p : polys) {
        std::reverse(p.v.begin(), p.v.end());
        p.plane.n = -p.plane.n;
        p.plane.w = -p.plane.w;
    }
    return polys;
}

void addAll(std::vector<Poly>& dst, std::vector<Poly>&& src) {
    for (auto& p : src) dst.push_back(std::move(p));
}

std::vector<Poly> toPolys(const Mesh& m, std::uint32_t firstSrc) {
    std::vector<Poly> out;
    out.reserve(m.triangleCount());
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const Vec3 &a = m.corner(t, 0), &b = m.corner(t, 1), &c = m.corner(t, 2);
        const Vec3 n = cross(b - a, c - a);
        const double len = length(n);
        if (len <= 0) continue;
        Poly p;
        p.v = {a, b, c};
        p.plane.n = n / len;
        p.plane.w = dot(p.plane.n, a);
        p.src = firstSrc + static_cast<std::uint32_t>(out.size());
        out.push_back(std::move(p));
    }
    return out;
}

// Inserts vertices that sit on another polygon's edge (T-junctions) into
// that edge, so neighbouring polygons share every vertex.
void stitch(std::vector<std::vector<std::uint32_t>>& polys, const std::vector<Vec3>& pts, double tol) {
    for (int round = 0; round < 4; ++round) {
        std::unordered_map<std::uint64_t, int> edges;
        for (const auto& p : polys) {
            for (std::size_t i = 0; i < p.size(); ++i) ++edges[detail::edgeKey(p[i], p[(i + 1) % p.size()])];
        }
        auto unmatched = [&](std::uint32_t a, std::uint32_t b) {
            auto it = edges.find(detail::edgeKey(b, a));
            return it == edges.end() || it->second < edges[detail::edgeKey(a, b)];
        };
        std::vector<std::uint32_t> cands;
        {
            std::unordered_set<std::uint32_t> seen;
            for (const auto& p : polys) {
                for (std::size_t i = 0; i < p.size(); ++i) {
                    const std::uint32_t a = p[i], b = p[(i + 1) % p.size()];
                    if (unmatched(a, b)) {
                        if (seen.insert(a).second) cands.push_back(a);
                        if (seen.insert(b).second) cands.push_back(b);
                    }
                }
            }
        }
        if (cands.empty()) return;
        std::sort(cands.begin(), cands.end(), [&](std::uint32_t a, std::uint32_t b) { return pts[a].x < pts[b].x; });
        bool changed = false;
        std::vector<std::pair<double, std::uint32_t>> hits;
        for (auto& p : polys) {
            std::vector<std::uint32_t> np;
            np.reserve(p.size());
            for (std::size_t i = 0; i < p.size(); ++i) {
                const std::uint32_t a = p[i], b = p[(i + 1) % p.size()];
                np.push_back(a);
                if (!unmatched(a, b)) continue;
                const Vec3 &pa = pts[a], &pb = pts[b], d = pb - pa;
                const double len2 = dot(d, d);
                if (len2 <= 0) continue;
                const double lo = std::min(pa.x, pb.x) - tol, hi = std::max(pa.x, pb.x) + tol;
                auto it = std::lower_bound(cands.begin(), cands.end(), lo, [&](std::uint32_t c, double x) { return pts[c].x < x; });
                hits.clear();
                for (; it != cands.end() && pts[*it].x <= hi; ++it) {
                    const std::uint32_t c = *it;
                    if (c == a || c == b) continue;
                    const Vec3 q = pts[c] - pa;
                    const double t = dot(q, d) / len2;
                    if (t <= 0 || t >= 1) continue;
                    const Vec3 off = q - d * t;
                    if (dot(off, off) <= tol * tol) hits.emplace_back(t, c);
                }
                if (hits.empty()) continue;
                std::sort(hits.begin(), hits.end());
                for (const auto& h : hits) {
                    if (np.back() != h.second) np.push_back(h.second);
                }
                changed = true;
            }
            p = std::move(np);
        }
        if (!changed) return;
    }
}

Mesh toMesh(const std::vector<Poly>& polys, double eps) {
    detail::Welder welder(eps);
    std::vector<std::vector<std::uint32_t>> rings;
    rings.reserve(polys.size());
    for (const auto& p : polys) {
        std::vector<std::uint32_t> r;
        for (const auto& v : p.v) {
            const std::uint32_t id = welder.add(v);
            if (r.empty() || r.back() != id) r.push_back(id);
        }
        while (r.size() > 1 && r.front() == r.back()) r.pop_back();
        if (r.size() >= 3) rings.push_back(std::move(r));
    }
    stitch(rings, welder.points, eps * 4);

    Mesh m;
    m.positions = std::move(welder.points);
    for (const auto& r : rings) {
        const std::size_t n = r.size();
        bool collinear = false;
        if (n > 3) {
            for (std::size_t i = 0; i < n && !collinear; ++i) {
                const Vec3 &a = m.positions[r[(i + n - 1) % n]], &b = m.positions[r[i]], &c = m.positions[r[(i + 1) % n]];
                const Vec3 u = b - a, w = c - b;
                collinear = length(cross(u, w)) <= 1e-9 * length(u) * length(w);
            }
        }
        if (!collinear) {
            for (std::size_t i = 1; i + 1 < n; ++i) m.addTriangle(r[0], r[i], r[i + 1]);
        } else {
            Vec3 c;
            for (auto id : r) c += m.positions[id];
            const std::uint32_t ci = m.addVertex(c / static_cast<double>(n));
            for (std::size_t i = 0; i < n; ++i) m.addTriangle(ci, r[i], r[(i + 1) % n]);
        }
    }
    cleanup(m);
    return m;
}

double polyArea(const Poly& p) {
    Vec3 s;
    for (std::size_t i = 0; i < p.v.size(); ++i) s += cross(p.v[i], p.v[(i + 1) % p.v.size()]);
    return 0.5 * dot(s, p.plane.n);
}

// BSP clipping splits faces by every plane on their way down the tree, even
// far from the other solid. Where every fragment of a face survived, put the
// whole face back (T-junctions it creates are stitched later).
std::vector<Poly> reassemble(std::vector<Poly> result, const std::vector<Poly>& originals) {
    std::vector<double> kept(originals.size(), 0.0);
    std::vector<int> pieces(originals.size(), 0);
    for (const auto& p : result) {
        kept[p.src] += std::fabs(polyArea(p));
        ++pieces[p.src];
    }
    std::vector<bool> whole(originals.size(), false), emitted(originals.size(), false);
    for (std::size_t i = 0; i < originals.size(); ++i) {
        whole[i] = pieces[i] > 1 && kept[i] >= std::fabs(polyArea(originals[i])) * (1 - 1e-9);
    }
    std::vector<Poly> out;
    out.reserve(result.size());
    for (auto& p : result) {
        if (!whole[p.src]) {
            out.push_back(std::move(p));
            continue;
        }
        if (emitted[p.src]) continue;
        emitted[p.src] = true;
        Poly o = originals[p.src];
        if (dot(o.plane.n, p.plane.n) < 0) {
            std::reverse(o.v.begin(), o.v.end());
            o.plane.n = -o.plane.n;
            o.plane.w = -o.plane.w;
        }
        out.push_back(std::move(o));
    }
    return out;
}

bool overlaps(const Bounds& a, const Bounds& b) {
    return !(a.empty() || b.empty() || a.max.x < b.min.x || b.max.x < a.min.x || a.max.y < b.min.y || b.max.y < a.min.y ||
             a.max.z < b.min.z || b.max.z < a.min.z);
}

}  // namespace

Mesh booleanOp(const Mesh& a, const Mesh& b, BooleanOp op) {
    const Bounds ba = bounds(a), bb = bounds(b);
    if (!overlaps(ba, bb)) {
        Mesh out;
        if (op == BooleanOp::Union) {
            out = a;
            append(out, b);
        } else if (op == BooleanOp::Difference) {
            out = a;
        }
        out.normals.clear();
        out.uvs.clear();
        return out;
    }
    Bounds all = ba;
    all.merge(bb);
    const double eps = std::max(1e-10, all.diagonal() * 1e-7);
    // Faces entirely outside the other solid's box can't touch it: they skip clipping.
    Bounds common;
    common.min = max(ba.min, bb.min) - Vec3{eps, eps, eps} * 10;
    common.max = min(ba.max, bb.max) + Vec3{eps, eps, eps} * 10;
    std::vector<Poly> originals = toPolys(a, 0);
    const std::size_t countA = originals.size();
    {
        auto pb = toPolys(b, static_cast<std::uint32_t>(countA));
        originals.insert(originals.end(), pb.begin(), pb.end());
    }
    std::vector<Poly> aIn, aOut, bIn, bOut;
    for (std::size_t i = 0; i < originals.size(); ++i) {
        Bounds pb;
        for (const auto& v : originals[i].v) pb.add(v);
        const bool isA = i < countA;
        (overlaps(pb, common) ? (isA ? aIn : bIn) : (isA ? aOut : bOut)).push_back(originals[i]);
    }

    Csg csg(eps);
    std::vector<Poly> all1 = aIn, all2 = bIn;
    all1.insert(all1.end(), aOut.begin(), aOut.end());
    all2.insert(all2.end(), bOut.begin(), bOut.end());
    const Tree ta = csg.build(std::move(all1)), tb = csg.build(std::move(all2));

    // The csg.js recipes, written as clips of the original faces.
    std::vector<Poly> result;
    switch (op) {
        case BooleanOp::Union:
            addAll(result, csg.clip(tb, std::move(aIn)));
            addAll(result, flipped(csg.clip(ta, flipped(csg.clip(ta, std::move(bIn))))));
            addAll(result, std::move(aOut));
            addAll(result, std::move(bOut));
            break;
        case BooleanOp::Difference: {
            const Tree na = Csg::inverted(ta);
            addAll(result, flipped(csg.clip(tb, flipped(std::move(aIn)))));
            addAll(result, csg.clip(na, flipped(csg.clip(na, std::move(bIn)))));
            addAll(result, std::move(aOut));
            break;
        }
        case BooleanOp::Intersection: {
            const Tree na = Csg::inverted(ta), nb = Csg::inverted(tb);
            addAll(result, flipped(csg.clip(nb, flipped(std::move(aIn)))));
            addAll(result, flipped(csg.clip(na, flipped(csg.clip(na, std::move(bIn))))));
            break;
        }
    }
    return toMesh(reassemble(std::move(result), originals), eps);
}

}  // namespace achclwss::mesh
