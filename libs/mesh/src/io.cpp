#include "wetwaresuite/mesh/io.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include "internal.hpp"

namespace wetwaresuite::mesh {

namespace {

using detail::getF32;
using detail::getU16;
using detail::getU32;
using detail::putF32;
using detail::putU16;
using detail::putU32;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string extOf(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower(e);
}

void num(std::string& out, double v) {
    char buf[32];
    int n = std::snprintf(buf, sizeof buf, "%.9g", v);
    out.append(buf, static_cast<std::size_t>(n));
}

// Whitespace tokenizer over a null-terminated buffer.
class Tokens {
public:
    explicit Tokens(const std::string& s, std::size_t at = 0) : p_(s.c_str() + std::min(at, s.size())), end_(s.c_str() + s.size()) {}
    bool done() {
        skip();
        return p_ >= end_;
    }
    std::string_view word() {
        skip();
        const char* b = p_;
        while (p_ < end_ && !std::isspace(static_cast<unsigned char>(*p_))) ++p_;
        return {b, static_cast<std::size_t>(p_ - b)};
    }
    double number() {
        skip();
        char* e = nullptr;
        double v = std::strtod(p_, &e);
        if (e == p_) throw std::runtime_error("expected a number near '" + std::string(p_, std::min<std::size_t>(16, static_cast<std::size_t>(end_ - p_))) + "'");
        p_ = e;
        return v;
    }

private:
    void skip() {
        while (p_ < end_ && std::isspace(static_cast<unsigned char>(*p_))) ++p_;
    }
    const char* p_;
    const char* end_;
};

// Splits text into lines with '#' comments removed.
std::vector<std::string_view> lines(const std::string& text) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + at, nl - at);
        if (auto h = line.find('#'); h != std::string_view::npos) line = line.substr(0, h);
        out.push_back(line);
        at = nl + 1;
    }
    return out;
}

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        std::size_t b = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i > b) out.push_back(line.substr(b, i - b));
    }
    return out;
}

double toNumber(std::string_view s) {
    std::string tmp(s);
    char* e = nullptr;
    double v = std::strtod(tmp.c_str(), &e);
    if (e == tmp.c_str()) throw std::runtime_error("expected a number, got '" + tmp + "'");
    return v;
}

long long toInt(std::string_view s) {
    std::string tmp(s);
    char* e = nullptr;
    long long v = std::strtoll(tmp.c_str(), &e, 10);
    if (e == tmp.c_str()) throw std::runtime_error("expected an integer, got '" + tmp + "'");
    return v;
}

void checkIndices(const Mesh& m, const char* format) {
    for (auto i : m.indices) {
        if (i >= m.positions.size()) throw std::runtime_error(std::string(format) + " file refers to a vertex that doesn't exist");
    }
}

// ---- STL ----

Mesh fromSoup(const std::vector<Vec3>& corners) {
    Mesh m;
    detail::ExactIndex ids;
    ids.reserve(corners.size() / 2);
    m.indices.reserve(corners.size());
    for (const auto& c : corners) {
        const std::size_t before = ids.size();
        const std::uint32_t id = ids.id(c);
        if (ids.size() > before) m.positions.push_back(c);
        m.indices.push_back(id);
    }
    cleanup(m);
    return m;
}

}  // namespace

Mesh readStl(const std::string& bytes) {
    const auto* u = reinterpret_cast<const unsigned char*>(bytes.data());
    bool binary = false;
    if (bytes.size() >= 84) {
        const std::uint64_t n = getU32(u + 80);
        if (84 + 50 * n == bytes.size()) binary = true;
    }
    std::size_t lead = 0;
    while (lead < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[lead]))) ++lead;
    const bool looksAscii = bytes.compare(lead, 5, "solid") == 0;
    std::vector<Vec3> corners;
    if (binary || !looksAscii) {
        if (bytes.size() < 84) throw std::runtime_error("STL file is too short");
        const std::uint64_t n = getU32(u + 80);
        if (84 + 50 * n > bytes.size()) throw std::runtime_error("STL file is truncated");
        corners.reserve(static_cast<std::size_t>(n) * 3);
        for (std::uint64_t t = 0; t < n; ++t) {
            const unsigned char* f = u + 84 + 50 * t + 12;
            for (int k = 0; k < 3; ++k) corners.push_back({getF32(f + 12 * k), getF32(f + 12 * k + 4), getF32(f + 12 * k + 8)});
        }
    } else {
        Tokens tok(bytes);
        while (!tok.done()) {
            if (tok.word() == "vertex") {
                const double x = tok.number(), y = tok.number(), z = tok.number();
                corners.push_back({x, y, z});
            }
        }
        if (corners.size() % 3) throw std::runtime_error("ASCII STL has an incomplete facet");
    }
    return fromSoup(corners);
}

