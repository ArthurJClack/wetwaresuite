#include "wetwaresuite/pdf/view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "internal.hpp"
#include "lexer.hpp"

namespace wetwaresuite::pdf {

using detail::Lexer;
using raster::Color;
using raster::Image;

Matrix operator*(const Matrix& m, const Matrix& n) {
    return {m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d, m.c * n.a + m.d * n.c, m.c * n.b + m.d * n.d,
            m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f};
}

Matrix Matrix::inverse() const {
    const double det = a * d - b * c;
    if (std::abs(det) < 1e-12) return {};
    Matrix r{d / det, -b / det, -c / det, a / det, 0, 0};
    r.e = -(e * r.a + f * r.c);
    r.f = -(e * r.b + f * r.d);
    return r;
}

namespace {

// ---- small helpers -------------------------------------------------------------

double numOf(const PdfFile& f, const Object* o, double fallback = 0) {
    if (!o) return fallback;
    const Object& r = f.resolve(*o);
    return r.kind == Object::Number ? r.number : fallback;
}

const Object& dictOf(const PdfFile& f, const Object* o) {
    static const Object empty = [] {
        Object e;
        e.kind = Object::Dict;
        return e;
    }();
    if (!o) return empty;
    const Object& r = f.resolve(*o);
    return (r.kind == Object::Dict || r.kind == Object::Stream) ? r : empty;
}

bool boxOf(const PdfFile& f, const Object* o, double v[4]) {
    if (!o) return false;
    const Object& arr = f.resolve(*o);
    if (arr.kind != Object::Array || arr.items.size() != 4) return false;
    for (int i = 0; i < 4; ++i) v[i] = numOf(f, &arr.items[static_cast<std::size_t>(i)]);
    if (v[0] > v[2]) std::swap(v[0], v[2]);
    if (v[1] > v[3]) std::swap(v[1], v[3]);
    return v[2] - v[0] > 0.01 && v[3] - v[1] > 0.01;
}

Matrix matrixOf(const PdfFile& f, const Object* o) {
    Matrix m;
    if (!o) return m;
    const Object& arr = f.resolve(*o);
    if (arr.kind != Object::Array || arr.items.size() != 6) return m;
    double v[6];
    for (int i = 0; i < 6; ++i) v[i] = numOf(f, &arr.items[static_cast<std::size_t>(i)]);
    return {v[0], v[1], v[2], v[3], v[4], v[5]};
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// The visible box of a page: the crop box inside the media box.
void visibleBox(const PdfFile& f, const Object& page, double v[4]) {
    if (!boxOf(f, page.get("MediaBox"), v)) {
        v[0] = v[1] = 0;
        v[2] = 612;
        v[3] = 792;
    }
    double c[4];
    if (boxOf(f, page.get("CropBox"), c)) {
        double w[4] = {std::max(v[0], c[0]), std::max(v[1], c[1]), std::min(v[2], c[2]), std::min(v[3], c[3])};
        if (w[2] - w[0] > 1 && w[3] - w[1] > 1) std::copy(w, w + 4, v);
    }
}

// unrotated page points (W x H) -> page points turned clockwise by `rot`
Matrix turn(int rot, double W, double H) {
    switch (rot) {
        case 90: return {0, 1, -1, 0, H, 0};
        case 180: return {-1, 0, 0, -1, W, H};
        case 270: return {0, -1, 1, 0, 0, W};
        default: return {};
    }
}

// ---- glyph names ---------------------------------------------------------------

const std::map<std::string, unsigned>& glyphNames() {
    static const std::map<std::string, unsigned> table = [] {
        std::map<std::string, unsigned> m;
        static const char* ascii[] = {
            "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quotesingle", "parenleft", "parenright",
            "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two", "three", "four", "five", "six", "seven",
            "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at"};
        for (unsigned i = 0; i < sizeof ascii / sizeof *ascii; ++i) m[ascii[i]] = 32 + i;
        for (unsigned c = 'A'; c <= 'Z'; ++c) m[std::string(1, static_cast<char>(c))] = c;
        for (unsigned c = 'a'; c <= 'z'; ++c) m[std::string(1, static_cast<char>(c))] = c;
        static const char* mid[] = {"bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "grave"};
        for (unsigned i = 0; i < 6; ++i) m[mid[i]] = 91 + i;
        static const char* tail[] = {"braceleft", "bar", "braceright", "asciitilde"};
        for (unsigned i = 0; i < 4; ++i) m[tail[i]] = 123 + i;
        static const char* high[] = {
            "Euro", nullptr, "quotesinglbase", "florin", "quotedblbase", "ellipsis", "dagger", "daggerdbl", "circumflex", "perthousand",
            "Scaron", "guilsinglleft", "OE", nullptr, "Zcaron", nullptr, nullptr, "quoteleft", "quoteright", "quotedblleft",
            "quotedblright", "bullet", "endash", "emdash", "tilde", "trademark", "scaron", "guilsinglright", "oe", nullptr, "zcaron",
            "Ydieresis"};
        for (unsigned i = 0; i < 32; ++i) {
            if (high[i]) m[high[i]] = fromWinAnsi(static_cast<unsigned char>(128 + i));
        }
        static const char* latin1[] = {
            "nbspace", "exclamdown", "cent", "sterling", "currency", "yen", "brokenbar", "section", "dieresis", "copyright",
            "ordfeminine", "guillemotleft", "logicalnot", "sfthyphen", "registered", "macron", "degree", "plusminus", "twosuperior",
            "threesuperior", "acute", "mu", "paragraph", "periodcentered", "cedilla", "onesuperior", "ordmasculine", "guillemotright",
            "onequarter", "onehalf", "threequarters", "questiondown", "Agrave", "Aacute", "Acircumflex", "Atilde", "Adieresis", "Aring",
            "AE", "Ccedilla", "Egrave", "Eacute", "Ecircumflex", "Edieresis", "Igrave", "Iacute", "Icircumflex", "Idieresis", "Eth",
            "Ntilde", "Ograve", "Oacute", "Ocircumflex", "Otilde", "Odieresis", "multiply", "Oslash", "Ugrave", "Uacute",
            "Ucircumflex", "Udieresis", "Yacute", "Thorn", "germandbls", "agrave", "aacute", "acircumflex", "atilde", "adieresis",
            "aring", "ae", "ccedilla", "egrave", "eacute", "ecircumflex", "edieresis", "igrave", "iacute", "icircumflex",
            "idieresis", "eth", "ntilde", "ograve", "oacute", "ocircumflex", "otilde", "odieresis", "divide", "oslash", "ugrave",
            "uacute", "ucircumflex", "udieresis", "yacute", "thorn", "ydieresis"};
        for (unsigned i = 0; i < 96; ++i) m[latin1[i]] = 160 + i;
        const std::pair<const char*, unsigned> extra[] = {
            {"fi", 0xFB01}, {"fl", 0xFB02}, {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04}, {"minus", 0x2212}, {"middot", 0xB7},
            {"dotlessi", 0x131}, {"Lslash", 0x141}, {"lslash", 0x142}, {"fraction", 0x2044}, {"ring", 0x2DA}, {"caron", 0x2C7},
            {"breve", 0x2D8}, {"dotaccent", 0x2D9}, {"hungarumlaut", 0x2DD}, {"ogonek", 0x2DB}, {"quotedblbase", 0x201E},
            {"periodcentered", 0xB7}, {"space", 32}, {"hyphen", '-'}, {"uni00A0", 0xA0}};
        for (const auto& [n, u] : extra) m[n] = u;
        return m;
    }();
    return table;
}

// UTF-8 for a glyph name, empty when unknown
std::string glyphNameText(const std::string& name) {
    if (name.empty()) return {};
    auto it = glyphNames().find(name);
    if (it != glyphNames().end()) return utf8Encode(it->second);
    std::string base = name.substr(0, name.find('.'));
    if (base != name) {
        it = glyphNames().find(base);
        if (it != glyphNames().end()) return utf8Encode(it->second);
    }
    auto hexRun = [](const std::string& s, std::size_t from, std::size_t len) -> long {
        if (s.size() < from + len) return -1;
        long v = 0;
        for (std::size_t i = from; i < from + len; ++i) {
            int h = detail::hexVal(s[i]);
            if (h < 0) return -1;
            v = v * 16 + h;
        }
        return v;
    };
    if (base.rfind("uni", 0) == 0 && base.size() >= 7) {
        std::string out;
        for (std::size_t i = 3; i + 4 <= base.size(); i += 4) {
            long v = hexRun(base, i, 4);
            if (v < 0) return out;
            out += utf8Encode(static_cast<unsigned>(v));
        }
        return out;
    }
    if (base.size() >= 5 && base.size() <= 7 && base[0] == 'u') {
        long v = hexRun(base, 1, base.size() - 1);
        if (v > 0) return utf8Encode(static_cast<unsigned>(v));
    }
    return {};
}

// ---- fonts ---------------------------------------------------------------------

std::uint32_t codeOf(const std::string& b, std::size_t at, std::size_t n) {
    std::uint32_t v = 0;
    for (std::size_t i = at; i < at + n && i < b.size(); ++i) v = (v << 8) | static_cast<unsigned char>(b[i]);
    return v;
}

std::string utf16Text(const std::string& b) {
    std::string out;
    for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
        unsigned u = (static_cast<unsigned char>(b[i]) << 8) | static_cast<unsigned char>(b[i + 1]);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
            unsigned lo = (static_cast<unsigned char>(b[i + 2]) << 8) | static_cast<unsigned char>(b[i + 3]);
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        out += utf8Encode(u);
    }
    if (b.size() == 1) out += utf8Encode(static_cast<unsigned char>(b[0]));
    return out;
}

struct FontInfo {
    int codeBytes = 1;
    bool type3 = false;
    Matrix fontMatrix{0.001, 0, 0, 0.001, 0, 0};
    const Object* charProcs = nullptr;
    const Object* resources = nullptr;  // Type3 glyph resources
    std::map<std::uint32_t, std::string> toUnicode;
    std::string encoding[256];   // simple fonts: code -> UTF-8 from the encoding
    std::string glyphName[256];  // simple fonts: code -> glyph name (Type3 procedures)
    int firstChar = 0;
    std::vector<double> widths;
    double missingWidth = 0;
    std::map<std::uint32_t, double> cidWidths;
    double defaultWidth = 1000;
    Font face;
    int standard = -1;
    double ascent = 0.75, descent = -0.22;

    double width(std::uint32_t code) const {  // 1/1000 em (Type3: glyph space units)
        if (codeBytes == 2) {
            auto it = cidWidths.find(code);
            return it != cidWidths.end() ? it->second : defaultWidth;
        }
        if (code >= static_cast<std::uint32_t>(firstChar) && code - static_cast<std::uint32_t>(firstChar) < widths.size()) {
            return widths[code - static_cast<std::uint32_t>(firstChar)];
        }
        if (standard >= 0 && code < 256) return charWidth(face, static_cast<unsigned char>(code));
        return missingWidth > 0 ? missingWidth : 500;
    }
    std::string text(std::uint32_t code) const {
        auto it = toUnicode.find(code);
        if (it != toUnicode.end()) return it->second;
        if (codeBytes == 1 && code < 256) return encoding[code];
        return {};
    }
};

void parseToUnicode(const std::string& data, FontInfo& fi) {
    Lexer lx(data, 0);
    std::vector<Object> ops;
    while (!lx.eof()) {
        std::string op;
        std::size_t before = lx.pos;
        Object o = lx.parse(&op);
        if (lx.pos == before) ++lx.pos;
        if (op.empty()) {
            if (o.kind != Object::Null) ops.push_back(std::move(o));
            continue;
        }
        if (op == "endbfchar") {
            for (std::size_t i = 0; i + 1 < ops.size(); i += 2) {
                if (ops[i].kind == Object::String && ops[i + 1].kind == Object::String)
                    fi.toUnicode[codeOf(ops[i].str, 0, ops[i].str.size())] = utf16Text(ops[i + 1].str);
            }
        } else if (op == "endbfrange") {
            for (std::size_t i = 0; i + 2 < ops.size(); i += 3) {
                if (ops[i].kind != Object::String || ops[i + 1].kind != Object::String) continue;
                std::uint32_t lo = codeOf(ops[i].str, 0, ops[i].str.size()), hi = codeOf(ops[i + 1].str, 0, ops[i + 1].str.size());
                if (hi < lo || hi - lo > 65535) continue;
                const Object& dst = ops[i + 2];
                if (dst.kind == Object::String) {
                    std::string base = dst.str;
                    for (std::uint32_t c = lo; c <= hi; ++c) {
                        fi.toUnicode[c] = utf16Text(base);
                        std::size_t k = base.size();
                        while (k-- > 0) {
                            unsigned char v = static_cast<unsigned char>(base[k]);
                            base[k] = static_cast<char>(v + 1);
                            if (v != 0xFF) break;
                        }
                    }
                } else if (dst.kind == Object::Array) {
                    for (std::uint32_t c = lo; c <= hi && c - lo < dst.items.size(); ++c) {
                        if (dst.items[c - lo].kind == Object::String) fi.toUnicode[c] = utf16Text(dst.items[c - lo].str);
                    }
                }
            }
        }
        ops.clear();
    }
}

FontInfo loadFont(const PdfFile& f, const Object& font) {
    FontInfo fi;
    const Object* sub = font.get("Subtype");
    const std::string subtype = sub ? f.resolve(*sub).str : "";
    std::string baseName;
    if (const Object* bf = font.get("BaseFont")) baseName = f.resolve(*bf).str;
    auto plus = baseName.find('+');
    if (plus != std::string::npos && plus == 6) baseName = baseName.substr(plus + 1);
    const Object* descriptor = font.get("FontDescriptor");
    if (subtype == "Type0") {
        fi.codeBytes = 2;
        if (const Object* df = font.get("DescendantFonts")) {
            const Object& arr = f.resolve(*df);
            if (arr.kind == Object::Array && !arr.items.empty()) {
                const Object& cid = f.resolve(arr.items[0]);
                if (!descriptor) descriptor = cid.get("FontDescriptor");
                fi.defaultWidth = numOf(f, cid.get("DW"), 1000);
                if (const Object* w = cid.get("W")) {
                    const Object& wa = f.resolve(*w);
                    for (std::size_t i = 0; i + 1 < wa.items.size();) {
                        const auto first = static_cast<std::uint32_t>(numOf(f, &wa.items[i]));
                        const Object& next = f.resolve(wa.items[i + 1]);
                        if (next.kind == Object::Array) {
                            for (std::size_t k = 0; k < next.items.size(); ++k) fi.cidWidths[first + static_cast<std::uint32_t>(k)] = numOf(f, &next.items[k]);
                            i += 2;
                        } else if (i + 2 < wa.items.size()) {
                            const auto last = static_cast<std::uint32_t>(next.number);
                            const double w = numOf(f, &wa.items[i + 2]);
                            for (std::uint32_t c = first; c <= last && c - first < 65536; ++c) fi.cidWidths[c] = w;
                            i += 3;
                        } else {
                            break;
                        }
                    }
                }
            }
        }
    } else {
        fi.firstChar = static_cast<int>(numOf(f, font.get("FirstChar")));
        if (const Object* w = font.get("Widths")) {
            for (const auto& it : f.resolve(*w).items) fi.widths.push_back(numOf(f, &it));
        }
        if (subtype == "Type3") {
            fi.type3 = true;
            fi.fontMatrix = matrixOf(f, font.get("FontMatrix"));
            if (const Object* cp = font.get("CharProcs")) fi.charProcs = &f.resolve(*cp);
            if (const Object* r = font.get("Resources")) fi.resources = &f.resolve(*r);
        }
        // the encoding: WinAnsi as the base, then the Differences
        for (int c = 0; c < 256; ++c) {
            if (c >= 32) fi.encoding[c] = utf8Encode(fromWinAnsi(static_cast<unsigned char>(c)));
        }
        if (const Object* enc = font.get("Encoding")) {
            const Object& e = f.resolve(*enc);
            if (e.kind == Object::Dict) {
                if (const Object* diff = e.get("Differences")) {
                    int code = 0;
                    for (const auto& it : f.resolve(*diff).items) {
                        const Object& d = f.resolve(it);
                        if (d.kind == Object::Number) {
                            code = static_cast<int>(d.number);
                        } else if (d.kind == Object::Name) {
                            if (code >= 0 && code < 256) {
                                fi.glyphName[code] = d.str;
                                std::string t = glyphNameText(d.str);
                                if (!t.empty() || fi.type3) fi.encoding[code] = t;
                            }
                            ++code;
                        }
                    }
                }
            }
        }
        for (int i = 0; i < 12; ++i) {
            static const Family fam[3] = {Family::Sans, Family::Serif, Family::Mono};
            Font sf{fam[i / 4], (i % 4) == 1 || (i % 4) == 3, (i % 4) >= 2};
            if (baseName == baseFontName(sf)) {
                fi.standard = fontIndex(sf);
                fi.face = sf;
            }
        }
        if (fi.standard < 0 && (baseName == "Arial" || baseName == "ArialMT")) fi.standard = 0;
    }
    // the face: from the name, then the descriptor flags
    const std::string n = lowerAscii(baseName);
    const Object& desc = dictOf(f, descriptor);
    const int flags = static_cast<int>(numOf(f, desc.get("Flags")));
    if (fi.standard < 0) {
        auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
        if (has("courier") || has("mono") || has("consol") || has("menlo") || (flags & 1)) fi.face.family = Family::Mono;
        else if (!has("sans") && (has("times") || has("serif") || has("roman") || has("georgia") || has("garamond") ||
                                   has("cambria") || has("minion") || has("palatino") || has("bookman") || has("century") || (flags & 2)))
            fi.face.family = Family::Serif;
        fi.face.bold = has("bold") || has("black") || has("heavy") || has("semibold") || has("demi") || (flags & (1 << 18)) ||
                       numOf(f, desc.get("FontWeight")) >= 600;
        fi.face.italic = has("italic") || has("oblique") || (flags & (1 << 6)) || std::abs(numOf(f, desc.get("ItalicAngle"))) > 1;
    }
    fi.missingWidth = numOf(f, desc.get("MissingWidth"));
    const double asc = numOf(f, desc.get("Ascent")), dsc = numOf(f, desc.get("Descent"));
    if (asc >= 500 && asc <= 1200) fi.ascent = asc / 1000;
    if (dsc <= -50 && dsc >= -500) fi.descent = dsc / 1000;
    if (fi.type3) {
        fi.ascent = 0.8;
        fi.descent = -0.2;
    }
    if (const Object* tu = font.get("ToUnicode")) {
        const Object& cm = f.resolve(*tu);
        if (cm.kind == Object::Stream) {
            try {
                parseToUnicode(f.streamData(cm), fi);
            } catch (const std::exception&) {
            }
        }
    }
    return fi;
}

// ---- filling -------------------------------------------------------------------

struct Pt {
    double x = 0, y = 0;
};
using Path = std::vector<std::vector<Pt>>;  // device px

struct Clip {
    double x0 = -1e9, y0 = -1e9, x1 = 1e9, y1 = 1e9;
    void intersect(double ax, double ay, double bx, double by) {
        x0 = std::max(x0, ax);
        y0 = std::max(y0, ay);
        x1 = std::min(x1, bx);
        y1 = std::min(y1, by);
    }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

void blend(Image& img, int x, int y, Color c, double a) {
    if (a <= 0.002) return;
    if (a > 1) a = 1;
    std::uint8_t* p = img.pixel(x, y);
    const double da = p[3] / 255.0;
    const double oa = a + da * (1 - a);
    if (oa <= 0) return;
    auto mix = [&](std::uint8_t d, std::uint8_t s) { return static_cast<std::uint8_t>(std::lround((s * a + d * da * (1 - a)) / oa)); };
    p[0] = mix(p[0], c.r);
    p[1] = mix(p[1], c.g);
    p[2] = mix(p[2], c.b);
    p[3] = static_cast<std::uint8_t>(std::lround(oa * 255));
}

void fillPath(Image& img, const Path& path, bool evenOdd, Color c, double alpha, const Clip& clip) {
    struct Edge {
        double x0, y0, x1, y1;
        int dir;
    };
    std::vector<Edge> edges;
    double minY = 1e18, maxY = -1e18, minX = 1e18, maxX = -1e18;
    for (const auto& sp : path) {
        const std::size_t n = sp.size();
        if (n < 2) continue;
        for (std::size_t i = 0; i < n; ++i) {
            Pt a = sp[i], b = sp[(i + 1) % n];
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) return;
            minX = std::min({minX, a.x, b.x});
            maxX = std::max({maxX, a.x, b.x});
            if (a.y == b.y) continue;
            int dir = 1;
            if (a.y > b.y) {
                std::swap(a, b);
                dir = -1;
            }
            edges.push_back({a.x, a.y, b.x, b.y, dir});
            minY = std::min(minY, a.y);
            maxY = std::max(maxY, b.y);
        }
    }
    if (edges.empty()) return;
    const double cx0 = std::max({clip.x0, 0.0, minX}), cx1 = std::min({clip.x1, static_cast<double>(img.width()), maxX});
    const int y0 = std::max({0, static_cast<int>(std::floor(std::max(minY, clip.y0)))});
    const int y1 = std::min(img.height(), static_cast<int>(std::ceil(std::min(maxY, clip.y1))));
    if (cx1 <= cx0 || y1 <= y0) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& p, const Edge& q) { return p.y0 < q.y0; });
    const int S = 4;
    const int ix0 = static_cast<int>(std::floor(cx0)), ix1 = static_cast<int>(std::ceil(cx1));
    std::vector<float> acc(static_cast<std::size_t>(ix1 - ix0 + 2), 0.0f);
    std::vector<const Edge*> active;
    std::vector<std::pair<double, int>> xs;
    // edges that start above y0 but are still live
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (edges[i].y0 < y0 && edges[i].y1 > y0) active.push_back(&edges[i]);
    }
    std::size_t next = 0;
    while (next < edges.size() && edges[next].y0 < y0) ++next;
    for (int py = y0; py < y1; ++py) {
        int lo = ix1, hi = ix0 - 1;
        for (int s = 0; s < S; ++s) {
            const double sy = py + (s + 0.5) / S;
            if (sy < clip.y0 || sy > clip.y1) continue;
            while (next < edges.size() && edges[next].y0 <= sy) active.push_back(&edges[next++]);
            active.erase(std::remove_if(active.begin(), active.end(), [&](const Edge* e) { return e->y1 <= sy; }), active.end());
            xs.clear();
            for (const Edge* e : active) {
                if (e->y0 > sy) continue;
                xs.emplace_back(e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0), e->dir);
            }
            std::sort(xs.begin(), xs.end());
            int w = 0;
            for (std::size_t k = 0; k + 1 < xs.size(); ++k) {
                w += xs[k].second;
                const bool inside = evenOdd ? (w & 1) != 0 : w != 0;
                if (!inside) continue;
                double xa = std::max(xs[k].first, cx0), xb = std::min(xs[k + 1].first, cx1);
                if (xb <= xa) continue;
                const int pa = static_cast<int>(std::floor(xa)), pb = static_cast<int>(std::ceil(xb));
                for (int px = pa; px < pb; ++px) {
                    const double cov = std::min(xb, px + 1.0) - std::max(xa, static_cast<double>(px));
                    if (cov > 0) acc[static_cast<std::size_t>(px - ix0)] += static_cast<float>(cov / S);
                }
                lo = std::min(lo, pa);
                hi = std::max(hi, pb - 1);
            }
        }
        for (int px = lo; px <= hi; ++px) {
            float& a = acc[static_cast<std::size_t>(px - ix0)];
            if (a > 0 && px >= 0 && px < img.width()) blend(img, px, py, c, std::min(1.0f, a) * alpha);
            a = 0;
        }
    }
}

