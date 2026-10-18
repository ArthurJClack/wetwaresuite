#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/type/type.hpp"
#include "check.hpp"

// A private stb_truetype to check exported fonts with an independent reader.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

using namespace wetwaresuite;
using namespace wetwaresuite::typeface;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_type_" + std::to_string(stamp) + "_" + std::to_string(rd() % 100000));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

bool sameContour(const Contour& a, const Contour& b) {
    if (a.points.size() != b.points.size()) return false;
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        if (a.points[i].type != b.points[i].type || !near(a.points[i].x, b.points[i].x, 0.001) || !near(a.points[i].y, b.points[i].y, 0.001))
            return false;
    }
    return true;
}

double polyArea(const std::vector<Contour>& cs) {
    double a = 0;
    for (const auto& c : cs) a += signedArea(c);
    return a;
}

// A small test font: A (triangle with a counter), V, H, O (ring of cubics), acute, Aacute (components).
Font sampleFont() {
    Font f = newFont("Testa", "Regular", 1000, 800, -200, 500, 700);
    auto add = [&](const std::string& name, std::uint32_t cp, double adv, std::vector<Contour> cs) {
        Glyph g;
        g.name = name;
        if (cp) g.unicodes = {cp};
        g.advance = adv;
        g.contours = std::move(cs);
        fixDirections(g.contours);
        f.glyphs.push_back(g);
    };
    add("A", 'A', 640, parsePathData("M20 0L280 700L360 700L620 0L520 0L320 560L120 0Z"));
    add("V", 'V', 640, parsePathData("M20 700L280 0L360 0L620 700L520 700L320 140L120 700Z"));
    add("H", 'H', 640, {rectContour(60, 0, 100, 700), rectContour(480, 0, 100, 700), rectContour(160, 300, 320, 100)});
    {
        Contour outer = ellipseContour(350, 350, 300, 360), inner = ellipseContour(350, 350, 200, 260);
        add("O", 'O', 700, {outer, inner});
    }
    add("acute", 0xB4, 300, parsePathData("M100 760L200 760L260 900L160 900Z"));
    Glyph aa;
    aa.name = "Aacute";
    aa.unicodes = {0xC1};
    aa.advance = 640;
    aa.components = {Component{"A"}, Component{"acute", 1, 0, 0, 1, -30, 0}};
    f.glyphs.push_back(aa);
    f.kerning[{"A", "V"}] = -80;
    f.kerning[{"V", "A"}] = -70;
    return f;
}

void testNames() {
    CHECK(macGlyphNames().size() == 258);
    CHECK(macGlyphNames()[36] == "A" && macGlyphNames()[257] == "dcroat");
    CHECK(glyphNameFor('A') == "A" && glyphNameFor('0') == "zero" && glyphNameFor('!') == "exclam" && glyphNameFor(' ') == "space");
    CHECK(glyphNameFor(0xE9) == "uni00E9" && glyphNameFor(0x1F600) == "u1F600");
    CHECK(unicodeForName("zero") == std::optional<std::uint32_t>('0'));
    CHECK(unicodeForName("uni00E9") == std::optional<std::uint32_t>(0xE9));
    CHECK(!unicodeForName("Aacute"));
    CHECK(parseUnicode("U+0041") == std::optional<std::uint32_t>(0x41));
    CHECK(parseUnicode("é") == std::optional<std::uint32_t>(0xE9));
    CHECK(parseUnicode("0x263A") == std::optional<std::uint32_t>(0x263A));
    CHECK(!parseUnicode("hello"));
    CHECK(utf8Encode(0x20AC) == "\xE2\x82\xAC" && utf8Decode("a\xE2\x82\xAC").size() == 2);
    CHECK(validGlyphName("A.alt") && !validGlyphName("a b") && !validGlyphName(""));
}

void testGeometry() {
    auto cs = parsePathData("M0 0L100 0L100 100Z m 10 10 h 20 v 20 h -20 z");
    CHECK(cs.size() == 2 && cs[0].points.size() == 3 && cs[1].points.size() == 4);
    CHECK(near(cs[1].points[2].x, 30, 1e-9) && near(cs[1].points[2].y, 30, 1e-9));
    auto curves = parsePathData("M0 0Q50 100 100 0C100 -50 0 -50 0 0Z");
    CHECK(curves.size() == 1);
    std::size_t quads = 0, cubics = 0;
    for (const auto& p : curves[0].points) {
        quads += p.type == PointType::Quad;
        cubics += p.type == PointType::Cubic;
    }
    CHECK(quads == 1 && cubics == 2);
    // arcs become cubics; a full circle of radius 50 has area ~ pi r^2
    auto circle = parsePathData("M0 50A50 50 0 1 1 100 50A50 50 0 1 1 0 50Z");
    CHECK(circle.size() == 1 && near(std::abs(polyArea(circle)), 3.14159 * 2500, 40));
    std::string err;
    parsePathData("M0 0L10", &err);
    CHECK(!err.empty());
    // exact curve bounds
    Box b = bounds({ellipseContour(0, 0, 100, 50)});
    CHECK(near(b.x0, -100, 0.01) && near(b.y1, 50, 0.01));
    // directions: outer clockwise, hole counter-clockwise
    std::vector<Contour> ring = {ellipseContour(0, 0, 100, 100), ellipseContour(0, 0, 50, 50)};
    for (auto& c : ring) reverse(c);
    fixDirections(ring);
    CHECK(signedArea(ring[0]) < 0 && signedArea(ring[1]) > 0);
    CHECK(fixDirections(ring) == 0);
    // cubic -> quadratic stays close
    Contour qc = toQuadratic(ellipseContour(0, 0, 300, 300), 0.5);
    bool allQuad = true;
    for (const auto& p : qc.points) allQuad &= p.type != PointType::Cubic;
    CHECK(allQuad);
    for (const auto& p : flatten(qc, 0.1)) CHECK(near(std::hypot(p.x, p.y), 300, 1.0));
    // path data round trip
    const std::string d = pathData(curves);
    auto back = parsePathData(d);
    CHECK(back.size() == 1 && back[0] == curves[0]);
    // affine
    Vec v = (Affine::translate(10, 0) * Affine::scale(2, 2)).apply({1, 1});
    CHECK(near(v.x, 12, 1e-9) && near(v.y, 2, 1e-9));
    Vec s = Affine::skewX(45).apply({0, 100});
    CHECK(near(s.x, 100, 1e-9));
}