std::string writeStl(const Mesh& m, bool ascii) {
    std::string out;
    if (ascii) {
        out = "solid wetwaresuite\n";
        for (std::size_t t = 0; t < m.triangleCount(); ++t) {
            const Vec3 n = normalize(m.faceNormal(t));
            out += "  facet normal ";
            num(out, n.x); out += ' '; num(out, n.y); out += ' '; num(out, n.z);
            out += "\n    outer loop\n";
            for (int k = 0; k < 3; ++k) {
                const Vec3& p = m.corner(t, k);
                out += "      vertex ";
                num(out, p.x); out += ' '; num(out, p.y); out += ' '; num(out, p.z);
                out += '\n';
            }
            out += "    endloop\n  endfacet\n";
        }
        out += "endsolid wetwaresuite\n";
        return out;
    }
    out.reserve(84 + 50 * m.triangleCount());
    std::string header = "binary STL written by wetwaresuite";
    header.resize(80, ' ');
    out += header;
    putU32(out, static_cast<std::uint32_t>(m.triangleCount()));
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const Vec3 n = normalize(m.faceNormal(t));
        putF32(out, n.x); putF32(out, n.y); putF32(out, n.z);
        for (int k = 0; k < 3; ++k) {
            const Vec3& p = m.corner(t, k);
            putF32(out, p.x); putF32(out, p.y); putF32(out, p.z);
        }
        putU16(out, 0);
    }
    return out;
}

// ---- OBJ ----

