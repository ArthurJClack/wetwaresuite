// DXF in and out. Files are ASCII AC1015 (AutoCAD 2000) with handles, the
// standard tables, real DIMENSION entities (each with its *D block of drawn
// geometry) and HATCH. Walls, doors, windows and rooms are written as the
// lines, arcs and text they are drawn with, so any CAD program shows them;
// the first piece of each carries the full definition as wetwaresuite XDATA so
// reopening the file here gets the editable wall back. Sheet settings
// (paper, scale, title block) live in an XRECORD in the OBJECTS section.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/blueprint/blueprint.hpp"
#include "internal.hpp"

namespace wetwaresuite::blueprint {

namespace {

constexpr const char* kApp = "wetwaresuite";
constexpr const char* kSheetLayer = "BP_SHEET";
constexpr const char* kMeta = "wetwaresuite-blueprint";


std::string upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

std::string g(double v) {
    if (std::abs(v) < 1e-12) return "0.0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

// UTF-8 to DXF text: ASCII with %%d (degree), %%p (plus-minus), %%c (diameter) and \U+XXXX escapes.
std::string encodeText(const std::string& text) {
    std::string out;
    for (unsigned cp : detail::utf8Decode(text)) {
        if (cp == '\n' || cp == '\r') {
            out += ' ';
        } else if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp == 0xB0) {
            out += "%%d";
        } else if (cp == 0xB1) {
            out += "%%p";
        } else if (cp == 0xD8 || cp == 0x2300) {
            out += "%%c";
        } else {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\\U+%04X", cp);
            out += buf;
        }
    }
    return out;
}

std::string decodeText(const std::string& raw) {
    std::string s;
    for (unsigned cp : detail::utf8Decode(raw)) s += detail::utf8Encode(cp);
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 2, "%%") == 0 && i + 2 < s.size()) {
            char code = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i + 2])));
            if (code == 'd') out += "°";
            else if (code == 'p') out += "±";
            else if (code == 'c') out += "Ø";
            else if (code == '%') out += "%";
            else if (code != 'u' && code != 'o' && code != 'k') out += s.substr(i, 3);  // %%u/%%o/%%k toggle underline etc.
            i += 2;
        } else if ((s.compare(i, 3, "\\U+") == 0 || s.compare(i, 3, "\\u+") == 0) && i + 7 <= s.size()) {
            unsigned cp = static_cast<unsigned>(std::strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16));
            out += detail::utf8Encode(cp);
            i += 6;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string escapeField(const std::string& s) {
    std::string out;
    for (unsigned cp : detail::utf8Decode(s)) {
        if (cp == ';' || cp == '=' || cp == '\\' || cp == '|' || cp >= 0x80 || cp < 0x20) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\\%06X", cp);
            out += buf;
        } else {
            out += static_cast<char>(cp);
        }
    }
    return out;
}

