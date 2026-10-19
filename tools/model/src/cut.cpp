// Cutting: loop cut, bisect, knife and subdivide.
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "wetwaresuite/model/ops.hpp"

namespace wetwaresuite::model {

namespace {

// New points along an edge, ordered from keyA to keyB, with their fractions from keyA.
struct Split {
    std::vector<std::uint32_t> ids;
    std::vector<double> t;
};
using Splits = std::unordered_map<EdgeKey, Split>;

Split makeSplit(PolyMesh& m, EdgeKey k, const std::vector<double>& fractions) {
    Split s;
    const Vec3 a = m.verts[keyA(k)].p, b = m.verts[keyB(k)].p;
    const Vec3 ca = m.verts[keyA(k)].color, cb = m.verts[keyB(k)].color;
    std::vector<double> fr = fractions;
    std::sort(fr.begin(), fr.end());
    for (double f : fr) {
        s.ids.push_back(m.addVertex(mesh::lerp(a, b, f)));
        m.verts[s.ids.back()].color = mesh::lerp(ca, cb, f);
        s.t.push_back(f);
    }
    return s;
}

// Points of edge (a -> b) in that direction, with fractions from a.
void along(const Splits& splits, std::uint32_t a, std::uint32_t b, std::vector<std::uint32_t>& ids, std::vector<double>& t) {
    ids.clear();
    t.clear();
    auto it = splits.find(edgeKey(a, b));
    if (it == splits.end()) return;
    const Split& s = it->second;
    if (a < b) {
        ids = s.ids;
        t = s.t;
    } else {
        for (std::size_t i = s.ids.size(); i-- > 0;) {
            ids.push_back(s.ids[i]);
            t.push_back(1 - s.t[i]);
        }
    }
}

// Puts split points into the outlines of faces that aren't otherwise rebuilt.
void insertSplits(PolyMesh& m, const Splits& splits, const std::vector<char>& skip) {
    std::vector<std::uint32_t> ids;
    std::vector<double> t;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        if (fi < skip.size() && skip[fi]) continue;
        Face& f = m.faces[fi];
        const std::size_t n = f.v.size();
        bool hit = false;
        for (std::size_t i = 0; i < n && !hit; ++i) hit = splits.count(edgeKey(f.v[i], f.v[(i + 1) % n])) > 0;
        if (!hit) continue;
        const bool uv = f.uv.size() == n;
        Face g = f;
        g.v.clear();
        g.uv.clear();
        for (std::size_t i = 0; i < n; ++i) {
            g.v.push_back(f.v[i]);
            if (uv) g.uv.push_back(f.uv[i]);
            along(splits, f.v[i], f.v[(i + 1) % n], ids, t);
            for (std::size_t j = 0; j < ids.size(); ++j) {
                g.v.push_back(ids[j]);
                if (uv) g.uv.push_back(f.uv[i] + (f.uv[(i + 1) % n] - f.uv[i]) * t[j]);
            }
        }
        f = std::move(g);
    }
}

}  // namespace

