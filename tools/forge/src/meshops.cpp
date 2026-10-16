#include "wetwaresuite/forge/meshops.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "wetwaresuite/mesh/ops.hpp"

namespace wetwaresuite::forge {

using mesh::Bounds;
using mesh::Mesh;
using mesh::Vec2;
using mesh::Vec3;

void generateUvs(Mesh& m, Projection projection, int axis, double scale) {
    if (m.empty()) return;
    axis = std::clamp(axis, 0, 2);
    const Bounds b = mesh::bounds(m);
    const Vec3 size = b.size(), center = b.center();
    const double longest = std::max({size.x, size.y, size.z, 1e-12});
    // the two axes across a projection direction, picked so the picture isn't mirrored
    auto across = [](int a) { return a == 0 ? std::pair{1, 2} : a == 1 ? std::pair{0, 2} : std::pair{0, 1}; };

    std::vector<Vec2> corner(m.indices.size());
    const std::size_t nt = m.triangleCount();
    for (std::size_t t = 0; t < nt; ++t) {
        Vec2 uv[3];
        if (projection == Projection::Planar || projection == Projection::Box) {
            int a = axis;
            bool negative = false;
            double k = 1.0 / longest;
            if (projection == Projection::Box) {
                const Vec3 n = m.faceNormal(t);
                const double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
                a = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
                negative = n[a] < 0;
            } else {
                auto [u, v] = across(a);
                k = 1.0 / std::max({size[u], size[v], 1e-12});
            }
            auto [u, v] = across(a);
            // look at each side from outside so pictures aren't mirrored (+Y faces read right to left in X)
            const bool flipU = a == 1 ? !negative : negative;
            for (int c = 0; c < 3; ++c) {
                const Vec3& p = m.corner(t, c);
                double uu = (p[u] - b.min[u]) * k;
                if (projection == Projection::Box && flipU) uu = (b.max[u] - p[u]) * k;
                uv[c] = {uu, (p[v] - b.min[v]) * k};
            }
        } else {
            auto [u, v] = across(axis);
            for (int c = 0; c < 3; ++c) {
                const Vec3 d = m.corner(t, c) - center;
                const double angle = std::atan2(d[v], d[u]);
                const double uu = (angle + mesh::kPi) / (2 * mesh::kPi);
                double vv;
                if (projection == Projection::Cylindrical) {
                    vv = size[axis] > 0 ? (m.corner(t, c)[axis] - b.min[axis]) / size[axis] : 0.5;
                } else {
                    const double len = mesh::length(d);
                    vv = len > 0 ? 1.0 - std::acos(std::clamp(d[axis] / len, -1.0, 1.0)) / mesh::kPi : 0.5;
                }
                uv[c] = {uu, vv};
            }
            // a triangle across the wrap seam gets its low side moved past 1
            const double lo = std::min({uv[0].x, uv[1].x, uv[2].x}), hi = std::max({uv[0].x, uv[1].x, uv[2].x});
            if (hi - lo > 0.5) {
                for (auto& q : uv) {
                    if (q.x < 0.5) q.x += 1.0;
                }
            }
        }
        for (int c = 0; c < 3; ++c) corner[t * 3 + static_cast<std::size_t>(c)] = uv[c] * scale;
    }

    // one vertex per (original vertex, uv) pair
    Mesh out;
    const bool normals = m.hasNormals();
    std::map<std::tuple<std::uint32_t, double, double>, std::uint32_t> seen;
    out.indices.reserve(m.indices.size());
    for (std::size_t c = 0; c < m.indices.size(); ++c) {
        const std::uint32_t v = m.indices[c];
        const Vec2 uv = corner[c];
        auto [it, inserted] = seen.try_emplace({v, uv.x, uv.y}, static_cast<std::uint32_t>(out.positions.size()));
        if (inserted) {
            out.positions.push_back(m.positions[v]);
            if (normals) out.normals.push_back(m.normals[v]);
            out.uvs.push_back(uv);
        }
        out.indices.push_back(it->second);
    }
    m = std::move(out);
}

void convertAxes(Mesh& m, AxisConversion conversion) {
    if (conversion == AxisConversion::YUpToZUp) mesh::transform(m, mesh::Mat4::rotateX(90));
    if (conversion == AxisConversion::ZUpToYUp) mesh::transform(m, mesh::Mat4::rotateX(-90));
}

std::optional<AxisConversion> parseAxes(const std::string& name) {
    if (name.empty() || name == "none") return AxisConversion::None;
    if (name == "y_up_to_z_up") return AxisConversion::YUpToZUp;
    if (name == "z_up_to_y_up") return AxisConversion::ZUpToYUp;
    return std::nullopt;
}

void recomputeNormals(Mesh& m, double creaseDegrees) {
    const std::size_t nv = m.positions.size(), nt = m.triangleCount();
    // vertices at the same position share one group
    std::map<std::tuple<double, double, double>, std::uint32_t> groupOf;
    std::vector<std::uint32_t> group(nv);
    for (std::size_t i = 0; i < nv; ++i) {
        const Vec3& p = m.positions[i];
        group[i] = groupOf.try_emplace({p.x, p.y, p.z}, static_cast<std::uint32_t>(groupOf.size())).first->second;
    }
    const std::size_t ng = groupOf.size();
    std::vector<Vec3> fn(nt), fu(nt);
    for (std::size_t t = 0; t < nt; ++t) {
        fn[t] = m.faceNormal(t);
        fu[t] = mesh::normalize(fn[t]);
    }
    std::vector<std::uint32_t> start(ng + 1, 0), faces(m.indices.size());
    for (auto i : m.indices) ++start[group[i] + 1];
    for (std::size_t g = 0; g < ng; ++g) start[g + 1] += start[g];
    std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
    for (std::size_t c = 0; c < m.indices.size(); ++c) faces[fill[group[m.indices[c]]]++] = static_cast<std::uint32_t>(c / 3);

    const bool all = creaseDegrees >= 180.0;
    const double cosCrease = std::cos(std::clamp(creaseDegrees, 0.0, 180.0) * mesh::kPi / 180.0);
    const bool uv = m.hasUvs();
    Mesh out;
    std::vector<std::vector<std::uint32_t>> copies(nv);
    out.indices.resize(m.indices.size());
    for (std::size_t c = 0; c < m.indices.size(); ++c) {
        const std::uint32_t v = m.indices[c];
        const std::size_t t = c / 3;
        Vec3 n;
        const std::uint32_t g = group[v];
        for (std::uint32_t k = start[g]; k < start[g + 1]; ++k) {
            const std::uint32_t f = faces[k];
            if (all || f == t || mesh::dot(fu[f], fu[t]) >= cosCrease) n += fn[f];
        }
        n = mesh::normalize(n);
        std::uint32_t id = UINT32_MAX;
        for (auto cand : copies[v]) {
            if (mesh::dot(out.normals[cand], n) > 1.0 - 1e-9) {
                id = cand;
                break;
            }
        }
        if (id == UINT32_MAX) {
            id = static_cast<std::uint32_t>(out.positions.size());
            out.positions.push_back(m.positions[v]);
            out.normals.push_back(n);
            if (uv) out.uvs.push_back(m.uvs[v]);
            copies[v].push_back(id);
        }
        out.indices[c] = id;
    }
    m = std::move(out);
}

Mesh decimateTo(const Mesh& m, std::size_t triangles, bool normals, double creaseDegrees) {
    if (triangles >= m.triangleCount()) return m;
    Mesh out = mesh::decimate(m, std::max<std::size_t>(triangles, 1));
    if (normals && !out.empty()) recomputeNormals(out, creaseDegrees);
    return out;
}

Mesh decimateRatio(const Mesh& m, double ratio, bool normals, double creaseDegrees) {
    if (ratio >= 1.0) return m;
    const auto target = static_cast<std::size_t>(std::llround(static_cast<double>(m.triangleCount()) * std::max(ratio, 0.0)));
    return decimateTo(m, target, normals, creaseDegrees);
}

}  // namespace wetwaresuite::forge
