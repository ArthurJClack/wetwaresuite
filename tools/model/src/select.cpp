// Selection tools: loops, rings, linked, grow/shrink, by normal, sharp,
// non-manifold, random, material.
#include <algorithm>
#include <cmath>
#include <random>

#include "wetwaresuite/model/ops.hpp"

namespace wetwaresuite::model {

namespace {

// The other edge at pivot v that continues a loop from edge (u, v).
int nextLoopEdge(const PolyMesh& m, const Topology& t, std::uint32_t cur, std::uint32_t v) {
    const EdgeKey ck = t.edges[cur];
    const auto& curFaces = t.edgeFaces[cur];
    const auto& around = t.vertEdges[v];
    if (curFaces.size() == 1) {
        // walk along the boundary
        int next = -1, found = 0;
        for (auto e : around) {
            if (e == cur || t.edgeFaces[e].size() != 1) continue;
            next = static_cast<int>(e);
            ++found;
        }
        return found == 1 ? next : -1;
    }
    if (curFaces.size() != 2 || around.size() != 4) return -1;
    int next = -1, found = 0;
    for (auto e : around) {
        if (e == cur) continue;
        const std::uint32_t w = keyA(t.edges[e]) == v ? keyB(t.edges[e]) : keyA(t.edges[e]);
        bool shares = false;
        for (const auto& h : curFaces) {
            const auto& fv = m.faces[h.face].v;
            if (std::find(fv.begin(), fv.end(), w) != fv.end()) shares = true;
        }
        if (shares) continue;
        next = static_cast<int>(e);
        ++found;
    }
    (void)ck;
    return found == 1 ? next : -1;
}

}  // namespace

std::vector<EdgeKey> edgeLoop(const PolyMesh& m, const Topology& t, EdgeKey e) {
    const int start = t.edge(keyA(e), keyB(e));
    if (start < 0) return {};
    std::vector<EdgeKey> out{e};
    std::vector<char> seen(t.edges.size(), 0);
    seen[static_cast<std::size_t>(start)] = 1;
    for (std::uint32_t pivot0 : {keyA(e), keyB(e)}) {
        std::uint32_t cur = static_cast<std::uint32_t>(start), pivot = pivot0;
        while (true) {
            const int next = nextLoopEdge(m, t, cur, pivot);
            if (next < 0 || seen[static_cast<std::size_t>(next)]) break;
            seen[static_cast<std::size_t>(next)] = 1;
            out.push_back(t.edges[static_cast<std::size_t>(next)]);
            const EdgeKey nk = t.edges[static_cast<std::size_t>(next)];
            pivot = keyA(nk) == pivot ? keyB(nk) : keyA(nk);
            cur = static_cast<std::uint32_t>(next);
        }
    }
    return out;
}

std::vector<EdgeKey> edgeRing(const PolyMesh& m, const Topology& t, EdgeKey e) {
    const int start = t.edge(keyA(e), keyB(e));
    if (start < 0) return {};
    std::vector<EdgeKey> out{e};
    std::vector<char> seenFace(m.faces.size(), 0), seenEdge(t.edges.size(), 0);
    seenEdge[static_cast<std::size_t>(start)] = 1;
    for (const auto& h0 : t.edgeFaces[static_cast<std::size_t>(start)]) {
        HalfRef h = h0;
        while (true) {
            if (seenFace[h.face]) break;
            const Face& f = m.faces[h.face];
            if (f.v.size() != 4) break;
            seenFace[h.face] = 1;
            const std::uint32_t c = (h.corner + 2) % 4;
            const int opp = t.edge(f.v[c], f.v[(c + 1) % 4]);
            if (opp < 0 || seenEdge[static_cast<std::size_t>(opp)]) break;
            seenEdge[static_cast<std::size_t>(opp)] = 1;
            out.push_back(t.edges[static_cast<std::size_t>(opp)]);
            const auto& nf = t.edgeFaces[static_cast<std::size_t>(opp)];
            if (nf.size() != 2) break;
            h = nf[0].face == h.face ? nf[1] : nf[0];
        }
    }
    return out;
}

void selectLinked(PolyMesh& m, SelectMode mode, bool delimitSeams) {
    flushSelection(m, mode);
    Topology t(m);
    std::vector<char> on(m.verts.size(), 0);
    std::vector<std::uint32_t> stack;
    for (std::uint32_t v = 0; v < m.verts.size(); ++v)
        if (m.verts[v].sel) {
            on[v] = 1;
            stack.push_back(v);
        }
    while (!stack.empty()) {
        const std::uint32_t v = stack.back();
        stack.pop_back();
        for (auto e : t.vertEdges[v]) {
            const EdgeKey k = t.edges[e];
            if (delimitSeams && m.seams.count(k)) continue;
            const std::uint32_t w = keyA(k) == v ? keyB(k) : keyA(k);
            if (!on[w]) {
                on[w] = 1;
                stack.push_back(w);
            }
        }
    }
    if (mode == SelectMode::Face) {
        // faces: everything reachable through shared edges
        std::vector<char> fon(m.faces.size(), 0);
        std::vector<std::uint32_t> fs;
        for (std::uint32_t f = 0; f < m.faces.size(); ++f)
            if (m.faces[f].sel) {
                fon[f] = 1;
                fs.push_back(f);
            }
        while (!fs.empty()) {
            const std::uint32_t f = fs.back();
            fs.pop_back();
            const auto& fv = m.faces[f].v;
            for (std::size_t i = 0; i < fv.size(); ++i) {
                const EdgeKey k = edgeKey(fv[i], fv[(i + 1) % fv.size()]);
                if (delimitSeams && m.seams.count(k)) continue;
                for (const auto& h : t.edgeFaces[t.index.at(k)])
                    if (!fon[h.face]) {
                        fon[h.face] = 1;
                        fs.push_back(h.face);
                    }
            }
        }
        for (std::size_t f = 0; f < m.faces.size(); ++f) m.faces[f].sel = fon[f];
        flushSelection(m, SelectMode::Face);
        return;
    }
    for (std::size_t v = 0; v < m.verts.size(); ++v) m.verts[v].sel = on[v];
    flushSelection(m, SelectMode::Vertex);
    if (mode == SelectMode::Edge) flushSelection(m, SelectMode::Edge);
}

void growSelection(PolyMesh& m, SelectMode mode, int steps) {
    for (int s = 0; s < std::abs(steps); ++s) {
        const bool grow = steps > 0;
        if (mode == SelectMode::Face) {
            // faces sharing a vertex with the selection (grow), or touching an unselected face (shrink)
            std::vector<char> touched(m.verts.size(), 0);
            for (const auto& f : m.faces)
                if (f.sel == grow)
                    for (auto v : f.v) touched[v] = 1;
            for (auto& f : m.faces) {
                bool any = false;
                for (auto v : f.v) any = any || touched[v];
                if (grow && any) f.sel = true;
                if (!grow && any) f.sel = false;
            }
            flushSelection(m, SelectMode::Face);
            continue;
        }
        std::vector<char> next(m.verts.size());
        for (std::size_t v = 0; v < m.verts.size(); ++v) next[v] = m.verts[v].sel;
        auto touch = [&](const std::vector<std::uint32_t>& vs) {
            bool hasSel = false, hasUnsel = false;
            for (auto v : vs) (m.verts[v].sel ? hasSel : hasUnsel) = true;
            if (grow && hasSel)
                for (auto v : vs) next[v] = 1;
            if (!grow && hasUnsel)
                for (auto v : vs) next[v] = 0;
        };
        for (const auto& f : m.faces) touch(f.v);
        for (const auto& e : m.loose) touch({e[0], e[1]});
        for (std::size_t v = 0; v < m.verts.size(); ++v) m.verts[v].sel = next[v];
        flushSelection(m, SelectMode::Vertex);
        if (mode == SelectMode::Edge) flushSelection(m, SelectMode::Edge);
    }
}

std::size_t selectByNormal(PolyMesh& m, const Vec3& dir, double degrees) {
    const Vec3 d = mesh::normalize(dir);
    const double c = std::cos(degrees * mesh::kPi / 180.0);
    std::size_t n = 0;
    for (auto& f : m.faces) {
        if (mesh::dot(faceNormal(m, f), d) >= c - 1e-12) {
            f.sel = true;
            ++n;
        }
    }
    flushSelection(m, SelectMode::Face);
    return n;
}

std::size_t selectNonManifold(PolyMesh& m) {
    Topology t(m);
    std::size_t n = 0;
    for (std::size_t e = 0; e < t.edges.size(); ++e) {
        const std::size_t c = t.edgeFaces[e].size();
        if (c == 1 || c > 2) {
            m.esel.insert(t.edges[e]);
            ++n;
        }
    }
    flushSelection(m, SelectMode::Edge);
    return n;
}

std::size_t selectSharpEdges(PolyMesh& m, double degrees) {
    Topology t(m);
    const double c = std::cos(degrees * mesh::kPi / 180.0);
    std::size_t n = 0;
    for (std::size_t e = 0; e < t.edges.size(); ++e) {
        const auto& ef = t.edgeFaces[e];
        if (ef.size() != 2) continue;
        if (mesh::dot(faceNormal(m, m.faces[ef[0].face]), faceNormal(m, m.faces[ef[1].face])) < c) {
            m.esel.insert(t.edges[e]);
            ++n;
        }
    }
    flushSelection(m, SelectMode::Edge);
    return n;
}

std::size_t selectRandom(PolyMesh& m, SelectMode mode, double ratio, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::size_t n = 0;
    if (mode == SelectMode::Face) {
        for (auto& f : m.faces)
            if (u(rng) < ratio) {
                f.sel = true;
                ++n;
            }
    } else if (mode == SelectMode::Edge) {
        for (auto k : allEdges(m))
            if (u(rng) < ratio) {
                m.esel.insert(k);
                ++n;
            }
    } else {
        for (auto& v : m.verts)
            if (u(rng) < ratio) {
                v.sel = true;
                ++n;
            }
    }
    flushSelection(m, mode);
    return n;
}

std::size_t selectMaterial(PolyMesh& m, int slot) {
    std::size_t n = 0;
    for (auto& f : m.faces)
        if (f.material == slot) {
            f.sel = true;
            ++n;
        }
    flushSelection(m, SelectMode::Face);
    return n;
}

}  // namespace wetwaresuite::model
