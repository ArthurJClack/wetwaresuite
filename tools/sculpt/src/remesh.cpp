// Voxel remesh (signed distance on a grid + surface nets) and retopology.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "wetwaresuite/mesh/topology.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::sculpt {

namespace {

// Drops vertices no triangle uses, keeping triangle order (and so quad pairs).
void dropUnused(Mesh& m) {
    std::vector<std::uint32_t> remap(m.positions.size(), UINT32_MAX);
    std::vector<Vec3> pos;
    pos.reserve(m.positions.size());
    for (auto& i : m.indices) {
        if (remap[i] == UINT32_MAX) {
            remap[i] = static_cast<std::uint32_t>(pos.size());
            pos.push_back(m.positions[i]);
        }
        i = remap[i];
    }
    m.positions = std::move(pos);
    m.normals.clear();
    m.uvs.clear();
}

struct Grid {
    Vec3 origin;
    double h = 1;
    int nx = 0, ny = 0, nz = 0;  // sample points per axis
    std::vector<float> f;        // signed distance, negative inside

    std::size_t at(int i, int j, int k) const {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(j)) * static_cast<std::size_t>(nx) +
               static_cast<std::size_t>(i);
    }
    Vec3 point(int i, int j, int k) const { return origin + Vec3{i * h, j * h, k * h}; }
};

Grid sampleDistance(const Mesh& m, int resolution, bool shell) {
    const mesh::Bounds b = mesh::bounds(m);
    const Vec3 size = b.size();
    const double longest = std::max({size.x, size.y, size.z});
    if (!(longest > 0)) throw std::invalid_argument("the mesh is flat in every direction; nothing to remesh");
    Grid g;
    g.h = longest / resolution;
    const int pad = 3;
    // Off by a fraction of a cell, so flat faces at the bounds (very common) don't sit exactly on sample points.
    const double shift = (pad + 0.371) * g.h;
    g.origin = b.min - Vec3{shift, shift, shift};
    g.nx = static_cast<int>(std::ceil(size.x / g.h)) + 2 * pad + 2;
    g.ny = static_cast<int>(std::ceil(size.y / g.h)) + 2 * pad + 2;
    g.nz = static_cast<int>(std::ceil(size.z / g.h)) + 2 * pad + 2;
    const float band = static_cast<float>(3 * g.h);
    g.f.assign(static_cast<std::size_t>(g.nx) * static_cast<std::size_t>(g.ny) * static_cast<std::size_t>(g.nz), band);

    // Unsigned distance in a narrow band around each triangle.
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const Vec3 &a = m.corner(t, 0), &bb = m.corner(t, 1), &c = m.corner(t, 2);
        const Vec3 lo = mesh::min(a, mesh::min(bb, c)) - Vec3{2 * g.h, 2 * g.h, 2 * g.h};
        const Vec3 hi = mesh::max(a, mesh::max(bb, c)) + Vec3{2 * g.h, 2 * g.h, 2 * g.h};
        const int i0 = std::max(0, static_cast<int>(std::floor((lo.x - g.origin.x) / g.h)));
        const int j0 = std::max(0, static_cast<int>(std::floor((lo.y - g.origin.y) / g.h)));
        const int k0 = std::max(0, static_cast<int>(std::floor((lo.z - g.origin.z) / g.h)));
        const int i1 = std::min(g.nx - 1, static_cast<int>(std::ceil((hi.x - g.origin.x) / g.h)));
        const int j1 = std::min(g.ny - 1, static_cast<int>(std::ceil((hi.y - g.origin.y) / g.h)));
        const int k1 = std::min(g.nz - 1, static_cast<int>(std::ceil((hi.z - g.origin.z) / g.h)));
        for (int k = k0; k <= k1; ++k) {
            for (int j = j0; j <= j1; ++j) {
                for (int i = i0; i <= i1; ++i) {
                    const Vec3 p = g.point(i, j, k);
                    const float d = static_cast<float>(mesh::length(mesh::closestPointOnTriangle(p, a, bb, c) - p));
                    float& cell = g.f[g.at(i, j, k)];
                    if (d < cell) cell = d;
                }
            }
        }
    }

    if (shell) {
        const float half = static_cast<float>(0.75 * g.h);
        for (auto& v : g.f) v -= half;
        return g;
    }

    // Sign: winding number along +Z columns (entering through a downward face counts +1).
    const float tiny = static_cast<float>(g.h * 1e-6);
    const double jx = g.h * 0.000123456, jy = g.h * 0.000271828;
    std::vector<std::vector<std::pair<float, int>>> hits(static_cast<std::size_t>(g.nx) * static_cast<std::size_t>(g.ny));
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const Vec3 &a = m.corner(t, 0), &bb = m.corner(t, 1), &c = m.corner(t, 2);
        const double area2 = (bb.x - a.x) * (c.y - a.y) - (bb.y - a.y) * (c.x - a.x);
        if (area2 == 0) continue;
        const int dir = area2 > 0 ? -1 : 1;  // CCW from above = normal up = leaving
        const double lx = std::min({a.x, bb.x, c.x}), hx = std::max({a.x, bb.x, c.x});
        const double ly = std::min({a.y, bb.y, c.y}), hy = std::max({a.y, bb.y, c.y});
        const int i0 = std::max(0, static_cast<int>(std::ceil((lx - g.origin.x - jx) / g.h)));
        const int i1 = std::min(g.nx - 1, static_cast<int>(std::floor((hx - g.origin.x - jx) / g.h)));
        const int j0 = std::max(0, static_cast<int>(std::ceil((ly - g.origin.y - jy) / g.h)));
        const int j1 = std::min(g.ny - 1, static_cast<int>(std::floor((hy - g.origin.y - jy) / g.h)));
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                const double px = g.origin.x + i * g.h + jx, py = g.origin.y + j * g.h + jy;
                const double w0 = ((bb.x - px) * (c.y - py) - (bb.y - py) * (c.x - px)) / area2;
                const double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area2;
                const double w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const double z = w0 * a.z + w1 * bb.z + w2 * c.z;
                hits[static_cast<std::size_t>(j) * static_cast<std::size_t>(g.nx) + static_cast<std::size_t>(i)].push_back({static_cast<float>(z), dir});
            }
        }
    }
    for (int j = 0; j < g.ny; ++j) {
        for (int i = 0; i < g.nx; ++i) {
            auto& col = hits[static_cast<std::size_t>(j) * static_cast<std::size_t>(g.nx) + static_cast<std::size_t>(i)];
            if (col.empty()) continue;
            std::sort(col.begin(), col.end());
            std::size_t h = 0;
            int winding = 0;
            for (int k = 0; k < g.nz; ++k) {
                const double z = g.origin.z + k * g.h;
                while (h < col.size() && col[h].first <= z) winding += col[h++].second;
                if (winding > 0) {
                    float& v = g.f[g.at(i, j, k)];
                    v = -std::max(v, tiny);  // a point exactly on an inner face is still inside
                }
            }
            col.clear();
            col.shrink_to_fit();
        }
    }
    return g;
}

