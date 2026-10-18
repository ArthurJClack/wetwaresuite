// Helpers shared by the mesh sources; not part of the public API.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "wetwaresuite/mesh/math.hpp"

namespace wetwaresuite::mesh::detail {

inline std::uint64_t bits(double d) {
    d += 0.0;  // -0.0 -> +0.0
    std::uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

inline std::size_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
}

struct ExactHash {
    std::size_t operator()(const Vec3& p) const { return mix(mix(bits(p.x), bits(p.y)), bits(p.z)); }
};
struct ExactEq {
    bool operator()(const Vec3& a, const Vec3& b) const { return a.x + 0.0 == b.x + 0.0 && a.y + 0.0 == b.y + 0.0 && a.z + 0.0 == b.z + 0.0; }
};

// Maps positions to ids; identical positions share an id.
class ExactIndex {
public:
    std::uint32_t id(const Vec3& p) {
        auto [it, inserted] = map_.try_emplace(p, static_cast<std::uint32_t>(map_.size()));
        (void)inserted;
        return it->second;
    }
    std::size_t size() const { return map_.size(); }
    void reserve(std::size_t n) { map_.reserve(n); }

private:
    std::unordered_map<Vec3, std::uint32_t, ExactHash, ExactEq> map_;
};

// Merges points within `tolerance` of an earlier point (first one wins).
class Welder {
public:
    explicit Welder(double tolerance) : tol_(tolerance) {}

    // Returns the index of the representative point, adding p if it's new.
    std::uint32_t add(const Vec3& p) {
        if (tol_ <= 0) {
            auto [it, inserted] = exact_.try_emplace(p, static_cast<std::uint32_t>(points.size()));
            if (inserted) points.push_back(p);
            return it->second;
        }
        const auto cx = cell(p.x), cy = cell(p.y), cz = cell(p.z);
        const double tol2 = tol_ * tol_;
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    auto it = grid_.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == grid_.end()) continue;
                    for (std::uint32_t i : it->second) {
                        Vec3 d = points[i] - p;
                        if (dot(d, d) <= tol2) return i;
                    }
                }
            }
        }
        auto i = static_cast<std::uint32_t>(points.size());
        points.push_back(p);
        grid_[key(cx, cy, cz)].push_back(i);
        return i;
    }

    std::vector<Vec3> points;

private:
    std::int64_t cell(double v) const { return static_cast<std::int64_t>(std::floor(v / tol_)); }
    static std::uint64_t key(std::int64_t x, std::int64_t y, std::int64_t z) {
        return mix(mix(static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y)), static_cast<std::uint64_t>(z));
    }

    double tol_;
    std::unordered_map<Vec3, std::uint32_t, ExactHash, ExactEq> exact_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid_;
};

inline std::uint64_t edgeKey(std::uint32_t a, std::uint32_t b) { return (static_cast<std::uint64_t>(a) << 32) | b; }

// Little-endian binary helpers (independent of host byte order).
inline void putU32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
inline void putU16(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}
inline void putF32(std::string& out, double v) {
    float f = static_cast<float>(v);
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    putU32(out, u);
}
inline std::uint32_t getU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint32_t getU16(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
}
inline float getF32(const unsigned char* p) {
    std::uint32_t u = getU32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Ear-clipping triangulation of a simple polygon with holes (any winding).
// Returns indices into the concatenation outer + holes[0] + holes[1] + ...,
// three per triangle, wound counter-clockwise.
std::vector<std::uint32_t> triangulate(const std::vector<Vec2>& outer, const std::vector<std::vector<Vec2>>& holes);

double signedArea(const std::vector<Vec2>& poly);

}  // namespace wetwaresuite::mesh::detail
