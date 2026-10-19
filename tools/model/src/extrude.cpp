// Extrude, inset, duplicate, spin, mirror and moving the selection.
#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "wetwaresuite/model/ops.hpp"

namespace wetwaresuite::model {

namespace {

// Connected areas of selected faces (through edges both selected faces share).
std::vector<int> faceRegions(const PolyMesh& m, const Topology& t, int& count) {
    std::vector<int> region(m.faces.size(), -1);
    count = 0;
    for (std::uint32_t f0 = 0; f0 < m.faces.size(); ++f0) {
        if (!m.faces[f0].sel || region[f0] >= 0) continue;
        std::vector<std::uint32_t> stack{f0};
        region[f0] = count;
        while (!stack.empty()) {
            const std::uint32_t f = stack.back();
            stack.pop_back();
            const auto& fv = m.faces[f].v;
            for (std::size_t i = 0; i < fv.size(); ++i) {
                for (const auto& h : t.edgeFaces[t.index.at(edgeKey(fv[i], fv[(i + 1) % fv.size()]))]) {
                    if (m.faces[h.face].sel && region[h.face] < 0) {
                        region[h.face] = count;
                        stack.push_back(h.face);
                    }
                }
            }
        }
        ++count;
    }
    return region;
}

// Region extrude without moving: selected faces get new vertices, joined to
// the old outline by side quads. Returns the new vertex of each old one
// (UINT32_MAX where none) and the number of side faces.
std::size_t extrudeRegionInPlace(PolyMesh& m, std::vector<std::uint32_t>& dup) {
    Topology t(m);
    dup.assign(m.verts.size(), UINT32_MAX);
    for (const auto& f : m.faces)
        if (f.sel)
            for (auto v : f.v)
                if (dup[v] == UINT32_MAX) dup[v] = m.addVertex(m.verts[v].p);
    for (std::size_t v = 0; v < dup.size(); ++v)
        if (dup[v] != UINT32_MAX) m.verts[dup[v]].color = m.verts[v].color;
    std::vector<Face> sides;
    for (std::uint32_t fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        if (!f.sel) continue;
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t a = f.v[i], b = f.v[(i + 1) % n];
            std::size_t selCount = 0;
            for (const auto& h : t.edgeFaces[t.index.at(edgeKey(a, b))]) selCount += m.faces[h.face].sel ? 1 : 0;
            if (selCount != 1) continue;  // inside the region
            Face s;
            s.v = {a, b, dup[b], dup[a]};
            s.material = f.material;
            s.smooth = f.smooth;
            sides.push_back(std::move(s));
        }
    }
    for (auto& f : m.faces)
        if (f.sel)
            for (auto& v : f.v) v = dup[v];
    const std::size_t made = sides.size();
    for (auto& s : sides) m.faces.push_back(std::move(s));
    return made;
}

// Each selected face extruded on its own; returns side faces made and fills
// owner with the face each new vertex belongs to.
std::size_t extrudeIndividualInPlace(PolyMesh& m) {
    std::vector<Face> sides;
    const std::size_t faceCount = m.faces.size();
    for (std::size_t fi = 0; fi < faceCount; ++fi) {
        if (!m.faces[fi].sel) continue;
        std::vector<std::uint32_t> old = m.faces[fi].v;
        std::vector<std::uint32_t> nv;
        for (auto v : old) {
            nv.push_back(m.addVertex(m.verts[v].p));
            m.verts[nv.back()].color = m.verts[v].color;
        }
        const std::size_t n = old.size();
        for (std::size_t i = 0; i < n; ++i) {
            Face s;
            s.v = {old[i], old[(i + 1) % n], nv[(i + 1) % n], nv[i]};
            s.material = m.faces[fi].material;
            s.smooth = m.faces[fi].smooth;
            sides.push_back(std::move(s));
        }
        m.faces[fi].v = nv;
    }
    const std::size_t made = sides.size();
    for (auto& s : sides) m.faces.push_back(std::move(s));
    return made;
}

}  // namespace

