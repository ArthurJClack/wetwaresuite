// Merging, dissolving, deleting, bridging, filling and normals.
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

#include "wetwaresuite/model/ops.hpp"

namespace wetwaresuite::model {

std::optional<MergeMode> parseMergeMode(const std::string& s) {
    if (s == "distance" || s == "by_distance") return MergeMode::Distance;
    if (s == "center") return MergeMode::Center;
    if (s == "first") return MergeMode::First;
    if (s == "last") return MergeMode::Last;
    if (s == "cursor") return MergeMode::Cursor;
    if (s == "collapse") return MergeMode::Collapse;
    return std::nullopt;
}

std::optional<DeleteMode> parseDeleteMode(const std::string& s) {
    if (s == "verts" || s == "vertices" || s == "vert") return DeleteMode::Verts;
    if (s == "edges" || s == "edge") return DeleteMode::Edges;
    if (s == "faces" || s == "face") return DeleteMode::Faces;
    if (s == "only_faces") return DeleteMode::OnlyFaces;
    return std::nullopt;
}

namespace {

// Rewrites vertex references through `map` (old -> new old-index), then cleans up.
void remapVertices(PolyMesh& m, const std::vector<std::uint32_t>& map) {
    for (auto& f : m.faces)
        for (auto& v : f.v) v = map[v];
    for (auto& e : m.loose) e = {map[e[0]], map[e[1]]};
    for (auto* set : {&m.esel, &m.seams, &m.sharp}) {
        std::set<EdgeKey> out;
        for (auto k : *set) {
            const std::uint32_t a = map[keyA(k)], b = map[keyB(k)];
            if (a != b) out.insert(edgeKey(a, b));
        }
        *set = std::move(out);
    }
    // faces that became the same polygon: one copy stays, two facing each other both go
    std::map<std::vector<std::uint32_t>, std::vector<std::size_t>> bySet;
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        std::vector<std::uint32_t> s = m.faces[f].v;
        std::sort(s.begin(), s.end());
        s.erase(std::unique(s.begin(), s.end()), s.end());
        if (s.size() == m.faces[f].v.size()) bySet[s].push_back(f);
    }
    std::vector<char> kill(m.faces.size(), 0);
    for (const auto& [s, fs] : bySet) {
        if (fs.size() < 2) continue;
        for (std::size_t i = 1; i < fs.size(); ++i) kill[fs[i]] = 1;
        // opposite windings: an inner wall, drop both
        const auto& a = m.faces[fs[0]].v;
        const auto& b = m.faces[fs[1]].v;
        const std::size_t n = a.size();
        const std::size_t at = static_cast<std::size_t>(std::find(b.begin(), b.end(), a[0]) - b.begin());
        if (n > 2 && b[(at + 1) % n] != a[1]) kill[fs[0]] = 1;
    }
    removeFaces(m, kill);
}

// Joins the faces across the edges in `dissolve`; returns edges dissolved.
std::size_t dissolveEdgeSet(PolyMesh& m, const std::set<EdgeKey>& dissolve, bool clean, double cleanDegrees = 180) {
    Topology t(m);
    std::vector<std::uint32_t> parent(m.faces.size());
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    std::set<std::uint32_t> touched;
    for (auto k : dissolve) {
        auto it = t.index.find(k);
        if (it == t.index.end()) continue;
        const auto& ef = t.edgeFaces[it->second];
        if (ef.size() != 2) continue;
        parent[find(ef[0].face)] = find(ef[1].face);
        touched.insert(keyA(k));
        touched.insert(keyB(k));
    }
    std::map<std::uint32_t, std::vector<std::uint32_t>> groups;
    for (std::uint32_t f = 0; f < m.faces.size(); ++f) groups[find(f)].push_back(f);
    std::vector<char> kill(m.faces.size(), 0);
    std::vector<Face> made;
    std::size_t count = 0;
    for (const auto& [root, fs] : groups) {
        if (fs.size() < 2) continue;
        std::set<std::uint32_t> in(fs.begin(), fs.end());
        struct Half {
            std::uint32_t b;
            std::uint32_t face, corner;
        };
        std::unordered_map<std::uint32_t, Half> next;
        bool ambiguous = false;
        std::size_t halfCount = 0;
        for (auto fi : fs) {
            const auto& fv = m.faces[fi].v;
            for (std::uint32_t c = 0; c < fv.size(); ++c) {
                const std::uint32_t a = fv[c], b = fv[(c + 1) % fv.size()];
                bool inner = true;
                const auto& ef = t.edgeFaces[t.index.at(edgeKey(a, b))];
                for (const auto& h : ef)
                    if (!in.count(h.face)) inner = false;
                if (inner && ef.size() > 1) continue;
                if (next.count(a)) ambiguous = true;
                next[a] = {b, fi, c};
                ++halfCount;
            }
        }
        if (ambiguous || next.empty()) continue;
        const std::uint32_t start = next.begin()->first;
        std::vector<std::uint32_t> loop;
        std::vector<Vec2> uvs;
        bool uv = true;
        for (auto fi : fs) uv = uv && m.faces[fi].uv.size() == m.faces[fi].v.size();
        std::uint32_t cur = start;
        bool closed = false;
        for (std::size_t g = 0; g <= halfCount; ++g) {
            auto it = next.find(cur);
            if (it == next.end()) break;
            loop.push_back(cur);
            if (uv) uvs.push_back(m.faces[it->second.face].uv[it->second.corner]);
            cur = it->second.b;
            if (cur == start) {
                closed = true;
                break;
            }
        }
        if (!closed || loop.size() != halfCount || loop.size() < 3) continue;  // holes or a pinch: leave it
        Face g;
        g.v = std::move(loop);
        if (uv) g.uv = std::move(uvs);
        g.material = m.faces[fs[0]].material;
        g.smooth = m.faces[fs[0]].smooth;
        g.sel = true;
        for (auto fi : fs) {
            kill[fi] = 1;
            g.sel = g.sel && m.faces[fi].sel;
        }
        made.push_back(std::move(g));
        count += fs.size() - 1;
    }
    std::vector<Face> faces;
    for (std::size_t f = 0; f < m.faces.size(); ++f)
        if (!kill[f]) faces.push_back(std::move(m.faces[f]));
    for (auto& f : made) faces.push_back(std::move(f));
    m.faces = std::move(faces);
    if (clean && !touched.empty()) {
        // vertices left in the middle of a straight run of two edges go too
        Topology t2(m);
        std::vector<char> looseV(m.verts.size(), 0);
        for (const auto& e : m.loose) looseV[e[0]] = looseV[e[1]] = 1;
        const double c = std::cos(cleanDegrees * mesh::kPi / 180.0);
        std::vector<char> drop(m.verts.size(), 0);
        for (auto v : touched) {
            if (v >= m.verts.size() || looseV[v] || t2.vertEdges[v].size() != 2) continue;
            const EdgeKey e0 = t2.edges[t2.vertEdges[v][0]], e1 = t2.edges[t2.vertEdges[v][1]];
            const std::uint32_t a = keyA(e0) == v ? keyB(e0) : keyA(e0), b = keyA(e1) == v ? keyB(e1) : keyA(e1);
            const Vec3 d0 = mesh::normalize(m.verts[v].p - m.verts[a].p), d1 = mesh::normalize(m.verts[b].p - m.verts[v].p);
            if (mesh::dot(d0, d1) < c) continue;
            drop[v] = 1;
        }
        for (auto& f : m.faces) {
            std::vector<std::uint32_t> v;
            std::vector<Vec2> u;
            const bool uv = f.uv.size() == f.v.size();
            for (std::size_t i = 0; i < f.v.size(); ++i) {
                if (drop[f.v[i]]) continue;
                v.push_back(f.v[i]);
                if (uv) u.push_back(f.uv[i]);
            }
            if (v.size() >= 3) {
                f.v = std::move(v);
                f.uv = std::move(u);
            }
        }
    }
    cleanup(m);
    return count;
}

std::uint32_t otherEnd(EdgeKey k, std::uint32_t v) { return keyA(k) == v ? keyB(k) : keyA(k); }

// Chains of the given edges: each a vertex list, closed when it loops back.
struct Chain {
    std::vector<std::uint32_t> v;
    bool closed = false;
};
std::vector<Chain> chains(const std::vector<EdgeKey>& edges) {
    std::map<std::uint32_t, std::vector<EdgeKey>> at;
    for (auto k : edges) {
        at[keyA(k)].push_back(k);
        at[keyB(k)].push_back(k);
    }
    for (const auto& [v, es] : at)
        if (es.size() > 2) throw std::runtime_error("the selected edges branch at vertex " + std::to_string(v) + "; select simple loops or chains");
    std::set<EdgeKey> used;
    std::vector<Chain> out;
    auto walk = [&](std::uint32_t start) {
        Chain c;
        c.v.push_back(start);
        std::uint32_t cur = start;
        while (true) {
            EdgeKey next = 0;
            bool found = false;
            for (auto k : at[cur])
                if (!used.count(k)) {
                    next = k;
                    found = true;
                    break;
                }
            if (!found) break;
            used.insert(next);
            cur = otherEnd(next, cur);
            if (cur == start) {
                c.closed = true;
                break;
            }
            c.v.push_back(cur);
        }
        return c;
    };
    for (const auto& [v, es] : at)
        if (es.size() == 1 && !used.count(es[0])) out.push_back(walk(v));
    for (const auto& [v, es] : at)
        for (auto k : es)
            if (!used.count(k)) out.push_back(walk(v));
    return out;
}

// +1 when faces already walk the chain in its order, -1 against it, 0 when no face does.
int chainDirection(const PolyMesh& m, const Topology& t, const Chain& c) {
    int score = 0;
    const std::size_t n = c.v.size();
    const std::size_t edges = c.closed ? n : n - 1;
    for (std::size_t i = 0; i < edges; ++i) {
        const std::uint32_t a = c.v[i], b = c.v[(i + 1) % n];
        const int e = t.edge(a, b);
        if (e < 0) continue;
        for (const auto& h : t.edgeFaces[static_cast<std::size_t>(e)]) score += m.faces[h.face].v[h.corner] == a ? 1 : -1;
    }
    return score > 0 ? 1 : score < 0 ? -1 : 0;
}

Vec3 centroid(const PolyMesh& m, const std::vector<std::uint32_t>& vs) {
    Vec3 c;
    for (auto v : vs) c += m.verts[v].p;
    return vs.empty() ? c : c / static_cast<double>(vs.size());
}

// Orients a new face against the faces already on its edges, or away from the mesh centre.
void orientNewFace(const PolyMesh& m, const Topology& t, std::vector<std::uint32_t>& loop) {
    int score = 0;
    const std::size_t n = loop.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t a = loop[i], b = loop[(i + 1) % n];
        const int e = t.edge(a, b);
        if (e < 0) continue;
        for (const auto& h : t.edgeFaces[static_cast<std::size_t>(e)]) score += m.faces[h.face].v[h.corner] == a ? -1 : 1;
    }
    if (score < 0) {
        std::reverse(loop.begin(), loop.end());
        return;
    }
    if (score > 0) return;
    Face f;
    f.v = loop;
    const Vec3 nrm = faceNormal(m, f);
    const Vec3 c = centroid(m, loop);
    const Vec3 mc = bounds(m).center();
    const Vec3 out = mesh::length(c - mc) > 1e-9 * (1 + bounds(m).diagonal()) ? c - mc : Vec3{0, 0, 1};
    if (mesh::dot(nrm, out) < 0) std::reverse(loop.begin(), loop.end());
}

}  // namespace

