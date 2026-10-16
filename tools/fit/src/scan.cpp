// Reading scans, cleaning them up, aligning and cross-sections.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "wetwaresuite/fit/geom.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/ops.hpp"
#include "wetwaresuite/mesh/topology.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::fit {

namespace fs = std::filesystem;
using mesh::cross;
using mesh::dot;
using mesh::length;
using mesh::normalize;

namespace {

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("can't read " + p.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct UnionFind {
    std::vector<std::uint32_t> parent;
    explicit UnionFind(std::size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
    std::uint32_t find(std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    }
    void join(std::uint32_t a, std::uint32_t b) { parent[find(a)] = find(b); }
};

Vec3 triCentre(const Mesh& m, std::size_t t) { return (m.corner(t, 0) + m.corner(t, 1) + m.corner(t, 2)) / 3.0; }

}  // namespace

// ---- reading ----

std::vector<Vec3> readPoints(const std::string& text) {
    std::vector<Vec3> pts;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        std::size_t end = text.find('\n', i);
        if (end == std::string::npos) end = n;
        std::string line = text.substr(i, end - i);
        i = end + 1;
        for (char& c : line) {
            if (c == ',' || c == ';' || c == '\t' || c == '\r') c = ' ';
        }
        const char* s = line.c_str();
        double v[3];
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            char* after = nullptr;
            v[k] = std::strtod(s, &after);
            ok = after != s && std::isfinite(v[k]);
            s = after;
        }
        if (ok) pts.push_back({v[0], v[1], v[2]});
    }
    return pts;
}

bool isPointFile(const fs::path& path) {
    const std::string e = lowerExt(path);
    return e == "xyz" || e == "pts" || e == "txt" || e == "csv" || e == "asc";
}