Mesh readObj(const std::string& text) {
    std::vector<Vec3> v, vn;
    std::vector<Vec2> vt;
    struct Corner {
        long long v, t, n;
    };
    std::vector<std::vector<Corner>> faces;
    for (auto line : lines(text)) {
        auto parts = split(line);
        if (parts.empty()) continue;
        const auto& k = parts[0];
        if (k == "v" && parts.size() >= 4) v.push_back({toNumber(parts[1]), toNumber(parts[2]), toNumber(parts[3])});
        else if (k == "vt" && parts.size() >= 3) vt.push_back({toNumber(parts[1]), toNumber(parts[2])});
        else if (k == "vt" && parts.size() == 2) vt.push_back({toNumber(parts[1]), 0});
        else if (k == "vn" && parts.size() >= 4) vn.push_back({toNumber(parts[1]), toNumber(parts[2]), toNumber(parts[3])});
        else if (k == "f" && parts.size() >= 4) {
            std::vector<Corner> f;
            for (std::size_t i = 1; i < parts.size(); ++i) {
                Corner c{0, 0, 0};
                std::string_view s = parts[i];
                std::size_t s1 = s.find('/');
                c.v = toInt(s.substr(0, s1));
                if (s1 != std::string_view::npos) {
                    std::size_t s2 = s.find('/', s1 + 1);
                    auto ts = s.substr(s1 + 1, s2 == std::string_view::npos ? std::string_view::npos : s2 - s1 - 1);
                    if (!ts.empty()) c.t = toInt(ts);
                    if (s2 != std::string_view::npos && s2 + 1 < s.size()) c.n = toInt(s.substr(s2 + 1));
                }
                // 1-based; negative counts back from the latest element.
                auto fix = [](long long idx, std::size_t count) -> long long { return idx < 0 ? static_cast<long long>(count) + idx + 1 : idx; };
                c.v = fix(c.v, v.size());
                c.t = fix(c.t, vt.size());
                c.n = fix(c.n, vn.size());
                if (c.v < 1 || c.v > static_cast<long long>(v.size())) throw std::runtime_error("OBJ face refers to a vertex that doesn't exist");
                if (c.t > static_cast<long long>(vt.size()) || c.n > static_cast<long long>(vn.size()) || c.t < 0 || c.n < 0) {
                    throw std::runtime_error("OBJ face refers to a texture coordinate or normal that doesn't exist");
                }
                f.push_back(c);
            }
            faces.push_back(std::move(f));
        }
    }
    bool allT = true, allN = true, anyExtra = false;
    for (const auto& f : faces) {
        for (const auto& c : f) {
            allT = allT && c.t > 0;
            allN = allN && c.n > 0;
            anyExtra = anyExtra || c.t > 0 || c.n > 0;
        }
    }
    Mesh m;
    if (!anyExtra) {
        m.positions = v;
        for (const auto& f : faces) {
            for (std::size_t i = 1; i + 1 < f.size(); ++i) {
                m.addTriangle(static_cast<std::uint32_t>(f[0].v - 1), static_cast<std::uint32_t>(f[i].v - 1),
                              static_cast<std::uint32_t>(f[i + 1].v - 1));
            }
        }
    } else {
        struct KeyHash {
            std::size_t operator()(const Corner& c) const {
                return detail::mix(detail::mix(static_cast<std::uint64_t>(c.v), static_cast<std::uint64_t>(c.t)), static_cast<std::uint64_t>(c.n));
            }
        };
        struct KeyEq {
            bool operator()(const Corner& a, const Corner& b) const { return a.v == b.v && a.t == b.t && a.n == b.n; }
        };
        std::unordered_map<Corner, std::uint32_t, KeyHash, KeyEq> map;
        auto vertex = [&](const Corner& in) {
            const Corner c{in.v, allT ? in.t : 0, allN ? in.n : 0};
            auto [it, inserted] = map.try_emplace(c, static_cast<std::uint32_t>(m.positions.size()));
            if (inserted) {
                m.addVertex(v[static_cast<std::size_t>(c.v - 1)]);
                if (allT) m.uvs.push_back(vt[static_cast<std::size_t>(c.t - 1)]);
                if (allN) m.normals.push_back(vn[static_cast<std::size_t>(c.n - 1)]);
            }
            return it->second;
        };
        for (const auto& f : faces) {
            std::vector<std::uint32_t> ids;
            for (const auto& c : f) ids.push_back(vertex(c));
            for (std::size_t i = 1; i + 1 < ids.size(); ++i) m.addTriangle(ids[0], ids[i], ids[i + 1]);
        }
    }
    cleanup(m);
    return m;
}

std::string writeObj(const Mesh& m) {
    std::string out = "# written by wetwaresuite\no mesh\n";
    for (const auto& p : m.positions) {
        out += "v "; num(out, p.x); out += ' '; num(out, p.y); out += ' '; num(out, p.z); out += '\n';
    }
    const bool t = m.hasUvs(), n = m.hasNormals();
    if (t) {
        for (const auto& uv : m.uvs) { out += "vt "; num(out, uv.x); out += ' '; num(out, uv.y); out += '\n'; }
    }
    if (n) {
        for (const auto& v : m.normals) {
            out += "vn "; num(out, v.x); out += ' '; num(out, v.y); out += ' '; num(out, v.z); out += '\n';
        }
    }
    for (std::size_t f = 0; f < m.triangleCount(); ++f) {
        out += 'f';
        for (int k = 0; k < 3; ++k) {
            const std::string i = std::to_string(m.indices[f * 3 + static_cast<std::size_t>(k)] + 1);
            out += ' ';
            out += i;
            if (t || n) out += '/';
            if (t) out += i;
            if (n) { out += '/'; out += i; }
        }
        out += '\n';
    }
    return out;
}

// ---- PLY ----