std::size_t loopCut(PolyMesh& m, EdgeKey e, int cuts, double factor) {
    cuts = std::clamp(cuts, 1, 1000);
    Topology t(m);
    const int start = t.edge(keyA(e), keyB(e));
    if (start < 0) return 0;
    std::map<EdgeKey, std::uint32_t> ringStart;  // ring edge -> the end its cuts are measured from
    ringStart[e] = keyA(e);
    std::vector<std::uint32_t> ringFaces;
    std::vector<char> visited(m.faces.size(), 0);
    for (const auto& h0 : t.edgeFaces[static_cast<std::size_t>(start)]) {
        HalfRef h = h0;
        EdgeKey cur = e;
        while (true) {
            if (visited[h.face]) break;
            const Face& f = m.faces[h.face];
            if (f.v.size() != 4) break;
            visited[h.face] = 1;
            ringFaces.push_back(h.face);
            const std::uint32_t c = h.corner, c2 = (c + 2) % 4;
            const EdgeKey opp = edgeKey(f.v[c2], f.v[(c2 + 1) % 4]);
            const std::uint32_t s = ringStart[cur];
            const std::uint32_t os = s == f.v[c] ? f.v[(c2 + 1) % 4] : f.v[c2];
            if (ringStart.count(opp)) break;  // came all the way round
            ringStart[opp] = os;
            const auto& nf = t.edgeFaces[t.index.at(opp)];
            if (nf.size() != 2) break;
            h = nf[0].face == h.face ? nf[1] : nf[0];
            cur = opp;
        }
    }
    if (ringFaces.empty()) return 0;
    Splits splits;
    for (const auto& [k, s] : ringStart) {
        std::vector<double> fr;
        for (int j = 1; j <= cuts; ++j) {
            double f = cuts == 1 ? std::clamp(factor, 0.01, 0.99) : static_cast<double>(j) / (cuts + 1);
            fr.push_back(s == keyA(k) ? f : 1 - f);
        }
        splits[k] = makeSplit(m, k, fr);
    }
    std::vector<char> skip(m.faces.size(), 0);
    for (auto f : ringFaces) skip[f] = 1;
    insertSplits(m, splits, skip);
    std::vector<Face> made;
    std::set<EdgeKey> newLoop;
    std::vector<std::uint32_t> ids;
    std::vector<double> tt;
    for (auto fi : ringFaces) {
        const Face& f = m.faces[fi];
        int c0 = -1;
        for (int c = 0; c < 4 && c0 < 0; ++c)
            if (splits.count(edgeKey(f.v[c], f.v[(c + 1) % 4])) && splits.count(edgeKey(f.v[(c + 2) % 4], f.v[(c + 3) % 4]))) c0 = c;
        if (c0 < 0) continue;
        const std::uint32_t v0 = f.v[c0], v1 = f.v[(c0 + 1) % 4], v2 = f.v[(c0 + 2) % 4], v3 = f.v[(c0 + 3) % 4];
        const bool uv = f.uv.size() == 4;
        const Vec2 u0 = uv ? f.uv[c0] : Vec2{}, u1 = uv ? f.uv[(c0 + 1) % 4] : Vec2{}, u2 = uv ? f.uv[(c0 + 2) % 4] : Vec2{},
                   u3 = uv ? f.uv[(c0 + 3) % 4] : Vec2{};
        std::vector<std::uint32_t> X{v0}, Y{v3};
        std::vector<Vec2> XU{u0}, YU{u3};
        along(splits, v0, v1, ids, tt);
        for (std::size_t j = 0; j < ids.size(); ++j) {
            X.push_back(ids[j]);
            XU.push_back(u0 + (u1 - u0) * tt[j]);
        }
        along(splits, v3, v2, ids, tt);
        for (std::size_t j = 0; j < ids.size(); ++j) {
            Y.push_back(ids[j]);
            YU.push_back(u3 + (u2 - u3) * tt[j]);
        }
        X.push_back(v1);
        XU.push_back(u1);
        Y.push_back(v2);
        YU.push_back(u2);
        for (std::size_t j = 0; j + 1 < X.size() && j + 1 < Y.size(); ++j) {
            Face q;
            q.v = {X[j], X[j + 1], Y[j + 1], Y[j]};
            if (uv) q.uv = {XU[j], XU[j + 1], YU[j + 1], YU[j]};
            q.material = f.material;
            q.smooth = f.smooth;
            made.push_back(std::move(q));
            if (j > 0) newLoop.insert(edgeKey(X[j], Y[j]));
        }
    }
    std::vector<Face> faces;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi)
        if (!skip[fi]) faces.push_back(std::move(m.faces[fi]));
    for (auto& f : made) faces.push_back(std::move(f));
    m.faces = std::move(faces);
    for (auto& v : m.verts) v.sel = false;
    for (auto& f : m.faces) f.sel = false;
    m.esel = newLoop;
    cleanup(m);
    flushSelection(m, SelectMode::Edge);
    return newLoop.size();
}