std::size_t merge(PolyMesh& m, MergeMode mode, double distance, const Vec3& cursor, bool allVerts) {
    std::vector<std::uint32_t> sel;
    for (std::uint32_t v = 0; v < m.verts.size(); ++v)
        if (allVerts || m.verts[v].sel) sel.push_back(v);
    if (sel.size() < 2) return 0;
    const std::size_t before = m.verts.size();
    std::vector<std::uint32_t> map(m.verts.size());
    std::iota(map.begin(), map.end(), 0u);
    if (mode == MergeMode::Distance) {
        const double r = std::max(distance, 0.0);
        std::vector<std::uint32_t> parent(m.verts.size());
        std::iota(parent.begin(), parent.end(), 0u);
        auto find = [&](std::uint32_t x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        const double cell = r > 0 ? r : 1e-12;
        std::map<std::array<long long, 3>, std::vector<std::uint32_t>> grid;
        auto cellOf = [&](const Vec3& p) {
            return std::array<long long, 3>{static_cast<long long>(std::floor(p.x / cell)), static_cast<long long>(std::floor(p.y / cell)),
                                            static_cast<long long>(std::floor(p.z / cell))};
        };
        for (auto v : sel) grid[cellOf(m.verts[v].p)].push_back(v);
        for (auto v : sel) {
            const auto c = cellOf(m.verts[v].p);
            for (long long dx = -1; dx <= 1; ++dx)
                for (long long dy = -1; dy <= 1; ++dy)
                    for (long long dz = -1; dz <= 1; ++dz) {
                        auto it = grid.find({c[0] + dx, c[1] + dy, c[2] + dz});
                        if (it == grid.end()) continue;
                        for (auto w : it->second) {
                            if (w <= v) continue;
                            if (mesh::length(m.verts[w].p - m.verts[v].p) <= r) {
                                const std::uint32_t a = find(v), b = find(w);
                                if (a != b) parent[std::max(a, b)] = std::min(a, b);
                            }
                        }
                    }
        }
        for (auto v : sel) map[v] = find(v);
    } else if (mode == MergeMode::Collapse) {
        // one vertex per connected piece of the selection
        Topology t(m);
        std::vector<int> comp(m.verts.size(), -1);
        int n = 0;
        for (auto v0 : sel) {
            if (comp[v0] >= 0) continue;
            std::vector<std::uint32_t> stack{v0}, members;
            comp[v0] = n;
            while (!stack.empty()) {
                const std::uint32_t v = stack.back();
                stack.pop_back();
                members.push_back(v);
                for (auto e : t.vertEdges[v]) {
                    const std::uint32_t w = otherEnd(t.edges[e], v);
                    if (m.verts[w].sel && comp[w] < 0) {
                        comp[w] = n;
                        stack.push_back(w);
                    }
                }
            }
            const std::uint32_t keep = *std::min_element(members.begin(), members.end());
            m.verts[keep].p = centroid(m, members);
            for (auto v : members) map[v] = keep;
            ++n;
        }
    } else {
        const std::uint32_t keep = mode == MergeMode::Last ? sel.back() : sel.front();
        Vec3 at = m.verts[keep].p;
        if (mode == MergeMode::Center) at = centroid(m, sel);
        if (mode == MergeMode::Cursor) at = cursor;
        m.verts[keep].p = at;
        for (auto v : sel) map[v] = keep;
    }
    remapVertices(m, map);
    return before - m.verts.size();
}

std::size_t dissolveEdges(PolyMesh& m, bool clean) { return dissolveEdgeSet(m, m.esel, clean); }

std::size_t dissolveFaces(PolyMesh& m, bool clean) {
    Topology t(m);
    std::set<EdgeKey> inner;
    for (std::size_t e = 0; e < t.edges.size(); ++e) {
        const auto& ef = t.edgeFaces[e];
        if (ef.size() == 2 && m.faces[ef[0].face].sel && m.faces[ef[1].face].sel) inner.insert(t.edges[e]);
    }
    const std::size_t n = dissolveEdgeSet(m, inner, clean);
    flushSelection(m, SelectMode::Face);
    return n;
}

std::size_t dissolveVerts(PolyMesh& m) {
    std::vector<Vec3> targets;
    for (const auto& v : m.verts)
        if (v.sel) targets.push_back(v.p);
    std::size_t count = 0;
    for (const Vec3& p : targets) {
        // find the vertex again (indices move as faces merge)
        std::uint32_t v = UINT32_MAX;
        for (std::uint32_t i = 0; i < m.verts.size(); ++i)
            if (m.verts[i].p == p && m.verts[i].sel) v = i;
        if (v == UINT32_MAX) continue;
        Topology t(m);
        const auto& fs = t.vertFaces[v];
        if (fs.empty()) continue;
        if (t.vertEdges[v].size() == 2) {
            // a vertex in the middle of an edge: just take it out
            bool ok = true;
            for (auto fi : fs) ok = ok && m.faces[fi].v.size() > 3;
            if (!ok) continue;
            for (auto fi : fs) {
                Face& f = m.faces[fi];
                const std::size_t at = static_cast<std::size_t>(std::find(f.v.begin(), f.v.end(), v) - f.v.begin());
                f.v.erase(f.v.begin() + static_cast<std::ptrdiff_t>(at));
                if (f.uv.size() == f.v.size() + 1) f.uv.erase(f.uv.begin() + static_cast<std::ptrdiff_t>(at));
            }
            std::vector<std::array<std::uint32_t, 2>> loose;
            for (const auto& e : m.loose)
                if (e[0] != v && e[1] != v) loose.push_back(e);
            m.loose = std::move(loose);
            cleanup(m);
            ++count;
            continue;
        }
        // join the faces around it, leaving it out
        std::set<std::uint32_t> in(fs.begin(), fs.end());
        std::unordered_map<std::uint32_t, std::pair<std::uint32_t, Vec2>> next;
        bool ambiguous = false, uv = true;
        for (auto fi : fs) uv = uv && m.faces[fi].uv.size() == m.faces[fi].v.size();
        for (auto fi : fs) {
            const auto& fv = m.faces[fi].v;
            for (std::size_t c = 0; c < fv.size(); ++c) {
                const std::uint32_t a = fv[c], b = fv[(c + 1) % fv.size()];
                if (a == v || b == v) continue;
                bool inner = false;
                for (const auto& h : t.edgeFaces[t.index.at(edgeKey(a, b))])
                    if (h.face != fi && in.count(h.face)) inner = true;
                if (inner) continue;
                if (next.count(a)) ambiguous = true;
                next[a] = {b, uv ? m.faces[fi].uv[c] : Vec2{}};
            }
        }
        if (ambiguous || next.size() < 3) continue;
        // start where nothing leads in (an open fan) or anywhere
        std::set<std::uint32_t> targetsIn;
        for (const auto& [a, b] : next) targetsIn.insert(b.first);
        std::uint32_t start = next.begin()->first;
        for (const auto& [a, b] : next)
            if (!targetsIn.count(a)) start = a;
        std::vector<std::uint32_t> loop;
        std::vector<Vec2> uvs;
        std::uint32_t cur = start;
        for (std::size_t g = 0; g <= next.size(); ++g) {
            loop.push_back(cur);
            auto it = next.find(cur);
            if (it == next.end()) {
                uv = false;  // an open fan: the last corner has no uv to take
                break;
            }
            uvs.push_back(it->second.second);
            cur = it->second.first;
            if (cur == start) break;
        }
        if (loop.size() < 3) continue;
        Face g;
        g.v = loop;
        if (uv && uvs.size() == loop.size()) g.uv = uvs;
        g.material = m.faces[fs[0]].material;
        g.smooth = m.faces[fs[0]].smooth;
        std::vector<char> kill(m.faces.size(), 0);
        for (auto fi : fs) kill[fi] = 1;
        std::vector<Face> faces;
        for (std::size_t f = 0; f < m.faces.size(); ++f)
            if (!kill[f]) faces.push_back(std::move(m.faces[f]));
        faces.push_back(std::move(g));
        m.faces = std::move(faces);
        m.verts[v].sel = false;
        std::vector<std::array<std::uint32_t, 2>> loose;
        for (const auto& e : m.loose)
            if (e[0] != v && e[1] != v) loose.push_back(e);
        m.loose = std::move(loose);
        cleanup(m);
        ++count;
    }
    for (auto& v : m.verts) v.sel = false;
    flushSelection(m, SelectMode::Vertex);
    return count;
}

std::size_t limitedDissolve(PolyMesh& m, double degrees, bool selectedOnly) {
    Topology t(m);
    const double c = std::cos(degrees * mesh::kPi / 180.0);
    std::set<EdgeKey> flat;
    for (std::size_t e = 0; e < t.edges.size(); ++e) {
        const auto& ef = t.edgeFaces[e];
        if (ef.size() != 2) continue;
        const Face& a = m.faces[ef[0].face];
        const Face& b = m.faces[ef[1].face];
        if (selectedOnly && !(a.sel && b.sel)) continue;
        if (a.material != b.material || m.seams.count(t.edges[e]) || m.sharp.count(t.edges[e])) continue;
        if (mesh::dot(faceNormal(m, a), faceNormal(m, b)) >= c) flat.insert(t.edges[e]);
    }
    // Grow regions across flat edges (longest first), but only while each
    // region stays a disk: two disks join into a disk when they meet along one
    // run of edges (shared vertices = shared edges + 1). A flat area with holes
    // (a wall with windows) ends up as a few n-gons instead of not at all.
    std::vector<EdgeKey> order(flat.begin(), flat.end());
    auto len = [&](EdgeKey k) { return mesh::length(m.verts[keyA(k)].p - m.verts[keyB(k)].p); };
    std::stable_sort(order.begin(), order.end(), [&](EdgeKey x, EdgeKey y) { return len(x) > len(y); });
    std::vector<std::uint32_t> region(m.faces.size());
    std::iota(region.begin(), region.end(), 0u);
    std::vector<std::vector<std::uint32_t>> members(m.faces.size());
    std::vector<std::set<std::uint32_t>> verts(m.faces.size());
    for (std::uint32_t f = 0; f < m.faces.size(); ++f) {
        members[f] = {f};
        verts[f].insert(m.faces[f].v.begin(), m.faces[f].v.end());
    }
    std::set<EdgeKey> joined;
    for (auto k : order) {
        const auto& ef = t.edgeFaces[t.index.at(k)];
        std::uint32_t r1 = region[ef[0].face], r2 = region[ef[1].face];
        if (r1 == r2) {
            joined.insert(k);
            continue;
        }
        if (members[r1].size() > members[r2].size()) std::swap(r1, r2);
        std::size_t sharedEdges = 0;
        for (auto f : members[r1]) {
            const auto& fv = m.faces[f].v;
            for (std::size_t i = 0; i < fv.size(); ++i) {
                const auto& hf = t.edgeFaces[t.index.at(edgeKey(fv[i], fv[(i + 1) % fv.size()]))];
                for (const auto& h : hf)
                    if (region[h.face] == r2) ++sharedEdges;
            }
        }
        std::size_t sharedVerts = 0;
        for (auto v : verts[r1]) sharedVerts += verts[r2].count(v);
        if (sharedVerts != sharedEdges + 1) continue;
        for (auto f : members[r1]) region[f] = r2;
        members[r2].insert(members[r2].end(), members[r1].begin(), members[r1].end());
        members[r1].clear();
        verts[r2].insert(verts[r1].begin(), verts[r1].end());
        verts[r1].clear();
        joined.insert(k);
    }
    for (auto k : order) {
        const auto& ef = t.edgeFaces[t.index.at(k)];
        if (region[ef[0].face] == region[ef[1].face]) joined.insert(k);
    }
    return dissolveEdgeSet(m, joined, true, degrees);
}

std::size_t remove(PolyMesh& m, DeleteMode mode) {
    const SelectionCount before = countSelection(m);
    switch (mode) {
        case DeleteMode::Verts: {
            std::vector<char> kill(m.verts.size(), 0);
            for (std::size_t v = 0; v < m.verts.size(); ++v) kill[v] = m.verts[v].sel;
            removeVertices(m, kill);
            return before.verts;
        }
        case DeleteMode::Edges: {
            std::vector<char> kill(m.faces.size(), 0);
            for (std::size_t f = 0; f < m.faces.size(); ++f) {
                const auto& fv = m.faces[f].v;
                for (std::size_t i = 0; i < fv.size() && !kill[f]; ++i)
                    if (m.esel.count(edgeKey(fv[i], fv[(i + 1) % fv.size()]))) kill[f] = 1;
            }
            std::vector<std::array<std::uint32_t, 2>> loose;
            for (const auto& e : m.loose)
                if (!m.esel.count(edgeKey(e[0], e[1]))) loose.push_back(e);
            m.loose = std::move(loose);
            m.esel.clear();
            removeFaces(m, kill);
            return before.edges;
        }
        case DeleteMode::Faces:
        case DeleteMode::OnlyFaces: {
            std::vector<char> kill(m.faces.size(), 0);
            for (std::size_t f = 0; f < m.faces.size(); ++f) kill[f] = m.faces[f].sel;
            if (mode == DeleteMode::OnlyFaces) {
                // their edges stay as loose edges where no other face keeps them
                std::set<EdgeKey> kept, gone;
                for (std::size_t f = 0; f < m.faces.size(); ++f) {
                    const auto& fv = m.faces[f].v;
                    for (std::size_t i = 0; i < fv.size(); ++i) (kill[f] ? gone : kept).insert(edgeKey(fv[i], fv[(i + 1) % fv.size()]));
                }
                for (auto k : gone)
                    if (!kept.count(k)) m.loose.push_back({keyA(k), keyB(k)});
            }
            removeFaces(m, kill);
            return before.faces;
        }
    }
    return 0;
}

std::size_t bridge(PolyMesh& m, int twist) {
    std::vector<Chain> loops;
    bool anyFace = false;
    for (const auto& f : m.faces) anyFace = anyFace || f.sel;
    if (anyFace) {
        // the outlines of the selected areas, then the areas go
        Topology t(m);
        std::vector<EdgeKey> outline;
        for (std::size_t e = 0; e < t.edges.size(); ++e) {
            std::size_t s = 0;
            for (const auto& h : t.edgeFaces[e]) s += m.faces[h.face].sel ? 1 : 0;
            if (s == 1) outline.push_back(t.edges[e]);
        }
        loops = chains(outline);
        std::vector<char> kill(m.faces.size(), 0);
        for (std::size_t f = 0; f < m.faces.size(); ++f) kill[f] = m.faces[f].sel;
        // keep the vertices: remember positions, since removing faces renumbers them
        std::vector<std::vector<Vec3>> pos;
        for (const auto& c : loops) {
            pos.emplace_back();
            for (auto v : c.v) pos.back().push_back(m.verts[v].p);
        }
        std::vector<Face> faces;
        for (std::size_t f = 0; f < m.faces.size(); ++f)
            if (!kill[f]) faces.push_back(std::move(m.faces[f]));
        m.faces = std::move(faces);
        // vertices used only by the removed faces would be dropped by cleanup; keep them with loose edges for now
        for (const auto& c : loops)
            for (std::size_t i = 0; i < c.v.size(); ++i) m.loose.push_back({c.v[i], c.v[(i + 1) % c.v.size()]});
        cleanup(m);
        // find the loop vertices again by position
        std::map<std::array<double, 3>, std::uint32_t> at;
        for (std::uint32_t v = 0; v < m.verts.size(); ++v) at[{m.verts[v].p.x, m.verts[v].p.y, m.verts[v].p.z}] = v;
        for (std::size_t i = 0; i < loops.size(); ++i)
            for (std::size_t j = 0; j < loops[i].v.size(); ++j) loops[i].v[j] = at[{pos[i][j].x, pos[i][j].y, pos[i][j].z}];
    } else {
        loops = chains(selectedEdges(m));
    }
    if (loops.size() < 2) throw std::runtime_error("bridge needs two or more edge loops (or face areas) selected");
    for (const auto& c : loops) {
        if (c.closed != loops[0].closed) throw std::runtime_error("bridge can't join a closed loop to an open chain");
        if (c.v.size() != loops[0].v.size())
            throw std::runtime_error("the loops need the same number of edges (" + std::to_string(loops[0].v.size() - (loops[0].closed ? 0 : 1)) + " and " +
                                     std::to_string(c.v.size() - (c.closed ? 0 : 1)) + ")");
        if (c.v.size() < 2) throw std::runtime_error("a selected chain has only one vertex");
    }
    // order several loops along the line through the two farthest apart
    if (loops.size() > 2) {
        std::vector<Vec3> cs;
        for (const auto& c : loops) cs.push_back(centroid(m, c.v));
        std::size_t ia = 0, ib = 1;
        double best = -1;
        for (std::size_t i = 0; i < cs.size(); ++i)
            for (std::size_t j = i + 1; j < cs.size(); ++j)
                if (mesh::length(cs[i] - cs[j]) > best) best = mesh::length(cs[i] - cs[j]), ia = i, ib = j;
        const Vec3 axis = cs[ib] - cs[ia];
        std::vector<std::size_t> order(loops.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return mesh::dot(cs[a], axis) < mesh::dot(cs[b], axis); });
        std::vector<Chain> sorted;
        for (auto i : order) sorted.push_back(loops[i]);
        loops = std::move(sorted);
    }
    Topology t(m);
    for (auto& f : m.faces) f.sel = false;
    std::vector<Face> made;
    for (std::size_t li = 0; li + 1 < loops.size(); ++li) {
        Chain A = loops[li], B = loops[li + 1];
        int da = chainDirection(m, t, A), db = chainDirection(m, t, B);
        if (da == 0 && db != 0) {
            std::swap(A, B);
            std::swap(da, db);
        }
        if (da < 0) {
            std::reverse(A.v.begin(), A.v.end());
            da = 1;
        }
        const std::size_t n = A.v.size();
        const bool closed = A.closed;
        // candidate pairings: sigma(i) = k - i (against A) or k + i (with A)
        struct Pairing {
            int sign;
            std::size_t k;
            double cost;
        };
        std::vector<Pairing> cands;
        auto sigma = [&](int sign, std::size_t k, std::size_t i) {
            const long long nn = static_cast<long long>(n);
            long long s = static_cast<long long>(k) + sign * static_cast<long long>(i);
            return static_cast<std::size_t>(((s % nn) + nn) % nn);
        };
        for (int sign : {-1, 1}) {
            if (db != 0 && sign != (db > 0 ? -1 : 1) && da != 0) continue;
            const std::size_t ks = closed ? n : 1;
            for (std::size_t k0 = 0; k0 < ks; ++k0) {
                const std::size_t k = closed ? k0 : (sign < 0 ? n - 1 : 0);
                double cost = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    const Vec3 d = m.verts[A.v[i]].p - m.verts[B.v[sigma(sign, k, i)]].p;
                    cost += mesh::dot(d, d);
                }
                cands.push_back({sign, k, cost});
            }
        }
        if (cands.empty()) continue;
        const Pairing best = *std::min_element(cands.begin(), cands.end(), [](const Pairing& a, const Pairing& b) { return a.cost < b.cost; });
        const std::size_t k = closed ? (best.k + static_cast<std::size_t>(((twist % static_cast<int>(n)) + static_cast<int>(n)) % static_cast<int>(n))) % n : best.k;
        const std::size_t edges = closed ? n : n - 1;
        std::vector<Face> part;
        for (std::size_t i = 0; i < edges; ++i) {
            Face q;
            q.v = {A.v[(i + 1) % n], A.v[i], B.v[sigma(best.sign, k, i)], B.v[sigma(best.sign, k, i + 1)]};
            q.sel = true;
            part.push_back(std::move(q));
        }
        if (da == 0 && db == 0) {
            // nothing to match: face away from the middle of the two loops
            std::vector<std::uint32_t> all = A.v;
            all.insert(all.end(), B.v.begin(), B.v.end());
            const Vec3 mid = centroid(m, all);
            double s = 0;
            for (const auto& q : part) s += mesh::dot(faceNormal(m, q), faceCenter(m, q) - mid);
            if (s < 0)
                for (auto& q : part) std::reverse(q.v.begin(), q.v.end());
        }
        for (auto& q : part) made.push_back(std::move(q));
    }
    const std::size_t count = made.size();
    for (auto& q : made) m.faces.push_back(std::move(q));
    cleanup(m);
    flushSelection(m, SelectMode::Face);
    return count;
}