void testOutlineOps() {
    // union of two overlapping squares: one contour, area 17500
    std::vector<Contour> two = {rectContour(0, 0, 100, 100), rectContour(50, 50, 100, 100)};
    auto u = removeOverlaps(two, 0.5);
    CHECK(u.size() == 1);
    CHECK(near(std::abs(polyArea(u)), 17500, 50));
    // a square fully inside another disappears; a separate one stays untouched
    auto inside = removeOverlaps({rectContour(0, 0, 100, 100), rectContour(20, 20, 10, 10), rectContour(300, 0, 10, 10)}, 0.5);
    CHECK(inside.size() == 2);
    // a ring keeps its hole
    auto ringU = removeOverlaps({ellipseContour(0, 0, 100, 100), [] {
                                     Contour c = ellipseContour(0, 0, 50, 50);
                                     reverse(c);
                                     return c;
                                 }()},
                                0.5);
    CHECK(ringU.size() == 2);
    // a cross of two bars: 12 corners after union
    auto cross = removeOverlaps({rectContour(0, 40, 100, 20), rectContour(40, 0, 20, 100)}, 0.5);
    CHECK(cross.size() == 1 && cross[0].points.size() == 12 && near(std::abs(polyArea(cross)), 3600, 10));
    // collinear edges (an E made of rectangles)
    auto e = removeOverlaps({rectContour(0, 0, 30, 100), rectContour(0, 0, 80, 20), rectContour(0, 40, 70, 20), rectContour(0, 80, 80, 20)}, 0.5);
    CHECK(e.size() == 1 && near(std::abs(polyArea(e)), 3000 + 50 * 20 * 2 + 40 * 20, 20));
    // embolden a square by 10: 120 x 120
    auto bold = embolden({rectContour(0, 0, 100, 100)}, 10, 10);
    Box bb = bounds(bold);
    CHECK(near(bb.x0, -10, 1e-6) && near(bb.x1, 110, 1e-6) && near(bb.y1, 110, 1e-6));
    auto thin = embolden({rectContour(0, 0, 100, 100)}, -10, 0);
    Box tb = bounds(thin);
    CHECK(near(tb.x0, 10, 1e-6) && near(tb.y0, 0, 1e-6));
    // curve fitting a circle of points
    Polygon pts;
    for (int i = 0; i < 200; ++i) pts.push_back({100 * std::cos(i * 0.0314159), 100 * std::sin(i * 0.0314159)});
    Contour fit = fitCurves(pts, 0.5);
    CHECK(fit.points.size() < 40);
    for (const auto& p : flatten(fit, 0.1)) CHECK(near(std::hypot(p.x, p.y), 100, 1.5));
    // and a square of points keeps its corners as lines
    Polygon sq;
    for (int i = 0; i < 40; ++i) sq.push_back({i * 2.5, 0});
    for (int i = 0; i < 40; ++i) sq.push_back({100, i * 2.5});
    for (int i = 0; i < 40; ++i) sq.push_back({100 - i * 2.5, 100});
    for (int i = 0; i < 40; ++i) sq.push_back({0, 100 - i * 2.5});
    Contour fsq = fitCurves(sq, 0.5);
    CHECK(fsq.points.size() == 4);
}