std::size_t bisect(PolyMesh& m, const Vec3& point, const Vec3& normal, bool clearInner, bool clearOuter, bool fillCut,
                   const std::vector<char>* filter) {
    const Vec3 N = mesh::normalize(normal);
    if (mesh::length(N) == 0) return 0;
    const double eps = std::max(bounds(m).diagonal(), 1e-9) * 1e-9;
    std::vector<double> d(m.verts.size());
    for (std::size_t v = 0; v < m.verts.size(); ++v) {
        d[v] = mesh::dot(m.verts[v].p - point, N);
        if (std::fabs(d[v]) < eps) d[v] = 0;
    }
    auto inFilter = [&](std::size_t f) { return !filter || (f < filter->size() && (*filter)[f]); };
    // split crossing edges of the faces being cut
    Splits splits;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        if (!inFilter(fi)) continue;
        const auto& fv = m.faces[fi].v;
        for (std::size_t i = 0; i < fv.size(); ++i) {
            const std::uint32_t a = fv[i], b = fv[(i + 1) % fv.size()];
            if (d[a] * d[b] >= 0) continue;
            const EdgeKey k = edgeKey(a, b);
            if (splits.count(k)) continue;
            const std::uint32_t lo = keyA(k), hi = keyB(k);
            splits[k] = makeSplit(m, k, {d[lo] / (d[lo] - d[hi])});
            d.push_back(0);
        }
    }
    const std::size_t faceCount = m.faces.size();
    std::vector<char> fil(faceCount, 0);
    for (std::size_t f = 0; f < faceCount; ++f) fil[f] = inFilter(f);
    insertSplits(m, splits, {});
    // split each face along the chords between its on-plane corners
    std::set<EdgeKey> cutEdges;
    std::vector<Face> out;
    std::vector<char> outFil;
    for (std::size_t fi = 0; fi < faceCount; ++fi) {
        Face f = std::move(m.faces[fi]);
        if (!fil[fi]) {
            out.push_back(std::move(f));
            outFil.push_back(0);
            continue;
        }
        bool pos = false, neg = false;
        for (auto v : f.v) {
            pos = pos || d[v] > 0;
            neg = neg || d[v] < 0;
        }
        if (!pos || !neg) {
            out.push_back(std::move(f));
            outFil.push_back(1);
            continue;
        }
        const std::size_t n = f.v.size();
        const Vec3 fn = faceNormal(m, f);
        const Vec3 L = mesh::cross(N, fn);
        // crossing corners: on the plane, with the nearest off-plane neighbours on opposite sides
        std::vector<std::pair<double, std::uint32_t>> cross;
        for (std::size_t i = 0; i < n; ++i) {
            if (d[f.v[i]] != 0) continue;
            double before = 0, after = 0;
            for (std::size_t k = 1; k < n && before == 0; ++k) before = d[f.v[(i + n - k) % n]];
            for (std::size_t k = 1; k < n && after == 0; ++k) after = d[f.v[(i + k) % n]];
            if (before * after < 0) cross.push_back({mesh::dot(m.verts[f.v[i]].p, L), f.v[i]});
        }
        std::sort(cross.begin(), cross.end());
        std::vector<Face> pieces{std::move(f)};
        for (std::size_t c = 0; c + 1 < cross.size(); c += 2) {
            const std::uint32_t a = cross[c].second, b = cross[c + 1].second;
            for (std::size_t p = 0; p < pieces.size(); ++p) {
                Face& g = pieces[p];
                auto ia = std::find(g.v.begin(), g.v.end(), a), ib = std::find(g.v.begin(), g.v.end(), b);
                if (ia == g.v.end() || ib == g.v.end()) continue;
                std::size_t i = static_cast<std::size_t>(ia - g.v.begin()), j = static_cast<std::size_t>(ib - g.v.begin());
                if (i > j) std::swap(i, j);
                if (j - i < 2 && !(i == 0 && j == g.v.size() - 1)) break;  // already neighbours
                if (i == 0 && j == g.v.size() - 1) break;
                const bool uv = g.uv.size() == g.v.size();
                Face h1 = g, h2 = g;
                h1.v.assign(g.v.begin() + static_cast<std::ptrdiff_t>(i), g.v.begin() + static_cast<std::ptrdiff_t>(j) + 1);
                h2.v.assign(g.v.begin() + static_cast<std::ptrdiff_t>(j), g.v.end());
                h2.v.insert(h2.v.end(), g.v.begin(), g.v.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                if (uv) {
                    h1.uv.assign(g.uv.begin() + static_cast<std::ptrdiff_t>(i), g.uv.begin() + static_cast<std::ptrdiff_t>(j) + 1);
                    h2.uv.assign(g.uv.begin() + static_cast<std::ptrdiff_t>(j), g.uv.end());
                    h2.uv.insert(h2.uv.end(), g.uv.begin(), g.uv.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                }
                pieces[p] = std::move(h1);
                pieces.push_back(std::move(h2));
                cutEdges.insert(edgeKey(a, b));
                break;
            }
        }
        for (auto& p : pieces) {
            out.push_back(std::move(p));
            outFil.push_back(1);
        }
    }
    m.faces = std::move(out);
    // clear a side
    if (clearInner || clearOuter) {
        std::vector<char> kill(m.faces.size(), 0);
        for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
            if (!outFil[fi]) continue;
            double s = 0;
            for (auto v : m.faces[fi].v) s += d[v];
            if ((clearInner && s < 0) || (clearOuter && s > 0)) kill[fi] = 1;
        }
        std::vector<Face> keep;
        for (std::size_t fi = 0; fi < m.faces.size(); ++fi)
            if (!kill[fi]) keep.push_back(std::move(m.faces[fi]));
        m.faces = std::move(keep);
        std::vector<std::array<std::uint32_t, 2>> loose;
        for (const auto& e : m.loose) {
            const double s = d[e[0]] + d[e[1]];
            if (!((clearInner && s < 0) || (clearOuter && s > 0))) loose.push_back(e);
        }
        m.loose = std::move(loose);
        if (fillCut) {
            // cap boundary loops that lie in the plane
            std::set<std::pair<std::uint32_t, std::uint32_t>> halfs;
            for (const auto& f : m.faces)
                for (std::size_t i = 0; i < f.v.size(); ++i) halfs.insert({f.v[i], f.v[(i + 1) % f.v.size()]});
            std::multimap<std::uint32_t, std::uint32_t> open;
            for (const auto& [a, b] : halfs)
                if (!halfs.count({b, a}) && d[a] == 0 && d[b] == 0) open.emplace(a, b);
            std::set<std::uint32_t> used;
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
                std::reverse(loop.begin(), loop.end());
                m.addFace(loop);
            }
        }
    }
    for (auto& v : m.verts) v.sel = false;
    for (auto& f : m.faces) f.sel = false;
    m.esel = cutEdges;
    cleanup(m);
    flushSelection(m, SelectMode::Edge);
    return m.esel.size();
}

