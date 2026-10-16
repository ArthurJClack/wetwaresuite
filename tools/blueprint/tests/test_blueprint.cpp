#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "achclwss/blueprint/blueprint.hpp"
#include "achclwss/core/cli.hpp"
#include "achclwss/mcp/server.hpp"
#include "achclwss/raster/codec.hpp"
#include "check.hpp"

using namespace achclwss;
using namespace achclwss::blueprint;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

fs::path tmp(const std::string& name) { return fs::temp_directory_path() / ("achclwss_blueprint_" + name); }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string str(const char* s) { return s; }

double numField(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return std::nan("");
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return std::nan("");
}

std::string textField(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

std::shared_ptr<Drawing> drawing(Session& s, const std::string& id) { return s.workspace().getAs<Drawing>(id); }

void testGeometry() {
    std::vector<Vec2> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    CHECK(near(polygonArea(square), 100));
    CHECK(near(polygonArea({{0, 0}, {0, 10}, {10, 10}, {10, 0}}), -100));
    CHECK(near(polylineLength(square, true), 40));
    CHECK(near(polylineLength(square, false), 30));
    Vec2 c = polygonCentroid(square);
    CHECK(near(c.x, 5) && near(c.y, 5));

    // offsetting a counter-clockwise square to the left shrinks it with mitred corners
    auto in = offsetPolyline(square, true, 1);
    CHECK(in.size() == 4);
    CHECK(near(in[0].x, 1) && near(in[0].y, 1));
    CHECK(near(polygonArea(in), 64));
    auto out = offsetPolyline(square, true, -2);
    CHECK(near(polygonArea(out), 196));
    auto open = offsetPolyline({{0, 0}, {10, 0}, {10, 10}}, false, 1);
    CHECK(open.size() == 3 && near(open[0].y, 1) && near(open[1].x, 9) && near(open[1].y, 1) && near(open[2].x, 9));

    CHECK(near(normalizeAngle(-90), 270) && near(normalizeAngle(720), 0));

    // transforms
    Xform r = Xform::rotate({0, 0}, 90);
    Vec2 p = r.apply({1, 0});
    CHECK(near(p.x, 0) && near(p.y, 1));
    Xform m = Xform::mirror({0, 0}, {0, 1});  // across the y axis
    p = m.apply({3, 2});
    CHECK(near(p.x, -3) && near(p.y, 2) && m.mirrors());
    Xform sc = Xform::scaleAbout({1, 1}, 2);
    p = sc.apply({2, 2});
    CHECK(near(p.x, 3) && near(p.y, 3) && near(sc.scale(), 2));

    Entity arc;
    arc.type = EntityType::Arc;
    arc.c = {0, 0};
    arc.r = 1;
    arc.a0 = 0;
    arc.a1 = 90;
    transformEntity(arc, m);  // mirrored quarter arc runs from 90 to 180
    CHECK(near(arc.a0, 90) && near(arc.a1, 180));

    double num = 0, den = 0;
    CHECK(parseScale("1:50", num, den) && near(num, 1) && near(den, 50));
    CHECK(parseScale("100", num, den) && near(den, 100));
    CHECK(parseScale("2:1", num, den) && near(num, 2));
    CHECK(!parseScale("1:0", num, den) && !parseScale("abc", num, den));

    CHECK(aciColor(1) == (Color{255, 0, 0, 255}));
    CHECK(nearestAci({255, 0, 0, 255}) == 1 && nearestAci({0, 0, 255, 255}) == 5 && nearestAci({0, 0, 0, 255}) == 7);
    CHECK(textWidth("") == 0 && textWidth("WW") > textWidth("ii"));
}

void testDimensionsAndFormat() {
    Drawing doc;
    Entity d;
    d.type = EntityType::Dimension;
    d.dim = DimKind::Aligned;
    d.pts = {{0, 0}, {3000, 4000}, {0, 100}};
    CHECK(near(dimensionValue(d), 5000));
    CHECK(dimensionText(doc, d) == "5000");
    d.dim = DimKind::Horizontal;
    CHECK(near(dimensionValue(d), 3000));
    d.dim = DimKind::Vertical;
    CHECK(near(dimensionValue(d), 4000));
    d.text = "<> clear";
    CHECK(dimensionText(doc, d) == "4000 clear");
    d.text.clear();

    d.dim = DimKind::Radius;
    d.r = 250;
    CHECK(dimensionText(doc, d) == "R250");
    d.dim = DimKind::Diameter;
    CHECK(near(dimensionValue(d), 500) && dimensionText(doc, d) == "Ø500");

    d.dim = DimKind::Angular;
    d.pts = {{0, 0}, {10, 0}, {0, 10}, {5, 5}};
    CHECK(near(dimensionValue(d), 90));
    CHECK(dimensionText(doc, d) == "90°");
    d.pts[3] = {-5, -5};  // the other side: the reflex angle
    CHECK(near(dimensionValue(d), 270));

    doc.units = Unit::M;
    CHECK(formatLength(doc, 3.14159) == "3.14");
    doc.dims.suffix = true;
    CHECK(formatLength(doc, 2) == "2.00 m");
    doc.units = Unit::Ft;
    doc.dims.suffix = false;
    CHECK(formatLength(doc, 12.5) == "12'-6\"");
    CHECK(formatLength(doc, 3.0 + 1.0 / 32) == "3'-0 3/8\"");
    doc.units = Unit::Mm;
    CHECK(formatArea(doc, 4e6) == "4.00 m²");
    CHECK(formatArea(doc, 15700) == "15700 mm²");  // small parts don't round to 0.02 m²
    doc.units = Unit::Ft;
    CHECK(formatArea(doc, 100) == "100.00 ft²");
}

void testCommands() {
    Session s;
    registerCommands(s);
    Result r = s.run("blueprint.new", {{"units", str("mm")}, {"scale", str("1:50")}, {"paper", str("A3")}});
    CHECK(r.ok && textField(r, "doc") == "bp1");
    auto doc = drawing(s, "bp1");
    CHECK(doc && near(doc->k(), 1.0 / 50) && near(doc->paperWidth(), 420) && near(doc->paperHeight(), 297));
    const Value bp = str("bp1");

    // line by endpoints and by length/angle
    r = s.run("blueprint.line", {{"doc", bp}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 3000.0}, {"y2", 4000.0}});
    CHECK(r.ok && textField(r, "entity") == "e1" && near(numField(r, "length"), 5000));
    r = s.run("blueprint.line", {{"doc", bp}, {"x1", 0.0}, {"y1", 0.0}, {"length", 1000.0}, {"angle", 90.0}});
    CHECK(r.ok);
    CHECK(near(doc->entities[1].pts[1].x, 0, 1e-9) && near(doc->entities[1].pts[1].y, 1000));

    // dimension measured from the geometry
    r = s.run("blueprint.dimension", {{"doc", bp}, {"entity", str("e1")}});
    CHECK(r.ok && near(numField(r, "value"), 5000) && textField(r, "text") == "5000");
    r = s.run("blueprint.dimension", {{"doc", bp}, {"kind", str("horizontal")}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 3000.0}, {"y2", 4000.0}});
    CHECK(r.ok && near(numField(r, "value"), 3000));
    r = s.run("blueprint.dimension", {{"doc", bp}, {"kind", str("angular")}, {"entity", str("e1,e2")}});
    CHECK(r.ok && near(numField(r, "value"), degrees(std::atan2(3.0, 4.0)), 1e-6));
    // with no location an angular dimension takes the angle under 180 degrees
    r = s.run("blueprint.dimension", {{"doc", bp}, {"kind", str("angular")}, {"cx", 80.0}, {"cy", 85.0}, {"x1", 60.0}, {"y1", 100.0}, {"x2", 100.0}, {"y2", 100.0}});
    CHECK(r.ok && near(numField(r, "value"), 180 - 2 * degrees(std::atan2(15.0, 20.0)), 1e-6));
    CHECK(s.run("blueprint.delete", {{"doc", bp}, {"entities", str("last")}}).ok);
    r = s.run("blueprint.circle", {{"doc", bp}, {"cx", 0.0}, {"cy", 0.0}, {"diameter", 600.0}});
    CHECK(r.ok && near(numField(r, "area"), kPi * 300 * 300));
    const std::string circle = textField(r, "entity");
    r = s.run("blueprint.dimension", {{"doc", bp}, {"kind", str("radius")}, {"entity", Value{circle}}});
    CHECK(r.ok && textField(r, "text") == "R300");
    r = s.run("blueprint.dimension", {{"doc", bp}, {"kind", str("diameter")}, {"entity", Value{circle}}});
    CHECK(r.ok && near(numField(r, "value"), 600));
    CHECK(!s.run("blueprint.dimension", {{"doc", bp}, {"kind", str("radius")}, {"entity", str("e1")}}).ok);

    // a dimension follows its points when they move
    const std::string dim1 = "e3";
    CHECK(s.run("blueprint.scale", {{"doc", bp}, {"entities", Value{dim1}}, {"factor", 2.0}, {"cx", 0.0}, {"cy", 0.0}}).ok);
    r = s.run("blueprint.measure", {{"doc", bp}, {"entity", Value{dim1}}});
    CHECK(r.ok && near(numField(r, "value"), 10000));

    // rect, area, perimeter, offset, measure
    r = s.run("blueprint.rect", {{"doc", bp}, {"x", 0.0}, {"y", 0.0}, {"width", 4000.0}, {"height", 3000.0}});
    CHECK(r.ok && near(numField(r, "area"), 12e6) && near(numField(r, "perimeter"), 14000));
    const std::string rect = textField(r, "entity");
    r = s.run("blueprint.measure", {{"doc", bp}, {"entity", Value{rect}}});
    CHECK(r.ok && near(numField(r, "area"), 12e6) && near(numField(r, "perimeter"), 14000) && textField(r, "area_text") == "12.00 m²");
    r = s.run("blueprint.offset", {{"doc", bp}, {"entity", Value{rect}}, {"distance", 100.0}});
    CHECK(r.ok);
    const Entity& grown = doc->entities.back();
    CHECK(near(std::abs(polygonArea(grown.pts)), 4200.0 * 3200.0));
    r = s.run("blueprint.offset", {{"doc", bp}, {"entity", Value{rect}}, {"distance", 100.0}, {"x", 2000.0}, {"y", 1500.0}});
    CHECK(r.ok && near(std::abs(polygonArea(doc->entities.back().pts)), 3800.0 * 2800.0));
    r = s.run("blueprint.offset", {{"doc", bp}, {"entity", str("e1")}, {"distance", 50.0}});
    CHECK(r.ok);
    {
        const Entity& l = doc->entities.back();
        // left of (3,4) direction is (-4,3)/5
        CHECK(near(l.pts[0].x, -40) && near(l.pts[0].y, 30));
    }
    r = s.run("blueprint.offset", {{"doc", bp}, {"entity", Value{circle}}, {"distance", 50.0}, {"x", 0.0}, {"y", 0.0}});
    CHECK(r.ok && near(doc->entities.back().r, 250));

    r = s.run("blueprint.measure", {{"doc", bp}, {"points", NumberList{0, 0, 3, 4}}});
    CHECK(r.ok && near(numField(r, "distance"), 5));
    r = s.run("blueprint.measure", {{"doc", bp}, {"points", NumberList{0, 0, 10, 0, 10, 10, 0, 10}}});
    CHECK(r.ok && near(numField(r, "area"), 100) && near(numField(r, "perimeter"), 40));
    r = s.run("blueprint.measure", {{"doc", bp}, {"points", NumberList{10, 0, 0, 0, 0, 10}}, {"kind", str("angle")}});
    CHECK(r.ok && near(numField(r, "angle"), 90));
    r = s.run("blueprint.measure", {{"doc", bp}, {"entity", str("e1,e2")}});
    CHECK(r.ok && near(numField(r, "angle"), degrees(std::atan2(3.0, 4.0)), 1e-6));

    // move / copy / rotate / mirror
    const std::size_t before = doc->entities.size();
    r = s.run("blueprint.copy", {{"doc", bp}, {"entities", str("e2")}, {"dx", 500.0}, {"count", std::int64_t{3}}});
    CHECK(r.ok && doc->entities.size() == before + 3);
    CHECK(near(doc->entities.back().pts[0].x, 1500));
    CHECK(s.run("blueprint.move", {{"doc", bp}, {"entities", str("last")}, {"dx", 10.0}, {"dy", -10.0}}).ok);
    CHECK(near(doc->entities.back().pts[0].x, 1510) && near(doc->entities.back().pts[0].y, -10));
    CHECK(s.run("blueprint.rotate", {{"doc", bp}, {"entities", str("e2")}, {"angle", -90.0}, {"cx", 0.0}, {"cy", 0.0}}).ok);
    CHECK(near(doc->entities[1].pts[1].x, 1000) && near(doc->entities[1].pts[1].y, 0, 1e-9));
    CHECK(s.run("blueprint.mirror", {{"doc", bp}, {"entities", str("e2")}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 0.0}, {"y2", 1.0}, {"copy", true}}).ok);
    CHECK(near(doc->entities.back().pts[1].x, -1000));

    // text, hatch, point, polyline, arc
    r = s.run("blueprint.text", {{"doc", bp}, {"x", 100.0}, {"y", 100.0}, {"text", str("Kitchen")}, {"align", str("center")}});
    CHECK(r.ok && near(doc->entities.back().height, 3.5 * 50));
    CHECK(s.run("blueprint.hatch", {{"doc", bp}, {"entity", Value{rect}}, {"pattern", str("solid")}}).ok);
    CHECK(!s.run("blueprint.hatch", {{"doc", bp}, {"entity", str("e1")}}).ok);
    CHECK(s.run("blueprint.point", {{"doc", bp}, {"x", 1.0}, {"y", 2.0}}).ok);
    CHECK(s.run("blueprint.polyline", {{"doc", bp}, {"points", NumberList{0, 0, 100, 0, 100, 100}}}).ok);
    CHECK(!s.run("blueprint.polyline", {{"doc", bp}, {"points", NumberList{0, 0, 100}}}).ok);
    r = s.run("blueprint.arc", {{"doc", bp}, {"cx", 0.0}, {"cy", 0.0}, {"radius", 100.0}, {"start", 0.0}, {"end", 180.0}});
    CHECK(r.ok && near(numField(r, "length"), kPi * 100));

    // list with filters
    r = s.run("blueprint.list", {{"doc", bp}, {"entities", str("type:circle")}});
    CHECK(r.ok && near(numField(r, "count"), 2));
    CHECK(!s.run("blueprint.list", {{"doc", bp}, {"entities", str("e999")}}).ok);
}

