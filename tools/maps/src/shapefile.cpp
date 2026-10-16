// ESRI shapefiles: .shp geometry, .shx index, .dbf attributes, .prj and .cpg.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include "internal.hpp"

namespace achclwss::maps {

namespace fs = std::filesystem;
using namespace detail;

namespace {

struct Reader {
    const std::string& b;
    std::size_t size() const { return b.size(); }
    std::uint32_t u8(std::size_t o) const { return o < b.size() ? static_cast<unsigned char>(b[o]) : 0u; }
    std::int32_t be32(std::size_t o) const { return static_cast<std::int32_t>((u8(o) << 24) | (u8(o + 1) << 16) | (u8(o + 2) << 8) | u8(o + 3)); }
    std::int32_t le32(std::size_t o) const { return static_cast<std::int32_t>(u8(o) | (u8(o + 1) << 8) | (u8(o + 2) << 16) | (u8(o + 3) << 24)); }
    std::uint16_t le16(std::size_t o) const { return static_cast<std::uint16_t>(u8(o) | (u8(o + 1) << 8)); }
    double f64(std::size_t o) const {
        std::uint64_t v = 0;
        for (int k = 7; k >= 0; --k) v = (v << 8) | u8(o + static_cast<std::size_t>(k));
        double d = 0;
        std::memcpy(&d, &v, 8);
        return d;
    }
};

struct Writer {
    std::string b;
    void be32(std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        for (int k = 3; k >= 0; --k) b += static_cast<char>((u >> (8 * k)) & 0xFF);
    }
    void le32(std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        for (int k = 0; k < 4; ++k) b += static_cast<char>((u >> (8 * k)) & 0xFF);
    }
    void le16(std::uint16_t v) {
        b += static_cast<char>(v & 0xFF);
        b += static_cast<char>(v >> 8);
    }
    void f64(double d) {
        std::uint64_t v = 0;
        std::memcpy(&v, &d, 8);
        for (int k = 0; k < 8; ++k) b += static_cast<char>((v >> (8 * k)) & 0xFF);
    }
    void putBe32(std::size_t at, std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        for (int k = 0; k < 4; ++k) b[at + static_cast<std::size_t>(k)] = static_cast<char>((u >> (8 * (3 - k))) & 0xFF);
    }
};

fs::path sibling(const fs::path& shp, const std::string& ext) {
    fs::path p = shp;
    p.replace_extension(ext);
    if (fs::exists(p)) return p;
    std::string up = ext;
    for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    p.replace_extension(up);
    return p;
}

std::vector<Props> readDbf(const std::string& bytes, bool utf8Hint, std::vector<std::string>* warnings) {
    Reader r{bytes};
    std::vector<Props> rows;
    if (bytes.size() < 32) return rows;
    const std::uint32_t count = static_cast<std::uint32_t>(r.le32(4));
    const std::size_t headerLen = r.le16(8), recordLen = r.le16(10);
    struct Field {
        std::string name;
        char type;
        std::size_t offset, length;
    };
    std::vector<Field> fields;
    std::size_t off = 1;
    for (std::size_t p = 32; p + 32 <= headerLen && p < bytes.size() && bytes[p] != '\x0d'; p += 32) {
        std::string name(bytes.data() + p, 11);
        name = name.substr(0, name.find('\0'));
        Field f{trim(name), bytes[p + 11], off, static_cast<std::size_t>(r.u8(p + 16))};
        if (f.type == 'C') f.length += static_cast<std::size_t>(r.u8(p + 17)) << 8;  // long character fields
        off += f.length;
        if (!validUtf8(f.name)) f.name = latin1ToUtf8(f.name);
        fields.push_back(f);
    }
    std::size_t latin1 = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t base = headerLen + static_cast<std::size_t>(i) * recordLen;
        if (base + recordLen > bytes.size()) break;
        Props p;
        if (bytes[base] == '*') {
            rows.push_back(p);  // deleted: keep the slot so indexes match the .shp
            continue;
        }
        for (const auto& f : fields) {
            std::string v = trim(bytes.substr(base + f.offset, f.length));
            if (!v.empty() && !validUtf8(v)) {
                v = latin1ToUtf8(v);
                ++latin1;
            }
            switch (f.type) {
                case 'N':
                case 'F':
                case 'O': {
                    if (v.empty() || v.find_first_not_of("*") == std::string::npos) p[f.name] = Prop::null();
                    else p[f.name] = Prop::number(std::strtod(v.c_str(), nullptr));
                    break;
                }
                case 'L':
                    if (v == "T" || v == "t" || v == "Y" || v == "y") p[f.name] = Prop::boolean(true);
                    else if (v == "F" || v == "f" || v == "N" || v == "n") p[f.name] = Prop::boolean(false);
                    else p[f.name] = Prop::null();
                    break;
                case 'D':
                    p[f.name] = v.size() == 8 ? Prop::string(v.substr(0, 4) + "-" + v.substr(4, 2) + "-" + v.substr(6, 2)) : Prop::null();
                    break;
                default: p[f.name] = v.empty() ? Prop::null() : Prop::string(v);
            }
        }
        rows.push_back(std::move(p));
    }
    if (latin1 && warnings && utf8Hint) warnings->push_back("some .dbf text wasn't UTF-8 and was read as Latin-1");
    return rows;
}

}  // namespace