std::size_t knife(PolyMesh& m, const Vec3& a, const Vec3& b, const Vec3& view, bool through, bool selectedOnly) {
    const Vec3 ab = b - a;
    const Vec3 N = mesh::normalize(mesh::cross(ab, view));
    if (mesh::length(N) == 0 || mesh::dot(ab, ab) == 0) return 0;
    const Vec3 dir = ab / mesh::dot(ab, ab);
    std::vector<char> filter(m.faces.size(), 0);
    std::size_t any = 0;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        if (selectedOnly && !f.sel) continue;
        if (!through && mesh::dot(faceNormal(m, f), view) >= 0) continue;
        // where the plane crosses the outline, as positions along a->b
        double lo = 1e300, hi = -1e300;
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Vec3 p = m.verts[f.v[i]].p, q = m.verts[f.v[(i + 1) % n]].p;
            const double dp = mesh::dot(p - a, N), dq = mesh::dot(q - a, N);
            if (dp * dq > 0 || (dp == 0 && dq == 0)) continue;
            const Vec3 x = dp == dq ? p : p + (q - p) * (dp / (dp - dq));
            const double s = mesh::dot(x - a, dir);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        if (hi < 0 || lo > 1) continue;
        filter[fi] = 1;
        ++any;
    }
    if (!any) return 0;
    return bisect(m, a, N, false, false, false, &filter);
}

