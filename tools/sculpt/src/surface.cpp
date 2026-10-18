// Whole-mesh operations: smoothing, subdivision, symmetrize, displacement, masks, stats.
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "wetwaresuite/mesh/topology.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::sculpt {

namespace {

std::uint64_t edgeKey(std::uint32_t a, std::uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<std::uint64_t>(a) << 32) | b;
}

double maskAt(const Mask& mask, std::size_t v) { return v < mask.size() ? std::clamp(static_cast<double>(mask[v]), 0.0, 1.0) : 0.0; }

}  // namespace

// ---- smoothing ----

double smooth(Mesh& m, const Mask& mask, int iterations, double strength, bool taubin) {
    if (iterations < 0) throw std::invalid_argument("iterations can't be negative");
    if (!(strength > 0) || strength > 1) throw std::invalid_argument("strength must be in (0, 1]");
    const mesh::Adjacency nbr = mesh::vertexNeighbors(m);
    const std::vector<bool> boundary = mesh::boundaryVertices(m);
    const std::vector<Vec3> start = m.positions;
    std::vector<Vec3> next(m.positions.size());
    const double mu = taubin ? 1.0 / (0.1 - 1.0 / strength) : 0;
    auto pass = [&](double factor) {
        for (std::size_t v = 0; v < m.positions.size(); ++v) {
            const std::size_t c = nbr.count(v);
            if (c == 0 || boundary[v]) { next[v] = m.positions[v]; continue; }
            Vec3 avg;
            for (auto i = nbr.begin(v); i != nbr.end(v); ++i) avg += m.positions[*i];
            next[v] = m.positions[v] + (avg / static_cast<double>(c) - m.positions[v]) * (factor * (1 - maskAt(mask, v)));
        }
        m.positions.swap(next);
    };
    for (int i = 0; i < iterations; ++i) {
        pass(strength);
        if (taubin) pass(mu);
    }
    double most = 0;
    for (std::size_t v = 0; v < start.size(); ++v) most = std::max(most, mesh::length(m.positions[v] - start[v]));
    return most;
}

// ---- subdivision ----

