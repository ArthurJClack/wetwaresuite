// Helpers shared by the KiCad schematic and board readers/writers. Internal.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "wetwaresuite/circuit/circuit.hpp"
#include "sexpr.hpp"

namespace wetwaresuite::circuit::kicad {

constexpr double kPi = 3.14159265358979323846;

// A number as KiCad writes it: up to 4 decimals, no trailing zeros.
inline std::string num(double v) {
    if (std::abs(v) < 5e-5) v = 0;
    char b[40];
    std::snprintf(b, sizeof b, "%.4f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

inline std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

// A stable UUID from a string (FNV-1a), so files written twice come out the same.
inline std::string uuidOf(const std::string& key) {
    std::uint64_t h1 = 1469598103934665603ull, h2 = 1099511628211ull * 31;
    for (unsigned char c : key) {
        h1 = (h1 ^ c) * 1099511628211ull;
        h2 = (h2 ^ c) * 1469598103934665603ull + 7;
    }
    char b[40];
    std::snprintf(b, sizeof b, "%08x-%04x-4%03x-8%03x-%012llx", static_cast<unsigned>(h1 >> 32), static_cast<unsigned>((h1 >> 16) & 0xffff),
                  static_cast<unsigned>(h1 & 0xfff), static_cast<unsigned>((h2 >> 48) & 0xfff),
                  static_cast<unsigned long long>(h2 & 0xffffffffffffull));
    return b;
}

// (at x y ...), (xy x y), (start x y) ...
inline Pt xy(const Sx* n) { return n ? Pt{n->num(1), n->num(2)} : Pt{}; }

// Points along the circular arc from a through m to b.
inline std::vector<Pt> arcPoints(Pt a, Pt m, Pt b, int segments = 12) {
    const double d = 2 * (a.x * (m.y - b.y) + m.x * (b.y - a.y) + b.x * (a.y - m.y));
    if (std::abs(d) < 1e-12) return {a, b};
    const double a2 = a.x * a.x + a.y * a.y, m2 = m.x * m.x + m.y * m.y, b2 = b.x * b.x + b.y * b.y;
    const Pt c{(a2 * (m.y - b.y) + m2 * (b.y - a.y) + b2 * (a.y - m.y)) / d, (a2 * (b.x - m.x) + m2 * (a.x - b.x) + b2 * (m.x - a.x)) / d};
    const double r = std::hypot(a.x - c.x, a.y - c.y);
    const double t0 = std::atan2(a.y - c.y, a.x - c.x), tm = std::atan2(m.y - c.y, m.x - c.x), t1 = std::atan2(b.y - c.y, b.x - c.x);
    auto norm = [](double t) {
        while (t <= 0) t += 2 * kPi;
        while (t > 2 * kPi) t -= 2 * kPi;
        return t;
    };
    double sweep = norm(t1 - t0);
    if (norm(tm - t0) > sweep) sweep -= 2 * kPi;
    std::vector<Pt> out;
    for (int i = 0; i <= segments; ++i) {
        const double t = t0 + sweep * i / segments;
        out.push_back({c.x + r * std::cos(t), c.y + r * std::sin(t)});
    }
    out.front() = a;
    out.back() = b;
    return out;
}

inline std::vector<Pt> circlePoints(Pt c, double r, int segments = 24) {
    std::vector<Pt> out;
    for (int i = 0; i <= segments; ++i) {
        const double t = 2 * kPi * i / segments;
        out.push_back({c.x + r * std::cos(t), c.y + r * std::sin(t)});
    }
    return out;
}

// Collects warnings once each.
struct Warn {
    std::vector<std::string>* out = nullptr;
    std::set<std::string> seen;
    void operator()(const std::string& w) {
        if (out && seen.insert(w).second) out->push_back(w);
    }
};

inline int snapAngle(double deg) {
    int r = static_cast<int>(std::lround(deg / 90.0)) * 90;
    return ((r % 360) + 360) % 360;
}

}  // namespace wetwaresuite::circuit::kicad