std::size_t subdivide(PolyMesh& m, int cuts) {
    cuts = std::clamp(cuts, 1, 100);
    std::vector<char> sel(m.faces.size(), 0);
    bool any = false;
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        sel[f] = m.faces[f].sel;
        any = any || sel[f];
    }
    if (!any) return 0;
    Splits splits;
    std::vector<double> fr;
    for (int j = 1; j <= cuts; ++j) fr.push_back(static_cast<double>(j) / (cuts + 1));
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        if (!sel[fi]) continue;
        const auto& fv = m.faces[fi].v;
        for (std::size_t i = 0; i < fv.size(); ++i) {
            const EdgeKey k = edgeKey(fv[i], fv[(i + 1) % fv.size()]);
            if (!splits.count(k)) splits[k] = makeSplit(m, k, fr);
        }
    }
    insertSplits(m, splits, sel);
    const int K = cuts + 1;
    std::vector<Face> made;
    std::vector<std::uint32_t> ids;
    std::vector<double> tt;
    // the path of an edge a -> b including both ends, and its uvs
    auto path = [&](std::uint32_t a, std::uint32_t b, const Vec2& ua, const Vec2& ub, std::vector<std::uint32_t>& p, std::vector<Vec2>& pu) {
        along(splits, a, b, ids, tt);
        p = {a};
        pu = {ua};
        for (std::size_t j = 0; j < ids.size(); ++j) {
            p.push_back(ids[j]);
            pu.push_back(ua + (ub - ua) * tt[j]);
        }
        p.push_back(b);
        pu.push_back(ub);
    };
    auto newPoint = [&](const Vec3& p, const Vec3& c) {
        const std::uint32_t id = m.addVertex(p);
        m.verts[id].color = c;
        return id;
    };
    for (std::size_t fi = 0; fi < sel.size(); ++fi) {
        if (!sel[fi]) continue;
        const Face f = m.faces[fi];
        const std::size_t n = f.v.size();
        const bool uv = f.uv.size() == n;
        std::vector<Vec2> cu = uv ? f.uv : std::vector<Vec2>(n);
        auto emit = [&](std::vector<std::uint32_t> v, std::vector<Vec2> u) {
            Face g;
            g.v = std::move(v);
            if (uv) g.uv = std::move(u);
            g.material = f.material;
            g.smooth = f.smooth;
            g.sel = true;
            made.push_back(std::move(g));
        };
        if (n == 4) {
            std::vector<std::vector<std::uint32_t>> G(static_cast<std::size_t>(K) + 1, std::vector<std::uint32_t>(static_cast<std::size_t>(K) + 1));
            std::vector<std::vector<Vec2>> GU(static_cast<std::size_t>(K) + 1, std::vector<Vec2>(static_cast<std::size_t>(K) + 1));
            std::vector<std::uint32_t> p;
            std::vector<Vec2> pu;
            path(f.v[0], f.v[1], cu[0], cu[1], p, pu);
            for (int i = 0; i <= K; ++i) G[i][0] = p[i], GU[i][0] = pu[i];
            path(f.v[1], f.v[2], cu[1], cu[2], p, pu);
            for (int j = 0; j <= K; ++j) G[K][j] = p[j], GU[K][j] = pu[j];
            path(f.v[3], f.v[2], cu[3], cu[2], p, pu);
            for (int i = 0; i <= K; ++i) G[i][K] = p[i], GU[i][K] = pu[i];
            path(f.v[0], f.v[3], cu[0], cu[3], p, pu);
            for (int j = 0; j <= K; ++j) G[0][j] = p[j], GU[0][j] = pu[j];
            const Vec3 P0 = m.verts[f.v[0]].p, P1 = m.verts[f.v[1]].p, P2 = m.verts[f.v[2]].p, P3 = m.verts[f.v[3]].p;
            const Vec3 C0 = m.verts[f.v[0]].color, C1 = m.verts[f.v[1]].color, C2 = m.verts[f.v[2]].color, C3 = m.verts[f.v[3]].color;
            for (int i = 1; i < K; ++i)
                for (int j = 1; j < K; ++j) {
                    const double s = static_cast<double>(i) / K, t = static_cast<double>(j) / K;
                    auto bil = [&](auto a, auto b, auto c, auto d) { return a * ((1 - s) * (1 - t)) + b * (s * (1 - t)) + c * (s * t) + d * ((1 - s) * t); };
                    G[i][j] = newPoint(bil(P0, P1, P2, P3), bil(C0, C1, C2, C3));
                    GU[i][j] = bil(cu[0], cu[1], cu[2], cu[3]);
                }
            for (int i = 0; i < K; ++i)
                for (int j = 0; j < K; ++j)
                    emit({G[i][j], G[i + 1][j], G[i + 1][j + 1], G[i][j + 1]}, {GU[i][j], GU[i + 1][j], GU[i + 1][j + 1], GU[i][j + 1]});
        } else if (n == 3) {
            // T[i][j], i + j <= K: a + (b - a) i/K + (c - a) j/K
            std::map<std::pair<int, int>, std::pair<std::uint32_t, Vec2>> T;
            std::vector<std::uint32_t> p;
            std::vector<Vec2> pu;
            path(f.v[0], f.v[1], cu[0], cu[1], p, pu);
            for (int i = 0; i <= K; ++i) T[{i, 0}] = {p[i], pu[i]};
            path(f.v[0], f.v[2], cu[0], cu[2], p, pu);
            for (int j = 0; j <= K; ++j) T[{0, j}] = {p[j], pu[j]};
            path(f.v[1], f.v[2], cu[1], cu[2], p, pu);
            for (int j = 0; j <= K; ++j) T[{K - j, j}] = {p[j], pu[j]};
            const Vec3 A = m.verts[f.v[0]].p, B = m.verts[f.v[1]].p, C = m.verts[f.v[2]].p;
            const Vec3 CA = m.verts[f.v[0]].color, CB = m.verts[f.v[1]].color, CC = m.verts[f.v[2]].color;
            for (int i = 1; i < K; ++i)
                for (int j = 1; i + j < K; ++j) {
                    const double s = static_cast<double>(i) / K, t = static_cast<double>(j) / K;
                    T[{i, j}] = {newPoint(A + (B - A) * s + (C - A) * t, CA + (CB - CA) * s + (CC - CA) * t), cu[0] + (cu[1] - cu[0]) * s + (cu[2] - cu[0]) * t};
                }
            for (int i = 0; i < K; ++i)
                for (int j = 0; i + j < K; ++j) {
                    const auto &a = T[{i, j}], &b = T[{i + 1, j}], &c = T[{i, j + 1}];
                    emit({a.first, b.first, c.first}, {a.second, b.second, c.second});
                    if (i + j + 2 <= K) {
                        const auto& d2 = T[{i + 1, j + 1}];
                        emit({b.first, d2.first, c.first}, {b.second, d2.second, c.second});
                    }
                }
        } else {
            Vec3 cp, cc;
            Vec2 cuv;
            for (std::size_t i = 0; i < n; ++i) {
                cp += m.verts[f.v[i]].p;
                cc += m.verts[f.v[i]].color;
                cuv += cu[i];
            }
            cp = cp / static_cast<double>(n);
            const std::uint32_t center = newPoint(cp, cc / static_cast<double>(n));
            cuv = cuv / static_cast<double>(n);
            std::vector<std::vector<std::uint32_t>> P(n);
            std::vector<std::vector<Vec2>> PU(n);
            for (std::size_t k = 0; k < n; ++k) path(f.v[k], f.v[(k + 1) % n], cu[k], cu[(k + 1) % n], P[k], PU[k]);
            const std::size_t mid = static_cast<std::size_t>((cuts + 1) / 2);
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t pk = (k + n - 1) % n;
                std::vector<std::uint32_t> v;
                std::vector<Vec2> u;
                for (std::size_t j = mid; j < P[pk].size(); ++j) v.push_back(P[pk][j]), u.push_back(PU[pk][j]);
                for (std::size_t j = 1; j <= mid; ++j) v.push_back(P[k][j]), u.push_back(PU[k][j]);
                v.push_back(center);
                u.push_back(cuv);
                emit(std::move(v), std::move(u));
            }
        }
    }
    std::vector<Face> faces;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi)
        if (fi >= sel.size() || !sel[fi]) {
            m.faces[fi].sel = false;
            faces.push_back(std::move(m.faces[fi]));
        }
    const std::size_t count = made.size();
    for (auto& f : made) faces.push_back(std::move(f));
    m.faces = std::move(faces);
    cleanup(m);
    flushSelection(m, SelectMode::Face);
    return count;
}

}  // namespace wetwaresuite::model
