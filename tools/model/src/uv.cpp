// UV mapping: projections, smart projection, least-squares conformal
// unwrapping along seams, island packing and a picture of the layout.
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

#include "wetwaresuite/model/ops.hpp"

namespace wetwaresuite::model {

std::optional<UvMethod> parseUvMethod(const std::string& s) {
    if (s == "smart" || s == "smart_project") return UvMethod::Smart;
    if (s == "cube") return UvMethod::Cube;
    if (s == "cylinder") return UvMethod::Cylinder;
    if (s == "sphere") return UvMethod::Sphere;
    if (s == "planar" || s == "project") return UvMethod::Planar;
    if (s == "unwrap" || s == "seams" || s == "lscm") return UvMethod::Unwrap;
    return std::nullopt;
}

namespace {

constexpr double kTau = 2 * mesh::kPi;

void basis(const Vec3& n, Vec3& u, Vec3& w) {
    const Vec3 up = std::fabs(n.z) < 0.99 ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    u = mesh::normalize(mesh::cross(up, n));
    w = mesh::cross(n, u);
}

std::vector<char> eligibleFaces(const PolyMesh& m, bool selectedOnly) {
    std::vector<char> e(m.faces.size(), 1);
    if (selectedOnly)
        for (std::size_t f = 0; f < m.faces.size(); ++f) e[f] = m.faces[f].sel;
    return e;
}

// Faces connected through edges that aren't seams (and, with `same`, share a label).
std::vector<std::vector<std::uint32_t>> islands(const PolyMesh& m, const std::vector<char>& eligible, bool useSeams,
                                                const std::vector<int>* label = nullptr) {
    Topology t(m);
    std::vector<int> id(m.faces.size(), -1);
    std::vector<std::vector<std::uint32_t>> out;
    for (std::uint32_t f0 = 0; f0 < m.faces.size(); ++f0) {
        if (!eligible[f0] || id[f0] >= 0) continue;
        out.emplace_back();
        std::vector<std::uint32_t> stack{f0};
        id[f0] = static_cast<int>(out.size() - 1);
        while (!stack.empty()) {
            const std::uint32_t f = stack.back();
            stack.pop_back();
            out.back().push_back(f);
            const auto& fv = m.faces[f].v;
            for (std::size_t i = 0; i < fv.size(); ++i) {
                const EdgeKey k = edgeKey(fv[i], fv[(i + 1) % fv.size()]);
                if (useSeams && m.seams.count(k)) continue;
                for (const auto& h : t.edgeFaces[t.index.at(k)]) {
                    if (id[h.face] >= 0 || !eligible[h.face]) continue;
                    if (label && (*label)[h.face] != (*label)[f]) continue;
                    id[h.face] = id[f];
                    stack.push_back(h.face);
                }
            }
        }
    }
    return out;
}

void projectIsland(PolyMesh& m, const std::vector<std::uint32_t>& faces, const Vec3& n, double scale) {
    Vec3 u, w;
    basis(mesh::normalize(n), u, w);
    for (auto f : faces) {
        Face& face = m.faces[f];
        face.uv.resize(face.v.size());
        for (std::size_t c = 0; c < face.v.size(); ++c) {
            const Vec3& p = m.verts[face.v[c]].p;
            face.uv[c] = {mesh::dot(p, u) * scale, mesh::dot(p, w) * scale};
        }
    }
}

// Wraps a face's u values so it doesn't straddle the 0/1 seam.
void unwrapU(Face& f) {
    double lo = 1e9, hi = -1e9;
    for (const auto& uv : f.uv) lo = std::min(lo, uv.x), hi = std::max(hi, uv.x);
    if (hi - lo > 0.5)
        for (auto& uv : f.uv)
            if (uv.x < 0.5) uv.x += 1;
}

// ---- least-squares conformal maps ----

struct SparseRow {
    std::vector<std::pair<std::uint32_t, double>> c;  // unknown index, coefficient
    double rhs = 0;
};

// Conjugate gradients on the normal equations A^T A x = A^T b.
void solveLeastSquares(const std::vector<SparseRow>& rows, std::vector<double>& x, int maxIter) {
    const std::size_t n = x.size();
    auto applyA = [&](const std::vector<double>& v, std::vector<double>& out) {
        out.assign(rows.size(), 0);
        for (std::size_t r = 0; r < rows.size(); ++r)
            for (const auto& [i, a] : rows[r].c) out[r] += a * v[i];
    };
    auto applyAt = [&](const std::vector<double>& v, std::vector<double>& out) {
        out.assign(n, 0);
        for (std::size_t r = 0; r < rows.size(); ++r)
            for (const auto& [i, a] : rows[r].c) out[i] += a * v[r];
    };
    std::vector<double> Ax, res(rows.size()), g, p, Ap, AtAp;
    applyA(x, Ax);
    for (std::size_t r = 0; r < rows.size(); ++r) res[r] = rows[r].rhs - Ax[r];
    applyAt(res, g);
    p = g;
    double gg = std::inner_product(g.begin(), g.end(), g.begin(), 0.0);
    const double g0 = gg;
    for (int it = 0; it < maxIter && gg > 1e-24 && gg > g0 * 1e-20; ++it) {
        applyA(p, Ap);
        const double pAp = std::inner_product(Ap.begin(), Ap.end(), Ap.begin(), 0.0);
        if (pAp <= 0) break;
        const double alpha = gg / pAp;
        for (std::size_t i = 0; i < n; ++i) x[i] += alpha * p[i];
        for (std::size_t r = 0; r < rows.size(); ++r) res[r] -= alpha * Ap[r];
        applyAt(res, g);
        const double gg2 = std::inner_product(g.begin(), g.end(), g.begin(), 0.0);
        const double beta = gg2 / gg;
        for (std::size_t i = 0; i < n; ++i) p[i] = g[i] + beta * p[i];
        gg = gg2;
    }
}

// Flattens one island cut out by seams. False when it can't (no boundary).
bool lscm(PolyMesh& m, const Topology& t, const std::vector<std::uint32_t>& faces) {
    std::set<std::uint32_t> in(faces.begin(), faces.end());
    // corner -> unknown: a vertex splits where seams run through it inside the island
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> wedge;  // (face, corner) -> unknown
    std::uint32_t unknowns = 0;
    bool hasBoundary = false;
    std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> cornersAt;  // vertex -> (face, corner)
    for (auto f : faces)
        for (std::uint32_t c = 0; c < m.faces[f].v.size(); ++c) cornersAt[m.faces[f].v[c]].push_back({f, c});
    for (auto f : faces) {
        const auto& fv = m.faces[f].v;
        for (std::size_t i = 0; i < fv.size(); ++i) {
            const EdgeKey k = edgeKey(fv[i], fv[(i + 1) % fv.size()]);
            const auto& ef = t.edgeFaces[t.index.at(k)];
            std::size_t inside = 0;
            for (const auto& h : ef) inside += in.count(h.face);
            if (inside < 2 || m.seams.count(k)) hasBoundary = true;
        }
    }
    if (!hasBoundary) return false;
    for (const auto& [v, cs] : cornersAt) {
        // union corners of faces that share a non-seam edge through v
        std::vector<std::size_t> parent(cs.size());
        std::iota(parent.begin(), parent.end(), 0);
        auto find = [&](std::size_t x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        for (std::size_t a = 0; a < cs.size(); ++a)
            for (std::size_t b = a + 1; b < cs.size(); ++b) {
                const auto& fa = m.faces[cs[a].first].v;
                const auto& fb = m.faces[cs[b].first].v;
                const std::size_t na = fa.size();
                for (std::uint32_t w : {fa[(cs[a].second + 1) % na], fa[(cs[a].second + na - 1) % na]}) {
                    if (std::find(fb.begin(), fb.end(), w) == fb.end() || m.seams.count(edgeKey(v, w))) continue;
                    parent[find(a)] = find(b);
                }
            }
        std::map<std::size_t, std::uint32_t> ids;
        for (std::size_t a = 0; a < cs.size(); ++a) {
            const std::size_t r = find(a);
            auto it = ids.find(r);
            if (it == ids.end()) it = ids.emplace(r, unknowns++).first;
            wedge[cs[a]] = it->second;
        }
    }
    // start from a planar projection (also the fallback) and pin its two farthest points
    Vec3 n;
    for (auto f : faces) n += faceNormal(m, m.faces[f]) * faceArea(m, m.faces[f]);
    n = mesh::normalize(n);
    if (mesh::length(n) == 0) n = {0, 0, 1};
    Vec3 bu, bw;
    basis(n, bu, bw);
    std::vector<Vec2> uv(unknowns);
    std::vector<Vec3> pos(unknowns);
    for (const auto& [fc, id] : wedge) {
        pos[id] = m.verts[m.faces[fc.first].v[fc.second]].p;
        uv[id] = {mesh::dot(pos[id], bu), mesh::dot(pos[id], bw)};
    }
    std::uint32_t pinA = 0, pinB = 0;
    {
        double best = -1;
        // farthest pair along the main direction of the projection
        std::uint32_t lo = 0, hi = 0;
        for (std::uint32_t i = 0; i < unknowns; ++i) {
            if (uv[i].x < uv[lo].x) lo = i;
            if (uv[i].x > uv[hi].x) hi = i;
        }
        std::uint32_t lo2 = 0, hi2 = 0;
        for (std::uint32_t i = 0; i < unknowns; ++i) {
            if (uv[i].y < uv[lo2].y) lo2 = i;
            if (uv[i].y > uv[hi2].y) hi2 = i;
        }
        for (auto [a, b] : {std::make_pair(lo, hi), std::make_pair(lo2, hi2)}) {
            const double d = mesh::length(pos[a] - pos[b]);
            if (d > best) best = d, pinA = a, pinB = b;
        }
    }
    if (pinA == pinB) return false;
    // pinned points keep their 3D distance so the island keeps its real size
    const double L = mesh::length(pos[pinB] - pos[pinA]);
    const Vec2 d0 = uv[pinB] - uv[pinA];
    const double dl = std::max(std::hypot(d0.x, d0.y), 1e-12);
    const Vec2 pA = uv[pinA], pB = uv[pinA] + d0 * (L / dl);
    // unknown index: 2 per corner unknown, pins removed
    std::vector<int> col(unknowns, -1);
    std::uint32_t free = 0;
    for (std::uint32_t i = 0; i < unknowns; ++i)
        if (i != pinA && i != pinB) col[i] = static_cast<int>(free++);
    std::vector<SparseRow> rows;
    for (auto f : faces) {
        const Face& face = m.faces[f];
        const auto tri = triangulateFace(m, face);
        for (std::size_t k = 0; k + 2 < tri.size(); k += 3) {
            std::uint32_t id[3];
            Vec3 P[3];
            for (int j = 0; j < 3; ++j) {
                id[j] = wedge.at({f, tri[k + static_cast<std::size_t>(j)]});
                P[j] = m.verts[face.v[tri[k + static_cast<std::size_t>(j)]]].p;
            }
            // the triangle in its own plane
            const Vec3 e1 = P[1] - P[0], e2 = P[2] - P[0];
            const double l1 = mesh::length(e1);
            if (l1 < 1e-14) continue;
            const Vec3 x = e1 / l1;
            const Vec3 nn = mesh::normalize(mesh::cross(e1, e2));
            if (mesh::length(nn) == 0) continue;
            const Vec3 y = mesh::cross(nn, x);
            const Vec2 q[3] = {{0, 0}, {l1, 0}, {mesh::dot(e2, x), mesh::dot(e2, y)}};
            const double dT = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[1].y - q[0].y) * (q[2].x - q[0].x);
            if (dT <= 1e-20) continue;
            const double s = 1.0 / std::sqrt(2 * dT);
            SparseRow rx, ry;
            for (int j = 0; j < 3; ++j) {
                const Vec2 e = q[(j + 2) % 3] - q[(j + 1) % 3];
                // residual of grad v - rot90(grad u): u_j e + v_j rot90(e)
                const double ux = e.x * s, vx = -e.y * s, uy = e.y * s, vy = e.x * s;
                const std::uint32_t i = id[j];
                if (col[i] >= 0) {
                    rx.c.push_back({static_cast<std::uint32_t>(2 * col[i]), ux});
                    rx.c.push_back({static_cast<std::uint32_t>(2 * col[i] + 1), vx});
                    ry.c.push_back({static_cast<std::uint32_t>(2 * col[i]), uy});
                    ry.c.push_back({static_cast<std::uint32_t>(2 * col[i] + 1), vy});
                } else {
                    const Vec2 pin = i == pinA ? pA : pB;
                    rx.rhs -= ux * pin.x + vx * pin.y;
                    ry.rhs -= uy * pin.x + vy * pin.y;
                }
            }
            rows.push_back(std::move(rx));
            rows.push_back(std::move(ry));
        }
    }
    std::vector<double> sol(2 * free);
    for (std::uint32_t i = 0; i < unknowns; ++i)
        if (col[i] >= 0) {
            sol[2 * static_cast<std::size_t>(col[i])] = uv[i].x;
            sol[2 * static_cast<std::size_t>(col[i]) + 1] = uv[i].y;
        }
    solveLeastSquares(rows, sol, std::max(400, static_cast<int>(6 * free)));
    for (double v : sol)
        if (!std::isfinite(v)) return false;
    for (std::uint32_t i = 0; i < unknowns; ++i) {
        if (i == pinA) uv[i] = pA;
        else if (i == pinB) uv[i] = pB;
        else uv[i] = {sol[2 * static_cast<std::size_t>(col[i])], sol[2 * static_cast<std::size_t>(col[i]) + 1]};
    }
    for (auto f : faces) {
        Face& face = m.faces[f];
        face.uv.resize(face.v.size());
        for (std::uint32_t c = 0; c < face.v.size(); ++c) face.uv[c] = uv[wedge.at({f, c})];
    }
    return true;
}

void smartProject(PolyMesh& m, const std::vector<std::uint32_t>& faces, double angle, double scale, std::vector<std::vector<std::uint32_t>>& out) {
    // greedy clusters of normals, biggest faces first
    std::vector<std::uint32_t> order = faces;
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return faceArea(m, m.faces[a]) > faceArea(m, m.faces[b]); });
    const double c = std::cos(angle * mesh::kPi / 180.0);
    std::vector<Vec3> dirs;
    std::vector<int> label(m.faces.size(), -1);
    for (auto f : order) {
        const Vec3 n = faceNormal(m, m.faces[f]);
        int best = -1;
        double bestDot = c;
        for (std::size_t d = 0; d < dirs.size(); ++d) {
            const double dd = mesh::dot(n, dirs[d]);
            if (dd >= bestDot) bestDot = dd, best = static_cast<int>(d);
        }
        if (best < 0) {
            dirs.push_back(mesh::length(n) > 0 ? n : Vec3{0, 0, 1});
            best = static_cast<int>(dirs.size() - 1);
        }
        label[f] = best;
    }
    std::vector<char> el(m.faces.size(), 0);
    for (auto f : faces) el[f] = 1;
    for (auto& isl : islands(m, el, true, &label)) {
        projectIsland(m, isl, dirs[static_cast<std::size_t>(label[isl[0]])], scale);
        out.push_back(std::move(isl));
    }
}