void testTtfWithStb(const fs::path& dir) {
    Font f = sampleFont();
    std::vector<std::string> warnings;
    TtfOptions o;
    o.warnings = &warnings;
    const std::string ttf = toTtf(f, o);
    CHECK(warnings.empty());
    stbtt_fontinfo info;
    const auto* data = reinterpret_cast<const unsigned char*>(ttf.data());
    CHECK(stbtt_InitFont(&info, data, 0) == 1);
    CHECK(info.numGlyphs == static_cast<int>(f.glyphs.size()));
    const int gA = stbtt_FindGlyphIndex(&info, 'A'), gO = stbtt_FindGlyphIndex(&info, 'O'), gAa = stbtt_FindGlyphIndex(&info, 0xC1);
    CHECK(gA > 0 && gO > 0 && gAa > 0 && stbtt_FindGlyphIndex(&info, 'Z') == 0);
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, gA, &adv, &lsb);
    CHECK(adv == 640 && lsb == 20);
    int x0, y0, x1, y1;
    CHECK(stbtt_GetGlyphBox(&info, gA, &x0, &y0, &x1, &y1) && x0 == 20 && y0 == 0 && x1 == 620 && y1 == 700);
    CHECK(stbtt_GetGlyphBox(&info, gAa, &x0, &y0, &x1, &y1) && y1 == 900);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    CHECK(asc == 800 && desc == -200);
    CHECK(stbtt_GetCodepointKernAdvance(&info, 'A', 'V') == -80);
    CHECK(stbtt_GetCodepointKernAdvance(&info, 'V', 'A') == -70);
    CHECK(stbtt_GetCodepointKernAdvance(&info, 'A', 'A') == 0);
    // the O is a ring: rasterized, the middle is empty and the stroke is ink
    const float scale = stbtt_ScaleForPixelHeight(&info, 100);
    int w, h, xo, yo;
    unsigned char* bmp = stbtt_GetCodepointBitmap(&info, scale, scale, 'O', &w, &h, &xo, &yo);
    CHECK(bmp && w > 50 && h > 50);
    if (bmp) {
        CHECK(bmp[(h / 2) * w + w / 2] == 0);
        CHECK(bmp[(h / 2) * w + 4] > 200);
        stbtt_FreeBitmap(bmp, nullptr);
    }
    // the composite has both components' outlines
    stbtt_vertex* verts = nullptr;
    const int nv = stbtt_GetGlyphShape(&info, gAa, &verts);
    CHECK(nv >= 7 + 4 + 2);
    stbtt_FreeShape(&info, verts);
    // kern-table-only kerning also works for stb
    TtfOptions ko;
    ko.kerning = "kern";
    const std::string kttf = toTtf(f, ko);
    stbtt_fontinfo kinfo;
    CHECK(stbtt_InitFont(&kinfo, reinterpret_cast<const unsigned char*>(kttf.data()), 0) == 1);
    CHECK(stbtt_GetCodepointKernAdvance(&kinfo, 'A', 'V') == -80);
    // checksum adjustment: the whole file sums to 0xB1B0AFBA
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < ttf.size(); i += 4) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | (i + k < ttf.size() ? static_cast<unsigned char>(ttf[i + k]) : 0u);
        sum += v;
    }
    CHECK(sum == 0xB1B0AFBAu);

    // reopen with our own reader
    Font back = fromSfnt(ttf);
    CHECK(back.glyphs.size() == f.glyphs.size());
    CHECK(back.info.family == "Testa" && back.info.style == "Regular" && back.metrics.unitsPerEm == 1000);
    CHECK(near(back.metrics.xHeight, 500, 0.5) && near(back.metrics.capHeight, 700, 0.5));
    const Glyph* a = back.glyph("A");
    CHECK(a && a->advance == 640 && a->contours.size() == 1 && a->unicodes == std::vector<std::uint32_t>{'A'});
    const Glyph* comp = back.glyph("Aacute");
    CHECK(comp && comp->components.size() == 2 && comp->components[1].base == "acute" && comp->components[1].dx == -30);
    CHECK(back.kern("A", "V") == -80 && back.kerning.size() == 2);
    const Glyph* oo = back.glyph("O");
    CHECK(oo && oo->contours.size() == 2);
    Box bo = glyphBounds(back, *oo);
    CHECK(near(bo.x0, 50, 1.5) && near(bo.y1, 710, 1.5));

    // WOFF round trip
    const std::string woff = toWoff(ttf);
    CHECK(woff.compare(0, 4, "wOFF") == 0 && woff.size() < ttf.size());
    const std::string sfnt = woffToSfnt(woff);
    stbtt_fontinfo winfo;
    CHECK(stbtt_InitFont(&winfo, reinterpret_cast<const unsigned char*>(sfnt.data()), 0) == 1);
    Font wf = fromSfnt(woff);
    CHECK(wf.glyphs.size() == f.glyphs.size() && wf.kern("V", "A") == -70);

    // a font with a non-BMP character uses cmap format 12
    Font emoji = f;
    Glyph smile;
    smile.name = "u1F600";
    smile.unicodes = {0x1F600};
    smile.advance = 800;
    smile.contours = {ellipseContour(400, 300, 300, 300)};
    emoji.glyphs.push_back(smile);
    const std::string ettf = toTtf(emoji);
    stbtt_fontinfo einfo;
    CHECK(stbtt_InitFont(&einfo, reinterpret_cast<const unsigned char*>(ettf.data()), 0) == 1);
    CHECK(stbtt_FindGlyphIndex(&einfo, 0x1F600) == static_cast<int>(emoji.glyphs.size()) - 1);
    CHECK(fromSfnt(ettf).glyph("u1F600") != nullptr);

    // SVG font
    const std::string svg = toSvgFont(f);
    CHECK(svg.find("<font-face") != std::string::npos && svg.find("glyph-name=\"Aacute\"") != std::string::npos &&
          svg.find("<hkern g1=\"A\" g2=\"V\" k=\"80\"") != std::string::npos);
    (void)dir;
}