Mesh surfaceNets(const Grid& g) {
    const int cx = g.nx - 1, cy = g.ny - 1, cz = g.nz - 1;
    auto cellAt = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(cy) + static_cast<std::size_t>(j)) * static_cast<std::size_t>(cx) +
               static_cast<std::size_t>(i);
    };
    std::vector<std::int32_t> cellVert(static_cast<std::size_t>(cx) * static_cast<std::size_t>(cy) * static_cast<std::size_t>(cz), -1);
    Mesh out;
    static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int edge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int k = 0; k < cz; ++k) {
        for (int j = 0; j < cy; ++j) {
            for (int i = 0; i < cx; ++i) {
                float v[8];
                int inside = 0;
                for (int c = 0; c < 8; ++c) {
                    v[c] = g.f[g.at(i + corner[c][0], j + corner[c][1], k + corner[c][2])];
                    inside += v[c] < 0 ? 1 : 0;
                }
                if (inside == 0 || inside == 8) continue;
                Vec3 sum;
                int n = 0;
                for (const auto& e : edge) {
                    const float a = v[e[0]], b = v[e[1]];
                    if ((a < 0) == (b < 0)) continue;
                    const double t = a / static_cast<double>(a - b);
                    const Vec3 pa{static_cast<double>(corner[e[0]][0]), static_cast<double>(corner[e[0]][1]), static_cast<double>(corner[e[0]][2])};
                    const Vec3 pb{static_cast<double>(corner[e[1]][0]), static_cast<double>(corner[e[1]][1]), static_cast<double>(corner[e[1]][2])};
                    sum += mesh::lerp(pa, pb, t);
                    ++n;
                }
                const Vec3 local = sum / n;
                cellVert[cellAt(i, j, k)] = static_cast<std::int32_t>(out.addVertex(g.point(i, j, k) + local * g.h));
            }
        }
    }
    // One quad per sign-changing grid edge, joining the four cells around it.
    const int dims[3] = {g.nx, g.ny, g.nz};
    for (int k = 1; k < g.nz - 1; ++k) {
        for (int j = 1; j < g.ny - 1; ++j) {
            for (int i = 1; i < g.nx - 1; ++i) {
                const float f0 = g.f[g.at(i, j, k)];
                const int p[3] = {i, j, k};
                for (int a = 0; a < 3; ++a) {
                    if (p[a] + 1 >= dims[a]) continue;
                    int q[3] = {i, j, k};
                    ++q[a];
                    const float f1 = g.f[g.at(q[0], q[1], q[2])];
                    if ((f0 < 0) == (f1 < 0)) continue;
                    const int b = (a + 1) % 3, c = (a + 2) % 3;
                    static const int off[4][2] = {{-1, -1}, {0, -1}, {0, 0}, {-1, 0}};
                    std::uint32_t quad[4];
                    bool ok = true;
                    for (int s = 0; s < 4; ++s) {
                        int cell[3] = {i, j, k};
                        cell[b] += off[s][0];
                        cell[c] += off[s][1];
                        const std::int32_t vi = cellVert[cellAt(cell[0], cell[1], cell[2])];
                        if (vi < 0) { ok = false; break; }
                        quad[s] = static_cast<std::uint32_t>(vi);
                    }
                    if (!ok) continue;
                    if (!(f0 < 0)) std::swap(quad[1], quad[3]);
                    const Vec3 &q0 = out.positions[quad[0]], &q1 = out.positions[quad[1]], &q2 = out.positions[quad[2]], &q3 = out.positions[quad[3]];
                    if (mesh::length(q0 - q2) <= mesh::length(q1 - q3)) {
                        out.addTriangle(quad[0], quad[1], quad[2]);
                        out.addTriangle(quad[0], quad[2], quad[3]);
                    } else {
                        out.addTriangle(quad[0], quad[1], quad[3]);
                        out.addTriangle(quad[1], quad[2], quad[3]);
                    }
                    if (out.triangleCount() > kMaxTriangles) {
                        throw std::length_error("the remesh would pass " + std::to_string(kMaxTriangles) + " triangles; use a lower resolution");
                    }
                }
            }
        }
    }
    dropUnused(out);
    return out;
}

