// Writing TrueType fonts table by table (head, hhea, maxp, OS/2, name, cmap
// formats 4 and 12, loca, glyf, hmtx, post, kern, GPOS pair adjustment), WOFF
// 1.0 and SVG fonts.
#include <algorithm>
#include <cctype>
#include <functional>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

#include "wetwaresuite/pdf/page.hpp"
#include "internal.hpp"

namespace wetwaresuite::typeface {

namespace {

struct TtGlyph {
    // simple
    std::vector<std::vector<std::pair<int, int>>> contours;
    std::vector<std::vector<bool>> onCurve;
    // composite
    struct Ref {
        unsigned gid;
        Component c;
    };
    std::vector<Ref> refs;
    int xMin = 0, yMin = 0, xMax = 0, yMax = 0;
    bool empty = true;
    int points = 0, contourCount = 0;  // of the decomposed outline (maxp)
    int depth = 0;
    int advance = 0;
};

bool f2dot14(double v) { return v >= -2.0 && v < 2.0; }

void setBox(TtGlyph& t, const std::vector<Contour>& quads) {
    t.empty = true;
    t.points = 0;
    t.contourCount = 0;
    for (const auto& c : quads) {
        if (c.points.empty()) continue;
        ++t.contourCount;
        for (const auto& p : c.points) {
            const int x = toInt(p.x), y = toInt(p.y);
            if (t.empty) {
                t.xMin = t.xMax = x;
                t.yMin = t.yMax = y;
                t.empty = false;
            }
            t.xMin = std::min(t.xMin, x);
            t.xMax = std::max(t.xMax, x);
            t.yMin = std::min(t.yMin, y);
            t.yMax = std::max(t.yMax, y);
            ++t.points;
        }
    }
}

std::vector<Contour> quadratic(const std::vector<Contour>& contours, double tol) {
    std::vector<Contour> out;
    for (const auto& c : contours) {
        if (c.points.size() < 2) continue;
        Contour q = toQuadratic(c, tol);
        // drop repeated on-curve points
        Contour clean;
        for (const auto& p : q.points) {
            if (!clean.points.empty()) {
                const Point& b = clean.points.back();
                if (p.on() && b.on() && toInt(p.x) == toInt(b.x) && toInt(p.y) == toInt(b.y)) continue;
            }
            clean.points.push_back(p);
        }
        if (clean.points.size() > 1) {
            const Point& f = clean.points.front();
            const Point& b = clean.points.back();
            if (b.on() && toInt(b.x) == toInt(f.x) && toInt(b.y) == toInt(f.y)) clean.points.pop_back();
        }
        if (clean.points.size() >= 2) out.push_back(std::move(clean));
    }
    return out;
}

std::string encodeSimple(const TtGlyph& t) {
    Bytes b;
    if (t.empty) return {};
    b.i16(static_cast<int>(t.contours.size()));
    b.i16(t.xMin);
    b.i16(t.yMin);
    b.i16(t.xMax);
    b.i16(t.yMax);
    int end = -1;
    for (const auto& c : t.contours) {
        end += static_cast<int>(c.size());
        b.u16(static_cast<unsigned>(end));
    }
    b.u16(0);  // no instructions
    std::string flags, xs, ys;
    int px = 0, py = 0;
    for (std::size_t ci = 0; ci < t.contours.size(); ++ci) {
        for (std::size_t i = 0; i < t.contours[ci].size(); ++i) {
            const auto [x, y] = t.contours[ci][i];
            unsigned f = t.onCurve[ci][i] ? 1 : 0;
            const int dx = x - px, dy = y - py;
            if (dx == 0) {
                f |= 16;
            } else if (dx > -256 && dx < 256) {
                f |= 2;
                if (dx > 0) f |= 16;
                xs += static_cast<char>(std::abs(dx));
            } else {
                xs += static_cast<char>((dx >> 8) & 0xFF);
                xs += static_cast<char>(dx & 0xFF);
            }
            if (dy == 0) {
                f |= 32;
            } else if (dy > -256 && dy < 256) {
                f |= 4;
                if (dy > 0) f |= 32;
                ys += static_cast<char>(std::abs(dy));
            } else {
                ys += static_cast<char>((dy >> 8) & 0xFF);
                ys += static_cast<char>(dy & 0xFF);
            }
            flags += static_cast<char>(f);
            px = x;
            py = y;
        }
    }
    b.raw(flags);
    b.raw(xs);
    b.raw(ys);
    return b.data;
}

std::string encodeComposite(const TtGlyph& t) {
    Bytes b;
    b.i16(-1);
    b.i16(t.xMin);
    b.i16(t.yMin);
    b.i16(t.xMax);
    b.i16(t.yMax);
    for (std::size_t i = 0; i < t.refs.size(); ++i) {
        const auto& r = t.refs[i];
        const Component& c = r.c;
        unsigned flags = 0x0001 | 0x0002 | 0x0004;  // word args, xy offsets, round to grid
        if (i + 1 < t.refs.size()) flags |= 0x0020;
        const bool linear = !(c.xx == 1 && c.xy == 0 && c.yx == 0 && c.yy == 1);
        if (linear) {
            if (c.xy == 0 && c.yx == 0) flags |= c.xx == c.yy ? 0x0008 : 0x0040;
            else flags |= 0x0080;
        }
        b.u16(flags);
        b.u16(r.gid);
        b.i16(clamp16(c.dx));
        b.i16(clamp16(c.dy));
        auto f2 = [&](double v) { b.i16(static_cast<int>(std::lround(v * 16384))); };
        if (flags & 0x0008) f2(c.xx);
        if (flags & 0x0040) {
            f2(c.xx);
            f2(c.yy);
        }
        if (flags & 0x0080) {
            f2(c.xx);
            f2(c.xy);
            f2(c.yx);
            f2(c.yy);
        }
    }
    return b.data;
}

std::string asciiOnly(const std::string& s) {
    std::string out;
    for (std::uint32_t cp : utf8Decode(s)) out += cp < 0x80 ? static_cast<char>(cp) : '?';
    return out;
}

std::string postScriptName(const Font& font) {
    std::string out;
    for (char c : asciiOnly(font.info.family + "-" + font.info.style)) {
        const auto u = static_cast<unsigned char>(c);
        if (u > 32 && u < 127 && std::string("[](){}<>/%").find(c) == std::string::npos) out += c;
    }
    if (out.size() > 63) out.resize(63);
    if (out.empty() || out == "-") out = "Untitled-Regular";
    return out;
}

bool isBold(const Font& f) { return f.info.weight >= 700 || f.info.style.find("Bold") != std::string::npos; }
bool isItalic(const Font& f) {
    return f.metrics.italicAngle != 0 || f.info.style.find("Italic") != std::string::npos ||
           f.info.style.find("Oblique") != std::string::npos;
}

std::string nameTable(const Font& font) {
    const bool ribbi = font.info.style == "Regular" || font.info.style == "Bold" || font.info.style == "Italic" ||
                       font.info.style == "Bold Italic";
    std::map<unsigned, std::string> names;
    auto set = [&](unsigned id, const std::string& v) {
        if (!v.empty()) names[id] = v;
    };
    set(0, font.info.copyright);
    if (ribbi) {
        set(1, font.info.family);
        set(2, font.info.style);
    } else {
        set(1, font.info.family + " " + font.info.style);
        set(2, isItalic(font) ? (isBold(font) ? "Bold Italic" : "Italic") : "Regular");
        set(16, font.info.family);
        set(17, font.info.style);
    }
    const std::string ps = postScriptName(font);
    set(3, font.info.version + ";" + font.info.vendor + ";" + ps);
    set(4, font.info.family + " " + font.info.style);
    set(5, "Version " + font.info.version);
    set(6, ps);
    set(8, font.info.manufacturer);
    set(9, font.info.designer);
    set(10, font.info.description);
    set(13, font.info.license);
    set(14, font.info.licenseUrl);
    struct Rec {
        unsigned plat, enc, lang, id;
        std::string bytes;
    };
    std::vector<Rec> recs;
    for (const auto& [id, v] : names) recs.push_back({1, 0, 0, id, asciiOnly(v)});
    for (const auto& [id, v] : names) {
        std::string u;
        for (std::uint32_t cp : utf8Decode(v)) {
            auto put = [&](unsigned w) {
                u += static_cast<char>((w >> 8) & 0xFF);
                u += static_cast<char>(w & 0xFF);
            };
            if (cp >= 0x10000) {
                cp -= 0x10000;
                put(0xD800 + (cp >> 10));
                put(0xDC00 + (cp & 0x3FF));
            } else {
                put(cp);
            }
        }
        recs.push_back({3, 1, 0x409, id, u});
    }
    Bytes b;
    b.u16(0);
    b.u16(static_cast<unsigned>(recs.size()));
    b.u16(static_cast<unsigned>(6 + 12 * recs.size()));
    std::string storage;
    for (const auto& r : recs) {
        b.u16(r.plat);
        b.u16(r.enc);
        b.u16(r.lang);
        b.u16(r.id);
        b.u16(static_cast<unsigned>(std::min<std::size_t>(r.bytes.size(), 0xFFFF)));
        b.u16(static_cast<unsigned>(storage.size()));
        storage += r.bytes;
    }
    b.raw(storage);
    return b.data;
}

std::string cmapTable(const std::map<std::uint32_t, unsigned>& map, std::vector<std::string>* warnings) {
    // format 4: runs of consecutive code points with consecutive glyph ids
    struct Seg {
        unsigned start, end, delta;
    };
    std::vector<Seg> segs;
    for (const auto& [cp, g] : map) {
        if (cp >= 0xFFFF) break;
        if (!segs.empty() && segs.back().end + 1 == cp && ((g - cp) & 0xFFFF) == segs.back().delta) segs.back().end = cp;
        else segs.push_back({cp, cp, (g - cp) & 0xFFFF});
    }
    if (16 + 8 * (segs.size() + 1) > 0xFFFF) {
        if (warnings) warnings->push_back("cmap format 4 is full; some BMP characters are only in format 12");
        while (16 + 8 * (segs.size() + 1) > 0xFFFF) segs.pop_back();
    }
    segs.push_back({0xFFFF, 0xFFFF, 1});
    Bytes f4;
    const unsigned n = static_cast<unsigned>(segs.size());
    unsigned pow2 = 1, log2 = 0;
    while (pow2 * 2 <= n) {
        pow2 *= 2;
        ++log2;
    }
    f4.u16(4);
    f4.u16(16 + 8 * n);
    f4.u16(0);
    f4.u16(2 * n);
    f4.u16(2 * pow2);
    f4.u16(log2);
    f4.u16(2 * n - 2 * pow2);
    for (const auto& s : segs) f4.u16(s.end);
    f4.u16(0);
    for (const auto& s : segs) f4.u16(s.start);
    for (const auto& s : segs) f4.u16(s.delta);
    for (std::size_t i = 0; i < segs.size(); ++i) f4.u16(0);
    // format 12: sequential groups over everything
    struct Group {
        std::uint32_t start, end, gid;
    };
    std::vector<Group> groups;
    for (const auto& [cp, g] : map) {
        if (!groups.empty() && groups.back().end + 1 == cp && groups.back().gid + (cp - groups.back().start) == g) groups.back().end = cp;
        else groups.push_back({cp, cp, g});
    }
    Bytes f12;
    f12.u16(12);
    f12.u16(0);
    f12.u32(static_cast<std::uint32_t>(16 + 12 * groups.size()));
    f12.u32(0);
    f12.u32(static_cast<std::uint32_t>(groups.size()));
    for (const auto& g : groups) {
        f12.u32(g.start);
        f12.u32(g.end);
        f12.u32(g.gid);
    }
    Bytes b;
    b.u16(0);
    b.u16(3);
    const std::uint32_t off4 = 4 + 8 * 3, off12 = off4 + static_cast<std::uint32_t>(f4.size());
    b.u16(0);
    b.u16(3);
    b.u32(off4);
    b.u16(3);
    b.u16(1);
    b.u32(off4);
    b.u16(3);
    b.u16(10);
    b.u32(off12);
    b.raw(f4.data);
    b.raw(f12.data);
    return b.data;
}

std::string kernTable(const std::vector<std::pair<std::pair<unsigned, unsigned>, int>>& pairs) {
    const unsigned n = static_cast<unsigned>(std::min<std::size_t>(pairs.size(), 10920));
    Bytes b;
    b.u16(0);
    b.u16(1);
    b.u16(0);
    b.u16(14 + 6 * n);
    b.u16(1);
    b.u16(n);
    unsigned pow2 = 1, log2 = 0;
    while (pow2 * 2 <= n) {
        pow2 *= 2;
        ++log2;
    }
    b.u16(n ? 6 * pow2 : 0);
    b.u16(n ? log2 : 0);
    b.u16(n ? 6 * n - 6 * pow2 : 0);
    for (unsigned i = 0; i < n; ++i) {
        b.u16(pairs[i].first.first);
        b.u16(pairs[i].first.second);
        b.i16(pairs[i].second);
    }
    return b.data;
}

std::string gposTable(const std::vector<std::pair<std::pair<unsigned, unsigned>, int>>& pairs, std::vector<std::string>* warnings) {
    // group by first glyph
    std::map<unsigned, std::vector<std::pair<unsigned, int>>> byFirst;
    for (const auto& [p, v] : pairs) byFirst[p.first].push_back({p.second, v});
    // PairPos format 1 subtables, each kept under 64 KB
    std::vector<std::string> subtables;
    auto it = byFirst.begin();
    while (it != byFirst.end()) {
        std::vector<unsigned> firsts;
        std::vector<std::string> sets;
        std::size_t size = 10;
        while (it != byFirst.end()) {
            const std::size_t setSize = 2 + 4 * it->second.size();
            if (!firsts.empty() && size + setSize + 2 + 2 * (firsts.size() + 1) + 4 > 60000) break;
            Bytes s;
            s.u16(static_cast<unsigned>(it->second.size()));
            for (const auto& [g2, v] : it->second) {
                s.u16(g2);
                s.i16(v);
            }
            firsts.push_back(it->first);
            sets.push_back(s.data);
            size += setSize + 2;
            ++it;
        }
        Bytes t;
        const std::size_t header = 10 + 2 * firsts.size();
        t.u16(1);
        std::size_t setsSize = 0;
        for (const auto& s : sets) setsSize += s.size();
        t.u16(static_cast<unsigned>(header + setsSize));  // coverage after the pair sets
        t.u16(4);
        t.u16(0);
        t.u16(static_cast<unsigned>(firsts.size()));
        std::size_t off = header;
        for (const auto& s : sets) {
            t.u16(static_cast<unsigned>(off));
            off += s.size();
        }
        for (const auto& s : sets) t.raw(s);
        t.u16(1);
        t.u16(static_cast<unsigned>(firsts.size()));
        for (unsigned g : firsts) t.u16(g);
        subtables.push_back(t.data);
    }
    // The first lookup holds subtable offsets as 16 bits, so it takes what fits in ~60 KB
    // (every reader, stb_truetype too, understands it); the rest goes in an extension
    // lookup (type 9) whose 32-bit offsets have no such limit.
    std::size_t total = 6, keep = 0;
    for (const auto& s : subtables) {
        if (total + 2 + s.size() > 60000) break;
        total += 2 + s.size();
        ++keep;
    }
    Bytes lookup;
    lookup.u16(2);
    lookup.u16(0);
    lookup.u16(static_cast<unsigned>(keep));
    std::size_t off = 6 + 2 * keep;
    for (std::size_t i = 0; i < keep; ++i) {
        lookup.u16(static_cast<unsigned>(off));
        off += subtables[i].size();
    }
    for (std::size_t i = 0; i < keep; ++i) lookup.raw(subtables[i]);
    Bytes extension;
    const std::size_t rest = std::min<std::size_t>(subtables.size() - keep, 6000);
    if (rest < subtables.size() - keep && warnings) warnings->push_back("GPOS kerning was cut short (more than 6000 subtables)");
    if (rest) {
        extension.u16(9);
        extension.u16(0);
        extension.u16(static_cast<unsigned>(rest));
        const std::size_t records = 6 + 2 * rest;
        for (std::size_t i = 0; i < rest; ++i) extension.u16(static_cast<unsigned>(records + 8 * i));
        std::size_t data = records + 8 * rest;
        for (std::size_t i = 0; i < rest; ++i) {
            extension.u16(1);
            extension.u16(2);
            extension.u32(static_cast<std::uint32_t>(data - (records + 8 * i)));
            data += subtables[keep + i].size();
        }
        for (std::size_t i = 0; i < rest; ++i) extension.raw(subtables[keep + i]);
    }
    const unsigned lookupCount = rest ? 2 : 1;

    Bytes scripts;  // DFLT and latn, each with a default LangSys using feature 0
    scripts.u16(2);
    scripts.tag("DFLT");
    scripts.u16(14);
    scripts.tag("latn");
    scripts.u16(26);
    for (int i = 0; i < 2; ++i) {
        scripts.u16(4);       // default LangSys offset
        scripts.u16(0);       // no other LangSys
        scripts.u16(0);       // lookupOrder
        scripts.u16(0xFFFF);  // no required feature
        scripts.u16(1);
        scripts.u16(0);
    }
    Bytes features;
    features.u16(1);
    features.tag("kern");
    features.u16(8);
    features.u16(0);
    features.u16(lookupCount);
    for (unsigned i = 0; i < lookupCount; ++i) features.u16(i);
    Bytes lookups;
    lookups.u16(lookupCount);
    lookups.u16(2 + 2 * lookupCount);
    if (rest) lookups.u16(static_cast<unsigned>(2 + 2 * lookupCount + lookup.size()));
    lookups.raw(lookup.data);
    lookups.raw(extension.data);

    Bytes b;
    b.u16(1);
    b.u16(0);
    const std::size_t s0 = 10, f0 = s0 + scripts.size(), l0 = f0 + features.size();
    b.u16(static_cast<unsigned>(s0));
    b.u16(static_cast<unsigned>(f0));
    b.u16(static_cast<unsigned>(l0));
    b.raw(scripts.data);
    b.raw(features.data);
    b.raw(lookups.data);
    return b.data;
}

std::uint32_t checksum(const std::string& data) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); i += 4) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | (i + k < data.size() ? static_cast<unsigned char>(data[i + k]) : 0u);
        sum += v;
    }
    return sum;
}

}  // namespace