std::size_t fill(PolyMesh& m) {
    Topology t(m);
    std::vector<EdgeKey> edges = selectedEdges(m);
    std::size_t made = 0;
    if (!edges.empty()) {
        std::vector<Chain> cs;
        try {
            cs = chains(edges);
        } catch (const std::exception&) {
            cs.clear();
        }
        bool allClosed = !cs.empty();
        for (const auto& c : cs) allClosed = allClosed && c.closed && c.v.size() >= 3;
        if (allClosed) {
            for (auto& c : cs) {
                orientNewFace(m, t, c.v);
                Face& f = m.faces[m.addFace(c.v)];
                f.sel = true;
                ++made;
            }
            cleanup(m);
            flushSelection(m, SelectMode::Face);
            return made;
        }
    }
    std::vector<std::uint32_t> vs = selectedVerts(m);
    if (vs.size() == 2) {
        if (t.edge(vs[0], vs[1]) >= 0) return 0;
        m.loose.push_back({vs[0], vs[1]});
        cleanup(m);
        flushSelection(m, SelectMode::Vertex);
        return 1;
    }
    if (vs.size() < 3) return 0;
    // order the vertices around their middle, in their best plane
    const Vec3 c = centroid(m, vs);
    std::uint32_t far = vs[0];
    for (auto v : vs)
        if (mesh::length(m.verts[v].p - c) > mesh::length(m.verts[far].p - c)) far = v;
    const Vec3 u = mesh::normalize(m.verts[far].p - c);
    Vec3 nrm;
    for (auto v : vs) {
        const Vec3 x = mesh::cross(u, m.verts[v].p - c);
        if (mesh::length(x) > mesh::length(nrm)) nrm = x;
    }
    nrm = mesh::normalize(nrm);
    if (mesh::length(nrm) == 0) return 0;  // all in a line
    const Vec3 w = mesh::cross(nrm, u);
    std::sort(vs.begin(), vs.end(), [&](std::uint32_t a, std::uint32_t b) {
        const Vec3 pa = m.verts[a].p - c, pb = m.verts[b].p - c;
        return std::atan2(mesh::dot(pa, w), mesh::dot(pa, u)) < std::atan2(mesh::dot(pb, w), mesh::dot(pb, u));
    });
    orientNewFace(m, t, vs);
    Face& f = m.faces[m.addFace(vs)];
    f.sel = true;
    cleanup(m);
    flushSelection(m, SelectMode::Face);
    return 1;
}

