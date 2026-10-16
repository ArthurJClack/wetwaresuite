#include "achclwss/mesh/topology.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace achclwss::mesh {

Adjacency vertexTriangles(const Mesh& m) {
    Adjacency a;
    const std::size_t nv = m.positions.size();
    a.offsets.assign(nv + 1, 0);
    for (auto i : m.indices) ++a.offsets[i + 1];
    for (std::size_t v = 0; v < nv; ++v) a.offsets[v + 1] += a.offsets[v];
    a.items.resize(m.indices.size());
    std::vector<std::uint32_t> fill(a.offsets.begin(), a.offsets.end() - 1);
    for (std::size_t c = 0; c < m.indices.size(); ++c) a.items[fill[m.indices[c]]++] = static_cast<std::uint32_t>(c / 3);
    return a;
}

Adjacency vertexNeighbors(const Mesh& m) {
    const std::size_t nv = m.positions.size();
    // Each corner contributes its two triangle neighbours; dedupe per vertex.
    std::vector<std::uint32_t> start(nv + 1, 0);
    for (auto i : m.indices) start[i + 1] += 2;
    for (std::size_t v = 0; v < nv; ++v) start[v + 1] += start[v];
    std::vector<std::uint32_t> raw(start[nv]);
    std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const std::uint32_t* f = &m.indices[t * 3];
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = f[k];
            raw[fill[v]++] = f[(k + 1) % 3];
            raw[fill[v]++] = f[(k + 2) % 3];
        }
    }
    Adjacency a;
    a.offsets.assign(nv + 1, 0);
    a.items.reserve(raw.size() / 2 + nv);
    for (std::size_t v = 0; v < nv; ++v) {
        auto b = raw.begin() + start[v], e = raw.begin() + start[v + 1];
        std::sort(b, e);
        auto u = std::unique(b, e);
        for (auto it = b; it != u; ++it) {
            if (*it != v) a.items.push_back(*it);
        }
        a.offsets[v + 1] = static_cast<std::uint32_t>(a.items.size());
    }
    return a;
}

std::vector<Vec3> faceNormals(const Mesh& m) {
    std::vector<Vec3> n(m.triangleCount());
    for (std::size_t t = 0; t < n.size(); ++t) n[t] = normalize(m.faceNormal(t));
    return n;
}

std::vector<Vec3> vertexNormals(const Mesh& m) {
    std::vector<Vec3> n(m.positions.size());
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const Vec3 fn = m.faceNormal(t);
        for (int k = 0; k < 3; ++k) n[m.indices[t * 3 + static_cast<std::size_t>(k)]] += fn;
    }
    for (auto& v : n) v = normalize(v);
    return n;
}

std::vector<bool> boundaryVertices(const Mesh& m) {
    std::unordered_map<std::uint64_t, int> edges;
    edges.reserve(m.indices.size());
    auto key = [](std::uint32_t a, std::uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        for (std::size_t k = 0; k < 3; ++k) ++edges[key(m.indices[t * 3 + k], m.indices[t * 3 + (k + 1) % 3])];
    }
    std::vector<bool> out(m.positions.size(), false);
    for (const auto& [k, n] : edges) {
        if (n == 1) {
            out[static_cast<std::size_t>(k >> 32)] = true;
            out[static_cast<std::size_t>(k & 0xffffffffu)] = true;
        }
    }
    return out;
}

Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double denom = va + vb + vc;
    if (denom == 0) return a;
    const double v = vb / denom, w = vc / denom;
    return a + ab * v + ac * w;
}

double rayTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c) {
    // Moller-Trumbore.
    const Vec3 e1 = b - a, e2 = c - a, pv = cross(d, e2);
    const double det = dot(e1, pv);
    if (std::fabs(det) < 1e-300) return -1;
    const double inv = 1.0 / det;
    const Vec3 tv = o - a;
    const double u = dot(tv, pv) * inv;
    if (u < 0 || u > 1) return -1;
    const Vec3 qv = cross(tv, e1);
    const double v = dot(d, qv) * inv;
    if (v < 0 || u + v > 1) return -1;
    const double t = dot(e2, qv) * inv;
    return t >= 0 ? t : -1;
}

}  // namespace achclwss::mesh
