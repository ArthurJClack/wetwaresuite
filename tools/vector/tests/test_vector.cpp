#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::vector;

namespace {

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

bool isColor(raster::Color c, int r, int g, int b, int tol = 8) {
    return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol;
}


std::filesystem::path tmp(const std::string& name) { return std::filesystem::temp_directory_path() / ("wetwaresuite_vector_" + name); }

void testPathParsing() {
    std::string err;
    Outline o = flattenPath("M10 10 h20 v20 h-20 z", 1.0, &err);
    CHECK(err.empty());
    CHECK(o.size() == 1);
    CHECK(o[0].closed);
    CHECK(o[0].points.size() == 4);
    CHECK(near(o[0].points[1].x, 30) && near(o[0].points[1].y, 10));
    CHECK(near(o[0].points[2].x, 30) && near(o[0].points[2].y, 30));
    CHECK(near(o[0].points[3].x, 10) && near(o[0].points[3].y, 30));

    // relative moveto after closepath starts from the subpath start
    o = flattenPath("M0 0 L10 0 L10 10 Z m 20 0 l 5 0 l 0 5 z");
    CHECK(o.size() == 2);
    CHECK(near(o[1].points[0].x, 20) && near(o[1].points[0].y, 0));
    CHECK(near(o[1].points[2].x, 25) && near(o[1].points[2].y, 5));

    // compact numbers and implicit linetos after moveto
    o = flattenPath("M1.5.5L-2-3");
    CHECK(o.size() == 1 && o[0].points.size() == 2);
    CHECK(near(o[0].points[0].x, 1.5) && near(o[0].points[0].y, 0.5));
    CHECK(near(o[0].points[1].x, -2) && near(o[0].points[1].y, -3));
    o = flattenPath("M0 0 10 0 10 10");
    CHECK(o.size() == 1 && o[0].points.size() == 3 && !o[0].closed);
    o = flattenPath("m5 5 10 0 0 10");  // relative implicit lineto
    CHECK(near(o[0].points[2].x, 15) && near(o[0].points[2].y, 15));

    // arcs: sweep 1 goes up (clockwise on screen) from the left end, sweep 0 goes down
    Bounds b = bounds(flattenPath("M0 0 A10 10 0 0 1 20 0"));
    CHECK(near(b.y0, -10, 0.05) && near(b.y1, 0, 1e-9) && near(b.x1, 20, 1e-9));
    b = bounds(flattenPath("M0 0 A10 10 0 0 0 20 0"));
    CHECK(near(b.y1, 10, 0.05) && near(b.y0, 0, 1e-9));
    // compact flags, relative endpoint
    Outline arc = flattenPath("M0 0a10 10 0 0120 0", 1.0, &err);
    CHECK(err.empty());
    CHECK(near(arc[0].points.back().x, 20) && near(arc[0].points.back().y, 0));
    CHECK(near(bounds(arc).y0, -10, 0.05));
    // radii too small are scaled up to reach the end point: a half circle of radius 50
    b = bounds(flattenPath("M0 0 A1 1 0 0 1 100 0"));
    CHECK(near(b.y0, -50, 0.2));
    // large arc flag: three quarters of a circle; the flags pick which of the two centres
    b = bounds(flattenPath("M10 0 A10 10 0 1 1 0 10"));
    CHECK(near(b.x1, 20, 0.1) && near(b.y1, 20, 0.1) && near(b.x0, 0, 0.1) && near(b.y0, 0, 0.1));
    b = bounds(flattenPath("M10 0 A10 10 0 1 0 0 10"));
    CHECK(near(b.x0, -10, 0.1) && near(b.y0, -10, 0.1) && near(b.x1, 10, 0.1) && near(b.y1, 10, 0.1));
    b = bounds(flattenPath("M10 0 A10 10 0 0 1 0 10"));
    CHECK(near(b.x1, 10, 1e-9) && near(b.y1, 10, 1e-9) && b.x0 >= -1e-9);

    // curves: cubic bulge, S reflects the previous control point, Q/T likewise
    b = bounds(flattenPath("M0 0 C0 10 10 10 10 0"));
    CHECK(near(b.y1, 7.5, 0.15));
    b = bounds(flattenPath("M0 0 C0 10 10 10 10 0 S20 -10 20 0"));
    CHECK(near(b.y0, -7.5, 0.15) && near(b.x1, 20, 1e-9));
    b = bounds(flattenPath("M0 0 Q5 10 10 0 T20 0"));
    CHECK(near(b.y1, 5, 0.05) && near(b.y0, -5, 0.05));
    b = bounds(flattenPath("M0 0 q5 10 10 0 t10 0"));
    CHECK(near(b.y0, -5, 0.05) && near(b.x1, 20, 1e-9));
    b = bounds(flattenPath("M0 0 c0 10 10 10 10 0 s10 -10 10 0"));
    CHECK(near(b.y0, -7.5, 0.15));

    // errors keep what parsed so far
    o = flattenPath("M0 0 L10 10 L10", 1.0, &err);
    CHECK(!err.empty());
    CHECK(o.size() == 1 && o[0].points.size() == 2);
    flattenPath("X 1 2", 1.0, &err);
    CHECK(!err.empty());
    flattenPath("10 10", 1.0, &err);
    CHECK(!err.empty());
    flattenPath("M 1e2 1E-1 L 0,0", 1.0, &err);
    CHECK(err.empty());
}

void testRendering() {
    VectorDoc doc;
    doc.width = 100;
    doc.height = 100;
    Shape r;
    r.kind = ShapeKind::Rect;
    r.x = 20, r.y = 20, r.w = 40, r.h = 40;
    r.fill = Color{255, 0, 0, 255};
    doc.add(r);
    raster::Image img = render(doc);
    CHECK(img.width() == 100 && img.height() == 100);
    CHECK(isColor(img.at(40, 40), 255, 0, 0, 0));
    CHECK(isColor(img.at(21, 21), 255, 0, 0, 0));
    CHECK(isColor(img.at(10, 10), 255, 255, 255, 0));
    CHECK(isColor(img.at(61, 61), 255, 255, 255, 0));
    int red = 0;
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) red += isColor(img.at(x, y), 255, 0, 0, 2);
    }
    CHECK(red >= 1560 && red <= 1640);  // 40x40 = 1600

    // scale renders bigger
    raster::Image big = render(doc, 2.0);
    CHECK(big.width() == 200 && isColor(big.at(80, 80), 255, 0, 0, 0) && isColor(big.at(30, 30), 255, 255, 255, 0));

    // even-odd hole
    doc.shapes.clear();
    Shape p;
    p.kind = ShapeKind::Path;
    p.d = "M10 10 h80 v80 h-80 z M30 30 h40 v40 h-40 z";
    p.fill = Color{0, 0, 255, 255};
    doc.add(p);
    img = render(doc);
    CHECK(isColor(img.at(20, 20), 0, 0, 255, 0));
    CHECK(isColor(img.at(50, 50), 255, 255, 255, 0));
    CHECK(isColor(img.at(80, 50), 0, 0, 255, 0));
    CHECK(isColor(img.at(5, 5), 255, 255, 255, 0));

    // two separate pieces in one path
    doc.shapes.clear();
    p.d = "M0 0 h20 v20 h-20 z M60 60 h20 v20 h-20 z";
    doc.add(p);
    img = render(doc);
    CHECK(isColor(img.at(10, 10), 0, 0, 255, 0) && isColor(img.at(70, 70), 0, 0, 255, 0) && isColor(img.at(40, 40), 255, 255, 255, 0));

    // stroke of a line
    doc.shapes.clear();
    Shape l;
    l.kind = ShapeKind::Line;
    l.points = {{0, 50}, {100, 50}};
    l.stroke = Color{0, 0, 0, 255};
    l.strokeWidth = 6;
    doc.add(l);
    img = render(doc);
    CHECK(isColor(img.at(50, 50), 0, 0, 0, 10));
    CHECK(isColor(img.at(50, 40), 255, 255, 255, 0));

    // outline only: stroke around, empty inside
    doc.shapes.clear();
    Shape e;
    e.kind = ShapeKind::Ellipse;
    e.cx = 50, e.cy = 50, e.rx = 30, e.ry = 30;
    e.stroke = Color{0, 128, 0, 255};
    e.strokeWidth = 4;
    doc.add(e);
    img = render(doc);
    CHECK(isColor(img.at(80, 50), 0, 128, 0, 10));
    CHECK(isColor(img.at(50, 50), 255, 255, 255, 0));

    // opacity
    doc.shapes.clear();
    r.opacity = 0.5;
    doc.add(r);
    img = render(doc);
    CHECK(isColor(img.at(40, 40), 255, 128, 128, 3));

    // transforms: move, rotate a bar 90 degrees around its centre, hide
    doc.shapes.clear();
    r.opacity = 1;
    r.transform.tx = 30;
    doc.add(r);
    img = render(doc);
    CHECK(isColor(img.at(30, 40), 255, 255, 255, 0) && isColor(img.at(70, 40), 255, 0, 0, 0));

    doc.shapes.clear();
    Shape bar;
    bar.kind = ShapeKind::Rect;
    bar.x = 10, bar.y = 45, bar.w = 80, bar.h = 10;  // horizontal bar centred at 50,50
    bar.fill = Color{0, 0, 0, 255};
    bar.transform.rotate = 90;
    doc.add(bar);
    img = render(doc);
    CHECK(isColor(img.at(50, 15), 0, 0, 0, 0));        // now vertical
    CHECK(isColor(img.at(15, 50), 255, 255, 255, 0));  // no longer horizontal
    Bounds bb = bounds(outline(doc.shapes[0]));
    CHECK(near(bb.x0, 45, 1e-6) && near(bb.y0, 10, 1e-6));

    doc.shapes[0].transform = {};
    doc.shapes[0].transform.sx = 0.5;
    bb = bounds(outline(doc.shapes[0]));
    CHECK(near(bb.x0, 30, 1e-6) && near(bb.x1, 70, 1e-6) && near(bb.y0, 45, 1e-6));

    doc.shapes[0].visible = false;
    img = render(doc);
    CHECK(isColor(img.at(50, 50), 255, 255, 255, 0));

    // rounded rect leaves the corner empty
    doc.shapes.clear();
    Shape rr = r;
    rr.transform = {};
    rr.x = 0, rr.y = 0, rr.w = 100, rr.h = 100, rr.radius = 30;
    doc.add(rr);
    img = render(doc);
    CHECK(isColor(img.at(1, 1), 255, 255, 255, 0) && isColor(img.at(50, 50), 255, 0, 0, 0) && isColor(img.at(50, 1), 255, 0, 0, 0));
}