struct Box {
    double x0, y0, x1, y1;
};

Box islandBox(const PolyMesh& m, const std::vector<std::uint32_t>& isl) {
    Box b{1e300, 1e300, -1e300, -1e300};
    for (auto f : isl)
        for (const auto& uv : m.faces[f].uv) b.x0 = std::min(b.x0, uv.x), b.y0 = std::min(b.y0, uv.y), b.x1 = std::max(b.x1, uv.x), b.y1 = std::max(b.y1, uv.y);
    return b;
}

// Rotates each island to its smallest bounding box, then shelf-packs them into 0..1.
void pack(PolyMesh& m, std::vector<std::vector<std::uint32_t>>& isls, double margin) {
    if (isls.empty()) return;
    for (auto& isl : isls) {
        double bestArea = 1e300, bestAngle = 0;
        for (int k = 0; k < 18; ++k) {
            const double a = (mesh::kPi / 2) * k / 18, ca = std::cos(a), sa = std::sin(a);
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (auto f : isl)
                for (const auto& uv : m.faces[f].uv) {
                    const double x = uv.x * ca - uv.y * sa, y = uv.x * sa + uv.y * ca;
                    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
                }
            const double area = (x1 - x0) * (y1 - y0);
            if (area < bestArea - 1e-12) bestArea = area, bestAngle = a;
        }
        const double ca = std::cos(bestAngle), sa = std::sin(bestAngle);
        for (auto f : isl)
            for (auto& uv : m.faces[f].uv) uv = {uv.x * ca - uv.y * sa, uv.x * sa + uv.y * ca};
        // taller than wide: lie it down
        const Box b = islandBox(m, isl);
        if (b.y1 - b.y0 > b.x1 - b.x0)
            for (auto f : isl)
                for (auto& uv : m.faces[f].uv) uv = {uv.y, -uv.x};
    }
    std::vector<Box> boxes;
    double area = 0;
    for (const auto& isl : isls) {
        boxes.push_back(islandBox(m, isl));
        area += (boxes.back().x1 - boxes.back().x0) * (boxes.back().y1 - boxes.back().y0);
    }
    const double gap = margin * std::sqrt(std::max(area, 1e-24));
    std::vector<std::size_t> order(isls.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return boxes[a].y1 - boxes[a].y0 > boxes[b].y1 - boxes[b].y0; });
    // try a few shelf widths and keep the squarest result
    double bestSide = 1e300;
    std::vector<Vec2> bestAt;
    double widest = 0;
    for (const auto& b : boxes) widest = std::max(widest, b.x1 - b.x0 + gap);
    for (double f : {0.9, 1.0, 1.1, 1.25, 1.5}) {
        const double rowW = std::max(widest, std::sqrt(area) * f + gap);
        std::vector<Vec2> at(isls.size());
        double x = gap, y = gap, rowH = 0, usedW = 0;
        for (auto i : order) {
            const double w = boxes[i].x1 - boxes[i].x0, h = boxes[i].y1 - boxes[i].y0;
            if (x + w + gap > rowW + 1e-12 && x > gap) {
                x = gap;
                y += rowH + gap;
                rowH = 0;
            }
            at[i] = {x, y};
            x += w + gap;
            rowH = std::max(rowH, h);
            usedW = std::max(usedW, x);
        }
        const double side = std::max(usedW, y + rowH + gap);
        if (side < bestSide) bestSide = side, bestAt = at;
    }
    const double s = 1.0 / std::max(bestSide, 1e-24);
    for (std::size_t i = 0; i < isls.size(); ++i) {
        for (auto f : isls[i])
            for (auto& uv : m.faces[f].uv) uv = {(uv.x - boxes[i].x0 + bestAt[i].x) * s, (uv.y - boxes[i].y0 + bestAt[i].y) * s};
    }
}

}  // namespace

