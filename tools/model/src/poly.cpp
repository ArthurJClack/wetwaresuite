// PolyMesh basics: connectivity, selection flushing, clean-up, checks and
// conversion to and from triangle meshes.
#include "wetwaresuite/model/poly.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace wetwaresuite::model {

const char* toString(SelectMode m) {
    switch (m) {
        case SelectMode::Vertex: return "vertex";
        case SelectMode::Edge: return "edge";
        case SelectMode::Face: return "face";
    }
    return "vertex";
}

std::optional<SelectMode> parseSelectMode(const std::string& s) {
    if (s == "vert" || s == "vertex" || s == "verts" || s == "vertices") return SelectMode::Vertex;
    if (s == "edge" || s == "edges") return SelectMode::Edge;
    if (s == "face" || s == "faces") return SelectMode::Face;
    return std::nullopt;
}

bool PolyMesh::hasUvs() const {
    for (const auto& f : faces)
        if (!f.uv.empty()) return true;
    return false;
}

// ---- topology ----

Topology::Topology(const PolyMesh& m) {
    std::vector<EdgeKey> keys;
    std::size_t corners = 0;
    for (const auto& f : m.faces) corners += f.v.size();
    keys.reserve(corners + m.loose.size());
    for (const auto& f : m.faces) {
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) keys.push_back(edgeKey(f.v[i], f.v[(i + 1) % n]));
    }
    for (const auto& e : m.loose) keys.push_back(edgeKey(e[0], e[1]));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    edges = std::move(keys);
    index.reserve(edges.size() * 2);
    for (std::size_t i = 0; i < edges.size(); ++i) index.emplace(edges[i], static_cast<std::uint32_t>(i));
    edgeFaces.assign(edges.size(), {});
    vertFaces.assign(m.verts.size(), {});
    vertEdges.assign(m.verts.size(), {});
    for (std::uint32_t fi = 0; fi < m.faces.size(); ++fi) {
        const auto& f = m.faces[fi];
        const std::size_t n = f.v.size();
        for (std::uint32_t i = 0; i < n; ++i) {
            edgeFaces[index.at(edgeKey(f.v[i], f.v[(i + 1) % n]))].push_back({fi, i});
            if (f.v[i] < vertFaces.size()) vertFaces[f.v[i]].push_back(fi);
        }
    }
    for (auto& vf : vertFaces) {
        std::sort(vf.begin(), vf.end());
        vf.erase(std::unique(vf.begin(), vf.end()), vf.end());
    }
    for (std::uint32_t e = 0; e < edges.size(); ++e) {
        const std::uint32_t a = keyA(edges[e]), b = keyB(edges[e]);
        if (a < vertEdges.size()) vertEdges[a].push_back(e);
        if (b < vertEdges.size() && b != a) vertEdges[b].push_back(e);
    }
}

int Topology::edge(std::uint32_t a, std::uint32_t b) const {
    auto it = index.find(edgeKey(a, b));
    return it == index.end() ? -1 : static_cast<int>(it->second);
}

std::size_t Topology::faceCount(EdgeKey k) const {
    auto it = index.find(k);
    return it == index.end() ? 0 : edgeFaces[it->second].size();
}

std::vector<EdgeKey> allEdges(const PolyMesh& m) {
    std::vector<EdgeKey> keys;
    for (const auto& f : m.faces) {
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) keys.push_back(edgeKey(f.v[i], f.v[(i + 1) % n]));
    }
    for (const auto& e : m.loose) keys.push_back(edgeKey(e[0], e[1]));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

// ---- geometry ----

