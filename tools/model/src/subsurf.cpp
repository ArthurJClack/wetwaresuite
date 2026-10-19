// Catmull-Clark subdivision surfaces.
#include <stdexcept>

#include "wetwaresuite/model/ops.hpp"

namespace wetwaresuite::model {

PolyMesh catmullClark(const PolyMesh& input, int levels, std::size_t maxFaces) {
    PolyMesh m = input;
    for (int level = 0; level < levels; ++level) {
        std::size_t corners = 0;
        for (const auto& f : m.faces) corners += f.v.size();
        if (corners > maxFaces) {
            throw std::length_error("subdivision would make " + std::to_string(corners) + " faces (limit " + std::to_string(maxFaces) +
                                    "); use fewer levels");
        }
        Topology t(m);
        const std::size_t V = m.verts.size(), E = t.edges.size(), F = m.faces.size();
        std::vector<Vec3> fp(F), fc(F);
        for (std::size_t f = 0; f < F; ++f) {
            for (auto v : m.faces[f].v) {
                fp[f] += m.verts[v].p;
                fc[f] += m.verts[v].color;
            }
            fp[f] = fp[f] / static_cast<double>(m.faces[f].v.size());
            fc[f] = fc[f] / static_cast<double>(m.faces[f].v.size());
        }
        // crisp edges: boundaries, sharp edges, loose edges, non-manifold edges
        std::vector<char> crisp(E, 0);
        for (std::size_t e = 0; e < E; ++e) crisp[e] = t.edgeFaces[e].size() != 2 || m.sharp.count(t.edges[e]) > 0;
        std::vector<Vec3> ep(E);
        for (std::size_t e = 0; e < E; ++e) {
            const Vec3 a = m.verts[keyA(t.edges[e])].p, b = m.verts[keyB(t.edges[e])].p;
            if (crisp[e]) ep[e] = (a + b) * 0.5;
            else ep[e] = (a + b + fp[t.edgeFaces[e][0].face] + fp[t.edgeFaces[e][1].face]) * 0.25;
        }
        std::vector<Vec3> vp(V);
        for (std::uint32_t v = 0; v < V; ++v) {
            const Vec3 S = m.verts[v].p;
            const auto& es = t.vertEdges[v];
            const auto& fs = t.vertFaces[v];
            std::vector<std::uint32_t> hard;
            for (auto e : es)
                if (crisp[e]) hard.push_back(e);
            if (fs.empty() || hard.size() > 2) {
                vp[v] = S;  // a corner (or a loose vertex) stays put
            } else if (hard.size() == 2) {
                auto other = [&](std::uint32_t e) {
                    const EdgeKey k = t.edges[e];
                    return m.verts[keyA(k) == v ? keyB(k) : keyA(k)].p;
                };
                vp[v] = (other(hard[0]) + S * 6.0 + other(hard[1])) / 8.0;
            } else {
                const double n = static_cast<double>(es.size());
                Vec3 Q, R;
                for (auto f : fs) Q += fp[f];
                Q = Q / static_cast<double>(fs.size());
                for (auto e : es) R += (m.verts[keyA(t.edges[e])].p + m.verts[keyB(t.edges[e])].p) * 0.5;
                R = R / n;
                vp[v] = (Q + R * 2.0 + S * (n - 3)) / n;
            }
        }
        PolyMesh out;
        out.colors = m.colors;
        out.verts.resize(V + E + F);
        for (std::size_t v = 0; v < V; ++v) {
            out.verts[v].p = vp[v];
            out.verts[v].color = m.verts[v].color;
        }
        for (std::size_t e = 0; e < E; ++e) {
            out.verts[V + e].p = ep[e];
            out.verts[V + e].color = (m.verts[keyA(t.edges[e])].color + m.verts[keyB(t.edges[e])].color) * 0.5;
        }
        for (std::size_t f = 0; f < F; ++f) {
            out.verts[V + E + f].p = fp[f];
            out.verts[V + E + f].color = fc[f];
        }
        auto edgeVert = [&](std::uint32_t a, std::uint32_t b) { return static_cast<std::uint32_t>(V + t.index.at(edgeKey(a, b))); };
        out.faces.reserve(corners);
        for (std::size_t f = 0; f < F; ++f) {
            const Face& src = m.faces[f];
            const std::size_t n = src.v.size();
            const bool uv = src.uv.size() == n;
            Vec2 cuv;
            if (uv)
                for (const auto& u : src.uv) cuv += u;
            if (uv) cuv = cuv / static_cast<double>(n);
            for (std::size_t c = 0; c < n; ++c) {
                const std::uint32_t prev = src.v[(c + n - 1) % n], cur = src.v[c], next = src.v[(c + 1) % n];
                Face q;
                q.v = {cur, edgeVert(cur, next), static_cast<std::uint32_t>(V + E + f), edgeVert(prev, cur)};
                if (uv) {
                    const Vec2 up = src.uv[(c + n - 1) % n], uc = src.uv[c], un = src.uv[(c + 1) % n];
                    q.uv = {uc, (uc + un) * 0.5, cuv, (up + uc) * 0.5};
                }
                q.material = src.material;
                q.smooth = src.smooth;
                q.sel = src.sel;
                out.faces.push_back(std::move(q));
            }
        }
        for (const auto& e : m.loose) {
            const std::uint32_t mid = edgeVert(e[0], e[1]);
            out.loose.push_back({e[0], mid});
            out.loose.push_back({mid, e[1]});
        }
        for (auto pair : {std::make_pair(&m.sharp, &out.sharp), std::make_pair(&m.seams, &out.seams)}) {
            for (auto k : *pair.first) {
                auto it = t.index.find(k);
                if (it == t.index.end()) continue;
                const std::uint32_t mid = static_cast<std::uint32_t>(V + it->second);
                pair.second->insert(edgeKey(keyA(k), mid));
                pair.second->insert(edgeKey(mid, keyB(k)));
            }
        }
        flushSelection(out, SelectMode::Face);
        m = std::move(out);
    }
    return m;
}

}  // namespace wetwaresuite::model