// A hand-built OpenType CFF font with one square glyph, read back through stb_truetype.
void testCff() {
    auto i5 = [](std::string& s, int v) {
        s += static_cast<char>(29);
        for (int k = 3; k >= 0; --k) s += static_cast<char>((v >> (8 * k)) & 0xFF);
    };
    auto num = [](std::string& s, int v) {
        s += static_cast<char>(28);
        s += static_cast<char>((v >> 8) & 0xFF);
        s += static_cast<char>(v & 0xFF);
    };
    auto index = [](const std::vector<std::string>& items) {
        std::string s;
        s += static_cast<char>(0);
        s += static_cast<char>(items.size());
        if (items.empty()) return s;
        s += static_cast<char>(1);
        int off = 1;
        s += static_cast<char>(off);
        for (const auto& it : items) {
            off += static_cast<int>(it.size());
            s += static_cast<char>(off);
        }
        for (const auto& it : items) s += it;
        return s;
    };
    std::string notdef(1, static_cast<char>(14));
    std::string square;
    num(square, 100);
    num(square, 100);
    square += static_cast<char>(21);  // rmoveto
    num(square, 300);
    square += static_cast<char>(6);  // hlineto
    num(square, 400);
    square += static_cast<char>(7);  // vlineto
    num(square, -300);
    square += static_cast<char>(6);
    square += static_cast<char>(14);
    const std::string charstrings = index({notdef, square});
    const std::string header = std::string("\x01\x00\x04\x01", 4);
    const std::string names = index({"T"});
    // top dict: CharStrings offset, Private size + offset (sizes fixed by the 5-byte ints)
    const int topLen = 17;
    const int before = static_cast<int>(header.size() + names.size()) + 2 + 1 + 2 + topLen + 2 + 2;
    std::string top;
    i5(top, before);
    top += static_cast<char>(17);
    i5(top, 0);
    i5(top, before + static_cast<int>(charstrings.size()));
    top += static_cast<char>(18);
    CHECK(static_cast<int>(top.size()) == topLen);
    std::string cff = header + names + index({top}) + index({}) + index({}) + charstrings;
    CHECK(static_cast<int>(cff.size()) == before + static_cast<int>(charstrings.size()));

    Font f = newFont("Cffy", "Regular", 1000, 800, -200, 500, 700);
    f.glyphs.resize(1);  // .notdef only
    Glyph a;
    a.name = "A";
    a.unicodes = {'A'};
    a.advance = 500;
    a.contours = {rectContour(100, 100, 300, 400)};
    f.glyphs.push_back(a);
    auto tables = buildTables(f);
    tables.erase("glyf");
    tables.erase("loca");
    std::string maxp = tables["maxp"].substr(0, 6);
    maxp[0] = 0;
    maxp[1] = 0;
    maxp[2] = 0x50;
    maxp[3] = 0;
    tables["maxp"] = maxp;
    tables["CFF "] = cff;
    const std::string otf = assembleSfnt(tables, 0x4F54544F);
    std::vector<std::string> warnings;
    Font back = fromSfnt(otf, &warnings);
    const Glyph* g = back.glyph("A");
    CHECK(g && g->contours.size() == 1 && g->advance == 500);
    if (g) {
        Box b = bounds(g->contours);
        CHECK(near(b.x0, 100, 1e-9) && near(b.y0, 100, 1e-9) && near(b.x1, 400, 1e-9) && near(b.y1, 500, 1e-9));
    }
    // and it converts to a TrueType font
    stbtt_fontinfo info;
    const std::string ttf = toTtf(back);
    CHECK(stbtt_InitFont(&info, reinterpret_cast<const unsigned char*>(ttf.data()), 0) == 1);
    int x0, y0, x1, y1;
    CHECK(stbtt_GetGlyphBox(&info, stbtt_FindGlyphIndex(&info, 'A'), &x0, &y0, &x1, &y1) && x0 == 100 && y1 == 500);
}

void testFiles(const fs::path& dir) {
    Font f = sampleFont();
    f.info.license = "SIL Open Font License 1.1";
    // JSON round trip
    Font j = fromJson(toJson(f));
    CHECK(j.glyphs.size() == f.glyphs.size() && j.info.license == f.info.license && j.kerning == f.kerning);
    for (std::size_t i = 0; i < f.glyphs.size(); ++i) {
        CHECK(j.glyphs[i].name == f.glyphs[i].name && j.glyphs[i].contours.size() == f.glyphs[i].contours.size());
        CHECK(j.glyphs[i].components == f.glyphs[i].components);
    }
    CHECK(sameContour(j.glyph("O")->contours[0], f.glyph("O")->contours[0]));
    // JSON with CRLF line ends still reads
    std::string crlf;
    for (char c : toJson(f)) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    CHECK(fromJson(crlf).glyphs.size() == f.glyphs.size());
    {
        auto renamed = [](std::string t, const std::string& from, const std::string& to) {  // undo the rename in a saved file
            for (auto at = t.find(from); at != std::string::npos; at = t.find(from, at + to.size())) t.replace(at, from.size(), to);
            return t;
        };
        // fonts saved before the rename ("achclwss.typeface") still load
        CHECK(toJson(f).find("\"wetwaresuite.typeface\"") != std::string::npos);
        CHECK(fromJson(renamed(crlf, "wetwaresuite.typeface", "achclwss.typeface")).glyphs.size() == f.glyphs.size());
    }
    // UFO round trip
    saveUfo(f, dir / "Testa.ufo");
    CHECK(fs::exists(dir / "Testa.ufo" / "glyphs" / "A_.glif") && fs::exists(dir / "Testa.ufo" / "fontinfo.plist"));
    Font u = loadUfo(dir / "Testa.ufo");
    CHECK(u.glyphs.size() == f.glyphs.size() && u.glyphs[0].name == ".notdef");
    CHECK(u.info.family == "Testa" && u.info.license == f.info.license && u.metrics.xHeight == 500);
    CHECK(u.glyph("O") && sameContour(u.glyph("O")->contours[0], f.glyph("O")->contours[0]));
    CHECK(u.glyph("Aacute")->components == f.glyph("Aacute")->components);
    CHECK(u.kern("A", "V") == -80 && u.glyph("A")->unicodes == f.glyph("A")->unicodes);
    // UFO kerning groups expand
    {
        std::ofstream(dir / "Testa.ufo" / "groups.plist", std::ios::binary)
            << "<?xml version=\"1.0\"?>\r\n<plist version=\"1.0\"><dict><key>public.kern1.A</key><array><string>A</string>"
               "<string>Aacute</string></array></dict></plist>\r\n";
        std::ofstream(dir / "Testa.ufo" / "kerning.plist", std::ios::binary)
            << "<?xml version=\"1.0\"?>\r\n<plist version=\"1.0\"><dict><key>public.kern1.A</key><dict><key>V</key><integer>-50</integer>"
               "</dict><key>A</key><dict><key>V</key><integer>-90</integer></dict></dict></plist>\r\n";
        Font g = loadUfo(dir / "Testa.ufo");
        CHECK(g.kern("Aacute", "V") == -50 && g.kern("A", "V") == -90);
    }
    // SVG import: a 100x100 canvas with a square and a circle hole drawn even-odd style
    const std::string svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" viewBox=\"0 0 100 100\">"
        "<path d=\"M10 10H90V90H10Z M30 30H70V70H30Z\" fill=\"black\"/><circle cx=\"50\" cy=\"5\" r=\"3\" fill=\"none\" stroke=\"red\"/></svg>";
    double adv = 0;
    auto cs = importSvg(svg, f.metrics, "canvas", &adv);
    CHECK(cs.size() == 2 && adv == 1000);
    Box b = bounds(cs);
    CHECK(near(b.x0, 100, 1e-6) && near(b.y1, 700, 1e-6) && near(b.y0, -100, 1e-6));
    CHECK(near(polyArea(cs), -(800 * 800 - 400 * 400), 1));  // clockwise outer, hole subtracts
    auto capFit = importSvg(svg, f.metrics, "cap");
    Box cb = bounds(capFit);
    CHECK(near(cb.y0, 0, 1e-6) && near(cb.y1, 700, 1e-6) && near(cb.x0, 0, 1e-6));
    // loadFont picks the reader
    std::ofstream(dir / "f.typeface.json", std::ios::binary) << toJson(f);
    CHECK(loadFont(dir / "f.typeface.json").glyphs.size() == f.glyphs.size());
    std::ofstream(dir / "f.ttf", std::ios::binary) << toTtf(f);
    CHECK(loadFont(dir / "f.ttf").glyphs.size() == f.glyphs.size());
    CHECK(loadFont(dir / "Testa.ufo").glyphs.size() == f.glyphs.size());
    bool threw = false;
    try {
        fromSfnt("not a font at all, really");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void testRender(const fs::path& dir) {
    Font f = sampleFont();
    TextStyle st;
    st.size = 50;
    Image img = renderText(f, "AVO", st, {255, 255, 255, 255}, 10);
    CHECK(img.width() > 100 && img.height() > 50);
    int dark = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) dark += img.at(x, y).r < 128;
    }
    CHECK(dark > 500);
    // kerning narrows AV
    TextStyle nk = st;
    nk.kerning = false;
    CHECK(textAdvance(f, "AV", st) == textAdvance(f, "AV", nk) - 80);
    // the O ring's middle stays empty when filled through our rasterizer
    Image o(200, 200, {255, 255, 255, 255});
    fillContours(o, f.glyph("O")->contours, {0.2, 0, 0, -0.2, 30, 170}, {0, 0, 0, 255});
    CHECK(o.at(30 + 70, 170 - 70).r == 255 && o.at(30 + 10, 170 - 70).r < 50);
    Image v = glyphView(f, *f.glyph("O"));
    CHECK(v.width() == 512);
    Image ov = overview(f);
    CHECK(ov.width() > 500);
    ProofOptions po;
    Image proof = proofImage(f, po);
    CHECK(proof.width() == 1200 && proof.height() > 800);
    const std::string pdf = proofPdf(f, po);
    CHECK(pdf.compare(0, 5, "%PDF-") == 0 && pdf.find("/Type /Page") != std::string::npos);
    (void)dir;
}

