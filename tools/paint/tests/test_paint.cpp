#include <algorithm>
#include <filesystem>

#include "wetwaresuite/paint/paint.hpp"
#include "wetwaresuite/photo/photo.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "check.hpp"

using namespace wetwaresuite;
namespace fs = std::filesystem;

namespace {

std::string pick(Session& s, const std::string& id, int x, int y) {
    Result r = s.run("photo.pick_color", {{"doc", id}, {"x", std::int64_t{x}}, {"y", std::int64_t{y}}});
    const Value* v = r.field("color");
    return v ? toText(*v) : "";
}

}  // namespace

int main() {
    Session s;
    photo::registerCommands(s);
    paint::registerCommands(s);

    Result made = s.run("paint.new", {{"width", std::int64_t{100}}, {"height", std::int64_t{80}}});
    CHECK(made.ok);
    const std::string id = toText(*made.field("doc"));
    auto doc = s.workspace().getAs<raster::ImageDoc>(id);
    CHECK(doc && doc->image.layers.size() == 2 && doc->active == 1);

    CHECK(s.run("paint.stroke", {{"doc", id}, {"points", NumberList{10, 40, 90, 40}}, {"color", std::string("#ff0000")},
                                 {"size", 8.0}, {"hardness", 100.0}}).ok);
    CHECK(pick(s, id, 50, 40) == "#ff0000ff");
    CHECK(pick(s, id, 50, 10) == "#ffffffff");
    CHECK(doc->image.layers[0].image.at(50, 40).r == 255 && doc->image.layers[0].image.at(50, 40).g == 255);  // background untouched

    // pressure + presets + smoothing
    CHECK(s.run("paint.stroke", {{"doc", id}, {"points", NumberList{10, 70, 50, 72, 90, 70}}, {"pressures", NumberList{0.2, 1.0, 0.2}},
                                 {"brush", std::string("ink")}, {"smooth", 50.0}}).ok);
    CHECK(!s.run("paint.stroke", {{"doc", id}, {"points", NumberList{10, 70, 50}}}).ok);
    CHECK(!s.run("paint.stroke", {{"doc", id}, {"points", NumberList{1, 1, 2, 2}}, {"pressures", NumberList{1}}}).ok);
    CHECK(!s.run("paint.stroke", {{"doc", id}, {"points", NumberList{1, 1}}, {"brush", std::string("crayon")}}).ok);

    CHECK(s.run("paint.erase", {{"doc", id}, {"points", NumberList{50, 40}}, {"size", 20.0}, {"hardness", 100.0}}).ok);
    CHECK(pick(s, id, 50, 40) == "#ffffffff");

    // undo the erase brings the red back
    CHECK(s.history().undo());
    CHECK(pick(s, id, 50, 40) == "#ff0000ff");
    CHECK(s.history().redo());

    // layers
    Result added = s.run("paint.layer_add", {{"doc", id}, {"name", std::string("Ink")}});
    CHECK(added.ok && doc->active == 2 && doc->activeLayer().name == "Ink");
    CHECK(s.run("paint.shape", {{"doc", id}, {"kind", std::string("rect")}, {"x", 0.0}, {"y", 0.0}, {"width", 20.0},
                                {"height", 20.0}, {"color", std::string("blue")}}).ok);
    CHECK(pick(s, id, 5, 5) == "#0000ffff");
    CHECK(s.run("paint.layer_set", {{"doc", id}, {"opacity", 50.0}, {"blend", std::string("multiply")}}).ok);
    CHECK(doc->activeLayer().blend == raster::Blend::Multiply);
    CHECK(s.run("paint.layer_set", {{"doc", id}, {"visible", false}}).ok);
    CHECK(pick(s, id, 5, 5) == "#ffffffff");
    CHECK(s.run("paint.layer_set", {{"doc", id}, {"visible", true}, {"opacity", 100.0}, {"blend", std::string("normal")}}).ok);
    CHECK(s.run("paint.layer_duplicate", {{"doc", id}}).ok && doc->image.layers.size() == 4);
    CHECK(s.run("paint.layer_move", {{"doc", id}, {"layer", std::int64_t{3}}, {"to", std::int64_t{0}}}).ok);
    CHECK(doc->image.layers[0].name == "Ink copy");
    CHECK(s.run("paint.layer_delete", {{"doc", id}, {"layer", std::int64_t{0}}}).ok && doc->image.layers.size() == 3);
    CHECK(s.run("paint.layer_merge_down", {{"doc", id}, {"layer", std::int64_t{2}}}).ok && doc->image.layers.size() == 2);
    CHECK(!s.run("paint.layer_merge_down", {{"doc", id}, {"layer", std::int64_t{0}}}).ok);
    CHECK(!s.run("paint.layer_select", {{"doc", id}, {"layer", std::int64_t{9}}}).ok);

    // fills, shapes, gradients
    CHECK(s.run("paint.layer_add", {{"doc", id}, {"name", std::string("Fill")}}).ok);
    CHECK(s.run("paint.shape", {{"doc", id}, {"kind", std::string("ellipse")}, {"x", 30.0}, {"y", 20.0}, {"width", 40.0},
                                {"height", 40.0}, {"filled", false}, {"line_width", 3.0}, {"color", std::string("black")}}).ok);
    Result filled = s.run("paint.fill", {{"doc", id}, {"x", std::int64_t{50}}, {"y", std::int64_t{40}}, {"color", std::string("lime")}});
    CHECK(filled.ok && std::get<std::int64_t>(*filled.field("pixels")) > 500);
    CHECK(pick(s, id, 50, 40) == "#00ff00ff");
    CHECK(pick(s, id, 90, 10) == "#ffffffff");  // outside the ring
    CHECK(s.run("paint.polygon", {{"doc", id}, {"points", NumberList{0, 79, 10, 60, 20, 79}}, {"color", std::string("#123456")}}).ok);
    CHECK(s.run("paint.gradient", {{"doc", id}, {"x1", 0.0}, {"y1", 0.0}, {"x2", 100.0}, {"y2", 0.0}, {"layer", std::int64_t{0}}}).ok);
    CHECK(s.run("paint.clear", {{"doc", id}}).ok);
    CHECK(s.run("paint.flatten", {{"doc", id}, {"preview", true}}).attachments.size() == 1);
    CHECK(doc->image.layers.size() == 1);

    // round trip through .ora keeps layers
    auto dir = fs::temp_directory_path() / "wetwaresuite_test_paint";
    fs::create_directories(dir);
    CHECK(s.run("paint.layer_add", {{"doc", id}, {"name", std::string("Top")}}).ok);
    CHECK(s.run("photo.save", {{"doc", id}, {"path", (dir / "art.ora").string()}}).ok);
    Result reopened = s.run("photo.open", {{"path", (dir / "art.ora").string()}});
    CHECK(reopened.ok && std::get<std::int64_t>(*reopened.field("layers")) == 2);
    fs::remove_all(dir);

    // dirty-region undo: 100 small strokes on a 4000x4000 canvas keep only the painted areas
    {
        Session big;
        photo::registerCommands(big);
        paint::registerCommands(big);
        const std::size_t baseline = raster::undoMemoryBytes();
        Result r = big.run("paint.new", {{"width", std::int64_t{4000}}, {"height", std::int64_t{4000}}});
        CHECK(r.ok);
        const std::string bid = toText(*r.field("doc"));
        auto bdoc = big.workspace().getAs<raster::ImageDoc>(bid);
        const raster::LayeredImage original = bdoc->image;
        for (int i = 0; i < 100; ++i) {
            const double x = 37.0 * i, y = 3990.0 - 39.0 * i;
            CHECK(big.run("paint.stroke", {{"doc", bid}, {"points", NumberList{x, y, x + 15, y + 6, x + 25, y - 4}},
                                           {"size", 10.0}, {"color", std::string(i % 2 ? "#ff0000" : "#0000ff80")}}).ok);
        }
        CHECK(big.run("paint.erase", {{"doc", bid}, {"points", NumberList{100, 100, 140, 120}}, {"size", 20.0}}).ok);
        const raster::LayeredImage painted = bdoc->image;
        const std::size_t used = raster::undoMemoryBytes() - baseline;
        CHECK(used < 3u * 1024 * 1024);  // a whole-image snapshot would be 128 MB per step
        int steps = 0;
        while (big.history().undo()) ++steps;
        CHECK(steps >= 101);
        CHECK(bdoc->image.layers[1].image.bytes() == original.layers[1].image.bytes());
        CHECK(std::equal(original.layers[1].image.data(), original.layers[1].image.data() + original.layers[1].image.bytes(),
                         bdoc->image.layers[1].image.data()));
        CHECK(raster::undoMemoryBytes() - baseline == 0);  // undone steps give their memory back
        while (big.history().redo()) {
        }
        CHECK(std::equal(painted.layers[1].image.data(), painted.layers[1].image.data() + painted.layers[1].image.bytes(),
                         bdoc->image.layers[1].image.data()));
    }

    return check::summary("paint");
}