std::size_t unwrap(PolyMesh& m, const UvOptions& o) {
    const auto el = eligibleFaces(m, o.selectedOnly);
    std::vector<std::uint32_t> all;
    for (std::uint32_t f = 0; f < m.faces.size(); ++f)
        if (el[f]) all.push_back(f);
    if (all.empty()) return 0;
    std::vector<std::vector<std::uint32_t>> isls;
    const int axis = std::clamp(o.axis, 0, 2);
    Vec3 ax;
    ax[axis] = 1;
    const mesh::Bounds b = bounds(m);
    switch (o.method) {
        case UvMethod::Planar:
            projectIsland(m, all, ax, o.scale);
            isls.push_back(all);
            break;
        case UvMethod::Cube: {
            std::vector<int> label(m.faces.size(), -1);
            for (auto f : all) {
                const Vec3 n = faceNormal(m, m.faces[f]);
                int best = 0;
                for (int k = 1; k < 3; ++k)
                    if (std::fabs(n[k]) > std::fabs(n[best])) best = k;
                label[f] = best * 2 + (n[best] < 0 ? 1 : 0);
            }
            for (auto& isl : islands(m, el, false, &label)) {
                const int l = label[isl[0]];
                Vec3 d;
                d[l / 2] = (l % 2) ? -1 : 1;
                projectIsland(m, isl, d, o.scale);
                isls.push_back(std::move(isl));
            }
            break;
        }
        case UvMethod::Cylinder:
        case UvMethod::Sphere: {
            const Vec3 c = b.center();
            const int ua = (axis + 1) % 3, va = (axis + 2) % 3;
            const double h0 = b.min[axis], h1 = std::max(b.max[axis], h0 + 1e-12);
            for (auto f : all) {
                Face& face = m.faces[f];
                face.uv.resize(face.v.size());
                for (std::size_t k = 0; k < face.v.size(); ++k) {
                    const Vec3 p = m.verts[face.v[k]].p - c;
                    const double u = std::atan2(p[va], p[ua]) / kTau + 0.5;
                    double v;
                    if (o.method == UvMethod::Cylinder) {
                        v = (m.verts[face.v[k]].p[axis] - h0) / (h1 - h0);
                    } else {
                        const double r = mesh::length(p);
                        v = r > 0 ? 1.0 - std::acos(std::clamp(p[axis] / r, -1.0, 1.0)) / mesh::kPi : 0.5;
                    }
                    face.uv[k] = {u, v};
                }
                unwrapU(face);
                if (o.method == UvMethod::Sphere) {
                    // corners at a pole take the u of the rest of the face
                    double us = 0;
                    int n = 0;
                    for (const auto& uv : face.uv)
                        if (uv.y > 1e-6 && uv.y < 1 - 1e-6) us += uv.x, ++n;
                    for (auto& uv : face.uv)
                        if ((uv.y <= 1e-6 || uv.y >= 1 - 1e-6) && n) uv.x = us / n;
                }
            }
            if (o.pack) {
                // keep the whole wrap as one island in 0..1 (scale u so the tile isn't stretched)
                isls.push_back(all);
                double lo = 1e300, hi = -1e300;
                for (auto f : all)
                    for (const auto& uv : m.faces[f].uv) lo = std::min(lo, uv.x), hi = std::max(hi, uv.x);
                const double span = std::max(hi - lo, 1e-12);
                for (auto f : all)
                    for (auto& uv : m.faces[f].uv) uv.x = (uv.x - lo) / span;
                return 1;
            }
            return 1;
        }
        case UvMethod::Smart:
            smartProject(m, all, o.angle, o.scale, isls);
            break;
        case UvMethod::Unwrap: {
            Topology t(m);
            std::vector<std::uint32_t> leftovers;
            for (auto& isl : islands(m, el, true)) {
                if (lscm(m, t, isl)) isls.push_back(std::move(isl));
                else leftovers.insert(leftovers.end(), isl.begin(), isl.end());
            }
            if (!leftovers.empty()) smartProject(m, leftovers, o.angle, o.scale, isls);
            break;
        }
    }
    if (o.pack) pack(m, isls, o.margin);
    return isls.size();
}