void testTrace() {
    // draw H and O into a 2-cell sheet the way a scan would look, then trace them back
    Font f = sampleFont();
    const int cell = 300;
    Image sheet(cell * 2, cell, {255, 255, 255, 255});
    const double k = cell / 1000.0;  // ascender 800 at the top, descender -200 at the bottom
    fillContours(sheet, f.glyph("H")->contours, {k, 0, 0, -k, 0, 800 * k}, {20, 20, 20, 255});
    fillContours(sheet, f.glyph("O")->contours, {k, 0, 0, -k, cell, 800 * k}, {20, 20, 20, 255});
    GridLayout g;
    g.rows = 1;
    g.cols = 2;
    g.inset = 0.02;
    auto cells = traceGrid(sheet, {'H', 'O'}, g, f.metrics, TraceOptions{});
    CHECK(cells.size() == 2);
    if (cells.size() == 2) {
        CHECK(cells[0].contours.size() == 1 && cells[1].contours.size() == 2);
        Box h = bounds(cells[0].contours), want = glyphBounds(f, *f.glyph("H"));
        CHECK(near(h.x0, want.x0, 8) && near(h.x1, want.x1, 8) && near(h.y0, 0, 8) && near(h.y1, 700, 8));
        Box ob = bounds(cells[1].contours), wo = glyphBounds(f, *f.glyph("O"));
        CHECK(near(ob.y0, wo.y0, 8) && near(ob.y1, wo.y1, 8));
        CHECK(std::abs(polyArea(cells[1].contours) - polyArea(f.glyph("O")->contours)) < 0.05 * std::abs(polyArea(f.glyph("O")->contours)));
        CHECK(signedArea(cells[1].contours[0]) * signedArea(cells[1].contours[1]) < 0);  // one outer, one hole
    }
    // the template draws something and leaves the guides light enough to trace as nothing
    Image t = gridTemplate(f, {'A', 'B'}, 2, 200);
    CHECK(t.width() == 400 && t.height() == 200);
    GridLayout tg;
    tg.cols = 2;
    auto empty = traceGrid(t, {'A', 'B'}, tg, f.metrics, TraceOptions{});
    CHECK(empty.size() == 2 && empty[0].contours.empty() && empty[1].contours.empty());
}

void testAutoKern() {
    Font f = sampleFont();
    auto pairs = autoKern(f, {"A", "V", "H", "O"}, AutoKernOptions{});
    CHECK(pairs.count({"A", "V"}) && pairs[{"A", "V"}] < -30);
    CHECK(!pairs.count({"H", "H"}));
    CHECK(!pairs.count({"H", "O"}) || std::abs(pairs[{"H", "O"}]) < 60);
}