Mesh pointsToSurface(const std::vector<Vec3>& points, double cell) {
    if (points.empty()) throw std::runtime_error("the point cloud has no points");
    Bounds b;
    for (const auto& p : points) b.add(p);
    const Vec3 sz = b.size();
    const double longest = std::max({sz.x, sz.y, sz.z});
    if (cell <= 0) cell = std::max(longest / 200.0, 1e-6);
    if (longest / cell > kMaxCloudResolution) {
        throw std::length_error("cell " + std::to_string(cell) + " gives more than " + std::to_string(kMaxCloudResolution) +
                                " voxels along the longest side; use a bigger cell");
    }
    // pad by 2 so the closing step never touches the border
    const int nx = static_cast<int>(sz.x / cell) + 5, ny = static_cast<int>(sz.y / cell) + 5, nz = static_cast<int>(sz.z / cell) + 5;
    const Vec3 base = b.min - Vec3{2 * cell, 2 * cell, 2 * cell};
    auto idx = [&](int x, int y, int z) { return (static_cast<std::size_t>(z) * ny + y) * nx + x; };
    std::vector<std::uint8_t> occ(static_cast<std::size_t>(nx) * ny * nz, 0);
    for (const auto& p : points) {
        const int x = std::clamp(static_cast<int>((p.x - base.x) / cell), 0, nx - 1);
        const int y = std::clamp(static_cast<int>((p.y - base.y) / cell), 0, ny - 1);
        const int z = std::clamp(static_cast<int>((p.z - base.z) / cell), 0, nz - 1);
        occ[idx(x, y, z)] = 1;
    }
    // close small gaps between scan points: dilate then erode (6-neighbour)
    auto morph = [&](bool dilate) {
        std::vector<std::uint8_t> out = occ;
        for (int z = 1; z < nz - 1; ++z)
            for (int y = 1; y < ny - 1; ++y)
                for (int x = 1; x < nx - 1; ++x) {
                    const std::uint8_t c = occ[idx(x, y, z)];
                    const bool any = occ[idx(x - 1, y, z)] || occ[idx(x + 1, y, z)] || occ[idx(x, y - 1, z)] ||
                                     occ[idx(x, y + 1, z)] || occ[idx(x, y, z - 1)] || occ[idx(x, y, z + 1)];
                    const bool all = occ[idx(x - 1, y, z)] && occ[idx(x + 1, y, z)] && occ[idx(x, y - 1, z)] &&
                                     occ[idx(x, y + 1, z)] && occ[idx(x, y, z - 1)] && occ[idx(x, y, z + 1)];
                    out[idx(x, y, z)] = dilate ? (c || any) : (c && all);
                }
        occ.swap(out);
    };
    morph(true);
    morph(false);
    // faces between filled and empty cells, sharing lattice corners
    Mesh m;
    std::unordered_map<std::size_t, std::uint32_t> corner;
    auto vert = [&](int x, int y, int z) {
        const std::size_t key = (static_cast<std::size_t>(z) * (ny + 1) + y) * (nx + 1) + x;
        auto it = corner.find(key);
        if (it != corner.end()) return it->second;
        const std::uint32_t v = m.addVertex(base + Vec3{x * cell, y * cell, z * cell});
        corner.emplace(key, v);
        return v;
    };
    auto quad = [&](std::uint32_t a, std::uint32_t b2, std::uint32_t c, std::uint32_t d) {
        m.addTriangle(a, b2, c);
        m.addTriangle(a, c, d);
    };
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                if (!occ[idx(x, y, z)]) continue;
                auto empty = [&](int X, int Y, int Z) {
                    return X < 0 || Y < 0 || Z < 0 || X >= nx || Y >= ny || Z >= nz || !occ[idx(X, Y, Z)];
                };
                if (empty(x - 1, y, z)) quad(vert(x, y, z), vert(x, y, z + 1), vert(x, y + 1, z + 1), vert(x, y + 1, z));
                if (empty(x + 1, y, z)) quad(vert(x + 1, y, z), vert(x + 1, y + 1, z), vert(x + 1, y + 1, z + 1), vert(x + 1, y, z + 1));
                if (empty(x, y - 1, z)) quad(vert(x, y, z), vert(x + 1, y, z), vert(x + 1, y, z + 1), vert(x, y, z + 1));
                if (empty(x, y + 1, z)) quad(vert(x, y + 1, z), vert(x, y + 1, z + 1), vert(x + 1, y + 1, z + 1), vert(x + 1, y + 1, z));
                if (empty(x, y, z - 1)) quad(vert(x, y, z), vert(x, y + 1, z), vert(x + 1, y + 1, z), vert(x + 1, y, z));
                if (empty(x, y, z + 1)) quad(vert(x, y, z + 1), vert(x + 1, y, z + 1), vert(x + 1, y + 1, z + 1), vert(x, y + 1, z + 1));
            }
    if (m.empty()) throw std::runtime_error("the point cloud didn't make a surface");
    sculpt::smooth(m, {}, 8, 0.6, true);
    return m;
}

double unitScale(const std::string& units) {
    if (units == "mm") return 1;
    if (units == "cm") return 10;
    if (units == "m") return 1000;
    if (units == "in") return 25.4;
    return 0;
}