std::size_t fillHoles(PolyMesh& m, bool selectedOnly) {
    std::set<std::pair<std::uint32_t, std::uint32_t>> halfs;
    for (const auto& f : m.faces)
        for (std::size_t i = 0; i < f.v.size(); ++i) halfs.insert({f.v[i], f.v[(i + 1) % f.v.size()]});
    std::multimap<std::uint32_t, std::uint32_t> open;
    for (const auto& [a, b] : halfs)
        if (!halfs.count({b, a})) open.emplace(a, b);
    std::set<std::uint32_t> used;
    std::size_t made = 0;
    for (const auto& [a0, b0] : open) {
        if (used.count(a0) || open.count(a0) > 1) continue;
        std::vector<std::uint32_t> loop{a0};
        std::uint32_t cur = b0;
        bool closed = false;
        for (std::size_t g = 0; g <= open.size(); ++g) {
            if (cur == a0) {
                closed = true;
                break;
            }
            if (open.count(cur) != 1) break;
            loop.push_back(cur);
            cur = open.find(cur)->second;
        }
        for (auto x : loop) used.insert(x);
        if (!closed || loop.size() < 3) continue;
        bool sel = true;
        for (auto v : loop) sel = sel && m.verts[v].sel;
        if (selectedOnly && !sel) continue;
        std::reverse(loop.begin(), loop.end());
        Face& f = m.faces[m.addFace(loop)];
        f.sel = true;
        ++made;
    }
    if (made) {
        cleanup(m);
        flushSelection(m, SelectMode::Face);
    }
    return made;
}