// Triangles bucketed on a uniform grid, for closest-point queries near the surface.
class TriangleGrid {
public:
    TriangleGrid(const Mesh& m, double cell) : m_(m), cell_(cell) {
        const mesh::Bounds b = mesh::bounds(m);
        origin_ = b.min - Vec3{cell, cell, cell};
        const Vec3 s = b.size();
        n_[0] = static_cast<int>(s.x / cell) + 3;
        n_[1] = static_cast<int>(s.y / cell) + 3;
        n_[2] = static_cast<int>(s.z / cell) + 3;
        const std::size_t cells = static_cast<std::size_t>(n_[0]) * static_cast<std::size_t>(n_[1]) * static_cast<std::size_t>(n_[2]);
        start_.assign(cells + 1, 0);
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<std::uint32_t> fill;
            if (pass == 1) {
                for (std::size_t c = 0; c < cells; ++c) start_[c + 1] += start_[c];
                items_.resize(start_[cells]);
                fill.assign(start_.begin(), start_.end() - 1);
            }
            for (std::size_t t = 0; t < m.triangleCount(); ++t) {
                int lo[3], hi[3];
                range(mesh::min(m.corner(t, 0), mesh::min(m.corner(t, 1), m.corner(t, 2))), lo);
                range(mesh::max(m.corner(t, 0), mesh::max(m.corner(t, 1), m.corner(t, 2))), hi);
                for (int z = lo[2]; z <= hi[2]; ++z)
                    for (int y = lo[1]; y <= hi[1]; ++y)
                        for (int x = lo[0]; x <= hi[0]; ++x) {
                            const std::size_t c = index(x, y, z);
                            if (pass == 0) ++start_[c + 1];
                            else items_[fill[c]++] = static_cast<std::uint32_t>(t);
                        }
            }
        }
    }

    // Closest surface point within about one cell, or p itself if none is near.
    Vec3 closest(const Vec3& p) const {
        int c[3];
        range(p, c);
        double best = std::numeric_limits<double>::infinity();
        Vec3 out = p;
        for (int z = std::max(0, c[2] - 1); z <= std::min(n_[2] - 1, c[2] + 1); ++z)
            for (int y = std::max(0, c[1] - 1); y <= std::min(n_[1] - 1, c[1] + 1); ++y)
                for (int x = std::max(0, c[0] - 1); x <= std::min(n_[0] - 1, c[0] + 1); ++x) {
                    const std::size_t ci = index(x, y, z);
                    for (std::uint32_t k = start_[ci]; k < start_[ci + 1]; ++k) {
                        const std::uint32_t t = items_[k];
                        const Vec3 q = mesh::closestPointOnTriangle(p, m_.corner(t, 0), m_.corner(t, 1), m_.corner(t, 2));
                        const Vec3 d = q - p;
                        const double d2 = mesh::dot(d, d);
                        if (d2 < best) { best = d2; out = q; }
                    }
                }
        return out;
    }