Mesh loadScan(const fs::path& path, const ScanOptions& o, ScanInfo& info) {
    if (!fs::exists(path)) throw std::runtime_error("no file at " + path.string());
    const std::string ext = lowerExt(path);
    Mesh m;
    if (isPointFile(path)) {
        const auto pts = readPoints(readFile(path));
        if (pts.size() < 10) throw std::runtime_error(path.string() + " has fewer than 10 points (one x y z per line)");
        info.pointCloud = true;
        info.points = pts.size();
        m.positions = pts;
    } else if (mesh::canRead(path)) {
        m = mesh::load(path);
        if (m.empty()) {
            if (m.positions.size() < 10) throw std::runtime_error(path.string() + " has no triangles and no points");
            info.pointCloud = true;
            info.points = m.positions.size();
        }
    } else {
        throw std::runtime_error("can't open ." + ext +
                                 " scans; export OBJ, PLY, STL, GLB, glTF, OFF or 3MF from the scanner app (or XYZ/PTS points)");
    }
    m.normals.clear();
    m.uvs.clear();
    info.fileTriangles = m.triangleCount();

    // phone scanner apps (ARKit/ARCore) write Y-up; glTF is turned to Z-up on load already
    std::string up = o.up;
    if (up == "auto") up = (ext == "glb" || ext == "gltf" || ext == "3mf" || ext == "stl") ? "z" : "y";
    if (up == "y") {
        info.yUp = true;
        for (auto& p : m.positions) p = {p.x, -p.z, p.y};
    }

    Bounds b;
    for (const auto& p : m.positions) b.add(p);
    double scale = 1;
    if (o.units == "auto") {
        // apps save metres; nothing worth scanning for a car part is under 10 mm across
        if (b.diagonal() < 10) {
            scale = 1000;
            info.units = "m";
        } else {
            info.units = "mm";
        }
    } else {
        scale = unitScale(o.units);
        if (scale <= 0) throw std::runtime_error("units must be auto, mm, cm, m or in");
        info.units = o.units;
    }
    info.scale = scale;
    if (scale != 1) {
        for (auto& p : m.positions) p *= scale;
    }

    if (info.pointCloud) {
        Bounds pb;
        for (const auto& p : m.positions) pb.add(p);
        double cell = o.cloudCell;
        if (cell <= 0) {
            const Vec3 s = pb.size();
            cell = std::max({s.x, s.y, s.z}) / 160.0;
        }
        m = pointsToSurface(m.positions, cell);
    } else {
        mesh::weld(m, 0);
        mesh::cleanup(m);
    }
    if (o.budget > 0 && m.triangleCount() > o.budget) {
        // very big scans: a quick cluster pass first keeps the quadric pass short
        if (m.triangleCount() > o.budget * 4) {
            Mesh c = mesh::decimate(m, o.budget * 2);
            mesh::weld(c, 0);
            mesh::cleanup(c);
            m = std::move(c);
        }
        m = sculpt::decimateQuadric(m, o.budget);
        info.simplified = true;
    }
    if (m.empty()) throw std::runtime_error(path.string() + " has no usable triangles");
    return m;
}

// ---- cleaning ----

void dropTriangles(Mesh& m, const std::vector<bool>& drop) {
    std::vector<std::uint32_t> kept;
    kept.reserve(m.indices.size());
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        if (drop[t]) continue;
        kept.insert(kept.end(), {m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2]});
    }
    m.indices.swap(kept);
    std::vector<std::int64_t> remap(m.positions.size(), -1);
    std::vector<Vec3> pos;
    for (auto& i : m.indices) {
        if (remap[i] < 0) {
            remap[i] = static_cast<std::int64_t>(pos.size());
            pos.push_back(m.positions[i]);
        }
        i = static_cast<std::uint32_t>(remap[i]);
    }
    m.positions.swap(pos);
    m.normals.clear();
    m.uvs.clear();
}

std::vector<std::vector<std::uint32_t>> pieces(const Mesh& m) {
    UnionFind uf(m.vertexCount());
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        uf.join(m.indices[t * 3], m.indices[t * 3 + 1]);
        uf.join(m.indices[t * 3], m.indices[t * 3 + 2]);
    }
    std::unordered_map<std::uint32_t, std::size_t> which;
    std::vector<std::vector<std::uint32_t>> out;
    std::vector<double> area;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const std::uint32_t r = uf.find(m.indices[t * 3]);
        auto it = which.find(r);
        if (it == which.end()) {
            it = which.emplace(r, out.size()).first;
            out.emplace_back();
            area.push_back(0);
        }
        out[it->second].push_back(static_cast<std::uint32_t>(t));
        area[it->second] += length(m.faceNormal(t)) * 0.5;
    }
    std::vector<std::size_t> order(out.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return area[a] > area[b]; });
    std::vector<std::vector<std::uint32_t>> sorted;
    sorted.reserve(out.size());
    for (std::size_t i : order) sorted.push_back(std::move(out[i]));
    return sorted;
}

std::size_t keepLargest(Mesh& m, int keep, double minFraction) {
    const auto ps = pieces(m);
    if (ps.size() <= 1) return 0;
    auto areaOf = [&](const std::vector<std::uint32_t>& tris) {
        double a = 0;
        for (auto t : tris) a += length(m.faceNormal(t)) * 0.5;
        return a;
    };
    const double biggest = areaOf(ps[0]);
    std::vector<bool> drop(m.triangleCount(), true);
    for (std::size_t i = 0; i < ps.size(); ++i) {
        const bool k = static_cast<int>(i) < std::max(keep, 1) || (minFraction > 0 && areaOf(ps[i]) >= biggest * minFraction);
        if (!k) continue;
        for (auto t : ps[i]) drop[t] = false;
    }
    const auto removed = static_cast<std::size_t>(std::count(drop.begin(), drop.end(), true));
    if (removed) dropTriangles(m, drop);
    return removed;
}