std::map<std::string, std::string> buildTables(const Font& font, const TtfOptions& o) {
    const auto& m = font.metrics;
    // glyph order: .notdef first
    std::vector<const Glyph*> order;
    Glyph notdef;
    notdef.name = ".notdef";
    notdef.advance = std::round(m.unitsPerEm * 0.5);
    if (int i = font.index(".notdef"); i >= 0) order.push_back(&font.glyphs[static_cast<std::size_t>(i)]);
    else order.push_back(&notdef);
    for (const auto& g : font.glyphs) {
        if (g.name != ".notdef") order.push_back(&g);
    }
    if (order.size() > 65535) throw std::runtime_error("a TrueType font holds at most 65535 glyphs");
    std::map<std::string, unsigned> gid;
    for (std::size_t i = 0; i < order.size(); ++i) gid.emplace(order[i]->name, static_cast<unsigned>(i));

    // outlines
    std::vector<TtGlyph> tt(order.size());
    std::function<int(const Glyph&, int)> depthOf = [&](const Glyph& g, int d) {
        int best = d;
        if (d > 8) return d;
        for (const auto& c : g.components) {
            if (const Glyph* b = font.glyph(c.base); b && font.index(c.base) >= 0) best = std::max(best, depthOf(*b, d + 1));
        }
        return best;
    };
    for (std::size_t i = 0; i < order.size(); ++i) {
        const Glyph& g = *order[i];
        TtGlyph& t = tt[i];
        t.advance = std::max(0, toInt(g.advance));
        bool composite = g.contours.empty() && !g.components.empty();
        for (const auto& c : g.components) {
            if (!gid.count(c.base) || c.base == g.name || !f2dot14(c.xx) || !f2dot14(c.xy) || !f2dot14(c.yx) || !f2dot14(c.yy))
                composite = false;
        }
        const auto quads = quadratic(outline(font, g), o.tolerance);
        setBox(t, quads);
        if (composite && depthOf(g, 0) <= 8) {
            for (const auto& c : g.components) t.refs.push_back({gid[c.base], c});
            t.depth = depthOf(g, 0);
        } else {
            for (const auto& c : quads) {
                std::vector<std::pair<int, int>> pts;
                std::vector<bool> on;
                for (const auto& p : c.points) {
                    pts.push_back({toInt(p.x), toInt(p.y)});
                    on.push_back(p.on());
                }
                t.contours.push_back(std::move(pts));
                t.onCurve.push_back(std::move(on));
            }
        }
    }

    std::map<std::string, std::string> tables;
    // glyf + loca (long offsets)
    Bytes glyf, loca;
    for (const auto& t : tt) {
        loca.u32(static_cast<std::uint32_t>(glyf.size()));
        glyf.raw(t.refs.empty() ? encodeSimple(t) : encodeComposite(t));
        glyf.pad4();
    }
    loca.u32(static_cast<std::uint32_t>(glyf.size()));
    tables["glyf"] = glyf.data;
    tables["loca"] = loca.data;

    // global numbers
    int xMin = 0, yMin = 0, xMax = 0, yMax = 0;
    bool any = false;
    int advMax = 0, minLsb = 0, minRsb = 0, maxExtent = 0;
    int maxPoints = 0, maxContours = 0, maxCompPoints = 0, maxCompContours = 0, maxElements = 0, maxDepth = 0;
    double advSum = 0;
    int advCount = 0;
    std::set<int> widths;
    for (const auto& t : tt) {
        advMax = std::max(advMax, t.advance);
        if (t.advance > 0) {
            advSum += t.advance;
            ++advCount;
            widths.insert(t.advance);
        }
        if (t.empty) continue;
        if (!any) {
            xMin = t.xMin;
            yMin = t.yMin;
            xMax = t.xMax;
            yMax = t.yMax;
            minLsb = t.xMin;
            minRsb = t.advance - t.xMax;
            maxExtent = t.xMax;
            any = true;
        }
        xMin = std::min(xMin, t.xMin);
        yMin = std::min(yMin, t.yMin);
        xMax = std::max(xMax, t.xMax);
        yMax = std::max(yMax, t.yMax);
        minLsb = std::min(minLsb, t.xMin);
        minRsb = std::min(minRsb, t.advance - t.xMax);
        maxExtent = std::max(maxExtent, t.xMax);
        if (t.refs.empty()) {
            maxPoints = std::max(maxPoints, t.points);
            maxContours = std::max(maxContours, t.contourCount);
        } else {
            maxCompPoints = std::max(maxCompPoints, t.points);
            maxCompContours = std::max(maxCompContours, t.contourCount);
            maxElements = std::max(maxElements, static_cast<int>(t.refs.size()));
            maxDepth = std::max(maxDepth, t.depth);
        }
    }
    const bool bold = isBold(font), italic = isItalic(font);

    // head
    {
        Bytes b;
        b.u32(0x00010000);
        double rev = 1.0;
        try {
            rev = std::stod(font.info.version);
        } catch (...) {
        }
        b.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(rev * 65536))));
        b.u32(0);  // checkSumAdjustment, set by assembleSfnt
        b.u32(0x5F0F3CF5);
        b.u16(0x000B);
        b.u16(static_cast<unsigned>(m.unitsPerEm));
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::int64_t mac = static_cast<std::int64_t>(now) + 2082844800LL;  // seconds since 1904
        b.i64(mac);
        b.i64(mac);
        b.i16(xMin);
        b.i16(yMin);
        b.i16(xMax);
        b.i16(yMax);
        b.u16((bold ? 1u : 0u) | (italic ? 2u : 0u));
        b.u16(8);
        b.i16(2);
        b.i16(1);  // long loca
        b.i16(0);
        tables["head"] = b.data;
    }
    // hhea
    {
        Bytes b;
        b.u32(0x00010000);
        b.i16(toInt(m.ascender));
        b.i16(toInt(m.descender));
        b.i16(toInt(m.lineGap));
        b.u16(static_cast<unsigned>(advMax));
        b.i16(minLsb);
        b.i16(minRsb);
        b.i16(maxExtent);
        if (m.italicAngle != 0) {
            b.i16(m.unitsPerEm);
            b.i16(toInt(std::tan(-m.italicAngle * kPi / 180) * m.unitsPerEm));
        } else {
            b.i16(1);
            b.i16(0);
        }
        b.i16(0);
        for (int i = 0; i < 4; ++i) b.i16(0);
        b.i16(0);
        b.u16(static_cast<unsigned>(tt.size()));
        tables["hhea"] = b.data;
    }
    // hmtx
    {
        Bytes b;
        for (const auto& t : tt) {
            b.u16(static_cast<unsigned>(t.advance));
            b.i16(t.empty ? 0 : t.xMin);
        }
        tables["hmtx"] = b.data;
    }
    // maxp 1.0
    {
        Bytes b;
        b.u32(0x00010000);
        b.u16(static_cast<unsigned>(tt.size()));
        b.u16(static_cast<unsigned>(maxPoints));
        b.u16(static_cast<unsigned>(maxContours));
        b.u16(static_cast<unsigned>(maxCompPoints));
        b.u16(static_cast<unsigned>(maxCompContours));
        b.u16(2);
        for (int i = 0; i < 6; ++i) b.u16(0);
        b.u16(static_cast<unsigned>(maxElements));
        b.u16(static_cast<unsigned>(maxDepth));
        tables["maxp"] = b.data;
    }
    // cmap
    std::map<std::uint32_t, unsigned> cmap;
    for (std::size_t i = 0; i < order.size(); ++i) {
        for (auto u : order[i]->unicodes) cmap.emplace(u, static_cast<unsigned>(i));
    }
    tables["cmap"] = cmapTable(cmap, o.warnings);
    // OS/2 version 4
    {
        Bytes b;
        const double em = m.unitsPerEm;
        b.u16(4);
        b.i16(advCount ? toInt(advSum / advCount) : 0);
        b.u16(static_cast<unsigned>(std::clamp(font.info.weight, 1, 1000)));
        b.u16(5);
        b.u16(0);  // installable embedding
        b.i16(toInt(em * 0.65));
        b.i16(toInt(em * 0.6));
        b.i16(0);
        b.i16(toInt(em * 0.075));
        b.i16(toInt(em * 0.65));
        b.i16(toInt(em * 0.6));
        b.i16(0);
        b.i16(toInt(em * 0.35));
        b.i16(toInt(m.underlineThickness));
        b.i16(toInt(m.xHeight * 0.5 + m.underlineThickness * 0.5));
        b.i16(0);
        for (int i = 0; i < 10; ++i) b.u8(0);  // panose
        std::uint32_t range1 = 0;
        unsigned first = 0xFFFF, last = 0;
        for (const auto& [cp, g] : cmap) {
            if (cp < 0x80) range1 |= 1u;
            else if (cp < 0x100) range1 |= 2u;
            else if (cp < 0x180) range1 |= 8u;
            first = std::min<unsigned>(first, std::min<std::uint32_t>(cp, 0xFFFF));
            last = std::max<unsigned>(last, std::min<std::uint32_t>(cp, 0xFFFF));
        }
        if (cmap.empty()) first = last = 0;
        b.u32(range1);
        b.u32(0);
        b.u32(0);
        b.u32(0);
        std::string vendor = asciiOnly(font.info.vendor);
        vendor.resize(4, ' ');
        b.raw(vendor);
        unsigned sel = 0x80;  // USE_TYPO_METRICS
        if (italic) sel |= 0x01;
        if (bold) sel |= 0x20;
        if (!italic && !bold) sel |= 0x40;
        b.u16(sel);
        b.u16(first);
        b.u16(last);
        b.i16(toInt(m.ascender));
        b.i16(toInt(m.descender));
        b.i16(toInt(m.lineGap));
        b.u16(static_cast<unsigned>(std::max(std::max(yMax, toInt(m.ascender)), 0)));
        b.u16(static_cast<unsigned>(std::max(std::max(-yMin, -toInt(m.descender)), 0)));
        b.u32(1);  // Latin 1 code page
        b.u32(0);
        b.i16(toInt(m.xHeight));
        b.i16(toInt(m.capHeight));
        b.u16(0);
        b.u16(0x20);
        b.u16(font.kerning.empty() ? 1 : 2);
        tables["OS/2"] = b.data;
    }
    tables["name"] = nameTable(font);
    // post format 2
    {
        Bytes b;
        b.u32(0x00020000);
        b.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(m.italicAngle * 65536))));
        b.i16(toInt(m.underlinePosition));
        b.i16(toInt(m.underlineThickness));
        b.u32(widths.size() == 1 ? 1 : 0);
        for (int i = 0; i < 4; ++i) b.u32(0);
        b.u16(static_cast<unsigned>(order.size()));
        std::map<std::string, unsigned> mac;
        for (std::size_t i = 0; i < macGlyphNames().size(); ++i) mac.emplace(macGlyphNames()[i], static_cast<unsigned>(i));
        std::string strings;
        unsigned next = 258;
        for (const Glyph* g : order) {
            auto it = mac.find(g->name);
            if (it != mac.end()) {
                b.u16(it->second);
                continue;
            }
            std::string n = asciiOnly(g->name);
            if (n.size() > 63) n.resize(63);
            b.u16(next++);
            strings += static_cast<char>(n.size());
            strings += n;
        }
        b.raw(strings);
        tables["post"] = b.data;
    }
    // kerning
    std::vector<std::pair<std::pair<unsigned, unsigned>, int>> pairs;
    for (const auto& [p, v] : font.kerning) {
        auto a = gid.find(p.first), c = gid.find(p.second);
        const int k = toInt(v);
        if (a == gid.end() || c == gid.end() || k == 0) continue;
        pairs.push_back({{a->second, c->second}, k});
    }
    std::sort(pairs.begin(), pairs.end());
    if (!pairs.empty()) {
        if (o.kerning == "both" || o.kerning == "kern") {
            tables["kern"] = kernTable(pairs);
            if (pairs.size() > 10920 && o.warnings && o.kerning == "kern") o.warnings->push_back("the kern table holds 10920 pairs; the rest were dropped (use kerning=both or gpos)");
        }
        if (o.kerning == "both" || o.kerning == "gpos") tables["GPOS"] = gposTable(pairs, o.warnings);
    }
    return tables;
}

