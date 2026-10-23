// Bevel: selected edges become strips of `segments` faces with a rounded
// profile; selected vertices get their corner cut off.
//
// Every face corner at a bevelled vertex v is replaced by new points:
//   both edges at the corner bevelled   -> one miter point inside the face
//   one edge bevelled                   -> a point slid along the other edge
//   neither (but v is bevelled)         -> the points slid along its two edges
//                                          (or v itself where nothing slid)
// Points slid along the same edge are shared by the faces on both sides, so
// the surface stays closed. Each bevelled edge then gets a strip between the
// points the faces on its two sides left behind, and any hole left around a
// vertex (where three or more bevelled edges meet) is capped with an n-gon.
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "wetwaresuite/model/ops.hpp"

namespace wetwaresuite::model {

namespace {

struct Slide {
    double t = 0;  // summed distance along the edge
    int n = 0;
    std::uint32_t id = UINT32_MAX;
};

// UV of point x inside a face corner (p, v, n), from the corner's own UVs.
Vec2 cornerUv(const Vec3& x, const Vec3& p, const Vec3& v, const Vec3& n, const Vec2& up, const Vec2& uv, const Vec2& un) {
    const Vec3 a = p - v, b = n - v, d = x - v;
    const double aa = mesh::dot(a, a), ab = mesh::dot(a, b), bb = mesh::dot(b, b);
    const double det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-20) return uv;
    const double da = mesh::dot(d, a), db = mesh::dot(d, b);
    const double al = (da * bb - db * ab) / det, be = (db * aa - da * ab) / det;
    return uv + (up - uv) * al + (un - uv) * be;
}

}  // namespace