void testLayersAndUndo() {
    Session s;
    registerCommands(s);
    CHECK(s.run("blueprint.new", {}).ok);
    const Value bp = str("bp1");
    auto doc = drawing(s, "bp1");
    CHECK(s.run("blueprint.layer_add", {{"doc", bp}, {"name", str("WALLS")}, {"color", str("red")}, {"weight", 0.5}, {"linetype", str("dashed")},
                                        {"current", true}})
              .ok);
    CHECK(doc->current == "WALLS" && doc->layer("WALLS")->linetype == LineType::Dashed);
    CHECK(!s.run("blueprint.layer_add", {{"doc", bp}, {"name", str("WALLS")}}).ok);
    CHECK(s.run("blueprint.line", {{"doc", bp}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 100.0}, {"y2", 0.0}}).ok);
    CHECK(doc->entities.back().layer == "WALLS");
    CHECK(s.run("blueprint.line", {{"doc", bp}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 0.0}, {"y2", 100.0}, {"layer", str("NOTES")}}).ok);
    CHECK(doc->layer("NOTES") != nullptr);

    // locked layers protect their entities
    CHECK(s.run("blueprint.layer_set", {{"doc", bp}, {"name", str("WALLS")}, {"locked", true}}).ok);
    CHECK(!s.run("blueprint.move", {{"doc", bp}, {"entities", str("e1")}, {"dx", 5.0}}).ok);
    CHECK(!s.run("blueprint.line", {{"doc", bp}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 1.0}, {"y2", 1.0}}).ok);
    Result r = s.run("blueprint.move", {{"doc", bp}, {"entities", str("all")}, {"dx", 5.0}});
    CHECK(r.ok && near(doc->entities[0].pts[0].x, 0) && near(doc->entities[1].pts[0].x, 5));
    CHECK(s.run("blueprint.layer_set", {{"doc", bp}, {"name", str("WALLS")}, {"locked", false}, {"rename", str("A-WALL")}}).ok);
    CHECK(doc->entities[0].layer == "A-WALL" && doc->current == "A-WALL");

    // hidden layers don't render
    CHECK(s.run("blueprint.layer_set", {{"doc", bp}, {"name", str("NOTES")}, {"visible", false}}).ok);
    Box b = extents(*doc);
    CHECK(near(b.x0, 0) && near(b.x1, 100) && near(b.height(), 0));

    // undo / redo walk the history one command at a time
    const std::size_t steps = s.history().labels().size();
    CHECK(steps >= 6);
    CHECK(s.run("blueprint.delete", {{"doc", bp}, {"entities", str("all")}}).ok);
    CHECK(doc->entities.empty());
    CHECK(s.history().undo());
    CHECK(doc->entities.size() == 2);
    CHECK(s.history().redo());
    CHECK(doc->entities.empty());
    CHECK(s.history().undo());
    CHECK(s.history().undo());  // un-hide NOTES
    CHECK(doc->layer("NOTES")->visible);
    CHECK(s.history().undo());  // un-rename
    CHECK(doc->layer("WALLS") && doc->entities[0].layer == "WALLS");
    // failed commands don't touch the drawing or the history
    const std::size_t n = s.history().labels().size();
    CHECK(!s.run("blueprint.circle", {{"doc", bp}, {"cx", 0.0}, {"cy", 0.0}, {"radius", -1.0}}).ok);
    CHECK(s.history().labels().size() == n);
    // close is undoable too
    CHECK(s.run("blueprint.close", {{"doc", bp}}).ok);
    CHECK(!s.workspace().get("bp1"));
    CHECK(s.history().undo());
    CHECK(s.workspace().get("bp1"));
    CHECK(!s.run("blueprint.info", {{"doc", str("bp7")}}).ok);
}