namespace {

enum class PlyType { I8, U8, I16, U16, I32, U32, F32, F64 };

PlyType plyType(const std::string& s) {
    if (s == "char" || s == "int8") return PlyType::I8;
    if (s == "uchar" || s == "uint8") return PlyType::U8;
    if (s == "short" || s == "int16") return PlyType::I16;
    if (s == "ushort" || s == "uint16") return PlyType::U16;
    if (s == "int" || s == "int32") return PlyType::I32;
    if (s == "uint" || s == "uint32") return PlyType::U32;
    if (s == "float" || s == "float32") return PlyType::F32;
    if (s == "double" || s == "float64") return PlyType::F64;
    throw std::runtime_error("PLY property type '" + s + "' isn't supported");
}

std::size_t plySize(PlyType t) {
    switch (t) {
        case PlyType::I8: case PlyType::U8: return 1;
        case PlyType::I16: case PlyType::U16: return 2;
        case PlyType::I32: case PlyType::U32: case PlyType::F32: return 4;
        case PlyType::F64: return 8;
    }
    return 4;
}

struct PlyProp {
    std::string name;
    PlyType type = PlyType::F32;
    bool list = false;
    PlyType countType = PlyType::U8;
};

struct PlyElement {
    std::string name;
    std::size_t count = 0;
    std::vector<PlyProp> props;
};

class PlyReader {
public:
    PlyReader(const std::string& data, std::size_t at, int format) : d_(data), at_(at), format_(format), tok_(data, at) {}

    double read(PlyType t) {
        if (format_ == 0) return tok_.number();
        const std::size_t n = plySize(t);
        if (at_ + n > d_.size()) throw std::runtime_error("PLY file is truncated");
        unsigned char b[8];
        std::memcpy(b, d_.data() + at_, n);
        at_ += n;
        if (format_ == 2) std::reverse(b, b + n);  // big endian
        switch (t) {
            case PlyType::I8: return static_cast<std::int8_t>(b[0]);
            case PlyType::U8: return b[0];
            case PlyType::I16: return static_cast<std::int16_t>(getU16(b));
            case PlyType::U16: return getU16(b);
            case PlyType::I32: return static_cast<std::int32_t>(getU32(b));
            case PlyType::U32: return getU32(b);
            case PlyType::F32: return getF32(b);
            case PlyType::F64: {
                std::uint64_t u = static_cast<std::uint64_t>(getU32(b)) | (static_cast<std::uint64_t>(getU32(b + 4)) << 32);
                double v;
                std::memcpy(&v, &u, 8);
                return v;
            }
        }
        return 0;
    }

private:
    const std::string& d_;
    std::size_t at_;
    int format_;  // 0 ascii, 1 little endian, 2 big endian
    Tokens tok_;
};

void putPly(std::string& out, PlyType t, double v) {
    if (t == PlyType::F32) putF32(out, v);
    else if (t == PlyType::U8) out.push_back(static_cast<char>(static_cast<unsigned>(v) & 0xff));
    else putU32(out, static_cast<std::uint32_t>(v));
}

}  // namespace

