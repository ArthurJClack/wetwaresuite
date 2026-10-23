#include "wetwaresuite/mesh/primitives.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "internal.hpp"

namespace wetwaresuite::mesh {

namespace {

void need(bool ok, const std::string& what) {
    if (!ok) throw std::invalid_argument(what);
}

// Exact on the axes so radii and bounds come out exact.
Vec2 unitCircle(int i, int n) {
    if (n % 4 == 0) {
        const int q = n / 4;
        if (i % q == 0) {
            switch ((i / q) % 4) {
                case 0: return {1, 0};
                case 1: return {0, 1};
                case 2: return {-1, 0};
                default: return {0, -1};
            }
        }
    }
    const double a = 2.0 * kPi * i / n;
    return {std::cos(a), std::sin(a)};
}

std::vector<Vec2> cleanRing(const std::vector<Vec2>& ring) {
    std::vector<Vec2> out;
    for (const auto& p : ring) {
        if (out.empty() || !(out.back() == p)) out.push_back(p);
    }
    while (out.size() > 1 && out.front() == out.back()) out.pop_back();
    return out;
}

void fixOrientation(Mesh& m) {
    if (volume(m) < 0) flip(m);
}

}  // namespace

std::vector<Vec2> circle2(double radius, int segments, const Vec2& center) {
    need(radius > 0, "circle radius must be positive");
    need(segments >= 3, "a circle needs at least 3 segments");
    std::vector<Vec2> out;
    for (int i = 0; i < segments; ++i) out.push_back(center + unitCircle(i, segments) * radius);
    return out;
}

std::vector<Vec2> rectangle2(double width, double height, bool center) {
    need(width > 0 && height > 0, "rectangle sides must be positive");
    const double x0 = center ? -width / 2 : 0, y0 = center ? -height / 2 : 0;
    return {{x0, y0}, {x0 + width, y0}, {x0 + width, y0 + height}, {x0, y0 + height}};
}

Mesh box(const Vec3& size, bool center) {
    need(size.x > 0 && size.y > 0 && size.z > 0, "box sides must be positive");
    const Vec3 lo = center ? size * -0.5 : Vec3{}, hi = center ? size * 0.5 : size;
    Mesh m;
    for (int i = 0; i < 8; ++i) m.addVertex({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
    const std::uint32_t quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& q : quads) {
        m.addTriangle(q[0], q[1], q[2]);
        m.addTriangle(q[0], q[2], q[3]);
    }
    return m;
}

Mesh revolve(const std::vector<Vec2>& profileIn, double angle, int segments) {
    need(angle > 0 && angle <= 360.0 + 1e-9, "revolve angle must be in (0, 360] degrees");
    need(segments >= 3 || (angle < 360.0 && segments >= 1), "revolve needs at least 3 segments");
    std::vector<Vec2> prof = cleanRing(profileIn);
    need(prof.size() >= 3, "a revolve profile needs at least 3 distinct points");
    for (auto& p : prof) {
        need(p.x >= -1e-12, "revolve profile points must have x >= 0 (x is the radius)");
        if (p.x < 0) p.x = 0;
    }
    need(std::fabs(detail::signedArea(prof)) > 0, "revolve profile has no area");
    if (detail::signedArea(prof) < 0) std::reverse(prof.begin(), prof.end());

    const bool full = angle >= 360.0 - 1e-9;
    const int columns = full ? segments : segments + 1;
    std::vector<Vec2> dirs(static_cast<std::size_t>(columns));
    for (int c = 0; c < columns; ++c) {
        if (full) dirs[static_cast<std::size_t>(c)] = unitCircle(c, segments);
        else {
            const double a = angle * kPi / 180.0 * c / segments;
            dirs[static_cast<std::size_t>(c)] = {std::cos(a), std::sin(a)};
            if (c == 0) dirs[0] = {1, 0};
        }
    }

    Mesh m;
    const std::size_t np = prof.size();
    // ids[j][c]: vertex for profile point j at column c (axis points share one).
    std::vector<std::vector<std::uint32_t>> ids(np);
    for (std::size_t j = 0; j < np; ++j) {
        const Vec2& p = prof[j];
        if (p.x == 0) {
            ids[j].assign(static_cast<std::size_t>(columns), m.addVertex({0, 0, p.y}));
        } else {
            for (int c = 0; c < columns; ++c) {
                const Vec2& d = dirs[static_cast<std::size_t>(c)];
                ids[j].push_back(m.addVertex({p.x * d.x, p.x * d.y, p.y}));
            }
        }
    }
    for (std::size_t j = 0; j < np; ++j) {
        const std::size_t k = (j + 1) % np;
        const bool ja = prof[j].x == 0, ka = prof[k].x == 0;
        if (ja && ka) continue;
        for (int c = 0; c < segments; ++c) {
            const auto c0 = static_cast<std::size_t>(c), c1 = static_cast<std::size_t>(full ? (c + 1) % columns : c + 1);
            const std::uint32_t a = ids[j][c0], b = ids[j][c1], cc = ids[k][c1], d = ids[k][c0];
            if (ja) m.addTriangle(a, cc, d);
            else if (ka) m.addTriangle(a, b, cc);
            else {
                m.addTriangle(a, b, cc);
                m.addTriangle(a, cc, d);
            }
        }
    }
    if (!full) {
        auto tris = detail::triangulate(prof, {});
        const auto last = static_cast<std::size_t>(columns - 1);
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            m.addTriangle(ids[tris[t]][0], ids[tris[t + 1]][0], ids[tris[t + 2]][0]);
            m.addTriangle(ids[tris[t]][last], ids[tris[t + 2]][last], ids[tris[t + 1]][last]);
        }
    }
    cleanup(m);
    fixOrientation(m);
    return m;
}

Mesh cylinder(double radius, double height, int segments, bool center) {
    need(radius > 0 && height > 0, "cylinder radius and height must be positive");
    return cone(radius, radius, height, segments, center);
}

Mesh cone(double r1, double r2, double height, int segments, bool center) {
    need(r1 >= 0 && r2 >= 0 && (r1 > 0 || r2 > 0), "cone radii must be >= 0 and not both 0");
    need(height > 0, "cone height must be positive");
    need(segments >= 3, "a cone needs at least 3 segments");
    const double z0 = center ? -height / 2 : 0, z1 = z0 + height;
    return revolve({{0, z0}, {r1, z0}, {r2, z1}, {0, z1}}, 360.0, segments);
}

Mesh sphere(double radius, int segments, int rings) {
    need(radius > 0, "sphere radius must be positive");
    need(segments >= 3 && rings >= 2, "a sphere needs at least 3 segments and 2 rings");
    std::vector<Vec2> prof;
    for (int i = 0; i <= rings; ++i) {
        // Profile from the south pole up the +x side to the north pole.
        Vec2 u = unitCircle(i, 2 * rings);  // angle pi*i/rings
        prof.push_back({u.y * radius, -u.x * radius});
    }
    prof.front() = {0, -radius};
    prof.back() = {0, radius};
    return revolve(prof, 360.0, segments);
}

Mesh torus(double major, double minor, int segments, int tubeSegments) {
    need(minor > 0 && major > minor, "torus needs major > minor > 0");
    need(segments >= 3 && tubeSegments >= 3, "a torus needs at least 3 segments each way");
    return revolve(circle2(minor, tubeSegments, {major, 0}), 360.0, segments);
}

Mesh plane(double width, double depth, int nx, int ny) {
    need(width > 0 && depth > 0, "plane sides must be positive");
    need(nx >= 1 && ny >= 1, "plane divisions must be at least 1");
    Mesh m;
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            const double u = static_cast<double>(i) / nx, v = static_cast<double>(j) / ny;
            m.addVertex({(u - 0.5) * width, (v - 0.5) * depth, 0});
            m.normals.push_back({0, 0, 1});
            m.uvs.push_back({u, v});
        }
    }
    const auto row = static_cast<std::uint32_t>(nx + 1);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(j) * row + static_cast<std::uint32_t>(i);
            m.addTriangle(a, a + 1, a + row + 1);
            m.addTriangle(a, a + row + 1, a + row);
        }
    }
    return m;
}