void testSvgRoundTrip() {
    VectorDoc doc;
    doc.width = 120;
    doc.height = 80;
    doc.background = {10, 20, 30, 255};
    Shape r;
    r.kind = ShapeKind::Rect;
    r.x = 5, r.y = 6, r.w = 50, r.h = 30, r.radius = 4;
    r.fill = Color{255, 0, 0, 128};
    r.stroke = Color{0, 0, 255, 255};
    r.strokeWidth = 3;
    r.name = "box & <friends>";
    r.transform.tx = 10, r.transform.ty = -2, r.transform.rotate = 30, r.transform.sx = 1.5, r.transform.sy = 0.5;
    doc.add(r);
    Shape e;
    e.kind = ShapeKind::Ellipse;
    e.cx = 60, e.cy = 40, e.rx = 20, e.ry = 10;
    e.fill = Color{0, 255, 0, 255};
    e.opacity = 0.25;
    doc.add(e);
    Shape c = e;
    c.ry = 20;
    c.visible = false;
    doc.add(c);
    Shape pg;
    pg.kind = ShapeKind::Polygon;
    pg.points = {{1, 1}, {20, 1}, {10.5, 15.25}};
    pg.fill = Color{1, 2, 3, 255};
    doc.add(pg);
    Shape pl = pg;
    pl.kind = ShapeKind::Polyline;
    pl.fill.reset();
    pl.stroke = Color{9, 9, 9, 255};
    doc.add(pl);
    Shape ln;
    ln.kind = ShapeKind::Line;
    ln.points = {{0, 0}, {100, 70}};
    ln.stroke = Color{200, 100, 0, 255};
    ln.strokeWidth = 2.5;
    doc.add(ln);
    Shape path;
    path.kind = ShapeKind::Path;
    path.d = "M10 10 C 20 30 40 30 50 10 A 5 5 0 0 1 60 10 Z";
    path.fill = Color{50, 60, 70, 255};
    doc.add(path);

    std::string svg = toSvg(doc);
    CHECK(svg.find("<svg xmlns=\"http://www.w3.org/2000/svg\"") != std::string::npos);
    CHECK(svg.find("<circle") != std::string::npos);
    CHECK(svg.find("box &amp; &lt;friends&gt;") != std::string::npos);

    auto back = fromSvg(svg);
    CHECK(back->width == 120 && back->height == 80);
    CHECK(back->background == doc.background);
    CHECK(back->shapes.size() == doc.shapes.size());
    if (back->shapes.size() == doc.shapes.size()) {
        for (std::size_t i = 0; i < doc.shapes.size(); ++i) {
            const Shape& a = doc.shapes[i];
            const Shape& b = back->shapes[i];
            CHECK(a.kind == b.kind);
            CHECK(a.id == b.id);
            CHECK(a.name == b.name);
            CHECK(a.fill == b.fill);
            CHECK(a.stroke == b.stroke);
            CHECK(a.visible == b.visible);
            CHECK(near(a.opacity, b.opacity, 1e-3));
            if (a.stroke) CHECK(near(a.strokeWidth, b.strokeWidth, 1e-3));
            CHECK(near(a.transform.tx, b.transform.tx, 1e-3) && near(a.transform.ty, b.transform.ty, 1e-3));
            CHECK(near(a.transform.rotate, b.transform.rotate, 1e-3));
            CHECK(near(a.transform.sx, b.transform.sx, 1e-3) && near(a.transform.sy, b.transform.sy, 1e-3));
        }
        CHECK(near(back->shapes[0].radius, 4) && near(back->shapes[0].w, 50));
        CHECK(back->shapes[3].points.size() == 3 && near(back->shapes[3].points[2].y, 15.25));
        CHECK(back->shapes[6].d == path.d);
    }
    // same pixels
    raster::Image a = render(doc), b = render(*back);
    int maxDiff = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            auto p = a.at(x, y), q = b.at(x, y);
            maxDiff = std::max({maxDiff, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b)});
        }
    }
    CHECK(maxDiff <= 8);

    // to disk and back
    auto file = tmp("roundtrip.svg");
    saveDoc(doc, file);
    auto loaded = openSvg(file);
    CHECK(loaded->shapes.size() == doc.shapes.size());
    CHECK(loaded->path == file.string());
    std::filesystem::remove(file);
}

