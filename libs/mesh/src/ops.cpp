#include "wetwaresuite/mesh/ops.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "internal.hpp"

namespace wetwaresuite::mesh {

Mesh clusterVertices(const Mesh& mesh, double cellSize) {
    if (cellSize <= 0 || mesh.empty()) return mesh;
    const Bounds b = bounds(mesh);
    std::unordered_map<std::uint64_t, std::uint32_t> cells;
    std::vector<Vec3> sums;
    std::vector<double> counts;
    std::vector<std::uint32_t> remap(mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        const Vec3 q = (mesh.positions[i] - b.min) / cellSize;
        const auto x = static_cast<std::uint64_t>(q.x), y = static_cast<std::uint64_t>(q.y), z = static_cast<std::uint64_t>(q.z);
        const std::uint64_t key = (x << 42) ^ (y << 21) ^ z;
        auto [it, inserted] = cells.try_emplace(key, static_cast<std::uint32_t>(sums.size()));
        if (inserted) {
            sums.emplace_back();
            counts.push_back(0);
        }
        sums[it->second] += mesh.positions[i];
        counts[it->second] += 1;
        remap[i] = it->second;
    }
    Mesh out;
    out.positions.resize(sums.size());
    for (std::size_t i = 0; i < sums.size(); ++i) out.positions[i] = sums[i] / counts[i];
    out.indices.reserve(mesh.indices.size());
    for (auto i : mesh.indices) out.indices.push_back(remap[i]);
    cleanup(out);
    return out;
}

Mesh decimate(const Mesh& mesh, std::size_t target) {
    if (mesh.triangleCount() <= target) return mesh;
    const double extent = std::max({bounds(mesh).size().x, bounds(mesh).size().y, bounds(mesh).size().z});
    if (extent <= 0 || target == 0) return {};
    // Largest grid resolution (cells along the longest side) that meets the target.
    int lo = 1, hi = 4096;
    Mesh best = clusterVertices(mesh, extent * 1.0001);
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        Mesh m = clusterVertices(mesh, extent * 1.0001 / mid);
        if (m.triangleCount() <= target) {
            lo = mid;
            best = std::move(m);
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

double loopArea(const Loop& loop) {
    double a = 0;
    for (std::size_t i = 0, n = loop.size(); i < n; ++i) a += cross(loop[i], loop[(i + 1) % n]);
    return a * 0.5;
}

namespace {

struct Segment {
    std::uint64_t from, to;  // undirected edge keys where the segment starts and ends
    Vec2 start;
};

std::uint64_t undirected(std::uint32_t a, std::uint32_t b) { return a < b ? detail::edgeKey(a, b) : detail::edgeKey(b, a); }

// Removes points that sit on the straight line between their neighbours
// (each quad's diagonal adds one), keeping corners and direction reversals.
void dropCollinear(Loop& loop) {
    bool changed = true;
    while (changed && loop.size() > 3) {
        changed = false;
        Loop out;
        out.reserve(loop.size());
        const std::size_t n = loop.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2& a = out.empty() ? loop[(i + n - 1) % n] : out.back();
            const Vec2 &b = loop[i], &c = loop[(i + 1) % n];
            const Vec2 u = b - a, w = c - b;
            const bool straight = std::fabs(cross(u, w)) <= 1e-12 * length(u) * length(w) && dot(u, w) > 0;
            if (straight && n - (i - out.size()) > 3) {
                changed = true;
                continue;
            }
            out.push_back(b);
        }
        loop = std::move(out);
    }
}

std::vector<Loop> chain(const std::vector<Segment>& segs) {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> byStart(segs.size());
    for (std::size_t i = 0; i < segs.size(); ++i) byStart[i] = {segs[i].from, static_cast<std::uint32_t>(i)};
    std::sort(byStart.begin(), byStart.end());
    std::vector<bool> used(segs.size(), false);
    auto find = [&](std::uint64_t key) -> std::int64_t {
        auto it = std::lower_bound(byStart.begin(), byStart.end(), std::make_pair(key, std::uint32_t{0}));
        for (; it != byStart.end() && it->first == key; ++it) {
            if (!used[it->second]) return it->second;
        }
        return -1;
    };
    std::vector<Loop> loops;
    for (std::size_t s = 0; s < segs.size(); ++s) {
        if (used[s]) continue;
        Loop loop;
        std::int64_t cur = static_cast<std::int64_t>(s);
        while (cur >= 0) {
            used[static_cast<std::size_t>(cur)] = true;
            const Segment& seg = segs[static_cast<std::size_t>(cur)];
            if (loop.empty() || !(loop.back() == seg.start)) loop.push_back(seg.start);
            cur = find(seg.to);
        }
        while (loop.size() > 1 && loop.front() == loop.back()) loop.pop_back();
        dropCollinear(loop);
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    }
    return loops;
}

}  // namespace

std::vector<std::vector<Loop>> sliceLayers(const Mesh& mesh, const std::vector<double>& zs) {
    std::vector<std::vector<Loop>> result(zs.size());
    if (zs.empty() || mesh.empty()) return result;
    // Topology by exact position, so meshes split for normals still chain up.
    detail::ExactIndex ids;
    std::vector<std::uint32_t> id(mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) id[i] = ids.id(mesh.positions[i]);

    std::vector<std::size_t> order(zs.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return zs[a] < zs[b]; });
    std::vector<double> sorted(zs.size());
    for (std::size_t i = 0; i < order.size(); ++i) sorted[i] = zs[order[i]];

    std::vector<std::vector<Segment>> segs(zs.size());
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const std::uint32_t vi[3] = {mesh.indices[t * 3], mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]};
        const Vec3* p[3] = {&mesh.positions[vi[0]], &mesh.positions[vi[1]], &mesh.positions[vi[2]]};
        const double zmin = std::min({p[0]->z, p[1]->z, p[2]->z}), zmax = std::max({p[0]->z, p[1]->z, p[2]->z});
        // Crossed when some vertex is below (z < h) and some is not: zmin < h <= zmax.
        auto first = std::upper_bound(sorted.begin(), sorted.end(), zmin);
        for (auto it = first; it != sorted.end() && *it <= zmax; ++it) {
            const double h = *it;
            Segment s{};
            bool haveFrom = false, haveTo = false;
            for (int k = 0; k < 3; ++k) {
                const int k2 = (k + 1) % 3;
                const bool b1 = p[k]->z < h, b2 = p[k2]->z < h;
                if (b1 == b2) continue;
                const Vec3& lo = b1 ? *p[k] : *p[k2];
                const Vec3& hi = b1 ? *p[k2] : *p[k];
                const double f = (h - lo.z) / (hi.z - lo.z);
                const Vec2 x{lo.x + (hi.x - lo.x) * f, lo.y + (hi.y - lo.y) * f};
                const std::uint64_t key = undirected(id[vi[k]], id[vi[k2]]);
                if (!b1) {  // above -> below: the segment starts here
                    s.from = key;
                    s.start = x;
                    haveFrom = true;
                } else {
                    s.to = key;
                    haveTo = true;
                }
            }
            if (haveFrom && haveTo && s.from != s.to) segs[static_cast<std::size_t>(it - sorted.begin())].push_back(s);
        }
    }
    for (std::size_t i = 0; i < order.size(); ++i) result[order[i]] = chain(segs[i]);
    return result;
}

std::vector<Loop> slice(const Mesh& mesh, double z) { return sliceLayers(mesh, {z}).front(); }

}  // namespace wetwaresuite::mesh