void testFloorPlan() {
    Session s;
    registerCommands(s);
    CHECK(s.run("blueprint.new", {{"scale", str("1:50")}}).ok);
    const Value bp = str("bp1");
    auto doc = drawing(s, "bp1");
    Result r = s.run("blueprint.wall", {{"doc", bp}, {"points", NumberList{0, 0, 5000, 0, 5000, 4000, 0, 4000}}, {"closed", true}, {"thickness", 200.0}});
    CHECK(r.ok && near(numField(r, "length"), 18000));
    const std::string wall = textField(r, "entity");
    // faces: outer 5200 x 4200, inner 4800 x 3800
    const Entity& w = doc->entities.back();
    CHECK(near(std::abs(polygonArea(wallFace(w, 1))), 4800.0 * 3800.0) || near(std::abs(polygonArea(wallFace(w, -1))), 4800.0 * 3800.0));

    r = s.run("blueprint.door", {{"doc", bp}, {"wall", Value{wall}}, {"at", 1000.0}, {"width", 900.0}});
    CHECK(r.ok);
    const std::string door = textField(r, "entity");
    CHECK(!s.run("blueprint.door", {{"doc", bp}, {"wall", Value{wall}}, {"at", 1200.0}}).ok);  // overlaps
    CHECK(!s.run("blueprint.door", {{"doc", bp}, {"wall", Value{wall}}, {"at", 4800.0}}).ok);  // runs round the corner
    r = s.run("blueprint.window", {{"doc", bp}, {"wall", Value{wall}}, {"x", 2500.0}, {"y", 4100.0}, {"width", 1200.0}});
    CHECK(r.ok);
    CHECK(near(doc->entities.back().at, 5000 + 4000 + 2500));

    // the door leaf and swing arc are there, and the wall has an opening
    auto pieces = explode(*doc, doc->entities[1]);
    int arcs = 0, lines = 0;
    for (const auto& pc : pieces) {
        arcs += pc.e.type == EntityType::Arc;
        lines += pc.e.type == EntityType::Line;
    }
    CHECK(arcs == 1 && lines == 1);
    CHECK(near(pieces[1].e.r, 900));
    auto wallPieces = explode(*doc, doc->entities[0]);
    double faceLength = 0;
    for (const auto& pc : wallPieces) {
        if (pc.e.type == EntityType::Line) faceLength += distance(pc.e.pts[0], pc.e.pts[1]);
    }
    // faces 2 * 18000 minus two openings on both faces, plus 4 jambs of 200
    CHECK(near(faceLength, 5200 * 2 + 4200 * 2 + 4800 * 2 + 3800 * 2 - 2 * 900 - 2 * 1200 + 4 * 200, 1e-6));

    // room from the closed wall's inside face
    r = s.run("blueprint.room", {{"doc", bp}, {"wall", Value{wall}}, {"name", str("Living")}});
    CHECK(r.ok && near(numField(r, "area"), 4800.0 * 3800.0) && textField(r, "area_text") == "18.24 m²");
    r = s.run("blueprint.room", {{"doc", bp}, {"points", NumberList{0, 0, 2000, 0, 2000, 1000, 0, 1000}}, {"name", str("Store")}});
    CHECK(r.ok && near(numField(r, "area"), 2e6));

    // an interior wall butting into the outer wall: no end caps, and the outer face opens up
    r = s.run("blueprint.wall", {{"doc", bp}, {"points", NumberList{2500, 100, 2500, 3900}}, {"thickness", 100.0}});
    CHECK(r.ok);
    int caps = 0;
    for (const auto& pc : explode(*doc, doc->entities.back())) {
        if (pc.e.type == EntityType::Line && near(pc.e.pts[0].y, pc.e.pts[1].y)) ++caps;
    }
    CHECK(caps == 0);

    // moving the wall carries its door; mirroring flips the swing; deleting removes it
    CHECK(s.run("blueprint.move", {{"doc", bp}, {"entities", Value{wall}}, {"dx", 1000.0}}).ok);
    auto moved = explode(*doc, doc->entities[1]);
    CHECK(!moved.empty() && near(moved[0].e.pts[0].x, 1000 + 1000 - 450));
    CHECK(s.run("blueprint.mirror", {{"doc", bp}, {"entities", Value{wall}}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 0.0}, {"y2", 1.0}}).ok);
    CHECK(doc->entities[1].swingRight);
    CHECK(s.run("blueprint.set", {{"doc", bp}, {"entities", Value{door}}, {"at", 3000.0}}).ok);
    CHECK(near(doc->entities[1].at, 3000));
    CHECK(!s.run("blueprint.set", {{"doc", bp}, {"entities", Value{door}}, {"at", 99999.0}}).ok);
    CHECK(s.run("blueprint.copy", {{"doc", bp}, {"entities", Value{wall}}, {"dy", 10000.0}}).ok);
    int doors = 0;
    for (const auto& e : doc->entities) doors += e.type == EntityType::Door;
    CHECK(doors == 2);
    r = s.run("blueprint.delete", {{"doc", bp}, {"entities", Value{wall}}});
    CHECK(r.ok && textField(r, "entities") == wall + "," + door + ",e3");

    // snap rounds input
    CHECK(s.run("blueprint.grid", {{"doc", bp}, {"spacing", 1000.0}, {"snap", 50.0}}).ok);
    CHECK(s.run("blueprint.line", {{"doc", bp}, {"x1", 12.0}, {"y1", 74.0}, {"x2", 1013.0}, {"y2", 26.0}}).ok);
    CHECK(near(doc->entities.back().pts[0].x, 0) && near(doc->entities.back().pts[0].y, 50) && near(doc->entities.back().pts[1].x, 1000));

    // page fitting
    CHECK(s.run("blueprint.page", {{"doc", bp}, {"paper", str("A4")}, {"orientation", str("portrait")}, {"scale", str("fit")}}).ok);
    CHECK(doc->paper == "A4" && !doc->landscape);
    Box b = extents(*doc);
    Vec2 lo = doc->toPaper({b.x0, b.y0}), hi = doc->toPaper({b.x1, b.y1});
    CHECK(lo.x >= 0 && lo.y >= 0 && hi.x <= 210 && hi.y <= 297);
    CHECK(s.run("blueprint.titleblock", {{"doc", bp}, {"title", str("Flat 2")}, {"author", str("AC")}, {"sheet", str("A-101")}}).ok);
    CHECK(doc->title.title == "Flat 2" && doc->title.sheet == "A-101");
}