Vec3 faceNormal(const PolyMesh& m, const Face& f) {
    Vec3 n;
    const std::size_t k = f.v.size();
    for (std::size_t i = 0; i < k; ++i) {
        const Vec3& a = m.verts[f.v[i]].p;
        const Vec3& b = m.verts[f.v[(i + 1) % k]].p;
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return mesh::normalize(n);
}

Vec3 faceCenter(const PolyMesh& m, const Face& f) {
    Vec3 c;
    for (auto v : f.v) c += m.verts[v].p;
    return f.v.empty() ? c : c / static_cast<double>(f.v.size());
}

double faceArea(const PolyMesh& m, const Face& f) {
    Vec3 n;
    const std::size_t k = f.v.size();
    for (std::size_t i = 0; i < k; ++i) n += mesh::cross(m.verts[f.v[i]].p, m.verts[f.v[(i + 1) % k]].p);
    return mesh::length(n) * 0.5;
}

mesh::Bounds bounds(const PolyMesh& m) {
    mesh::Bounds b;
    for (const auto& v : m.verts) b.add(v.p);
    return b;
}

std::vector<Vec3> vertexNormals(const PolyMesh& m) {
    std::vector<Vec3> n(m.verts.size());
    for (const auto& f : m.faces) {
        const Vec3 fn = faceNormal(m, f) * faceArea(m, f);
        for (auto v : f.v) n[v] += fn;
    }
    for (auto& x : n) x = mesh::normalize(x);
    return n;
}

// ---- selection ----

void flushSelection(PolyMesh& m, SelectMode mode) {
    if (mode == SelectMode::Vertex) {
        m.esel.clear();
        for (auto k : allEdges(m))
            if (m.verts[keyA(k)].sel && m.verts[keyB(k)].sel) m.esel.insert(k);
        for (auto& f : m.faces) {
            f.sel = true;
            for (auto v : f.v) f.sel = f.sel && m.verts[v].sel;
        }
    } else if (mode == SelectMode::Edge) {
        const auto edges = allEdges(m);
        std::set<EdgeKey> keep;
        for (auto k : edges)
            if (m.esel.count(k)) keep.insert(k);
        m.esel = std::move(keep);
        for (auto& v : m.verts) v.sel = false;
        for (auto k : m.esel) m.verts[keyA(k)].sel = m.verts[keyB(k)].sel = true;
        for (auto& f : m.faces) {
            f.sel = true;
            const std::size_t n = f.v.size();
            for (std::size_t i = 0; i < n && f.sel; ++i) f.sel = m.esel.count(edgeKey(f.v[i], f.v[(i + 1) % n])) > 0;
        }
    } else {
        m.esel.clear();
        for (auto& v : m.verts) v.sel = false;
        for (const auto& f : m.faces) {
            if (!f.sel) continue;
            const std::size_t n = f.v.size();
            for (std::size_t i = 0; i < n; ++i) {
                m.verts[f.v[i]].sel = true;
                m.esel.insert(edgeKey(f.v[i], f.v[(i + 1) % n]));
            }
        }
    }
}

void selectAll(PolyMesh& m, bool on) {
    for (auto& v : m.verts) v.sel = on;
    for (auto& f : m.faces) f.sel = on;
    m.esel.clear();
    if (on)
        for (auto k : allEdges(m)) m.esel.insert(k);
}

void invertSelection(PolyMesh& m, SelectMode mode) {
    if (mode == SelectMode::Vertex) {
        for (auto& v : m.verts) v.sel = !v.sel;
    } else if (mode == SelectMode::Edge) {
        std::set<EdgeKey> inv;
        for (auto k : allEdges(m))
            if (!m.esel.count(k)) inv.insert(k);
        m.esel = std::move(inv);
    } else {
        for (auto& f : m.faces) f.sel = !f.sel;
    }
    flushSelection(m, mode);
}

std::vector<std::uint32_t> selectedVerts(const PolyMesh& m) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < m.verts.size(); ++i)
        if (m.verts[i].sel) out.push_back(i);
    return out;
}

std::vector<std::uint32_t> selectedFaces(const PolyMesh& m) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < m.faces.size(); ++i)
        if (m.faces[i].sel) out.push_back(i);
    return out;
}

std::vector<EdgeKey> selectedEdges(const PolyMesh& m) { return {m.esel.begin(), m.esel.end()}; }

SelectionCount countSelection(const PolyMesh& m) {
    SelectionCount c;
    for (const auto& v : m.verts) c.verts += v.sel ? 1 : 0;
    for (const auto& f : m.faces) c.faces += f.sel ? 1 : 0;
    c.edges = m.esel.size();
    return c;
}

