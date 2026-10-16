// Reading TrueType / OpenType (glyf and CFF outlines) and WOFF 1.0 fonts.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

#include "achclwss/pdf/page.hpp"
#include "internal.hpp"

namespace achclwss::typeface {

namespace {

class Reader {
public:
    Reader(const std::string& d, std::size_t base = 0, std::size_t size = std::string::npos)
        : d_(&d), base_(base), size_(size == std::string::npos ? (base <= d.size() ? d.size() - base : 0) : size) {
        if (base_ > d_->size() || size_ > d_->size() - base_) throw std::runtime_error("font table runs past the end of the file");
    }
    std::size_t size() const { return size_; }
    bool fits(std::size_t off, std::size_t n) const { return off <= size_ && n <= size_ - off; }
    unsigned u8(std::size_t off) const {
        need(off, 1);
        return static_cast<unsigned char>((*d_)[base_ + off]);
    }
    unsigned u16(std::size_t off) const {
        need(off, 2);
        return (static_cast<unsigned>(static_cast<unsigned char>((*d_)[base_ + off])) << 8) | static_cast<unsigned char>((*d_)[base_ + off + 1]);
    }
    int i16(std::size_t off) const { return static_cast<std::int16_t>(static_cast<std::uint16_t>(u16(off))); }
    std::uint32_t u32(std::size_t off) const { return (static_cast<std::uint32_t>(u16(off)) << 16) | u16(off + 2); }
    std::int32_t i32(std::size_t off) const { return static_cast<std::int32_t>(u32(off)); }
    std::string str(std::size_t off, std::size_t n) const {
        need(off, n);
        return d_->substr(base_ + off, n);
    }
    Reader sub(std::size_t off, std::size_t n = std::string::npos) const {
        if (off > size_) throw std::runtime_error("font table offset out of range");
        if (n == std::string::npos) n = size_ - off;
        need(off, n);
        return Reader(*d_, base_ + off, n);
    }

private:
    void need(std::size_t off, std::size_t n) const {
        if (!fits(off, n)) throw std::runtime_error("font table is truncated or corrupt");
    }
    const std::string* d_;
    std::size_t base_, size_;
};

using Tables = std::map<std::string, std::pair<std::size_t, std::size_t>>;

std::string utf16be(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        std::uint32_t c = (static_cast<unsigned char>(s[i]) << 8) | static_cast<unsigned char>(s[i + 1]);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < s.size()) {
            const std::uint32_t lo = (static_cast<unsigned char>(s[i + 2]) << 8) | static_cast<unsigned char>(s[i + 3]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        out += utf8Encode(c);
    }
    return out;
}

std::map<std::uint32_t, unsigned> readCmap(const Reader& t, std::vector<std::string>& warnings) {
    std::map<std::uint32_t, unsigned> map;
    const unsigned n = t.u16(2);
    int bestScore = -1;
    std::size_t best = 0;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned plat = t.u16(4 + 8 * i), enc = t.u16(6 + 8 * i);
        const std::uint32_t off = t.u32(8 + 8 * i);
        if (!t.fits(off, 2)) continue;
        const unsigned format = t.u16(off);
        int score = -1;
        if (format == 12 && ((plat == 3 && enc == 10) || plat == 0)) score = 5;
        else if (format == 4 && ((plat == 3 && enc == 1) || plat == 0)) score = 4;
        else if (format == 4 && plat == 3 && enc == 0) score = 3;
        else if ((format == 6 || format == 0) && (plat == 1 || plat == 0 || plat == 3)) score = 1;
        if (score > bestScore) {
            bestScore = score;
            best = off;
        }
    }
    if (bestScore < 0) {
        warnings.push_back("no Unicode cmap subtable this reader knows (formats 0, 4, 6, 12)");
        return map;
    }
    const unsigned format = t.u16(best);
    std::size_t budget = 2000000;
    if (format == 4) {
        const unsigned segX2 = t.u16(best + 6);
        const std::size_t ends = best + 14, starts = ends + segX2 + 2, deltas = starts + segX2, ranges = deltas + segX2;
        for (unsigned s = 0; s < segX2 / 2; ++s) {
            const unsigned end = t.u16(ends + 2 * s), start = t.u16(starts + 2 * s);
            const unsigned delta = t.u16(deltas + 2 * s), ro = t.u16(ranges + 2 * s);
            for (unsigned c = start; c <= end && c != 0xFFFF && budget; ++c, --budget) {
                unsigned g = 0;
                if (ro == 0) {
                    g = (c + delta) & 0xFFFF;
                } else {
                    const std::size_t addr = ranges + 2 * s + ro + 2 * (c - start);
                    if (!t.fits(addr, 2)) break;
                    g = t.u16(addr);
                    if (g) g = (g + delta) & 0xFFFF;
                }
                if (g) map[c] = g;
            }
        }
    } else if (format == 12) {
        const std::uint32_t groups = t.u32(best + 12);
        for (std::uint32_t i = 0; i < groups; ++i) {
            const std::size_t at = best + 16 + 12 * static_cast<std::size_t>(i);
            const std::uint32_t start = t.u32(at), end = std::min<std::uint32_t>(t.u32(at + 4), 0x10FFFF), g0 = t.u32(at + 8);
            for (std::uint32_t c = start; c <= end && budget; ++c, --budget) map[c] = g0 + (c - start);
        }
    } else if (format == 6) {
        const unsigned first = t.u16(best + 6), count = t.u16(best + 8);
        for (unsigned i = 0; i < count; ++i) {
            if (unsigned g = t.u16(best + 10 + 2 * i)) map[first + i] = g;
        }
    } else if (format == 0) {
        for (unsigned i = 0; i < 256; ++i) {
            if (unsigned g = t.u8(best + 6 + i)) map[i] = g;
        }
    }
    return map;
}

struct RawGlyph {
    std::vector<Contour> contours;
    struct Ref {
        unsigned glyph = 0;
        Component c;
    };
    std::vector<Ref> refs;
};

RawGlyph readGlyf(const Reader& g, std::vector<std::string>& warnings, bool& warnedPoints) {
    RawGlyph out;
    if (g.size() < 10) return out;
    const int nc = g.i16(0);
    if (nc >= 0) {
        std::vector<unsigned> ends;
        for (int i = 0; i < nc; ++i) ends.push_back(g.u16(10 + 2 * static_cast<std::size_t>(i)));
        const std::size_t np = ends.empty() ? 0 : ends.back() + 1;
        std::size_t p = 10 + 2 * static_cast<std::size_t>(nc);
        const unsigned ilen = g.u16(p);
        p += 2 + ilen;
        std::vector<unsigned> flags;
        flags.reserve(np);
        while (flags.size() < np) {
            const unsigned f = g.u8(p++);
            flags.push_back(f);
            if (f & 8) {
                unsigned rep = g.u8(p++);
                while (rep-- && flags.size() < np) flags.push_back(f);
            }
        }
        std::vector<int> xs(np), ys(np);
        int v = 0;
        for (std::size_t i = 0; i < np; ++i) {
            const unsigned f = flags[i];
            if (f & 2) {
                const int d = static_cast<int>(g.u8(p++));
                v += (f & 16) ? d : -d;
            } else if (!(f & 16)) {
                v += g.i16(p);
                p += 2;
            }
            xs[i] = v;
        }
        v = 0;
        for (std::size_t i = 0; i < np; ++i) {
            const unsigned f = flags[i];
            if (f & 4) {
                const int d = static_cast<int>(g.u8(p++));
                v += (f & 32) ? d : -d;
            } else if (!(f & 32)) {
                v += g.i16(p);
                p += 2;
            }
            ys[i] = v;
        }
        std::size_t s = 0;
        for (unsigned e : ends) {
            Contour c;
            for (std::size_t i = s; i <= e && i < np; ++i)
                c.points.push_back({static_cast<double>(xs[i]), static_cast<double>(ys[i]), (flags[i] & 1) ? PointType::On : PointType::Quad});
            s = e + 1;
            if (c.points.size() >= 2) {
                // start on an on-curve point when there is one
                auto it = std::find_if(c.points.begin(), c.points.end(), [](const Point& q) { return q.on(); });
                if (it != c.points.end()) std::rotate(c.points.begin(), it, c.points.end());
                out.contours.push_back(std::move(c));
            }
        }
        return out;
    }
    std::size_t p = 10;
    for (int guard = 0; guard < 1000; ++guard) {
        const unsigned flags = g.u16(p), idx = g.u16(p + 2);
        p += 4;
        RawGlyph::Ref r;
        r.glyph = idx;
        double a1, a2;
        if (flags & 1) {
            a1 = (flags & 2) ? static_cast<double>(g.i16(p)) : static_cast<double>(g.u16(p));
            a2 = (flags & 2) ? static_cast<double>(g.i16(p + 2)) : static_cast<double>(g.u16(p + 2));
            p += 4;
        } else {
            a1 = (flags & 2) ? static_cast<double>(static_cast<std::int8_t>(g.u8(p))) : static_cast<double>(g.u8(p));
            a2 = (flags & 2) ? static_cast<double>(static_cast<std::int8_t>(g.u8(p + 1))) : static_cast<double>(g.u8(p + 1));
            p += 2;
        }
        if (flags & 2) {
            r.c.dx = a1;
            r.c.dy = a2;
        } else if (!warnedPoints) {
            warnings.push_back("components positioned by point numbers were placed at their origin");
            warnedPoints = true;
        }
        auto f2 = [&](std::size_t at) { return g.i16(at) / 16384.0; };
        if (flags & 8) {
            r.c.xx = r.c.yy = f2(p);
            p += 2;
        } else if (flags & 0x40) {
            r.c.xx = f2(p);
            r.c.yy = f2(p + 2);
            p += 4;
        } else if (flags & 0x80) {
            r.c.xx = f2(p);
            r.c.xy = f2(p + 2);
            r.c.yx = f2(p + 4);
            r.c.yy = f2(p + 6);
            p += 8;
        }
        out.refs.push_back(r);
        if (!(flags & 0x20)) break;
    }
    return out;
}

std::vector<std::string> postNames(const Reader& t, unsigned numGlyphs) {
    std::vector<std::string> names;
    if (t.size() < 34 || t.u32(0) != 0x00020000) return names;
    const unsigned n = std::min(t.u16(32), numGlyphs);
    std::vector<unsigned> idx(n);
    for (unsigned i = 0; i < n; ++i) idx[i] = t.u16(34 + 2 * i);
    std::vector<std::string> custom;
    std::size_t p = 34 + 2 * static_cast<std::size_t>(t.u16(32));
    while (p < t.size()) {
        const unsigned l = t.u8(p);
        if (!t.fits(p + 1, l)) break;
        custom.push_back(t.str(p + 1, l));
        p += 1 + l;
    }
    const auto& mac = macGlyphNames();
    for (unsigned i = 0; i < n; ++i) {
        if (idx[i] < 258) names.push_back(mac[idx[i]]);
        else if (idx[i] - 258 < custom.size()) names.push_back(custom[idx[i] - 258]);
        else names.emplace_back();
    }
    return names;
}

// Glyph ids listed by an OpenType coverage table, in coverage order.
std::vector<unsigned> coverage(const Reader& t) {
    std::vector<unsigned> out;
    const unsigned format = t.u16(0);
    if (format == 1) {
        const unsigned n = t.u16(2);
        for (unsigned i = 0; i < n; ++i) out.push_back(t.u16(4 + 2 * i));
    } else if (format == 2) {
        const unsigned n = t.u16(2);
        for (unsigned i = 0; i < n; ++i) {
            const unsigned s = t.u16(4 + 6 * i), e = t.u16(6 + 6 * i);
            for (unsigned g = s; g <= e && out.size() < 70000; ++g) out.push_back(g);
        }
    }
    return out;
}

std::map<unsigned, unsigned> classDef(const Reader& t) {
    std::map<unsigned, unsigned> out;
    const unsigned format = t.u16(0);
    if (format == 1) {
        const unsigned start = t.u16(2), n = t.u16(4);
        for (unsigned i = 0; i < n; ++i) {
            if (unsigned c = t.u16(6 + 2 * i)) out[start + i] = c;
        }
    } else if (format == 2) {
        const unsigned n = t.u16(2);
        for (unsigned i = 0; i < n; ++i) {
            const unsigned s = t.u16(4 + 6 * i), e = t.u16(6 + 6 * i), c = t.u16(8 + 6 * i);
            for (unsigned g = s; g <= e && c && out.size() < 70000; ++g) out[g] = c;
        }
    }
    return out;
}

int popcount(unsigned v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

// XAdvance kerning from GPOS pair-adjustment lookups of the kern feature.
void readGpos(const Reader& t, std::map<std::pair<unsigned, unsigned>, int>& pairs, std::vector<std::string>& warnings) {
    if (t.u16(0) != 1) return;
    const Reader features = t.sub(t.u16(6));
    const Reader lookups = t.sub(t.u16(8));
    std::set<unsigned> wanted;
    for (unsigned i = 0, n = features.u16(0); i < n; ++i) {
        if (features.str(2 + 6 * i, 4) != "kern") continue;
        const Reader f = features.sub(features.u16(6 + 6 * i));
        for (unsigned k = 0, m = f.u16(2); k < m; ++k) wanted.insert(f.u16(4 + 2 * k));
    }
    const std::size_t limit = 300000;
    bool truncated = false;
    for (unsigned li : wanted) {
        if (li >= lookups.u16(0)) continue;
        const Reader lk = lookups.sub(lookups.u16(2 + 2 * li));
        unsigned type = lk.u16(0);
        for (unsigned s = 0, n = lk.u16(4); s < n; ++s) {
            Reader st = lk.sub(lk.u16(6 + 2 * s));
            if (type == 9) {
                if (st.u16(2) != 2) continue;
                st = st.sub(st.u32(4));
            } else if (type != 2) {
                continue;
            }
            const unsigned format = st.u16(0), vf1 = st.u16(4), vf2 = st.u16(6);
            const std::size_t size1 = 2 * static_cast<std::size_t>(popcount(vf1)), size2 = 2 * static_cast<std::size_t>(popcount(vf2));
            if (!(vf1 & 4)) continue;  // no XAdvance on the first glyph
            const std::size_t xadv = 2 * static_cast<std::size_t>(popcount(vf1 & 3));
            const auto firsts = coverage(st.sub(st.u16(2)));
            if (format == 1) {
                for (unsigned ci = 0, n2 = std::min<unsigned>(st.u16(8), static_cast<unsigned>(firsts.size())); ci < n2; ++ci) {
                    const Reader ps = st.sub(st.u16(10 + 2 * ci));
                    for (unsigned k = 0, m = ps.u16(0); k < m; ++k) {
                        const std::size_t at = 2 + k * (2 + size1 + size2);
                        const int v = ps.i16(at + 2 + xadv);
                        if (v && pairs.size() < limit) pairs.emplace(std::make_pair(firsts[ci], ps.u16(at)), v);
                        else if (v) truncated = true;
                    }
                }
            } else if (format == 2) {
                const auto c1 = classDef(st.sub(st.u16(8)));
                const auto c2 = classDef(st.sub(st.u16(10)));
                const unsigned n1 = st.u16(12), n2 = st.u16(14);
                std::map<unsigned, std::vector<unsigned>> members2;
                for (const auto& [g, c] : c2) members2[c].push_back(g);
                for (unsigned g1 : firsts) {
                    auto it = c1.find(g1);
                    const unsigned cls1 = it == c1.end() ? 0 : it->second;
                    if (cls1 >= n1) continue;
                    for (unsigned cls2 = 1; cls2 < n2; ++cls2) {
                        const std::size_t at = 16 + (static_cast<std::size_t>(cls1) * n2 + cls2) * (size1 + size2);
                        const int v = st.i16(at + xadv);
                        if (!v) continue;
                        for (unsigned g2 : members2[cls2]) {
                            if (pairs.size() >= limit) {
                                truncated = true;
                                break;
                            }
                            pairs.emplace(std::make_pair(g1, g2), v);
                        }
                    }
                }
            }
        }
    }
    if (truncated) warnings.push_back("kerning has more than 300000 pairs; the rest were skipped");
}

void readKern(const Reader& t, std::map<std::pair<unsigned, unsigned>, int>& pairs, std::vector<std::string>& warnings) {
    if (t.u16(0) != 0) {
        warnings.push_back("Apple-style kern table skipped");
        return;
    }
    std::size_t p = 4;
    for (unsigned i = 0, n = t.u16(2); i < n; ++i) {
        const unsigned length = t.u16(p + 2), coverage = t.u16(p + 4);
        if ((coverage >> 8) == 0 && (coverage & 1)) {
            for (unsigned k = 0, m = t.u16(p + 6); k < m; ++k) {
                const std::size_t at = p + 14 + 6 * static_cast<std::size_t>(k);
                if (int v = t.i16(at + 4)) pairs.emplace(std::make_pair(t.u16(at), t.u16(at + 2)), v);
            }
        }
        if (length < 6) break;
        p += length;
    }
}

}  // namespace

std::string woffToSfnt(const std::string& woff) {
    Reader r(woff);
    if (r.size() < 44 || r.str(0, 4) != "wOFF") throw std::runtime_error("not a WOFF file");
    const std::uint32_t flavor = r.u32(4);
    std::map<std::string, std::string> tables;
    for (unsigned i = 0, n = r.u16(12); i < n; ++i) {
        const std::size_t at = 44 + 20 * static_cast<std::size_t>(i);
        const std::string tag = r.str(at, 4);
        const std::uint32_t off = r.u32(at + 4), comp = r.u32(at + 8), orig = r.u32(at + 12);
        std::string data = r.str(off, comp);
        if (comp < orig) data = pdf::inflate(data);
        if (data.size() != orig) throw std::runtime_error("WOFF table " + tag + " has the wrong size after decompression");
        tables[tag] = data;
    }
    return assembleSfnt(tables, flavor);
}

Font fromSfnt(const std::string& input, std::vector<std::string>* warningsOut) {
    std::vector<std::string> warnings;
    std::string woffBytes;
    const std::string* bytes = &input;
    if (input.size() >= 4 && input.compare(0, 4, "wOF2") == 0)
        throw std::runtime_error("WOFF2 needs Brotli, which this build doesn't include; convert it to TTF or WOFF first");
    if (input.size() >= 4 && input.compare(0, 4, "wOFF") == 0) {
        woffBytes = woffToSfnt(input);
        bytes = &woffBytes;
    }
    const std::string& d = *bytes;
    Reader file(d);
    if (file.size() < 12) throw std::runtime_error("file is too small to be a font");
    std::size_t base = 0;
    if (file.str(0, 4) == "ttcf") {
        base = file.u32(12);
        if (file.u32(8) > 1) warnings.push_back("font collection: opened the first font");
    }
    const std::uint32_t version = file.u32(base);
    if (version != 0x00010000 && version != 0x74727565 && version != 0x4F54544F)
        throw std::runtime_error("not a TrueType/OpenType font (unknown sfnt version)");
    Tables tables;
    for (unsigned i = 0, n = file.u16(base + 4); i < n; ++i) {
        const std::size_t at = base + 12 + 16 * static_cast<std::size_t>(i);
        const std::uint32_t off = file.u32(at + 8), length = file.u32(at + 12);
        if (!file.fits(off, length)) throw std::runtime_error("table " + file.str(at, 4) + " runs past the end of the file");
        tables[file.str(at, 4)] = {off, length};
    }
    auto table = [&](const std::string& tag) -> std::optional<Reader> {
        auto it = tables.find(tag);
        if (it == tables.end()) return std::nullopt;
        return Reader(d, it->second.first, it->second.second);
    };
    auto head = table("head"), hhea = table("hhea"), maxp = table("maxp");
    if (!head || !hhea || !maxp) throw std::runtime_error("font is missing its head, hhea or maxp table");

    Font font;
    font.glyphs.clear();
    font.metrics.unitsPerEm = static_cast<int>(head->u16(18));
    if (font.metrics.unitsPerEm < 16) font.metrics.unitsPerEm = 1000;
    const int locFormat = head->i16(50);
    const unsigned macStyle = head->u16(44);
    font.metrics.ascender = hhea->i16(4);
    font.metrics.descender = hhea->i16(6);
    font.metrics.lineGap = hhea->i16(8);
    const unsigned numH = hhea->u16(34);
    const unsigned numGlyphs = maxp->u16(4);

    std::vector<double> advances(numGlyphs, 0.0);
    if (auto hmtx = table("hmtx")) {
        double last = 0;
        for (unsigned i = 0; i < numGlyphs; ++i) {
            if (i < numH && hmtx->fits(4 * static_cast<std::size_t>(i), 2)) last = hmtx->u16(4 * static_cast<std::size_t>(i));
            advances[i] = last;
        }
    }
    bool haveXHeight = false;
    if (auto os2 = table("OS/2"); os2 && os2->size() >= 78) {
        font.info.weight = static_cast<int>(os2->u16(4));
        std::string vendor = os2->str(58, 4);
        while (!vendor.empty() && (vendor.back() == ' ' || vendor.back() == '\0')) vendor.pop_back();
        if (!vendor.empty()) font.info.vendor = vendor;
        if (font.metrics.ascender == 0 && font.metrics.descender == 0) {
            font.metrics.ascender = os2->i16(68);
            font.metrics.descender = os2->i16(70);
            font.metrics.lineGap = os2->i16(72);
        }
        if (os2->u16(0) >= 2 && os2->size() >= 90) {
            font.metrics.xHeight = os2->i16(86);
            font.metrics.capHeight = os2->i16(88);
            haveXHeight = font.metrics.xHeight > 0 && font.metrics.capHeight > 0;
        }
    }
    std::vector<std::string> names;
    if (auto post = table("post"); post && post->size() >= 32) {
        font.metrics.italicAngle = post->i32(4) / 65536.0;
        font.metrics.underlinePosition = post->i16(8);
        font.metrics.underlineThickness = post->i16(10);
        names = postNames(*post, numGlyphs);
    }
    if (auto name = table("name"); name && name->size() >= 6) {
        std::map<unsigned, std::pair<int, std::string>> found;  // id -> (score, text)
        const unsigned count = name->u16(2), strings = name->u16(4);
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t at = 6 + 12 * static_cast<std::size_t>(i);
            if (!name->fits(at, 12)) break;
            const unsigned plat = name->u16(at), enc = name->u16(at + 2), lang = name->u16(at + 4), id = name->u16(at + 6);
            const unsigned length = name->u16(at + 8), off = name->u16(at + 10);
            if (!name->fits(strings + off, length)) continue;
            const std::string raw = name->str(strings + off, length);
            int score = 0;
            std::string text;
            if (plat == 3 && (enc == 1 || enc == 10)) {
                score = lang == 0x409 ? 3 : 2;
                text = utf16be(raw);
            } else if (plat == 0) {
                score = 2;
                text = utf16be(raw);
            } else if (plat == 1 && enc == 0) {
                score = 1;
                for (char c : raw) text += static_cast<unsigned char>(c) < 0x80 ? std::string(1, c) : utf8Encode(static_cast<unsigned char>(c));
            } else {
                continue;
            }
            auto& slot = found[id];
            if (score > slot.first) slot = {score, text};
        }
        auto get = [&](unsigned id) { return found.count(id) ? found[id].second : std::string(); };
        if (!get(16).empty()) font.info.family = get(16);
        else if (!get(1).empty()) font.info.family = get(1);
        if (!get(17).empty()) font.info.style = get(17);
        else if (!get(2).empty()) font.info.style = get(2);
        std::string v = get(5);
        if (v.rfind("Version ", 0) == 0) v = v.substr(8);
        if (auto semi = v.find(';'); semi != std::string::npos) v = v.substr(0, semi);
        if (!v.empty()) font.info.version = v;
        font.info.copyright = get(0);
        font.info.license = get(13);
        font.info.licenseUrl = get(14);
        font.info.designer = get(9);
        font.info.manufacturer = get(8);
        font.info.description = get(10);
    }
    if (font.metrics.italicAngle != 0 && font.info.style.find("Italic") == std::string::npos &&
        font.info.style.find("Oblique") == std::string::npos && (macStyle & 2))
        font.info.style += " Italic";