void addQuad(Path& out, Pt a, Pt b, Pt c, Pt d) {
    std::vector<Pt> q{a, b, c, d};
    double area = 0;
    for (std::size_t i = 0; i < 4; ++i) area += q[i].x * q[(i + 1) % 4].y - q[(i + 1) % 4].x * q[i].y;
    if (area < 0) std::reverse(q.begin(), q.end());
    out.push_back(std::move(q));
}

void addDisc(Path& out, Pt c, double r) {
    std::vector<Pt> d;
    const int n = r < 3 ? 8 : 16;
    for (int i = 0; i < n; ++i) {
        const double t = 6.283185307179586 * i / n;
        d.push_back({c.x + r * std::cos(t), c.y + r * std::sin(t)});
    }
    out.push_back(std::move(d));
}

void strokePath(Image& img, const Path& path, const std::vector<bool>& closed, double widthPx, Color c, double alpha, const Clip& clip) {
    const double hw = std::max(0.35, widthPx / 2);
    Path quads;
    for (std::size_t s = 0; s < path.size(); ++s) {
        const auto& sp = path[s];
        if (sp.empty()) continue;
        const std::size_t n = sp.size();
        const std::size_t segs = (s < closed.size() && closed[s] && n > 2) ? n : n - 1;
        if (n == 1 && hw > 0.6) addDisc(quads, sp[0], hw);
        for (std::size_t i = 0; i < segs; ++i) {
            const Pt a = sp[i], b = sp[(i + 1) % n];
            const double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
            if (len < 1e-9) continue;
            const double nx = -dy / len * hw, ny = dx / len * hw;
            addQuad(quads, {a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny});
            if (hw > 1.0 && (i + 1 < segs || segs == n)) addDisc(quads, b, hw);
        }
    }
    fillPath(img, quads, false, c, alpha, clip);
}