private:
    void range(const Vec3& p, int out[3]) const {
        for (int a = 0; a < 3; ++a) out[a] = std::clamp(static_cast<int>(std::floor((p[a] - origin_[a]) / cell_)), 0, n_[a] - 1);
    }
    std::size_t index(int x, int y, int z) const {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(n_[1]) + static_cast<std::size_t>(y)) * static_cast<std::size_t>(n_[0]) +
               static_cast<std::size_t>(x);
    }

    const Mesh& m_;
    double cell_;
    Vec3 origin_;
    int n_[3] = {1, 1, 1};
    std::vector<std::uint32_t> start_, items_;
};

}  // namespace

Mesh voxelRemesh(const Mesh& m, int resolution, bool* shellOut) {
    if (m.empty()) throw std::invalid_argument("the mesh has no triangles to remesh");
    if (resolution < 4) throw std::invalid_argument("resolution must be at least 4 cells");
    if (resolution > kMaxResolution) {
        throw std::length_error("resolution " + std::to_string(resolution) + " is over the limit of " + std::to_string(kMaxResolution) +
                                " cells (memory); remesh at " + std::to_string(kMaxResolution) + " or less and subdivide afterwards if you need more");
    }
    const bool shell = mesh::boundaryEdgeCount(m) > 0;
    if (shellOut) *shellOut = shell;
    const Grid g = sampleDistance(m, resolution, shell);
    Mesh out = surfaceNets(g);
    if (out.empty()) throw std::runtime_error("the remesh found no surface; try a higher resolution");
    return out;
}

Mesh retopologize(const Mesh& m, std::size_t targetQuads, int relax) {
    if (m.empty()) throw std::invalid_argument("the mesh has no triangles to retopologize");
    if (targetQuads < 6) throw std::invalid_argument("aim for at least 6 quads");
    if (targetQuads * 2 > kMaxTriangles) throw std::length_error("at most " + std::to_string(kMaxTriangles / 2) + " quads");
    const bool shell = mesh::boundaryEdgeCount(m) > 0;
    const double area = mesh::surfaceArea(m) * (shell ? 2 : 1);
    // Surface nets give about 1.5 quads per h^2 of area (cells meet the surface at angles).
    const double h = std::sqrt(1.5 * area / static_cast<double>(targetQuads));
    const Vec3 size = mesh::bounds(m).size();
    const double longest = std::max({size.x, size.y, size.z});
    const int res = std::clamp(static_cast<int>(std::lround(longest / h)), 4, kMaxResolution);
    Mesh out = voxelRemesh(m, res, nullptr);
    if (relax <= 0) return out;
    const double cell = longest / res * 2;
    const TriangleGrid grid(m, cell);
    const mesh::Adjacency nbr = mesh::vertexNeighbors(out);
    std::vector<Vec3> next(out.positions.size());
    for (int it = 0; it < relax; ++it) {
        const std::vector<Vec3> n = mesh::vertexNormals(out);
        for (std::size_t v = 0; v < out.positions.size(); ++v) {
            const std::size_t c = nbr.count(v);
            if (c == 0) { next[v] = out.positions[v]; continue; }
            Vec3 avg;
            for (auto i = nbr.begin(v); i != nbr.end(v); ++i) avg += out.positions[*i];
            Vec3 d = avg / static_cast<double>(c) - out.positions[v];
            d -= n[v] * mesh::dot(d, n[v]);  // tangential only, so it slides instead of shrinking
            next[v] = out.positions[v] + d * 0.5;
            if (!shell) next[v] = grid.closest(next[v]);
        }
        out.positions.swap(next);
    }
    return out;
}

}  // namespace wetwaresuite::sculpt