std::size_t cropBox(Mesh& m, const Bounds& box, bool keepInside) {
    std::vector<bool> drop(m.triangleCount());
    std::size_t removed = 0;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        drop[t] = box.contains(triCentre(m, t)) != keepInside;
        removed += drop[t];
    }
    if (removed) dropTriangles(m, drop);
    return removed;
}

std::size_t cropPlane(Mesh& m, const Vec3& point, const Vec3& normal) {
    const Vec3 n = normalize(normal);
    std::vector<bool> drop(m.triangleCount());
    std::size_t removed = 0;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        drop[t] = dot(triCentre(m, t) - point, n) < 0;
        removed += drop[t];
    }
    if (removed) dropTriangles(m, drop);
    return removed;
}

FloorResult removeFloor(Mesh& m, double tolerance) {
    FloorResult r;
    if (m.empty()) return r;
    const Bounds b = mesh::bounds(m);
    if (tolerance <= 0) tolerance = std::max(2.0, b.diagonal() * 0.005);
    const std::size_t n = m.triangleCount();
    std::vector<double> area(n);
    std::vector<bool> flat(n);
    std::vector<Vec3> fn(n);
    for (std::size_t t = 0; t < n; ++t) {
        const Vec3 f = m.faceNormal(t);
        area[t] = length(f) * 0.5;
        fn[t] = normalize(f);
        flat[t] = std::fabs(fn[t].z) > 0.9;
    }
    // candidate heights: histogram of flat triangle heights, weighted by area
    const double bin = tolerance;
    std::map<long long, double> hist;
    for (std::size_t t = 0; t < n; ++t) {
        if (flat[t]) hist[static_cast<long long>(std::floor(triCentre(m, t).z / bin))] += area[t];
    }
    if (hist.empty()) return r;
    double best = 0;
    for (const auto& [k, a] : hist) best = std::max(best, a + (hist.count(k + 1) ? hist[k + 1] : 0));
    // the lowest height bin that holds at least half the best flat area
    long long pick = hist.begin()->first;
    for (const auto& [k, a] : hist) {
        if (a + (hist.count(k + 1) ? hist[k + 1] : 0) >= best * 0.5) {
            pick = k;
            break;
        }
    }
    // refine the height as the area-weighted mean of flat triangles in the band
    double zs = 0, ws = 0;
    for (std::size_t t = 0; t < n; ++t) {
        const double z = triCentre(m, t).z;
        if (flat[t] && z >= pick * bin - bin && z <= (pick + 2) * bin + bin) {
            zs += z * area[t];
            ws += area[t];
        }
    }
    if (ws <= 0) return r;
    const double z0 = zs / ws;
    // the floor must be a real share of the scan, not one flat bit of the part
    double total = 0;
    for (double a : area) total += a;
    if (ws < total * 0.05) return r;
    r.found = true;
    r.point = {b.center().x, b.center().y, z0};
    r.normal = {0, 0, 1};
    r.area = ws;
    std::vector<bool> drop(n);
    for (std::size_t t = 0; t < n; ++t) {
        const double z = triCentre(m, t).z;
        drop[t] = z < z0 - tolerance || (std::fabs(z - z0) <= tolerance && flat[t]);
        r.removed += drop[t];
    }
    if (r.removed) dropTriangles(m, drop);
    return r;
}