// ---- colours -------------------------------------------------------------------

Color rgbOf(double r, double g, double b) {
    auto v = [](double x) { return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0, 1.0) * 255)); };
    return {v(r), v(g), v(b), 255};
}

Color cmyk(double c, double m, double y, double k) { return rgbOf((1 - c) * (1 - k), (1 - m) * (1 - k), (1 - y) * (1 - k)); }

struct ColorSpace {
    enum Kind { Gray, RGB, CMYK, Lab, Indexed, Separation, DeviceN, Pattern } kind = Gray;
    int n = 1;
    std::shared_ptr<ColorSpace> base;  // Indexed
    std::string lookup;                // Indexed
    int hival = 0;
    bool none = false;  // Separation /None
};

ColorSpace parseSpace(const PdfFile& f, const Object& csIn, const Object& resources, int depth = 0) {
    ColorSpace cs;
    const Object* csp = &f.resolve(csIn);
    if (csp->kind == Object::Name) {
        const std::string& n = csp->str;
        if (n == "DeviceRGB" || n == "RGB" || n == "CalRGB") return {ColorSpace::RGB, 3};
        if (n == "DeviceCMYK" || n == "CMYK") return {ColorSpace::CMYK, 4};
        if (n == "DeviceGray" || n == "G" || n == "CalGray") return {ColorSpace::Gray, 1};
        if (n == "Pattern") return {ColorSpace::Pattern, 1};
        if (n == "Indexed" || n == "I") return cs;
        // a named space from the resources
        const Object& spaces = dictOf(f, resources.get("ColorSpace"));
        if (const Object* named = spaces.get(n); named && depth < 4) return parseSpace(f, *named, resources, depth + 1);
        return cs;
    }
    if (csp->kind != Object::Array || csp->items.empty()) return cs;
    const Object& arr = *csp;
    const std::string fam = f.resolve(arr.items[0]).str;
    if (fam == "ICCBased" && arr.items.size() > 1) {
        const int n = static_cast<int>(numOf(f, dictOf(f, &arr.items[1]).get("N"), 3));
        if (n == 1) return {ColorSpace::Gray, 1};
        if (n == 4) return {ColorSpace::CMYK, 4};
        return {ColorSpace::RGB, 3};
    }
    if (fam == "CalRGB") return {ColorSpace::RGB, 3};
    if (fam == "CalGray") return {ColorSpace::Gray, 1};
    if (fam == "Lab") return {ColorSpace::Lab, 3};
    if ((fam == "Indexed" || fam == "I") && arr.items.size() >= 4 && depth < 4) {
        cs.kind = ColorSpace::Indexed;
        cs.n = 1;
        cs.base = std::make_shared<ColorSpace>(parseSpace(f, arr.items[1], resources, depth + 1));
        cs.hival = static_cast<int>(numOf(f, &arr.items[2]));
        const Object& lk = f.resolve(arr.items[3]);
        cs.lookup = lk.kind == Object::Stream ? f.streamData(lk) : lk.str;
        return cs;
    }
    if (fam == "Separation") {
        cs.kind = ColorSpace::Separation;
        cs.n = 1;
        if (arr.items.size() > 1 && f.resolve(arr.items[1]).isName("None")) cs.none = true;
        return cs;
    }
    if (fam == "DeviceN" && arr.items.size() > 1) {
        cs.kind = ColorSpace::DeviceN;
        cs.n = std::max<int>(1, static_cast<int>(f.resolve(arr.items[1]).items.size()));
        return cs;
    }
    if (fam == "Pattern") return {ColorSpace::Pattern, 1};
    return cs;
}

Color toColor(const ColorSpace& cs, const double* v) {
    switch (cs.kind) {
        case ColorSpace::RGB: return rgbOf(v[0], v[1], v[2]);
        case ColorSpace::CMYK: return cmyk(v[0], v[1], v[2], v[3]);
        case ColorSpace::Lab: {
            const double L = std::clamp(v[0] / 100.0, 0.0, 1.0);
            return rgbOf(L + v[1] / 300, L - v[1] / 600 + v[2] / 600, L - v[2] / 300);
        }
        case ColorSpace::Indexed: {
            if (!cs.base) return {0, 0, 0, 255};
            const int i = std::clamp(static_cast<int>(std::lround(v[0])), 0, std::max(0, cs.hival));
            double comps[4] = {0, 0, 0, 0};
            const int n = std::min(4, cs.base->n);
            for (int k = 0; k < n; ++k) {
                const std::size_t at = static_cast<std::size_t>(i * cs.base->n + k);
                comps[k] = at < cs.lookup.size() ? static_cast<unsigned char>(cs.lookup[at]) / 255.0 : 0;
            }
            if (cs.base->kind == ColorSpace::Lab) {
                comps[0] *= 100;
                comps[1] = comps[1] * 255 - 128;
                comps[2] = comps[2] * 255 - 128;
            }
            return toColor(*cs.base, comps);
        }
        case ColorSpace::Separation: return rgbOf(1 - v[0], 1 - v[0], 1 - v[0]);
        case ColorSpace::DeviceN: {
            double t = 0;
            for (int k = 0; k < cs.n; ++k) t = std::max(t, v[k]);
            return rgbOf(1 - t, 1 - t, 1 - t);
        }
        default: return rgbOf(v[0], v[0], v[0]);
    }
}