// ---- clean-up ----

namespace {

void remapSet(std::set<EdgeKey>& s, const std::vector<std::uint32_t>& map, const std::set<EdgeKey>& existing) {
    std::set<EdgeKey> out;
    for (auto k : s) {
        const std::uint32_t a = keyA(k), b = keyB(k);
        if (a >= map.size() || b >= map.size()) continue;
        if (map[a] == UINT32_MAX || map[b] == UINT32_MAX || map[a] == map[b]) continue;
        const EdgeKey nk = edgeKey(map[a], map[b]);
        if (existing.count(nk)) out.insert(nk);
    }
    s = std::move(out);
}

}  // namespace

void cleanup(PolyMesh& m) {
    // faces: drop repeated corners and degenerate faces
    std::vector<Face> faces;
    faces.reserve(m.faces.size());
    for (auto& f : m.faces) {
        bool bad = false;
        for (auto v : f.v) bad = bad || v >= m.verts.size();
        if (bad) continue;
        Face g = f;
        g.v.clear();
        g.uv.clear();
        const bool uv = f.uv.size() == f.v.size();
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (f.v[i] == f.v[(i + 1) % n]) continue;
            g.v.push_back(f.v[i]);
            if (uv) g.uv.push_back(f.uv[i]);
        }
        if (g.v.size() < 3) continue;
        std::vector<std::uint32_t> sorted = g.v;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) continue;
        faces.push_back(std::move(g));
    }
    m.faces = std::move(faces);
    // loose edges: no repeats, none that a face already has
    std::set<EdgeKey> faceEdges;
    for (const auto& f : m.faces)
        for (std::size_t i = 0; i < f.v.size(); ++i) faceEdges.insert(edgeKey(f.v[i], f.v[(i + 1) % f.v.size()]));
    std::set<EdgeKey> seen;
    std::vector<std::array<std::uint32_t, 2>> loose;
    for (const auto& e : m.loose) {
        if (e[0] == e[1] || e[0] >= m.verts.size() || e[1] >= m.verts.size()) continue;
        const EdgeKey k = edgeKey(e[0], e[1]);
        if (faceEdges.count(k) || !seen.insert(k).second) continue;
        loose.push_back(e);
    }
    m.loose = std::move(loose);
    // vertices nothing uses
    std::vector<char> used(m.verts.size(), 0);
    for (const auto& f : m.faces)
        for (auto v : f.v) used[v] = 1;
    for (const auto& e : m.loose) used[e[0]] = used[e[1]] = 1;
    std::vector<std::uint32_t> map(m.verts.size(), UINT32_MAX);
    std::vector<Vertex> verts;
    verts.reserve(m.verts.size());
    for (std::uint32_t i = 0; i < m.verts.size(); ++i) {
        if (!used[i]) continue;
        map[i] = static_cast<std::uint32_t>(verts.size());
        verts.push_back(m.verts[i]);
    }
    const bool changed = verts.size() != m.verts.size();
    m.verts = std::move(verts);
    if (changed) {
        for (auto& f : m.faces)
            for (auto& v : f.v) v = map[v];
        for (auto& e : m.loose) e = {map[e[0]], map[e[1]]};
    } else {
        for (std::uint32_t i = 0; i < map.size(); ++i) map[i] = i;
    }
    const auto edges = allEdges(m);
    const std::set<EdgeKey> existing(edges.begin(), edges.end());
    remapSet(m.esel, map, existing);
    remapSet(m.seams, map, existing);
    remapSet(m.sharp, map, existing);
}

void removeVertices(PolyMesh& m, const std::vector<char>& kill) {
    auto dead = [&](std::uint32_t v) { return v < kill.size() && kill[v]; };
    std::vector<Face> faces;
    for (auto& f : m.faces) {
        bool k = false;
        for (auto v : f.v) k = k || dead(v);
        if (!k) faces.push_back(std::move(f));
    }
    m.faces = std::move(faces);
    std::vector<std::array<std::uint32_t, 2>> loose;
    for (const auto& e : m.loose)
        if (!dead(e[0]) && !dead(e[1])) loose.push_back(e);
    m.loose = std::move(loose);
    cleanup(m);
}