std::size_t extrude(PolyMesh& m, ExtrudeMode mode, double offset, const std::optional<Vec3>& direction) {
    bool anyFace = false;
    for (const auto& f : m.faces) anyFace = anyFace || f.sel;
    if (anyFace) {
        // moves are worked out before the topology changes
        std::vector<Vec3> move;
        if (mode == ExtrudeMode::Region) {
            Topology t(m);
            int regions = 0;
            const auto region = faceRegions(m, t, regions);
            std::vector<Vec3> rn(static_cast<std::size_t>(regions));
            for (std::size_t f = 0; f < m.faces.size(); ++f)
                if (region[f] >= 0) rn[static_cast<std::size_t>(region[f])] += faceNormal(m, m.faces[f]) * faceArea(m, m.faces[f]);
            for (auto& n : rn) n = mesh::normalize(n);
            const std::size_t before = m.faces.size();
            std::vector<std::uint32_t> dup;
            const std::size_t made = extrudeRegionInPlace(m, dup);
            std::vector<Vec3> delta(m.verts.size());
            std::vector<char> moved(m.verts.size(), 0);
            for (std::size_t f = 0; f < before; ++f) {
                if (region[f] < 0) continue;
                const Vec3 d = direction ? *direction : rn[static_cast<std::size_t>(region[f])] * offset;
                for (auto v : m.faces[f].v) {
                    delta[v] = d;
                    moved[v] = 1;
                }
            }
            for (std::size_t v = 0; v < m.verts.size(); ++v)
                if (moved[v]) m.verts[v].p += delta[v];
            for (std::size_t f = 0; f < m.faces.size(); ++f) m.faces[f].sel = f < before && region[f] >= 0;
            cleanup(m);
            flushSelection(m, SelectMode::Face);
            return made;
        }
        std::vector<Vec3> fn;
        std::vector<std::size_t> sel;
        for (std::size_t f = 0; f < m.faces.size(); ++f)
            if (m.faces[f].sel) {
                sel.push_back(f);
                fn.push_back(faceNormal(m, m.faces[f]));
            }
        const std::size_t made = extrudeIndividualInPlace(m);
        for (std::size_t i = 0; i < sel.size(); ++i) {
            const Vec3 d = direction ? *direction : fn[i] * offset;
            for (auto v : m.faces[sel[i]].v) m.verts[v].p += d;
        }
        cleanup(m);
        flushSelection(m, SelectMode::Face);
        return made;
    }
    // edges and lone vertices
    Topology t(m);
    std::vector<EdgeKey> edges;
    for (auto k : m.esel)
        if (t.index.count(k)) edges.push_back(k);
    std::vector<char> inEdge(m.verts.size(), 0);
    for (auto k : edges) inEdge[keyA(k)] = inEdge[keyB(k)] = 1;
    std::vector<std::uint32_t> lone;
    for (std::uint32_t v = 0; v < m.verts.size(); ++v)
        if (m.verts[v].sel && !inEdge[v]) lone.push_back(v);
    if (edges.empty() && lone.empty()) return 0;
    Vec3 d;
    if (direction) {
        d = *direction;
    } else {
        const auto vn = vertexNormals(m);
        Vec3 n;
        for (std::uint32_t v = 0; v < m.verts.size(); ++v)
            if (m.verts[v].sel) n += vn[v];
        n = mesh::normalize(n);
        if (mesh::length(n) == 0) n = {0, 0, 1};
        d = n * offset;
    }
    std::unordered_map<std::uint32_t, std::uint32_t> dup;
    auto copyOf = [&](std::uint32_t v) {
        auto it = dup.find(v);
        if (it != dup.end()) return it->second;
        const std::uint32_t nv = m.addVertex(m.verts[v].p + d);
        m.verts[nv].color = m.verts[v].color;
        dup.emplace(v, nv);
        return nv;
    };
    std::size_t made = 0;
    for (auto k : edges) {
        std::uint32_t a = keyA(k), b = keyB(k);
        // wind the new face against the face already on this edge
        const auto& ef = t.edgeFaces[t.index.at(k)];
        int mat = 0;
        bool smooth = false;
        if (ef.size() == 1) {
            const Face& f = m.faces[ef[0].face];
            if (f.v[ef[0].corner] == a) std::swap(a, b);  // that face walks a->b, so this one walks b->a
            mat = f.material;
            smooth = f.smooth;
        }
        const std::uint32_t na = copyOf(a), nb = copyOf(b);
        m.addFace({a, b, nb, na}, mat, smooth);
        ++made;
    }
    for (auto v : lone) {
        const std::uint32_t nv = copyOf(v);
        m.loose.push_back({v, nv});
        ++made;
    }
    for (auto& v : m.verts) v.sel = false;
    for (auto& f : m.faces) f.sel = false;
    m.esel.clear();
    for (const auto& [o, n] : dup) m.verts[n].sel = true;
    cleanup(m);
    flushSelection(m, SelectMode::Vertex);
    return made;
}