// One flat colour for a shading: the middle of its function's range.
std::optional<Color> shadingColor(const PdfFile& f, const Object& shadingIn, const Object& resources) {
    const Object& sh = f.resolve(shadingIn);
    if (sh.kind != Object::Dict && sh.kind != Object::Stream) return std::nullopt;
    ColorSpace cs = sh.get("ColorSpace") ? parseSpace(f, *sh.get("ColorSpace"), resources) : ColorSpace{};
    const Object* fn = sh.get("Function");
    if (!fn) return std::nullopt;
    const Object* fp = &f.resolve(*fn);
    if (fp->kind == Object::Array && !fp->items.empty()) fp = &f.resolve(fp->items[0]);
    for (int hops = 0; hops < 4; ++hops) {
        const int type = static_cast<int>(numOf(f, fp->get("FunctionType"), -1));
        if (type == 2) {
            double v[4] = {0, 0, 0, 0}, c0[4] = {0, 0, 0, 0}, c1[4] = {1, 1, 1, 1};
            if (const Object* a = fp->get("C0")) {
                const Object& arr = f.resolve(*a);
                for (std::size_t k = 0; k < arr.items.size() && k < 4; ++k) c0[k] = numOf(f, &arr.items[k]);
            }
            if (const Object* a = fp->get("C1")) {
                const Object& arr = f.resolve(*a);
                for (std::size_t k = 0; k < arr.items.size() && k < 4; ++k) c1[k] = numOf(f, &arr.items[k]);
            }
            for (int k = 0; k < 4; ++k) v[k] = (c0[k] + c1[k]) / 2;
            return toColor(cs, v);
        }
        if (type == 3) {
            const Object* fs = fp->get("Functions");
            if (!fs) break;
            const Object& arr = f.resolve(*fs);
            if (arr.items.empty()) break;
            fp = &f.resolve(arr.items[arr.items.size() / 2]);
            continue;
        }
        break;
    }
    double mid[4] = {0.5, 0.5, 0.5, 0.5};
    if (cs.kind == ColorSpace::CMYK) mid[3] = 0.2;
    return toColor(cs, mid);
}

// ---- the content interpreter ---------------------------------------------------

struct GState {
    Matrix ctm;
    Color fill{0, 0, 0, 255}, stroke{0, 0, 0, 255};
    ColorSpace fillSpace, strokeSpace;
    bool fillNone = false, strokeNone = false;
    double lineWidth = 1;
    double fillAlpha = 1, strokeAlpha = 1;
    Clip clip;
    const FontInfo* font = nullptr;
    double fontSize = 0, charSpace = 0, wordSpace = 0, hscale = 1, leading = 0, rise = 0;
    int render = 0;
};

// What a text operator in the page's own content looked like, to take glyphs back out.
struct TextOpRecord {
    std::size_t start = 0, end = 0;  // bytes in the joined content
    std::string op;
    double aw = 0, ac = 0;  // the " operator's spacing
    std::vector<std::string> codes;
    std::vector<double> advance;  // per glyph, TJ units (thousandths of the font size)
    std::vector<double> kernBefore;
    double kernAfter = 0;
};

class Interpreter {
public:
    Interpreter(const PdfFile& f, Image* img, std::vector<TextChar>* chars) : f_(f), img_(img), chars_(chars) {}

    std::vector<TextOpRecord> records;  // text operators of the top-level content
    bool keepRecords = false;

    void runPage(const std::string& content, const Object& resources, const GState& start) {
        gs_ = start;
        run(content, resources, 0);
    }

    void runForm(const Object& form, const Object& parentRes, const Matrix& extra, int depth) {
        if (depth > 12) return;
        const std::string data = f_.streamData(form);
        const Object* fr = form.get("Resources");
        const Object& res = fr ? dictOf(f_, fr) : parentRes;
        std::vector<GState> saved{gs_};
        gs_.ctm = matrixOf(f_, form.get("Matrix")) * extra * gs_.ctm;
        double bb[4];
        if (boxOf(f_, form.get("BBox"), bb)) clipToBox(bb);
        try {
            run(data, res, depth + 1);
        } catch (const std::exception&) {
        }
        gs_ = saved.back();
    }

    GState& state() { return gs_; }

private:
    const PdfFile& f_;
    Image* img_;
    std::vector<TextChar>* chars_;
    GState gs_;
    std::map<const Object*, std::unique_ptr<FontInfo>> fonts_;
    Path path_;
    std::vector<bool> closed_;
    Pt current_;
    int textOps_ = 0;

    void clipToBox(const double bb[4]) {
        double xs[4], ys[4];
        gs_.ctm.apply(bb[0], bb[1], xs[0], ys[0]);
        gs_.ctm.apply(bb[2], bb[1], xs[1], ys[1]);
        gs_.ctm.apply(bb[2], bb[3], xs[2], ys[2]);
        gs_.ctm.apply(bb[0], bb[3], xs[3], ys[3]);
        gs_.clip.intersect(*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                           *std::max_element(ys, ys + 4));
    }

    const FontInfo* fontFor(const Object& resources, const std::string& name) {
        const Object& fonts = dictOf(f_, resources.get("Font"));
        const Object* fr = fonts.get(name);
        if (!fr) return nullptr;
        const Object& font = f_.resolve(*fr);
        auto it = fonts_.find(&font);
        if (it != fonts_.end()) return it->second.get();
        auto fi = std::make_unique<FontInfo>(loadFont(f_, font));
        const FontInfo* out = fi.get();
        fonts_[&font] = std::move(fi);
        return out;
    }

    void addPoint(double x, double y) {
        Pt p;
        gs_.ctm.apply(x, y, p.x, p.y);
        if (path_.empty()) {
            path_.emplace_back();
            closed_.push_back(false);
        }
        path_.back().push_back(p);
    }