std::size_t packIslands(PolyMesh& m, double margin, bool selectedOnly) {
    auto el = eligibleFaces(m, selectedOnly);
    for (std::size_t f = 0; f < m.faces.size(); ++f) el[f] = el[f] && m.faces[f].uv.size() == m.faces[f].v.size();
    // islands in UV space: faces sharing an edge with the same uvs on both sides
    Topology t(m);
    std::vector<std::vector<std::uint32_t>> isls;
    std::vector<int> id(m.faces.size(), -1);
    auto uvAt = [&](std::uint32_t f, std::uint32_t v) {
        const auto& fv = m.faces[f].v;
        return m.faces[f].uv[static_cast<std::size_t>(std::find(fv.begin(), fv.end(), v) - fv.begin())];
    };
    for (std::uint32_t f0 = 0; f0 < m.faces.size(); ++f0) {
        if (!el[f0] || id[f0] >= 0) continue;
        isls.emplace_back();
        std::vector<std::uint32_t> stack{f0};
        id[f0] = static_cast<int>(isls.size() - 1);
        while (!stack.empty()) {
            const std::uint32_t f = stack.back();
            stack.pop_back();
            isls.back().push_back(f);
            const auto& fv = m.faces[f].v;
            for (std::size_t i = 0; i < fv.size(); ++i) {
                const std::uint32_t a = fv[i], b2 = fv[(i + 1) % fv.size()];
                for (const auto& h : t.edgeFaces[t.index.at(edgeKey(a, b2))]) {
                    if (id[h.face] >= 0 || !el[h.face]) continue;
                    const Vec2 d1 = uvAt(f, a) - uvAt(h.face, a), d2 = uvAt(f, b2) - uvAt(h.face, b2);
                    if (std::hypot(d1.x, d1.y) > 1e-9 || std::hypot(d2.x, d2.y) > 1e-9) continue;
                    id[h.face] = id[f];
                    stack.push_back(h.face);
                }
            }
        }
    }
    pack(m, isls, margin);
    return isls.size();
}