std::vector<std::vector<std::uint32_t>> boundaryLoops(const Mesh& m) {
    std::unordered_map<std::uint64_t, int> count;
    auto key = [](std::uint32_t a, std::uint32_t b) { return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b); };
    for (std::size_t t = 0; t < m.triangleCount(); ++t)
        for (int k = 0; k < 3; ++k) ++count[key(m.indices[t * 3 + k], m.indices[t * 3 + (k + 1) % 3])];
    // boundary half-edges a -> b as the triangle walks them; the hole runs b -> a
    std::unordered_multimap<std::uint32_t, std::uint32_t> next;
    std::size_t edges = 0;
    for (std::size_t t = 0; t < m.triangleCount(); ++t)
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = m.indices[t * 3 + k], b = m.indices[t * 3 + (k + 1) % 3];
            if (count[key(a, b)] == 1) {
                next.emplace(b, a);
                ++edges;
            }
        }
    std::vector<std::vector<std::uint32_t>> loops;
    while (!next.empty()) {
        auto it = next.begin();
        const std::uint32_t start = it->first;
        std::vector<std::uint32_t> loop{start};
        std::uint32_t cur = it->second;
        next.erase(it);
        bool closed = false;
        for (std::size_t guard = 0; guard <= edges; ++guard) {
            if (cur == start) {
                closed = true;
                break;
            }
            loop.push_back(cur);
            auto nx = next.find(cur);
            if (nx == next.end()) break;
            const std::uint32_t to = nx->second;
            next.erase(nx);
            cur = to;
        }
        if (closed && loop.size() >= 3) loops.push_back(std::move(loop));
    }
    return loops;
}

HoleStats fillHoles(Mesh& m, double maxSize) {
    HoleStats s;
    for (const auto& loop : boundaryLoops(m)) {
        double perimeter = 0;
        Vec3 c;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            perimeter += length(m.positions[loop[(i + 1) % loop.size()]] - m.positions[loop[i]]);
            c += m.positions[loop[i]];
        }
        const double diameter = perimeter / mesh::kPi;
        if (maxSize > 0 && diameter > maxSize) {
            ++s.skipped;
            continue;
        }
        c = c / static_cast<double>(loop.size());
        // loop runs opposite to its triangles' edges, so fan triangles keep the winding
        if (loop.size() == 3) {
            m.addTriangle(loop[0], loop[1], loop[2]);
        } else {
            const std::uint32_t mid = m.addVertex(c);
            for (std::size_t i = 0; i < loop.size(); ++i) m.addTriangle(loop[i], loop[(i + 1) % loop.size()], mid);
        }
        ++s.filled;
        s.largestFilled = std::max(s.largestFilled, diameter);
    }
    m.normals.clear();
    m.uvs.clear();
    return s;
}

double meanEdgeLength(const Mesh& m) {
    if (m.empty()) return 0;
    double sum = 0;
    for (std::size_t t = 0; t < m.triangleCount(); ++t)
        for (int k = 0; k < 3; ++k) sum += length(m.corner(t, (k + 1) % 3) - m.corner(t, k));
    return sum / static_cast<double>(m.triangleCount() * 3);
}

double noiseLevel(const Mesh& m) {
    if (m.empty()) return 0;
    const auto nb = mesh::vertexNeighbors(m);
    const auto vn = mesh::vertexNormals(m);
    const auto boundary = mesh::boundaryVertices(m);
    std::vector<double> d;
    d.reserve(m.vertexCount());
    for (std::size_t v = 0; v < m.vertexCount(); ++v) {
        if (boundary[v] || nb.count(v) < 3) continue;
        Vec3 avg;
        for (auto it = nb.begin(v); it != nb.end(v); ++it) avg += m.positions[*it];
        avg = avg / static_cast<double>(nb.count(v));
        // only the part along the normal: sideways spread is just mesh layout
        d.push_back(std::fabs(dot(m.positions[v] - avg, vn[v])));
    }
    if (d.empty()) return 0;
    std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
    return d[d.size() / 2];
}

// ---- aligning ----

Mat4 principalAlignment(const Mesh& m) {
    if (m.empty()) return Mat4::identity();
    Vec3 c;
    double total = 0;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const double a = length(m.faceNormal(t)) * 0.5;
        c += triCentre(m, t) * a;
        total += a;
    }
    if (total <= 0) return Mat4::identity();
    c = c / total;
    double cov[9] = {0};
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const double a = length(m.faceNormal(t)) * 0.5;
        const Vec3 d = triCentre(m, t) - c;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) cov[i * 3 + j] += a * d[i] * d[j];
    }
    const Eigen3 e = eigenSymmetric(cov);
    Vec3 x = e.vectors[2], y = e.vectors[1], z = e.vectors[0];
    if (z.z < 0) z = -z;  // keep the side that was up, up
    if (x.x < 0) x = -x;
    y = cross(z, x);
    Mat4 r;  // rows are the new axes: world -> aligned
    r.at(0, 0) = x.x; r.at(0, 1) = x.y; r.at(0, 2) = x.z;
    r.at(1, 0) = y.x; r.at(1, 1) = y.y; r.at(1, 2) = y.z;
    r.at(2, 0) = z.x; r.at(2, 1) = z.y; r.at(2, 2) = z.z;
    return Mat4::translate(c) * r * Mat4::translate(-c);
}

