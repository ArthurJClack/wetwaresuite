// Brush settings and the retouching brushes, checked pixel by pixel.
#include "check.hpp"
#include "wetwaresuite/paint/paint.hpp"
#include "wetwaresuite/photo/photo.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/paint.hpp"

using namespace wetwaresuite;
using raster::Color;

namespace {

Session* S = nullptr;
Result run(const std::string& id, Params p) { return S->run(id, p); }
std::int64_t I(int v) { return v; }
int lum(Color c) { return (c.r * 299 + c.g * 587 + c.b * 114) / 1000; }
int sat(Color c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }

// A fresh 80x40 canvas: left half black, right half white, on one layer.
std::string halves(std::shared_ptr<raster::ImageDoc>& doc) {
    const std::string d = toText(*run("photo.new", {{"width", I(80)}, {"height", I(40)}, {"background", std::string("#ffffff")}}).field("doc"));
    doc = S->workspace().getAs<raster::ImageDoc>(d);
    run("paint.shape", {{"doc", d}, {"kind", std::string("rect")}, {"x", 0.0}, {"y", 0.0}, {"width", 40.0}, {"height", 40.0}, {"color", std::string("#000000")}});
    return d;
}

}  // namespace

int main() {
    Session s;
    S = &s;
    photo::registerCommands(s);
    paint::registerCommands(s);
    std::shared_ptr<raster::ImageDoc> doc;

    // ---- clone stamp: paint the black half's pixels onto the white half ----
    {
        const std::string d = halves(doc);
        CHECK(run("paint.clone", {{"doc", d}, {"points", NumberList{60, 20, 70, 20}}, {"source_x", 20.0}, {"source_y", 20.0},
                                  {"size", 10.0}, {"hardness", 100.0}}).ok);
        CHECK(doc->activeLayer().image.at(65, 20) == (Color{0, 0, 0, 255}));
        CHECK(doc->activeLayer().image.at(65, 32) == (Color{255, 255, 255, 255}));
        CHECK(s.history().undo() && doc->activeLayer().image.at(65, 20) == (Color{255, 255, 255, 255}));
        // copy from what you see: a top layer's pixels clone onto the bottom layer
        CHECK(run("paint.layer_add", {{"doc", d}}).ok);
        CHECK(run("paint.shape", {{"doc", d}, {"kind", std::string("rect")}, {"x", 50.0}, {"y", 0.0}, {"width", 5.0}, {"height", 5.0}, {"color", std::string("#ff0000")}}).ok);
        CHECK(run("paint.clone", {{"doc", d}, {"layer", I(0)}, {"points", NumberList{60, 30}}, {"source_x", 52.0}, {"source_y", 2.0},
                                  {"size", 3.0}, {"hardness", 100.0}, {"sample", std::string("image")}}).ok);
        CHECK(doc->image.layers[0].image.at(60, 30) == (Color{255, 0, 0, 255}));
    }

    // ---- dodge / burn / sponge ----
    {
        const std::string d = toText(*run("photo.new", {{"width", I(40)}, {"height", I(20)}, {"background", std::string("#808080")}}).field("doc"));
        doc = s.workspace().getAs<raster::ImageDoc>(d);
        CHECK(run("paint.dodge", {{"doc", d}, {"points", NumberList{5, 10, 15, 10}}, {"size", 8.0}, {"hardness", 100.0}, {"exposure", 50.0}}).ok);
        CHECK(lum(doc->activeLayer().image.at(10, 10)) > 160 && doc->activeLayer().image.at(10, 2) == (Color{128, 128, 128, 255}));
        CHECK(run("paint.dodge", {{"doc", d}, {"tool", std::string("burn")}, {"points", NumberList{25, 10, 35, 10}}, {"size", 8.0}, {"hardness", 100.0}}).ok);
        CHECK(lum(doc->activeLayer().image.at(30, 10)) < 90);
        // burn on shadows darkens dark grey more than on highlights
        CHECK(run("paint.clear", {{"doc", d}, {"color", std::string("#c06040")}}).ok);
        CHECK(run("paint.dodge", {{"doc", d}, {"tool", std::string("sponge")}, {"points", NumberList{5, 10, 15, 10}}, {"size", 8.0}, {"hardness", 100.0},
                                  {"exposure", 100.0}}).ok);
        CHECK(sat(doc->activeLayer().image.at(10, 10)) < sat(Color{0xc0, 0x60, 0x40, 255}) / 3);
        CHECK(run("paint.dodge", {{"doc", d}, {"tool", std::string("sponge")}, {"sponge_mode", std::string("saturate")}, {"points", NumberList{25, 10, 35, 10}},
                                  {"size", 8.0}, {"hardness", 100.0}}).ok);
        CHECK(sat(doc->activeLayer().image.at(30, 10)) > sat(Color{0xc0, 0x60, 0x40, 255}));
    }

    // ---- smudge drags black into the white half ----
    {
        const std::string d = halves(doc);
        CHECK(run("paint.smudge", {{"doc", d}, {"points", NumberList{35, 20, 50, 20}}, {"size", 10.0}, {"hardness", 100.0}, {"strength", 80.0}}).ok);
        CHECK(lum(doc->activeLayer().image.at(45, 20)) < 200);
        CHECK(doc->activeLayer().image.at(45, 35) == (Color{255, 255, 255, 255}));
        CHECK(run("paint.smudge", {{"doc", d}, {"points", NumberList{70, 5, 75, 5}}, {"size", 4.0}, {"finger_paint", true}, {"color", std::string("#ff0000")},
                                   {"strength", 100.0}, {"hardness", 100.0}}).ok);
        CHECK(doc->activeLayer().image.at(72, 5).r > 200 && doc->activeLayer().image.at(72, 5).g < 100);
    }

    // ---- blur / sharpen brush ----
    {
        const std::string d = halves(doc);
        CHECK(run("paint.blur_brush", {{"doc", d}, {"points", NumberList{40, 15, 40, 25}}, {"size", 20.0}, {"hardness", 100.0}, {"strength", 100.0}}).ok);
        const int edge = lum(doc->activeLayer().image.at(39, 20));
        CHECK(edge > 20 && edge < 235);                                              // the hard edge is soft now
        CHECK(doc->activeLayer().image.at(39, 1) == (Color{0, 0, 0, 255}));          // but only under the brush
        CHECK(run("paint.clear", {{"doc", d}, {"color", std::string("#808080")}}).ok);
        CHECK(run("paint.shape", {{"doc", d}, {"kind", std::string("rect")}, {"x", 40.0}, {"y", 0.0}, {"width", 40.0}, {"height", 40.0}, {"color", std::string("#a0a0a0")}}).ok);
        CHECK(run("paint.blur_brush", {{"doc", d}, {"mode", std::string("sharpen")}, {"points", NumberList{40, 10, 40, 30}}, {"size", 20.0}, {"hardness", 100.0}}).ok);
        CHECK(lum(doc->activeLayer().image.at(39, 20)) < 0x80 && lum(doc->activeLayer().image.at(40, 20)) > 0xa0);  // more contrast at the edge
    }

    // ---- colour replacement keeps light and shade ----
    {
        const std::string d = toText(*run("photo.new", {{"width", I(40)}, {"height", I(20)}, {"background", std::string("#cc2020")}}).field("doc"));
        doc = s.workspace().getAs<raster::ImageDoc>(d);
        CHECK(run("paint.shape", {{"doc", d}, {"kind", std::string("rect")}, {"x", 20.0}, {"y", 0.0}, {"width", 20.0}, {"height", 20.0}, {"color", std::string("#ffffff")}}).ok);
        CHECK(run("paint.color_replace", {{"doc", d}, {"points", NumberList{5, 10, 35, 10}}, {"size", 10.0}, {"hardness", 100.0}, {"color", std::string("#0000ff")},
                                          {"tolerance", I(30)}}).ok);
        const Color c = doc->activeLayer().image.at(10, 10);
        CHECK(c.b > c.r && c.b > c.g);
        CHECK(std::abs(lum(c) - lum(Color{0xcc, 0x20, 0x20, 255})) < 12);
        CHECK(doc->activeLayer().image.at(30, 10) == (Color{255, 255, 255, 255}));  // white wasn't like the red
    }

    // ---- brush settings: scatter, tip, texture, symmetry ----
    {
        const std::string d = toText(*run("paint.new", {{"width", I(100)}, {"height", I(60)}, {"background", std::string("transparent")}}).field("doc"));
        doc = s.workspace().getAs<raster::ImageDoc>(d);
        auto img = [&] { return doc->activeLayer().image; };
        // symmetry: a stroke on the left also paints the mirror on the right
        CHECK(run("paint.stroke", {{"doc", d}, {"points", NumberList{10, 30, 20, 30}}, {"size", 6.0}, {"hardness", 100.0}, {"symmetry", std::string("vertical")}}).ok);
        CHECK(img().at(15, 30).a == 255 && img().at(85, 30).a == 255 && img().at(50, 30).a == 0);
        CHECK(run("paint.clear", {{"doc", d}}).ok);
        CHECK(run("paint.stroke", {{"doc", d}, {"points", NumberList{50, 10, 50, 12}}, {"size", 4.0}, {"hardness", 100.0}, {"symmetry", std::string("radial")},
                                   {"symmetry_count", I(4)}}).ok);
        CHECK(img().at(50, 11).a == 255 && img().at(50, 49).a == 255 && img().at(31, 30).a == 255 && img().at(69, 30).a == 255);
        // a square tip fills the corners a round tip leaves empty
        CHECK(run("paint.clear", {{"doc", d}}).ok);
        CHECK(run("paint.stroke", {{"doc", d}, {"points", NumberList{30, 30}}, {"size", 20.0}, {"tip", std::string("square")}}).ok);
        CHECK(img().at(21, 21).a > 200);
        CHECK(run("paint.clear", {{"doc", d}}).ok);
        CHECK(run("paint.stroke", {{"doc", d}, {"points", NumberList{30, 30}}, {"size", 20.0}, {"hardness", 100.0}}).ok);
        CHECK(img().at(21, 21).a == 0);
        // a tip made from another layer's pixels
        CHECK(run("paint.clear", {{"doc", d}}).ok);
        CHECK(run("paint.layer_add", {{"doc", d}, {"name", std::string("TipSrc")}}).ok);
        CHECK(run("paint.shape", {{"doc", d}, {"kind", std::string("rect")}, {"x", 0.0}, {"y", 0.0}, {"width", 10.0}, {"height", 2.0}}).ok);  // a flat bar
        CHECK(run("paint.stroke", {{"doc", d}, {"layer", I(1)}, {"points", NumberList{50, 30}}, {"size", 20.0}, {"tip", std::string("layer:2")}}).ok);
        CHECK(doc->image.layers[1].image.at(55, 30).a > 200 && doc->image.layers[1].image.at(50, 37).a == 0);
        // scatter throws dabs off the path; the same stroke scatters the same way every time
        CHECK(run("paint.clear", {{"doc", d}, {"layer", I(1)}}).ok);
        CHECK(run("paint.stroke", {{"doc", d}, {"layer", I(1)}, {"points", NumberList{10, 30, 90, 30}}, {"size", 4.0}, {"hardness", 100.0}, {"scatter", 300.0}}).ok);
        const raster::Image first = doc->image.layers[1].image;
        int off = 0;
        for (int x = 0; x < 100; ++x) {
            for (int y = 0; y < 60; ++y) off += (std::abs(y - 30) > 4 && first.at(x, y).a > 0);
        }
        CHECK(off > 20);
        CHECK(s.history().undo() && s.history().redo());
        CHECK(std::equal(first.data(), first.data() + first.bytes(), doc->image.layers[1].image.data()));
        // texture: a checker texture leaves gaps in the paint
        CHECK(run("paint.clear", {{"doc", d}, {"layer", I(1)}}).ok);
        CHECK(run("paint.stroke", {{"doc", d}, {"layer", I(1)}, {"points", NumberList{10, 30, 90, 30}}, {"size", 20.0}, {"hardness", 100.0},
                                   {"texture", std::string("checker")}, {"texture_depth", 100.0}}).ok);
        int low = 0, high = 0;
        for (int x = 15; x < 85; ++x) {
            const int a = doc->image.layers[1].image.at(x, 30).a;
            low += a < 160;
            high += a > 240;
        }
        CHECK(low > 10 && high > 10);
        CHECK(!run("paint.stroke", {{"doc", d}, {"points", NumberList{1, 1}}, {"tip", std::string("layer:9")}}).ok);
    }

    // ---- nonzero fill keeps holes of reversed outlines ----
    {
        using raster::Point;
        const std::vector<Point> outer = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};
        const std::vector<Point> innerSame = {{5, 5}, {15, 5}, {15, 15}, {5, 15}};
        const std::vector<Point> innerReversed = {{5, 5}, {5, 15}, {15, 15}, {15, 5}};
        auto a = raster::polygonCoverage(20, 20, {outer, innerSame}, true);
        auto b = raster::polygonCoverage(20, 20, {outer, innerReversed}, true);
        auto c = raster::polygonCoverage(20, 20, {outer, innerSame}, false);
        CHECK(a[10 * 20 + 10] > 0.99f && b[10 * 20 + 10] < 0.01f && c[10 * 20 + 10] < 0.01f && b[2 * 20 + 2] > 0.99f);
    }

    // ---- pattern fill and gradients ----
    {
        const std::string d = toText(*run("paint.new", {{"width", I(64)}, {"height", I(32)}}).field("doc"));
        doc = s.workspace().getAs<raster::ImageDoc>(d);
        CHECK(run("paint.pattern_fill", {{"doc", d}, {"pattern", std::string("checker")}, {"size", I(8)}, {"color", std::string("#ff0000")},
                                         {"background", std::string("#0000ff")}}).ok);
        const raster::Image& p = doc->activeLayer().image;
        CHECK(p.at(1, 1) == (Color{0, 0, 255, 255}) && p.at(5, 1) == (Color{255, 0, 0, 255}) && p.at(9, 1) == (Color{0, 0, 255, 255}) && p.at(5, 5) == (Color{0, 0, 255, 255}));
        // only the area around a point
        CHECK(run("paint.clear", {{"doc", d}}).ok);
        CHECK(run("paint.shape", {{"doc", d}, {"kind", std::string("rect")}, {"x", 0.0}, {"y", 0.0}, {"width", 32.0}, {"height", 32.0}, {"color", std::string("#00ff00")}}).ok);
        CHECK(run("paint.pattern_fill", {{"doc", d}, {"pattern", std::string("stripes")}, {"x", I(40)}, {"y", I(10)}, {"layer", I(1)}}).ok);
        CHECK(doc->activeLayer().image.at(10, 10) == (Color{0, 255, 0, 255}) && doc->activeLayer().image.at(40, 10).a == 255);
        // a layer as the tile
        CHECK(run("paint.pattern_fill", {{"doc", d}, {"layer", I(0)}, {"pattern_layer", I(1)}}).ok);
        // multi-colour gradient
        CHECK(run("paint.clear", {{"doc", d}}).ok);
        CHECK(run("paint.gradient", {{"doc", d}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 64.0}, {"y2", 0.0}, {"colors", std::string("#ff0000,#00ff00,#0000ff")}}).ok);
        // read through doc each time: a command may replace the layers (undo keeps the old ones)
        auto g = [&] { return doc->activeLayer().image; };
        CHECK(g().at(0, 5).r > 240 && g().at(32, 5).g > 240 && g().at(63, 5).b > 240);
        CHECK(run("paint.gradient", {{"doc", d}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 64.0}, {"y2", 0.0}, {"colors", std::string("#ff0000,#0000ff")}, {"reverse", true}}).ok);
        CHECK(g().at(0, 5).b > 240 && g().at(63, 5).r > 240);
        CHECK(run("paint.gradient", {{"doc", d}, {"x1", 32.0}, {"y1", 16.0}, {"x2", 64.0}, {"y2", 16.0}, {"kind", std::string("reflected")},
                                     {"from", std::string("#000000")}, {"to", std::string("#ffffff")}}).ok);
        CHECK(lum(g().at(32, 5)) < 10 && lum(g().at(1, 5)) > 240 && lum(g().at(63, 5)) > 240);
        CHECK(run("paint.gradient", {{"doc", d}, {"x1", 32.0}, {"y1", 16.0}, {"x2", 64.0}, {"y2", 16.0}, {"preset", std::string("rainbow")}, {"kind", std::string("angle")}}).ok);
        CHECK(run("paint.gradient", {{"doc", d}, {"x1", 32.0}, {"y1", 16.0}, {"x2", 48.0}, {"y2", 16.0}, {"kind", std::string("diamond")}}).ok);
        CHECK(lum(g().at(32, 16)) < 20 && lum(g().at(32 + 12, 16 + 6)) > 240);
        CHECK(!run("paint.gradient", {{"doc", d}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 1.0}, {"y2", 0.0}, {"colors", std::string("#ff0000")}}).ok);
        CHECK(run("paint.gradient", {{"doc", d}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 64.0}, {"y2", 0.0}, {"colors", std::string("#000@0,#fff@50,#fff@100")}}).ok);
        CHECK(lum(g().at(40, 5)) > 250);
    }
    return check::summary("paint_brushes");
}