Mesh subdivide(const Mesh& m, bool loop, Mask* mask) {
    if (m.empty()) throw std::invalid_argument("the mesh has no triangles to subdivide");
    if (m.triangleCount() * 4 > kMaxTriangles) {
        throw std::length_error("subdividing " + std::to_string(m.triangleCount()) + " triangles would make " + std::to_string(m.triangleCount() * 4) +
                                ", over the limit of " + std::to_string(kMaxTriangles) + "; decimate or remesh lower first");
    }
    const std::size_t nv = m.positions.size(), nt = m.triangleCount();
    struct Edge {
        std::uint32_t mid = 0;
        int faces = 0;
        Vec3 opposite;  // sum of the vertices opposite the edge
    };
    std::unordered_map<std::uint64_t, Edge> edges;
    edges.reserve(nt * 2);
    for (std::size_t t = 0; t < nt; ++t) {
        for (std::size_t k = 0; k < 3; ++k) {
            auto& e = edges[edgeKey(m.indices[t * 3 + k], m.indices[t * 3 + (k + 1) % 3])];
            ++e.faces;
            e.opposite += m.positions[m.indices[t * 3 + (k + 2) % 3]];
        }
    }
    Mesh out;
    out.positions.reserve(nv + edges.size());
    const bool keepUv = m.hasUvs();
    // Even (original) vertices.
    if (!loop) {
        out.positions = m.positions;
    } else {
        std::vector<Vec3> ring(nv), bsum(nv);
        std::vector<int> valence(nv, 0), bcount(nv, 0);
        for (const auto& [k, e] : edges) {
            const auto a = static_cast<std::uint32_t>(k >> 32), b = static_cast<std::uint32_t>(k & 0xffffffffu);
            ring[a] += m.positions[b];
            ring[b] += m.positions[a];
            ++valence[a];
            ++valence[b];
            if (e.faces == 1) {
                bsum[a] += m.positions[b];
                bsum[b] += m.positions[a];
                ++bcount[a];
                ++bcount[b];
            }
        }
        out.positions.resize(nv);
        for (std::size_t v = 0; v < nv; ++v) {
            const Vec3& p = m.positions[v];
            if (bcount[v] == 2) {
                out.positions[v] = p * 0.75 + bsum[v] * 0.125;
            } else if (bcount[v] > 0 || valence[v] < 3) {
                out.positions[v] = p;
            } else {
                const int n = valence[v];
                const double beta = n == 3 ? 3.0 / 16.0 : 3.0 / (8.0 * n);
                out.positions[v] = p * (1 - n * beta) + ring[v] * beta;
            }
        }
    }
    if (keepUv) out.uvs = m.uvs;
    Mask newMask;
    const bool hasMask = mask && mask->size() == nv;
    if (hasMask) newMask = *mask;
    // Odd (edge) vertices.
    for (auto& [k, e] : edges) {
        const auto a = static_cast<std::uint32_t>(k >> 32), b = static_cast<std::uint32_t>(k & 0xffffffffu);
        Vec3 p = (m.positions[a] + m.positions[b]) * 0.5;
        if (loop && e.faces == 2) p = (m.positions[a] + m.positions[b]) * 0.375 + e.opposite * 0.125;
        e.mid = out.addVertex(p);
        if (keepUv) out.uvs.push_back((m.uvs[a] + m.uvs[b]) * 0.5);
        if (hasMask) newMask.push_back(((*mask)[a] + (*mask)[b]) * 0.5f);
    }
    out.indices.reserve(nt * 12);
    for (std::size_t t = 0; t < nt; ++t) {
        const std::uint32_t a = m.indices[t * 3], b = m.indices[t * 3 + 1], c = m.indices[t * 3 + 2];
        const std::uint32_t ab = edges[edgeKey(a, b)].mid, bc = edges[edgeKey(b, c)].mid, ca = edges[edgeKey(c, a)].mid;
        out.indices.insert(out.indices.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
    }
    if (hasMask) *mask = std::move(newMask);
    else if (mask) mask->clear();
    return out;
}

// ---- symmetrize ----

Mesh symmetrize(const Mesh& m, int axis, bool positiveToNegative) {
    if (m.empty()) throw std::invalid_argument("the mesh has no triangles");
    if (axis < 0 || axis > 2) throw std::invalid_argument("axis must be x, y or z");
    const double side = positiveToNegative ? 1.0 : -1.0;
    const double eps = mesh::bounds(m).diagonal() * 1e-9;
    std::vector<Vec3> pos = m.positions;
    for (auto& p : pos) {
        if (std::fabs(p[axis]) <= eps) p[axis] = 0;  // on the plane exactly, so the seam welds
    }
    auto s = [&](const Vec3& p) { return p[axis] * side; };
    Mesh half;
    std::unordered_map<std::uint64_t, std::uint32_t> cuts;
    auto cut = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint64_t key = edgeKey(a, b);
        auto it = cuts.find(key);
        if (it != cuts.end()) return it->second;
        const std::uint32_t lo = std::min(a, b), hi = std::max(a, b);
        const double t = s(pos[lo]) / (s(pos[lo]) - s(pos[hi]));
        Vec3 p = mesh::lerp(pos[lo], pos[hi], t);
        p[axis] = 0;
        const std::uint32_t id = half.addVertex(p);
        cuts.emplace(key, id);
        return id;
    };
    std::vector<std::uint32_t> keep(pos.size(), UINT32_MAX);
    auto kept = [&](std::uint32_t v) {
        if (keep[v] == UINT32_MAX) keep[v] = half.addVertex(pos[v]);
        return keep[v];
    };
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const std::uint32_t f[3] = {m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2]};
        if (s(pos[f[0]]) == 0 && s(pos[f[1]]) == 0 && s(pos[f[2]]) == 0) continue;  // lies in the mirror plane
        std::vector<std::uint32_t> poly;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = f[k], b = f[(k + 1) % 3];
            const double sa = s(pos[a]), sb = s(pos[b]);
            if (sa >= 0) poly.push_back(kept(a));
            if ((sa > 0 && sb < 0) || (sa < 0 && sb > 0)) poly.push_back(cut(a, b));
        }
        for (std::size_t k = 1; k + 1 < poly.size(); ++k) half.addTriangle(poly[0], poly[k], poly[k + 1]);
    }
    if (half.empty()) throw std::invalid_argument("nothing on that side of the mirror plane to copy");
    Mesh other = half;
    for (auto& p : other.positions) p[axis] = -p[axis];
    mesh::flip(other);
    mesh::append(half, other);
    mesh::weld(half, 0);
    half.normals.clear();
    half.uvs.clear();
    return half;
}