std::size_t inset(PolyMesh& m, double thickness, double depth, bool individual) {
    bool anyFace = false;
    for (const auto& f : m.faces) anyFace = anyFace || f.sel;
    if (!anyFace) return 0;
    std::size_t made = 0;
    const std::size_t before = m.faces.size();
    std::vector<char> wasSel(before);
    for (std::size_t f = 0; f < before; ++f) wasSel[f] = m.faces[f].sel;
    if (individual) {
        made = extrudeIndividualInPlace(m);
    } else {
        std::vector<std::uint32_t> dup;
        made = extrudeRegionInPlace(m, dup);
    }
    // the inner faces are the old selected faces (indices < before); their outline moves in
    Topology t(m);
    std::vector<Vec3> offs(m.verts.size());
    std::vector<int> hits(m.verts.size(), 0);
    std::vector<Vec3> regionNormal(m.verts.size());
    for (std::size_t fi = 0; fi < before; ++fi) {
        if (!wasSel[fi]) continue;
        const Face& f = m.faces[fi];
        const Vec3 nrm = faceNormal(m, f);
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) regionNormal[f.v[i]] += nrm * faceArea(m, f);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t a = f.v[i], b = f.v[(i + 1) % n];
            // an outline edge is shared with a side face (not with another inner face)
            bool outline = false;
            for (const auto& h : t.edgeFaces[t.index.at(edgeKey(a, b))])
                if (h.face >= before) outline = true;
            if (!outline) continue;
            const Vec3 in = mesh::normalize(mesh::cross(nrm, m.verts[b].p - m.verts[a].p));
            offs[a] += in;
            offs[b] += in;
            hits[a]++;
            hits[b]++;
        }
    }
    std::vector<Vec3> moveTo(m.verts.size());
    for (std::size_t v = 0; v < m.verts.size(); ++v) {
        if (hits[v] == 0) continue;
        // with two outline edges the two inward unit vectors in1, in2 sum to s;
        // the miter point is t (in1 + in2) / (1 + in1.in2) = t s / (|s|^2 / 2)
        const Vec3 s = offs[v];
        const double l2 = mesh::dot(s, s);
        Vec3 off;
        if (hits[v] == 2 && l2 > 1e-12) off = s * (2.0 / std::max(l2, 0.1)) * thickness;
        else off = mesh::normalize(s) * thickness;
        moveTo[v] = off;
    }
    for (std::size_t v = 0; v < m.verts.size(); ++v) m.verts[v].p += moveTo[v];
    if (depth != 0) {
        std::vector<char> inner(m.verts.size(), 0);
        for (std::size_t fi = 0; fi < before; ++fi)
            if (wasSel[fi])
                for (auto v : m.faces[fi].v) inner[v] = 1;
        if (individual) {
            for (std::size_t fi = 0; fi < before; ++fi) {
                if (!wasSel[fi]) continue;
                const Vec3 nrm = faceNormal(m, m.faces[fi]);
                for (auto v : m.faces[fi].v) m.verts[v].p += nrm * depth;
            }
        } else {
            for (std::size_t v = 0; v < m.verts.size(); ++v)
                if (inner[v]) m.verts[v].p += mesh::normalize(regionNormal[v]) * depth;
        }
    }
    for (std::size_t f = 0; f < m.faces.size(); ++f) m.faces[f].sel = f < before && wasSel[f];
    cleanup(m);
    flushSelection(m, SelectMode::Face);
    return made;
}

