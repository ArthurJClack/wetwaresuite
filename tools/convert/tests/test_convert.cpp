#include <cmath>
#include <filesystem>
#include <fstream>

#include "achclwss/convert/convert.hpp"
#include "achclwss/core/process.hpp"
#include "achclwss/mesh/io.hpp"
#include "achclwss/mesh/primitives.hpp"
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
    CHECK(convert::backendFor(s, "a.stl", "glb") == "mesh");
    CHECK(convert::backendFor(s, "a.OBJ", "3mf") == "mesh");
    CHECK(convert::backendFor(s, "a.stl", "png") == "mesh");  // rendered preview
    CHECK(convert::backendFor(s, "a.png", "stl").empty());
    CHECK(convert::backendFor(s, "a.fbx", "stl").empty());

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
    CHECK(!model.ok && model.message.find("3D meshes convert between") != std::string::npos);
    CHECK(!fs::exists(dir / "a.stl"));

    // 3D meshes go through the mesh library.
    fs::create_directories(dir / "models");
    mesh::save(dir / "models" / "cube.stl", mesh::box({10, 20, 30}));
    mesh::save(dir / "models" / "pipe.obj", mesh::revolve({{1, 0}, {2, 0}, {2, 5}, {1, 5}}, 360, 24));
    Result m1 = s.run("convert.file", {{"input", (dir / "models" / "cube.stl").string()}, {"output", (dir / "cube.glb").string()}});
    CHECK(m1.ok && std::get<std::string>(*m1.field("backend")) == "mesh");
    CHECK(std::abs(mesh::volume(mesh::load(dir / "cube.glb")) - 6000) < 0.01);
    Result m2 = s.run("convert.file", {{"input", (dir / "cube.glb").string()}, {"output", (dir / "cube_in.3mf").string()},
                                       {"scale", 25.4}});
    CHECK(m2.ok && std::abs(mesh::bounds(mesh::load(dir / "cube_in.3mf")).size().z - 30 * 25.4) < 1e-3);
    Result m3 = s.run("convert.file", {{"input", (dir / "models" / "cube.stl").string()}, {"output", (dir / "cube_text.stl").string()},
                                       {"ascii", true}});
    CHECK(m3.ok);
    {
        std::ifstream in(dir / "cube_text.stl");
        std::string first;
        in >> first;
        CHECK(first == "solid");
    }
    Result pic = s.run("convert.file", {{"input", (dir / "models" / "pipe.obj").string()}, {"output", (dir / "pipe.png").string()},
                                        {"width", std::int64_t{200}}, {"view", std::string("top")}});
    CHECK(pic.ok);
    CHECK(raster::load(dir / "pipe.png").width() == 200 && raster::load(dir / "pipe.png").height() == 200);
    Result later = s.run("convert.file", {{"input", (dir / "models" / "cube.stl").string()}, {"output", (dir / "cube.fbx").string()}});
    CHECK(!later.ok && later.message.find("isn't supported yet") != std::string::npos);
    Result meshes = s.run("convert.batch", {{"input_dir", (dir / "models").string()}, {"to", std::string("ply")},
                                            {"output_dir", (dir / "ply").string()}});
    CHECK(meshes.ok && std::get<std::int64_t>(*meshes.field("converted")) == 2);
    CHECK(mesh::isWatertight(mesh::load(dir / "ply" / "pipe.ply")));
    CHECK(s.run("convert.check", {{"input", std::string("x.stl")}, {"to", std::string("obj")}}).ok);

    Result batch = s.run("convert.batch", {{"input_dir", (dir / "pics").string()}, {"to", std::string("tga")},
                                           {"output_dir", (dir / "tga").string()}});
    CHECK(batch.ok && std::get<std::int64_t>(*batch.field("converted")) == 2);
    CHECK(fs::exists(dir / "tga" / "a.tga") && fs::exists(dir / "tga" / "b.tga"));
    CHECK(!fs::exists(dir / "tga" / "notes.tga"));

    Result formats = s.run("convert.formats", {});
    CHECK(formats.ok && formats.message.find("3D meshes (built in, ready)") != std::string::npos);
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