std::size_t recalcNormals(PolyMesh& m, bool inside, bool selectedOnly) {
    Topology t(m);
    std::vector<char> eligible(m.faces.size(), 1);
    if (selectedOnly)
        for (std::size_t f = 0; f < m.faces.size(); ++f) eligible[f] = m.faces[f].sel;
    std::vector<int> flip(m.faces.size(), -1);  // -1 unvisited, 0 keep, 1 flip
    std::size_t flipped = 0;
    for (std::uint32_t f0 = 0; f0 < m.faces.size(); ++f0) {
        if (!eligible[f0] || flip[f0] >= 0) continue;
        std::vector<std::uint32_t> comp{f0}, stack{f0};
        flip[f0] = 0;
        bool open = false;
        while (!stack.empty()) {
            const std::uint32_t f = stack.back();
            stack.pop_back();
            const auto& fv = m.faces[f].v;
            for (std::uint32_t c = 0; c < fv.size(); ++c) {
                const std::uint32_t a = fv[c];
                const auto& ef = t.edgeFaces[t.index.at(edgeKey(a, fv[(c + 1) % fv.size()]))];
                if (ef.size() == 1) open = true;
                if (ef.size() != 2) continue;
                const HalfRef& o = ef[0].face == f ? ef[1] : ef[0];
                if (!eligible[o.face]) {
                    open = true;
                    continue;
                }
                const bool same = m.faces[o.face].v[o.corner] == a;  // both walk a -> b
                const int want = same ? 1 - flip[f] : flip[f];
                if (flip[o.face] < 0) {
                    flip[o.face] = want;
                    comp.push_back(o.face);
                    stack.push_back(o.face);
                }
            }
        }
        // which way is out: signed volume for closed pieces, else away from the piece's middle
        auto oriented = [&](std::uint32_t f) {
            Face g = m.faces[f];
            if (flip[f]) std::reverse(g.v.begin(), g.v.end());
            return g;
        };
        double score = 0;
        if (!open) {
            for (auto f : comp) {
                const Face g = oriented(f);
                const auto tri = triangulateFace(m, g);
                for (std::size_t k = 0; k + 2 < tri.size(); k += 3) {
                    const Vec3 &a = m.verts[g.v[tri[k]]].p, &b = m.verts[g.v[tri[k + 1]]].p, &c = m.verts[g.v[tri[k + 2]]].p;
                    score += mesh::dot(a, mesh::cross(b, c));
                }
            }
        }
        if (open || std::fabs(score) < 1e-18) {
            score = 0;
            Vec3 mid;
            double area = 0;
            for (auto f : comp) {
                const double a = faceArea(m, m.faces[f]);
                mid += faceCenter(m, m.faces[f]) * a;
                area += a;
            }
            mid = area > 0 ? mid / area : mid;
            for (auto f : comp) {
                const Face g = oriented(f);
                score += mesh::dot(faceNormal(m, g), faceCenter(m, g) - mid) * faceArea(m, g);
            }
        }
        const bool flipAll = (score < 0) != inside;
        for (auto f : comp) {
            const bool doFlip = (flip[f] == 1) != flipAll;
            if (!doFlip) continue;
            std::reverse(m.faces[f].v.begin(), m.faces[f].v.end());
            std::reverse(m.faces[f].uv.begin(), m.faces[f].uv.end());
            ++flipped;
        }
    }
    return flipped;
}

