// Small 3D math: vectors, 4x4 transforms and bounding boxes. Doubles
// throughout so CAD booleans and slicing keep their precision; files are
// converted to float only when written.
#pragma once

#include <cmath>
#include <limits>
#include <utility>

namespace wetwaresuite::mesh {

constexpr double kPi = 3.14159265358979323846;

struct Vec2 {
    double x = 0, y = 0;

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    Vec2 operator/(double s) const { return {x / s, y / s}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    bool operator==(const Vec2&) const = default;
};

inline double dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline double cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
inline double length(const Vec2& v) { return std::sqrt(dot(v, v)); }

struct Vec3 {
    double x = 0, y = 0, z = 0;

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
    double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
    double& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    bool operator==(const Vec3&) const = default;
};

inline Vec3 operator*(double s, const Vec3& v) { return v * s; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    double l = length(v);
    return l > 0 ? v / l : Vec3{};
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, double t) { return a + (b - a) * t; }
inline Vec3 min(const Vec3& a, const Vec3& b) { return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)}; }
inline Vec3 max(const Vec3& a, const Vec3& b) { return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)}; }

// Row-major 4x4 matrix acting on column vectors: p' = M * p. Compose with
// `a * b` (b applies first), or chain `Mat4::translate(t) * Mat4::rotate(r)`.
struct Mat4 {
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    double& at(int row, int col) { return m[row * 4 + col]; }
    double at(int row, int col) const { return m[row * 4 + col]; }

    static Mat4 identity() { return {}; }
    static Mat4 translate(const Vec3& t);
    static Mat4 scale(const Vec3& s);
    static Mat4 scale(double s) { return scale({s, s, s}); }
    static Mat4 rotateX(double degrees);
    static Mat4 rotateY(double degrees);
    static Mat4 rotateZ(double degrees);
    // Rotates about X, then Y, then Z (degrees), like OpenSCAD's rotate([x, y, z]).
    static Mat4 rotate(const Vec3& degrees);
    // Rotation of `degrees` about an arbitrary axis through the origin.
    static Mat4 rotateAxis(const Vec3& axis, double degrees);

    Mat4 operator*(const Mat4& o) const;
    Vec3 point(const Vec3& p) const;      // applies translation
    Vec3 direction(const Vec3& d) const;  // ignores translation
    Vec3 normal(const Vec3& n) const;     // inverse-transpose of the 3x3 part, normalized
    double determinant3() const;          // < 0 means the transform mirrors
    Mat4 inverse() const;                 // general inverse; identity if singular
};

// Axis-aligned box. Starts empty (min > max) and grows with add().
struct Bounds {
    Vec3 min{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::infinity()};
    Vec3 max{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity()};

    bool empty() const { return min.x > max.x; }
    void add(const Vec3& p) {
        min = mesh::min(min, p);
        max = mesh::max(max, p);
    }
    void merge(const Bounds& b) {
        if (!b.empty()) { add(b.min); add(b.max); }
    }
    Vec3 size() const { return empty() ? Vec3{} : max - min; }
    Vec3 center() const { return empty() ? Vec3{} : (min + max) * 0.5; }
    double diagonal() const { return length(size()); }
    bool contains(const Vec3& p, double eps = 0) const {
        return p.x >= min.x - eps && p.y >= min.y - eps && p.z >= min.z - eps && p.x <= max.x + eps &&
               p.y <= max.y + eps && p.z <= max.z + eps;
    }
};

}  // namespace wetwaresuite::mesh
