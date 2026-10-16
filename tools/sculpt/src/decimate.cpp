// Quadric edge collapse decimation (Garland and Heckbert 1997).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::sculpt {

namespace {

// Symmetric 4x4 error quadric: xx xy xz xw yy yz yw zz zw ww.
struct Quadric {
    double q[10] = {};

    void addPlane(const Vec3& n, double d, double w) {
        q[0] += w * n.x * n.x; q[1] += w * n.x * n.y; q[2] += w * n.x * n.z; q[3] += w * n.x * d;
        q[4] += w * n.y * n.y; q[5] += w * n.y * n.z; q[6] += w * n.y * d;
        q[7] += w * n.z * n.z; q[8] += w * n.z * d;
        q[9] += w * d * d;
    }
    Quadric& operator+=(const Quadric& o) {
        for (int i = 0; i < 10; ++i) q[i] += o.q[i];
        return *this;
    }
    double error(const Vec3& p) const {
        return q[0] * p.x * p.x + 2 * q[1] * p.x * p.y + 2 * q[2] * p.x * p.z + 2 * q[3] * p.x + q[4] * p.y * p.y + 2 * q[5] * p.y * p.z +
               2 * q[6] * p.y + q[7] * p.z * p.z + 2 * q[8] * p.z + q[9];
    }
    // Minimiser of the error, if the 3x3 system is well conditioned.
    bool optimum(Vec3& out) const {
        const double a = q[0], b = q[1], c = q[2], d = q[4], e = q[5], f = q[7];
        const double det = a * (d * f - e * e) - b * (b * f - e * c) + c * (b * e - d * c);
        const double scale = std::fabs(a) + std::fabs(d) + std::fabs(f);
        if (std::fabs(det) < 1e-12 * scale * scale * scale || scale == 0) return false;
        const double rx = -q[3], ry = -q[6], rz = -q[8];
        out.x = (rx * (d * f - e * e) - b * (ry * f - e * rz) + c * (ry * e - d * rz)) / det;
        out.y = (a * (ry * f - e * rz) - rx * (b * f - e * c) + c * (b * rz - ry * c)) / det;
        out.z = (a * (d * rz - ry * e) - b * (b * rz - ry * c) + rx * (b * e - d * c)) / det;
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }
};

struct Candidate {
    double cost;
    std::uint32_t a, b;
    std::uint32_t va, vb;  // versions when pushed
    Vec3 target;
    bool operator<(const Candidate& o) const { return cost > o.cost; }  // min-heap
};

}  // namespace

