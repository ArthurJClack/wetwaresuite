#include "achclwss/mesh/math.hpp"

namespace achclwss::mesh {

namespace {

double rad(double degrees) { return degrees * kPi / 180.0; }

// Exact values for multiples of 90 degrees so rotated boxes stay on the grid.
void sinCos(double degrees, double& s, double& c) {
    double r = std::fmod(degrees, 360.0);
    if (r < 0) r += 360.0;
    if (r == 0) { s = 0; c = 1; }
    else if (r == 90) { s = 1; c = 0; }
    else if (r == 180) { s = 0; c = -1; }
    else if (r == 270) { s = -1; c = 0; }
    else { s = std::sin(rad(r)); c = std::cos(rad(r)); }
}

}  // namespace

Mat4 Mat4::translate(const Vec3& t) {
    Mat4 r;
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}

Mat4 Mat4::scale(const Vec3& s) {
    Mat4 r;
    r.at(0, 0) = s.x;
    r.at(1, 1) = s.y;
    r.at(2, 2) = s.z;
    return r;
}

Mat4 Mat4::rotateX(double degrees) {
    double s, c;
    sinCos(degrees, s, c);
    Mat4 r;
    r.at(1, 1) = c; r.at(1, 2) = -s;
    r.at(2, 1) = s; r.at(2, 2) = c;
    return r;
}

Mat4 Mat4::rotateY(double degrees) {
    double s, c;
    sinCos(degrees, s, c);
    Mat4 r;
    r.at(0, 0) = c; r.at(0, 2) = s;
    r.at(2, 0) = -s; r.at(2, 2) = c;
    return r;
}

Mat4 Mat4::rotateZ(double degrees) {
    double s, c;
    sinCos(degrees, s, c);
    Mat4 r;
    r.at(0, 0) = c; r.at(0, 1) = -s;
    r.at(1, 0) = s; r.at(1, 1) = c;
    return r;
}

Mat4 Mat4::rotate(const Vec3& d) { return rotateZ(d.z) * rotateY(d.y) * rotateX(d.x); }

Mat4 Mat4::rotateAxis(const Vec3& axis, double degrees) {
    Vec3 a = normalize(axis);
    if (length(a) == 0) return {};
    double s, c;
    sinCos(degrees, s, c);
    const double t = 1 - c;
    Mat4 r;
    r.at(0, 0) = t * a.x * a.x + c;       r.at(0, 1) = t * a.x * a.y - s * a.z; r.at(0, 2) = t * a.x * a.z + s * a.y;
    r.at(1, 0) = t * a.x * a.y + s * a.z; r.at(1, 1) = t * a.y * a.y + c;       r.at(1, 2) = t * a.y * a.z - s * a.x;
    r.at(2, 0) = t * a.x * a.z - s * a.y; r.at(2, 1) = t * a.y * a.z + s * a.x; r.at(2, 2) = t * a.z * a.z + c;
    return r;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += at(i, k) * o.at(k, j);
            r.at(i, j) = v;
        }
    }
    return r;
}

Vec3 Mat4::point(const Vec3& p) const {
    Vec3 r{at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3),
           at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3),
           at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3)};
    const double w = at(3, 0) * p.x + at(3, 1) * p.y + at(3, 2) * p.z + at(3, 3);
    return (w != 1.0 && w != 0.0) ? r / w : r;
}

Vec3 Mat4::direction(const Vec3& d) const {
    return {at(0, 0) * d.x + at(0, 1) * d.y + at(0, 2) * d.z, at(1, 0) * d.x + at(1, 1) * d.y + at(1, 2) * d.z,
            at(2, 0) * d.x + at(2, 1) * d.y + at(2, 2) * d.z};
}

double Mat4::determinant3() const {
    return at(0, 0) * (at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1)) - at(0, 1) * (at(1, 0) * at(2, 2) - at(1, 2) * at(2, 0)) +
           at(0, 2) * (at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0));
}

Vec3 Mat4::normal(const Vec3& n) const {
    // Cofactor matrix = det * inverse-transpose; the scale drops out after normalizing.
    const double c00 = at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1), c01 = at(1, 2) * at(2, 0) - at(1, 0) * at(2, 2),
                 c02 = at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0);
    const double c10 = at(0, 2) * at(2, 1) - at(0, 1) * at(2, 2), c11 = at(0, 0) * at(2, 2) - at(0, 2) * at(2, 0),
                 c12 = at(0, 1) * at(2, 0) - at(0, 0) * at(2, 1);
    const double c20 = at(0, 1) * at(1, 2) - at(0, 2) * at(1, 1), c21 = at(0, 2) * at(1, 0) - at(0, 0) * at(1, 2),
                 c22 = at(0, 0) * at(1, 1) - at(0, 1) * at(1, 0);
    Vec3 r{c00 * n.x + c01 * n.y + c02 * n.z, c10 * n.x + c11 * n.y + c12 * n.z, c20 * n.x + c21 * n.y + c22 * n.z};
    if (determinant3() < 0) r = -r;
    return normalize(r);
}

Mat4 Mat4::inverse() const {
    // Gauss-Jordan with partial pivoting.
    double a[4][8];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            a[i][j] = at(i, j);
            a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
        }
        if (std::fabs(a[pivot][col]) < 1e-300) return {};
        if (pivot != col) {
            for (int j = 0; j < 8; ++j) std::swap(a[pivot][j], a[col][j]);
        }
        const double inv = 1.0 / a[col][col];
        for (int j = 0; j < 8; ++j) a[col][j] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            if (f == 0) continue;
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[col][j];
        }
    }
    Mat4 out;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) out.at(i, j) = a[i][j + 4];
    }
    return out;
}

}  // namespace achclwss::mesh