std::string unescapeField(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\') {
            if (i + 7 > s.size()) break;
            out += detail::utf8Encode(static_cast<unsigned>(std::strtoul(s.substr(i + 1, 6).c_str(), nullptr, 16)));
            i += 6;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---- writing ----------------------------------------------------------------

class Writer {
public:
    std::string out;
    void code(int c, const std::string& v) {
        out += std::to_string(c);
        out += '\n';
        out += v;
        out += '\n';
    }
    void num(int c, double v) { code(c, g(v)); }
    void integer(int c, long v) { code(c, std::to_string(v)); }
    void point(int c, Vec2 p) {
        num(c, p.x);
        num(c + 10, p.y);
        num(c + 20, 0);
    }
};

class Handles {
public:
    std::string next() {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%X", n_++);
        return buf;
    }
    std::string seed() const {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%X", n_);
        return buf;
    }

private:
    unsigned n_ = 0x20;
};

int lineweight(double mm) {
    static const int standard[] = {0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};
    int best = 25;
    double bestD = 1e9;
    for (int w : standard) {
        double d = std::abs(w - mm * 100);
        if (d < bestD) {
            bestD = d;
            best = w;
        }
    }
    return best;
}

std::string linetypeName(LineType t) {
    switch (t) {
        case LineType::Continuous: return "Continuous";
        case LineType::Dashed: return "DASHED";
        case LineType::Center: return "CENTER";
        case LineType::Hidden: return "HIDDEN";
    }
    return "Continuous";
}

void colorCodes(Writer& w, Color c, bool negative = false) {
    int aci = nearestAci(c);
    w.integer(62, negative ? -aci : aci);
    Color exact = aciColor(aci);
    const bool isAci = aci == 7 ? (c.r == c.g && c.g == c.b && (c.r == 0 || c.r == 255)) : exact == Color{c.r, c.g, c.b, 255};
    if (!isAci) w.integer(420, (static_cast<long>(c.r) << 16) | (static_cast<long>(c.g) << 8) | c.b);
}

std::string serializeDef(const Entity& e) {
    std::string s = std::string("def;type=") + toString(e.type) + ";id=" + e.id;
    if (!e.pts.empty()) {
        s += ";pts=";
        for (std::size_t i = 0; i < e.pts.size(); ++i) s += (i ? "," : "") + g(e.pts[i].x) + "," + g(e.pts[i].y);
    }
    switch (e.type) {
        case EntityType::Wall:
            s += ";closed=" + std::string(e.closed ? "1" : "0") + ";thickness=" + g(e.thickness) + ";fill=" + e.fill;
            break;
        case EntityType::Door:
        case EntityType::Window:
            s += ";host=" + e.host + ";at=" + g(e.at) + ";width=" + g(e.width) + ";hinge=" + (e.hingeEnd ? "end" : "start") +
                 ";side=" + (e.swingRight ? "right" : "left");
            break;
        case EntityType::Room:
            s += ";name=" + escapeField(e.name) + ";c=" + g(e.c.x) + "," + g(e.c.y) + ";area=" + (e.showArea ? "1" : "0") +
                 ";outline=" + (e.outline ? "1" : "0");
            break;
        default: break;
    }
    return s;
}

void xdata(Writer& w, const std::string& data) {
    if (data.empty()) return;
    w.code(1001, kApp);
    for (std::size_t i = 0; i < data.size(); i += 240) w.code(1000, data.substr(i, 240));
}

struct EntityContext {
    std::string owner;
    std::string layerOverride;  // inside dimension blocks everything goes on layer 0
    bool inBlock = false;
};

// Writes one simple entity. `tag` is XDATA (empty for none).
void writeBasic(Writer& w, Handles& h, const Drawing& doc, const Entity& e, const EntityContext& ctx, const std::string& tag) {
    const std::string layer = ctx.layerOverride.empty() ? e.layer : ctx.layerOverride;
    auto head = [&](const char* type) {
        w.code(0, type);
        w.code(5, h.next());
        w.code(330, ctx.owner);
        w.code(100, "AcDbEntity");
        w.code(8, layer);
        if (e.color && !ctx.inBlock) colorCodes(w, *e.color);
    };
    switch (e.type) {
        case EntityType::Line:
            if (e.pts.size() < 2) return;
            head("LINE");
            w.code(100, "AcDbLine");
            w.point(10, e.pts[0]);
            w.point(11, e.pts[1]);
            break;
        case EntityType::Polyline:
            if (e.pts.size() < 2) return;
            head("LWPOLYLINE");
            w.code(100, "AcDbPolyline");
            w.integer(90, static_cast<long>(e.pts.size()));
            w.integer(70, e.closed ? 1 : 0);
            w.num(43, 0);
            for (auto p : e.pts) {
                w.num(10, p.x);
                w.num(20, p.y);
            }
            break;
        case EntityType::Circle:
            head("CIRCLE");
            w.code(100, "AcDbCircle");
            w.point(10, e.c);
            w.num(40, e.r);
            break;
        case EntityType::Arc:
            head("ARC");
            w.code(100, "AcDbCircle");
            w.point(10, e.c);
            w.num(40, e.r);
            w.code(100, "AcDbArc");
            w.num(50, normalizeAngle(e.a0));
            w.num(51, normalizeAngle(e.a1));
            break;
        case EntityType::Point:
            head("POINT");
            w.code(100, "AcDbPoint");
            w.point(10, e.c);
            break;
        case EntityType::Text: {
            head("TEXT");
            w.code(100, "AcDbText");
            const double width = textWidth(e.text) * e.height;
            Vec2 ux = direction(e.rotation), uy = perp(ux);
            const double dx = e.halign == HAlign::Left ? 0 : e.halign == HAlign::Center ? -width / 2 : -width;
            const double dy = e.valign == VAlign::Baseline ? 0 : e.valign == VAlign::Middle ? -e.height / 2 : -e.height;
            w.point(10, e.c + ux * dx + uy * dy);
            w.num(40, e.height);
            w.code(1, encodeText(e.text));
            if (std::abs(e.rotation) > 1e-12) w.num(50, normalizeAngle(e.rotation));
            w.code(7, "Standard");
            const int h72 = e.halign == HAlign::Left ? 0 : e.halign == HAlign::Center ? 1 : 2;
            const int v73 = e.valign == VAlign::Baseline ? 0 : e.valign == VAlign::Middle ? 2 : 3;
            if (h72) w.integer(72, h72);
            if (h72 || v73) w.point(11, e.c);
            w.code(100, "AcDbText");
            if (v73) w.integer(73, v73);
            break;
        }
        case EntityType::Hatch: {
            if (e.pts.size() < 3) return;
            if (ctx.inBlock && e.pattern == "solid" && e.pts.size() <= 4) {
                head("SOLID");
                w.code(100, "AcDbTrace");
                const std::vector<Vec2>& p = e.pts;
                w.point(10, p[0]);
                w.point(11, p[1]);
                w.point(12, p.size() == 4 ? p[3] : p[2]);
                w.point(13, p[2]);
                break;
            }
            head("HATCH");
            w.code(100, "AcDbHatch");
            w.point(10, {0, 0});
            w.num(210, 0);
            w.num(220, 0);
            w.num(230, 1);
            const bool solid = e.pattern != "lines" && e.pattern != "cross";
            w.code(2, solid ? "SOLID" : "_USER");
            w.integer(70, solid ? 1 : 0);
            w.integer(71, 0);
            w.integer(91, 1);
            w.integer(92, 3);
            w.integer(72, 0);
            w.integer(73, 1);
            w.integer(93, static_cast<long>(e.pts.size()));
            for (auto p : e.pts) {
                w.num(10, p.x);
                w.num(20, p.y);
            }
            w.integer(97, 0);
            w.integer(75, 0);
            w.integer(76, solid ? 1 : 0);
            if (!solid) {
                const double spacing = e.spacing / doc.k();
                w.num(52, e.angle);
                w.num(41, spacing);
                w.integer(77, e.pattern == "cross" ? 1 : 0);
                const int lines = e.pattern == "cross" ? 2 : 1;
                w.integer(78, lines);
                for (int i = 0; i < lines; ++i) {
                    const double a = e.angle + 90.0 * i;
                    Vec2 off = perp(direction(a)) * spacing;
                    w.num(53, a);
                    w.num(43, 0);
                    w.num(44, 0);
                    w.num(45, off.x);
                    w.num(46, off.y);
                    w.integer(79, 0);
                }
            }
            w.integer(98, 1);
            Vec2 seed = polygonCentroid(e.pts);
            w.num(10, seed.x);
            w.num(20, seed.y);
            break;
        }
        default: return;
    }
    xdata(w, tag);
}

// DIMENSION group 70 type and the definition points.
void writeDimension(Writer& w, Handles& h, const Drawing& doc, const Entity& d, const std::string& owner, const std::string& block) {
    w.code(0, "DIMENSION");
    w.code(5, h.next());
    w.code(330, owner);
    w.code(100, "AcDbEntity");
    w.code(8, d.layer);
    if (d.color) colorCodes(w, *d.color);
    w.code(100, "AcDbDimension");
    w.code(2, block);
    // text position: where the exploded label sits
    Vec2 textAt = d.c;
    for (const auto& piece : explode(doc, d)) {
        if (piece.e.type == EntityType::Text) textAt = piece.e.c;
    }
    int type = 0;
    Vec2 def10;
    switch (d.dim) {
        case DimKind::Aligned:
        case DimKind::Horizontal:
        case DimKind::Vertical: {
            type = d.dim == DimKind::Aligned ? 1 : 0;
            Vec2 u = d.dim == DimKind::Horizontal ? Vec2{1, 0} : d.dim == DimKind::Vertical ? Vec2{0, 1} : normalized(d.pts[1] - d.pts[0]);
            Vec2 n = perp(u);
            def10 = d.pts[1] + n * dot(d.pts[2] - d.pts[1], n);
            break;
        }
        case DimKind::Radius:
            type = 4;
            def10 = d.c;
            break;
        case DimKind::Diameter:
            type = 3;
            def10 = d.c - direction(d.rotation) * d.r;
            break;
        case DimKind::Angular:
            type = 5;
            def10 = d.pts[3];
            break;
    }
    w.point(10, def10);
    w.point(11, textAt);
    w.integer(70, type | 32 | 128);
    w.code(1, encodeText(d.text));
    w.code(3, "Standard");
    w.num(42, dimensionValue(d));
    switch (d.dim) {
        case DimKind::Aligned:
            w.code(100, "AcDbAlignedDimension");
            w.point(13, d.pts[0]);
            w.point(14, d.pts[1]);
            break;
        case DimKind::Horizontal:
        case DimKind::Vertical:
            w.code(100, "AcDbAlignedDimension");
            w.point(13, d.pts[0]);
            w.point(14, d.pts[1]);
            w.num(50, d.dim == DimKind::Vertical ? 90 : 0);
            w.code(100, "AcDbRotatedDimension");
            break;
        case DimKind::Radius:
            w.code(100, "AcDbRadialDimension");
            w.point(15, d.c + direction(d.rotation) * d.r);
            w.num(40, 0);
            break;
        case DimKind::Diameter:
            w.code(100, "AcDbDiametricDimension");
            w.point(15, d.c + direction(d.rotation) * d.r);
            w.num(40, 0);
            break;
        case DimKind::Angular:
            w.code(100, "AcDb3PointAngularDimension");
            w.point(13, d.pts[1]);
            w.point(14, d.pts[2]);
            w.point(15, d.pts[0]);
            break;
    }
}

bool validDimension(const Entity& d) {
    if (d.dim == DimKind::Radius || d.dim == DimKind::Diameter) return d.r > 0;
    return d.pts.size() >= (d.dim == DimKind::Angular ? 4u : 3u);
}

std::string metaLine(const std::string& key, const std::string& value) { return key + "=" + escapeField(value); }

}  // namespace

