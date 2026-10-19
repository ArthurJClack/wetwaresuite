// Adjustment layers, the filter menu, liquify/warp and select subject, checked pixel by pixel.
#include <cmath>

#include "check.hpp"
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

std::shared_ptr<raster::ImageDoc> make(int w, int h, const std::string& bg, std::string& id) {
    id = toText(*run("photo.new", {{"width", I(w)}, {"height", I(h)}, {"background", bg}}).field("doc"));
    return S->workspace().getAs<raster::ImageDoc>(id);
}

}  // namespace

int main() {
    Session s;
    S = &s;
    photo::registerCommands(s);
    std::string id;

    // ---- adjustment layers ----
    {
        auto doc = make(40, 20, "#336699", id);
        Result r = run("photo.adjustment_layer", {{"doc", id}, {"type", std::string("invert")}});
        CHECK(r.ok && doc->image.layers.size() == 2 && doc->image.layers[1].kind == raster::LayerKind::Adjustment);
        CHECK(doc->flatten().at(5, 5) == (Color{0xcc, 0x99, 0x66, 255}));
        CHECK(doc->image.layers[0].image.at(5, 5) == (Color{0x33, 0x66, 0x99, 255}));  // the pixels below are untouched
        // edit: switch to levels and change its settings
        CHECK(run("photo.adjustment_edit", {{"doc", id}, {"type", std::string("levels")}, {"black", 0x33 * 1.0}, {"white", 0x99 * 1.0}}).ok);
        const Color lv = doc->flatten().at(5, 5);
        CHECK(lv.r == 0 && lv.b == 255 && std::abs(lv.g - 128) < 3);
        CHECK(!run("photo.adjustment_edit", {{"doc", id}, {"layer", I(0)}, {"black", 1.0}}).ok);  // not an adjustment layer
        CHECK(!run("photo.adjustment_edit", {{"doc", id}, {"hue", 10.0}}).ok);                    // levels has no hue
        CHECK(!run("photo.adjustment_edit", {{"doc", id}, {"gamma", 50.0}}).ok);                  // out of range
        // hue/saturation, opacity and hiding
        CHECK(run("photo.adjustment_edit", {{"doc", id}, {"type", std::string("hue_saturation")}, {"saturation", -100.0}}).ok);
        const Color gray = doc->flatten().at(5, 5);
        CHECK(gray.r == gray.g && gray.g == gray.b);
        CHECK(run("photo.adjustment_edit", {{"doc", id}, {"opacity", 0.0}}).ok && doc->flatten().at(5, 5) == (Color{0x33, 0x66, 0x99, 255}));
        // undo restores the previous settings
        CHECK(s.history().undo() && doc->flatten().at(5, 5) == gray);
        // a selection becomes the adjustment layer's mask
        CHECK(run("photo.select", {{"doc", id}, {"shape", std::string("rect")}, {"x", 0.0}, {"y", 0.0}, {"width", 20.0}, {"height", 20.0}}).ok);
        CHECK(run("photo.adjustment_layer", {{"doc", id}, {"type", std::string("threshold")}, {"level", 150.0}}).ok);
        CHECK(lum(doc->flatten().at(5, 5)) == 0 && doc->flatten().at(30, 5) == gray);
        // curves with a preset, gradient map, selective colour, channel mixer, photo filter, black & white
        auto one = [&](const std::string& type, Params extra, Color in) {
            std::string d;
            auto dd = make(4, 4, in.hex(), d);
            extra["doc"] = d;
            extra["type"] = type;
            CHECK(run("photo.adjustment_layer", extra).ok);
            return dd->flatten().at(1, 1);
        };
        CHECK(one("curves", {{"preset", std::string("invert")}}, {10, 20, 30, 255}) == (Color{245, 235, 225, 255}));
        CHECK(one("curves", {{"points", NumberList{0, 0, 128, 200, 255, 255}}}, {128, 128, 128, 255}).r >= 198);
        CHECK(one("gradient_map", {{"colors", std::string("#ff0000,#0000ff")}}, {0, 0, 0, 255}) == (Color{255, 0, 0, 255}));
        CHECK(one("selective_color", {{"range", std::string("reds")}, {"cyan", 100.0}, {"method", std::string("absolute")}}, {255, 0, 0, 255}).r < 200);
        CHECK(one("channel_mixer", {{"red", NumberList{0, 0, 100}}}, {10, 20, 200, 255}).r == 200);
        CHECK(one("photo_filter", {{"color", std::string("#ff0000")}, {"density", 100.0}, {"preserve_luminosity", false}}, {128, 128, 128, 255}).g < 100);
        const Color bw = one("black_white", {}, {200, 30, 30, 255});
        CHECK(bw.r == bw.g && bw.g == bw.b);
        CHECK(one("posterize", {{"levels", 2.0}}, {100, 200, 30, 255}) == (Color{0, 255, 0, 255}));
        CHECK(one("exposure", {{"exposure", 1.0}}, {100, 100, 100, 255}).r > 120);
        CHECK(one("vibrance", {{"vibrance", 100.0}}, {150, 120, 110, 255}).r > 150);
        CHECK(one("color_balance", {{"midtones", NumberList{100, 0, 0}}}, {128, 128, 128, 255}).r > 128);
        CHECK(one("brightness_contrast", {{"brightness", 50.0}}, {100, 100, 100, 255}).r > 120);
        // apply once to the pixels
        auto pixels = make(4, 4, "#102030", id);
        CHECK(run("photo.apply_adjustment", {{"doc", id}, {"type", std::string("invert")}}).ok);
        CHECK(pixels->image.layers.size() == 1 && pixels->image.layers[0].image.at(0, 0) == (Color{0xef, 0xdf, 0xcf, 255}));
        CHECK(!run("photo.apply_adjustment", {{"doc", id}, {"type", std::string("curves")}}).ok);  // needs points or a preset
    }

    // ---- blur ----
    {
        auto doc = make(40, 40, "#ffffff", id);
        auto img = [&]() -> raster::Image& { return doc->image.layers[0].image; };
        raster::fillRect(img(), 20, 0, 20, 40, {0, 0, 0, 255});
        CHECK(run("photo.blur", {{"doc", id}, {"radius", 3.0}}).ok);
        CHECK(lum(img().at(19, 20)) > 30 && lum(img().at(19, 20)) < 225 && lum(img().at(2, 20)) == 255);
        CHECK(s.history().undo());
        // motion blur sideways smears the edge sideways but not along it
        CHECK(run("photo.blur", {{"doc", id}, {"kind", std::string("motion")}, {"distance", 10.0}, {"angle", 0.0}}).ok);
        CHECK(lum(img().at(18, 20)) < 250 && lum(img().at(18, 20)) > 5);
        CHECK(s.history().undo());
        CHECK(run("photo.blur", {{"doc", id}, {"kind", std::string("motion")}, {"distance", 10.0}, {"angle", 90.0}}).ok);
        CHECK(lum(img().at(18, 20)) == 255);
        CHECK(s.history().undo());
        // zoom blur leaves the centre sharp
        CHECK(run("photo.blur", {{"doc", id}, {"kind", std::string("zoom")}, {"amount", 30.0}, {"center_x", 20.0}, {"center_y", 20.0}}).ok);
        CHECK(lum(img().at(20, 20)) < 10 || lum(img().at(19, 20)) > 245);
        CHECK(s.history().undo());
        CHECK(run("photo.blur", {{"doc", id}, {"kind", std::string("spin")}, {"amount", 40.0}}).ok);
        CHECK(lum(img().at(20, 3)) > 5 && lum(img().at(20, 3)) < 250);  // the edge far from the centre turned and blurred
        CHECK(s.history().undo());
    }

    // ---- sharpen, noise, stylize ----
    {
        auto doc = make(40, 40, "#808080", id);
        auto img = [&]() -> raster::Image& { return doc->image.layers[0].image; };
        raster::fillRect(img(), 20, 0, 20, 40, {0xa0, 0xa0, 0xa0, 255});
        CHECK(run("photo.sharpen", {{"doc", id}, {"amount", 200.0}, {"radius", 2.0}}).ok);
        CHECK(lum(img().at(19, 20)) < 0x80 && lum(img().at(20, 20)) > 0xa0 && lum(img().at(5, 20)) == 0x80);
        CHECK(s.history().undo());
        CHECK(run("photo.sharpen", {{"doc", id}, {"amount", 200.0}, {"threshold", I(100)}}).ok);
        CHECK(lum(img().at(19, 20)) == 0x80);  // the step is below the threshold
        CHECK(s.history().undo());
        CHECK(run("photo.sharpen", {{"doc", id}, {"kind", std::string("high_pass")}, {"radius", 3.0}}).ok);
        CHECK(lum(img().at(5, 20)) == 128 && lum(img().at(19, 20)) < 128 && lum(img().at(20, 20)) > 128);
        CHECK(s.history().undo());
        // noise then median brings it back close to flat
        CHECK(run("photo.noise", {{"doc", id}, {"amount", 20.0}, {"monochrome", true}}).ok);
        int spread = 0;
        for (int x = 0; x < 15; ++x) spread = std::max(spread, std::abs(lum(img().at(x, 10)) - 0x80));
        CHECK(spread > 10 && img().at(3, 3).r == img().at(3, 3).g);
        CHECK(run("photo.noise", {{"doc", id}, {"kind", std::string("median")}, {"radius", I(2)}}).ok);
        int after = 0;
        for (int x = 2; x < 15; ++x) after = std::max(after, std::abs(lum(img().at(x, 10)) - 0x80));
        CHECK(after < spread);
        s.history().undo();
        s.history().undo();
        // a lone speck goes with despeckle, the edge stays
        img().set(5, 5, {255, 255, 255, 255});
        CHECK(run("photo.noise", {{"doc", id}, {"kind", std::string("despeckle")}}).ok);
        CHECK(img().at(5, 5) == (Color{0x80, 0x80, 0x80, 255}) && img().at(20, 20) == (Color{0xa0, 0xa0, 0xa0, 255}));
        CHECK(run("photo.stylize", {{"doc", id}, {"kind", std::string("find_edges")}}).ok);
        CHECK(lum(img().at(5, 20)) == 255 && lum(img().at(20, 20)) < 240);
        s.history().undo();
        CHECK(run("photo.stylize", {{"doc", id}, {"kind", std::string("emboss")}, {"angle", 0.0}}).ok);
        CHECK(lum(img().at(5, 20)) == 128 && lum(img().at(19, 20)) != 128);
        s.history().undo();
        CHECK(run("photo.stylize", {{"doc", id}, {"kind", std::string("oil_paint")}, {"radius", I(3)}}).ok);
        CHECK(img().at(5, 20) == (Color{0x80, 0x80, 0x80, 255}) && img().at(30, 20) == (Color{0xa0, 0xa0, 0xa0, 255}));
        CHECK(img().at(19, 20) == (Color{0x80, 0x80, 0x80, 255}) || img().at(19, 20) == (Color{0xa0, 0xa0, 0xa0, 255}));  // edges stay crisp
        CHECK(run("photo.clouds", {{"doc", id}, {"scale", 16.0}}).ok);
        int lo = 255, hi = 0;
        for (int x = 0; x < 40; ++x) lo = std::min(lo, lum(img().at(x, 7))), hi = std::max(hi, lum(img().at(x, 7)));
        CHECK(hi - lo > 40);
    }

    // ---- distort ----
    {
        auto doc = make(60, 60, "#ffffff", id);
        auto img = [&]() -> raster::Image& { return doc->image.layers[0].image; };
        raster::fillRect(img(), 25, 25, 10, 10, {0, 0, 0, 255});
        // liquify push drags the square right
        CHECK(run("photo.liquify", {{"doc", id}, {"points", NumberList{30, 30, 42, 30}}, {"size", 30.0}, {"pressure", 100.0}}).ok);
        CHECK(lum(img().at(37, 30)) < 128);
        CHECK(s.history().undo() && lum(img().at(37, 30)) == 255);
        CHECK(run("photo.liquify", {{"doc", id}, {"mode", std::string("bloat")}, {"points", NumberList{30, 30}}, {"size", 40.0}, {"pressure", 100.0}}).ok);
        CHECK(lum(img().at(36, 30)) < 128);  // the square grew
        CHECK(s.history().undo());
        CHECK(run("photo.liquify", {{"doc", id}, {"mode", std::string("pucker")}, {"points", NumberList{30, 30}}, {"size", 40.0}, {"pressure", 100.0}}).ok);
        CHECK(lum(img().at(26, 30)) > 128);  // the square shrank
        CHECK(s.history().undo());
        // warp grid: moving every control point right by 5 moves the picture
        NumberList moves;
        for (int i = 0; i < 9; ++i) moves.push_back(5), moves.push_back(0);
        CHECK(run("photo.warp", {{"doc", id}, {"cols", I(2)}, {"rows", I(2)}, {"moves", moves}}).ok);
        CHECK(lum(img().at(26, 30)) == 255 && lum(img().at(38, 30)) == 0);
        CHECK(s.history().undo());
        CHECK(!run("photo.warp", {{"doc", id}, {"moves", NumberList{1, 2}}}).ok);
        CHECK(run("photo.warp", {{"doc", id}, {"preset", std::string("arc")}, {"bend", 50.0}}).ok);
        CHECK(lum(img().at(30, 24)) < 128);  // the middle rose
        CHECK(s.history().undo());
        // lens correction: pincushion fix pulls the edges in, vignette lights the corners
        CHECK(run("photo.lens_correction", {{"doc", id}, {"vignette", -100.0}}).ok);
        CHECK(lum(img().at(1, 1)) < 200 && lum(img().at(30, 21)) > 235);
        CHECK(s.history().undo());
        CHECK(run("photo.lens_correction", {{"doc", id}, {"distortion", -80.0}}).ok);
        CHECK(lum(img().at(30, 30)) == 0);
        CHECK(s.history().undo());
        // displace by a flat map that's all red: everything shifts sideways
        raster::Layer map("map", raster::Image(60, 60, {255, 128, 128, 255}));
        map.visible = false;
        doc->image.layers.push_back(map);
        CHECK(run("photo.displace", {{"doc", id}, {"layer", I(0)}, {"map_layer", I(1)}, {"scale_x", 10.0}, {"scale_y", 10.0}}).ok);
        CHECK(lum(img().at(16, 30)) < 30 && lum(img().at(33, 30)) > 225);
    }

    // ---- select subject and refine edge ----
    {
        auto doc = make(80, 60, "#2050a0", id);
        auto img = [&]() -> raster::Image& { return doc->image.layers[0].image; };
        raster::fillEllipse(img(), 40, 30, 18, 18, {230, 200, 40, 255});
        raster::fillEllipse(img(), 40, 30, 5, 5, {40, 90, 160, 255});  // a hole of near-background colour: filled in
        img().set(5, 5, {250, 250, 250, 255});                           // a stray speck: left out
        Result r = run("photo.select_subject", {{"doc", id}});
        CHECK(r.ok && doc->hasSelection());
        CHECK(doc->selection.at(40, 30) == 255 && doc->selection.at(30, 30) == 255 && doc->selection.at(2, 2) == 0 && doc->selection.at(5, 5) == 0);
        CHECK(doc->selection.at(75, 30) == 0);
        // refine: a sloppy box snaps toward the circle's edge
        CHECK(run("photo.select", {{"doc", id}, {"shape", std::string("rect")}, {"x", 20.0}, {"y", 10.0}, {"width", 40.0}, {"height", 40.0}}).ok);
        CHECK(doc->selection.at(22, 12) == 255);
        CHECK(run("photo.refine_edge", {{"doc", id}, {"radius", I(8)}, {"contrast", 60.0}}).ok);
        CHECK(doc->selection.at(22, 12) < doc->selection.at(40, 30));
        CHECK(doc->selection.at(40, 30) > 200);
        CHECK(!run("photo.refine_edge", {{"doc", id}, {"target", std::string("mask")}}).ok);  // no mask yet
    }
    return check::summary("photo_filters");
}
