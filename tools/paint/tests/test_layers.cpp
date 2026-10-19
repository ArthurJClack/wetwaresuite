// Layer panel commands: groups, merge, move, layer properties, styles, text and shape layers.
#include <cstdio>
#include <filesystem>

#include "check.hpp"
#include "wetwaresuite/paint/paint.hpp"
#include "wetwaresuite/photo/photo.hpp"
#include "wetwaresuite/raster/document.hpp"

using namespace wetwaresuite;
using raster::Color;
namespace fs = std::filesystem;

namespace {

Session* S = nullptr;

Result run(const std::string& id, Params p) { return S->run(id, p); }

Color px(const raster::ImageDoc& doc, int x, int y) { return doc.flatten().at(x, y); }

bool near(Color c, int r, int g, int b, int tol = 3) {
    return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol;
}

std::int64_t I(int v) { return v; }

}  // namespace

int main() {
    Session s;
    S = &s;
    photo::registerCommands(s);
    paint::registerCommands(s);
    const std::string id = toText(*run("paint.new", {{"width", I(60)}, {"height", I(40)}}).field("doc"));
    auto doc = s.workspace().getAs<raster::ImageDoc>(id);
    auto& L = doc->image.layers;

    // ---- groups ----
    CHECK(run("paint.clear", {{"doc", id}, {"color", std::string("#ff0000")}}).ok);  // Layer 1 red
    CHECK(run("paint.layer_add", {{"doc", id}, {"name", std::string("Blue")}, {"fill", std::string("#0000ff")}}).ok);
    CHECK(L.size() == 3 && doc->active == 2);
    CHECK(run("paint.group", {{"doc", id}, {"from", I(1)}, {"to", I(2)}, {"name", std::string("G")}}).ok);
    CHECK(L.size() == 4 && L[3].kind == raster::LayerKind::Group && L[1].depth == 1 && L[2].depth == 1 && doc->active == 3);
    CHECK(near(px(*doc, 5, 5), 0, 0, 255));
    CHECK(run("paint.layer_set", {{"doc", id}, {"layer", I(3)}, {"visible", false}}).ok);
    CHECK(near(px(*doc, 5, 5), 255, 255, 255));  // hiding the group hides both members
    CHECK(run("paint.layer_set", {{"doc", id}, {"layer", I(3)}, {"visible", true}, {"opacity", 50.0}}).ok);
    CHECK(near(px(*doc, 5, 5), 128, 128, 255, 4));
    CHECK(!run("paint.layer_set", {{"doc", id}, {"layer", I(1)}, {"pass_through", false}}).ok);  // not a group
    CHECK(run("paint.layer_set", {{"doc", id}, {"layer", I(3)}, {"pass_through", false}, {"opacity", 100.0}}).ok);
    // adding inside a group: active group -> new layer goes on top of its members
    CHECK(run("paint.layer_add", {{"doc", id}, {"name", std::string("In")}}).ok);
    CHECK(doc->active == 3 && L[3].name == "In" && L[3].depth == 1 && L[4].kind == raster::LayerKind::Group);
    // duplicate a whole group
    CHECK(run("paint.layer_duplicate", {{"doc", id}, {"layer", I(4)}}).ok);
    CHECK(L.size() == 9 && L[8].kind == raster::LayerKind::Group && L[8].name == "G copy" && L[5].depth == 1 && doc->active == 8);
    // delete it again (with its contents)
    CHECK(run("paint.layer_delete", {{"doc", id}, {"layer", I(8)}}).ok && L.size() == 5);
    // move a layer out of the group (to the top) and back in
    CHECK(run("paint.layer_move", {{"doc", id}, {"layer", I(3)}, {"to", I(4)}}).ok);
    CHECK(L[4].name == "In" && L[4].depth == 0 && L[3].kind == raster::LayerKind::Group);
    CHECK(run("paint.layer_move", {{"doc", id}, {"layer", I(4)}, {"to", I(3)}}).ok);
    CHECK(L[3].name == "In" && L[3].depth == 1);
    // moving a group moves everything in it
    CHECK(run("paint.layer_move", {{"doc", id}, {"layer", I(4)}, {"to", I(3)}}).ok);
    CHECK(L[3].kind == raster::LayerKind::Group && L[0].depth == 1 && L[4].name == "Background" && L[4].depth == 0);
    CHECK(near(px(*doc, 5, 5), 255, 255, 255));  // the white background is on top now
    CHECK(s.history().undo());
    CHECK(L[4].kind == raster::LayerKind::Group && L[0].name == "Background");
    // ungroup keeps the members
    CHECK(run("paint.ungroup", {{"doc", id}, {"layer", I(4)}}).ok);
    CHECK(L.size() == 4 && L[1].depth == 0 && L[3].depth == 0 && L[3].name == "In");
    CHECK(!run("paint.ungroup", {{"doc", id}, {"layer", I(1)}}).ok);

    // ---- merge ----
    // a multiply layer merged down keeps the look
    CHECK(run("paint.layer_set", {{"doc", id}, {"layer", I(2)}, {"blend", std::string("multiply")}}).ok);
    CHECK(run("paint.clear", {{"doc", id}, {"layer", I(2)}, {"color", std::string("#808080")}}).ok);
    const Color before = px(*doc, 5, 5);
    CHECK(run("paint.layer_merge_down", {{"doc", id}, {"layer", I(2)}}).ok);
    CHECK(L.size() == 3 && near(px(*doc, 5, 5), before.r, before.g, before.b, 1));
    CHECK(L[1].blend == raster::Blend::Normal);
    // an adjustment layer merged down is applied to the layer below
    CHECK(run("photo.adjustment_layer", {{"doc", id}, {"type", std::string("invert")}, {"layer", I(1)}}).ok);
    CHECK(L[2].kind == raster::LayerKind::Adjustment);
    const Color inverted = px(*doc, 5, 5);
    CHECK(near(inverted, 255 - before.r, 255 - before.g, 255 - before.b, 1));
    CHECK(run("paint.layer_merge_down", {{"doc", id}, {"layer", I(2)}}).ok);
    CHECK(L.size() == 3 && L[1].kind == raster::LayerKind::Pixel && near(px(*doc, 5, 5), inverted.r, inverted.g, inverted.b, 1));
    // merge visible leaves hidden layers alone
    CHECK(run("paint.layer_set", {{"doc", id}, {"layer", I(2)}, {"visible", false}}).ok);
    CHECK(run("paint.merge_visible", {{"doc", id}}).ok);
    CHECK(L.size() == 2 && L[0].name == "Merged" && !L[1].visible);
    CHECK(s.history().undo() && L.size() == 3);

    // ---- layer properties ----
    // lock alpha: painting can't spread past the painted pixels
    {
        const std::string d = toText(*run("paint.new", {{"width", I(40)}, {"height", I(20)}, {"background", std::string("transparent")}}).field("doc"));
        auto dd = s.workspace().getAs<raster::ImageDoc>(d);
        CHECK(run("paint.shape", {{"doc", d}, {"kind", std::string("rect")}, {"x", 0.0}, {"y", 0.0}, {"width", 20.0}, {"height", 20.0},
                                  {"color", std::string("#ff0000")}}).ok);
        CHECK(run("paint.layer_set", {{"doc", d}, {"lock_alpha", true}}).ok);
        CHECK(run("paint.stroke", {{"doc", d}, {"points", NumberList{2, 10, 38, 10}}, {"size", 6.0}, {"hardness", 100.0}, {"color", std::string("#0000ff")}}).ok);
        CHECK(dd->activeLayer().image.at(10, 10) == (Color{0, 0, 255, 255}));
        CHECK(dd->activeLayer().image.at(30, 10).a == 0);
        // fill opacity fades the pixels but not the stroke style
        CHECK(run("paint.layer_set", {{"doc", d}, {"lock_alpha", false}, {"fill", 0.0}}).ok);
        CHECK(dd->flatten().at(10, 10).a == 0);
        CHECK(run("paint.layer_style", {{"doc", d}, {"effect", std::string("stroke")}, {"size", 2.0}, {"color", std::string("#00ff00")},
                                        {"position", std::string("inside")}}).ok);
        CHECK(dd->flatten().at(0, 10) == (Color{0, 255, 0, 255}));
        CHECK(dd->flatten().at(10, 10).a == 0);
        // clip
        CHECK(!run("paint.layer_set", {{"doc", d}, {"layer", I(0)}, {"clip", true}}).ok);
    }

    // ---- layer styles ----
    {
        const std::string d = toText(*run("paint.new", {{"width", I(60)}, {"height", I(60)}}).field("doc"));
        auto dd = s.workspace().getAs<raster::ImageDoc>(d);
        CHECK(run("paint.shape", {{"doc", d}, {"kind", std::string("rect")}, {"x", 20.0}, {"y", 20.0}, {"width", 20.0}, {"height", 20.0},
                                  {"color", std::string("#ff0000")}}).ok);
        Result st = run("paint.layer_style", {{"doc", d}, {"effect", std::string("drop_shadow")}, {"distance", 6.0}, {"angle", 90.0},
                                              {"size", 0.0}, {"opacity", 100.0}, {"color", std::string("#000000")}});
        CHECK(st.ok && st.field("style"));
        // the shadow falls straight down (angle 90 = light from above), 6 px
        CHECK(near(dd->flatten().at(30, 43), 0, 0, 0, 2));
        CHECK(near(dd->flatten().at(30, 17), 255, 255, 255, 2));
        CHECK(!run("paint.layer_style", {{"doc", d}, {"effect", std::string("drop_shadow")}, {"position", std::string("inside")}}).ok);
        CHECK(run("paint.layer_style", {{"doc", d}, {"effect", std::string("color_overlay")}, {"color", std::string("#00ff00")}}).ok);
        CHECK(near(dd->flatten().at(30, 30), 0, 255, 0));
        // copy the style to another layer
        const std::string text = toText(*run("paint.layer_style", {{"doc", d}, {"effect", std::string("color_overlay")}}).field("style"));
        CHECK(run("paint.layer_add", {{"doc", d}}).ok);
        CHECK(run("paint.layer_style", {{"doc", d}, {"style", text}}).ok);
        CHECK(dd->activeLayer().style == dd->image.layers[1].style);
        // rasterize bakes the style into pixels
        CHECK(run("paint.rasterize", {{"doc", d}, {"layer", I(1)}}).ok);
        CHECK(!dd->image.layers[1].style.any() && near(dd->image.layers[1].image.at(30, 30), 0, 255, 0) && near(dd->flatten().at(30, 43), 0, 0, 0, 2));
        CHECK(run("paint.layer_style", {{"doc", d}, {"layer", I(2)}, {"effect", std::string("all")}, {"on", false}}).ok);
        CHECK(!dd->image.layers[2].style.any());
        CHECK(s.history().undo() && dd->image.layers[2].style.any());
    }

    // ---- text and shape layers ----
    {
        const std::string d = toText(*run("paint.new", {{"width", I(200)}, {"height", I(80)}}).field("doc"));
        auto dd = s.workspace().getAs<raster::ImageDoc>(d);
        Result t = run("paint.text", {{"doc", d}, {"text", std::string("Hello")}, {"x", 10.0}, {"y", 10.0}, {"size", 40.0}, {"color", std::string("#0000ff")}});
        CHECK(t.ok);
        raster::Layer& tl = dd->activeLayer();
        CHECK(tl.kind == raster::LayerKind::Text && tl.settings.at("text") == "Hello");
        auto inked = [](const raster::Image& img, int x0, int x1) {
            int n = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = x0; x < x1; ++x) n += img.at(x, y).a > 128;
            }
            return n;
        };
        const int ink = inked(tl.image, 0, 200);
        CHECK(ink > 300 && inked(tl.image, 150, 200) == 0);
        // edit: bigger text has more ink; colour changes
        CHECK(run("paint.text_edit", {{"doc", d}, {"size", 60.0}, {"color", std::string("#ff0000")}}).ok);
        CHECK(inked(dd->activeLayer().image, 0, 200) > ink * 3 / 2);
        bool red = false;
        for (int x = 0; x < 200 && !red; ++x) red = dd->activeLayer().image.at(x, 45).r > 200 && dd->activeLayer().image.at(x, 45).a > 200;
        CHECK(red);
        // centre alignment around x
        CHECK(run("paint.text_edit", {{"doc", d}, {"size", 20.0}, {"align", std::string("center")}, {"x", 100.0}}).ok);
        CHECK(inked(dd->activeLayer().image, 0, 60) == 0 && inked(dd->activeLayer().image, 140, 200) == 0 && inked(dd->activeLayer().image, 80, 120) > 0);
        CHECK(!run("paint.text_edit", {{"doc", d}, {"layer", I(0)}, {"size", 10.0}}).ok);  // background isn't text
        // painting on it turns it into pixels
        CHECK(run("paint.stroke", {{"doc", d}, {"points", NumberList{5, 5, 6, 6}}}).ok);
        CHECK(dd->activeLayer().kind == raster::LayerKind::Pixel);
        CHECK(s.history().undo() && dd->activeLayer().kind == raster::LayerKind::Text);
        // shape layers
        CHECK(run("paint.shape_layer", {{"doc", d}, {"shape", std::string("ellipse")}, {"x", 100.0}, {"y", 10.0}, {"width", 60.0}, {"height", 60.0},
                                        {"fill", std::string("#00ff00")}}).ok);
        CHECK(dd->activeLayer().kind == raster::LayerKind::Shape && near(dd->flatten().at(130, 40), 0, 255, 0) && near(dd->flatten().at(101, 11), 255, 255, 255));
        CHECK(run("paint.shape_edit", {{"doc", d}, {"shape", std::string("rect")}}).ok);
        CHECK(near(dd->flatten().at(101, 11), 0, 255, 0));
        CHECK(run("paint.shape_edit", {{"doc", d}, {"shape", std::string("star")}, {"sides", I(5)}, {"stroke", std::string("#000000")}, {"stroke_width", 3.0}}).ok);
        CHECK(near(dd->flatten().at(130, 40), 0, 255, 0) && near(dd->flatten().at(130, 11), 0, 0, 0, 40));  // top point outlined
        CHECK(!run("paint.shape_edit", {{"doc", d}, {"fill", std::string("nope")}}).ok);
        // layered save keeps them editable
        auto dir = fs::temp_directory_path() / "wetwaresuite_test_paint_layers";
        fs::create_directories(dir);
        CHECK(run("photo.save", {{"doc", d}, {"path", (dir / "t.ora").string()}}).ok);
        Result re = run("photo.open", {{"path", (dir / "t.ora").string()}});
        CHECK(re.ok);
        auto back = s.workspace().getAs<raster::ImageDoc>(toText(*re.field("doc")));
        CHECK(back && back->image.layers.size() == 4 && back->image.layers[2].kind == raster::LayerKind::Text &&
              back->image.layers[2].settings.at("text") == "Hello" && back->image.layers[3].kind == raster::LayerKind::Shape);
        Result ps = run("photo.save", {{"doc", d}, {"path", (dir / "t.psd").string()}});
        CHECK(ps.ok && ps.message.find("pixels") != std::string::npos);  // the notes say what PSD couldn't keep
        fs::remove_all(dir);
        // resizing rasterizes text
        CHECK(run("photo.resize", {{"doc", d}, {"percent", 50.0}}).ok);
        CHECK(dd->image.layers[2].kind == raster::LayerKind::Pixel && dd->image.layers[2].image.width() == 100);
    }
    return check::summary("paint_layers");
}