std::size_t duplicate(PolyMesh& m) {
    std::vector<std::uint32_t> dup(m.verts.size(), UINT32_MAX);
    auto copyOf = [&](std::uint32_t v) {
        if (dup[v] == UINT32_MAX) {
            dup[v] = m.addVertex(m.verts[v].p);
            m.verts[dup[v]].color = m.verts[v].color;
        }
        return dup[v];
    };
    std::vector<Face> made;
    for (const auto& f : m.faces) {
        if (!f.sel) continue;
        Face g = f;
        for (auto& v : g.v) v = copyOf(v);
        made.push_back(std::move(g));
    }
    std::set<EdgeKey> faceEdges;
    for (const auto& f : m.faces)
        if (f.sel)
            for (std::size_t i = 0; i < f.v.size(); ++i) faceEdges.insert(edgeKey(f.v[i], f.v[(i + 1) % f.v.size()]));
    std::vector<std::array<std::uint32_t, 2>> loose;
    for (auto k : m.esel)
        if (!faceEdges.count(k)) loose.push_back({copyOf(keyA(k)), copyOf(keyB(k))});
    const std::size_t count = made.size() + loose.size();
    for (auto& f : m.faces) f.sel = false;
    for (auto& v : m.verts) v.sel = false;
    m.esel.clear();
    for (auto& f : made) {
        f.sel = true;
        m.faces.push_back(std::move(f));
    }
    for (const auto& e : loose) m.loose.push_back(e);
    for (std::size_t v = 0; v < dup.size(); ++v)
        if (dup[v] != UINT32_MAX) m.verts[dup[v]].sel = true;
    cleanup(m);
    flushSelection(m, SelectMode::Vertex);
    return count;
}

std::size_t transformSelected(PolyMesh& m, const Mat4& t, double proportional) {
    std::vector<std::uint32_t> sel = selectedVerts(m);
    if (sel.empty()) return 0;
    std::vector<Vec3> start(m.verts.size());
    for (std::size_t v = 0; v < m.verts.size(); ++v) start[v] = m.verts[v].p;
    for (auto v : sel) m.verts[v].p = t.point(start[v]);
    std::size_t moved = sel.size();
    if (proportional > 0) {
        // nearest selected vertex through a grid of cell = radius
        const double r = proportional;
        struct Cell {
            long long x, y, z;
            bool operator==(const Cell&) const = default;
        };
        struct CellHash {
            std::size_t operator()(const Cell& c) const {
                return static_cast<std::size_t>(c.x * 73856093LL ^ c.y * 19349663LL ^ c.z * 83492791LL);
            }
        };
        auto cellOf = [&](const Vec3& p) { return Cell{static_cast<long long>(std::floor(p.x / r)), static_cast<long long>(std::floor(p.y / r)), static_cast<long long>(std::floor(p.z / r))}; };
        std::unordered_map<Cell, std::vector<std::uint32_t>, CellHash> grid;
        for (auto v : sel) grid[cellOf(start[v])].push_back(v);
        for (std::uint32_t v = 0; v < m.verts.size(); ++v) {
            if (m.verts[v].sel) continue;
            const Cell c = cellOf(start[v]);
            double best = r;
            for (long long dx = -1; dx <= 1; ++dx)
                for (long long dy = -1; dy <= 1; ++dy)
                    for (long long dz = -1; dz <= 1; ++dz) {
                        auto it = grid.find({c.x + dx, c.y + dy, c.z + dz});
                        if (it == grid.end()) continue;
                        for (auto s : it->second) best = std::min(best, mesh::length(start[s] - start[v]));
                    }
            if (best >= r) continue;
            const double x = best / r;
            const double w = 1 - x * x * (3 - 2 * x);  // smooth falloff
            m.verts[v].p = mesh::lerp(start[v], t.point(start[v]), w);
            ++moved;
        }
    }
    return moved;
}