void removeFaces(PolyMesh& m, const std::vector<char>& kill) {
    std::vector<Face> faces;
    for (std::size_t i = 0; i < m.faces.size(); ++i)
        if (i >= kill.size() || !kill[i]) faces.push_back(std::move(m.faces[i]));
    m.faces = std::move(faces);
    cleanup(m);
}

// ---- checks ----

Stats stats(const PolyMesh& m) {
    Stats s;
    s.verts = m.verts.size();
    s.faces = m.faces.size();
    for (const auto& f : m.faces) {
        if (f.v.size() == 3) ++s.tris;
        else if (f.v.size() == 4) ++s.quads;
        else ++s.ngons;
        s.triangles += f.v.size() >= 3 ? f.v.size() - 2 : 0;
    }
    Topology t(m);
    s.edges = t.edges.size();
    s.consistent = true;
    for (std::size_t e = 0; e < t.edges.size(); ++e) {
        const auto& ef = t.edgeFaces[e];
        if (ef.empty()) ++s.looseEdges;
        else if (ef.size() == 1) ++s.boundaryEdges;
        else if (ef.size() > 2) ++s.nonManifoldEdges;
        else {
            const Face& f0 = m.faces[ef[0].face];
            const Face& f1 = m.faces[ef[1].face];
            if (f0.v[ef[0].corner] == f1.v[ef[1].corner]) s.consistent = false;  // both walk it the same way
        }
    }
    s.manifold = s.nonManifoldEdges == 0 && s.consistent;
    s.closed = s.manifold && s.boundaryEdges == 0 && s.looseEdges == 0 && s.faces > 0;
    s.euler = static_cast<long long>(s.verts) - static_cast<long long>(s.edges) + static_cast<long long>(s.faces);
    return s;
}

bool valid(const PolyMesh& m, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    for (std::size_t i = 0; i < m.faces.size(); ++i) {
        const Face& f = m.faces[i];
        if (f.v.size() < 3) return fail("face " + std::to_string(i) + " has fewer than 3 corners");
        if (!f.uv.empty() && f.uv.size() != f.v.size()) return fail("face " + std::to_string(i) + " has the wrong number of uvs");
        std::vector<std::uint32_t> s = f.v;
        std::sort(s.begin(), s.end());
        if (std::adjacent_find(s.begin(), s.end()) != s.end()) return fail("face " + std::to_string(i) + " repeats a vertex");
        for (auto v : f.v)
            if (v >= m.verts.size()) return fail("face " + std::to_string(i) + " uses a missing vertex");
    }
    for (const auto& e : m.loose)
        if (e[0] >= m.verts.size() || e[1] >= m.verts.size() || e[0] == e[1]) return fail("a loose edge is broken");
    return true;
}

// ---- triangulation ----