Layer readShapefile(const fs::path& shp, const std::optional<Crs>& crsOverride, std::vector<std::string>* warnings) {
    const std::string bytes = readFile(shp);
    Reader r{bytes};
    if (bytes.size() < 100 || r.be32(0) != 9994) throw std::runtime_error(shp.string() + " is not a shapefile (.shp)");
    std::optional<Crs> crs = crsOverride;
    const fs::path prj = sibling(shp, ".prj");
    if (!crs && fs::exists(prj)) {
        const std::string wkt = readFile(prj);
        crs = Crs::parse(wkt);
        if (!crs && warnings) warnings->push_back("the .prj coordinate system isn't one maps knows (WGS84, Web Mercator, UTM/transverse Mercator); coordinates were read as lon/lat — pass crs= to say what they are");
    }
    bool utf8 = true;
    if (const fs::path cpg = sibling(shp, ".cpg"); fs::exists(cpg)) {
        const std::string enc = lower(trim(readFile(cpg)));
        utf8 = enc.find("utf") != std::string::npos || enc.find("65001") != std::string::npos;
    }
    std::vector<Props> rows;
    if (const fs::path dbf = sibling(shp, ".dbf"); fs::exists(dbf)) rows = readDbf(readFile(dbf), utf8, warnings);
    else if (warnings) warnings->push_back("no .dbf next to the .shp: features have no attributes");

    Layer layer;
    layer.source = shp.string();
    std::size_t pos = 100, index = 0, skipped = 0;
    while (pos + 12 <= bytes.size()) {
        const std::size_t len = static_cast<std::size_t>(r.be32(pos + 4)) * 2;
        const std::size_t c = pos + 8;
        if (c + len > bytes.size()) break;
        const int type = r.le32(c);
        Feature f;
        auto zAt = [&](std::size_t zOff, std::size_t n, std::vector<Coord*> cs) {
            if (zOff + 16 + n * 8 > c + len) return;
            for (std::size_t i = 0; i < n && i < cs.size(); ++i) cs[i]->z = r.f64(zOff + 16 + i * 8);
        };
        if (type == 1 || type == 11 || type == 21) {
            f.geom.type = GeomType::Point;
            Coord p{r.f64(c + 4), r.f64(c + 12)};
            if (type == 11) p.z = r.f64(c + 20);
            f.geom.points.push_back(p);
        } else if (type == 8 || type == 18 || type == 28) {
            f.geom.type = GeomType::Point;
            const std::size_t n = static_cast<std::size_t>(r.le32(c + 36));
            for (std::size_t i = 0; i < n && c + 40 + i * 16 + 16 <= c + len; ++i) f.geom.points.push_back({r.f64(c + 40 + i * 16), r.f64(c + 48 + i * 16)});
            if (type == 18) {
                std::vector<Coord*> cs;
                for (auto& p : f.geom.points) cs.push_back(&p);
                zAt(c + 40 + n * 16, n, cs);
            }
        } else if (type == 3 || type == 13 || type == 23 || type == 5 || type == 15 || type == 25) {
            const std::size_t parts = static_cast<std::size_t>(r.le32(c + 36)), n = static_cast<std::size_t>(r.le32(c + 40));
            const std::size_t ptsAt = c + 44 + parts * 4;
            if (ptsAt + n * 16 > c + len) {
                ++skipped;
            } else {
                std::vector<Line> chains;
                for (std::size_t k = 0; k < parts; ++k) {
                    const std::size_t a = static_cast<std::size_t>(r.le32(c + 44 + k * 4));
                    const std::size_t b = k + 1 < parts ? static_cast<std::size_t>(r.le32(c + 44 + (k + 1) * 4)) : n;
                    Line l;
                    for (std::size_t i = a; i < b && i < n; ++i) l.push_back({r.f64(ptsAt + i * 16), r.f64(ptsAt + i * 16 + 8)});
                    chains.push_back(std::move(l));
                }
                if (type == 13 || type == 15) {
                    std::vector<Coord*> cs;
                    for (auto& l : chains) {
                        for (auto& p : l) cs.push_back(&p);
                    }
                    zAt(ptsAt + n * 16, n, cs);
                }
                if (type == 3 || type == 13 || type == 23) {
                    f.geom.type = GeomType::Line;
                    for (auto& l : chains) {
                        if (l.size() >= 2) f.geom.lines.push_back(std::move(l));
                    }
                } else {
                    f.geom.type = GeomType::Polygon;
                    // clockwise rings are outer rings, counter-clockwise ones holes
                    std::vector<Ring> outers, holes;
                    for (auto& l : chains) {
                        if (l.size() > 1 && l.front().x == l.back().x && l.front().y == l.back().y) l.pop_back();
                        if (l.size() < 3) continue;
                        (signedArea(l) < 0 ? outers : holes).push_back(std::move(l));
                    }
                    if (outers.empty()) {
                        for (auto& h : holes) {
                            std::reverse(h.begin(), h.end());
                            f.geom.polygons.push_back({std::move(h)});
                        }
                    } else {
                        for (auto& o : outers) {
                            std::reverse(o.begin(), o.end());  // stored counter-clockwise
                            f.geom.polygons.push_back({std::move(o)});
                        }
                        for (auto& h : holes) {
                            int best = -1;
                            double bestA = 0;
                            for (std::size_t k = 0; k < f.geom.polygons.size(); ++k) {
                                const Ring& o = f.geom.polygons[k][0];
                                if (!pointInRing(o, h[0].x, h[0].y)) continue;
                                const double a = std::abs(signedArea(o));
                                if (best < 0 || a < bestA) {
                                    best = static_cast<int>(k);
                                    bestA = a;
                                }
                            }
                            std::reverse(h.begin(), h.end());
                            if (best >= 0) f.geom.polygons[static_cast<std::size_t>(best)].push_back(std::move(h));
                            else f.geom.polygons.push_back({std::move(h)});
                        }
                    }
                }
            }
        } else if (type != 0) {
            ++skipped;
        }
        if (f.geom.empty()) f.geom.type = GeomType::None;
        if (crs && !crs->geographic()) {
            f.geom.forEach([&](Coord& p) {
                const Coord ll = crs->inverse(p);
                p.x = ll.x;
                p.y = ll.y;
            });
        }
        if (index < rows.size()) f.props = rows[index];
        layer.features.push_back(std::move(f));
        ++index;
        pos = c + len;
    }
    if (skipped && warnings) warnings->push_back(std::to_string(skipped) + " shapes of an unsupported type (e.g. MultiPatch) were read without geometry");
    return layer;
}

