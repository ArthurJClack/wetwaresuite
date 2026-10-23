// Primitive meshes with quads kept, the shapes Blender's Add menu has.
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "wetwaresuite/model/ops.hpp"

namespace wetwaresuite::model {

const std::vector<std::string>& primitiveNames() {
    static const std::vector<std::string> v = {"cube", "plane", "grid", "circle", "uv_sphere", "ico_sphere", "cylinder", "cone", "torus"};
    return v;
}

namespace {

constexpr double kTau = 2 * mesh::kPi;

void squareUvs(Face& f) {
    if (f.v.size() == 4) f.uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
}

void positive(double v, const char* what) {
    if (!(v > 0) || !std::isfinite(v)) throw std::invalid_argument(std::string(what) + " must be positive");
}

void count(int v, int lo, int hi, const char* what) {
    if (v < lo || v > hi) throw std::invalid_argument(std::string(what) + " must be " + std::to_string(lo) + " to " + std::to_string(hi));
}

PolyMesh cube(double size) {
    PolyMesh m;
    const double h = size / 2;
    for (int i = 0; i < 8; ++i) m.addVertex({(i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h});
    // each face counter-clockwise seen from outside
    const std::uint32_t f[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& q : f) {
        m.addFace({q[0], q[1], q[2], q[3]});
        squareUvs(m.faces.back());
    }
    return m;
}

PolyMesh grid(double sx, double sy, int nx, int ny) {
    PolyMesh m;
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i) m.addVertex({(static_cast<double>(i) / nx - 0.5) * sx, (static_cast<double>(j) / ny - 0.5) * sy, 0});
    auto id = [&](int i, int j) { return static_cast<std::uint32_t>(j * (nx + 1) + i); };
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            m.addFace({id(i, j), id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
            const double u0 = static_cast<double>(i) / nx, u1 = static_cast<double>(i + 1) / nx;
            const double v0 = static_cast<double>(j) / ny, v1 = static_cast<double>(j + 1) / ny;
            m.faces.back().uv = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
        }
    }
    return m;
}

// A ring of n vertices at height z; returns the first index.
std::uint32_t ring(PolyMesh& m, double r, double z, int n) {
    const std::uint32_t first = static_cast<std::uint32_t>(m.verts.size());
    for (int i = 0; i < n; ++i) {
        const double a = kTau * i / n;
        m.addVertex({r * std::cos(a), r * std::sin(a), z});
    }
    return first;
}

// Caps a ring: up=true faces +Z. fill is ngon, fan or none.
void cap(PolyMesh& m, std::uint32_t first, int n, bool up, const std::string& fill, double z) {
    auto uvOf = [&](std::uint32_t v) {
        const Vec3& p = m.verts[v].p;
        double r = 0;
        for (int i = 0; i < n; ++i) r = std::max(r, std::hypot(m.verts[first + i].p.x, m.verts[first + i].p.y));
        r = r > 0 ? r : 1;
        return Vec2{0.5 + p.x / (2 * r), 0.5 + p.y / (2 * r)};
    };
    if (fill == "none") return;
    if (fill == "fan") {
        const std::uint32_t c = m.addVertex({0, 0, z});
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = first + i, b = first + (i + 1) % n;
            Face& f = m.faces[m.addFace(up ? std::vector<std::uint32_t>{c, a, b} : std::vector<std::uint32_t>{c, b, a})];
            for (auto v : f.v) f.uv.push_back(uvOf(v));
        }
        return;
    }
    std::vector<std::uint32_t> loop;
    for (int i = 0; i < n; ++i) loop.push_back(first + (up ? i : n - 1 - i));
    Face& f = m.faces[m.addFace(loop)];
    for (auto v : f.v) f.uv.push_back(uvOf(v));
}

void checkFill(const std::string& fill) {
    if (fill != "ngon" && fill != "fan" && fill != "none") throw std::invalid_argument("fill is ngon, fan or none");
}

PolyMesh circle(double r, int n, const std::string& fill) {
    checkFill(fill);
    PolyMesh m;
    const std::uint32_t first = ring(m, r, 0, n);
    if (fill == "none") {
        for (int i = 0; i < n; ++i) m.loose.push_back({first + i, first + (i + 1) % n});
    } else {
        cap(m, first, n, true, fill, 0);
    }
    return m;
}

PolyMesh uvSphere(double r, int segs, int rings) {
    PolyMesh m;
    const std::uint32_t north = m.addVertex({0, 0, r});
    std::vector<std::uint32_t> rowStart;
    for (int i = 1; i < rings; ++i) {
        const double phi = mesh::kPi * i / rings;
        rowStart.push_back(ring(m, r * std::sin(phi), r * std::cos(phi), segs));
    }
    const std::uint32_t south = m.addVertex({0, 0, -r});
    auto at = [&](int i, int j) { return rowStart[static_cast<std::size_t>(i)] + static_cast<std::uint32_t>(j % segs); };
    auto uv = [&](int i, int j) { return Vec2{static_cast<double>(j) / segs, 1.0 - static_cast<double>(i + 1) / rings}; };
    for (int j = 0; j < segs; ++j) {
        Face& f = m.faces[m.addFace({north, at(0, j), at(0, j + 1)})];
        f.uv = {{(j + 0.5) / segs, 1.0}, uv(0, j), uv(0, j + 1)};
    }
    for (int i = 0; i + 1 < rings - 1; ++i) {
        for (int j = 0; j < segs; ++j) {
            Face& f = m.faces[m.addFace({at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)})];
            f.uv = {uv(i, j), uv(i + 1, j), uv(i + 1, j + 1), uv(i, j + 1)};
        }
    }
    const int last = rings - 2;
    for (int j = 0; j < segs; ++j) {
        Face& f = m.faces[m.addFace({at(last, j), south, at(last, j + 1)})];
        f.uv = {uv(last, j), {(j + 0.5) / segs, 0.0}, uv(last, j + 1)};
    }
    return m;
}