std::size_t flipNormals(PolyMesh& m, bool selectedOnly) {
    std::size_t n = 0;
    for (auto& f : m.faces) {
        if (selectedOnly && !f.sel) continue;
        std::reverse(f.v.begin(), f.v.end());
        std::reverse(f.uv.begin(), f.uv.end());
        ++n;
    }
    return n;
}

std::size_t triangulateFaces(PolyMesh& m, bool selectedOnly) {
    std::vector<Face> out;
    std::size_t n = 0;
    for (auto& f : m.faces) {
        if (f.v.size() == 3 || (selectedOnly && !f.sel)) {
            out.push_back(std::move(f));
            continue;
        }
        const auto tri = triangulateFace(m, f);
        const bool uv = f.uv.size() == f.v.size();
        for (std::size_t k = 0; k + 2 < tri.size(); k += 3) {
            Face g;
            g.v = {f.v[tri[k]], f.v[tri[k + 1]], f.v[tri[k + 2]]};
            if (uv) g.uv = {f.uv[tri[k]], f.uv[tri[k + 1]], f.uv[tri[k + 2]]};
            g.material = f.material;
            g.smooth = f.smooth;
            g.sel = f.sel;
            out.push_back(std::move(g));
        }
        ++n;
    }
    m.faces = std::move(out);
    return n;
}