// ---- writing ----------------------------------------------------------------------------

namespace {

struct DbfField {
    std::string key, name;
    char type = 'C';
    std::size_t length = 1;
    int decimals = 0;
};

std::string dbfBytes(const std::vector<const Feature*>& feats, const std::vector<std::string>& keys) {
    std::vector<DbfField> fields;
    std::set<std::string> used;
    for (const auto& k : keys) {
        DbfField f;
        f.key = k;
        // 10 ASCII bytes, unique
        std::string n;
        for (char c : k) {
            if (n.size() >= 10) break;
            n += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
        }
        if (n.empty()) n = "field";
        std::string base = n;
        for (int i = 1; used.count(lower(n)); ++i) {
            const std::string suf = std::to_string(i);
            n = base.substr(0, 10 - suf.size()) + suf;
        }
        used.insert(lower(n));
        f.name = n;
        bool allNum = true, allBool = true, any = false;
        int decimals = 0;
        std::size_t maxLen = 1;
        for (const Feature* ft : feats) {
            auto it = ft->props.find(k);
            if (it == ft->props.end() || it->second.isNull()) continue;
            any = true;
            const Prop& p = it->second;
            if (p.kind != Prop::Number) allNum = false;
            if (p.kind != Prop::Bool) allBool = false;
            if (p.kind == Prop::Number) {
                const std::string t = fmt(p.num, 10);
                const auto dot = t.find('.');
                if (dot != std::string::npos) decimals = std::max(decimals, static_cast<int>(t.size() - dot - 1));
            }
            maxLen = std::max(maxLen, p.text().size());
        }
        if (any && allBool) {
            f.type = 'L';
            f.length = 1;
        } else if (any && allNum) {
            f.type = 'N';
            f.decimals = std::min(decimals, 10);
            f.length = std::min<std::size_t>(std::max<std::size_t>(maxLen + 1, 10) + (f.decimals ? 2 : 0), 30);
        } else {
            f.type = 'C';
            f.length = std::min<std::size_t>(maxLen, 254);
        }
        fields.push_back(f);
    }
    Writer w;
    w.b += '\x03';
    // last update date, YY MM DD
    const auto days = std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now().time_since_epoch()).count() / 24;
    int y = 0;
    unsigned m = 0, d = 0;
    civilFromDays(days, y, m, d);
    w.b += static_cast<char>(y - 1900);
    w.b += static_cast<char>(m);
    w.b += static_cast<char>(d);
    w.le32(static_cast<std::int32_t>(feats.size()));
    std::size_t recordLen = 1;
    for (const auto& f : fields) recordLen += f.length;
    w.le16(static_cast<std::uint16_t>(32 + fields.size() * 32 + 1));
    w.le16(static_cast<std::uint16_t>(recordLen));
    w.b += std::string(20, '\0');
    for (const auto& f : fields) {
        std::string n = f.name;
        n.resize(11, '\0');
        w.b += n;
        w.b += f.type;
        w.b += std::string(4, '\0');
        w.b += static_cast<char>(f.length);
        w.b += static_cast<char>(f.decimals);
        w.b += std::string(14, '\0');
    }
    w.b += '\x0d';
    for (const Feature* ft : feats) {
        w.b += ' ';
        for (const auto& f : fields) {
            auto it = ft->props.find(f.key);
            std::string v;
            if (it != ft->props.end() && !it->second.isNull()) {
                const Prop& p = it->second;
                if (f.type == 'N') {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%.*f", f.decimals, p.asNumber().value_or(0));
                    v = buf;
                } else if (f.type == 'L') {
                    v = p.truthy() ? "T" : "F";
                } else {
                    v = p.text();
                    if (v.size() > f.length) {
                        v.resize(f.length);
                        // don't cut a UTF-8 sequence in half
                        while (!v.empty() && (static_cast<unsigned char>(v.back()) & 0xC0) == 0x80) v.pop_back();
                        if (!v.empty() && static_cast<unsigned char>(v.back()) >= 0xC0) v.pop_back();
                    }
                }
            } else if (f.type == 'L') {
                v = "?";
            }
            if (f.type == 'N') v = std::string(f.length > v.size() ? f.length - v.size() : 0, ' ') + v.substr(0, f.length);
            else v.resize(f.length, ' ');
            w.b += v;
        }
    }
    w.b += '\x1a';
    return w.b;
}

}  // namespace