void testSvgImport() {
    const std::string svg = R"svg(<?xml version="1.0"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<!-- a comment <rect/> -->
<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     width="200mm" height="100mm" viewBox="0 0 200 100">
  <defs><linearGradient id="g"><stop offset="0"/></linearGradient><rect id="hidden-in-defs" width="5" height="5"/></defs>
  <title>test</title>
  <g fill="red" transform="translate(10,20)" opacity="0.5">
    <rect id="r1" width="10" height="10" style="stroke: blue; stroke-width: 2px"/>
    <circle inkscape:label="dot" cx="50" cy="50" r="5" fill="none" stroke="#0f0"/>
  </g>
  <unknownthing><rect width="1" height="1"/></unknownthing>
  <ellipse cx="100" cy="50" rx="20" ry="10" style="fill:#00ff0080;fill-opacity:0.5" transform="rotate(45 100 50)"/>
  <line x1="0" y1="0" x2="10" y2="10" stroke="black"/>
  <polyline points="0,0 10,10 20,0" fill="none" stroke="black"/>
  <polygon points="0 0, 10 0, 5 5"/>
  <path d="M0 0 L10 10" stroke="black" display="none"/>
  <svg:rect xmlns:svg="http://www.w3.org/2000/svg" x="1" y="1" width="2" height="2" fill="url(#g)"/>
  <rect width="0" height="5"/>
</svg>)svg";
    auto doc = fromSvg(svg);
    CHECK(doc->width == 756 && doc->height == 378);  // 200mm at 96 dpi
    CHECK(doc->background.a == 0);
    // the <g> becomes a group that owns the opacity and translate; its shapes inherit the fill
    CHECK(doc->shapes.size() == 9);
    if (doc->shapes.size() != 9) return;
    const Shape& g = doc->shapes[0];
    CHECK(g.kind == ShapeKind::Group && near(g.opacity, 0.5));
    const Shape& r = doc->shapes[1];
    CHECK(r.kind == ShapeKind::Rect && r.name == "r1" && r.parent == g.id);
    CHECK(r.fill == Color{255, 0, 0, 255});
    CHECK(r.stroke == Color{0, 0, 255, 255});
    CHECK(near(r.strokeWidth, 2));
    // viewBox scale 3.78 and the group's translate both end up in the outline on the canvas
    Bounds b = bounds(outlineIn(*doc, 1));
    CHECK(near(b.x0, 10 * 3.7795, 0.01) && near(b.y0, 20 * 3.7795, 0.01) && near(b.width(), 37.795, 0.01));
    const Shape& dot = doc->shapes[2];
    CHECK(dot.kind == ShapeKind::Ellipse && dot.name == "dot" && !dot.fill && dot.stroke == Color{0, 255, 0, 255});
    const Shape& el = doc->shapes[3];
    CHECK(el.kind == ShapeKind::Ellipse && near(el.transform.rotate, 45, 1e-6));
    CHECK(el.fill && el.fill->a == 64);
    CHECK(doc->shapes[4].kind == ShapeKind::Line && !doc->shapes[4].fill);
    CHECK(doc->shapes[5].kind == ShapeKind::Polyline && doc->shapes[5].points.size() == 3);
    CHECK(doc->shapes[6].kind == ShapeKind::Polygon && doc->shapes[6].fill == Color{0, 0, 0, 255});
    CHECK(doc->shapes[7].kind == ShapeKind::Path && !doc->shapes[7].visible);
    CHECK(doc->shapes[8].kind == ShapeKind::Rect && doc->shapes[8].fill == Color{128, 128, 128, 255});

    bool threw = false;
    try {
        fromSvg("<html><body/></html>");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void testCommandsAndUndo() {
    Session session;
    registerCommands(session);

    Result r = session.run("vector.new", {{"width", std::int64_t{100}}, {"height", std::int64_t{100}}});
    CHECK(r.ok);
    CHECK(r.field("doc") && toText(*r.field("doc")) == "vec1");
    auto doc = session.workspace().getAs<VectorDoc>("vec1");
    CHECK(doc && doc->width == 100 && doc->background == Color{255, 255, 255, 255});

    r = session.run("vector.add_rect", {{"doc", std::string("vec1")}, {"x", 20.0}, {"y", std::int64_t{20}}, {"width", 40.0},
                                        {"height", 40.0}, {"fill", std::string("red")}, {"preview", true}});
    CHECK(r.ok);
    CHECK(r.field("shape") && toText(*r.field("shape")) == "s1");
    CHECK(r.attachments.size() == 1 && r.attachments[0].mime == "image/png" && r.attachments[0].bytes.rfind("\x89PNG", 0) == 0);
    CHECK(doc->shapes.size() == 1 && doc->shapes[0].fill == Color{255, 0, 0, 255} && !doc->shapes[0].stroke);

    r = session.run("vector.add_ellipse", {{"doc", std::string("vec1")}, {"cx", 50.0}, {"cy", 50.0}, {"rx", 10.0},
                                           {"name", std::string("sun")}, {"fill", std::string("#ffcc00")}});
    CHECK(r.ok && toText(*r.field("shape")) == "s2");
    CHECK(near(doc->shapes[1].ry, 10));

    // undo / redo
    CHECK(session.history().undo());
    CHECK(doc->shapes.size() == 1);
    CHECK(session.history().redo());
    CHECK(doc->shapes.size() == 2 && doc->shapes[1].name == "sun");

    // style by name, transform, undo restores
    r = session.run("vector.style", {{"doc", std::string("vec1")}, {"shape", std::string("sun")}, {"stroke", std::string("black")},
                                     {"stroke_width", 3.0}, {"fill", std::string("none")}});
    CHECK(r.ok && !doc->shapes[1].fill && doc->shapes[1].stroke && near(doc->shapes[1].strokeWidth, 3));
    session.history().undo();
    CHECK(doc->shapes[1].fill && !doc->shapes[1].stroke);

    r = session.run("vector.transform", {{"doc", std::string("vec1")}, {"shape", std::string("s1")}, {"dx", 5.0}, {"rotate", 45.0},
                                         {"scale", 2.0}});
    CHECK(r.ok && near(doc->shapes[0].transform.tx, 5) && near(doc->shapes[0].transform.rotate, 45) &&
          near(doc->shapes[0].transform.sx, 2));
    r = session.run("vector.transform", {{"doc", std::string("vec1")}, {"shape", std::string("s1")}, {"reset", true}});
    CHECK(r.ok && doc->shapes[0].transform.identity());
    session.history().undo();
    CHECK(near(doc->shapes[0].transform.rotate, 45));
    session.history().undo();
    CHECK(doc->shapes[0].transform.identity());

    // bad input fails cleanly and leaves no history step
    auto steps = session.history().labels().size();
    CHECK(!session.run("vector.style", {{"doc", std::string("vec1")}, {"shape", std::string("s99")}}).ok);
    CHECK(!session.run("vector.style", {{"doc", std::string("vec1")}, {"shape", std::string("s1")}, {"fill", std::string("blurple")}}).ok);
    CHECK(!session.run("vector.add_rect", {{"doc", std::string("nope")}, {"x", 0.0}, {"y", 0.0}, {"width", 1.0}, {"height", 1.0}}).ok);
    CHECK(!session.run("vector.add_path", {{"doc", std::string("vec1")}, {"d", std::string("M0 0 L")}}).ok);
    CHECK(!session.run("vector.add_polygon", {{"doc", std::string("vec1")}, {"points", NumberList{0, 0, 1, 1}}}).ok);
    CHECK(!session.run("vector.reorder", {{"doc", std::string("vec1")}, {"shape", std::string("s1")}, {"to", std::string("sideways")}}).ok);
    CHECK(session.history().labels().size() == steps);
    CHECK(doc->shapes.size() == 2);

    // more shapes
    CHECK(session.run("vector.add_line", {{"doc", std::string("vec1")}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 100.0}, {"y2", 100.0}}).ok);
    CHECK(doc->shapes[2].stroke && near(doc->shapes[2].strokeWidth, 2));
    r = session.run("vector.add_polygon", {{"doc", std::string("vec1")}, {"points", NumberList{10, 10, 90, 10, 50, 80}}});
    CHECK(r.ok && doc->shapes[3].points.size() == 3);
    CHECK(session.run("vector.add_polyline", {{"doc", std::string("vec1")}, {"points", NumberList{0, 0, 50, 50, 100, 0}}}).ok);
    r = session.run("vector.add_path", {{"doc", std::string("vec1")}, {"d", std::string("M10 10 Q50 90 90 10 Z")}, {"fill", std::string("blue")}});
    CHECK(r.ok && toText(*r.field("shape")) == "s6");

    // reorder: s6 to back, s1 to front
    CHECK(session.run("vector.reorder", {{"doc", std::string("vec1")}, {"shape", std::string("s6")}, {"to", std::string("back")}}).ok);
    CHECK(doc->shapes.front().id == "s6");
    CHECK(session.run("vector.reorder", {{"doc", std::string("vec1")}, {"shape", std::string("s1")}, {"to", std::string("front")}}).ok);
    CHECK(doc->shapes.back().id == "s1");
    CHECK(session.run("vector.reorder", {{"doc", std::string("vec1")}, {"shape", std::string("s1")}, {"to", std::string("down")}}).ok);
    CHECK(doc->shapes[doc->shapes.size() - 2].id == "s1");
    session.history().undo();
    CHECK(doc->shapes.back().id == "s1");

    // duplicate goes just above the original
    r = session.run("vector.duplicate", {{"doc", std::string("vec1")}, {"shape", std::string("s6")}, {"dx", 10.0}});
    CHECK(r.ok && toText(*r.field("shape")) == "s7");
    CHECK(doc->shapes[1].id == "s7" && near(doc->shapes[1].transform.tx, 10) && doc->shapes[1].name == "path 6 copy");

    // delete + undo
    auto count = doc->shapes.size();
    CHECK(session.run("vector.delete", {{"doc", std::string("vec1")}, {"shape", std::string("s7")}}).ok);
    CHECK(doc->shapes.size() == count - 1 && doc->find("s7") < 0);
    session.history().undo();
    CHECK(doc->find("s7") == 1);

    // align
    r = session.run("vector.align", {{"doc", std::string("vec1")}, {"shape", std::string("sun")}, {"horizontal", std::string("right")},
                                     {"vertical", std::string("top")}});
    CHECK(r.ok);
    Bounds b = bounds(outline(doc->shapes[static_cast<std::size_t>(doc->find("sun"))]));
    CHECK(near(b.x1, 100, 1e-6) && near(b.y0, 0, 1e-6));

    // canvas
    CHECK(session.run("vector.canvas", {{"doc", std::string("vec1")}, {"width", std::int64_t{200}}, {"background", std::string("none")}}).ok);
    CHECK(doc->width == 200 && doc->height == 100 && doc->background.a == 0);
    session.history().undo();
    CHECK(doc->width == 100 && doc->background.a == 255);

    // info, preview
    r = session.run("vector.info", {{"doc", std::string("vec1")}});
    CHECK(r.ok && r.message.find("s1 rect") != std::string::npos && r.message.find("\"sun\"") != std::string::npos);
    CHECK(r.field("shapes") && std::get<std::int64_t>(*r.field("shapes")) == static_cast<std::int64_t>(doc->shapes.size()));
    r = session.run("vector.preview", {{"doc", std::string("vec1")}, {"size", std::int64_t{256}}});
    CHECK(r.ok && r.attachments.size() == 1);
    raster::Image prev = raster::decode(r.attachments[0].bytes);
    CHECK(prev.width() == 256 && prev.height() == 256);
    CHECK(doc->preview() != nullptr);

    // save svg + png, reopen
    auto svgFile = tmp("cmd.svg"), pngFile = tmp("cmd.png");
    CHECK(session.run("vector.save", {{"doc", std::string("vec1")}, {"path", svgFile.string()}}).ok);
    CHECK(doc->path == svgFile.string());
    CHECK(session.run("vector.save", {{"doc", std::string("vec1")}, {"path", pngFile.string()}, {"scale", 2.0}}).ok);
    raster::Image png = raster::load(pngFile);
    CHECK(png.width() == 200 && png.height() == 200);
    CHECK(!session.run("vector.save", {{"doc", std::string("vec1")}, {"path", tmp("cmd.xyz").string()}}).ok);
    r = session.run("vector.open", {{"path", svgFile.string()}});
    CHECK(r.ok && toText(*r.field("doc")) == "vec2");
    auto reopened = session.workspace().getAs<VectorDoc>("vec2");
    CHECK(reopened && reopened->shapes.size() == doc->shapes.size());
    CHECK(!session.run("vector.open", {{"path", tmp("missing.svg").string()}}).ok);
    auto convFile = tmp("conv.bmp");
    r = session.run("vector.convert", {{"input", svgFile.string()}, {"output", convFile.string()}, {"scale", 0.5}});
    CHECK(r.ok);
    CHECK(raster::load(convFile).width() == 50);
    std::filesystem::remove(convFile);
    std::filesystem::remove(svgFile);
    std::filesystem::remove(pngFile);

    // close is undoable
    CHECK(session.run("vector.close", {{"doc", std::string("vec2")}}).ok);
    CHECK(!session.workspace().get("vec2"));
    session.history().undo();
    CHECK(session.workspace().getAs<VectorDoc>("vec2") != nullptr);
}

