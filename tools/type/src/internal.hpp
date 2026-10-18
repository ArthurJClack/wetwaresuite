// Helpers shared by the type tool's sources.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "achclwss/type/type.hpp"

namespace achclwss::typeface {

constexpr double kPi = 3.14159265358979323846;

inline Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
inline Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y}; }
inline Vec operator-(Vec a) { return {-a.x, -a.y}; }
inline Vec operator*(Vec a, double k) { return {a.x * k, a.y * k}; }
inline double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
inline double cross(Vec a, Vec b) { return a.x * b.y - a.y * b.x; }
inline double len(Vec a) { return std::hypot(a.x, a.y); }
inline Vec normalize(Vec a) {
    const double l = len(a);
    return l > 1e-12 ? Vec{a.x / l, a.y / l} : Vec{0, 0};
}

Vec evalSegment(const Segment& s, double t);
Affine componentMatrix(const Component& c);
bool pointInPolygon(const Polygon& poly, Vec p);  // even-odd
std::string fmt(double v);                         // compact number: up to 3 decimals

// Big-endian byte writer for font tables.
class Bytes {
public:
    std::string data;
    void u8(unsigned v) { data += static_cast<char>(v & 0xFF); }
    void u16(unsigned v) {
        u8(v >> 8);
        u8(v);
    }
    void i16(int v) { u16(static_cast<unsigned>(static_cast<std::uint16_t>(static_cast<std::int16_t>(v)))); }
    void u32(std::uint32_t v) {
        u16(v >> 16);
        u16(v & 0xFFFF);
    }
    void i64(std::int64_t v) {
        u32(static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) >> 32));
        u32(static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) & 0xFFFFFFFFu));
    }
    void tag(const std::string& t) {
        for (int i = 0; i < 4; ++i) data += i < static_cast<int>(t.size()) ? t[static_cast<std::size_t>(i)] : ' ';
    }
    void raw(const std::string& s) { data += s; }
    std::size_t size() const { return data.size(); }
    void put16(std::size_t at, unsigned v) {
        data[at] = static_cast<char>((v >> 8) & 0xFF);
        data[at + 1] = static_cast<char>(v & 0xFF);
    }
    void put32(std::size_t at, std::uint32_t v) {
        put16(at, v >> 16);
        put16(at + 2, v & 0xFFFF);
    }
    void pad4() {
        while (data.size() % 4) data += '\0';
    }
};

inline int toInt(double v) { return static_cast<int>(std::lround(v)); }
inline std::int16_t clamp16(double v) {
    const long r = std::lround(v);
    return static_cast<std::int16_t>(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
}

// stb_truetype's outlines for a CFF-flavoured OpenType font starting at `offset` in the file,
// one entry per glyph. False when stb can't read the font.
bool cffOutlines(const std::string& file, std::size_t offset, std::vector<std::vector<Contour>>& glyphs, std::string& error);

// Small XML reader for UFO plists, .glif files and SVG fonts.
struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    std::string text;
    const std::string* attr(const std::string& key) const {
        for (const auto& [k, v] : attrs) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    const XmlNode* child(const std::string& n) const {
        for (const auto& c : children) {
            if (c.name == n) return &c;
        }
        return nullptr;
    }
};
XmlNode parseXml(const std::string& text);  // the root element; throws std::runtime_error
std::string xmlEscape(const std::string& s);

std::string readFile(const std::string& path);  // throws std::runtime_error
void writeFile(const std::string& path, const std::string& bytes);  // creates parent folders; throws

}  // namespace achclwss::typeface
