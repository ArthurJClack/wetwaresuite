// Mapbox Vector Tile 2.1 decoder (protobuf by hand): layers, keys/values,
// features with MoveTo/LineTo/ClosePath zigzag geometry.
#include <cstring>
#include <stdexcept>

#include "wetwaresuite/maps/tiles.hpp"

namespace wetwaresuite::maps {

namespace {

struct Pb {
    const std::string& b;
    std::size_t p = 0, end = 0;
    Pb(const std::string& bytes, std::size_t from, std::size_t to) : b(bytes), p(from), end(to) {}
    bool more() const { return p < end; }
    std::uint64_t varint() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= end) throw std::runtime_error("vector tile is cut short");
            const unsigned char c = static_cast<unsigned char>(b[p++]);
            v |= static_cast<std::uint64_t>(c & 0x7F) << shift;
            if (!(c & 0x80)) return v;
        }
        throw std::runtime_error("vector tile has a broken number");
    }
    // a length-delimited field: [from, to)
    std::pair<std::size_t, std::size_t> bytes() {
        const std::uint64_t n = varint();
        if (n > end - p) throw std::runtime_error("vector tile is cut short");
        const std::size_t from = p;
        p += static_cast<std::size_t>(n);
        return {from, p};
    }
    std::uint64_t fixed(int n) {
        if (p + n > end) throw std::runtime_error("vector tile is cut short");
        std::uint64_t v = 0;
        for (int i = n - 1; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b[p + i]);
        p += n;
        return v;
    }
    void skip(int wire) {
        switch (wire) {
            case 0: varint(); break;
            case 1: fixed(8); break;
            case 2: bytes(); break;
            case 5: fixed(4); break;
            default: throw std::runtime_error("vector tile has an unknown field type");
        }
    }
};

std::int64_t zigzag(std::uint64_t v) { return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1); }

Prop value(const std::string& b, std::size_t from, std::size_t to) {
    Pb pb(b, from, to);
    Prop out;
    while (pb.more()) {
        const std::uint64_t key = pb.varint();
        const int field = static_cast<int>(key >> 3), wire = static_cast<int>(key & 7);
        if (field == 1 && wire == 2) {
            auto [f, t] = pb.bytes();
            out = Prop::string(b.substr(f, t - f));
        } else if (field == 2 && wire == 5) {
            const auto bits = static_cast<std::uint32_t>(pb.fixed(4));
            float fv = 0;
            std::memcpy(&fv, &bits, 4);
            out = Prop::number(fv);
        } else if (field == 3 && wire == 1) {
            const std::uint64_t bits = pb.fixed(8);
            double dv = 0;
            std::memcpy(&dv, &bits, 8);
            out = Prop::number(dv);
        } else if (field == 4 && wire == 0) {
            out = Prop::number(static_cast<double>(static_cast<std::int64_t>(pb.varint())));
        } else if (field == 5 && wire == 0) {
            out = Prop::number(static_cast<double>(pb.varint()));
        } else if (field == 6 && wire == 0) {
            out = Prop::number(static_cast<double>(zigzag(pb.varint())));
        } else if (field == 7 && wire == 0) {
            out = Prop::boolean(pb.varint() != 0);
        } else {
            pb.skip(wire);
        }
    }
    return out;
}

double mvtRingArea(const Ring& r) {
    double a = 0;
    for (std::size_t i = 0, n = r.size(); i < n; ++i) a += r[i].x * r[(i + 1) % n].y - r[(i + 1) % n].x * r[i].y;
    return a / 2;
}

