#include <filesystem>

#include "achclwss/photo/photo.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/document.hpp"
#include "check.hpp"

using namespace achclwss;
namespace fs = std::filesystem;

namespace {

std::string docOf(const Result& r) {
    const Value* v = r.field("doc");
    return v ? toText(*v) : "";
}

std::int64_t intField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? std::get<std::int64_t>(*v) : -1;
}

}  // namespace

int main() {
    auto dir = fs::temp_directory_path() / "achclwss_test_photo";
    fs::remove_all(dir);
    fs::create_directories(dir / "in");

    raster::Image src(200, 100, {200, 100, 50, 255});
    for (int x = 0; x < 100; ++x) src.set(x, 50, {0, 0, 0, 255});
    raster::save(src, dir / "in" / "a.png");
    raster::save(src, dir / "in" / "b.jpg");

    Session s;
    photo::registerCommands(s);

    Result opened = s.run("photo.open", {{"path", (dir / "in" / "a.png").string()}});
    CHECK(opened.ok);
    std::string id = docOf(opened);
    CHECK(id == "img1");
    CHECK(intField(opened, "width") == 200);

    CHECK(!s.run("photo.open", {{"path", (dir / "nope.png").string()}}).ok);
    CHECK(!s.run("photo.open", {{"path", (dir / "x.xyz").string()}}).ok);

    Result resized = s.run("photo.resize", {{"doc", id}, {"width", std::int64_t{100}}});
    CHECK(resized.ok && intField(resized, "height") == 50);
    CHECK(s.history().undo());
    CHECK(intField(s.run("photo.info", {{"doc", id}}), "width") == 200);
    CHECK(s.history().redo());
    CHECK(intField(s.run("photo.info", {{"doc", id}}), "width") == 100);

    CHECK(s.run("photo.resize", {{"doc", id}, {"percent", 200.0}}).ok);
    CHECK(intField(s.run("photo.info", {{"doc", id}}), "width") == 200);
    CHECK(!s.run("photo.resize", {{"doc", id}}).ok);  // nothing to resize to

    Result cropped = s.run("photo.crop", {{"doc", id}, {"x", std::int64_t{10}}, {"y", std::int64_t{10}},
                                          {"width", std::int64_t{50}}, {"height", std::int64_t{40}}});
    CHECK(cropped.ok && intField(cropped, "width") == 50 && intField(cropped, "height") == 40);
    CHECK(!s.run("photo.crop", {{"doc", id}, {"x", std::int64_t{999}}, {"y", std::int64_t{0}},
                               {"width", std::int64_t{5}}, {"height", std::int64_t{5}}}).ok);

    CHECK(intField(s.run("photo.rotate", {{"doc", id}, {"angle", std::int64_t{90}}}), "width") == 40);
    CHECK(!s.run("photo.rotate", {{"doc", id}, {"angle", std::int64_t{45}}}).ok);
    CHECK(s.run("photo.flip", {{"doc", id}, {"direction", std::string("horizontal")}}).ok);
    CHECK(!s.run("photo.flip", {{"doc", id}, {"direction", std::string("sideways")}}).ok);  // not in choices

    Result before = s.run("photo.pick_color", {{"doc", id}, {"x", std::int64_t{1}}, {"y", std::int64_t{1}}});
    CHECK(s.run("photo.adjust", {{"doc", id}, {"kind", std::string("brightness")}, {"amount", 30.0}}).ok);
    Result after = s.run("photo.pick_color", {{"doc", id}, {"x", std::int64_t{1}}, {"y", std::int64_t{1}}});
    CHECK(intField(after, "r") > intField(before, "r"));
    CHECK(s.run("photo.effect", {{"doc", id}, {"kind", std::string("grayscale")}}).ok);
    Result gray = s.run("photo.pick_color", {{"doc", id}, {"x", std::int64_t{1}}, {"y", std::int64_t{1}}});
    CHECK(intField(gray, "r") == intField(gray, "g"));
    CHECK(s.run("photo.levels", {{"doc", id}, {"auto", true}}).ok);
    CHECK(s.run("photo.effect", {{"doc", id}, {"kind", std::string("blur")}, {"amount", 4.0}, {"preview", true}}).attachments.size() == 1);
    CHECK(!s.run("photo.adjust", {{"doc", id}, {"kind", std::string("brightness")}, {"amount", 1.0}, {"layer", std::int64_t{5}}}).ok);

    Result preview = s.run("photo.preview", {{"doc", id}, {"max_size", std::int64_t{32}}});
    CHECK(preview.ok && preview.attachments.size() == 1 && preview.attachments[0].mime == "image/png");
    CHECK(raster::decode(preview.attachments[0].bytes).height() == 32);

    CHECK(s.run("photo.save", {{"doc", id}, {"path", (dir / "out.jpg").string()}, {"quality", std::int64_t{80}}}).ok);
    CHECK(raster::load(dir / "out.jpg").width() == 40);
    CHECK(s.run("photo.save", {{"doc", id}, {"path", (dir / "out.ora").string()}}).ok);
    CHECK(!s.run("photo.save", {{"doc", id}, {"path", (dir / "out.xyz").string()}}).ok);

    Result created = s.run("photo.new", {{"width", std::int64_t{10}}, {"height", std::int64_t{10}}, {"background", std::string("red")}});
    CHECK(created.ok && docOf(created) == "img2");
    CHECK(!s.run("photo.new", {{"width", std::int64_t{10}}, {"height", std::int64_t{10}}, {"background", std::string("blurple")}}).ok);
    CHECK(s.run("photo.close", {{"doc", std::string("img2")}}).ok);
    CHECK(!s.run("photo.info", {{"doc", std::string("img2")}}).ok);

    Result batch = s.run("photo.batch", {{"input_dir", (dir / "in").string()}, {"output_dir", (dir / "out").string()},
                                         {"format", std::string("png")}, {"max_size", std::int64_t{50}}});
    CHECK(batch.ok && intField(batch, "processed") == 2);
    CHECK(raster::load(dir / "out" / "a.png").width() == 50);
    CHECK(raster::load(dir / "out" / "b.png").height() == 25);
    Result again = s.run("photo.batch", {{"input_dir", (dir / "in").string()}, {"output_dir", (dir / "out").string()},
                                         {"format", std::string("png")}});
    CHECK(again.ok && intField(again, "skipped") == 2 && intField(again, "processed") == 0);
    Result inPlace = s.run("photo.batch", {{"input_dir", (dir / "in").string()}, {"output_dir", (dir / "in").string()},
                                           {"format", std::string("png")}, {"max_size", std::int64_t{10}}, {"overwrite", true}});
    CHECK(raster::load(dir / "in" / "a.png").width() == 200);  // the original is never replaced
    CHECK(inPlace.ok && intField(inPlace, "processed") == 1);   // b.jpg -> b.png is fine
    CHECK(s.run("photo.save", {{"doc", id}, {"path", (dir / "new" / "deep" / "x.png").string()}}).ok);

    fs::remove_all(dir);
    return check::summary("photo");
}