void testFiles() {
    Session s;
    registerCommands(s);
    CHECK(s.run("blueprint.new", {{"units", str("mm")}, {"scale", str("1:100")}, {"paper", str("A4")}, {"orientation", str("portrait")},
                                  {"title", str("House")}})
              .ok);
    const Value bp = str("bp1");
    auto doc = drawing(s, "bp1");
    CHECK(s.run("blueprint.layer_add", {{"doc", bp}, {"name", str("WALLS")}, {"color", str("#336699")}, {"weight", 0.5}, {"current", true}}).ok);
    CHECK(s.run("blueprint.layer_add", {{"doc", bp}, {"name", str("HIDDEN")}, {"color", str("blue")}, {"linetype", str("hidden")}}).ok);
    CHECK(s.run("blueprint.wall", {{"doc", bp}, {"points", NumberList{0, 0, 6000, 0, 6000, 4000, 0, 4000}}, {"closed", true}, {"fill", str("solid")}}).ok);
    CHECK(s.run("blueprint.door", {{"doc", bp}, {"wall", str("e1")}, {"at", 2000.0}, {"hinge", str("end")}, {"side", str("right")}}).ok);
    CHECK(s.run("blueprint.window", {{"doc", bp}, {"wall", str("e1")}, {"at", 8000.0}}).ok);
    CHECK(s.run("blueprint.room", {{"doc", bp}, {"wall", str("e1")}, {"name", str("Hall; main")}}).ok);
    CHECK(s.run("blueprint.line", {{"doc", bp}, {"x1", 0.0}, {"y1", -1000.0}, {"x2", 6000.0}, {"y2", -1000.0}, {"layer", str("HIDDEN")}}).ok);
    CHECK(s.run("blueprint.dimension", {{"doc", bp}, {"entity", str("e5")}, {"offset", -500.0}, {"layer", str("0")}}).ok);
    CHECK(s.run("blueprint.circle", {{"doc", bp}, {"cx", 3000.0}, {"cy", 2000.0}, {"radius", 500.0}}).ok);
    CHECK(s.run("blueprint.arc", {{"doc", bp}, {"cx", 3000.0}, {"cy", 2000.0}, {"radius", 800.0}, {"start", 10.0}, {"end", 100.0}}).ok);
    CHECK(s.run("blueprint.text", {{"doc", bp}, {"x", 100.0}, {"y", 100.0}, {"text", str("Plan Ø 45° é")}, {"height", 200.0},
                                   {"rotation", 30.0}, {"align", str("right")}, {"valign", str("top")}})
              .ok);
    CHECK(s.run("blueprint.hatch", {{"doc", bp}, {"points", NumberList{0, 0, 1000, 0, 1000, 1000}}, {"pattern", str("cross")}, {"angle", 30.0}}).ok);
    CHECK(s.run("blueprint.point", {{"doc", bp}, {"x", 7.0}, {"y", 8.0}}).ok);
    CHECK(s.run("blueprint.dimension", {{"doc", bp}, {"kind", str("radius")}, {"entity", str("e7")}, {"angle", 30.0}}).ok);
    CHECK(s.run("blueprint.dimension", {{"doc", bp}, {"kind", str("angular")}, {"cx", 0.0}, {"cy", 0.0}, {"x1", 1000.0}, {"y1", 0.0}, {"x2", 0.0},
                                        {"y2", 1000.0}, {"text", str("<> typ")}})
              .ok);
    CHECK(s.run("blueprint.polyline", {{"doc", bp}, {"points", NumberList{0, 5000, 1000, 5500, 2000, 5000}}}).ok);
    CHECK(s.run("blueprint.set", {{"doc", bp}, {"entities", str("last")}, {"color", str("green")}}).ok);
    CHECK(s.run("blueprint.titleblock", {{"doc", bp}, {"author", str("Arthur")}, {"date", str("2026-10-16")}}).ok);

    // ---- DXF round trip ----
    const fs::path dxf = tmp("house.dxf");
    Result r = s.run("blueprint.save", {{"doc", bp}, {"path", Value{dxf.string()}}, {"sheet", true}});
    CHECK(r.ok && fs::exists(dxf));
    CHECK(!s.run("blueprint.save", {{"doc", bp}, {"path", Value{tmp("house.svg").string()}}}).ok);
    const std::string text = readFile(dxf);
    CHECK(text.find("AC1015") != std::string::npos && text.find("LWPOLYLINE") != std::string::npos);
    CHECK(text.find("\nDIMENSION\n") != std::string::npos && text.find("\nHATCH\n") != std::string::npos && text.find("*D1") != std::string::npos);
    CHECK(text.rfind("0\nEOF\n") == text.size() - 6);

    r = s.run("blueprint.open", {{"path", Value{dxf.string()}}});
    CHECK(r.ok && textField(r, "doc") == "bp2");
    auto back = drawing(s, "bp2");
    CHECK(back->entities.size() == doc->entities.size());
    CHECK(back->units == Unit::Mm && near(back->scaleDen, 100) && back->paper == "A4" && !back->landscape);
    CHECK(back->title.title == "House" && back->title.author == "Arthur");
    CHECK(back->layer("WALLS") && back->layer("WALLS")->color == (Color{0x33, 0x66, 0x99, 255}) && near(back->layer("WALLS")->weight, 0.5));
    CHECK(back->layer("HIDDEN") && back->layer("HIDDEN")->linetype == LineType::Hidden);
    CHECK(!back->layer("BP_SHEET"));
    CHECK(back->current == "WALLS");
    for (std::size_t i = 0; i < doc->entities.size() && i < back->entities.size(); ++i) {
        const Entity &a = doc->entities[i], &b = back->entities[i];
        CHECK(a.type == b.type);
        CHECK(a.layer == b.layer);
        CHECK(a.pts.size() == b.pts.size());
        // a linear dimension's location comes back as another point on the same dimension line
        const bool linearDim = a.type == EntityType::Dimension && a.pts.size() == 3;
        for (std::size_t j = 0; j < a.pts.size() && j < b.pts.size() && !(linearDim && j == 2); ++j) {
            CHECK(near(a.pts[j].x, b.pts[j].x, 1e-6) && near(a.pts[j].y, b.pts[j].y, 1e-6));
        }
        if (linearDim && b.pts.size() == 3) {
            Vec2 n = perp(normalized(a.pts[1] - a.pts[0]));
            CHECK(near(dot(a.pts[2] - a.pts[0], n), dot(b.pts[2] - b.pts[0], n), 1e-6));
        }
        if (a.type == EntityType::Dimension) {
            CHECK(a.dim == b.dim);
            CHECK(dimensionText(*doc, a) == dimensionText(*back, b));
        }
        if (a.type == EntityType::Circle || a.type == EntityType::Arc) CHECK(near(a.r, b.r, 1e-9) && near(a.c.x, b.c.x, 1e-9));
        if (a.type == EntityType::Arc) CHECK(near(a.a0, b.a0, 1e-9) && near(a.a1, b.a1, 1e-9));
        if (a.type == EntityType::Text) {
            CHECK(a.text == b.text && near(a.height, b.height) && near(a.rotation, b.rotation, 1e-9) && a.halign == b.halign && a.valign == b.valign);
            CHECK(near(a.c.x, b.c.x, 1e-6) && near(a.c.y, b.c.y, 1e-6));
        }
        if (a.type == EntityType::Door) CHECK(b.host == back->entities[0].id && near(a.at, b.at) && b.hingeEnd && b.swingRight);
        if (a.type == EntityType::Room) CHECK(a.name == b.name);
        if (a.type == EntityType::Hatch) CHECK(a.pattern == b.pattern && near(a.angle, b.angle, 1e-9) && near(a.spacing, b.spacing, 1e-9));
        if (a.type == EntityType::Wall) CHECK(near(a.thickness, b.thickness) && b.closed && b.fill == "solid");
        CHECK(a.color.has_value() == b.color.has_value());
    }

    // a hand-written R12 file: POLYLINE/VERTEX with a bulge, no header, layer table colours
    const std::string r12 =
        "0\nSECTION\n2\nTABLES\n0\nTABLE\n2\nLAYER\n70\n1\n0\nLAYER\n2\nWALLS\n70\n0\n62\n-1\n6\nDASHED\n0\nENDTAB\n0\nENDSEC\n"
        "0\nSECTION\n2\nENTITIES\n"
        "0\nPOLYLINE\n8\nWALLS\n66\n1\n70\n1\n"
        "0\nVERTEX\n8\nWALLS\n10\n0\n20\n0\n0\nVERTEX\n8\nWALLS\n10\n10\n20\n0\n42\n1\n0\nVERTEX\n8\nWALLS\n10\n10\n20\n10\n"
        "0\nVERTEX\n8\nWALLS\n10\n0\n20\n10\n0\nSEQEND\n"
        "0\nLINE\n8\n0\n10\n0\n20\n0\n11\n3\n21\n4\n"
        "0\nINSERT\n8\n0\n2\nBLOCK1\n10\n0\n20\n0\n"
        "0\nTEXT\n8\n0\n10\n1\n20\n2\n40\n2.5\n1\n%%c50 %%d\n"
        "0\nENDSEC\n0\nEOF\n";
    std::string notes;
    bool meta = true;
    auto old = fromDxf(r12, &notes, &meta);
    CHECK(!meta && notes.find("1 INSERT") != std::string::npos);
    CHECK(old->entities.size() == 3);
    CHECK(old->layer("WALLS") && !old->layer("WALLS")->visible && old->layer("WALLS")->color == (Color{255, 0, 0, 255}) &&
          old->layer("WALLS")->linetype == LineType::Dashed);
    const Entity& poly = old->entities[0];
    CHECK(poly.type == EntityType::Polyline && poly.closed && poly.pts.size() > 4);
    // the bulge makes a half circle of radius 5 to the right of x=10
    double maxX = 0;
    for (auto q : poly.pts) maxX = std::max(maxX, q.x);
    CHECK(near(maxX, 15, 1e-6));
    CHECK(near(std::abs(polygonArea(poly.pts)), 100 + kPi * 25 / 2, 0.5));
    CHECK(old->entities[2].text == "Ø50 °");
    bool threw = false;
    try {
        fromDxf("hello world\nthis is not dxf\n");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // ---- SVG ----
    const fs::path svg = tmp("house.svg");
    CHECK(s.run("blueprint.export", {{"doc", bp}, {"path", Value{svg.string()}}}).ok);
    const std::string sv = readFile(svg);
    CHECK(sv.rfind("<?xml", 0) == 0);
    CHECK(sv.find("width=\"210mm\" height=\"297mm\" viewBox=\"0 0 210 297\"") != std::string::npos);
    CHECK(sv.find("</svg>") != std::string::npos);
    auto count = [](const std::string& hay, const std::string& needle) {
        int n = 0;
        for (auto pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + 1)) ++n;
        return n;
    };
    CHECK(count(sv, "<g ") == count(sv, "</g>"));
    CHECK(count(sv, "<text") == count(sv, "</text>"));
    CHECK(sv.find("stroke-dasharray") != std::string::npos);  // the hidden layer
    CHECK(sv.find("Hall; main") != std::string::npos && sv.find("House") != std::string::npos);
    CHECK(sv.find("&") == std::string::npos || sv.find("&amp;") != std::string::npos);

    // ---- PDF ----
    const fs::path pdf = tmp("house.pdf");
    CHECK(s.run("blueprint.export", {{"doc", bp}, {"path", Value{pdf.string()}}}).ok);
    const std::string pd = readFile(pdf);
    CHECK(pd.rfind("%PDF-1.4", 0) == 0);
    CHECK(pd.size() > 6 && pd.substr(pd.size() - 6) == "%%EOF\n");
    CHECK(pd.find("/MediaBox [0 0 595.276 841.89]") != std::string::npos);
    CHECK(pd.find("/BaseFont /Helvetica") != std::string::npos);
    auto sx = pd.rfind("startxref\n");
    CHECK(sx != std::string::npos);
    const std::size_t xref = std::stoul(pd.substr(sx + 10));
    CHECK(pd.compare(xref, 4, "xref") == 0);
    std::istringstream xs(pd.substr(xref));
    std::string line;
    std::getline(xs, line);  // xref
    int first = 0, objects = 0;
    xs >> first >> objects;
    std::getline(xs, line);
    CHECK(first == 0 && objects == 7);
    for (int i = 0; i < objects; ++i) {
        char entry[21] = {};
        xs.read(entry, 20);
        CHECK(xs.gcount() == 20);
        if (i == 0) {
            CHECK(std::string(entry, 20) == "0000000000 65535 f \n");
            continue;
        }
        const std::size_t off = std::stoul(std::string(entry, 10));
        const std::string expect = std::to_string(i) + " 0 obj";
        CHECK(pd.compare(off, expect.size(), expect) == 0);
    }
    // stream /Length matches
    auto lenPos = pd.find("/Length ");
    const std::size_t len = std::stoul(pd.substr(lenPos + 8));
    auto streamPos = pd.find("stream\n", lenPos) + 7;
    CHECK(pd.compare(streamPos + len, 9, "endstream") == 0);

    // ---- PNG at the right size for the paper and dpi ----
    const fs::path png = tmp("house.png");
    CHECK(s.run("blueprint.export", {{"doc", bp}, {"path", Value{png.string()}}, {"dpi", 50.0}}).ok);
    raster::Image img = raster::load(png);
    CHECK(std::abs(img.width() - 413) <= 1 && std::abs(img.height() - 585) <= 1);
    bool dark = false;
    for (int y = 0; y < img.height() && !dark; y += 2) {
        for (int x = 0; x < img.width() && !dark; x += 2) dark = img.at(x, y).r < 100;
    }
    CHECK(dark);
    CHECK(!s.run("blueprint.export", {{"doc", bp}, {"path", Value{tmp("house.xyz").string()}}}).ok);

    // ---- one-shot reads from a file ----
    r = s.run("blueprint.list", {{"file", Value{dxf.string()}}, {"entities", str("type:wall")}});
    CHECK(r.ok && near(numField(r, "count"), 1));
    r = s.run("blueprint.measure", {{"file", Value{dxf.string()}}, {"entity", str("e4")}});
    CHECK(r.ok && near(numField(r, "area"), 5800.0 * 3800.0));
    r = s.run("blueprint.info", {{"file", Value{dxf.string()}}});
    CHECK(r.ok && textField(r, "scale") == "1:100");
    const fs::path conv = tmp("conv.pdf");
    r = s.run("blueprint.convert", {{"input", Value{dxf.string()}}, {"output", Value{conv.string()}}, {"paper", str("A3")}, {"scale", str("1:50")}});
    CHECK(r.ok && fs::exists(conv) && textField(r, "scale") == "1:50");

    // previews
    r = s.run("blueprint.preview", {{"doc", bp}, {"size", std::int64_t{300}}});
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    raster::Image pv = raster::decode(r.attachments[0].bytes);
    CHECK(std::max(pv.width(), pv.height()) == 300);
    CHECK(doc->preview() != nullptr);
    r = s.run("blueprint.point", {{"doc", bp}, {"x", 0.0}, {"y", 0.0}, {"preview", true}});
    CHECK(r.ok && r.attachments.size() == 1);

    for (const auto& f : {dxf, svg, pdf, png, conv}) fs::remove(f);
}