std::vector<std::string> writeShapefile(const Layer& layer, const fs::path& shp0, const ExportOptions& o) {
    std::map<GeomType, std::vector<const Feature*>> groups;
    for (const auto& f : layer.features) {
        if (f.geom.type != GeomType::None && !f.geom.empty()) groups[f.geom.type].push_back(&f);
    }
    if (groups.empty()) throw std::runtime_error("layer '" + layer.name + "' has no geometry to write");
    const auto keys = layer.fields();
    std::vector<std::string> written;
    fs::path base = shp0;
    if (lower(base.extension().string()) == ".shp") base.replace_extension();
    for (const auto& [type, feats] : groups) {
        fs::path stem = base;
        if (groups.size() > 1) stem += std::string("_") + (type == GeomType::Point ? "points" : type == GeomType::Line ? "lines" : "polygons");
        bool hasZ = false, multi = false;
        for (const Feature* f : feats) {
            f->geom.forEach([&](const Coord& c) { hasZ = hasZ || !std::isnan(c.z); });
            multi = multi || f->geom.points.size() > 1;
        }
        int shapeType = type == GeomType::Point ? (multi ? 8 : 1) : type == GeomType::Line ? 3 : 5;
        if (hasZ) shapeType += 10;
        Writer shp, shx;
        BBox all;
        std::vector<std::pair<std::int32_t, std::int32_t>> index;
        auto header = [&](Writer& w) {
            w.be32(9994);
            for (int i = 0; i < 5; ++i) w.be32(0);
            w.be32(0);  // length, patched later
            w.le32(1000);
            w.le32(shapeType);
            for (int i = 0; i < 8; ++i) w.f64(0);
        };
        header(shp);
        header(shx);
        std::int32_t num = 1;
        for (const Feature* f : feats) {
            Geometry g = f->geom;
            g.forEach([&](Coord& c) {
                const Coord p = o.crs.forward(c);
                c.x = p.x;
                c.y = p.y;
            });
            BBox b = bboxOf(g);
            all.add(b);
            Writer rec;
            rec.le32(shapeType);
            std::vector<Coord> pts;
            std::vector<std::int32_t> parts;
            if (shapeType == 1 || shapeType == 11) {
                const Coord& p = g.points[0];
                rec.f64(p.x);
                rec.f64(p.y);
                if (shapeType == 11) {
                    rec.f64(std::isnan(p.z) ? 0 : p.z);
                    rec.f64(0);
                }
            } else {
                if (type == GeomType::Point) {
                    pts = g.points;
                } else if (type == GeomType::Line) {
                    for (const auto& l : g.lines) {
                        parts.push_back(static_cast<std::int32_t>(pts.size()));
                        pts.insert(pts.end(), l.begin(), l.end());
                    }
                } else {
                    for (auto p : g.polygons) {
                        for (std::size_t i = 0; i < p.size(); ++i) {
                            Ring r = p[i];
                            const bool cw = signedArea(r) < 0;
                            if ((i == 0) != cw) std::reverse(r.begin(), r.end());  // outer clockwise, holes counter-clockwise
                            parts.push_back(static_cast<std::int32_t>(pts.size()));
                            pts.insert(pts.end(), r.begin(), r.end());
                            pts.push_back(r.front());
                        }
                    }
                }
                rec.f64(b.minX);
                rec.f64(b.minY);
                rec.f64(b.maxX);
                rec.f64(b.maxY);
                if (type != GeomType::Point) rec.le32(static_cast<std::int32_t>(parts.size()));
                rec.le32(static_cast<std::int32_t>(pts.size()));
                for (auto p : parts) rec.le32(p);
                for (const auto& p : pts) {
                    rec.f64(p.x);
                    rec.f64(p.y);
                }
                if (hasZ) {
                    double zmin = 0, zmax = 0;
                    bool first = true;
                    for (const auto& p : pts) {
                        const double z = std::isnan(p.z) ? 0 : p.z;
                        zmin = first ? z : std::min(zmin, z);
                        zmax = first ? z : std::max(zmax, z);
                        first = false;
                    }
                    rec.f64(zmin);
                    rec.f64(zmax);
                    for (const auto& p : pts) rec.f64(std::isnan(p.z) ? 0 : p.z);
                }
            }
            index.push_back({static_cast<std::int32_t>(shp.b.size() / 2), static_cast<std::int32_t>(rec.b.size() / 2)});
            shp.be32(num++);
            shp.be32(static_cast<std::int32_t>(rec.b.size() / 2));
            shp.b += rec.b;
        }
        for (const auto& [off, len] : index) {
            shx.be32(off);
            shx.be32(len);
        }
        for (Writer* w : {&shp, &shx}) {
            w->putBe32(24, static_cast<std::int32_t>(w->b.size() / 2));
            Writer box;
            box.f64(all.minX);
            box.f64(all.minY);
            box.f64(all.maxX);
            box.f64(all.maxY);
            w->b.replace(36, 32, box.b);
        }
        fs::path p = stem;
        auto put = [&](const std::string& ext, const std::string& bytes) {
            fs::path f = stem;
            f += ext;
            writeFile(f, bytes);
            written.push_back(f.string());
        };
        put(".shp", shp.b);
        put(".shx", shx.b);
        put(".dbf", dbfBytes(feats, keys));
        put(".prj", o.crs.wkt());
        put(".cpg", "UTF-8");
    }
    return written;
}

}  // namespace achclwss::maps
