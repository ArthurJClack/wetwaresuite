#include <filesystem>
#include <fstream>

#include "achclwss/convert/convert.hpp"
#include "achclwss/core/process.hpp"
#include "achclwss/raster/codec.hpp"
#include "check.hpp"

using namespace achclwss;
namespace fs = std::filesystem;

int main() {
    auto dir = fs::temp_directory_path() / "achclwss_test_convert";
    fs::remove_all(dir);
    fs::create_directories(dir / "pics");
    raster::Image img(64, 32, {10, 200, 30, 255});
    raster::save(img, dir / "pics" / "a.png");
    raster::save(img, dir / "pics" / "b.bmp");
    {
        std::ofstream(dir / "pics" / "notes.txt") << "not an image";
    }

    Session s;
    convert::registerCommands(s);

    CHECK(convert::backendFor(s, "a.png", "jpg") == "raster");
    CHECK(convert::backendFor(s, "a.ora", "png") == "raster");
    CHECK(convert::backendFor(s, "a.wav", "mp3") == "ffmpeg");
    CHECK(convert::backendFor(s, "a.mp4", "mp3") == "ffmpeg");
    CHECK(convert::backendFor(s, "a.docx", "pdf") == "libreoffice");
    CHECK(convert::backendFor(s, "a.svg", "png").empty());  // vector tool not registered here
    CHECK(convert::backendFor(s, "a.xyz", "png").empty());

    Result r = s.run("convert.file", {{"input", (dir / "pics" / "a.png").string()}, {"output", (dir / "a.jpg").string()}});
    CHECK(r.ok);
    CHECK(raster::load(dir / "a.jpg").width() == 64);
    CHECK(!s.run("convert.file", {{"input", (dir / "pics" / "a.png").string()}, {"output", (dir / "a.jpg").string()}}).ok);  // exists
    CHECK(s.run("convert.file", {{"input", (dir / "pics" / "a.png").string()}, {"output", (dir / "a.jpg").string()},
                                 {"overwrite", true}, {"width", std::int64_t{32}}}).ok);
    CHECK(raster::load(dir / "a.jpg").height() == 16);
    CHECK(!s.run("convert.file", {{"input", (dir / "missing.png").string()}, {"output", (dir / "m.jpg").string()}}).ok);
    CHECK(!s.run("convert.file", {{"input", (dir / "pics" / "a.png").string()}, {"output", (dir / "noext").string()}}).ok);
    Result model = s.run("convert.file", {{"input", (dir / "pics" / "a.png").string()}, {"output", (dir / "a.stl").string()}});
    CHECK(!model.ok && model.message.find("phase 3") != std::string::npos);

    Result batch = s.run("convert.batch", {{"input_dir", (dir / "pics").string()}, {"to", std::string("tga")},
                                           {"output_dir", (dir / "tga").string()}});
    CHECK(batch.ok && std::get<std::int64_t>(*batch.field("converted")) == 2);
    CHECK(fs::exists(dir / "tga" / "a.tga") && fs::exists(dir / "tga" / "b.tga"));
    CHECK(!fs::exists(dir / "tga" / "notes.tga"));

    CHECK(s.run("convert.formats", {}).ok);
    CHECK(s.run("convert.check", {{"input", std::string("x.png")}, {"to", std::string(".jpg")}}).ok);
    CHECK(!s.run("convert.check", {{"input", std::string("x.png")}, {"to", std::string("xyz")}}).ok);

    if (findProgram("ffmpeg")) {
        Result wav = s.run("convert.file", {{"input", (dir / "pics" / "a.png").string()}, {"output", (dir / "a.webp").string()}});
        CHECK(wav.ok);
        CHECK(fs::exists(dir / "a.webp") && fs::file_size(dir / "a.webp") > 0);
    }

    fs::remove_all(dir);
    return check::summary("convert");
}