void testBatchAndMcp() {
    Session s;
    registerCommands(s);
    cli::Batch batch(s);
    std::ostringstream out, err;
    std::istringstream script(
        "blueprint.new units=m scale=1:100 -> plan\n"
        "blueprint.wall doc=$plan points=0,0,8,0,8,6,0,6 closed=true thickness=0.3 -> w\n"
        "blueprint.door doc=$plan wall=$w at=2\n"
        "blueprint.room doc=$plan wall=$w name=Studio -> r\n"
        "blueprint.measure doc=$plan entity=$r\n"
        "echo $plan $w $r\n");
    CHECK(batch.run(script, out, err) == 0);
    CHECK(err.str().empty());
    CHECK(out.str().find("bp1 e1 e3") != std::string::npos);
    CHECK(out.str().find("43.89 m²") != std::string::npos);  // 7.7 x 5.7
    auto doc = drawing(s, "bp1");
    CHECK(doc && near(doc->entities[1].width, 0.9));  // default door width in metres

    mcp::Server server(s, {"achclwss", "test", "draw"});
    json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
    bool sawWall = false, sawDim = false, sawExport = false;
    for (const auto& t : list) {
        if (t["name"] == "blueprint_wall") sawWall = t["inputSchema"]["properties"]["points"]["type"] == "array";
        if (t["name"] == "blueprint_dimension") sawDim = t["inputSchema"]["properties"]["kind"].contains("enum");
        if (t["name"] == "blueprint_export") sawExport = t["annotations"]["readOnlyHint"] == false;  // writes files
    }
    CHECK(sawWall && sawDim && sawExport);
    json req = {{"jsonrpc", "2.0"},
                {"id", 2},
                {"method", "tools/call"},
                {"params", {{"name", "blueprint_line"}, {"arguments", {{"doc", "bp1"}, {"x1", 0}, {"y1", 0}, {"x2", 3}, {"y2", 4}, {"preview", true}}}}}};
    json res = json::parse(*server.handle(req.dump()))["result"];
    CHECK(res["isError"] == false);
    CHECK(res["structuredContent"]["length"] == 5.0);
    CHECK(res["content"].size() == 2 && res["content"][1]["type"] == "image");
}

}  // namespace

int main() {
    testGeometry();
    testDimensionsAndFormat();
    testCommands();
    testLayersAndUndo();
    testFloorPlan();
    testFiles();
    testBatchAndMcp();
    return check::summary("blueprint");
}