std::string assembleSfnt(const std::map<std::string, std::string>& tables, std::uint32_t flavor) {
    const unsigned n = static_cast<unsigned>(tables.size());
    unsigned pow2 = 1, log2 = 0;
    while (pow2 * 2 <= n) {
        pow2 *= 2;
        ++log2;
    }
    Bytes b;
    b.u32(flavor);
    b.u16(n);
    b.u16(16 * pow2);
    b.u16(log2);
    b.u16(16 * n - 16 * pow2);
    std::size_t off = 12 + 16 * static_cast<std::size_t>(n);
    std::size_t headAt = 0;
    std::string body;
    for (const auto& [tag, raw] : tables) {
        std::string data = raw;
        if (tag == "head" && data.size() >= 12) {
            for (int i = 8; i < 12; ++i) data[static_cast<std::size_t>(i)] = 0;
            headAt = off + body.size();
        }
        b.tag(tag);
        b.u32(checksum(data));
        b.u32(static_cast<std::uint32_t>(off + body.size()));
        b.u32(static_cast<std::uint32_t>(data.size()));
        body += data;
        while (body.size() % 4) body += '\0';
    }
    b.raw(body);
    if (headAt) b.put32(headAt + 8, 0xB1B0AFBAu - checksum(b.data));
    return b.data;
}