PolyMesh icoSphere(double r, int level) {
    PolyMesh m;
    const double t = (1.0 + std::sqrt(5.0)) / 2.0;
    const Vec3 base[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (const auto& p : base) m.addVertex(mesh::normalize(p) * r);
    const std::uint32_t f[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                    {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                                    {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (const auto& tri : f) m.addFace({tri[0], tri[1], tri[2]});
    for (auto& face : m.faces)
        if (mesh::dot(faceNormal(m, face), faceCenter(m, face)) < 0) std::swap(face.v[1], face.v[2]);
    for (int l = 1; l < level; ++l) {
        PolyMesh next;
        next.verts = m.verts;
        std::unordered_map<EdgeKey, std::uint32_t> mid;
        auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            auto it = mid.find(edgeKey(a, b));
            if (it != mid.end()) return it->second;
            const std::uint32_t v = next.addVertex(mesh::normalize(next.verts[a].p + next.verts[b].p) * r);
            mid.emplace(edgeKey(a, b), v);
            return v;
        };
        for (const auto& face : m.faces) {
            const std::uint32_t a = face.v[0], b = face.v[1], c = face.v[2];
            const std::uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.addFace({a, ab, ca});
            next.addFace({b, bc, ab});
            next.addFace({c, ca, bc});
            next.addFace({ab, bc, ca});
        }
        m = std::move(next);
    }
    return m;
}

// Side faces between two rings of n vertices (bottom first), with UVs u = i/n.
void side(PolyMesh& m, std::uint32_t bottom, std::uint32_t top, int n, bool topPoint) {
    for (int i = 0; i < n; ++i) {
        const std::uint32_t b0 = bottom + i, b1 = bottom + (i + 1) % n;
        const double u0 = static_cast<double>(i) / n, u1 = static_cast<double>(i + 1) / n;
        if (topPoint) {
            Face& f = m.faces[m.addFace({b0, b1, top})];
            f.uv = {{u0, 0}, {u1, 0}, {(u0 + u1) / 2, 1}};
        } else {
            Face& f = m.faces[m.addFace({b0, b1, top + (i + 1) % n, top + i})];
            f.uv = {{u0, 0}, {u1, 0}, {u1, 1}, {u0, 1}};
        }
    }
}

PolyMesh cone(double r1, double r2, double depth, int n, const std::string& fill) {
    checkFill(fill);
    PolyMesh m;
    const std::uint32_t bottom = ring(m, r1, -depth / 2, n);
    if (r2 > 0) {
        const std::uint32_t top = ring(m, r2, depth / 2, n);
        side(m, bottom, top, n, false);
        cap(m, top, n, true, fill, depth / 2);
    } else {
        const std::uint32_t apex = m.addVertex({0, 0, depth / 2});
        side(m, bottom, apex, n, true);
    }
    cap(m, bottom, n, false, fill, -depth / 2);
    return m;
}

PolyMesh torus(double major, double minor, int segs, int tube) {
    PolyMesh m;
    for (int i = 0; i < segs; ++i) {
        const double th = kTau * i / segs;
        for (int j = 0; j < tube; ++j) {
            const double ph = kTau * j / tube;
            const double rr = major + minor * std::cos(ph);
            m.addVertex({rr * std::cos(th), rr * std::sin(th), minor * std::sin(ph)});
        }
    }
    auto at = [&](int i, int j) { return static_cast<std::uint32_t>((i % segs) * tube + (j % tube)); };
    for (int i = 0; i < segs; ++i) {
        for (int j = 0; j < tube; ++j) {
            Face& f = m.faces[m.addFace({at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)})];
            const double u0 = static_cast<double>(i) / segs, u1 = static_cast<double>(i + 1) / segs;
            const double v0 = static_cast<double>(j) / tube, v1 = static_cast<double>(j + 1) / tube;
            f.uv = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
        }
    }
    return m;
}

}  // namespace

PolyMesh primitive(const std::string& type, const PrimitiveOptions& o) {
    if (type == "cube") {
        positive(o.size, "size");
        return cube(o.size);
    }
    if (type == "plane") {
        positive(o.size, "size");
        return grid(o.size, o.size, 1, 1);
    }
    if (type == "grid") {
        positive(o.size, "size");
        count(o.xSubdivisions, 1, 1000, "x_subdivisions");
        count(o.ySubdivisions, 1, 1000, "y_subdivisions");
        return grid(o.size, o.size, o.xSubdivisions, o.ySubdivisions);
    }
    if (type == "circle") {
        positive(o.radius, "radius");
        count(o.vertices, 3, 10000, "vertices");
        return circle(o.radius, o.vertices, o.fill);
    }
    if (type == "uv_sphere") {
        positive(o.radius, "radius");
        count(o.vertices, 3, 2000, "segments");
        count(o.rings, 3, 1000, "rings");
        return uvSphere(o.radius, o.vertices, o.rings);
    }
    if (type == "ico_sphere") {
        positive(o.radius, "radius");
        count(o.subdivisions, 1, 8, "subdivisions");
        return icoSphere(o.radius, o.subdivisions);
    }
    if (type == "cylinder") {
        positive(o.radius, "radius");
        positive(o.depth, "depth");
        count(o.vertices, 3, 10000, "vertices");
        return cone(o.radius, o.radius, o.depth, o.vertices, o.fill);
    }
    if (type == "cone") {
        positive(o.radius, "radius");
        positive(o.depth, "depth");
        if (o.radius2 < 0) throw std::invalid_argument("radius2 can't be negative");
        count(o.vertices, 3, 10000, "vertices");
        return cone(o.radius, o.radius2, o.depth, o.vertices, o.fill);
    }
    if (type == "torus") {
        positive(o.majorRadius, "major_radius");
        positive(o.minorRadius, "minor_radius");
        if (o.minorRadius >= o.majorRadius) throw std::invalid_argument("minor_radius must be smaller than major_radius");
        count(o.majorSegments, 3, 2000, "major_segments");
        count(o.minorSegments, 3, 1000, "minor_segments");
        return torus(o.majorRadius, o.minorRadius, o.majorSegments, o.minorSegments);
    }
    throw std::invalid_argument("unknown primitive '" + type + "'");
}

}  // namespace wetwaresuite::model