    void curve(double x1, double y1, double x2, double y2, double x3, double y3) {
        Pt p0 = path_.empty() || path_.back().empty() ? Pt{} : path_.back().back();
        Pt p1, p2, p3;
        gs_.ctm.apply(x1, y1, p1.x, p1.y);
        gs_.ctm.apply(x2, y2, p2.x, p2.y);
        gs_.ctm.apply(x3, y3, p3.x, p3.y);
        const double len = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) + std::hypot(p3.x - p2.x, p3.y - p2.y);
        const int n = std::clamp(static_cast<int>(len / 3), 2, 64);
        if (path_.empty()) {
            path_.emplace_back();
            closed_.push_back(false);
        }
        for (int i = 1; i <= n; ++i) {
            const double t = static_cast<double>(i) / n, u = 1 - t;
            path_.back().push_back({u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
                                    u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y});
        }
    }

    double deviceScale() const { return std::sqrt(std::abs(gs_.ctm.a * gs_.ctm.d - gs_.ctm.b * gs_.ctm.c)); }

    void paint(bool fill, bool stroke, bool evenOdd) {
        if (img_ && !gs_.clip.empty()) {
            if (fill && !gs_.fillNone && gs_.fillAlpha > 0) fillPath(*img_, path_, evenOdd, gs_.fill, gs_.fillAlpha * gs_.fill.a / 255.0, gs_.clip);
            if (stroke && !gs_.strokeNone && gs_.strokeAlpha > 0)
                strokePath(*img_, path_, closed_, gs_.lineWidth * deviceScale(), gs_.stroke, gs_.strokeAlpha * gs_.stroke.a / 255.0, gs_.clip);
        }
    }

    void setClipFromPath() {
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const auto& sp : path_) {
            for (const auto& p : sp) {
                x0 = std::min(x0, p.x);
                y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x);
                y1 = std::max(y1, p.y);
            }
        }
        if (x1 >= x0) gs_.clip.intersect(x0, y0, x1, y1);
    }

    void setColor(bool fill, const std::vector<Object>& ops, const Object& resources) {
        ColorSpace& cs = fill ? gs_.fillSpace : gs_.strokeSpace;
        Color& out = fill ? gs_.fill : gs_.stroke;
        bool& none = fill ? gs_.fillNone : gs_.strokeNone;
        if (cs.kind == ColorSpace::Pattern) {
            none = true;
            if (!ops.empty() && ops.back().kind == Object::Name) {
                const Object& pats = dictOf(f_, resources.get("Pattern"));
                if (const Object* p = pats.get(ops.back().str)) {
                    const Object& pat = f_.resolve(*p);
                    if (numOf(f_, pat.get("PatternType")) == 2 && pat.get("Shading")) {
                        if (auto c = shadingColor(f_, *pat.get("Shading"), resources)) {
                            out = *c;
                            none = false;
                        }
                    }
                }
            }
            return;
        }
        double v[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int k = 0;
        for (const auto& o : ops) {
            if (o.kind == Object::Number && k < 8) v[k++] = o.number;
        }
        out = toColor(cs, v);
        none = cs.none;
    }

    void showText(const std::string& bytes, TextOpRecord* rec, int opIndex, int& glyphIndex) {
        const FontInfo* font = gs_.font;
        if (!font) return;
        const std::size_t n = static_cast<std::size_t>(font->codeBytes);
        const double tfs = gs_.fontSize, th = gs_.hscale;
        for (std::size_t i = 0; i + n <= bytes.size(); i += n) {
            const std::uint32_t code = codeOf(bytes, i, n);
            const double w0 = font->type3 ? font->width(code) * font->fontMatrix.a : font->width(code) / 1000.0;
            const bool space = n == 1 && code == 32;
            const double tx = (w0 * tfs + gs_.charSpace + (space ? gs_.wordSpace : 0)) * th;
            const Matrix m = tm_ * gs_.ctm;
            if (rec) {
                rec->codes.push_back(bytes.substr(i, n));
                rec->advance.push_back(tfs != 0 ? w0 * 1000 + (gs_.charSpace + (space ? gs_.wordSpace : 0)) * 1000 / tfs : 0);
                rec->kernBefore.push_back(pendingKern_);
                pendingKern_ = 0;
            }
            // the glyph box in device space
            const double gw = w0 * tfs * th, ya = gs_.rise + font->descent * tfs, yb = gs_.rise + font->ascent * tfs;
            double xs[4], ys[4];
            m.apply(0, ya, xs[0], ys[0]);
            m.apply(gw, ya, xs[1], ys[1]);
            m.apply(gw, yb, xs[2], ys[2]);
            m.apply(0, yb, xs[3], ys[3]);
            const std::string text = font->text(code);
            const double sizeDev = std::abs(tfs) * std::hypot(m.c, m.d);
            if (chars_ && !text.empty()) {
                TextChar tc;
                tc.text = text;
                tc.x0 = *std::min_element(xs, xs + 4);
                tc.x1 = *std::max_element(xs, xs + 4);
                tc.y0 = *std::min_element(ys, ys + 4);
                tc.y1 = *std::max_element(ys, ys + 4);
                m.apply(0, gs_.rise, tc.ox, tc.oy);
                tc.size = sizeDev;
                tc.font = font->face;
                tc.color = gs_.fill;
                tc.op = opIndex;
                tc.glyph = glyphIndex;
                chars_->push_back(std::move(tc));
            }
            ++glyphIndex;
            const bool visible = (gs_.render % 4) != 3 && !(gs_.render % 4 == 1 && gs_.strokeNone) && !gs_.fillNone;
            if (img_ && visible && !gs_.clip.empty()) {
                if (font->type3) {
                    drawType3(font, code, m);
                } else if (!text.empty() && sizeDev > 0.6 && text != " ") {
                    double ox, oy;
                    m.apply(0, gs_.rise, ox, oy);
                    if (ox >= gs_.clip.x0 - sizeDev && ox <= gs_.clip.x1 && oy >= gs_.clip.y0 && oy <= gs_.clip.y1 + sizeDev) {
                        const Color col = gs_.render % 4 == 1 ? gs_.stroke : gs_.fill;
                        Color c = col;
                        c.a = static_cast<std::uint8_t>(std::lround(c.a * std::clamp(gs_.fillAlpha, 0.0, 1.0)));
                        // squeeze wide built-in glyphs into the PDF's advance so lines keep their length
                        drawText(*img_, ox, oy, text, font->face, sizeDev, c);
                    }
                }
            }
            tm_ = Matrix{1, 0, 0, 1, tx, 0} * tm_;
        }
    }

    void drawType3(const FontInfo* font, std::uint32_t code, const Matrix&) {
        if (!font->charProcs || code > 255 || type3Depth_ > 2) return;
        const Object* proc = font->charProcs->get(font->glyphName[code]);
        if (!proc) return;
        const Object& stream = f_.resolve(*proc);
        if (stream.kind != Object::Stream) return;
        GState saved = gs_;
        const Matrix tm0 = tm_;
        gs_.ctm = font->fontMatrix * Matrix{gs_.fontSize * gs_.hscale, 0, 0, gs_.fontSize, 0, gs_.rise} * tm_ * saved.ctm;
        ++type3Depth_;
        static const Object empty;
        try {
            run(f_.streamData(stream), font->resources ? *font->resources : empty, 10);
        } catch (const std::exception&) {
        }
        --type3Depth_;
        gs_ = saved;
        tm_ = tm0;
    }

    Matrix tm_, lm_;
    double pendingKern_ = 0;
    int type3Depth_ = 0;

    // ---- images ----

    void drawImage(const Object& dict, const std::string& raw, const Object& resources, bool inlineImage);

    // ---- the operator loop ----

    void run(const std::string& content, const Object& resources, int depth) {
        if (depth > 16) return;
        Lexer lx(content, 0);
        std::vector<Object> ops;
        std::vector<GState> stack;
        std::size_t opStart = std::string::npos;
        auto num = [&](std::size_t i) { return i < ops.size() && ops[i].kind == Object::Number ? ops[i].number : 0.0; };
        auto lineMove = [&](double tx, double ty) {
            lm_ = Matrix{1, 0, 0, 1, tx, ty} * lm_;
            tm_ = lm_;
        };
        while (true) {
            lx.skipWs();
            if (lx.eof()) break;
            const std::size_t tokenStart = lx.pos;
            std::string op;
            Object o;
            try {
                o = lx.parse(&op);
            } catch (const std::exception&) {
                break;
            }
            if (lx.pos == tokenStart) ++lx.pos;
            if (opStart == std::string::npos) opStart = tokenStart;
            if (op.empty()) {
                ops.push_back(std::move(o));
                continue;
            }
            const std::size_t opEnd = lx.pos;
            if (op == "q") {
                stack.push_back(gs_);
            } else if (op == "Q") {
                if (!stack.empty()) {
                    gs_ = stack.back();
                    stack.pop_back();
                }
            } else if (op == "cm") {
                gs_.ctm = Matrix{num(0), num(1), num(2), num(3), num(4), num(5)} * gs_.ctm;
            } else if (op == "w") {
                gs_.lineWidth = num(0);
            } else if (op == "gs") {
                if (!ops.empty() && ops[0].kind == Object::Name) {
                    const Object& egs = dictOf(f_, dictOf(f_, resources.get("ExtGState")).get(ops[0].str));
                    if (const Object* ca = egs.get("ca")) gs_.fillAlpha = numOf(f_, ca, 1);
                    if (const Object* ca = egs.get("CA")) gs_.strokeAlpha = numOf(f_, ca, 1);
                    if (const Object* lw = egs.get("LW")) gs_.lineWidth = numOf(f_, lw, 1);
                }
            } else if (op == "m") {
                path_.emplace_back();
                closed_.push_back(false);
                addPoint(num(0), num(1));
            } else if (op == "l") {
                addPoint(num(0), num(1));
            } else if (op == "c") {
                curve(num(0), num(1), num(2), num(3), num(4), num(5));
            } else if (op == "v") {
                if (!path_.empty() && !path_.back().empty()) {
                    const Matrix inv = gs_.ctm.inverse();
                    double x0, y0;
                    inv.apply(path_.back().back().x, path_.back().back().y, x0, y0);
                    curve(x0, y0, num(0), num(1), num(2), num(3));
                }
            } else if (op == "y") {
                curve(num(0), num(1), num(2), num(3), num(2), num(3));
            } else if (op == "h") {
                if (!closed_.empty()) closed_.back() = true;
            } else if (op == "re") {
                const double x = num(0), y = num(1), w = num(2), h = num(3);
                path_.emplace_back();
                closed_.push_back(true);
                addPoint(x, y);
                addPoint(x + w, y);
                addPoint(x + w, y + h);
                addPoint(x, y + h);
            } else if (op == "f" || op == "F" || op == "f*" || op == "S" || op == "s" || op == "B" || op == "B*" || op == "b" || op == "b*" ||
                       op == "n") {
                if (op == "s" || op == "b" || op == "b*") {
                    for (std::size_t k = 0; k < closed_.size(); ++k) closed_[k] = true;
                }
                const bool fill = op[0] == 'f' || op[0] == 'F' || op[0] == 'B' || op[0] == 'b';
                const bool stroke = op[0] == 'S' || op[0] == 's' || op[0] == 'B' || op[0] == 'b';
                if (op != "n") paint(fill, stroke, op.back() == '*');
                if (pendingClip_) setClipFromPath();
                pendingClip_ = false;
                path_.clear();
                closed_.clear();
            } else if (op == "W" || op == "W*") {
                pendingClip_ = true;
            } else if (op == "g" || op == "G") {
                const bool fill = op == "g";
                (fill ? gs_.fillSpace : gs_.strokeSpace) = {ColorSpace::Gray, 1};
                setColor(fill, ops, resources);
            } else if (op == "rg" || op == "RG") {
                const bool fill = op == "rg";
                (fill ? gs_.fillSpace : gs_.strokeSpace) = {ColorSpace::RGB, 3};
                setColor(fill, ops, resources);
            } else if (op == "k" || op == "K") {
                const bool fill = op == "k";
                (fill ? gs_.fillSpace : gs_.strokeSpace) = {ColorSpace::CMYK, 4};
                setColor(fill, ops, resources);
            } else if (op == "cs" || op == "CS") {
                const bool fill = op == "cs";
                if (!ops.empty()) {
                    ColorSpace cs = parseSpace(f_, ops[0], resources);
                    (fill ? gs_.fillSpace : gs_.strokeSpace) = cs;
                    std::vector<Object> init;
                    double start = cs.kind == ColorSpace::Indexed ? 0 : (cs.kind == ColorSpace::Separation || cs.kind == ColorSpace::DeviceN) ? 1 : 0;
                    for (int k = 0; k < cs.n; ++k) init.push_back(Object::makeNumber(cs.kind == ColorSpace::CMYK && k < 3 ? 0 : start));
                    if (cs.kind == ColorSpace::CMYK) init[3] = Object::makeNumber(1);
                    setColor(fill, init, resources);
                }
            } else if (op == "sc" || op == "scn" || op == "SC" || op == "SCN") {
                setColor(op[0] == 's', ops, resources);
            } else if (op == "sh") {
                if (img_ && !ops.empty() && ops[0].kind == Object::Name && gs_.clip.x1 < 1e8) {
                    const Object& shadings = dictOf(f_, resources.get("Shading"));
                    if (const Object* s = shadings.get(ops[0].str)) {
                        if (auto c = shadingColor(f_, *s, resources)) {
                            Path box{{{gs_.clip.x0, gs_.clip.y0}, {gs_.clip.x1, gs_.clip.y0}, {gs_.clip.x1, gs_.clip.y1}, {gs_.clip.x0, gs_.clip.y1}}};
                            fillPath(*img_, box, false, *c, gs_.fillAlpha, gs_.clip);
                        }
                    }
                }
            } else if (op == "BT") {
                tm_ = lm_ = Matrix{};
            } else if (op == "Tf") {
                if (ops.size() >= 2 && ops[0].kind == Object::Name) gs_.font = fontFor(resources, ops[0].str);
                gs_.fontSize = num(1);
            } else if (op == "Tc") {
                gs_.charSpace = num(0);
            } else if (op == "Tw") {
                gs_.wordSpace = num(0);
            } else if (op == "Tz") {
                gs_.hscale = num(0) / 100;
            } else if (op == "TL") {
                gs_.leading = num(0);
            } else if (op == "Ts") {
                gs_.rise = num(0);
            } else if (op == "Tr") {
                gs_.render = static_cast<int>(num(0));
            } else if (op == "Td") {
                lineMove(num(0), num(1));
            } else if (op == "TD") {
                gs_.leading = -num(1);
                lineMove(num(0), num(1));
            } else if (op == "Tm") {
                lm_ = tm_ = Matrix{num(0), num(1), num(2), num(3), num(4), num(5)};
            } else if (op == "T*") {
                lineMove(0, -gs_.leading);
            } else if (op == "Tj" || op == "'" || op == "\"" || op == "TJ") {
                TextOpRecord rec;
                TextOpRecord* r = (keepRecords && depth == 0) ? &rec : nullptr;
                const int opIndex = depth == 0 ? textOps_++ : -1;
                int glyph = 0;
                pendingKern_ = 0;
                if (op == "\"") {
                    gs_.wordSpace = num(0);
                    gs_.charSpace = num(1);
                    rec.aw = num(0);
                    rec.ac = num(1);
                }
                if (op != "Tj" && op != "TJ") lineMove(0, -gs_.leading);
                if (op == "TJ") {
                    if (!ops.empty() && ops[0].kind == Object::Array) {
                        for (const auto& it : ops[0].items) {
                            if (it.kind == Object::String) {
                                showText(it.str, r, opIndex, glyph);
                            } else if (it.kind == Object::Number) {
                                tm_ = Matrix{1, 0, 0, 1, -it.number / 1000 * gs_.fontSize * gs_.hscale, 0} * tm_;
                                pendingKern_ += it.number;
                            }
                        }
                    }
                } else if (!ops.empty() && ops.back().kind == Object::String) {
                    showText(ops.back().str, r, opIndex, glyph);
                }
                if (r) {
                    rec.kernAfter = pendingKern_;
                    rec.start = opStart;
                    rec.end = opEnd;
                    rec.op = op;
                    records.push_back(std::move(rec));
                }
                pendingKern_ = 0;
            } else if (op == "Do") {
                if (!ops.empty() && ops[0].kind == Object::Name) {
                    const Object& xobjs = dictOf(f_, resources.get("XObject"));
                    if (const Object* x = xobjs.get(ops[0].str)) {
                        const Object& xo = f_.resolve(*x);
                        const Object* sub = xo.get("Subtype");
                        if (xo.kind == Object::Stream && sub && sub->isName("Form")) {
                            runForm(xo, resources, Matrix{}, depth);
                        } else if (xo.kind == Object::Stream && sub && sub->isName("Image") && img_) {
                            drawImage(xo, xo.str, resources, false);
                        }
                    }
                }
            } else if (op == "BI") {
                // inline image: key/value pairs up to ID, then the data up to EI
                Object dict;
                dict.kind = Object::Dict;
                while (true) {
                    lx.skipWs();
                    if (lx.eof() || lx.peekKeyword("ID")) break;
                    std::string kop;
                    Object key = lx.parse(&kop);
                    if (key.kind != Object::Name) break;
                    dict.set(key.str, lx.parse());
                }
                std::size_t d = lx.pos + 2;
                if (d < content.size() && detail::isWs(content[d])) ++d;
                std::size_t ei = content.find("EI", d);
                while (ei != std::string::npos &&
                       ((ei > 0 && !detail::isWs(content[ei - 1])) || (ei + 2 < content.size() && detail::isRegular(content[ei + 2]))))
                    ei = content.find("EI", ei + 2);
                if (ei == std::string::npos) ei = content.size();
                std::size_t end = ei;
                if (end > d && detail::isWs(content[end - 1])) --end;
                if (img_) {
                    static const std::pair<const char*, const char*> abbrev[] = {
                        {"W", "Width"}, {"H", "Height"}, {"BPC", "BitsPerComponent"}, {"CS", "ColorSpace"}, {"F", "Filter"},
                        {"DP", "DecodeParms"}, {"IM", "ImageMask"}, {"D", "Decode"}, {"I", "Interpolate"}};
                    Object full;
                    full.kind = Object::Stream;
                    for (std::size_t k = 0; k < dict.keys.size(); ++k) {
                        std::string key = dict.keys[k];
                        for (const auto& [s, l] : abbrev) {
                            if (key == s) key = l;
                        }
                        Object v = dict.items[k];
                        if (key == "Filter" || key == "ColorSpace") {
                            auto expand = [](Object& n) {
                                static const std::pair<const char*, const char*> names[] = {
                                    {"AHx", "ASCIIHexDecode"}, {"A85", "ASCII85Decode"}, {"LZW", "LZWDecode"}, {"Fl", "FlateDecode"},
                                    {"RL", "RunLengthDecode"}, {"CCF", "CCITTFaxDecode"}, {"DCT", "DCTDecode"}, {"G", "DeviceGray"},
                                    {"RGB", "DeviceRGB"}, {"CMYK", "DeviceCMYK"}, {"I", "Indexed"}};
                                for (const auto& [s, l] : names) {
                                    if (n.kind == Object::Name && n.str == s) n.str = l;
                                }
                            };
                            if (v.kind == Object::Array) {
                                for (auto& it : v.items) expand(it);
                            } else {
                                expand(v);
                            }
                        }
                        full.set(key, v);
                    }
                    full.str = content.substr(d, end - d);
                    drawImage(full, full.str, resources, true);
                }
                lx.pos = std::min(content.size(), ei + 2);
            }
            ops.clear();
            opStart = std::string::npos;
        }
    }

    bool pendingClip_ = false;
};