Mesh decimateQuadric(const Mesh& in, std::size_t target, Mask* mask) {
    if (in.empty()) throw std::invalid_argument("the mesh has no triangles to decimate");
    if (target < 4) throw std::invalid_argument("aim for at least 4 triangles");
    if (in.triangleCount() <= target) return in;

    std::vector<Vec3> P = in.positions;
    std::vector<std::uint32_t> T = in.indices;
    const std::size_t nv = P.size(), nt = in.triangleCount();
    std::vector<bool> triAlive(nt, true), vertAlive(nv, true);
    std::vector<std::uint32_t> version(nv, 0);
    std::vector<std::vector<std::uint32_t>> vtris(nv);
    for (std::size_t t = 0; t < nt; ++t) {
        for (std::size_t k = 0; k < 3; ++k) vtris[T[t * 3 + k]].push_back(static_cast<std::uint32_t>(t));
    }
    std::vector<bool> locked(nv, false);
    if (mask && mask->size() == nv) {
        for (std::size_t v = 0; v < nv; ++v) locked[v] = (*mask)[v] >= 0.5f;
    }

    // Quadrics: each face's plane weighted by area, plus stiff planes along open edges.
    std::vector<Quadric> Q(nv);
    std::unordered_map<std::uint64_t, std::pair<int, std::uint32_t>> edges;  // count, a face
    edges.reserve(nt * 2);
    auto key = [](std::uint32_t a, std::uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };
    for (std::size_t t = 0; t < nt; ++t) {
        const Vec3 fn = in.faceNormal(t);
        const double area2 = mesh::length(fn);
        if (area2 > 0) {
            const Vec3 n = fn / area2;
            const double d = -mesh::dot(n, P[T[t * 3]]);
            for (std::size_t k = 0; k < 3; ++k) Q[T[t * 3 + k]].addPlane(n, d, area2 * 0.5);
        }
        for (std::size_t k = 0; k < 3; ++k) {
            auto& e = edges[key(T[t * 3 + k], T[t * 3 + (k + 1) % 3])];
            ++e.first;
            e.second = static_cast<std::uint32_t>(t);
        }
    }
    for (const auto& [k, e] : edges) {
        if (e.first != 1) continue;
        const std::uint32_t a = static_cast<std::uint32_t>(k >> 32), b = static_cast<std::uint32_t>(k & 0xffffffffu);
        const Vec3 edge = P[b] - P[a];
        const Vec3 n = mesh::normalize(mesh::cross(edge, mesh::normalize(in.faceNormal(e.second))));
        if (mesh::length(n) == 0) continue;
        const double d = -mesh::dot(n, P[a]);
        const double w = mesh::dot(edge, edge) * 10;
        Q[a].addPlane(n, d, w);
        Q[b].addPlane(n, d, w);
    }

    auto neighbours = [&](std::uint32_t v, std::vector<std::uint32_t>& out) {
        out.clear();
        for (auto t : vtris[v]) {
            if (!triAlive[t]) continue;
            for (std::size_t k = 0; k < 3; ++k) {
                const std::uint32_t u = T[t * 3 + k];
                if (u != v) out.push_back(u);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    };

    std::priority_queue<Candidate> heap;
    auto push = [&](std::uint32_t a, std::uint32_t b) {
        if (locked[a] || locked[b]) return;
        Quadric q = Q[a];
        q += Q[b];
        Vec3 best;
        double cost;
        if (q.optimum(best)) {
            cost = q.error(best);
        } else {
            const Vec3 mid = (P[a] + P[b]) * 0.5;
            const double ea = q.error(P[a]), eb = q.error(P[b]), em = q.error(mid);
            best = ea <= eb && ea <= em ? P[a] : eb <= em ? P[b] : mid;
            cost = std::min({ea, eb, em});
        }
        // Keep the target near the edge: a far optimum on nearly flat regions makes spikes.
        const double len = mesh::length(P[b] - P[a]);
        const Vec3 mid = (P[a] + P[b]) * 0.5;
        if (mesh::length(best - mid) > len * 2) {
            best = mid;
            cost = q.error(mid);
        }
        heap.push({std::max(0.0, cost), a, b, version[a], version[b], best});
    };
    for (const auto& [k, e] : edges) push(static_cast<std::uint32_t>(k >> 32), static_cast<std::uint32_t>(k & 0xffffffffu));

    std::size_t live = nt;
    std::vector<std::uint32_t> na, nb, common;
    while (live > target && !heap.empty()) {
        const Candidate c = heap.top();
        heap.pop();
        if (!vertAlive[c.a] || !vertAlive[c.b] || version[c.a] != c.va || version[c.b] != c.vb) continue;
        // Link condition: the edge's two ends share exactly the vertices opposite it.
        neighbours(c.a, na);
        neighbours(c.b, nb);
        if (!std::binary_search(na.begin(), na.end(), c.b)) continue;
        common.clear();
        std::set_intersection(na.begin(), na.end(), nb.begin(), nb.end(), std::back_inserter(common));
        std::size_t shared = 0;
        for (auto t : vtris[c.a]) {
            if (!triAlive[t]) continue;
            const std::uint32_t* f = &T[static_cast<std::size_t>(t) * 3];
            if (f[0] == c.b || f[1] == c.b || f[2] == c.b) ++shared;
        }
        if (common.size() != shared || shared == 0) continue;
        // No face may flip or collapse.
        bool ok = true;
        for (std::uint32_t end : {c.a, c.b}) {
            for (auto t : vtris[end]) {
                if (!triAlive[t]) continue;
                const std::uint32_t* f = &T[static_cast<std::size_t>(t) * 3];
                if ((f[0] == c.a || f[1] == c.a || f[2] == c.a) && (f[0] == c.b || f[1] == c.b || f[2] == c.b)) continue;
                Vec3 p[3], q[3];
                for (int k = 0; k < 3; ++k) {
                    p[k] = P[f[k]];
                    q[k] = f[k] == end ? c.target : P[f[k]];
                }
                const Vec3 n0 = mesh::cross(p[1] - p[0], p[2] - p[0]), n1 = mesh::cross(q[1] - q[0], q[2] - q[0]);
                const double l0 = mesh::length(n0), l1 = mesh::length(n1);
                if (l1 <= 1e-14 * (l0 + 1e-300) || mesh::dot(n0, n1) < 0.2 * l0 * l1) {
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
        }
        if (!ok) continue;

        // Collapse b into a.
        P[c.a] = c.target;
        Q[c.a] += Q[c.b];
        if (mask && mask->size() == nv) (*mask)[c.a] = std::max((*mask)[c.a], (*mask)[c.b]);
        for (auto t : vtris[c.b]) {
            if (!triAlive[t]) continue;
            std::uint32_t* f = &T[static_cast<std::size_t>(t) * 3];
            if (f[0] == c.a || f[1] == c.a || f[2] == c.a) {
                triAlive[t] = false;
                --live;
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                if (f[k] == c.b) f[k] = c.a;
            }
            vtris[c.a].push_back(t);
        }
        vertAlive[c.b] = false;
        vtris[c.b].clear();
        auto& va = vtris[c.a];
        va.erase(std::remove_if(va.begin(), va.end(), [&](std::uint32_t t) { return !triAlive[t]; }), va.end());
        ++version[c.a];
        ++version[c.b];
        neighbours(c.a, na);
        for (auto n : na) push(c.a, n);
    }

    Mesh out;
    std::vector<std::uint32_t> remap(nv, UINT32_MAX);
    Mask newMask;
    for (std::size_t t = 0; t < nt; ++t) {
        if (!triAlive[t]) continue;
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t v = T[t * 3 + k];
            if (remap[v] == UINT32_MAX) {
                remap[v] = out.addVertex(P[v]);
                if (mask && mask->size() == nv) newMask.push_back((*mask)[v]);
            }
            out.indices.push_back(remap[v]);
        }
    }
    if (mask) *mask = std::move(newMask);
    return out;
}

}  // namespace wetwaresuite::sculpt