std::size_t bevel(PolyMesh& m, double width, int segments, bool vertexOnly) {
    if (!(width > 0)) return 0;
    segments = std::clamp(segments, 1, 64);
    if (vertexOnly) segments = 1;
    Topology t(m);
    std::set<EdgeKey> bev;
    std::vector<char> bv(m.verts.size(), 0);
    if (vertexOnly) {
        for (std::uint32_t v = 0; v < m.verts.size(); ++v)
            if (m.verts[v].sel && !t.vertFaces[v].empty()) bv[v] = 1;
    } else {
        for (auto k : m.esel) {
            auto it = t.index.find(k);
            if (it == t.index.end()) continue;
            const auto& ef = t.edgeFaces[it->second];
            if (ef.size() != 2) continue;
            if (m.faces[ef[0].face].v[ef[0].corner] == m.faces[ef[1].face].v[ef[1].corner]) continue;  // flipped neighbours
            bev.insert(k);
            bv[keyA(k)] = bv[keyB(k)] = 1;
        }
    }
    bool any = false;
    for (char c : bv) any = any || c;
    if (!any) return 0;

    const std::size_t oldVerts = m.verts.size();
    std::vector<std::uint32_t> tag;  // new vertex -> the vertex it was split from
    auto newVertex = [&](const Vec3& p, std::uint32_t from) {
        const std::uint32_t id = m.addVertex(p);
        m.verts[id].color = m.verts[from].color;
        tag.push_back(from);
        return id;
    };
    auto tagOf = [&](std::uint32_t v) -> std::uint32_t { return v < oldVerts ? v : tag[v - oldVerts]; };
    auto P = [&](std::uint32_t v) { return m.verts[v].p; };

    // pass 1: how far each slid point goes along its edge, and miter points
    std::map<std::pair<std::uint32_t, std::uint32_t>, Slide> slides;  // (v, w): point on edge v-w near v
    struct CornerPlan {
        int kind = 0;  // 0 neither, 1 miter, 2 in bevelled (slide on out edge), 3 out bevelled (slide on in edge), 4 vertex bevel
        std::uint32_t miter = UINT32_MAX;
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, CornerPlan> plan;  // (face, corner)
    auto addSlide = [&](std::uint32_t v, std::uint32_t w, double dist) {
        Slide& s = slides[{v, w}];
        s.t += dist;
        s.n++;
    };
    auto sinBetween = [](const Vec3& a, const Vec3& b) {
        return std::max(mesh::length(mesh::cross(mesh::normalize(a), mesh::normalize(b))), 0.05);
    };
    for (std::uint32_t fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        const std::size_t n = f.v.size();
        const Vec3 nrm = faceNormal(m, f);
        for (std::uint32_t c = 0; c < n; ++c) {
            const std::uint32_t v = f.v[c];
            if (!bv[v]) continue;
            const std::uint32_t p = f.v[(c + n - 1) % n], q = f.v[(c + 1) % n];
            CornerPlan cp;
            if (vertexOnly) {
                cp.kind = 4;
                addSlide(v, p, width);
                addSlide(v, q, width);
            } else {
                const bool inB = bev.count(edgeKey(p, v)) > 0, outB = bev.count(edgeKey(v, q)) > 0;
                if (inB && outB) {
                    cp.kind = 1;
                    const Vec3 in1 = mesh::normalize(mesh::cross(nrm, P(v) - P(p)));
                    const Vec3 in2 = mesh::normalize(mesh::cross(nrm, P(q) - P(v)));
                    Vec3 off = (in1 + in2) * (width / std::max(1 + mesh::dot(in1, in2), 0.05));
                    const double lim = 0.5 * mesh::length(faceCenter(m, f) - P(v));
                    if (mesh::length(off) > lim && mesh::length(off) > 0) off = mesh::normalize(off) * lim;
                    cp.miter = newVertex(P(v) + off, v);
                } else if (inB) {
                    cp.kind = 2;
                    addSlide(v, q, width / sinBetween(P(q) - P(v), P(v) - P(p)));
                } else if (outB) {
                    cp.kind = 3;
                    addSlide(v, p, width / sinBetween(P(p) - P(v), P(q) - P(v)));
                }
            }
            plan[{fi, c}] = cp;
        }
    }
    // pass 2: place the slid points
    for (auto& [key, s] : slides) {
        const std::uint32_t v = key.first, w = key.second;
        const Vec3 d = P(w) - P(v);
        const double len = mesh::length(d);
        double dist = s.t / s.n;
        dist = std::min(dist, len * (bv[w] ? 0.49 : 0.98));
        s.t = len > 0 ? dist / len : 0;  // now the fraction along the edge
        s.id = newVertex(P(v) + d * s.t, v);
    }
    auto slideId = [&](std::uint32_t v, std::uint32_t w) -> std::uint32_t {
        auto it = slides.find({v, w});
        return it == slides.end() ? UINT32_MAX : it->second.id;
    };

    // each corner's replacement points (in the face's winding order)
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> seq;
    for (const auto& [key, cp] : plan) {
        const Face& f = m.faces[key.first];
        const std::size_t n = f.v.size();
        const std::uint32_t c = key.second, v = f.v[c];
        const std::uint32_t p = f.v[(c + n - 1) % n], q = f.v[(c + 1) % n];
        std::vector<std::uint32_t> s;
        switch (cp.kind) {
            case 1: s = {cp.miter}; break;
            case 2: s = {slideId(v, q)}; break;
            case 3: s = {slideId(v, p)}; break;
            default: {
                const std::uint32_t a = slideId(v, p), b = slideId(v, q);
                s.push_back(a == UINT32_MAX ? v : a);
                const std::uint32_t second = b == UINT32_MAX ? v : b;
                if (second != s.back()) s.push_back(second);
            }
        }
        seq[key] = std::move(s);
    }

    // strips along bevelled edges, with shared rounded profiles at their ends
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> profiles;
    auto profile = [&](std::uint32_t v, std::uint32_t a, std::uint32_t b) {
        if (a == b) return std::vector<std::uint32_t>(static_cast<std::size_t>(segments) + 1, a);
        const auto key = std::make_tuple(v, std::min(a, b), std::max(a, b));
        auto it = profiles.find(key);
        if (it == profiles.end()) {
            std::vector<std::uint32_t> pts{std::min(a, b)};
            const std::uint32_t lo = std::min(a, b), hi = std::max(a, b);
            const Vec3 C = P(lo) + P(hi) - P(v);
            for (int j = 1; j < segments; ++j) {
                const double phi = (mesh::kPi / 2) * j / segments;
                pts.push_back(newVertex(C + (P(lo) - C) * std::cos(phi) + (P(hi) - C) * std::sin(phi), v));
            }
            pts.push_back(hi);
            it = profiles.emplace(key, std::move(pts)).first;
        }
        std::vector<std::uint32_t> pts = it->second;
        if (pts.front() != a) std::reverse(pts.begin(), pts.end());
        return pts;
    };
    auto cornerOf = [&](std::uint32_t face, std::uint32_t v) {
        const auto& fv = m.faces[face].v;
        return static_cast<std::uint32_t>(std::find(fv.begin(), fv.end(), v) - fv.begin());
    };
    std::vector<Face> made;
    for (auto k : bev) {
        const auto& ef = t.edgeFaces[t.index.at(k)];
        const Face& f0 = m.faces[ef[0].face];
        const std::uint32_t a = f0.v[ef[0].corner], b = f0.v[(ef[0].corner + 1) % f0.v.size()];  // f0 walks a -> b
        const std::uint32_t fl = ef[0].face, fr = ef[1].face;
        const std::uint32_t aL = seq[{fl, cornerOf(fl, a)}].back(), bL = seq[{fl, cornerOf(fl, b)}].front();
        const std::uint32_t bR = seq[{fr, cornerOf(fr, b)}].back(), aR = seq[{fr, cornerOf(fr, a)}].front();
        const auto pa = profile(a, aL, aR), pb = profile(b, bL, bR);
        for (int j = 0; j < segments; ++j) {
            Face s;
            s.v = {pa[static_cast<std::size_t>(j)], pa[static_cast<std::size_t>(j) + 1], pb[static_cast<std::size_t>(j) + 1], pb[static_cast<std::size_t>(j)]};
            s.material = f0.material;
            s.smooth = f0.smooth || segments > 1;
            s.sel = true;
            made.push_back(std::move(s));
        }
    }

    // rebuild the faces around bevelled vertices
    for (std::uint32_t fi = 0; fi < m.faces.size(); ++fi) {
        Face& f = m.faces[fi];
        const std::size_t n = f.v.size();
        bool touched = false;
        for (auto v : f.v) touched = touched || bv[v];
        if (!touched) continue;
        const bool uv = f.uv.size() == n;
        Face g = f;
        g.v.clear();
        g.uv.clear();
        for (std::uint32_t c = 0; c < n; ++c) {
            const std::uint32_t v = f.v[c];
            const std::uint32_t pi = (c + static_cast<std::uint32_t>(n) - 1) % static_cast<std::uint32_t>(n), qi = (c + 1) % static_cast<std::uint32_t>(n);
            auto push = [&](std::uint32_t id) {
                g.v.push_back(id);
                if (uv) g.uv.push_back(cornerUv(P(id), P(f.v[pi]), P(v), P(f.v[qi]), f.uv[pi], f.uv[c], f.uv[qi]));
            };
            if (!bv[v]) {
                g.v.push_back(v);
                if (uv) g.uv.push_back(f.uv[c]);
                continue;
            }
            const auto& s = seq[{fi, c}];
            if (s.size() == 2 && segments > 1) {
                // a face beside a single bevelled edge takes the rounded profile into its outline
                const auto key = std::make_tuple(v, std::min(s[0], s[1]), std::max(s[0], s[1]));
                auto it = profiles.find(key);
                if (it != profiles.end()) {
                    std::vector<std::uint32_t> pts = it->second;
                    if (pts.front() != s[0]) std::reverse(pts.begin(), pts.end());
                    for (auto id : pts) push(id);
                    continue;
                }
            }
            for (auto id : s) push(id);
        }
        f = std::move(g);
    }
    std::size_t count = made.size();
    for (auto& f : m.faces) f.sel = false;
    for (auto& f : made) m.faces.push_back(std::move(f));

    // cap the holes left around vertices: boundary half-edges between points split from the same vertex
    std::set<std::pair<std::uint32_t, std::uint32_t>> halfs;
    for (const auto& f : m.faces)
        for (std::size_t i = 0; i < f.v.size(); ++i) halfs.insert({f.v[i], f.v[(i + 1) % f.v.size()]});
    std::map<std::uint32_t, std::multimap<std::uint32_t, std::uint32_t>> open;  // group -> a -> b
    for (const auto& [a, b] : halfs) {
        if (halfs.count({b, a})) continue;
        const bool na = a >= oldVerts || bv[a], nb = b >= oldVerts || bv[b];
        if (!na || !nb) continue;
        const std::uint32_t ta = tagOf(a), tb = tagOf(b);
        if (ta != tb || !bv[ta]) continue;
        open[ta].emplace(a, b);
    }
    for (auto& [v, edges] : open) {
        bool ambiguous = false;
        for (auto it = edges.begin(); it != edges.end(); ++it)
            if (edges.count(it->first) > 1) ambiguous = true;
        if (ambiguous) continue;
        std::set<std::uint32_t> used;
        for (const auto& [a0, b0] : edges) {
            if (used.count(a0)) continue;
            std::vector<std::uint32_t> loop{a0};
            std::uint32_t cur = b0;
            bool closed = false;
            for (std::size_t guard = 0; guard <= edges.size(); ++guard) {
                if (cur == a0) {
                    closed = true;
                    break;
                }
                loop.push_back(cur);
                auto it = edges.find(cur);
                if (it == edges.end()) break;
                cur = it->second;
            }
            for (auto x : loop) used.insert(x);
            if (!closed || loop.size() < 3) continue;
            std::reverse(loop.begin(), loop.end());
            Face cap;
            cap.v = loop;
            cap.sel = true;
            cap.smooth = segments > 1;
            if (!t.vertFaces[v].empty()) cap.material = m.faces[t.vertFaces[v][0]].material;
            m.faces.push_back(std::move(cap));
            ++count;
        }
    }
    cleanup(m);
    flushSelection(m, SelectMode::Face);
    return count;
}

}  // namespace wetwaresuite::model