// ---- images --------------------------------------------------------------------

// Decodes an image XObject to RGBA. False when the codec isn't supported.
bool decodeImage(const PdfFile& f, const Object& dict, const Object& resources, Color fillColor, Image& out, bool& stencil, int depth = 0) {
    stencil = false;
    const int W = static_cast<int>(numOf(f, dict.get("Width"))), H = static_cast<int>(numOf(f, dict.get("Height")));
    if (W <= 0 || H <= 0 || static_cast<double>(W) * H > 6e7) return false;
    std::vector<std::string> filters;
    if (const Object* fl = dict.get("Filter")) {
        const Object& fr = f.resolve(*fl);
        if (fr.kind == Object::Name) filters.push_back(fr.str);
        for (const auto& it : fr.items) filters.push_back(f.resolve(it).str);
    }
    const std::string last = filters.empty() ? "" : filters.back();
    if (last == "JPXDecode" || last == "JBIG2Decode" || last == "CCITTFaxDecode") return false;
    if (last == "DCTDecode" || last == "DCT") {
        std::string bytes = dict.str;
        if (filters.size() > 1) {
            Object pre = dict;
            Object arr;
            arr.kind = Object::Array;
            for (std::size_t k = 0; k + 1 < filters.size(); ++k) arr.items.push_back(Object::makeName(filters[k]));
            pre.set("Filter", arr);
            pre.erase("DecodeParms");
            bytes = f.streamData(pre);
        }
        try {
            out = raster::decode(bytes);
        } catch (const std::exception&) {
            return false;
        }
    } else {
        const std::string data = f.streamData(dict);
        const bool mask = dict.get("ImageMask") && f.resolve(*dict.get("ImageMask")).boolean;
        const int bpc = mask ? 1 : static_cast<int>(numOf(f, dict.get("BitsPerComponent"), 8));
        ColorSpace cs;
        if (!mask && dict.get("ColorSpace")) cs = parseSpace(f, *dict.get("ColorSpace"), resources);
        const int n = mask ? 1 : cs.n;
        if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) return false;
        const std::size_t stride = (static_cast<std::size_t>(W) * n * bpc + 7) / 8;
        double decode[8];
        const double maxv = (1 << std::min(bpc, 8)) - 1;
        for (int k = 0; k < n; ++k) {
            decode[2 * k] = 0;
            decode[2 * k + 1] = cs.kind == ColorSpace::Indexed ? maxv : 1;
        }
        if (const Object* dec = dict.get("Decode")) {
            const Object& arr = f.resolve(*dec);
            for (std::size_t k = 0; k < arr.items.size() && k < static_cast<std::size_t>(2 * n) && k < 8; ++k) decode[k] = numOf(f, &arr.items[k]);
        }
        out = Image(W, H, {0, 0, 0, 0});
        auto sample = [&](std::size_t rowStart, std::size_t idx) -> unsigned {
            if (bpc == 8) {
                std::size_t at = rowStart + idx;
                return at < data.size() ? static_cast<unsigned char>(data[at]) : 0;
            }
            if (bpc == 16) {
                std::size_t at = rowStart + idx * 2;
                return at < data.size() ? static_cast<unsigned char>(data[at]) : 0;
            }
            const std::size_t bit = idx * static_cast<std::size_t>(bpc);
            const std::size_t at = rowStart + bit / 8;
            if (at >= data.size()) return 0;
            const unsigned byte = static_cast<unsigned char>(data[at]);
            return (byte >> (8 - bpc - static_cast<int>(bit % 8))) & ((1u << bpc) - 1);
        };
        const double sampleMax = bpc == 16 ? 255 : maxv;
        for (int y = 0; y < H; ++y) {
            const std::size_t rs = static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < W; ++x) {
                std::uint8_t* p = out.pixel(x, y);
                if (mask) {
                    const unsigned v = sample(rs, static_cast<std::size_t>(x));
                    const bool paint = decode[0] < 0.5 ? v == 0 : v == 1;
                    if (paint) {
                        p[0] = fillColor.r;
                        p[1] = fillColor.g;
                        p[2] = fillColor.b;
                        p[3] = 255;
                    }
                    continue;
                }
                double v[8];
                for (int k = 0; k < n && k < 8; ++k) {
                    const unsigned s = sample(rs, static_cast<std::size_t>(x) * static_cast<std::size_t>(n) + static_cast<std::size_t>(k));
                    v[k] = decode[2 * k] + s * (decode[2 * k + 1] - decode[2 * k]) / sampleMax;
                }
                Color c = toColor(cs, v);
                p[0] = c.r;
                p[1] = c.g;
                p[2] = c.b;
                p[3] = 255;
            }
        }
        stencil = mask;
    }
    // soft mask / stencil mask -> alpha
    const Object* sm = dict.get("SMask");
    const Object* mk = dict.get("Mask");
    const Object* alphaSrc = sm ? sm : (mk && f.resolve(*mk).kind == Object::Stream ? mk : nullptr);
    if (alphaSrc && depth == 0) {
        const Object& ms = f.resolve(*alphaSrc);
        Image a;
        bool st = false;
        if (ms.kind == Object::Stream && decodeImage(f, ms, resources, {0, 0, 0, 255}, a, st, 1)) {
            if (a.width() != out.width() || a.height() != out.height()) a = raster::resize(a, out.width(), out.height());
            for (int y = 0; y < out.height(); ++y) {
                for (int x = 0; x < out.width(); ++x) {
                    const std::uint8_t* q = a.pixel(x, y);
                    // a stencil Mask paints where its samples are 1-ish: those are the hidden parts
                    const int v = st ? (q[3] ? 0 : 255) : q[0];
                    out.pixel(x, y)[3] = static_cast<std::uint8_t>(out.pixel(x, y)[3] * v / 255);
                }
            }
        }
    }
    return true;
}