std::string toTtf(const Font& font, const TtfOptions& options) { return assembleSfnt(buildTables(font, options)); }

std::string toWoff(const std::string& sfnt) {
    if (sfnt.size() < 12) throw std::runtime_error("not an sfnt font");
    auto u16 = [&](std::size_t at) {
        return (static_cast<unsigned>(static_cast<unsigned char>(sfnt[at])) << 8) | static_cast<unsigned char>(sfnt[at + 1]);
    };
    auto u32 = [&](std::size_t at) { return (static_cast<std::uint32_t>(u16(at)) << 16) | u16(at + 2); };
    const std::uint32_t flavor = u32(0);
    const unsigned n = u16(4);
    if (sfnt.size() < 12 + 16 * static_cast<std::size_t>(n)) throw std::runtime_error("truncated sfnt");
    struct Entry {
        std::string tag, data;
        std::uint32_t orig, sum;
    };
    std::vector<Entry> entries;
    std::size_t sfntSize = 12 + 16 * static_cast<std::size_t>(n);
    for (unsigned i = 0; i < n; ++i) {
        const std::size_t at = 12 + 16 * static_cast<std::size_t>(i);
        const std::uint32_t sum = u32(at + 4), off = u32(at + 8), length = u32(at + 12);
        if (off > sfnt.size() || length > sfnt.size() - off) throw std::runtime_error("truncated sfnt table");
        std::string raw = sfnt.substr(off, length);
        std::string comp = pdf::deflate(raw, 9);
        entries.push_back({sfnt.substr(at, 4), comp.size() < raw.size() ? comp : raw, length, sum});
        sfntSize += (length + 3) & ~3u;
    }
    Bytes b;
    b.tag("wOFF");
    b.u32(flavor);
    const std::size_t lengthAt = b.size();
    b.u32(0);
    b.u16(n);
    b.u16(0);
    b.u32(static_cast<std::uint32_t>(sfntSize));
    b.u16(1);
    b.u16(0);
    for (int i = 0; i < 5; ++i) b.u32(0);
    std::size_t off = 44 + 20 * static_cast<std::size_t>(n);
    for (const auto& e : entries) {
        b.tag(e.tag);
        b.u32(static_cast<std::uint32_t>(off));
        b.u32(static_cast<std::uint32_t>(e.data.size()));
        b.u32(e.orig);
        b.u32(e.sum);
        off += (e.data.size() + 3) & ~std::size_t{3};
    }
    for (const auto& e : entries) {
        b.raw(e.data);
        b.pad4();
    }
    b.put32(lengthAt, static_cast<std::uint32_t>(b.size()));
    return b.data;
}