Mesh readPly(const std::string& bytes) {
    if (bytes.compare(0, 3, "ply") != 0) throw std::runtime_error("not a PLY file (no 'ply' header)");
    const std::size_t endHeader = bytes.find("end_header");
    if (endHeader == std::string::npos) throw std::runtime_error("PLY header never ends");
    std::size_t bodyAt = bytes.find('\n', endHeader);
    bodyAt = bodyAt == std::string::npos ? bytes.size() : bodyAt + 1;

    int format = -1;
    std::vector<PlyElement> elements;
    const std::string header = bytes.substr(0, endHeader);
    for (auto line : lines(header)) {
        auto parts = split(line);
        if (parts.empty()) continue;
        if (parts[0] == "format" && parts.size() >= 2) {
            if (parts[1] == "ascii") format = 0;
            else if (parts[1] == "binary_little_endian") format = 1;
            else if (parts[1] == "binary_big_endian") format = 2;
        } else if (parts[0] == "element" && parts.size() >= 3) {
            elements.push_back({std::string(parts[1]), static_cast<std::size_t>(toInt(parts[2])), {}});
        } else if (parts[0] == "property" && !elements.empty()) {
            PlyProp p;
            if (parts.size() >= 5 && parts[1] == "list") {
                p.list = true;
                p.countType = plyType(std::string(parts[2]));
                p.type = plyType(std::string(parts[3]));
                p.name = parts[4];
            } else if (parts.size() >= 3) {
                p.type = plyType(std::string(parts[1]));
                p.name = parts[2];
            }
            elements.back().props.push_back(p);
        }
    }
    if (format < 0) throw std::runtime_error("PLY format line is missing or unknown");

    Mesh m;
    PlyReader r(bytes, bodyAt, format);
    bool hasN = false, hasT = false;
    for (const auto& e : elements) {
        if (e.name == "vertex") {
            int ix = -1, iy = -1, iz = -1, inx = -1, iny = -1, inz = -1, iu = -1, iv = -1;
            for (std::size_t i = 0; i < e.props.size(); ++i) {
                const auto& n = e.props[i].name;
                const int ii = static_cast<int>(i);
                if (n == "x") ix = ii; else if (n == "y") iy = ii; else if (n == "z") iz = ii;
                else if (n == "nx") inx = ii; else if (n == "ny") iny = ii; else if (n == "nz") inz = ii;
                else if (n == "u" || n == "s" || n == "texture_u" || n == "texture_s") iu = ii;
                else if (n == "v" || n == "t" || n == "texture_v" || n == "texture_t") iv = ii;
            }
            if (ix < 0 || iy < 0 || iz < 0) throw std::runtime_error("PLY vertices have no x/y/z");
            hasN = inx >= 0 && iny >= 0 && inz >= 0;
            hasT = iu >= 0 && iv >= 0;
            std::vector<double> vals(e.props.size());
            m.positions.reserve(e.count);
            for (std::size_t k = 0; k < e.count; ++k) {
                for (std::size_t i = 0; i < e.props.size(); ++i) {
                    const auto& p = e.props[i];
                    if (p.list) {
                        const auto c = static_cast<std::size_t>(r.read(p.countType));
                        for (std::size_t j = 0; j < c; ++j) r.read(p.type);
                        vals[i] = 0;
                    } else {
                        vals[i] = r.read(p.type);
                    }
                }
                m.positions.push_back({vals[static_cast<std::size_t>(ix)], vals[static_cast<std::size_t>(iy)], vals[static_cast<std::size_t>(iz)]});
                if (hasN) m.normals.push_back({vals[static_cast<std::size_t>(inx)], vals[static_cast<std::size_t>(iny)], vals[static_cast<std::size_t>(inz)]});
                if (hasT) m.uvs.push_back({vals[static_cast<std::size_t>(iu)], vals[static_cast<std::size_t>(iv)]});
            }
        } else if (e.name == "face") {
            int listProp = -1;
            for (std::size_t i = 0; i < e.props.size(); ++i) {
                if (e.props[i].list && (e.props[i].name == "vertex_indices" || e.props[i].name == "vertex_index")) listProp = static_cast<int>(i);
            }
            for (std::size_t i = 0; listProp < 0 && i < e.props.size(); ++i) {
                if (e.props[i].list) listProp = static_cast<int>(i);
            }
            std::vector<std::uint32_t> f;
            for (std::size_t k = 0; k < e.count; ++k) {
                for (std::size_t i = 0; i < e.props.size(); ++i) {
                    const auto& p = e.props[i];
                    if (!p.list) {
                        r.read(p.type);
                        continue;
                    }
                    const auto c = static_cast<std::size_t>(r.read(p.countType));
                    f.clear();
                    for (std::size_t j = 0; j < c; ++j) f.push_back(static_cast<std::uint32_t>(r.read(p.type)));
                    if (static_cast<int>(i) != listProp) continue;
                    for (std::size_t j = 1; j + 1 < f.size(); ++j) m.addTriangle(f[0], f[j], f[j + 1]);
                }
            }
        } else {
            for (std::size_t k = 0; k < e.count; ++k) {
                for (const auto& p : e.props) {
                    if (p.list) {
                        const auto c = static_cast<std::size_t>(r.read(p.countType));
                        for (std::size_t j = 0; j < c; ++j) r.read(p.type);
                    } else {
                        r.read(p.type);
                    }
                }
            }
        }
    }
    checkIndices(m, "PLY");
    cleanup(m);
    return m;
}