void testCommands(const fs::path& dir) {
    fs::create_directories(dir);
    Session s;
    registerCommands(s);
    Result r = run(s, "type.new family=Batchy units_per_em=1000");
    CHECK(r.ok && r.field("doc"));
    const std::string id = "font1";
    CHECK(run(s, "type.glyph_add doc=font1 name=I rect=100,0,120,700").ok);
    auto font = [&]() -> Font& { return s.workspace().getAs<FontDoc>(id)->font; };
    CHECK(font().glyph("I") && font().glyph("I")->unicodes == std::vector<std::uint32_t>{'I'} && font().glyph("I")->advance == 320);
    CHECK(!run(s, "type.glyph_add doc=font1 name=I rect=0,0,10,10").ok);
    CHECK(run(s, "type.draw doc=font1 glyph=O ellipse=300,350,250,360").ok);
    CHECK(run(s, "type.draw doc=font1 glyph=O ellipse=300,350,150,260 mode=cut").ok);
    CHECK(font().glyph("O")->contours.size() == 2 && signedArea(font().glyph("O")->contours[1]) > 0);
    CHECK(run(s, "type.draw doc=font1 glyph=! path=\"M100 200L160 200L170 700L90 700Z M100 0H160V60H100Z\"").ok);
    CHECK(font().glyph("exclam") && font().glyph("exclam")->unicodes[0] == '!');
    CHECK(run(s, "type.draw doc=font1 glyph=T points=0,600,500,600,500,700,0,700").ok);
    CHECK(run(s, "type.draw doc=font1 glyph=T rect=200,0,100,650").ok);
    // a crossbar whose corners sit inside the legs still adds to the ink (not a hole)
    CHECK(run(s, "type.draw doc=font1 glyph=A path=\"M0 0H120L300 540L480 0H600L360 700H240Z\"").ok);
    CHECK(run(s, "type.draw doc=font1 glyph=A rect=130,170,340,95").ok);
    CHECK(font().glyph("A")->contours.size() == 2 &&
          signedArea(font().glyph("A")->contours[0]) * signedArea(font().glyph("A")->contours[1]) > 0);
    CHECK(run(s, "type.remove_overlap doc=font1 glyphs=A").ok && font().glyph("A")->contours.size() == 2);
    r = run(s, "type.remove_overlap doc=font1 glyphs=T");
    CHECK(r.ok && font().glyph("T")->contours.size() == 1);
    r = run(s, "type.glyph_info doc=font1 glyph=T points=true");
    CHECK(r.ok && r.message.find("contour 0:") != std::string::npos);
    CHECK(run(s, "type.metrics_auto doc=font1 spacing=50").ok);
    CHECK(near(leftBearing(font(), *font().glyph("I")), 50, 1e-9) && near(rightBearing(font(), *font().glyph("I")), 50, 1e-9));
    CHECK(near(font().metrics.capHeight, 700, 1e-9));
    CHECK(run(s, "type.glyph_set doc=font1 glyph=I rsb=80").ok && near(rightBearing(font(), *font().glyph("I")), 80, 1e-9));
    CHECK(run(s, "type.glyph_copy doc=font1 from=I to=l").ok && font().glyph("l"));
    CHECK(run(s, "type.component_add doc=font1 glyph=Iacute base=I").ok);
    CHECK(run(s, "type.glyph_add doc=font1 name=acute path=\"M0 760H80L140 900H60Z\" unicode=U+00B4").ok);
    CHECK(run(s, "type.component_add doc=font1 glyph=Iacute base=acute center=true").ok);
    CHECK(font().glyph("Iacute")->components.size() == 2 && font().glyph("U+00CD") == nullptr);
    CHECK(run(s, "type.glyph_set doc=font1 glyph=Iacute unicode=U+00CD").ok && font().glyph("Í"));
    CHECK(!run(s, "type.component_add doc=font1 glyph=I base=Iacute").ok);  // loop
    CHECK(run(s, "type.kern_set doc=font1 pairs=\"T O -60; O T -60\"").ok && font().kern("T", "O") == -60);
    CHECK(run(s, "type.kern_set doc=font1 left=T right=O value=0").ok && font().kern("T", "O") == 0);
    r = run(s, "type.kern_auto doc=font1");
    CHECK(r.ok);
    CHECK(run(s, "type.kern_list doc=font1").ok);
    // undo reverts the last edit only
    const std::size_t pairs = font().kerning.size();
    CHECK(run(s, "type.kern_clear doc=font1").ok && font().kerning.empty());
    CHECK(run(s, "undo").ok && font().kerning.size() == pairs);
    // transforms
    const Box before = glyphBounds(font(), *font().glyph("I"));
    CHECK(run(s, "type.transform doc=font1 glyphs=I dx=10").ok && near(glyphBounds(font(), *font().glyph("I")).x0, before.x0 + 10, 1e-9));
    CHECK(run(s, "type.transform doc=font1 glyphs=I scale=2 keep_advance=true").ok &&
          near(glyphBounds(font(), *font().glyph("I")).y1, 1400, 1e-9));
    CHECK(run(s, "undo").ok);
    CHECK(run(s, "type.embolden doc=font1 glyphs=I amount=10").ok);
    CHECK(near(glyphBounds(font(), *font().glyph("I")).width(), before.width() + 20, 1e-6));
    CHECK(run(s, "type.oblique doc=font1 angle=10 style=Oblique").ok && font().metrics.italicAngle == -10 && font().info.style == "Oblique");
    CHECK(run(s, "undo").ok && font().metrics.italicAngle == 0);
    // composites change exactly once: kept as components when their bases change too, drawn in otherwise
    {
        auto sameBox = [](const Box& a, const Box& b) {
            return near(a.x0, b.x0, 0.01) && near(a.y0, b.y0, 0.01) && near(a.x1, b.x1, 0.01) && near(a.y1, b.y1, 0.01);
        };
        const double oy = font().metrics.xHeight / 2;
        std::vector<Contour> want = outline(font(), *font().glyph("Iacute"));
        transform(want, Affine::translate(0, oy) * Affine::skewX(10) * Affine::translate(0, -oy));
        CHECK(run(s, "type.oblique doc=font1 angle=10").ok);
        CHECK(font().glyph("Iacute")->components.size() == 2 && sameBox(glyphBounds(font(), *font().glyph("Iacute")), bounds(want)));
        CHECK(run(s, "undo").ok);
        CHECK(run(s, "type.oblique doc=font1 angle=10 glyphs=Iacute").ok);
        CHECK(font().glyph("Iacute")->components.empty() && sameBox(glyphBounds(font(), *font().glyph("Iacute")), bounds(want)));
        CHECK(run(s, "undo").ok);
        const Box b0 = glyphBounds(font(), *font().glyph("Iacute"));
        const double adv = font().glyph("Iacute")->advance;
        CHECK(run(s, "type.embolden doc=font1 amount=10").ok);
        const Box b1 = glyphBounds(font(), *font().glyph("Iacute"));
        CHECK(font().glyph("Iacute")->components.size() == 2 && b1.width() > b0.width() + 19 && near(font().glyph("Iacute")->advance, adv + 20, 1e-9));
        CHECK(run(s, "undo").ok);
        CHECK(run(s, "type.transform doc=font1 glyphs=Iacute dx=5").ok && font().glyph("Iacute")->components.size() == 2 &&
              near(glyphBounds(font(), *font().glyph("Iacute")).x0, b0.x0 + 5, 1e-9));
        CHECK(run(s, "undo").ok);
    }
    CHECK(run(s, "type.fix_directions doc=font1").ok);
    CHECK(run(s, "type.decompose doc=font1 glyphs=Iacute").ok && font().glyph("Iacute")->components.empty());
    // svg import
    {
        std::ofstream(dir / "v.svg", std::ios::binary)
            << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"60\" height=\"100\"><polygon points=\"0,90 30,10 60,90\"/></svg>";
    }
    r = run(s, "type.import_svg doc=font1 glyph=V path=" + q(dir / "v.svg") + " fit=cap preview=true");
    CHECK(r.ok && r.attachments.size() == 1 && font().glyph("V") && near(glyphBounds(font(), *font().glyph("V")).y1, 700, 1e-6));
    // info, preview, measure
    r = run(s, "type.info doc=font1 glyphs=true");
    CHECK(r.ok && r.message.find("missing ASCII") != std::string::npos);
    r = run(s, "type.preview doc=font1 text=TOIV");
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    CHECK(run(s, "type.preview doc=font1 glyph=O").attachments.size() == 1);
    CHECK(run(s, "type.preview doc=font1").attachments.size() == 1);
    r = run(s, "type.measure doc=font1 text=IOI size=10");
    CHECK(r.ok && r.field("units"));
    // files
    CHECK(run(s, "type.set_info doc=font1 license=\"OFL 1.1\" designer=Tester version=1.5").ok);
    CHECK(run(s, "type.save doc=font1 path=" + q(dir / "b.typeface.json")).ok);
    CHECK(run(s, "type.save doc=font1 path=" + q(dir / "b.ufo")).ok);
    CHECK(run(s, "type.export doc=font1 path=" + q(dir / "b.ttf")).ok);
    CHECK(run(s, "type.export doc=font1 path=" + q(dir / "b.woff")).ok);
    CHECK(run(s, "type.export doc=font1 path=" + q(dir / "b.svg")).ok);
    CHECK(!run(s, "type.save doc=font1 path=" + q(dir / "b.ttf")).ok);
    CHECK(run(s, "type.render doc=font1 text=\"TIO\\nOI\" path=" + q(dir / "r.png")).ok && fs::exists(dir / "r.png"));
    CHECK(run(s, "type.render doc=font1 glyph=O path=" + q(dir / "g.png")).ok && fs::exists(dir / "g.png"));
    CHECK(run(s, "type.proof doc=font1 path=" + q(dir / "p.png")).ok && fs::exists(dir / "p.png"));
    CHECK(run(s, "type.proof doc=font1 path=" + q(dir / "p.pdf") + " sizes=48,24").ok && readAll(dir / "p.pdf").compare(0, 5, "%PDF-") == 0);
    {
        const std::string ttf = readAll(dir / "b.ttf");
        stbtt_fontinfo info;
        CHECK(stbtt_InitFont(&info, reinterpret_cast<const unsigned char*>(ttf.data()), 0) == 1);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, 'I', &adv, &lsb);
        CHECK(adv == static_cast<int>(std::lround(font().glyph("I")->advance)));
        CHECK(stbtt_FindGlyphIndex(&info, 0xCD) > 0);
    }
    r = run(s, "type.open path=" + q(dir / "b.ttf"));
    CHECK(r.ok && r.message.find("Batchy") != std::string::npos);
    CHECK(run(s, "type.open path=" + q(dir / "b.woff")).ok);
    CHECK(run(s, "type.open path=" + q(dir / "b.ufo")).ok);
    r = run(s, "type.open path=" + q(dir / "b.typeface.json"));
    CHECK(r.ok);
    const std::string reopened = std::get<std::string>(*r.field("doc"));
    CHECK(s.workspace().getAs<FontDoc>(reopened)->font.info.license == "OFL 1.1");
    CHECK(!run(s, "type.open path=" + q(dir / "nope.ttf")).ok);
    // one-shot use: read-only and output commands take font=<file> instead of doc=
    {
        const std::size_t docs = s.workspace().ids().size();
        r = run(s, "type.info font=" + q(dir / "b.typeface.json"));
        CHECK(r.ok && r.message.find("OFL 1.1") != std::string::npos && !r.field("doc"));
        CHECK(run(s, "type.export font=" + q(dir / "b.typeface.json") + " path=" + q(dir / "oneshot.ttf")).ok && fs::exists(dir / "oneshot.ttf"));
        CHECK(s.workspace().ids().size() == docs);  // nothing stays open
        CHECK(!run(s, "type.info").ok);
        CHECK(!run(s, "type.info font=" + q(dir / "missing.ttf")).ok);
    }
    CHECK(run(s, "type.set_metrics doc=font1 units_per_em=2000").ok && near(glyphBounds(font(), *font().glyph("O")).height(), 1440, 2));
    CHECK(run(s, "type.glyph_remove doc=font1 glyphs=I").ok && !font().glyph("I") && font().glyph("l"));
    {
        // subsetting: keep= removes everything else (but .notdef and the bases of kept composites)
        Font& rf = s.workspace().getAs<FontDoc>(reopened)->font;
        CHECK(run(s, "type.component_add doc=" + reopened + " glyph=Ograve base=O").ok);
        CHECK(run(s, "type.glyph_remove doc=" + reopened + " keep=Ograve,T").ok);
        CHECK(rf.glyphs.size() == 4 && rf.glyph(".notdef") && rf.glyph("O") && rf.glyph("T") && !rf.glyph("I"));
        CHECK(!run(s, "type.glyph_remove doc=" + reopened).ok);
        CHECK(run(s, "type.glyph_remove doc=" + reopened + " glyphs=U+0041-U+005A").ok && rf.glyphs.size() == 2 && rf.glyph("Ograve") &&
              !rf.glyph("Ograve")->contours.empty() && rf.glyph("Ograve")->components.empty());
    }
    CHECK(!run(s, "type.draw doc=nope glyph=A rect=0,0,1,1").ok);
    CHECK(!run(s, "type.draw doc=font1 glyph=A path=\"M0 0 L\"").ok);

    // handwriting: template -> "scan" (letters painted with the font) -> trace
    CHECK(run(s, "type.grid_template path=" + q(dir / "tpl.png") + " chars=OT cols=2 cell=240").ok);
    {
        Image sheet = raster::load(dir / "tpl.png");
        const Font& src = font();
        const double k = 240.0 / (src.metrics.ascender - src.metrics.descender);
        fillContours(sheet, src.glyph("O")->contours, {k, 0, 0, -k, 20, src.metrics.ascender * k}, {30, 30, 30, 255});
        fillContours(sheet, src.glyph("T")->contours, {k, 0, 0, -k, 260, src.metrics.ascender * k}, {30, 30, 30, 255});
        raster::save(sheet, dir / "scan.png");
    }
    CHECK(run(s, "type.new family=Hand -> h").ok);
    r = run(s, "type.trace_grid doc=font2 image=" + q(dir / "scan.png") + " chars=OT cols=2 preview=true");
    CHECK(r.ok && r.attachments.size() == 1);
    Font& hand = s.workspace().getAs<FontDoc>("font2")->font;
    CHECK(hand.glyph("O") && hand.glyph("O")->contours.size() == 2 && hand.glyph("T") && hand.glyph("T")->contours.size() == 1);
    if (hand.glyph("T")) {
        Box tb = glyphBounds(hand, *hand.glyph("T"));
        CHECK(near(tb.y1, 700, 15) && near(tb.y0, 0, 15) && near(tb.x0, 60, 1e-6));
    }
    CHECK(run(s, "type.trace_image doc=font2 image=" + q(dir / "scan.png") + " glyph=W fit=cap").ok && hand.glyph("W"));
    CHECK(run(s, "type.export doc=font2 path=" + q(dir / "hand.ttf")).ok);
    {
        const std::string ttf = readAll(dir / "hand.ttf");
        stbtt_fontinfo info;
        CHECK(stbtt_InitFont(&info, reinterpret_cast<const unsigned char*>(ttf.data()), 0) == 1);
        int w, h, xo, yo;
        unsigned char* bmp = stbtt_GetCodepointBitmap(&info, 0, stbtt_ScaleForPixelHeight(&info, 64), 'T', &w, &h, &xo, &yo);
        CHECK(bmp != nullptr && w > 10 && h > 20);
        if (bmp) stbtt_FreeBitmap(bmp, nullptr);
    }
    CHECK(run(s, "type.close doc=font2").ok && !s.workspace().get("font2"));
    CHECK(run(s, "undo").ok && s.workspace().get("font2"));
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}")
            .value_or("");
    };
    const std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"type_new", "type_open", "type_save", "type_export", "type_glyph_add", "type_draw", "type_import_svg",
                             "type_transform", "type_embolden", "type_remove_overlap", "type_kern_auto", "type_preview", "type_proof",
                             "type_trace_grid", "type_grid_template", "type_metrics_auto", "type_oblique"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string r = call("type_new", R"({"family":"Mcp"})");
    CHECK(r.find("\"doc\":\"font1\"") != std::string::npos);
    r = call("type_draw", R"({"doc":"font1","glyph":"O","ellipse":[300,350,250,360],"preview":true})");
    CHECK(r.find("\"isError\":false") != std::string::npos && r.find("\"type\":\"image\"") != std::string::npos);
    r = call("type_preview", R"({"doc":"font1","text":"OO"})");
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("undo", "{}");
    CHECK(r.find("undone") != std::string::npos);
    CHECK(session.workspace().getAs<FontDoc>("font1")->font.glyph("O") == nullptr);
    r = call("type_draw", R"({"doc":"nope","glyph":"A","rect":[0,0,1,1]})");
    CHECK(r.find("\"isError\":true") != std::string::npos);
}

}  // namespace

int main() {
    const fs::path dir = tempDir();
    testNames();
    testGeometry();
    testOutlineOps();
    testTtfWithStb(dir);
    testCff();
    testFiles(dir);
    testRender(dir);
    testTrace();
    testAutoKern();
    testCommands(dir / "cmd");
    testMcp();
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("type");
}
