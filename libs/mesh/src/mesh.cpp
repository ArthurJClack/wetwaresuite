#include "wetwaresuite/mesh/mesh.hpp"

#include <algorithm>
#include <unordered_map>

#include "internal.hpp"

namespace wetwaresuite::mesh {

using detail::edgeKey;

Vec3 Mesh::faceNormal(std::size_t t) const {
    const Vec3& a = corner(t, 0);
    return cross(corner(t, 1) - a, corner(t, 2) - a);
}

Bounds bounds(const Mesh& mesh) {
    Bounds b;
    for (const auto& p : mesh.positions) b.add(p);
    return b;
}

double surfaceArea(const Mesh& mesh) {
    double a = 0;
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) a += length(mesh.faceNormal(t));
    return a * 0.5;
}

double volume(const Mesh& mesh) {
    // Relative to the bounds centre to keep precision for meshes far from the origin.
    const Vec3 c = bounds(mesh).center();
    double v = 0;
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        v += dot(mesh.corner(t, 0) - c, cross(mesh.corner(t, 1) - c, mesh.corner(t, 2) - c));
    }
    return v / 6.0;
}

namespace {

// Directed edge counts over exact-position vertex ids.
std::unordered_map<std::uint64_t, int> directedEdges(const Mesh& mesh) {
    detail::ExactIndex ids;
    ids.reserve(mesh.positions.size());
    std::vector<std::uint32_t> remap(mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) remap[i] = ids.id(mesh.positions[i]);
    std::unordered_map<std::uint64_t, int> edges;
    edges.reserve(mesh.indices.size());
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        std::uint32_t v[3] = {remap[mesh.indices[t * 3]], remap[mesh.indices[t * 3 + 1]], remap[mesh.indices[t * 3 + 2]]};
        if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;
        for (int k = 0; k < 3; ++k) ++edges[edgeKey(v[k], v[(k + 1) % 3])];
    }
    return edges;
}

}  // namespace

bool isWatertight(const Mesh& mesh) {
    if (mesh.empty()) return false;
    auto edges = directedEdges(mesh);
    for (const auto& [key, count] : edges) {
        if (count != 1) return false;
        auto a = static_cast<std::uint32_t>(key >> 32), b = static_cast<std::uint32_t>(key & 0xffffffffu);
        auto it = edges.find(edgeKey(b, a));
        if (it == edges.end() || it->second != 1) return false;
    }
    return true;
}

std::size_t boundaryEdgeCount(const Mesh& mesh) {
    auto edges = directedEdges(mesh);
    std::size_t n = 0;
    for (const auto& [key, count] : edges) {
        auto a = static_cast<std::uint32_t>(key >> 32), b = static_cast<std::uint32_t>(key & 0xffffffffu);
        auto it = edges.find(edgeKey(b, a));
        int back = it == edges.end() ? 0 : it->second;
        if (count > back) n += static_cast<std::size_t>(count - back);
    }
    return n;
}

void cleanup(Mesh& mesh) {
    std::vector<std::uint32_t> kept;
    kept.reserve(mesh.indices.size());
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        std::uint32_t a = mesh.indices[t * 3], b = mesh.indices[t * 3 + 1], c = mesh.indices[t * 3 + 2];
        if (a == b || b == c || a == c) continue;
        kept.insert(kept.end(), {a, b, c});
    }
    mesh.indices = std::move(kept);

    const bool n = mesh.hasNormals(), uv = mesh.hasUvs();
    std::vector<std::uint32_t> remap(mesh.positions.size(), UINT32_MAX);
    std::vector<Vec3> pos, nor;
    std::vector<Vec2> tex;
    for (auto& i : mesh.indices) {
        if (remap[i] == UINT32_MAX) {
            remap[i] = static_cast<std::uint32_t>(pos.size());
            pos.push_back(mesh.positions[i]);
            if (n) nor.push_back(mesh.normals[i]);
            if (uv) tex.push_back(mesh.uvs[i]);
        }
        i = remap[i];
    }
    mesh.positions = std::move(pos);
    mesh.normals = std::move(nor);
    mesh.uvs = std::move(tex);
}

void weld(Mesh& mesh, double tolerance) {
    detail::Welder welder(tolerance);
    std::vector<std::uint32_t> remap(mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) remap[i] = welder.add(mesh.positions[i]);
    const bool n = mesh.hasNormals(), uv = mesh.hasUvs();
    std::vector<Vec3> nor(n ? welder.points.size() : 0);
    std::vector<Vec2> tex(uv ? welder.points.size() : 0);
    for (std::size_t i = mesh.positions.size(); i-- > 0;) {  // backwards so the first vertex wins
        if (n) nor[remap[i]] = mesh.normals[i];
        if (uv) tex[remap[i]] = mesh.uvs[i];
    }
    for (auto& i : mesh.indices) i = remap[i];
    mesh.positions = std::move(welder.points);
    mesh.normals = std::move(nor);
    mesh.uvs = std::move(tex);
    cleanup(mesh);
}