std::size_t shrinkFatten(PolyMesh& m, double offset) {
    const auto vn = vertexNormals(m);
    std::size_t n = 0;
    for (std::size_t v = 0; v < m.verts.size(); ++v)
        if (m.verts[v].sel) {
            m.verts[v].p += vn[v] * offset;
            ++n;
        }
    return n;
}

std::size_t smoothVertices(PolyMesh& m, double factor, int iterations, bool selectedOnly) {
    Topology t(m);
    std::vector<char> boundaryV(m.verts.size(), 0);
    for (std::size_t e = 0; e < t.edges.size(); ++e)
        if (t.edgeFaces[e].size() == 1) boundaryV[keyA(t.edges[e])] = boundaryV[keyB(t.edges[e])] = 1;
    std::size_t moved = 0;
    for (int it = 0; it < iterations; ++it) {
        std::vector<Vec3> next(m.verts.size());
        for (std::uint32_t v = 0; v < m.verts.size(); ++v) {
            next[v] = m.verts[v].p;
            if (selectedOnly && !m.verts[v].sel) continue;
            Vec3 sum;
            int k = 0;
            for (auto e : t.vertEdges[v]) {
                const EdgeKey key = t.edges[e];
                if (boundaryV[v] && t.edgeFaces[e].size() != 1) continue;  // boundaries slide along themselves
                const std::uint32_t w = keyA(key) == v ? keyB(key) : keyA(key);
                sum += m.verts[w].p;
                ++k;
            }
            if (k == 0) continue;
            next[v] = mesh::lerp(m.verts[v].p, sum / static_cast<double>(k), factor);
            if (it == 0) ++moved;
        }
        for (std::size_t v = 0; v < m.verts.size(); ++v) m.verts[v].p = next[v];
    }
    return moved;
}

std::size_t spin(PolyMesh& m, const Vec3& axis, const Vec3& center, double degrees, int steps) {
    if (steps < 1) steps = 1;
    const auto anySel = countSelection(m);
    if (anySel.verts == 0) return 0;
    const bool faces = anySel.faces > 0;
    const bool full = std::fabs(std::fabs(degrees) - 360.0) < 1e-9;
    const Mat4 step = Mat4::translate(center) * Mat4::rotateAxis(axis, degrees / steps) * Mat4::translate(-center);
    std::vector<Face> startCaps;
    if (faces && !full) {
        for (const auto& f : m.faces) {
            if (!f.sel) continue;
            Face g = f;
            g.sel = false;
            std::reverse(g.v.begin(), g.v.end());
            std::reverse(g.uv.begin(), g.uv.end());
            startCaps.push_back(std::move(g));
        }
    }
    std::size_t made = 0;
    for (int i = 0; i < steps; ++i) {
        made += extrude(m, ExtrudeMode::Region, 0, Vec3{0, 0, 0});
        transformSelected(m, step);
    }
    for (auto& f : startCaps) m.faces.push_back(std::move(f));
    // weld the copies that landed on each other (on the axis, and the last ring onto the first for a full turn)
    const mesh::Bounds b = bounds(m);
    const double eps = std::max(b.diagonal(), 1e-9) * 1e-7;
    if (full && faces) {
        // the last cap sits where the first faces were: drop it, then weld
        std::vector<char> kill(m.faces.size(), 0);
        for (std::size_t f = 0; f < m.faces.size(); ++f) kill[f] = m.faces[f].sel;
        std::vector<Face> keep;
        for (std::size_t f = 0; f < m.faces.size(); ++f)
            if (!kill[f]) keep.push_back(m.faces[f]);
        m.faces = std::move(keep);
        cleanup(m);
    }
    // keep the last copy selected across the weld
    std::set<std::array<double, 3>> lastPos;
    for (const auto& v : m.verts)
        if (v.sel) lastPos.insert({v.p.x, v.p.y, v.p.z});
    merge(m, MergeMode::Distance, eps, {}, true);
    for (auto& v : m.verts) v.sel = lastPos.count({v.p.x, v.p.y, v.p.z}) > 0;
    for (auto& f : m.faces) f.sel = false;
    flushSelection(m, SelectMode::Vertex);
    return made;
}