std::string writePly(const Mesh& m, bool ascii) {
    const bool n = m.hasNormals(), t = m.hasUvs();
    std::string out = "ply\nformat ";
    out += ascii ? "ascii" : "binary_little_endian";
    out += " 1.0\ncomment written by wetwaresuite\nelement vertex " + std::to_string(m.positions.size()) +
           "\nproperty float x\nproperty float y\nproperty float z\n";
    if (n) out += "property float nx\nproperty float ny\nproperty float nz\n";
    if (t) out += "property float s\nproperty float t\n";
    out += "element face " + std::to_string(m.triangleCount()) + "\nproperty list uchar uint vertex_indices\nend_header\n";
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        std::vector<double> vals = {m.positions[i].x, m.positions[i].y, m.positions[i].z};
        if (n) vals.insert(vals.end(), {m.normals[i].x, m.normals[i].y, m.normals[i].z});
        if (t) vals.insert(vals.end(), {m.uvs[i].x, m.uvs[i].y});
        for (std::size_t k = 0; k < vals.size(); ++k) {
            if (ascii) {
                if (k) out += ' ';
                num(out, vals[k]);
            } else {
                putF32(out, vals[k]);
            }
        }
        if (ascii) out += '\n';
    }
    for (std::size_t f = 0; f < m.triangleCount(); ++f) {
        if (ascii) {
            out += "3 " + std::to_string(m.indices[f * 3]) + ' ' + std::to_string(m.indices[f * 3 + 1]) + ' ' +
                   std::to_string(m.indices[f * 3 + 2]) + '\n';
        } else {
            putPly(out, PlyType::U8, 3);
            for (int k = 0; k < 3; ++k) putPly(out, PlyType::U32, m.indices[f * 3 + static_cast<std::size_t>(k)]);
        }
    }
    return out;
}

// ---- OFF ----

Mesh readOff(const std::string& text) {
    auto all = lines(text);
    std::vector<std::vector<std::string_view>> rows;
    for (auto l : all) {
        auto parts = split(l);
        if (!parts.empty()) rows.push_back(std::move(parts));
    }
    if (rows.empty()) throw std::runtime_error("empty OFF file");
    std::size_t row = 0;
    auto& head = rows[0];
    const std::string tag = lower(std::string(head[0]));
    if (tag.size() < 3 || tag.compare(tag.size() - 3, 3, "off") != 0) throw std::runtime_error("not an OFF file (no OFF header)");
    std::vector<std::string_view> counts(head.begin() + 1, head.end());
    ++row;
    if (counts.empty()) {
        if (row >= rows.size()) throw std::runtime_error("OFF file has no counts");
        counts = rows[row++];
    }
    if (counts.size() < 2) throw std::runtime_error("OFF counts line needs vertex and face counts");
    const auto nv = static_cast<std::size_t>(toInt(counts[0])), nf = static_cast<std::size_t>(toInt(counts[1]));
    if (row + nv + nf > rows.size()) throw std::runtime_error("OFF file is truncated");
    Mesh m;
    m.positions.reserve(nv);
    for (std::size_t i = 0; i < nv; ++i, ++row) {
        const auto& r = rows[row];
        if (r.size() < 3) throw std::runtime_error("OFF vertex line needs x y z");
        m.positions.push_back({toNumber(r[0]), toNumber(r[1]), toNumber(r[2])});
    }
    for (std::size_t i = 0; i < nf; ++i, ++row) {
        const auto& r = rows[row];
        const auto c = static_cast<std::size_t>(toInt(r[0]));
        if (r.size() < c + 1) throw std::runtime_error("OFF face line is short");
        for (std::size_t j = 2; j < c; ++j) {
            m.addTriangle(static_cast<std::uint32_t>(toInt(r[1])), static_cast<std::uint32_t>(toInt(r[j])),
                          static_cast<std::uint32_t>(toInt(r[j + 1])));
        }
    }
    checkIndices(m, "OFF");
    cleanup(m);
    return m;
}