Geometry geometry(int type, const std::vector<std::uint32_t>& cmds) {
    Geometry g;
    std::vector<std::vector<Coord>> parts;
    std::int64_t x = 0, y = 0;
    for (std::size_t i = 0; i < cmds.size();) {
        const std::uint32_t c = cmds[i++];
        const std::uint32_t id = c & 7, count = c >> 3;
        if (id == 7) {
            continue;  // ClosePath: rings are closed implicitly
        }
        if (id != 1 && id != 2) throw std::runtime_error("vector tile has an unknown geometry command");
        for (std::uint32_t k = 0; k < count; ++k) {
            if (i + 2 > cmds.size()) throw std::runtime_error("vector tile geometry is cut short");
            x += zigzag(cmds[i]);
            y += zigzag(cmds[i + 1]);
            i += 2;
            if (id == 1 && type != 1) parts.emplace_back();
            if (parts.empty()) parts.emplace_back();
            parts.back().push_back({static_cast<double>(x), static_cast<double>(y)});
        }
    }
    if (type == 1) {
        g.type = GeomType::Point;
        for (auto& p : parts) g.points.insert(g.points.end(), p.begin(), p.end());
    } else if (type == 2) {
        g.type = GeomType::Line;
        for (auto& p : parts) {
            if (p.size() >= 2) g.lines.push_back(std::move(p));
        }
    } else if (type == 3) {
        g.type = GeomType::Polygon;
        for (auto& r : parts) {
            if (r.size() > 1 && r.front().x == r.back().x && r.front().y == r.back().y) r.pop_back();
            if (r.size() < 3) continue;
            const double a = mvtRingArea(r);
            if (a == 0) continue;
            // the spec: positive area (clockwise on screen) starts a polygon, negative ones are its holes
            if (a > 0 || g.polygons.empty()) g.polygons.push_back({std::move(r)});
            else g.polygons.back().push_back(std::move(r));
        }
    }
    if (g.empty()) g.type = GeomType::None;
    return g;
}

void readFeature(const std::string& b, std::size_t from, std::size_t to, std::vector<std::uint32_t>& tags, int& type, std::vector<std::uint32_t>& cmds) {
    Pb pb(b, from, to);
    tags.clear();
    cmds.clear();
    type = 0;
    const auto packed = [&](std::vector<std::uint32_t>& out) {
        auto [f, t] = pb.bytes();
        Pb in(b, f, t);
        while (in.more()) out.push_back(static_cast<std::uint32_t>(in.varint()));
    };
    while (pb.more()) {
        const std::uint64_t key = pb.varint();
        const int field = static_cast<int>(key >> 3), wire = static_cast<int>(key & 7);
        if (field == 2 && wire == 2) packed(tags);
        else if (field == 3 && wire == 0) type = static_cast<int>(pb.varint());
        else if (field == 4 && wire == 2) packed(cmds);
        else pb.skip(wire);
    }
}

MvtLayer layer(const std::string& b, std::size_t from, std::size_t to) {
    MvtLayer l;
    std::vector<std::string> keys;
    std::vector<Prop> values;
    std::vector<std::pair<std::size_t, std::size_t>> features;
    Pb pb(b, from, to);
    while (pb.more()) {
        const std::uint64_t key = pb.varint();
        const int field = static_cast<int>(key >> 3), wire = static_cast<int>(key & 7);
        if (field == 1 && wire == 2) {
            auto [f, t] = pb.bytes();
            l.name = b.substr(f, t - f);
        } else if (field == 2 && wire == 2) {
            features.push_back(pb.bytes());
        } else if (field == 3 && wire == 2) {
            auto [f, t] = pb.bytes();
            keys.push_back(b.substr(f, t - f));
        } else if (field == 4 && wire == 2) {
            auto [f, t] = pb.bytes();
            values.push_back(value(b, f, t));
        } else if (field == 5 && wire == 0) {
            l.extent = static_cast<int>(pb.varint());
        } else {
            pb.skip(wire);
        }
    }
    if (l.extent <= 0) l.extent = 4096;
    std::vector<std::uint32_t> tags, cmds;
    for (const auto& [f, t] : features) {
        int type = 0;
        readFeature(b, f, t, tags, type, cmds);
        Feature feat;
        feat.geom = geometry(type, cmds);
        if (feat.geom.type == GeomType::None) continue;
        for (std::size_t i = 0; i + 1 < tags.size(); i += 2) {
            if (tags[i] < keys.size() && tags[i + 1] < values.size()) feat.props[keys[tags[i]]] = values[tags[i + 1]];
        }
        l.features.push_back(std::move(feat));
    }
    return l;
}

}  // namespace

std::vector<MvtLayer> decodeMvt(const std::string& bytes) {
    std::vector<MvtLayer> out;
    Pb pb(bytes, 0, bytes.size());
    while (pb.more()) {
        const std::uint64_t key = pb.varint();
        const int field = static_cast<int>(key >> 3), wire = static_cast<int>(key & 7);
        if (field == 3 && wire == 2) {
            auto [f, t] = pb.bytes();
            out.push_back(layer(bytes, f, t));
        } else {
            pb.skip(wire);
        }
    }
    return out;
}

}  // namespace wetwaresuite::maps