    std::map<std::uint32_t, unsigned> cmap;
    if (auto c = table("cmap")) cmap = readCmap(*c, warnings);

    // outlines
    std::vector<RawGlyph> raw(numGlyphs);
    auto glyf = table("glyf");
    auto loca = table("loca");
    if (glyf && loca) {
        bool warnedPoints = false;
        for (unsigned i = 0; i < numGlyphs; ++i) {
            std::size_t a, b;
            if (locFormat == 0) {
                a = 2 * static_cast<std::size_t>(loca->u16(2 * static_cast<std::size_t>(i)));
                b = 2 * static_cast<std::size_t>(loca->u16(2 * static_cast<std::size_t>(i) + 2));
            } else {
                a = loca->u32(4 * static_cast<std::size_t>(i));
                b = loca->u32(4 * static_cast<std::size_t>(i) + 4);
            }
            if (b <= a || !glyf->fits(a, b - a)) continue;
            try {
                raw[i] = readGlyf(glyf->sub(a, b - a), warnings, warnedPoints);
            } catch (const std::exception&) {
                warnings.push_back("glyph " + std::to_string(i) + " is corrupt and was left empty");
            }
        }
    } else if (tables.count("CFF ")) {
        std::vector<std::vector<Contour>> outlines;
        std::string error;
        if (!cffOutlines(d, base, outlines, error)) throw std::runtime_error("can't read the CFF outlines: " + error);
        for (unsigned i = 0; i < numGlyphs && i < outlines.size(); ++i) raw[i].contours = std::move(outlines[i]);
    } else if (tables.count("CFF2")) {
        throw std::runtime_error("CFF2 (variable OpenType) outlines aren't supported");
    } else {
        warnings.push_back("the font has no outlines (no glyf or CFF table)");
    }
    if (tables.count("fpgm") || tables.count("prep")) warnings.push_back("TrueType hinting instructions were dropped");
    if (tables.count("fvar")) warnings.push_back("variable font: only the default instance was read");

