#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "achclwss/core/cli.hpp"
#include "achclwss/mcp/server.hpp"
#include "achclwss/mesh/document.hpp"
#include "achclwss/mesh/io.hpp"
#include "achclwss/mesh/primitives.hpp"
#include "achclwss/slicer/slicer.hpp"
#include "check.hpp"

using namespace achclwss;
using namespace achclwss::slicer;

namespace {

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

std::filesystem::path tmp(const std::string& name) { return std::filesystem::temp_directory_path() / ("achclwss_slicer_" + name); }

Loop square(double x0, double y0, double size, bool ccw = true) {
    Loop l = {{x0, y0}, {x0 + size, y0}, {x0 + size, y0 + size}, {x0, y0 + size}};
    if (!ccw) std::reverse(l.begin(), l.end());
    return l;
}

Config defaults() {
    Config c;
    c.printer = *printerPreset("ender3");
    c.filament = *filamentPreset("pla");
    applyQuality(c.settings, "normal");
    return c;
}

mesh::Mesh cubeOnBed(double size, const Printer& p) {
    mesh::Mesh m = mesh::box({size, size, size});
    centerOnBed(m, p);
    return m;
}

void testOffset() {
    // a plain square shrinks and grows by the distance on every side
    Region sq = {square(0, 0, 10)};
    Region in = offset(sq, -1);
    CHECK(in.size() == 1);
    CHECK(near(area(in), 64, 1e-3));
    Region out = offset(sq, 1);
    CHECK(out.size() == 1);
    CHECK(near(area(out), 140 + mesh::kPi, 0.2));  // grown corners are rounded
    CHECK(offset(sq, -5.5).empty());                          // shrunk away

    // square with a square hole: the hole grows as the outline shrinks
    Region ring = {square(0, 0, 20), square(5, 5, 10, false)};
    CHECK(near(area(ring), 300, 1e-6));
    Region r1 = offset(ring, -1);
    CHECK(r1.size() == 2);
    CHECK(near(area(r1), 18 * 18 - (12 * 12 - (4 - mesh::kPi)), 0.2));  // the hole grows with round corners
    // shrinking by over half the wall (5 mm) leaves only the outer corners, then nothing
    CHECK(offset(ring, -2.6).size() == 4);
    CHECK(offset(ring, -3.6).empty());
    // growing closes the hole
    Region r2 = offset(ring, 5.5);
    CHECK(r2.size() == 1);

    // two overlapping squares unite into one outline
    Region u = normalize({square(0, 0, 10), square(5, 5, 10)});
    CHECK(u.size() == 1 && near(area(u), 175, 1e-3));
    CHECK(near(area(intersect({square(0, 0, 10)}, {square(5, 5, 10)})), 25, 1e-3));
    CHECK(near(area(subtract({square(0, 0, 10)}, {square(5, 5, 10)})), 75, 1e-3));
    // inside-out loops are fixed
    Region flipped = normalize({square(0, 0, 10, false)});
    CHECK(flipped.size() == 1 && near(area(flipped), 100, 1e-6));

    // an L shape keeps its concave corner
    Region L = {{{0, 0}, {20, 0}, {20, 5}, {5, 5}, {5, 20}, {0, 20}}};
    Region Li = offset(L, -1);
    CHECK(Li.size() == 1);
    CHECK(near(area(Li), 100 - mesh::kPi / 4, 0.05));  // round inside the concave corner

    // a circle shrinks to a circle
    std::vector<Vec2> c = mesh::circle2(10, 64);
    Region ci = offset({c}, -2);
    CHECK(ci.size() == 1 && near(area(ci), area({mesh::circle2(8, 64)}), 1.0));

    // infill lines: 10 mm square, 1 mm spacing, horizontal
    auto lines = fillLines(sq, 1.0, 0);
    CHECK(lines.size() == 9 || lines.size() == 10 || lines.size() == 11);
    for (const auto& s : lines) CHECK(near(length(s.second - s.first), 10, 1e-6));
    // holes cut lines in two
    auto holed = fillLines(ring, 1.0, 0);
    std::size_t crossing = 0;
    for (const auto& s : holed) crossing += (s.first.y > 5.5 && s.first.y < 14.5) ? 1 : 0;
    CHECK(crossing > 0 && crossing % 2 == 0);
}

void testPlacement() {
    Config c = defaults();
    mesh::Mesh m = mesh::box({10, 20, 30}, false);
    mesh::transform(m, mesh::Mat4::translate({-50, 7, 12}));
    centerOnBed(m, c.printer);
    mesh::Bounds b = mesh::bounds(m);
    CHECK(near(b.min.z, 0, 1e-9));
    CHECK(near(b.center().x, 110, 1e-9) && near(b.center().y, 110, 1e-9));
    CHECK(checkFit(m, c.printer).fits);

    // a tall box tipped over lies back down on a big face
    mesh::Mesh tilted = mesh::box({40, 30, 2});
    mesh::transform(tilted, mesh::Mat4::rotateX(70));
    centerOnBed(tilted, c.printer);
    CHECK(layFlat(tilted));
    CHECK(near(mesh::bounds(tilted).size().z, 2, 1e-6));
    CHECK(!layFlat(tilted));

    mesh::Mesh big = mesh::box({300, 50, 50});
    centerOnBed(big, c.printer);
    Fit f = checkFit(big, c.printer);
    CHECK(!f.fits && f.reason.find("300") != std::string::npos);
    mesh::Mesh tall = mesh::box({10, 10, 260});
    centerOnBed(tall, c.printer);
    CHECK(!checkFit(tall, c.printer).fits);
}

// Parses "G1 X.. Y.. E.." lines of the printed part (between ;LAYER:0 and ;END_GCODE).
struct Parsed {
    bool eMonotonic = true;
    bool inBed = true;
    int layers = 0;
    double maxE = 0;
};

Parsed parse(const std::string& g, const Printer& p) {
    Parsed out;
    std::istringstream in(g);
    std::string line;
    bool body = false;
    double lastE = 0;
    while (std::getline(in, line)) {
        if (line.rfind(";LAYER:", 0) == 0) {
            body = true;
            ++out.layers;
            continue;
        }
        if (line == ";END_GCODE") body = false;
        if (!body) continue;
        if (line.rfind("G92 E0", 0) == 0) lastE = 0;
        if (line.rfind("G0 ", 0) != 0 && line.rfind("G1 ", 0) != 0) continue;
        double x = -1, y = -1, e = -1;
        bool hasX = false, hasE = false;
        std::istringstream w(line.substr(3));
        std::string tok;
        while (w >> tok) {
            if (tok[0] == ';') break;
            const double v = std::stod(tok.substr(1));
            if (tok[0] == 'X') x = v, hasX = true;
            if (tok[0] == 'Y') y = v;
            if (tok[0] == 'E') e = v, hasE = true;
        }
        if (hasX && (x < 0 || y < 0 || x > p.bedX || y > p.bedY)) out.inBed = false;
        if (hasX && hasE) {
            if (e < lastE - 1e-9) out.eMonotonic = false;
            lastE = e;
            out.maxE = std::max(out.maxE, e);
        }
    }
    return out;
}

void testSliceCube() {
    Config c = defaults();
    mesh::Mesh cube = cubeOnBed(20, c.printer);
    SliceResult r = slice(cube, c);
    CHECK(r.layers.size() == 100);
    CHECK(near(r.layers.back().z, 20, 1e-6));
    // each layer has walls; bottom and top layers are solid, the middle is sparse
    bool walls = true;
    for (const auto& L : r.layers) {
        int outer = 0;
        for (const auto& p : L.paths) outer += p.type == PathType::OuterWall;
        walls = walls && outer == 1;
    }
    CHECK(walls);
    auto has = [&](std::size_t layer, PathType t) {
        for (const auto& p : r.layers[layer].paths) {
            if (p.type == t) return true;
        }
        return false;
    };
    CHECK(has(0, PathType::Skirt));
    CHECK(has(0, PathType::SolidInfill) && !has(0, PathType::Infill));
    CHECK(has(50, PathType::Infill) && !has(50, PathType::SolidInfill));
    CHECK(has(99, PathType::SolidInfill));
    CHECK(has(96, PathType::SolidInfill) && !has(95, PathType::SolidInfill));

    Gcode g = toGcode(r, c, "cube.stl");
    CHECK(g.text.find(";FLAVOR:Marlin") != std::string::npos);
    CHECK(g.text.find(";TIME_ESTIMATE:") != std::string::npos);
    CHECK(g.text.find(";LAYER_COUNT:100") != std::string::npos);
    CHECK(g.text.find("M190 S60") != std::string::npos);
    CHECK(g.text.find("M109 S210") != std::string::npos);
    CHECK(g.text.find("M104 S205") != std::string::npos);  // second layer temperature
    CHECK(g.text.find("M106 S255") != std::string::npos);
    CHECK(g.text.find(";LAYER:0\n") != std::string::npos && g.text.find(";LAYER:99\n") != std::string::npos);
    CHECK(g.text.find(";LAYER:100\n") == std::string::npos);
    CHECK(g.text.find("G92 E0") != std::string::npos);
    CHECK(g.text.find("M84") != std::string::npos);
    Parsed parsed = parse(g.text, c.printer);
    CHECK(parsed.layers == 100);
    CHECK(parsed.eMonotonic);
    CHECK(parsed.inBed);
    CHECK(g.seconds > 600 && g.seconds < 6 * 3600);
    CHECK(g.filamentMm > 0 && near(g.filamentGrams, g.extrudedVolume / 1000 * 1.24, 1e-6));

    // solid cube: the plastic laid down is close to the cube's volume
    Config solidC = c;
    solidC.settings.infill = 100;
    solidC.settings.skirt = 0;
    Gcode solidG = toGcode(slice(cube, solidC), solidC);
    std::printf("solid 20mm cube: %.0f mm3 extruded of 8000\n", solidG.extrudedVolume);
    CHECK(near(solidG.extrudedVolume, 8000, 8000 * 0.15));
    CHECK(solidG.filamentMm > g.filamentMm);

    // patterns, brim and supports all stay on the bed and keep E increasing
    for (const char* pattern : {"lines", "grid", "triangles"}) {
        Config pc = c;
        pc.settings.infillPattern = pattern;
        pc.settings.brim = 5;
        Parsed pp = parse(toGcode(slice(cube, pc), pc).text, pc.printer);
        CHECK(pp.inBed && pp.eMonotonic && pp.layers == 100);
    }
}

void testHoleAndSupports() {
    Config c = defaults();
    // a 30 mm tube: walls go round the hole too
    mesh::Polygon2 shape{mesh::circle2(15, 48), {mesh::circle2(8, 48)}};
    mesh::Mesh tube = mesh::extrude(shape, 10);
    centerOnBed(tube, c.printer);
    SliceResult r = slice(tube, c);
    CHECK(r.layers.size() == 50);
    int outer = 0;
    for (const auto& p : r.layers[25].paths) outer += p.type == PathType::OuterWall;
    CHECK(outer == 2);

    // a T shape: the bar's overhang gets supports only when asked
    mesh::Mesh t = mesh::box({10, 10, 20}, false);
    mesh::Mesh bar = mesh::box({40, 10, 4}, false);
    mesh::transform(bar, mesh::Mat4::translate({-15, 0, 20}));
    mesh::append(t, bar);
    centerOnBed(t, c.printer);
    auto count = [](const SliceResult& s, PathType type) {
        std::size_t n = 0;
        for (const auto& L : s.layers) {
            for (const auto& p : L.paths) n += p.type == type;
        }
        return n;
    };
    CHECK(count(slice(t, c), PathType::Support) == 0);
    Config sc = c;
    sc.settings.supports = "everywhere";
    SliceResult sup = slice(t, sc);
    CHECK(count(sup, PathType::Support) > 50);
    bool low = false;
    for (const auto& p : sup.layers[1].paths) low = low || p.type == PathType::Support;
    CHECK(low);
    Parsed pp = parse(toGcode(sup, sc).text, sc.printer);
    CHECK(pp.inBed && pp.eMonotonic);
}

void testCommands() {
    Session session;
    registerCommands(session);
    const auto stl = tmp("cube.stl");
    mesh::save(stl, mesh::box({20, 20, 20}));
    Result r = session.run("slicer.load", {{"path", stl.string()}, {"filament", std::string("petg")}, {"quality", std::string("draft")}});
    CHECK(r.ok);
    CHECK(r.field("doc") && toText(*r.field("doc")) == "print1");
    CHECK(session.workspace().get("print1")->kind() == "print");

    r = session.run("slicer.info", {{"doc", std::string("print1")}});
    CHECK(r.ok && r.field("fits") && std::get<bool>(*r.field("fits")));
    CHECK(r.field("volume_cm3") && near(std::get<double>(*r.field("volume_cm3")), 8, 0.01));
    CHECK(toText(*r.field("filament")) == "petg" && near(std::get<double>(*r.field("layer_height")), 0.28, 1e-9));

    r = session.run("slicer.settings", {{"doc", std::string("print1")}, {"infill", 35.0}, {"brim", 4.0}, {"infill_pattern", std::string("lines")}});
    CHECK(r.ok && near(std::get<double>(*r.field("infill")), 35, 1e-9));
    CHECK(!session.run("slicer.settings", {{"doc", std::string("print1")}, {"layer_height", 0.9}}).ok);

    r = session.run("slicer.move", {{"doc", std::string("print1")}, {"x", 30.0}});
    CHECK(r.ok && near(std::get<NumberList>(*r.field("center"))[0], 140, 1e-6));
    CHECK(session.history().undo());
    r = session.run("slicer.info", {{"doc", std::string("print1")}});
    CHECK(near(std::get<NumberList>(*r.field("center"))[0], 110, 1e-6));

    r = session.run("slicer.rotate", {{"doc", std::string("print1")}, {"x", 45.0}});
    CHECK(r.ok && std::get<NumberList>(*r.field("size"))[2] > 25);
    r = session.run("slicer.lay_flat", {{"doc", std::string("print1")}});
    CHECK(r.ok && near(std::get<NumberList>(*r.field("size"))[2], 20, 1e-6));

    r = session.run("slicer.scale", {{"doc", std::string("print1")}, {"factor", 20.0}});
    CHECK(r.ok && r.field("fits") && !std::get<bool>(*r.field("fits")));
    r = session.run("slicer.slice", {{"doc", std::string("print1")}});
    CHECK(!r.ok && r.message.find("can't slice") != std::string::npos);
    CHECK(session.history().undo());

    r = session.run("slicer.slice", {{"doc", std::string("print1")}});
    CHECK(r.ok && std::get<std::int64_t>(*r.field("layers")) == 72);  // 0.28 mm layers on a 20 mm cube
    const auto gcode = tmp("cube.gcode");
    r = session.run("slicer.export_gcode", {{"doc", std::string("print1")}, {"path", gcode.string()}});
    CHECK(r.ok && std::filesystem::file_size(gcode) > 1000);
    std::ifstream in(gcode);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    CHECK(text.find("M109 S240") != std::string::npos);  // PETG first layer

    r = session.run("slicer.preview_layer", {{"doc", std::string("print1")}, {"layer", std::int64_t{0}}});
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    CHECK(!session.run("slicer.preview_layer", {{"doc", std::string("print1")}, {"layer", std::int64_t{500}}}).ok);
    r = session.run("slicer.preview", {{"doc", std::string("print1")}});
    CHECK(r.ok && r.attachments.size() == 1);

    // a mesh document already in the session (what cad hands over)
    auto md = mesh::newMesh(mesh::cylinder(10, 15, 48));
    const std::string meshId = session.workspace().add(md, "mesh");
    r = session.run("slicer.load", {{"mesh", meshId}, {"printer", std::string("prusa_mk4")}});
    CHECK(r.ok && toText(*r.field("doc")) == "print2");
    CHECK(near(std::get<NumberList>(*r.field("center"))[0], 125, 1e-6));

    // custom printer too small for the model
    r = session.run("slicer.load", {{"path", stl.string()}, {"printer", std::string("custom")}, {"bed_x", 30.0}, {"bed_y", 30.0}, {"bed_z", 15.0}});
    CHECK(r.ok && !std::get<bool>(*r.field("fits")));

    // one-shot
    const auto out = tmp("oneshot.gcode");
    r = session.run("slicer.slice_file", {{"input", stl.string()}, {"output", out.string()}, {"printer", std::string("bambu_a1")},
                                          {"filament", std::string("pla")}, {"infill", 15.0}});
    CHECK(r.ok && std::filesystem::exists(out));
    CHECK(r.field("layers") && std::get<std::int64_t>(*r.field("layers")) == 100);

    // batch script
    std::istringstream script("slicer.load path=" + stl.string() + " quality=fine -> job\nslicer.settings doc=$job infill=10\nslicer.slice doc=$job\n");
    std::ostringstream o, e;
    cli::Batch batch(session);
    CHECK(batch.run(script, o, e) == 0);
    std::filesystem::remove(stl);
    std::filesystem::remove(gcode);
    std::filesystem::remove(out);
}

void testMcp() {
    Session session;
    registerCommands(session);
    const auto stl = tmp("mcp.stl");
    mesh::save(stl, mesh::box({15, 15, 6}));
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}")
            .value_or("");
    };
    std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    CHECK(list.find("\"slicer_slice_file\"") != std::string::npos);
    CHECK(list.find("\"slicer_preview_layer\"") != std::string::npos);
    std::string r = call("slicer_load", R"({"path":")" + stl.generic_string() + R"(","preview":true})");
    CHECK(r.find("\"doc\":\"print1\"") != std::string::npos);
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("slicer_slice", R"({"doc":"print1","preview":true,"layer":0})");
    CHECK(r.find("\"isError\":false") != std::string::npos);
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    CHECK(r.find("\"layers\":30") != std::string::npos);
    r = call("slicer_rotate", R"({"doc":"print1","z":45})");
    CHECK(r.find("\"isError\":false") != std::string::npos);
    r = server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"undo"}})").value_or("");
    CHECK(r.find("undone") != std::string::npos);
    std::filesystem::remove(stl);
}

}  // namespace

int main() {
    testOffset();
    testPlacement();
    testSliceCube();
    testHoleAndSupports();
    testCommands();
    testMcp();
    return check::summary("slicer");
}