void Interpreter::drawImage(const Object& dict, const std::string&, const Object& resources, bool) {
    if (!img_ || gs_.clip.empty()) return;
    const Matrix& m = gs_.ctm;
    double xs[4], ys[4];
    m.apply(0, 0, xs[0], ys[0]);
    m.apply(1, 0, xs[1], ys[1]);
    m.apply(1, 1, xs[2], ys[2]);
    m.apply(0, 1, xs[3], ys[3]);
    const double bx0 = std::max({*std::min_element(xs, xs + 4), gs_.clip.x0, 0.0});
    const double by0 = std::max({*std::min_element(ys, ys + 4), gs_.clip.y0, 0.0});
    const double bx1 = std::min({*std::max_element(xs, xs + 4), gs_.clip.x1, static_cast<double>(img_->width())});
    const double by1 = std::min({*std::max_element(ys, ys + 4), gs_.clip.y1, static_cast<double>(img_->height())});
    if (bx1 <= bx0 || by1 <= by0) return;
    Image pic;
    bool stencil = false;
    if (!decodeImage(f_, dict, resources, gs_.fill, pic, stencil)) {
        Path box{{{xs[0], ys[0]}, {xs[1], ys[1]}, {xs[2], ys[2]}, {xs[3], ys[3]}}};
        fillPath(*img_, box, false, {200, 200, 200, 255}, 1, gs_.clip);
        return;
    }
    // shrink big pictures to about their size on the page first
    const double devW = std::hypot(xs[1] - xs[0], ys[1] - ys[0]), devH = std::hypot(xs[3] - xs[0], ys[3] - ys[0]);
    if (pic.width() > devW * 2 + 2 || pic.height() > devH * 2 + 2) {
        const int nw = std::max(1, std::min(pic.width(), static_cast<int>(std::ceil(devW * 1.5))));
        const int nh = std::max(1, std::min(pic.height(), static_cast<int>(std::ceil(devH * 1.5))));
        pic = raster::resize(pic, nw, nh, raster::Filter::Area);
    }
    const Matrix inv = m.inverse();
    const int W = pic.width(), H = pic.height();
    const double alpha = gs_.fillAlpha;
    for (int py = static_cast<int>(std::floor(by0)); py < static_cast<int>(std::ceil(by1)); ++py) {
        for (int px = static_cast<int>(std::floor(bx0)); px < static_cast<int>(std::ceil(bx1)); ++px) {
            double u, v;
            inv.apply(px + 0.5, py + 0.5, u, v);
            if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
            const int sx = std::min(W - 1, static_cast<int>(u * W)), sy = std::min(H - 1, static_cast<int>((1 - v) * H));
            const std::uint8_t* s = pic.pixel(sx, sy);
            if (s[3] == 0) continue;
            blend(*img_, px, py, {s[0], s[1], s[2], 255}, s[3] / 255.0 * alpha);
        }
    }
}

// ---- pages ---------------------------------------------------------------------

std::string contentOf(const PdfFile& f, const Object& page) {
    std::string content;
    if (const Object* c = page.get("Contents")) {
        const Object& cr = f.resolve(*c);
        try {
            if (cr.kind == Object::Array) {
                for (const auto& part : cr.items) content += f.streamData(f.resolve(part)) + "\n";
            } else {
                content = f.streamData(cr);
            }
        } catch (const std::exception&) {
        }
    }
    return content;
}

// user space -> unrotated page points times `scale`
Matrix deviceMatrix(const PdfFile& f, const Object& page, double scale, double& W, double& H) {
    double v[4];
    visibleBox(f, page, v);
    W = v[2] - v[0];
    H = v[3] - v[1];
    return {scale, 0, 0, -scale, -v[0] * scale, v[3] * scale};
}

int rotationOf(const PdfFile& f, const Object& page) {
    const int r = static_cast<int>(numOf(f, page.get("Rotate")));
    return (((r % 360) + 360) % 360) / 90 * 90;
}

const PdfFile::PageInfo& pageAt(const PdfFile& f, int index) {
    if (index < 0 || index >= f.pageCount()) throw std::runtime_error("page " + std::to_string(index + 1) + " is out of range");
    return f.pages()[static_cast<std::size_t>(index)];
}

// Appearances of annotations (and simple stand-ins for ones without).
void drawAnnotations(const PdfFile& f, const Object& page, Interpreter& in, Image& img, const Matrix& dev, const RenderOptions& opt) {
    const Object* annots = page.get("Annots");
    if (!annots) return;
    const Object& arr = f.resolve(*annots);
    for (const auto& a : arr.items) {
        const Object& annot = f.resolve(a);
        if (annot.kind != Object::Dict) continue;
        const int objNum = a.kind == Object::Ref ? static_cast<int>(a.number) : 0;
        if (opt.skipAnnotation && opt.skipAnnotation(objNum, annot)) continue;
        const int flags = static_cast<int>(numOf(f, annot.get("F")));
        if (flags & 2) continue;  // hidden
        const std::string sub = annot.get("Subtype") ? f.resolve(*annot.get("Subtype")).str : "";
        if (sub == "Popup") continue;
        double rect[4];
        if (!boxOf(f, annot.get("Rect"), rect)) continue;
        const Object* ap = nullptr;
        if (const Object* apd = annot.get("AP")) {
            const Object& apDict = f.resolve(*apd);
            if (const Object* nrm = apDict.get("N")) {
                const Object& n = f.resolve(*nrm);
                if (n.kind == Object::Stream) {
                    ap = &n;
                } else if (n.kind == Object::Dict) {
                    std::string state = annot.get("AS") ? f.resolve(*annot.get("AS")).str : "";
                    if (const Object* s = n.get(state)) ap = &f.resolve(*s);
                    if (ap && ap->kind != Object::Stream) ap = nullptr;
                }
            }
        }
        if (ap) {
            double bb[4];
            if (!boxOf(f, ap->get("BBox"), bb)) continue;
            const Matrix fm = matrixOf(f, ap->get("Matrix"));
            double xs[4], ys[4];
            fm.apply(bb[0], bb[1], xs[0], ys[0]);
            fm.apply(bb[2], bb[1], xs[1], ys[1]);
            fm.apply(bb[2], bb[3], xs[2], ys[2]);
            fm.apply(bb[0], bb[3], xs[3], ys[3]);
            const double tx0 = *std::min_element(xs, xs + 4), ty0 = *std::min_element(ys, ys + 4);
            const double tw = *std::max_element(xs, xs + 4) - tx0, th = *std::max_element(ys, ys + 4) - ty0;
            if (tw <= 0 || th <= 0) continue;
            // the form's box lands on the annotation's rectangle
            const Matrix fit{(rect[2] - rect[0]) / tw, 0, 0, (rect[3] - rect[1]) / th, rect[0] - tx0 * (rect[2] - rect[0]) / tw,
                             rect[1] - ty0 * (rect[3] - rect[1]) / th};
            GState gs;
            gs.ctm = dev;
            in.state() = gs;
            static const Object empty;
            in.runForm(*ap, empty, fit, 1);
            continue;
        }
        // no appearance: a simple drawing of the common kinds
        auto devPt = [&](double x, double y) {
            Pt p;
            dev.apply(x, y, p.x, p.y);
            return p;
        };
        Color c{255, 210, 0, 255};
        if (const Object* co = annot.get("C")) {
            const Object& carr = f.resolve(*co);
            double v[4] = {0, 0, 0, 0};
            for (std::size_t k = 0; k < carr.items.size() && k < 4; ++k) v[k] = numOf(f, &carr.items[k]);
            if (carr.items.size() == 3) c = rgbOf(v[0], v[1], v[2]);
            else if (carr.items.size() == 1) c = rgbOf(v[0], v[0], v[0]);
            else if (carr.items.size() == 4) c = cmyk(v[0], v[1], v[2], v[3]);
        }
        const double ca = numOf(f, annot.get("CA"), 1);
        Clip clip;
        if (sub == "Highlight" || sub == "Underline" || sub == "StrikeOut" || sub == "Squiggly") {
            const Object* qp = annot.get("QuadPoints");
            if (!qp) continue;
            const Object& q = f.resolve(*qp);
            for (std::size_t k = 0; k + 7 < q.items.size(); k += 8) {
                double v[8];
                for (int j = 0; j < 8; ++j) v[j] = numOf(f, &q.items[k + static_cast<std::size_t>(j)]);
                const Pt p0 = devPt(v[0], v[1]), p1 = devPt(v[2], v[3]), p2 = devPt(v[6], v[7]), p3 = devPt(v[4], v[5]);
                if (sub == "Highlight") {
                    fillPath(img, {{p0, p1, p2, p3}}, false, c, 0.35 * ca, clip);
                } else {
                    const double t = sub == "StrikeOut" ? 0.5 : 0.92;
                    const Pt a{p0.x + (p3.x - p0.x) * t, p0.y + (p3.y - p0.y) * t}, b{p1.x + (p2.x - p1.x) * t, p1.y + (p2.y - p1.y) * t};
                    strokePath(img, {{a, b}}, {false}, std::max(1.0, std::hypot(p3.x - p0.x, p3.y - p0.y) * 0.07), c, ca, clip);
                }
            }
        } else if (sub == "Ink") {
            const Object* il = annot.get("InkList");
            if (!il) continue;
            Path p;
            for (const auto& stroke : f.resolve(*il).items) {
                const Object& s = f.resolve(stroke);
                std::vector<Pt> pts;
                for (std::size_t k = 0; k + 1 < s.items.size(); k += 2) pts.push_back(devPt(numOf(f, &s.items[k]), numOf(f, &s.items[k + 1])));
                p.push_back(std::move(pts));
            }
            strokePath(img, p, std::vector<bool>(p.size(), false), 1.5 * dev.a, c, ca, clip);
        } else if (sub == "Square" || sub == "Circle") {
            const Pt a = devPt(rect[0], rect[1]), b = devPt(rect[2], rect[3]);
            std::vector<Pt> pts;
            if (sub == "Square") {
                pts = {a, {b.x, a.y}, b, {a.x, b.y}};
            } else {
                for (int k = 0; k < 48; ++k) {
                    const double t = 6.283185307179586 * k / 48;
                    pts.push_back({(a.x + b.x) / 2 + (b.x - a.x) / 2 * std::cos(t), (a.y + b.y) / 2 + (b.y - a.y) / 2 * std::sin(t)});
                }
            }
            strokePath(img, {pts}, {true}, 1.5 * dev.a, c, ca, clip);
        } else if (sub == "Text") {
            const Pt a = devPt(rect[0], rect[3]);
            const double s = 18 * dev.a;
            Path note{{{a.x, a.y}, {a.x + s, a.y}, {a.x + s, a.y + s * 0.75}, {a.x + s * 0.45, a.y + s * 0.75}, {a.x + s * 0.2, a.y + s}, {a.x + s * 0.25, a.y + s * 0.75}, {a.x, a.y + s * 0.75}}};
            fillPath(img, note, false, c, 0.95, clip);
            strokePath(img, note, {true}, std::max(1.0, dev.a), {90, 70, 0, 255}, 0.9, clip);
        } else if (sub == "FreeText" || sub == "Widget") {
            std::string text;
            if (sub == "FreeText") {
                if (const Object* ct = annot.get("Contents")) text = decodeTextString(f.resolve(*ct).str);
            } else {
                // a form field's value (the value can live on a parent field)
                const Object* node = &annot;
                for (int hops = 0; hops < 8 && node; ++hops) {
                    if (const Object* v = node->get("V")) {
                        const Object& vv = f.resolve(*v);
                        if (vv.kind == Object::String) text = decodeTextString(vv.str);
                        else if (vv.kind == Object::Name && vv.str != "Off") text = "\xE2\x9C\x93";  // a tick
                        break;
                    }
                    const Object* parent = node->get("Parent");
                    node = parent ? &f.resolve(*parent) : nullptr;
                }
            }
            if (text.empty()) continue;
            const Pt a = devPt(rect[0], rect[3]), b = devPt(rect[2], rect[1]);
            const double size = std::min(12.0 * dev.a, (b.y - a.y) * 0.75);
            double y = a.y + size * 1.05;
            std::size_t pos = 0;
            while (pos <= text.size() && y < b.y + size) {
                std::size_t nl = text.find_first_of("\r\n", pos);
                std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                drawText(img, a.x + 2 * dev.a, y, line, Font{}, size, {0, 0, 0, 255});
                if (nl == std::string::npos) break;
                pos = nl + 1;
                y += size * 1.2;
            }
        }
    }
}

}  // namespace