std::size_t mirror(PolyMesh& m, int axis, const Vec3& center, bool duplicateIt, double mergeDistance) {
    if (axis < 0 || axis > 2) return 0;
    Vec3 s{1, 1, 1};
    s[axis] = -1;
    const Mat4 t = Mat4::translate(center) * Mat4::scale(s) * Mat4::translate(-center);
    auto sc = countSelection(m);
    if (!duplicateIt) {
        if (sc.verts == 0) return 0;
        for (auto& v : m.verts)
            if (v.sel) v.p = t.point(v.p);
        for (auto& f : m.faces)
            if (f.sel) {
                std::reverse(f.v.begin(), f.v.end());
                std::reverse(f.uv.begin(), f.uv.end());
            }
        return sc.verts;
    }
    const bool all = sc.verts == 0;
    std::vector<std::uint32_t> map(m.verts.size(), UINT32_MAX);
    const std::size_t oldVerts = m.verts.size();
    auto copyOf = [&](std::uint32_t v) {
        if (map[v] != UINT32_MAX) return map[v];
        const double d = m.verts[v].p[axis] - center[axis];
        if (std::fabs(d) <= mergeDistance) {
            m.verts[v].p[axis] = center[axis];  // on the plane: shared, snapped onto it
            map[v] = v;
        } else {
            map[v] = m.addVertex(t.point(m.verts[v].p));
            m.verts[map[v]].color = m.verts[v].color;
        }
        return map[v];
    };
    std::vector<Face> made;
    const std::size_t oldFaces = m.faces.size();
    for (std::size_t fi = 0; fi < oldFaces; ++fi) {
        const Face& f = m.faces[fi];
        if (!all && !f.sel) continue;
        Face g = f;
        g.sel = false;
        bool onPlane = true;
        for (auto& v : g.v) {
            const std::uint32_t c = copyOf(v);
            onPlane = onPlane && c == v;
            v = c;
        }
        if (onPlane) continue;  // lies in the mirror plane: its copy would double it
        std::reverse(g.v.begin(), g.v.end());
        std::reverse(g.uv.begin(), g.uv.end());
        made.push_back(std::move(g));
    }
    std::vector<std::array<std::uint32_t, 2>> loose;
    for (const auto& e : m.loose)
        if (all || (m.verts[e[0]].sel && m.verts[e[1]].sel)) loose.push_back({copyOf(e[0]), copyOf(e[1])});
    const std::size_t count = made.size() + loose.size();
    for (auto& f : made) m.faces.push_back(std::move(f));
    for (const auto& e : loose) m.loose.push_back(e);
    // seams and sharp edges come across too
    for (auto* set : {&m.seams, &m.sharp}) {
        std::vector<EdgeKey> add;
        for (auto k : *set) {
            const std::uint32_t a = keyA(k), b = keyB(k);
            if (a < oldVerts && b < oldVerts && map[a] != UINT32_MAX && map[b] != UINT32_MAX) add.push_back(edgeKey(map[a], map[b]));
        }
        set->insert(add.begin(), add.end());
    }
    cleanup(m);
    flushSelection(m, all ? SelectMode::Vertex : SelectMode::Face);
    return count;
}

}  // namespace wetwaresuite::model