// ---- measuring ----

Section section(const Mesh& m, const Vec3& point, const Vec3& normalIn) {
    Section out;
    const Vec3 n = normalize(normalIn);
    if (m.empty() || length(n) < 0.5) return out;
    std::vector<double> d(m.vertexCount());
    for (std::size_t i = 0; i < m.vertexCount(); ++i) {
        d[i] = dot(m.positions[i] - point, n);
        if (d[i] == 0) d[i] = 1e-12;  // on the plane counts as above, so pieces never double up
    }
    auto key = [](std::uint32_t a, std::uint32_t b) { return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b); };
    std::unordered_map<std::uint64_t, Vec3> crossing;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> links;  // edge key -> neighbouring edge keys
    auto at = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint64_t k = key(a, b);
        if (!crossing.count(k)) {
            const double t = d[a] / (d[a] - d[b]);
            crossing[k] = mesh::lerp(m.positions[a], m.positions[b], t);
        }
        return k;
    };
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        std::uint64_t ks[2];
        int c = 0;
        for (int e = 0; e < 3; ++e) {
            const std::uint32_t a = m.indices[t * 3 + e], b = m.indices[t * 3 + (e + 1) % 3];
            if ((d[a] < 0) != (d[b] < 0) && c < 2) ks[c++] = at(a, b);
        }
        if (c == 2 && ks[0] != ks[1]) {
            links[ks[0]].push_back(ks[1]);
            links[ks[1]].push_back(ks[0]);
        }
    }
    std::unordered_map<std::uint64_t, bool> used;
    auto walk = [&](std::uint64_t start) {
        std::vector<std::uint64_t> chain{start};
        used[start] = true;
        std::uint64_t cur = start;
        for (;;) {
            std::uint64_t nextKey = 0;
            bool found = false;
            for (auto k : links[cur]) {
                if (!used[k]) {
                    nextKey = k;
                    found = true;
                    break;
                }
            }
            if (!found) break;
            used[nextKey] = true;
            chain.push_back(nextKey);
            cur = nextKey;
        }
        return chain;
    };
    // open chains first, starting from their ends
    for (auto& [k, ls] : links) {
        if (used[k] || ls.size() != 1) continue;
        auto chain = walk(k);
        std::vector<Vec3> line;
        for (auto c : chain) line.push_back(crossing[c]);
        if (line.size() >= 2) {
            out.lines.push_back(std::move(line));
            out.closed.push_back(false);
        }
    }
    for (auto& [k, ls] : links) {
        if (used[k]) continue;
        auto chain = walk(k);
        std::vector<Vec3> line;
        for (auto c : chain) line.push_back(crossing[c]);
        if (line.size() >= 3) {
            const bool closed = std::find(links[chain.back()].begin(), links[chain.back()].end(), chain.front()) != links[chain.back()].end();
            out.lines.push_back(std::move(line));
            out.closed.push_back(closed);
        }
    }
    return out;
}

double polylineLength(const std::vector<Vec3>& line, bool closed) {
    double l = 0;
    for (std::size_t i = 1; i < line.size(); ++i) l += length(line[i] - line[i - 1]);
    if (closed && line.size() > 2) l += length(line.front() - line.back());
    return l;
}

Patch patchAround(const Mesh& m, const std::vector<Vec3>& vn, const Vec3& c, double radius) {
    Patch p;
    const double r2 = radius * radius;
    for (std::size_t i = 0; i < m.vertexCount(); ++i) {
        const Vec3 d = m.positions[i] - c;
        if (dot(d, d) <= r2) {
            p.points.push_back(m.positions[i]);
            p.normals.push_back(i < vn.size() ? vn[i] : Vec3{});
        }
    }
    return p;
}

}  // namespace wetwaresuite::fit