std::vector<std::uint32_t> triangulateFace(const PolyMesh& m, const Face& f) {
    const std::size_t n = f.v.size();
    std::vector<std::uint32_t> out;
    if (n < 3) return out;
    if (n == 3) return {0, 1, 2};
    const Vec3 nrm = faceNormal(m, f);
    if (n == 4) {
        // split along the diagonal that keeps both halves facing the same way (and the shorter one when both do)
        const Vec3 &a = m.verts[f.v[0]].p, &b = m.verts[f.v[1]].p, &c = m.verts[f.v[2]].p, &d = m.verts[f.v[3]].p;
        const bool ac = mesh::dot(mesh::cross(b - a, c - a), nrm) > 0 && mesh::dot(mesh::cross(c - a, d - a), nrm) > 0;
        const bool bd = mesh::dot(mesh::cross(c - b, d - b), nrm) > 0 && mesh::dot(mesh::cross(d - b, a - b), nrm) > 0;
        if (ac && (!bd || mesh::length(c - a) <= mesh::length(d - b))) return {0, 1, 2, 0, 2, 3};
        if (bd) return {1, 2, 3, 1, 3, 0};
        return {0, 1, 2, 0, 2, 3};
    }
    // ear clipping in the face's plane
    Vec3 ax = std::fabs(nrm.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 u = mesh::normalize(mesh::cross(ax, nrm));
    const Vec3 w = mesh::cross(nrm, u);
    std::vector<Vec2> p(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = {mesh::dot(m.verts[f.v[i]].p, u), mesh::dot(m.verts[f.v[i]].p, w)};
    std::vector<std::uint32_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0u);
    auto cross2 = [](const Vec2& a, const Vec2& b, const Vec2& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
    auto inside = [&](const Vec2& q, const Vec2& a, const Vec2& b, const Vec2& c) {
        return cross2(a, b, q) >= -1e-14 && cross2(b, c, q) >= -1e-14 && cross2(c, a, q) >= -1e-14;
    };
    std::size_t guard = 0;
    while (idx.size() > 3 && guard++ < n * n + 10) {
        bool clipped = false;
        const std::size_t k = idx.size();
        for (std::size_t i = 0; i < k; ++i) {
            const std::uint32_t ia = idx[(i + k - 1) % k], ib = idx[i], ic = idx[(i + 1) % k];
            if (cross2(p[ia], p[ib], p[ic]) <= 1e-14) continue;  // reflex or flat
            bool ear = true;
            for (std::size_t j = 0; j < k && ear; ++j) {
                const std::uint32_t q = idx[j];
                if (q == ia || q == ib || q == ic) continue;
                if (inside(p[q], p[ia], p[ib], p[ic])) ear = false;
            }
            if (!ear) continue;
            out.insert(out.end(), {ia, ib, ic});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (idx.size() == 3) {
        out.insert(out.end(), {idx[0], idx[1], idx[2]});
    } else if (idx.size() > 3) {
        for (std::size_t i = 1; i + 1 < idx.size(); ++i) out.insert(out.end(), {idx[0], idx[i], idx[i + 1]});  // fan the rest
    }
    return out;
}

namespace {

struct CornerKey {
    std::uint32_t v;
    std::uint64_t nx, ny, nz, u, w;
    bool operator==(const CornerKey&) const = default;
};
struct CornerHash {
    std::size_t operator()(const CornerKey& k) const {
        std::uint64_t h = k.v;
        for (std::uint64_t x : {k.nx, k.ny, k.nz, k.u, k.w}) h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};
std::uint64_t bitsOf(double d) {
    d += 0.0;
    std::uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

}  // namespace

Triangles triangulate(const PolyMesh& m, const TriangulateOptions& o) {
    Triangles out;
    const bool uvs = o.uvs && m.hasUvs();
    std::vector<Vec3> fn(m.faces.size());
    std::vector<double> fa(m.faces.size());
    for (std::size_t i = 0; i < m.faces.size(); ++i) {
        fn[i] = faceNormal(m, m.faces[i]);
        fa[i] = faceArea(m, m.faces[i]);
    }
    bool anySmooth = false;
    for (const auto& f : m.faces) anySmooth = anySmooth || f.smooth;
    std::vector<std::vector<std::uint32_t>> vf;
    if (anySmooth) {
        vf.assign(m.verts.size(), {});
        for (std::uint32_t i = 0; i < m.faces.size(); ++i)
            for (auto v : m.faces[i].v) vf[v].push_back(i);
    }
    const double cosLimit = std::cos(std::clamp(o.autoSmooth, 0.0, 180.0) * mesh::kPi / 180.0);
    // the smooth normal of face fi at vertex v: faces around v reachable through
    // shared edges that aren't sharp and don't bend more than autoSmooth
    auto cornerNormal = [&](std::uint32_t fi, std::uint32_t v) {
        const Face& f = m.faces[fi];
        if (!f.smooth) return fn[fi];
        const auto& around = vf[v];
        std::vector<std::uint32_t> group{fi}, stack{fi};
        std::vector<char> in(around.size(), 0);
        for (std::size_t i = 0; i < around.size(); ++i)
            if (around[i] == fi) in[i] = 1;
        auto edgeNeighbors = [&](std::uint32_t a, std::uint32_t b) {
            // the other vertex of a's corner edges at v that b shares
            const Face& fa2 = m.faces[a];
            const Face& fb = m.faces[b];
            const std::size_t na = fa2.v.size();
            for (std::size_t i = 0; i < na; ++i) {
                if (fa2.v[i] != v) continue;
                for (std::uint32_t w : {fa2.v[(i + 1) % na], fa2.v[(i + na - 1) % na]}) {
                    if (std::find(fb.v.begin(), fb.v.end(), w) != fb.v.end() && !m.sharp.count(edgeKey(v, w))) return true;
                }
            }
            return false;
        };
        while (!stack.empty()) {
            const std::uint32_t cur = stack.back();
            stack.pop_back();
            for (std::size_t i = 0; i < around.size(); ++i) {
                if (in[i]) continue;
                const std::uint32_t g = around[i];
                if (!m.faces[g].smooth || mesh::dot(fn[g], fn[cur]) < cosLimit || !edgeNeighbors(cur, g)) continue;
                in[i] = 1;
                group.push_back(g);
                stack.push_back(g);
            }
        }
        Vec3 n;
        for (auto g : group) n += fn[g] * std::max(fa[g], 1e-12);
        const Vec3 r = mesh::normalize(n);
        return mesh::length(r) > 0 ? r : fn[fi];
    };
    std::unordered_map<CornerKey, std::uint32_t, CornerHash> seen;
    for (std::uint32_t fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        const auto tri = triangulateFace(m, f);
        if (tri.empty()) continue;
        const bool fuv = uvs && f.uv.size() == f.v.size();
        std::vector<std::uint32_t> corner(f.v.size());
        for (std::size_t c = 0; c < f.v.size(); ++c) {
            const Vec3 n = cornerNormal(fi, f.v[c]);
            const Vec2 uv = fuv ? f.uv[c] : Vec2{};
            CornerKey key{f.v[c], bitsOf(n.x), bitsOf(n.y), bitsOf(n.z), bitsOf(uv.x), bitsOf(uv.y)};
            auto it = seen.find(key);
            if (it == seen.end()) {
                const std::uint32_t id = static_cast<std::uint32_t>(out.mesh.positions.size());
                out.mesh.positions.push_back(m.verts[f.v[c]].p);
                out.mesh.normals.push_back(n);
                if (uvs) out.mesh.uvs.push_back(uv);
                if (m.colors) out.color.push_back(m.verts[f.v[c]].color);
                it = seen.emplace(key, id).first;
            }
            corner[c] = it->second;
        }
        for (std::size_t t = 0; t + 2 < tri.size(); t += 3) {
            out.mesh.addTriangle(corner[tri[t]], corner[tri[t + 1]], corner[tri[t + 2]]);
            out.face.push_back(fi);
            out.material.push_back(f.material);
        }
    }
    return out;
}

PolyMesh fromTriangles(const mesh::Mesh& tris) {
    PolyMesh m;
    std::unordered_map<std::string, std::uint32_t> at;
    std::vector<std::uint32_t> map(tris.positions.size());
    for (std::size_t i = 0; i < tris.positions.size(); ++i) {
        const Vec3& p = tris.positions[i];
        std::string key(sizeof(double) * 3, '\0');
        const double xyz[3] = {p.x + 0.0, p.y + 0.0, p.z + 0.0};
        std::memcpy(key.data(), xyz, sizeof xyz);
        auto it = at.find(key);
        if (it == at.end()) it = at.emplace(key, m.addVertex(p)).first;
        map[i] = it->second;
    }
    const bool uv = tris.hasUvs();
    for (std::size_t t = 0; t < tris.triangleCount(); ++t) {
        Face f;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t i = tris.indices[t * 3 + static_cast<std::size_t>(k)];
            f.v.push_back(map[i]);
            if (uv) f.uv.push_back(tris.uvs[i]);
        }
        m.faces.push_back(std::move(f));
    }
    cleanup(m);
    return m;
}

void transform(PolyMesh& m, const Mat4& t) {
    for (auto& v : m.verts) v.p = t.point(v.p);
    if (t.determinant3() < 0) {
        for (auto& f : m.faces) {
            std::reverse(f.v.begin(), f.v.end());
            std::reverse(f.uv.begin(), f.uv.end());
        }
    }
}

std::uint32_t append(PolyMesh& dst, const PolyMesh& src, const Mat4& t, int materialOffset) {
    const std::uint32_t base = static_cast<std::uint32_t>(dst.verts.size());
    const bool flip = t.determinant3() < 0;
    for (const auto& v : src.verts) {
        Vertex nv = v;
        nv.p = t.point(v.p);
        dst.verts.push_back(nv);
    }
    for (const auto& f : src.faces) {
        Face g = f;
        for (auto& v : g.v) v += base;
        g.material += materialOffset;
        if (flip) {
            std::reverse(g.v.begin(), g.v.end());
            std::reverse(g.uv.begin(), g.uv.end());
        }
        dst.faces.push_back(std::move(g));
    }
    for (const auto& e : src.loose) dst.loose.push_back({e[0] + base, e[1] + base});
    for (auto k : src.esel) dst.esel.insert(edgeKey(keyA(k) + base, keyB(k) + base));
    for (auto k : src.seams) dst.seams.insert(edgeKey(keyA(k) + base, keyB(k) + base));
    for (auto k : src.sharp) dst.sharp.insert(edgeKey(keyA(k) + base, keyB(k) + base));
    dst.colors = dst.colors || src.colors;
    return base;
}

PolyMesh extractSelected(const PolyMesh& m) {
    PolyMesh out = m;
    std::vector<Face> faces;
    for (const auto& f : m.faces)
        if (f.sel) faces.push_back(f);
    out.faces = std::move(faces);
    out.loose.clear();
    for (const auto& e : m.loose)
        if (m.verts[e[0]].sel && m.verts[e[1]].sel) out.loose.push_back(e);
    cleanup(out);
    return out;
}

std::vector<PolyMesh> looseParts(const PolyMesh& m) {
    std::vector<std::uint32_t> parent(m.verts.size());
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    auto unite = [&](std::uint32_t a, std::uint32_t b) { parent[find(a)] = find(b); };
    for (const auto& f : m.faces)
        for (std::size_t i = 1; i < f.v.size(); ++i) unite(f.v[0], f.v[i]);
    for (const auto& e : m.loose) unite(e[0], e[1]);
    std::unordered_map<std::uint32_t, std::size_t> part;
    std::vector<PolyMesh> out;
    std::vector<std::size_t> owner(m.verts.size());
    std::vector<std::uint32_t> local(m.verts.size());
    for (std::uint32_t v = 0; v < m.verts.size(); ++v) {
        const std::uint32_t r = find(v);
        auto it = part.find(r);
        if (it == part.end()) {
            it = part.emplace(r, out.size()).first;
            out.emplace_back();
            out.back().colors = m.colors;
        }
        owner[v] = it->second;
        local[v] = static_cast<std::uint32_t>(out[it->second].verts.size());
        out[it->second].verts.push_back(m.verts[v]);
    }
    for (const auto& f : m.faces) {
        Face g = f;
        for (auto& v : g.v) v = local[v];
        out[owner[f.v[0]]].faces.push_back(std::move(g));
    }
    for (const auto& e : m.loose) out[owner[e[0]]].loose.push_back({local[e[0]], local[e[1]]});
    auto spread = [&](const std::set<EdgeKey>& s, std::set<EdgeKey> PolyMesh::*field) {
        for (auto k : s) {
            const std::uint32_t a = keyA(k), b = keyB(k);
            if (a < owner.size() && b < owner.size() && owner[a] == owner[b]) (out[owner[a]].*field).insert(edgeKey(local[a], local[b]));
        }
    };
    spread(m.esel, &PolyMesh::esel);
    spread(m.seams, &PolyMesh::seams);
    spread(m.sharp, &PolyMesh::sharp);
    return out;
}

}  // namespace wetwaresuite::model