void computeNormals(Mesh& mesh, double creaseDegrees) {
    const std::size_t nv = mesh.positions.size(), nt = mesh.triangleCount();
    std::vector<Vec3> fn(nt), fu(nt);
    for (std::size_t t = 0; t < nt; ++t) {
        fn[t] = mesh.faceNormal(t);
        fu[t] = normalize(fn[t]);
    }
    if (creaseDegrees >= 180.0) {
        std::vector<Vec3> normals(nv);
        for (std::size_t t = 0; t < nt; ++t) {
            for (int k = 0; k < 3; ++k) normals[mesh.indices[t * 3 + static_cast<std::size_t>(k)]] += fn[t];
        }
        for (auto& v : normals) v = normalize(v);
        mesh.normals = std::move(normals);
        return;
    }

    // Faces around each vertex (CSR layout).
    std::vector<std::uint32_t> start(nv + 1, 0), faces(mesh.indices.size());
    for (auto i : mesh.indices) ++start[i + 1];
    for (std::size_t i = 0; i < nv; ++i) start[i + 1] += start[i];
    std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
    for (std::size_t c = 0; c < mesh.indices.size(); ++c) faces[fill[mesh.indices[c]]++] = static_cast<std::uint32_t>(c / 3);

    const double cosCrease = std::cos(creaseDegrees * kPi / 180.0);
    const bool uv = mesh.hasUvs();
    Mesh out;
    out.positions.reserve(nv);
    out.normals.reserve(nv);
    // Per original vertex: the split copies created so far.
    std::vector<std::vector<std::uint32_t>> copies(nv);
    out.indices.resize(mesh.indices.size());
    for (std::size_t c = 0; c < mesh.indices.size(); ++c) {
        const std::uint32_t v = mesh.indices[c];
        const std::size_t t = c / 3;
        Vec3 n;
        for (std::uint32_t k = start[v]; k < start[v + 1]; ++k) {
            const std::uint32_t f = faces[k];
            if (f == t || dot(fu[f], fu[t]) >= cosCrease) n += fn[f];
        }
        n = normalize(n);
        std::uint32_t id = UINT32_MAX;
        for (auto cand : copies[v]) {
            if (dot(out.normals[cand], n) > 1.0 - 1e-9) {
                id = cand;
                break;
            }
        }
        if (id == UINT32_MAX) {
            id = static_cast<std::uint32_t>(out.positions.size());
            out.positions.push_back(mesh.positions[v]);
            out.normals.push_back(n);
            if (uv) out.uvs.push_back(mesh.uvs[v]);
            copies[v].push_back(id);
        }
        out.indices[c] = id;
    }
    mesh = std::move(out);
}

void flip(Mesh& mesh) {
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) std::swap(mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]);
    for (auto& n : mesh.normals) n = -n;
}

void transform(Mesh& mesh, const Mat4& m) {
    for (auto& p : mesh.positions) p = m.point(p);
    for (auto& n : mesh.normals) n = m.normal(n);
    if (m.determinant3() < 0) {
        for (std::size_t t = 0; t < mesh.triangleCount(); ++t) std::swap(mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]);
    }
}

void append(Mesh& dst, const Mesh& src) {
    if (dst.positions.empty()) {
        dst = src;
        return;
    }
    const auto offset = static_cast<std::uint32_t>(dst.positions.size());
    const bool n = dst.hasNormals() && src.hasNormals(), uv = dst.hasUvs() && src.hasUvs();
    dst.positions.insert(dst.positions.end(), src.positions.begin(), src.positions.end());
    if (n) dst.normals.insert(dst.normals.end(), src.normals.begin(), src.normals.end());
    else dst.normals.clear();
    if (uv) dst.uvs.insert(dst.uvs.end(), src.uvs.begin(), src.uvs.end());
    else dst.uvs.clear();
    dst.indices.reserve(dst.indices.size() + src.indices.size());
    for (auto i : src.indices) dst.indices.push_back(i + offset);
}

Mesh unweld(const Mesh& mesh) {
    Mesh out;
    const bool n = mesh.hasNormals(), uv = mesh.hasUvs();
    out.positions.reserve(mesh.indices.size());
    for (std::size_t c = 0; c < mesh.indices.size(); ++c) {
        const auto i = mesh.indices[c];
        out.positions.push_back(mesh.positions[i]);
        if (n) out.normals.push_back(mesh.normals[i]);
        if (uv) out.uvs.push_back(mesh.uvs[i]);
        out.indices.push_back(static_cast<std::uint32_t>(c));
    }
    return out;
}

}  // namespace wetwaresuite::mesh