// ---- displacement ----

std::optional<Mapping> parseMapping(const std::string& n) {
    if (n == "auto") return Mapping::Auto;
    if (n == "uv") return Mapping::Uv;
    if (n == "planar") return Mapping::Planar;
    if (n == "cylindrical") return Mapping::Cylindrical;
    if (n == "spherical") return Mapping::Spherical;
    return std::nullopt;
}

namespace {

double luma(const raster::Image& img, double u, double v) {
    u -= std::floor(u);
    v -= std::floor(v);
    const double x = u * (img.width() - 1), y = v * (img.height() - 1);
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, img.width() - 1), y1 = std::min(y0 + 1, img.height() - 1);
    const double fx = x - x0, fy = y - y0;
    auto l = [&](int px, int py) {
        const std::uint8_t* p = img.pixel(px, py);
        return (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0;
    };
    return (l(x0, y0) * (1 - fx) + l(x1, y0) * fx) * (1 - fy) + (l(x0, y1) * (1 - fx) + l(x1, y1) * fx) * fy;
}

}  // namespace

double displace(Mesh& m, const Mask& mask, const raster::Image& img, const DisplaceSettings& s) {
    if (m.empty()) throw std::invalid_argument("the mesh has no triangles");
    if (img.empty()) throw std::invalid_argument("the heightmap is empty");
    Mapping map = s.mapping;
    if (map == Mapping::Auto) map = m.hasUvs() ? Mapping::Uv : Mapping::Planar;
    if (map == Mapping::Uv && !m.hasUvs()) throw std::invalid_argument("this mesh has no UVs; use mapping=planar, cylindrical or spherical");
    const int a = std::clamp(s.axis, 0, 2), b = (a + 1) % 3, c = (a + 2) % 3;
    const mesh::Bounds bb = mesh::bounds(m);
    const Vec3 lo = bb.min, size = bb.size(), centre = bb.center();
    const std::vector<Vec3> n = mesh::vertexNormals(m);
    double most = 0;
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        const Vec3& p = m.positions[v];
        double u = 0, w = 0;
        switch (map) {
            case Mapping::Uv: u = m.uvs[v].x; w = 1 - m.uvs[v].y; break;
            case Mapping::Planar:
                u = size[b] > 0 ? (p[b] - lo[b]) / size[b] : 0.5;
                w = size[c] > 0 ? 1 - (p[c] - lo[c]) / size[c] : 0.5;
                break;
            case Mapping::Cylindrical:
                u = std::atan2(p[c] - centre[c], p[b] - centre[b]) / (2 * mesh::kPi) + 0.5;
                w = size[a] > 0 ? 1 - (p[a] - lo[a]) / size[a] : 0.5;
                break;
            case Mapping::Spherical:
            case Mapping::Auto: {
                const Vec3 d = mesh::normalize(p - centre);
                u = std::atan2(d[c], d[b]) / (2 * mesh::kPi) + 0.5;
                w = std::acos(std::clamp(d[a], -1.0, 1.0)) / mesh::kPi;
                break;
            }
        }
        // Clamp the far edge so u = 1 samples the last column instead of wrapping to the first.
        const double tu = std::min(u * s.tile, s.tile - 1e-9), tw = std::min(w * s.tile, s.tile - 1e-9);
        const double off = s.amount * (luma(img, tu, tw) - s.midlevel) * (1 - maskAt(mask, v));
        m.positions[v] += n[v] * off;
        most = std::max(most, std::fabs(off));
    }
    return most;
}

// ---- masks ----

std::optional<MaskMode> parseMaskMode(const std::string& n) {
    if (n == "set" || n == "replace") return MaskMode::Set;
    if (n == "add") return MaskMode::Add;
    if (n == "subtract") return MaskMode::Subtract;
    if (n == "intersect") return MaskMode::Intersect;
    return std::nullopt;
}