std::string writeOff(const Mesh& m) {
    std::string out = "OFF\n" + std::to_string(m.positions.size()) + ' ' + std::to_string(m.triangleCount()) + " 0\n";
    for (const auto& p : m.positions) {
        num(out, p.x); out += ' '; num(out, p.y); out += ' '; num(out, p.z); out += '\n';
    }
    for (std::size_t f = 0; f < m.triangleCount(); ++f) {
        out += "3 " + std::to_string(m.indices[f * 3]) + ' ' + std::to_string(m.indices[f * 3 + 1]) + ' ' +
               std::to_string(m.indices[f * 3 + 2]) + '\n';
    }
    return out;
}

// ---- dispatch ----

const std::vector<std::string>& readableExtensions() {
    static const std::vector<std::string> v = {"stl", "obj", "ply", "off", "glb", "gltf", "3mf", "fbx", "step", "stp"};
    return v;
}

const std::vector<std::string>& writableExtensions() {
    static const std::vector<std::string> v = {"stl", "obj", "ply", "off", "glb", "gltf", "3mf", "fbx", "step", "stp"};
    return v;
}

bool canRead(const std::filesystem::path& path) {
    const auto& v = readableExtensions();
    return std::find(v.begin(), v.end(), extOf(path)) != v.end();
}

bool canWrite(const std::filesystem::path& path) {
    const auto& v = writableExtensions();
    return std::find(v.begin(), v.end(), extOf(path)) != v.end();
}

Mesh decode(const std::string& bytes, const std::string& extIn, const std::filesystem::path& baseDir) {
    const std::string ext = lower(extIn);
    if (ext == "stl") return readStl(bytes);
    if (ext == "obj") return readObj(bytes);
    if (ext == "ply") return readPly(bytes);
    if (ext == "off") return readOff(bytes);
    if (ext == "glb" || ext == "gltf") return readGltf(bytes, baseDir);
    if (ext == "fbx") return readFbx(bytes);
    if (ext == "step" || ext == "stp") return readStep(bytes);
    if (ext == "3mf") return read3mf(bytes);
    throw std::runtime_error("can't read ." + ext + " meshes (readable: stl, obj, ply, off, glb, gltf, 3mf, fbx, step)");
}

std::string encode(const Mesh& mesh, const std::string& extIn, const SaveOptions& o) {
    const std::string ext = lower(extIn);
    if (ext == "stl") return writeStl(mesh, o.ascii);
    if (ext == "obj") return writeObj(mesh);
    if (ext == "ply") return writePly(mesh, o.ascii);
    if (ext == "off") return writeOff(mesh);
    if (ext == "glb") return writeGlb(mesh);
    if (ext == "fbx") return writeFbx(mesh);
    if (ext == "step" || ext == "stp") return writeStep(mesh);
    if (ext == "gltf") return writeGltf(mesh);
    if (ext == "3mf") return write3mf(mesh);
    throw std::runtime_error("can't write ." + ext + " meshes (writable: stl, obj, ply, off, glb, gltf, 3mf, fbx, step)");
}

Mesh load(const std::filesystem::path& path) {
    if (!canRead(path)) throw std::runtime_error("can't read " + path.string() + ": unsupported mesh format");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        return decode(ss.str(), extOf(path), path.parent_path());
    } catch (const std::exception& e) {
        throw std::runtime_error(path.filename().string() + ": " + e.what());
    }
}

void save(const std::filesystem::path& path, const Mesh& mesh, const SaveOptions& options) {
    if (!canWrite(path)) throw std::runtime_error("can't write " + path.string() + ": unsupported mesh format");
    const std::string bytes = encode(mesh, extOf(path), options);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + path.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("failed writing " + path.string());
}

}  // namespace wetwaresuite::mesh