PageBox pageBox(const PdfFile& file, int index) {
    const auto& info = pageAt(file, index);
    double W, H;
    const Matrix dev = deviceMatrix(file, info.dict, 1, W, H);
    PageBox b;
    b.rotate = rotationOf(file, info.dict);
    b.width = (b.rotate == 90 || b.rotate == 270) ? H : W;
    b.height = (b.rotate == 90 || b.rotate == 270) ? W : H;
    b.toPage = dev * turn(b.rotate, W, H);
    return b;
}

std::string pageContent(const PdfFile& file, int index) { return contentOf(file, pageAt(file, index).dict); }

raster::Image renderPdfPage(const PdfFile& file, int index, double scale, const RenderOptions& options) {
    const auto& info = pageAt(file, index);
    double W, H;
    const Matrix dev = deviceMatrix(file, info.dict, scale, W, H);
    const int iw = std::max(1, static_cast<int>(std::lround(W * scale))), ih = std::max(1, static_cast<int>(std::lround(H * scale)));
    if (static_cast<double>(iw) * ih > 1.2e8) throw std::runtime_error("the page is too big to draw at that size");
    Image img(iw, ih, options.background);
    Interpreter in(file, &img, nullptr);
    GState gs;
    gs.ctm = dev;
    gs.clip.intersect(0, 0, iw, ih);
    const Object& res = dictOf(file, info.dict.get("Resources"));
    try {
        in.runPage(options.content ? *options.content : contentOf(file, info.dict), res, gs);
    } catch (const std::exception&) {
    }
    if (options.annotations) {
        try {
            drawAnnotations(file, info.dict, in, img, dev, options);
        } catch (const std::exception&) {
        }
    }
    const int rot = rotationOf(file, info.dict);
    if (rot) img = raster::rotate90(img, rot / 90);
    return img;
}

namespace {

std::vector<TextChar> collect(const PdfFile& file, int index, std::vector<TextOpRecord>* records, const std::string* content) {
    const auto& info = pageAt(file, index);
    std::vector<TextChar> chars;
    Interpreter in(file, nullptr, &chars);
    in.keepRecords = records != nullptr;
    double W, H;
    GState gs;
    gs.ctm = deviceMatrix(file, info.dict, 1, W, H);
    const Object& res = dictOf(file, info.dict.get("Resources"));
    try {
        in.runPage(content ? *content : contentOf(file, info.dict), res, gs);
    } catch (const std::exception&) {
    }
    const int rot = rotationOf(file, info.dict);
    if (rot) {
        const Matrix t = turn(rot, W, H);
        for (auto& c : chars) {
            double xs[2], ys[2];
            t.apply(c.x0, c.y0, xs[0], ys[0]);
            t.apply(c.x1, c.y1, xs[1], ys[1]);
            t.apply(c.ox, c.oy, c.ox, c.oy);
            c.x0 = std::min(xs[0], xs[1]);
            c.x1 = std::max(xs[0], xs[1]);
            c.y0 = std::min(ys[0], ys[1]);
            c.y1 = std::max(ys[0], ys[1]);
        }
    }
    if (records) *records = std::move(in.records);
    return chars;
}

std::string hexString(const std::string& bytes) {
    static const char* hx = "0123456789ABCDEF";
    std::string s = "<";
    for (char c : bytes) {
        s += hx[(static_cast<unsigned char>(c) >> 4) & 15];
        s += hx[static_cast<unsigned char>(c) & 15];
    }
    return s + ">";
}

}  // namespace

void fillShapes(raster::Image& img, const std::vector<Polygon>& shapes, raster::Color color, double alpha, bool evenOdd) {
    Path p;
    for (const auto& s : shapes) {
        std::vector<Pt> pts;
        for (const auto& q : s) pts.push_back({q[0], q[1]});
        p.push_back(std::move(pts));
    }
    fillPath(img, p, evenOdd, color, alpha * color.a / 255.0, Clip{});
}

void strokeLines(raster::Image& img, const std::vector<Polygon>& lines, bool closed, double width, raster::Color color, double alpha) {
    Path p;
    for (const auto& s : lines) {
        std::vector<Pt> pts;
        for (const auto& q : s) pts.push_back({q[0], q[1]});
        p.push_back(std::move(pts));
    }
    strokePath(img, p, std::vector<bool>(p.size(), closed), width, color, alpha * color.a / 255.0, Clip{});
}

std::vector<TextChar> pageChars(const PdfFile& file, int index, const std::string* content) { return collect(file, index, nullptr, content); }

std::string removeGlyphs(const PdfFile& file, int index, const std::vector<std::pair<int, int>>& glyphs, const std::string* from) {
    std::vector<TextOpRecord> records;
    collect(file, index, &records, from);
    std::string content = from ? *from : pageContent(file, index);
    std::map<int, std::set<int>> drop;
    for (const auto& [op, g] : glyphs) {
        if (op >= 0) drop[op].insert(g);
    }
    // rewrite from the back so earlier offsets stay valid
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) {
        if (it->first >= static_cast<int>(records.size())) continue;
        const TextOpRecord& r = records[static_cast<std::size_t>(it->first)];
        if (r.end > content.size() || r.start >= r.end) continue;
        std::string arr;
        std::string run;
        double kern = 0;
        auto flushRun = [&] {
            if (!run.empty()) arr += hexString(run);
            run.clear();
        };
        auto flushKern = [&] {
            if (std::abs(kern) > 1e-9) {
                flushRun();
                arr += " " + detail::num(kern) + " ";
            }
            kern = 0;
        };
        for (std::size_t g = 0; g < r.codes.size(); ++g) {
            kern += r.kernBefore[g];
            if (it->second.count(static_cast<int>(g))) {
                kern -= r.advance[g];
                continue;
            }
            flushKern();
            run += r.codes[g];
        }
        kern += r.kernAfter;
        flushKern();
        flushRun();
        std::string repl;
        if (r.op == "'") repl = "T* ";
        else if (r.op == "\"") repl = detail::num(r.aw) + " Tw " + detail::num(r.ac) + " Tc T* ";
        repl += "[" + arr + "] TJ";
        content.replace(r.start, r.end - r.start, repl);
    }
    return content;
}

std::vector<TextLine> textLines(const std::vector<TextChar>& chars) {
    std::vector<TextLine> lines;
    TextLine cur;
    double lastX1 = 0, baseY = 0, size = 0;
    auto flush = [&] {
        if (!cur.text.empty()) lines.push_back(std::move(cur));
        cur = TextLine{};
    };
    for (std::size_t i = 0; i < chars.size(); ++i) {
        const TextChar& c = chars[i];
        const double cy = c.y1;
        const bool sameLine = !cur.text.empty() && std::abs(cy - baseY) < std::max(size, c.size) * 0.5 && c.x0 > lastX1 - std::max(size, c.size) * 0.6 &&
                              c.x0 < lastX1 + std::max(size, c.size) * 4;
        if (!cur.text.empty() && !sameLine) flush();
        if (cur.text.empty()) {
            cur.x0 = c.x0;
            cur.y0 = c.y0;
            cur.x1 = c.x1;
            cur.y1 = c.y1;
        } else if (c.x0 > lastX1 + c.size * 0.15 && cur.text.back() != ' ' && c.text != " ") {
            cur.text += ' ';
            cur.chars.push_back(-1);
        }
        cur.text += c.text;
        for (std::size_t k = 0; k < c.text.size(); ++k) cur.chars.push_back(static_cast<int>(i));
        cur.x0 = std::min(cur.x0, c.x0);
        cur.y0 = std::min(cur.y0, c.y0);
        cur.x1 = std::max(cur.x1, c.x1);
        cur.y1 = std::max(cur.y1, c.y1);
        lastX1 = c.x1;
        baseY = cy;
        size = c.size;
    }
    flush();
    return lines;
}

std::vector<TextMatch> findText(const std::vector<TextChar>& chars, const std::string& query, bool matchCase) {
    std::vector<TextMatch> out;
    if (query.empty()) return out;
    auto fold = [&](std::string s) { return matchCase ? s : lowerAscii(std::move(s)); };
    const std::string q = fold(query);
    const auto lines = textLines(chars);
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const std::string hay = fold(lines[li].text);
        for (std::size_t at = hay.find(q); at != std::string::npos; at = hay.find(q, at + q.size())) {
            TextMatch m;
            m.line = static_cast<int>(li);
            m.text = lines[li].text.substr(at, q.size());
            double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
            for (std::size_t k = at; k < at + q.size(); ++k) {
                const int ci = lines[li].chars[k];
                if (ci < 0 || (!m.chars.empty() && m.chars.back() == ci)) continue;
                m.chars.push_back(ci);
                const TextChar& c = chars[static_cast<std::size_t>(ci)];
                x0 = std::min(x0, c.x0);
                y0 = std::min(y0, c.y0);
                x1 = std::max(x1, c.x1);
                y1 = std::max(y1, c.y1);
            }
            if (m.chars.empty()) continue;
            m.boxes.push_back({x0, y0, x1 - x0, y1 - y0});
            out.push_back(std::move(m));
        }
    }
    return out;
}

}  // namespace wetwaresuite::pdf