namespace {

void combine(Mask& mask, std::size_t n, const std::vector<float>& value, MaskMode mode) {
    mask.resize(n, 0.f);
    for (std::size_t v = 0; v < n; ++v) {
        float& m = mask[v];
        switch (mode) {
            case MaskMode::Set: m = value[v]; break;
            case MaskMode::Add: m = std::min(1.f, m + value[v]); break;
            case MaskMode::Subtract: m = std::max(0.f, m - value[v]); break;
            case MaskMode::Intersect: m = std::min(m, value[v]); break;
        }
    }
}

}  // namespace

void maskSphere(const Mesh& m, Mask& mask, const Vec3& center, double radius, double softness, MaskMode mode) {
    std::vector<float> val(m.positions.size());
    for (std::size_t v = 0; v < val.size(); ++v) {
        const double d = mesh::length(m.positions[v] - center);
        val[v] = d <= radius ? 1.f : softness > 0 ? static_cast<float>(std::max(0.0, 1 - (d - radius) / softness)) : 0.f;
    }
    combine(mask, val.size(), val, mode);
}

void maskPlane(const Mesh& m, Mask& mask, const Vec3& point, const Vec3& normal, double softness, MaskMode mode) {
    const Vec3 n = mesh::normalize(normal);
    if (mesh::length(n) == 0) throw std::invalid_argument("the plane normal can't be 0,0,0");
    std::vector<float> val(m.positions.size());
    for (std::size_t v = 0; v < val.size(); ++v) {
        const double s = mesh::dot(m.positions[v] - point, n);
        val[v] = softness > 0 ? static_cast<float>(std::clamp(0.5 + s / softness, 0.0, 1.0)) : (s >= 0 ? 1.f : 0.f);
    }
    combine(mask, val.size(), val, mode);
}

void growMask(const Mesh& m, Mask& mask, int steps) {
    mask.resize(m.positions.size(), 0.f);
    const mesh::Adjacency nbr = mesh::vertexNeighbors(m);
    const bool grow = steps > 0;
    for (int s = 0; s < std::abs(steps); ++s) {
        Mask next = mask;
        for (std::size_t v = 0; v < mask.size(); ++v) {
            for (auto i = nbr.begin(v); i != nbr.end(v); ++i) next[v] = grow ? std::max(next[v], mask[*i]) : std::min(next[v], mask[*i]);
        }
        mask.swap(next);
    }
}

void blurMask(const Mesh& m, Mask& mask, int iterations) {
    mask.resize(m.positions.size(), 0.f);
    const mesh::Adjacency nbr = mesh::vertexNeighbors(m);
    for (int s = 0; s < iterations; ++s) {
        Mask next = mask;
        for (std::size_t v = 0; v < mask.size(); ++v) {
            float sum = mask[v];
            for (auto i = nbr.begin(v); i != nbr.end(v); ++i) sum += mask[*i];
            next[v] = sum / static_cast<float>(nbr.count(v) + 1);
        }
        mask.swap(next);
    }
}

// ---- stats ----

Stats stats(const Mesh& m) {
    Stats s;
    s.triangles = m.triangleCount();
    s.vertices = m.vertexCount();
    if (m.empty()) return s;
    std::unordered_map<std::uint64_t, int> edges;
    edges.reserve(m.indices.size());
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        for (std::size_t k = 0; k < 3; ++k) ++edges[edgeKey(m.indices[t * 3 + k], m.indices[t * 3 + (k + 1) % 3])];
    }
    s.edges = edges.size();
    s.minEdge = 1e300;
    double sum = 0;
    for (const auto& [k, n] : edges) {
        if (n == 1) ++s.openEdges;
        if (n > 2) ++s.nonManifoldEdges;
        const double len = mesh::length(m.positions[static_cast<std::size_t>(k >> 32)] - m.positions[static_cast<std::size_t>(k & 0xffffffffu)]);
        s.minEdge = std::min(s.minEdge, len);
        s.maxEdge = std::max(s.maxEdge, len);
        sum += len;
    }
    s.meanEdge = sum / static_cast<double>(edges.size());
    s.area = mesh::surfaceArea(m);
    s.watertight = mesh::isWatertight(m);
    s.volume = s.watertight ? mesh::volume(m) : 0;
    s.euler = static_cast<long long>(s.vertices) - static_cast<long long>(s.edges) + static_cast<long long>(s.triangles);
    return s;
}

}  // namespace wetwaresuite::sculpt