std::vector<std::uint32_t> triangulate(const Polygon2& shape) { return detail::triangulate(shape.outer, shape.holes); }

Mesh extrude(const Polygon2& shape, double height, bool center) {
    need(height > 0, "extrude height must be positive");
    std::vector<Vec2> outer = cleanRing(shape.outer);
    need(outer.size() >= 3 && std::fabs(detail::signedArea(outer)) > 0, "extrude needs an outer ring with area");
    if (detail::signedArea(outer) < 0) std::reverse(outer.begin(), outer.end());
    std::vector<std::vector<Vec2>> holes;
    for (const auto& h : shape.holes) {
        auto r = cleanRing(h);
        if (r.size() < 3 || detail::signedArea(r) == 0) continue;
        if (detail::signedArea(r) > 0) std::reverse(r.begin(), r.end());
        holes.push_back(std::move(r));
    }
    const double z0 = center ? -height / 2 : 0, z1 = z0 + height;

    Mesh m;
    std::vector<const std::vector<Vec2>*> rings = {&outer};
    for (const auto& h : holes) rings.push_back(&h);
    std::size_t total = 0;
    for (auto* r : rings) total += r->size();
    const auto top = static_cast<std::uint32_t>(total);  // bottom ids 0..total-1, top ids total..2*total-1
    for (double z : {z0, z1}) {
        for (auto* r : rings) {
            for (const auto& p : *r) m.addVertex({p.x, p.y, z});
        }
    }
    std::uint32_t base = 0;
    for (auto* r : rings) {
        const auto n = static_cast<std::uint32_t>(r->size());
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t a = base + i, b = base + (i + 1) % n;
            m.addTriangle(a, b, b + top);
            m.addTriangle(a, b + top, a + top);
        }
        base += n;
    }
    auto tris = detail::triangulate(outer, holes);
    for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
        m.addTriangle(tris[t] + top, tris[t + 1] + top, tris[t + 2] + top);
        m.addTriangle(tris[t], tris[t + 2], tris[t + 1]);
    }
    cleanup(m);
    fixOrientation(m);
    return m;
}

}  // namespace wetwaresuite::mesh