std::string toDxf(const Drawing& doc, bool sheet) {
    Handles h;
    const double k = doc.k();

    // the drawing's own layers plus the sheet layer when asked for
    std::vector<Layer> layers = doc.layers;
    std::vector<detail::SheetItem> sheetItems;
    if (sheet) {
        sheetItems = detail::sheetItems(doc);
        if (!sheetItems.empty()) layers.push_back(Layer{kSheetLayer, {0, 0, 0, 255}, 0.35});
    }
    std::vector<const Entity*> dims;
    for (const auto& e : doc.entities) {
        if (e.type == EntityType::Dimension && validDimension(e)) dims.push_back(&e);
    }

    // ---- tables ----
    Writer t;
    auto table = [&](const char* name, std::size_t count, const std::function<void(const std::string&)>& records,
                     bool dimstyle = false) {
        const std::string th = h.next();
        t.code(0, "TABLE");
        t.code(2, name);
        t.code(5, th);
        t.code(330, "0");
        t.code(100, "AcDbSymbolTable");
        t.integer(70, static_cast<long>(count));
        if (dimstyle) {
            t.code(100, "AcDbDimStyleTable");
            t.integer(71, 0);
        }
        records(th);
        t.code(0, "ENDTAB");
    };
    auto record = [&](const char* type, const std::string& owner, const char* subclass, bool dimstyle = false) {
        t.code(0, type);
        t.code(dimstyle ? 105 : 5, h.next());
        t.code(330, owner);
        t.code(100, "AcDbSymbolTableRecord");
        t.code(100, subclass);
    };

    Box ext = extents(doc);
    if (ext.empty) {
        ext.add(doc.fromPaper({0, 0}));
        ext.add(doc.fromPaper({doc.paperWidth(), doc.paperHeight()}));
    }

    table("VPORT", 1, [&](const std::string& owner) {
        record("VPORT", owner, "AcDbViewportTableRecord");
        t.code(2, "*ACTIVE");
        t.integer(70, 0);
        t.num(10, 0);
        t.num(20, 0);
        t.num(11, 1);
        t.num(21, 1);
        t.num(12, (ext.x0 + ext.x1) / 2);
        t.num(22, (ext.y0 + ext.y1) / 2);
        t.num(13, 0);
        t.num(23, 0);
        t.num(14, 1);
        t.num(24, 1);
        t.num(15, 1);
        t.num(25, 1);
        t.num(16, 0);
        t.num(26, 0);
        t.num(36, 1);
        t.num(17, 0);
        t.num(27, 0);
        t.num(37, 0);
        t.num(40, std::max(1.0, ext.height() * 1.1));
        t.num(41, std::max(1.0, ext.width()) / std::max(1.0, ext.height()));
        t.num(42, 50);
        t.num(43, 0);
        t.num(44, 0);
        t.num(50, 0);
        t.num(51, 0);
        t.integer(71, 0);
        t.integer(72, 100);
        t.integer(73, 1);
        t.integer(74, 3);
        t.integer(75, 0);
        t.integer(76, 0);
        t.integer(77, 0);
        t.integer(78, 0);
    });

    const LineType types[] = {LineType::Continuous, LineType::Dashed, LineType::Center, LineType::Hidden};
    table("LTYPE", 6, [&](const std::string& owner) {
        for (const char* name : {"ByBlock", "ByLayer"}) {
            record("LTYPE", owner, "AcDbLinetypeTableRecord");
            t.code(2, name);
            t.integer(70, 0);
            t.code(3, "");
            t.integer(72, 65);
            t.integer(73, 0);
            t.num(40, 0);
        }
        for (LineType lt : types) {
            record("LTYPE", owner, "AcDbLinetypeTableRecord");
            t.code(2, linetypeName(lt));
            t.integer(70, 0);
            t.code(3, lt == LineType::Continuous ? "Solid line" : lt == LineType::Dashed ? "Dashed __ __ __" : lt == LineType::Center ? "Center ____ _ ____" : "Hidden _ _ _ _");
            t.integer(72, 65);
            auto pattern = dashPattern(lt);
            t.integer(73, static_cast<long>(pattern.size()));
            double total = 0;
            for (double d : pattern) total += d / k;
            t.num(40, total);
            for (std::size_t i = 0; i < pattern.size(); ++i) {
                t.num(49, (i % 2 ? -1 : 1) * pattern[i] / k);
                t.integer(74, 0);
            }
        }
    });

    table("LAYER", layers.size(), [&](const std::string& owner) {
        for (const auto& l : layers) {
            record("LAYER", owner, "AcDbLayerTableRecord");
            t.code(2, l.name);
            t.integer(70, l.locked ? 4 : 0);
            colorCodes(t, l.color, !l.visible);
            t.code(6, linetypeName(l.linetype));
            t.integer(370, lineweight(l.weight));
        }
    });

    table("STYLE", 1, [&](const std::string& owner) {
        record("STYLE", owner, "AcDbTextStyleTableRecord");
        t.code(2, "Standard");
        t.integer(70, 0);
        t.num(40, 0);
        t.num(41, 1);
        t.num(50, 0);
        t.integer(71, 0);
        t.num(42, doc.dims.textHeight / k);
        t.code(3, "arial.ttf");
        t.code(4, "");
    });
    table("VIEW", 0, [](const std::string&) {});
    table("UCS", 0, [](const std::string&) {});
    table("APPID", 2, [&](const std::string& owner) {
        for (const char* name : {"ACAD", kApp}) {
            record("APPID", owner, "AcDbRegAppTableRecord");
            t.code(2, name);
            t.integer(70, 0);
        }
    });
    table("DIMSTYLE", 1, [&](const std::string& owner) {
        record("DIMSTYLE", owner, "AcDbDimStyleTableRecord", true);
        t.code(2, "Standard");
        t.integer(70, 0);
        t.num(40, 1);
        t.num(41, doc.dims.arrowSize / k);
        t.num(42, doc.dims.extOffset / k);
        t.num(44, doc.dims.extBeyond / k);
        t.num(140, doc.dims.textHeight / k);
        t.num(147, doc.dims.textGap / k);
        t.integer(271, doc.dims.precision < 0 ? 0 : doc.dims.precision);
    }, true);

    // block records: model space, paper space and one *D block per dimension
    std::string modelBR, paperBR;
    std::vector<std::string> dimBR;
    table("BLOCK_RECORD", 2 + dims.size(), [&](const std::string& owner) {
        auto br = [&](const std::string& name) {
            const std::string hh = h.next();
            t.code(0, "BLOCK_RECORD");
            t.code(5, hh);
            t.code(330, owner);
            t.code(100, "AcDbSymbolTableRecord");
            t.code(100, "AcDbBlockTableRecord");
            t.code(2, name);
            return hh;
        };
        modelBR = br("*Model_Space");
        paperBR = br("*Paper_Space");
        for (std::size_t i = 0; i < dims.size(); ++i) dimBR.push_back(br("*D" + std::to_string(i + 1)));
    });

    // ---- blocks ----
    Writer b;
    auto block = [&](const std::string& name, const std::string& owner, bool paper, int flags,
                     const std::function<void()>& contents) {
        b.code(0, "BLOCK");
        b.code(5, h.next());
        b.code(330, owner);
        b.code(100, "AcDbEntity");
        if (paper) b.integer(67, 1);
        b.code(8, "0");
        b.code(100, "AcDbBlockBegin");
        b.code(2, name);
        b.integer(70, flags);
        b.point(10, {0, 0});
        b.code(3, name);
        b.code(1, "");
        contents();
        b.code(0, "ENDBLK");
        b.code(5, h.next());
        b.code(330, owner);
        b.code(100, "AcDbEntity");
        if (paper) b.integer(67, 1);
        b.code(8, "0");
        b.code(100, "AcDbBlockEnd");
    };
    block("*Model_Space", modelBR, false, 0, [] {});
    block("*Paper_Space", paperBR, true, 0, [] {});
    for (std::size_t i = 0; i < dims.size(); ++i) {
        block("*D" + std::to_string(i + 1), dimBR[i], false, 1, [&] {
            EntityContext ctx{dimBR[i], "0", true};
            for (const auto& piece : explode(doc, *dims[i])) writeBasic(b, h, doc, piece.e, ctx, "");
        });
    }

    // ---- entities ----
    Writer en;
    EntityContext model{modelBR, "", false};
    std::size_t dimIndex = 0;
    for (const auto& e : doc.entities) {
        switch (e.type) {
            case EntityType::Dimension:
                if (validDimension(e)) writeDimension(en, h, doc, e, modelBR, "*D" + std::to_string(++dimIndex));
                break;
            case EntityType::Wall:
            case EntityType::Door:
            case EntityType::Window:
            case EntityType::Room: {
                auto pieces = explode(doc, e);
                const std::string def = serializeDef(e);
                if (pieces.empty()) {
                    Entity carrier;
                    carrier.type = EntityType::Point;
                    carrier.layer = e.layer;
                    carrier.c = e.pts.empty() ? e.c : e.pts[0];
                    writeBasic(en, h, doc, carrier, model, def);
                    break;
                }
                for (std::size_t i = 0; i < pieces.size(); ++i) writeBasic(en, h, doc, pieces[i].e, model, i == 0 ? def : "part");
                break;
            }
            default: writeBasic(en, h, doc, e, model, ""); break;
        }
    }
    for (const auto& item : sheetItems) {
        Entity e = item.e;
        e.layer = kSheetLayer;
        for (auto& p : e.pts) p = doc.fromPaper(p);
        e.c = doc.fromPaper(e.c);
        e.height /= k;
        writeBasic(en, h, doc, e, model, "");
    }

    // ---- objects: root dictionary, groups, and our sheet settings ----
    Writer ob;
    const std::string root = h.next(), groups = h.next(), xrec = h.next();
    ob.code(0, "DICTIONARY");
    ob.code(5, root);
    ob.code(330, "0");
    ob.code(100, "AcDbDictionary");
    ob.integer(281, 1);
    ob.code(3, "ACAD_GROUP");
    ob.code(350, groups);
    ob.code(3, "WETWARESUITE_BLUEPRINT");
    ob.code(350, xrec);
    ob.code(0, "DICTIONARY");
    ob.code(5, groups);
    ob.code(330, root);
    ob.code(100, "AcDbDictionary");
    ob.integer(281, 1);
    ob.code(0, "XRECORD");
    ob.code(5, xrec);
    ob.code(330, root);
    ob.code(100, "AcDbXrecord");
    ob.integer(280, 1);
    const DimStyle& ds = doc.dims;
    for (const std::string& line :
         {std::string(kMeta) + " 1", metaLine("units", toString(doc.units)), metaLine("scale", doc.scaleText()), metaLine("paper", doc.paper),
          metaLine("orientation", doc.landscape ? "landscape" : "portrait"), metaLine("origin", g(doc.origin.x) + "," + g(doc.origin.y)),
          metaLine("titleblock", doc.title.show ? "1" : "0"), metaLine("title", doc.title.title), metaLine("author", doc.title.author),
          metaLine("date", doc.title.date), metaLine("sheet", doc.title.sheet), metaLine("scaletext", doc.title.scale),
          metaLine("dims", g(ds.textHeight) + "," + g(ds.arrowSize) + "," + g(ds.extOffset) + "," + g(ds.extBeyond) + "," + g(ds.textGap) +
                               "," + ds.arrows + "," + std::to_string(ds.precision) + "," + (ds.suffix ? "1" : "0")),
          metaLine("grid", g(doc.grid.spacing) + "," + g(doc.grid.snap) + "," + (doc.grid.show ? "1" : "0")), metaLine("current", doc.current),
          metaLine("nextid", std::to_string(doc.nextId))}) {
        ob.code(1, line);
    }

    // ---- header, written last so $HANDSEED is final ----
    Writer hd;
    hd.code(0, "SECTION");
    hd.code(2, "HEADER");
    auto var = [&](const char* name) { hd.code(9, name); };
    var("$ACADVER");
    hd.code(1, "AC1015");
    var("$DWGCODEPAGE");
    hd.code(3, "ANSI_1252");
    var("$INSBASE");
    hd.point(10, {0, 0});
    var("$EXTMIN");
    hd.point(10, {ext.x0, ext.y0});
    var("$EXTMAX");
    hd.point(10, {ext.x1, ext.y1});
    var("$LIMMIN");
    hd.num(10, doc.origin.x);
    hd.num(20, doc.origin.y);
    var("$LIMMAX");
    Vec2 lim = doc.fromPaper({doc.paperWidth(), doc.paperHeight()});
    hd.num(10, lim.x);
    hd.num(20, lim.y);
    var("$LTSCALE");
    hd.num(40, 1);
    var("$DIMSCALE");
    hd.num(40, 1);
    var("$DIMTXT");
    hd.num(40, doc.dims.textHeight / k);
    var("$DIMASZ");
    hd.num(40, doc.dims.arrowSize / k);
    var("$LUNITS");
    hd.integer(70, doc.units == Unit::Ft || doc.units == Unit::In ? 4 : 2);
    var("$INSUNITS");
    hd.integer(70, doc.units == Unit::Mm ? 4 : doc.units == Unit::Cm ? 5 : doc.units == Unit::M ? 6 : doc.units == Unit::In ? 1 : 2);
    var("$MEASUREMENT");
    hd.integer(70, metric(doc.units) ? 1 : 0);
    var("$HANDSEED");
    hd.code(5, h.seed());
    hd.code(0, "ENDSEC");

    std::string out = hd.out;
    auto section = [&](const char* name, const std::string& body) {
        out += "0\nSECTION\n2\n";
        out += name;
        out += "\n" + body + "0\nENDSEC\n";
    };
    section("CLASSES", "");
    section("TABLES", t.out);
    section("BLOCKS", b.out);
    section("ENTITIES", en.out);
    section("OBJECTS", ob.out);
    out += "0\nEOF\n";
    return out;
}