std::string toSvgFont(const Font& font) {
    const auto& m = font.metrics;
    std::string id;
    for (char c : font.info.family + font.info.style) {
        if (std::isalnum(static_cast<unsigned char>(c))) id += c;
    }
    if (id.empty()) id = "font";
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\">\n<defs>\n";
    double avg = 0;
    for (const auto& g : font.glyphs) avg += g.advance;
    avg = font.glyphs.empty() ? m.unitsPerEm / 2.0 : avg / static_cast<double>(font.glyphs.size());
    out += "<font id=\"" + xmlEscape(id) + "\" horiz-adv-x=\"" + fmt(std::round(avg)) + "\">\n";
    out += "<font-face font-family=\"" + xmlEscape(font.info.family) + "\" font-weight=\"" + std::to_string(font.info.weight) +
           "\" font-style=\"" + (isItalic(font) ? "italic" : "normal") + "\" units-per-em=\"" + std::to_string(m.unitsPerEm) +
           "\" ascent=\"" + fmt(m.ascender) + "\" descent=\"" + fmt(m.descender) + "\" x-height=\"" + fmt(m.xHeight) +
           "\" cap-height=\"" + fmt(m.capHeight) + "\"" + (m.italicAngle != 0 ? " slope=\"" + fmt(m.italicAngle) + "\"" : "") +
           " />\n";
    if (const Glyph* nd = font.glyph(".notdef")) {
        out += "<missing-glyph horiz-adv-x=\"" + fmt(nd->advance) + "\" d=\"" + pathData(outline(font, *nd)) + "\" />\n";
    }
    for (const auto& g : font.glyphs) {
        if (g.name == ".notdef") continue;
        const std::string d = pathData(outline(font, g));
        if (g.unicodes.empty()) {
            out += "<glyph glyph-name=\"" + xmlEscape(g.name) + "\" horiz-adv-x=\"" + fmt(g.advance) + "\"" +
                   (d.empty() ? "" : " d=\"" + d + "\"") + " />\n";
            continue;
        }
        for (auto u : g.unicodes) {
            std::string ch;
            if (u < 0x20 || u == '"' || u == '&' || u == '<' || u == '>' || u == '\'') ch = "&#x" + unicodeLabel(u).substr(2) + ";";
            else ch = utf8Encode(u);
            out += "<glyph glyph-name=\"" + xmlEscape(g.name) + "\" unicode=\"" + ch + "\" horiz-adv-x=\"" + fmt(g.advance) + "\"" +
                   (d.empty() ? "" : " d=\"" + d + "\"") + " />\n";
        }
    }
    for (const auto& [p, v] : font.kerning) {
        if (v == 0) continue;
        // SVG kerning subtracts: k is the amount to move the glyphs closer
        out += "<hkern g1=\"" + xmlEscape(p.first) + "\" g2=\"" + xmlEscape(p.second) + "\" k=\"" + fmt(-v) + "\" />\n";
    }
    out += "</font>\n</defs>\n</svg>\n";
    return out;
}

}  // namespace wetwaresuite::typeface