void testBatch() {
    Session session;
    registerCommands(session);
    cli::Batch batch(session);
    std::istringstream script(R"(# a tiny logo
vector.new width=200 height=100 background=#102030 -> logo
vector.add_rect doc=$logo x=10 y=10 width=60 height=40 radius=8 fill=orange -> box
vector.add_polygon doc=$logo points=100,90,150,10,190,90 fill=none stroke=white stroke_width=4 -> tri
vector.add_path doc=$logo d="M10 90 C 40 60, 60 60, 90 90" fill=none stroke=#ffffff stroke_width=3
vector.style doc=$logo shape=$box opacity=0.8
vector.transform doc=$logo shape=$tri rotate=10
undo
vector.info doc=$logo
)");
    std::ostringstream out, err;
    int failures = batch.run(script, out, err);
    CHECK(failures == 0);
    if (failures) std::printf("%s\n", err.str().c_str());
    CHECK(batch.vars().at("logo") == "vec1");
    CHECK(batch.vars().at("box") == "s1");
    CHECK(batch.vars().at("tri") == "s2");
    auto doc = session.workspace().getAs<VectorDoc>("vec1");
    CHECK(doc && doc->shapes.size() == 3);
    CHECK(doc && near(doc->shapes[0].opacity, 0.8) && doc->shapes[1].transform.identity());
    CHECK(doc && doc->background == Color{0x10, 0x20, 0x30, 255});
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server
            .handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}")
            .value_or("");
    };
    std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    CHECK(list.find("\"vector_add_rect\"") != std::string::npos);
    CHECK(list.find("\"vector_reorder\"") != std::string::npos);
    std::string r = call("vector_new", R"({"width":64,"height":64,"background":"none"})");
    CHECK(r.find("\"doc\":\"vec1\"") != std::string::npos);
    r = call("vector_add_polygon", R"({"doc":"vec1","points":[2,2,60,2,30,60],"fill":"#ff0000","preview":true})");
    CHECK(r.find("\"shape\":\"s1\"") != std::string::npos);
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("vector_transform", R"({"doc":"vec1","shape":"s1","rotate":90})");
    CHECK(r.find("\"isError\":false") != std::string::npos);
    r = server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"undo"}})").value_or("");
    CHECK(r.find("undone") != std::string::npos);
    CHECK(session.workspace().getAs<VectorDoc>("vec1")->shapes[0].transform.identity());
}