    // names: post names, else from the cmap, else glyphN; unique
    std::vector<std::vector<std::uint32_t>> unicodes(numGlyphs);
    for (const auto& [cp, g] : cmap) {
        if (g < numGlyphs) unicodes[g].push_back(cp);
    }
    std::set<std::string> used;
    names.resize(numGlyphs);
    for (unsigned i = 0; i < numGlyphs; ++i) {
        std::string n = names[i];
        if (i == 0) n = ".notdef";
        if (n.empty() || !validGlyphName(n)) n = unicodes[i].empty() ? "glyph" + std::to_string(i) : glyphNameFor(unicodes[i][0]);
        std::string unique = n;
        for (int k = 1; used.count(unique); ++k) unique = n + "." + std::to_string(k);
        used.insert(unique);
        names[i] = unique;
    }
    for (unsigned i = 0; i < numGlyphs; ++i) {
        Glyph g;
        g.name = names[i];
        g.unicodes = unicodes[i];
        g.advance = advances[i];
        g.contours = std::move(raw[i].contours);
        for (auto& r : raw[i].refs) {
            if (r.glyph >= numGlyphs) continue;
            r.c.base = names[r.glyph];
            g.components.push_back(r.c);
        }
        font.glyphs.push_back(std::move(g));
    }

    std::map<std::pair<unsigned, unsigned>, int> pairs;
    if (auto gpos = table("GPOS")) {
        try {
            readGpos(*gpos, pairs, warnings);
        } catch (const std::exception&) {
            warnings.push_back("the GPOS table is corrupt; its kerning was skipped");
        }
    }
    if (pairs.empty()) {
        if (auto kern = table("kern")) {
            try {
                readKern(*kern, pairs, warnings);
            } catch (const std::exception&) {
                warnings.push_back("the kern table is corrupt and was skipped");
            }
        }
    }
    for (const auto& [p, v] : pairs) {
        if (p.first < numGlyphs && p.second < numGlyphs) font.kerning[{names[p.first], names[p.second]}] = v;
    }
    if (!haveXHeight) {
        const double asc = font.metrics.ascender, desc = font.metrics.descender;
        detectVerticalMetrics(font);
        font.metrics.ascender = asc;  // keep the font's own line metrics
        font.metrics.descender = desc;
    }
    if (warningsOut) warningsOut->insert(warningsOut->end(), warnings.begin(), warnings.end());
    return font;
}

}  // namespace achclwss::typeface