std::size_t seamsFromSharp(PolyMesh& m, double degrees) {
    Topology t(m);
    const double c = std::cos(degrees * mesh::kPi / 180.0);
    std::size_t n = 0;
    for (std::size_t e = 0; e < t.edges.size(); ++e) {
        const auto& ef = t.edgeFaces[e];
        bool seam = ef.size() == 1;
        if (ef.size() == 2) seam = mesh::dot(faceNormal(m, m.faces[ef[0].face]), faceNormal(m, m.faces[ef[1].face])) < c;
        if (seam && m.seams.insert(t.edges[e]).second) ++n;
    }
    return n;
}

raster::Image uvLayout(const PolyMesh& m, int size, bool selectedOnly) {
    raster::Image img(size, size, {0, 0, 0, 0});
    auto toPx = [&](const Vec2& uv) { return Vec2{uv.x * size, (1 - uv.y) * size}; };
    // fill: scanline over each face's triangles
    for (const auto& f : m.faces) {
        if (f.uv.size() != f.v.size() || (selectedOnly && !f.sel)) continue;
        const auto tri = triangulateFace(m, f);
        for (std::size_t k = 0; k + 2 < tri.size(); k += 3) {
            const Vec2 a = toPx(f.uv[tri[k]]), b = toPx(f.uv[tri[k + 1]]), c = toPx(f.uv[tri[k + 2]]);
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(size - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(size - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
            const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::fabs(area) < 1e-12) continue;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double px = x + 0.5, py = y + 0.5;
                    const double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                    const double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                    const double w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    img.set(x, y, f.sel ? raster::Color{255, 200, 140, 200} : raster::Color{200, 214, 235, 200});
                }
        }
    }
    // edges
    for (const auto& f : m.faces) {
        if (f.uv.size() != f.v.size() || (selectedOnly && !f.sel)) continue;
        for (std::size_t i = 0; i < f.uv.size(); ++i) {
            const Vec2 a = toPx(f.uv[i]), b = toPx(f.uv[(i + 1) % f.uv.size()]);
            const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y)))));
            for (int s = 0; s <= steps; ++s) {
                const double t = static_cast<double>(s) / steps;
                const int x = static_cast<int>(a.x + (b.x - a.x) * t), y = static_cast<int>(a.y + (b.y - a.y) * t);
                if (img.contains(x, y)) img.set(x, y, {30, 34, 44, 255});
            }
        }
    }
    return img;
}

}  // namespace wetwaresuite::model