// Area inside an outline (even-odd), sampled on a 0.25 px grid over [x0,x1]x[y0,y1].
double sampledArea(const Outline& o, double x0, double y0, double x1, double y1) {
    double area = 0;
    const double step = 0.25;
    for (double y = y0 + step / 2; y < y1; y += step)
        for (double x = x0 + step / 2; x < x1; x += step) {
            bool in = false;
            for (const auto& c : o)
                for (std::size_t i = 0, j = c.points.size() - 1; i < c.points.size(); j = i++) {
                    const Point& a = c.points[i];
                    const Point& b = c.points[j];
                    if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
                }
            if (in) area += step * step;
        }
    return area;
}

void testTextGradientBoolean() {
    Session session;
    registerCommands(session);
    const Value vec = std::string("vec1");
    CHECK(session.run("vector.new", {{"width", std::int64_t{400}}, {"height", std::int64_t{200}}}).ok);
    auto doc = session.workspace().getAs<VectorDoc>("vec1");

    // text with the built-in font: real glyph outlines, sized by the font size
    Result r = session.run("vector.add_text", {{"doc", vec}, {"text", std::string("Hello")}, {"x", 20.0}, {"y", 100.0}, {"size", 60.0}});
    CHECK(r.ok && toText(*r.field("shape")) == "s1");
    const Shape& t = doc->shapes[0];
    CHECK(t.kind == ShapeKind::Text && t.fill == Color{0, 0, 0, 255} && !t.stroke);
    Bounds b = bounds(outline(t));
    CHECK(!b.empty && b.x0 >= 19 && b.x0 < 30 && b.y1 <= 101 && b.y1 > 95);  // H sits on the baseline
    CHECK(b.height() > 35 && b.height() < 50);                              // cap height ~0.7 em
    const double width = b.width();
    CHECK(width > 100 && width < 200);
    raster::Image img = render(*doc);
    int dark = 0;
    for (int y = 50; y < 100; ++y)
        for (int x = 20; x < 200; ++x) dark += img.at(x, y).r < 100;
    CHECK(dark > 500);

    // anchors and multi-line text
    r = session.run("vector.set_text", {{"doc", vec}, {"shape", std::string("s1")}, {"anchor", std::string("center")}, {"x", 200.0}});
    CHECK(r.ok && doc->shapes[0].anchor == "middle");
    b = bounds(outline(doc->shapes[0]));
    CHECK(near((b.x0 + b.x1) / 2, 200, 3));
    r = session.run("vector.set_text", {{"doc", vec}, {"shape", std::string("s1")}, {"text", std::string("Hello\\nworld")}});
    CHECK(r.ok && doc->shapes[0].text == "Hello\nworld");
    CHECK(bounds(outline(doc->shapes[0])).y1 > 150);  // second line is 1.2 em lower
    CHECK(session.history().undo() && doc->shapes[0].text == "Hello");
    CHECK(!session.run("vector.set_text", {{"doc", vec}, {"shape", std::string("s1")}, {"anchor", std::string("diagonal")}}).ok);
    // unicode beyond Latin-1 still has glyphs
    CHECK(!textPathData(Shape{.kind = ShapeKind::Text, .text = "\xd0\x9f\xce\xa9"}).empty());

    // text survives an SVG round trip as text
    std::string svg = toSvg(*doc);
    CHECK(svg.find("<text") != std::string::npos && svg.find("Hello") != std::string::npos);
    auto back = fromSvg(svg);
    CHECK(back->shapes.size() == 1 && back->shapes[0].kind == ShapeKind::Text && back->shapes[0].text == "Hello" &&
          back->shapes[0].anchor == "middle" && near(back->shapes[0].fontSize, 60));

    // to_path keeps the look
    Bounds before = bounds(outline(doc->shapes[0]));
    r = session.run("vector.to_path", {{"doc", vec}, {"shape", std::string("s1")}});
    CHECK(r.ok && doc->shapes[0].kind == ShapeKind::Path && doc->shapes[0].d.find('Q') != std::string::npos);
    Bounds after = bounds(outline(doc->shapes[0]));
    CHECK(near(before.x0, after.x0, 0.5) && near(before.y1, after.y1, 0.5) && near(before.width(), after.width(), 0.5));
    session.history().undo();

    // gradients
    r = session.run("vector.add_rect", {{"doc", vec}, {"x", 0.0}, {"y", 0.0}, {"width", 400.0}, {"height", 20.0}, {"fill", std::string("gray")}});
    CHECK(r.ok);
    r = session.run("vector.gradient", {{"doc", vec}, {"shape", std::string("s2")}, {"colors", std::string("red,rgb(0,0,255)")}});
    CHECK(r.ok && doc->shapes[1].gradient && doc->shapes[1].gradient->stops.size() == 2);
    img = render(*doc);
    CHECK(isColor(img.at(2, 10), 253, 0, 2, 10) && isColor(img.at(397, 10), 2, 0, 253, 10));
    CHECK(isColor(img.at(200, 10), 128, 0, 128, 12));
    r = session.run("vector.gradient", {{"doc", vec}, {"shape", std::string("s2")}, {"stops", std::string("0:white,1:black")}, {"angle", 90.0}});
    CHECK(r.ok);
    img = render(*doc);
    CHECK(img.at(200, 1).r > img.at(200, 18).r + 150);
    r = session.run("vector.gradient", {{"doc", vec}, {"shape", std::string("s2")}, {"type", std::string("radial")}, {"colors", std::string("white,black")}});
    CHECK(r.ok && doc->shapes[1].gradient->radial);
    svg = toSvg(*doc);
    CHECK(svg.find("<radialGradient") != std::string::npos && svg.find("url(#") != std::string::npos);
    back = fromSvg(svg);
    CHECK(back->shapes.size() == 2 && back->shapes[1].gradient && back->shapes[1].gradient->radial && back->shapes[1].gradient->stops.size() == 2);
    CHECK(!session.run("vector.gradient", {{"doc", vec}, {"shape", std::string("s2")}, {"colors", std::string("red,notacolour")}}).ok);
    CHECK(session.run("vector.gradient", {{"doc", vec}, {"shape", std::string("s2")}, {"remove", true}}).ok && !doc->shapes[1].gradient);

    // booleans on two overlapping 10x10 squares
    struct Case {
        const char* op;
        double area;
        std::size_t contours;
    };
    for (Case c : {Case{"union", 150, 1}, Case{"subtract", 50, 1}, Case{"intersect", 50, 1}, Case{"exclude", 100, 2}}) {
        Session s2;
        registerCommands(s2);
        CHECK(s2.run("vector.new", {{"width", std::int64_t{40}}, {"height", std::int64_t{40}}}).ok);
        auto d2 = s2.workspace().getAs<VectorDoc>("vec1");
        s2.run("vector.add_rect", {{"doc", vec}, {"x", 0.0}, {"y", 0.0}, {"width", 10.0}, {"height", 10.0}, {"fill", std::string("red")}});
        s2.run("vector.add_rect", {{"doc", vec}, {"x", 5.0}, {"y", 0.0}, {"width", 10.0}, {"height", 10.0}, {"fill", std::string("blue")}});
        r = s2.run("vector.boolean", {{"doc", vec}, {"op", std::string(c.op)}, {"shapes", std::string("s1, s2")}});
        CHECK(r.ok);
        CHECK(d2->shapes.size() == 1 && d2->shapes[0].kind == ShapeKind::Path && d2->shapes[0].fill == Color{255, 0, 0, 255});
        Outline o = outline(d2->shapes[0]);
        CHECK(o.size() == c.contours);
        double area = sampledArea(o, -1, -1, 20, 12);
        if (!near(area, c.area, 1)) std::printf("  %s area %.2f, wanted %.0f\n", c.op, area, c.area);
        CHECK(near(area, c.area, 1));
        CHECK(s2.history().undo() && d2->shapes.size() == 2);
    }

    // a hole: big square minus a small one inside, and keep=true leaves the originals
    {
        Session s3;
        registerCommands(s3);
        s3.run("vector.new", {});
        auto d3 = s3.workspace().getAs<VectorDoc>("vec1");
        s3.run("vector.add_rect", {{"doc", vec}, {"x", 0.0}, {"y", 0.0}, {"width", 20.0}, {"height", 20.0}});
        s3.run("vector.add_ellipse", {{"doc", vec}, {"cx", 10.0}, {"cy", 10.0}, {"rx", 5.0}});
        r = s3.run("vector.boolean", {{"doc", vec}, {"op", std::string("subtract")}, {"shapes", std::string("s1,s2")}, {"keep", true},
                                      {"name", std::string("washer")}});
        CHECK(r.ok && d3->shapes.size() == 3 && d3->shapes[2].name == "washer");
        Outline o = outline(d3->shapes[2]);
        CHECK(o.size() == 2);
        CHECK(near(sampledArea(o, -1, -1, 21, 21), 400 - 3.14159 * 25, 2));
        // a rotated shape is combined where it's drawn
        s3.run("vector.transform", {{"doc", vec}, {"shape", std::string("s1")}, {"rotate", 45.0}});
        r = s3.run("vector.boolean", {{"doc", vec}, {"op", std::string("intersect")}, {"shapes", std::string("s1,s2")}});
        CHECK(r.ok && near(sampledArea(outline(d3->shapes[static_cast<std::size_t>(d3->find(toText(*r.field("shape"))))]), -5, -5, 25, 25), 3.14159 * 25, 2));
        CHECK(!s3.run("vector.boolean", {{"doc", vec}, {"op", std::string("melt")}, {"shapes", std::string("s1,s2")}}).ok);
        CHECK(!s3.run("vector.boolean", {{"doc", vec}, {"op", std::string("union")}, {"shapes", std::string("washer")}}).ok);
    }
}

}  // namespace

void runToolTests();  // test_vector_tools.cpp

int main() {
    testPathParsing();
    testRendering();
    testSvgRoundTrip();
    testSvgImport();
    testCommandsAndUndo();
    testTextGradientBoolean();
    testBatch();
    testMcp();
    runToolTests();
    return check::summary("vector");
}