std::size_t trisToQuads(PolyMesh& m, double degrees, bool selectedOnly) {
    Topology t(m);
    const double cosLimit = std::cos(degrees * mesh::kPi / 180.0);
    struct Cand {
        double score;
        std::uint32_t f0, f1;
        std::vector<std::uint32_t> quad;
        std::vector<Vec2> uv;
    };
    std::vector<Cand> cands;
    for (std::size_t e = 0; e < t.edges.size(); ++e) {
        const auto& ef = t.edgeFaces[e];
        if (ef.size() != 2) continue;
        const Face& A = m.faces[ef[0].face];
        const Face& B = m.faces[ef[1].face];
        if (A.v.size() != 3 || B.v.size() != 3 || A.material != B.material) continue;
        if (selectedOnly && !(A.sel && B.sel)) continue;
        if (A.v[ef[0].corner] == B.v[ef[1].corner]) continue;  // flipped neighbours
        const Vec3 na = faceNormal(m, A), nb = faceNormal(m, B);
        if (mesh::dot(na, nb) < cosLimit) continue;
        // A walks a -> b -> c, B walks b -> a -> d: the quad is a, d, b, c
        const std::uint32_t ca = ef[0].corner, cb = ef[1].corner;
        const std::uint32_t a = A.v[ca], b = A.v[(ca + 1) % 3], c = A.v[(ca + 2) % 3], d = B.v[(cb + 2) % 3];
        std::vector<std::uint32_t> q{a, d, b, c};
        const Vec3 n = mesh::normalize(na + nb);
        bool convex = true;
        double shape = 0;
        for (int i = 0; i < 4; ++i) {
            const Vec3 p0 = m.verts[q[(i + 3) % 4]].p, p1 = m.verts[q[i]].p, p2 = m.verts[q[(i + 1) % 4]].p;
            if (mesh::dot(mesh::cross(p1 - p0, p2 - p1), n) <= 0) convex = false;
            const double ang = std::acos(std::clamp(mesh::dot(mesh::normalize(p0 - p1), mesh::normalize(p2 - p1)), -1.0, 1.0));
            shape += std::fabs(ang - mesh::kPi / 2);
        }
        if (!convex) continue;
        Cand cd{shape + std::acos(std::clamp(mesh::dot(na, nb), -1.0, 1.0)), ef[0].face, ef[1].face, q, {}};
        if (A.uv.size() == 3 && B.uv.size() == 3) cd.uv = {A.uv[ca], B.uv[(cb + 2) % 3], A.uv[(ca + 1) % 3], A.uv[(ca + 2) % 3]};
        cands.push_back(std::move(cd));
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    std::vector<char> used(m.faces.size(), 0);
    std::vector<Face> made;
    for (auto& cd : cands) {
        if (used[cd.f0] || used[cd.f1]) continue;
        used[cd.f0] = used[cd.f1] = 1;
        Face g;
        g.v = cd.quad;
        g.uv = cd.uv;
        g.material = m.faces[cd.f0].material;
        g.smooth = m.faces[cd.f0].smooth;
        g.sel = m.faces[cd.f0].sel;
        made.push_back(std::move(g));
    }
    std::vector<Face> faces;
    for (std::size_t f = 0; f < m.faces.size(); ++f)
        if (!used[f]) faces.push_back(std::move(m.faces[f]));
    const std::size_t n = made.size();
    for (auto& g : made) faces.push_back(std::move(g));
    m.faces = std::move(faces);
    return n;
}

}  // namespace wetwaresuite::model