// ---- reading ------------------------------------------------------------------

namespace {

struct Record {
    std::string type;
    std::vector<std::pair<int, std::string>> groups;

    const std::string* first(int code) const {
        for (const auto& [c, v] : groups) {
            if (c == code) return &v;
        }
        return nullptr;
    }
    std::string str(int code, const std::string& fallback = {}) const {
        const std::string* v = first(code);
        return v ? *v : fallback;
    }
    double num(int code, double fallback = 0) const {
        const std::string* v = first(code);
        return v ? std::strtod(v->c_str(), nullptr) : fallback;
    }
    long integer(int code, long fallback = 0) const {
        const std::string* v = first(code);
        return v ? std::strtol(v->c_str(), nullptr, 10) : fallback;
    }
    Vec2 point(int code) const { return {num(code), num(code + 10)}; }
    bool has(int code) const { return first(code) != nullptr; }
    // the wetwaresuite XDATA strings, joined
    std::string appData() const {
        std::string data;
        bool mine = false;
        for (const auto& [c, v] : groups) {
            if (c == 1001) mine = v == kApp;
            else if (mine && c == 1000) data += v;
        }
        return data;
    }
};

std::string trimRight(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

std::string trimAll(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<Record> readRecords(const std::string& text) {
    std::vector<Record> out;
    std::istringstream in(text);
    std::string codeLine, value;
    bool any = false;
    while (std::getline(in, codeLine)) {
        if (!std::getline(in, value)) break;
        std::string c = trimAll(codeLine);
        if (c.empty()) continue;
        char* end = nullptr;
        long code = std::strtol(c.c_str(), &end, 10);
        if (!end || *end != '\0') throw std::runtime_error("not a DXF file (expected a group code, got '" + c.substr(0, 20) + "')");
        any = true;
        value = trimRight(value);
        if (code == 0) {
            out.push_back({trimAll(value), {}});
        } else if (code == 999) {
            continue;
        } else if (!out.empty()) {
            // numeric groups may carry padding; strings keep theirs
            const bool numeric = (code >= 10 && code < 60) || (code >= 60 && code < 100) || (code >= 140 && code < 290) ||
                                 (code >= 370 && code < 390) || (code >= 400 && code < 410) || (code >= 420 && code < 430) ||
                                 (code >= 1010 && code < 1072);
            out.back().groups.push_back({static_cast<int>(code), numeric || code == 2 || code == 8 || code == 9 ? trimAll(value) : value});
        }
    }
    if (!any) throw std::runtime_error("not a DXF file (empty)");
    return out;
}

// Points of a polyline segment with a bulge (arcs flattened every 10 degrees or less).
void bulgePoints(std::vector<Vec2>& out, Vec2 p, Vec2 q, double bulge) {
    if (std::abs(bulge) < 1e-9 || distance(p, q) < 1e-12) {
        out.push_back(q);
        return;
    }
    const double theta = 4 * std::atan(bulge);
    const double chord = distance(p, q);
    Vec2 u = normalized(q - p);
    const double dc = chord / 2 / std::tan(theta / 2);
    Vec2 c = (p + q) / 2 + perp(u) * dc;
    const double r = distance(c, p), a = std::atan2(p.y - c.y, p.x - c.x);
    const int n = std::max(2, static_cast<int>(std::ceil(std::abs(degrees(theta)) / 10)));
    for (int i = 1; i < n; ++i) {
        double t = a + theta * i / n;
        out.push_back(c + Vec2{std::cos(t), std::sin(t)} * r);
    }
    out.push_back(q);
}

std::map<std::string, std::string> parseDef(const std::string& data) {
    std::map<std::string, std::string> kv;
    std::size_t pos = 0;
    while (pos <= data.size()) {
        auto end = data.find(';', pos);
        if (end == std::string::npos) end = data.size();
        std::string item = data.substr(pos, end - pos);
        auto eq = item.find('=');
        if (eq != std::string::npos) kv[item.substr(0, eq)] = item.substr(eq + 1);
        else kv[item] = "";
        pos = end + 1;
    }
    return kv;
}

std::vector<double> numbers(const std::string& s) {
    std::vector<double> out;
    std::string item;
    for (char ch : s + ",") {
        if (ch == ',') {
            if (!item.empty()) out.push_back(std::strtod(item.c_str(), nullptr));
            item.clear();
        } else {
            item += ch;
        }
    }
    return out;
}

std::vector<Vec2> pointList(const std::string& s) {
    std::vector<Vec2> pts;
    auto v = numbers(s);
    for (std::size_t i = 0; i + 1 < v.size(); i += 2) pts.push_back({v[i], v[i + 1]});
    return pts;
}

Color trueColor(long rgb) {
    return {static_cast<std::uint8_t>((rgb >> 16) & 0xFF), static_cast<std::uint8_t>((rgb >> 8) & 0xFF), static_cast<std::uint8_t>(rgb & 0xFF), 255};
}

std::optional<Color> entityColor(const Record& r) {
    if (r.has(420)) return trueColor(r.integer(420));
    long aci = r.integer(62, 256);
    if (aci <= 0 || aci >= 256) return std::nullopt;
    return aciColor(static_cast<int>(aci));
}

// Strips MTEXT formatting codes, keeping plain text on one line.
std::string plainMtext(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (ch == '{' || ch == '}') continue;
        if (ch == '\\' && i + 1 < s.size()) {
            char nx = s[i + 1];
            if (nx == 'P' || nx == 'n' || nx == 'X') {
                out += ' ';
                ++i;
            } else if (nx == '~') {
                out += ' ';
                ++i;
            } else if (nx == '\\' || nx == '{' || nx == '}') {
                out += nx;
                ++i;
            } else if (nx == 'U' || nx == 'u') {
                out += ch;  // \U+XXXX is decoded later
            } else if (std::string("fFHhCcTQWAapS").find(nx) != std::string::npos) {
                auto end = s.find(';', i);
                if (end == std::string::npos) break;
                i = end;
            } else if (std::string("LlOoKk").find(nx) != std::string::npos) {
                ++i;
            } else {
                out += ch;
            }
            continue;
        }
        out += ch;
    }
    return out;
}

}  // namespace

std::shared_ptr<Drawing> fromDxf(const std::string& text, std::string* notes, bool* hadSheetInfo) {
    if (text.rfind("AutoCAD Binary DXF", 0) == 0) throw std::runtime_error("binary DXF isn't supported; save it as ASCII DXF");
    std::vector<Record> records = readRecords(text);
    auto doc = std::make_shared<Drawing>();
    std::map<std::string, int> skipped;
    std::map<std::string, std::string> idMap;  // ids in the file's definitions -> new ids
    bool meta = false;
    std::string section;
    int insunits = -1;
    long nextIdHint = 0;

    auto ensure = [&](const std::string& name) -> std::string {
        std::string n = name.empty() ? "0" : name;
        return doc->ensureLayer(n).name;
    };

    for (std::size_t i = 0; i < records.size(); ++i) {
        const Record& r = records[i];
        if (r.type == "SECTION") {
            section = upper(r.str(2));
            if (section == "HEADER") {
                for (std::size_t j = 0; j < r.groups.size(); ++j) {
                    if (r.groups[j].first == 9 && r.groups[j].second == "$INSUNITS" && j + 1 < r.groups.size()) {
                        insunits = static_cast<int>(std::strtol(r.groups[j + 1].second.c_str(), nullptr, 10));
                    }
                }
            }
            continue;
        }
        if (r.type == "ENDSEC") {
            section.clear();
            continue;
        }

        if (section == "TABLES" && r.type == "LAYER") {
            const std::string name = r.str(2);
            if (name.empty() || name == kSheetLayer) continue;
            Layer& l = doc->ensureLayer(name);
            long aci = r.integer(62, 7);
            l.visible = aci >= 0 && (r.integer(70) & 1) == 0;
            l.locked = (r.integer(70) & 4) != 0;
            if (r.has(420)) l.color = trueColor(r.integer(420));
            else l.color = std::abs(aci) == 7 ? Color{0, 0, 0, 255} : aciColor(static_cast<int>(std::abs(aci)));
            l.linetype = parseLineType(r.str(6, "Continuous")).value_or(LineType::Continuous);
            long lw = r.integer(370, -3);
            l.weight = lw > 0 ? static_cast<double>(lw) / 100.0 : 0.25;
            continue;
        }

        if (section == "OBJECTS" && r.type == "XRECORD") {
            std::vector<std::string> lines;
            for (const auto& [c, v] : r.groups) {
                if (c == 1) lines.push_back(v);
            }
            if (lines.empty() || lines[0].rfind(kMeta, 0) != 0) continue;
            meta = true;
            for (const auto& line : lines) {
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = line.substr(0, eq), value = unescapeField(line.substr(eq + 1));
                if (key == "units") {
                    if (auto u = parseUnit(value)) doc->units = *u;
                } else if (key == "scale") {
                    double n = 1, d = 1;
                    if (parseScale(value, n, d)) {
                        doc->scaleNum = n;
                        doc->scaleDen = d;
                    }
                } else if (key == "paper") {
                    if (findPaper(value)) doc->paper = findPaper(value)->name;
                } else if (key == "orientation") {
                    doc->landscape = value == "landscape";
                } else if (key == "origin") {
                    auto v = numbers(value);
                    if (v.size() == 2) doc->origin = {v[0], v[1]};
                } else if (key == "titleblock") {
                    doc->title.show = value == "1";
                } else if (key == "title") {
                    doc->title.title = value;
                } else if (key == "author") {
                    doc->title.author = value;
                } else if (key == "date") {
                    doc->title.date = value;
                } else if (key == "sheet") {
                    doc->title.sheet = value;
                } else if (key == "scaletext") {
                    doc->title.scale = value;
                } else if (key == "dims") {
                    std::vector<std::string> parts;
                    std::string item;
                    for (char ch : value + ",") {
                        if (ch == ',') {
                            parts.push_back(item);
                            item.clear();
                        } else {
                            item += ch;
                        }
                    }
                    if (parts.size() >= 8) {
                        DimStyle& ds = doc->dims;
                        ds.textHeight = std::strtod(parts[0].c_str(), nullptr);
                        ds.arrowSize = std::strtod(parts[1].c_str(), nullptr);
                        ds.extOffset = std::strtod(parts[2].c_str(), nullptr);
                        ds.extBeyond = std::strtod(parts[3].c_str(), nullptr);
                        ds.textGap = std::strtod(parts[4].c_str(), nullptr);
                        ds.arrows = parts[5];
                        ds.precision = std::atoi(parts[6].c_str());
                        ds.suffix = parts[7] == "1";
                    }
                } else if (key == "grid") {
                    auto v = numbers(value);
                    if (v.size() == 3) doc->grid = {v[0], v[1], v[2] != 0};
                } else if (key == "current") {
                    doc->current = value;
                } else if (key == "nextid") {
                    nextIdHint = std::atol(value.c_str());
                }
            }
            continue;
        }

        if (section != "ENTITIES") continue;
        const std::string layerName = r.str(8, "0");
        if (layerName == kSheetLayer) {
            if (r.type == "POLYLINE") {
                while (i + 1 < records.size() && records[i + 1].type != "SEQEND" && records[i + 1].type == "VERTEX") ++i;
            }
            continue;
        }
        const std::string data = r.appData();
        if (data == "part") continue;

        Entity e;
        e.layer = ensure(layerName);
        e.color = entityColor(r);

        if (data.rfind("def;", 0) == 0) {
            auto kv = parseDef(data);
            auto type = parseEntityType(kv["type"]);
            if (!type) continue;
            e.type = *type;
            e.color.reset();
            e.pts = pointList(kv["pts"]);
            e.closed = kv["closed"] == "1";
            e.thickness = std::strtod(kv["thickness"].c_str(), nullptr);
            if (!kv["fill"].empty()) e.fill = kv["fill"];
            e.host = kv["host"];
            e.at = std::strtod(kv["at"].c_str(), nullptr);
            e.width = std::strtod(kv["width"].c_str(), nullptr);
            e.hingeEnd = kv["hinge"] == "end";
            e.swingRight = kv["side"] == "right";
            e.name = unescapeField(kv["name"]);
            auto cc = numbers(kv["c"]);
            if (cc.size() == 2) e.c = {cc[0], cc[1]};
            if (kv.count("area")) e.showArea = kv["area"] == "1";
            if (kv.count("outline")) e.outline = kv["outline"] == "1";
            const std::string oldId = kv["id"];
            const std::string newId = doc->add(e);
            if (!oldId.empty()) idMap[oldId] = newId;
            continue;
        }

        const std::string& type = r.type;
        if (type == "LINE") {
            e.type = EntityType::Line;
            e.pts = {r.point(10), r.point(11)};
        } else if (type == "LWPOLYLINE") {
            e.type = EntityType::Polyline;
            e.closed = (r.integer(70) & 1) != 0;
            std::vector<Vec2> verts;
            std::vector<double> bulges;
            for (const auto& [c, v] : r.groups) {
                if (c == 10) {
                    verts.push_back({std::strtod(v.c_str(), nullptr), 0});
                    bulges.push_back(0);
                } else if (c == 20 && !verts.empty()) {
                    verts.back().y = std::strtod(v.c_str(), nullptr);
                } else if (c == 42 && !bulges.empty()) {
                    bulges.back() = std::strtod(v.c_str(), nullptr);
                }
            }
            if (verts.size() < 2) continue;
            e.pts = {verts[0]};
            const std::size_t n = verts.size();
            for (std::size_t j = 0; j + 1 < n + (e.closed ? 1 : 0); ++j) {
                if (j + 1 == n && e.closed) {
                    if (std::abs(bulges[j]) > 1e-9) {
                        bulgePoints(e.pts, verts[j], verts[0], bulges[j]);
                        e.pts.pop_back();  // closing point is implied
                    }
                } else {
                    bulgePoints(e.pts, verts[j], verts[j + 1], bulges[j]);
                }
            }
        } else if (type == "POLYLINE") {
            const long flags = r.integer(70);
            std::vector<Vec2> verts;
            std::vector<double> bulges;
            while (i + 1 < records.size() && records[i + 1].type == "VERTEX") {
                const Record& v = records[++i];
                if (v.integer(70) & 128) continue;  // polyface face record
                verts.push_back(v.point(10));
                bulges.push_back(v.num(42));
            }
            if (i + 1 < records.size() && records[i + 1].type == "SEQEND") ++i;
            if (flags & (16 | 64)) {
                ++skipped["POLYLINE (mesh)"];
                continue;
            }
            if (verts.size() < 2) continue;
            e.type = EntityType::Polyline;
            e.closed = (flags & 1) != 0;
            e.pts = {verts[0]};
            for (std::size_t j = 0; j + 1 < verts.size(); ++j) bulgePoints(e.pts, verts[j], verts[j + 1], bulges[j]);
            if (e.closed && std::abs(bulges.back()) > 1e-9) {
                bulgePoints(e.pts, verts.back(), verts[0], bulges.back());
                e.pts.pop_back();
            }
        } else if (type == "CIRCLE") {
            e.type = EntityType::Circle;
            e.c = r.point(10);
            e.r = r.num(40);
        } else if (type == "ARC") {
            e.type = EntityType::Arc;
            e.c = r.point(10);
            e.r = r.num(40);
            e.a0 = normalizeAngle(r.num(50));
            e.a1 = normalizeAngle(r.num(51));
        } else if (type == "POINT") {
            e.type = EntityType::Point;
            e.c = r.point(10);
        } else if (type == "TEXT" || type == "ATTDEF") {
            if (type == "ATTDEF") {
                ++skipped[type];
                continue;
            }
            e.type = EntityType::Text;
            e.text = decodeText(r.str(1));
            e.height = r.num(40, 1);
            e.rotation = r.num(50);
            const long h72 = r.integer(72), v73 = r.integer(73);
            e.halign = h72 == 1 || h72 == 4 ? HAlign::Center : h72 == 2 ? HAlign::Right : HAlign::Left;
            e.valign = v73 == 2 || h72 == 4 ? VAlign::Middle : v73 == 3 ? VAlign::Top : VAlign::Baseline;
            e.c = (h72 || v73) && r.has(11) ? r.point(11) : r.point(10);
            if (h72 == 3 || h72 == 5) {  // aligned / fit: keep the left start
                e.halign = HAlign::Left;
                e.c = r.point(10);
            }
        } else if (type == "MTEXT") {
            e.type = EntityType::Text;
            std::string raw;
            for (const auto& [c, v] : r.groups) {
                if (c == 3) raw += v;
            }
            raw += r.str(1);
            e.text = decodeText(plainMtext(raw));
            e.height = r.num(40, 1);
            e.c = r.point(10);
            e.rotation = r.has(50) ? r.num(50) : (r.has(11) ? angleOf(r.point(11)) : 0);
            const long ap = r.integer(71, 1);
            e.halign = (ap - 1) % 3 == 0 ? HAlign::Left : (ap - 1) % 3 == 1 ? HAlign::Center : HAlign::Right;
            e.valign = ap <= 3 ? VAlign::Top : ap <= 6 ? VAlign::Middle : VAlign::Baseline;
        } else if (type == "SOLID" || type == "TRACE") {
            e.type = EntityType::Hatch;
            e.pattern = "solid";
            Vec2 p1 = r.point(10), p2 = r.point(11), p3 = r.point(12), p4 = r.has(13) ? r.point(13) : p3;
            e.pts = {p1, p2, p4, p3};
            if (distance(p3, p4) < 1e-12) e.pts = {p1, p2, p3};
        } else if (type == "HATCH") {
            e.type = EntityType::Hatch;
            const bool solid = r.integer(70) == 1;
            // walk the boundary paths in order; keep the loop with the largest area
            std::vector<Vec2> bestLoop;
            std::size_t j = 0;
            const auto& gs = r.groups;
            auto at = [&](std::size_t k2) { return std::strtod(gs[k2].second.c_str(), nullptr); };
            while (j < gs.size() && gs[j].first != 91) ++j;
            long paths = j < gs.size() ? std::strtol(gs[j].second.c_str(), nullptr, 10) : 0;
            ++j;
            for (long pth = 0; pth < paths && j < gs.size(); ++pth) {
                while (j < gs.size() && gs[j].first != 92) ++j;
                if (j >= gs.size()) break;
                long flags = std::strtol(gs[j].second.c_str(), nullptr, 10);
                ++j;
                std::vector<Vec2> loop;
                if (flags & 2) {
                    bool hasBulge = false;
                    long count = 0;
                    while (j < gs.size() && gs[j].first != 93) {
                        if (gs[j].first == 72) hasBulge = gs[j].second.find('1') != std::string::npos;
                        ++j;
                    }
                    if (j < gs.size()) count = std::strtol(gs[j++].second.c_str(), nullptr, 10);
                    std::vector<Vec2> verts;
                    std::vector<double> bulges;
                    while (j < gs.size() && static_cast<long>(verts.size()) <= count) {
                        int c = gs[j].first;
                        if (c == 10) {
                            if (static_cast<long>(verts.size()) == count) break;
                            verts.push_back({at(j), 0});
                            bulges.push_back(0);
                        } else if (c == 20 && !verts.empty()) {
                            verts.back().y = at(j);
                        } else if (c == 42 && hasBulge && !bulges.empty()) {
                            bulges.back() = at(j);
                        } else if (c == 97 || c == 92 || c == 75) {
                            break;
                        }
                        ++j;
                    }
                    if (!verts.empty()) {
                        loop.push_back(verts[0]);
                        for (std::size_t q = 0; q < verts.size(); ++q) bulgePoints(loop, verts[q], verts[(q + 1) % verts.size()], bulges[q]);
                        loop.pop_back();
                    }
                } else {
                    while (j < gs.size() && gs[j].first != 93) ++j;
                    long edges = j < gs.size() ? std::strtol(gs[j++].second.c_str(), nullptr, 10) : 0;
                    for (long ed = 0; ed < edges && j < gs.size(); ++ed) {
                        while (j < gs.size() && gs[j].first != 72) ++j;
                        if (j >= gs.size()) break;
                        long et = std::strtol(gs[j++].second.c_str(), nullptr, 10);
                        std::map<int, double> v;
                        while (j < gs.size() && gs[j].first != 72 && gs[j].first != 97) {
                            if (!v.count(gs[j].first)) v[gs[j].first] = at(j);
                            ++j;
                        }
                        if (et == 1) {
                            if (loop.empty()) loop.push_back({v[10], v[20]});
                            loop.push_back({v[11], v[21]});
                        } else if (et == 2) {
                            Vec2 c{v[10], v[20]};
                            double rad = v[40], s0 = v[50], s1 = v[51];
                            bool ccw = v.count(73) ? v[73] != 0 : true;
                            double sweep = normalizeAngle(s1 - s0);
                            if (sweep < 1e-9) sweep = 360;
                            int n = std::max(2, static_cast<int>(std::ceil(sweep / 10)));
                            for (int q = 0; q <= n; ++q) {
                                double a = s0 + sweep * q / n;
                                Vec2 p = c + direction(ccw ? a : -a) * rad;
                                if (loop.empty() || distance(loop.back(), p) > 1e-9) loop.push_back(p);
                            }
                        } else if (et == 3 || et == 4) {
                            if (v.count(10)) loop.push_back({v[10], v[20]});
                        }
                    }
                    if (loop.size() > 2 && distance(loop.front(), loop.back()) < 1e-9) loop.pop_back();
                }
                if (std::abs(polygonArea(loop)) > std::abs(polygonArea(bestLoop))) bestLoop = loop;
            }
            if (bestLoop.size() < 3) {
                ++skipped["HATCH (no usable boundary)"];
                continue;
            }
            e.pts = bestLoop;
            e.closed = true;
            if (solid) {
                e.pattern = "solid";
            } else {
                // pattern lines: angle of the first, spacing from its offset
                std::vector<std::pair<double, Vec2>> lines;
                for (std::size_t q = 0; q < gs.size(); ++q) {
                    if (gs[q].first != 53) continue;
                    double a = at(q);
                    Vec2 off;
                    for (std::size_t z = q + 1; z < gs.size() && gs[z].first != 53; ++z) {
                        if (gs[z].first == 45) off.x = at(z);
                        if (gs[z].first == 46) off.y = at(z);
                    }
                    lines.push_back({a, off});
                }
                e.pattern = lines.size() >= 2 ? "cross" : "lines";
                double ang = lines.empty() ? r.num(52, 45) : lines[0].first;
                double sp = lines.empty() ? r.num(41, 1) : std::abs(dot(lines[0].second, perp(direction(ang))));
                if (!(sp > 0)) sp = r.num(41, 1);
                e.angle = std::fmod(normalizeAngle(ang), 180.0);
                e.spacing = sp;  // drawing units for now, converted to paper mm once the scale is known
                e.thickness = -1;  // marks the spacing as still in drawing units
            }
        } else if (type == "DIMENSION") {
            e.type = EntityType::Dimension;
            const long t = r.integer(70) & 7;
            std::string over = decodeText(r.str(1));
            e.text = over == "<>" ? "" : over;
            if (t == 0 || t == 1) {
                const double rot = r.num(50);
                e.dim = t == 1 ? DimKind::Aligned : std::abs(std::fmod(normalizeAngle(rot), 180.0)) < 1e-6 ? DimKind::Horizontal
                        : std::abs(std::fmod(normalizeAngle(rot), 180.0) - 90) < 1e-6                          ? DimKind::Vertical
                                                                                                                : DimKind::Aligned;
                e.pts = {r.point(13), r.point(14), r.point(10)};
            } else if (t == 4) {
                e.dim = DimKind::Radius;
                e.c = r.point(10);
                Vec2 on = r.point(15);
                e.r = distance(e.c, on);
                e.rotation = normalizeAngle(angleOf(on - e.c));
            } else if (t == 3) {
                e.dim = DimKind::Diameter;
                Vec2 a = r.point(10), on = r.point(15);
                e.c = (a + on) / 2;
                e.r = distance(a, on) / 2;
                e.rotation = normalizeAngle(angleOf(on - e.c));
            } else if (t == 5) {
                e.dim = DimKind::Angular;
                e.pts = {r.point(15), r.point(13), r.point(14), r.point(10)};
            } else if (t == 2) {
                // two lines: 13-14 and 10-15, arc through 16
                Vec2 a1 = r.point(13), a2 = r.point(14), b1 = r.point(10), b2 = r.point(15);
                Vec2 da = a2 - a1, db = b2 - b1;
                double den = cross(da, db);
                if (std::abs(den) < 1e-12) {
                    ++skipped["DIMENSION (parallel angular)"];
                    continue;
                }
                double s = cross(b1 - a1, db) / den;
                Vec2 v = a1 + da * s;
                Vec2 p1 = distance(v, a2) > distance(v, a1) ? a2 : a1, p2 = distance(v, b2) > distance(v, b1) ? b2 : b1;
                e.dim = DimKind::Angular;
                e.pts = {v, p1, p2, r.point(16)};
            } else {
                ++skipped["DIMENSION (ordinate)"];
                continue;
            }
        } else {
            ++skipped[type];
            continue;
        }
        doc->add(e);
    }

    // hosts written with the file's ids
    for (auto& e : doc->entities) {
        if (e.type == EntityType::Door || e.type == EntityType::Window) {
            auto it = idMap.find(e.host);
            e.host = it != idMap.end() ? it->second : "";
        }
    }
    doc->entities.erase(std::remove_if(doc->entities.begin(), doc->entities.end(),
                                       [](const Entity& e) {
                                           return (e.type == EntityType::Door || e.type == EntityType::Window) && e.host.empty();
                                       }),
                        doc->entities.end());

    if (!meta) {
        switch (insunits) {
            case 1: doc->units = Unit::In; break;
            case 2: doc->units = Unit::Ft; break;
            case 5: doc->units = Unit::Cm; break;
            case 6: doc->units = Unit::M; break;
            default: doc->units = Unit::Mm; break;
        }
    }
    // hatch spacing from the file is in drawing units; ours is paper mm
    for (auto& e : doc->entities) {
        if (e.type == EntityType::Hatch && e.thickness < 0) {
            e.spacing = std::max(0.2, e.spacing * doc->k());
            e.thickness = 0;
        }
    }
    if (!doc->layer(doc->current)) doc->current = "0";
    if (nextIdHint > doc->nextId) doc->nextId = static_cast<int>(nextIdHint);
    if (hadSheetInfo) *hadSheetInfo = meta;
    if (notes) {
        std::string s;
        for (const auto& [type, count] : skipped) s += (s.empty() ? "" : ", ") + std::to_string(count) + " " + type;
        *notes = s.empty() ? "" : "skipped " + s;
    }
    return doc;
}

std::shared_ptr<Drawing> openDxf(const std::filesystem::path& path, std::string* notes, bool* hadSheetInfo) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    auto doc = fromDxf(ss.str(), notes, hadSheetInfo);
    doc->path = path.string();
    return doc;
}

}  // namespace wetwaresuite::blueprint
