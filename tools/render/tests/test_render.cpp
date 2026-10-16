#include <cmath>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/render/render.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

std::string str(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : "";
}

std::int64_t intField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

double brightness(const raster::Image& img, int x0, int y0, int x1, int y1) {
    double sum = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const auto c = img.at(x, y);
            sum += (c.r + c.g + c.b) / 3.0;
            ++n;
        }
    }
    return n ? sum / n : 0;
}

json call(mcp::Server& server, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
}

render::RenderOptions small(const std::string& mode, int w = 48, int h = 36) {
    render::RenderOptions o;
    o.mode = mode;
    o.width = w;
    o.height = h;
    o.samples = 4;
    o.bounces = 3;
    return o;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_render_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ---- scene model ----
    {
        render::Scene sc = render::defaultScene();
        CHECK(sc.objects.size() == 1 && sc.objects[0].type == "plane" && sc.lights.size() == 1);
        CHECK(sc.uniqueName("sphere") == "sphere1");
        render::Object o;
        o.name = "ball";
        o.type = "sphere";
        o.radius = 1;
        sc.objects.push_back(o);
        const mesh::Mesh wm = render::worldMesh(o);
        const mesh::Bounds b = mesh::bounds(wm);
        CHECK(std::fabs(b.min.z) < 1e-9 && std::fabs(b.max.z - 2) < 1e-9);  // sits on the ground
        CHECK(wm.hasNormals());
        render::Material m;
        CHECK(render::applyPreset(m, "gold") && m.metallic == 1);
        CHECK(!render::applyPreset(m, "nope"));

        // camera fit puts the target at the ball's centre
        render::Camera cam = sc.camera;
        render::fitCamera(sc, cam);
        CHECK(std::fabs(cam.target.z - 1) < 1e-9);

        // JSON round trip
        sc.objects.back().material.color = {0.5, 0.1, 0.1};
        sc.objects.back().material.transmission = 1;
        const std::string text = render::toJson(sc);
        render::Scene back = render::fromJson(text);
        CHECK(back.objects.size() == 2 && back.objects[1].name == "ball" && back.objects[1].radius == 1);
        CHECK(std::fabs(back.objects[1].material.color.x - 0.5) < 0.01 && back.objects[1].material.transmission == 1);
        CHECK(back.lights[0].type == "sun");
        bool threw = false;
        try {
            render::fromJson("{\"objects\":[{\"type\":\"blob\"}]}");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);

        // both renderers
        for (const char* mode : {"path", "preview"}) {
            render::RenderStats st;
            raster::Image img = render::renderScene(sc, small(mode), &st);
            CHECK(img.width() == 48 && img.height() == 36);
            CHECK(st.triangles > 100);
            const double centre = brightness(img, 20, 14, 28, 22);
            CHECK(centre > 5 && centre < 250);
        }
        // a transparent sky gives alpha
        sc.sky.transparent = true;
        sc.objects.back().material.transmission = 0;
        sc.objects.erase(sc.objects.begin());  // no ground
        for (const char* mode : {"path", "preview"}) {
            raster::Image img = render::renderScene(sc, small(mode));
            CHECK(img.at(0, 0).a == 0 && img.at(24, 18).a == 255);
        }
    }

    // ---- lighting sanity: shadows and emission ----
    {
        render::Scene sc = render::defaultScene();
        sc.lights[0].position = {0, 0, 10};  // sun straight down
        sc.lights[0].radius = 0.5;
        sc.sky.strength = 0.2;
        render::Object roof;
        roof.name = "roof";
        roof.type = "box";
        roof.size = {2, 6, 0.1};
        roof.position = {0, 0, 1};
        sc.objects.push_back(roof);
        sc.camera.position = {0, 0, 10};
        sc.camera.target = {0, 0, 0};
        sc.camera.up = {0, 1, 0};
        sc.camera.autoFit = false;
        sc.camera.orthographic = true;
        sc.camera.orthoSize = 4;
        for (const char* mode : {"path", "preview"}) {
            raster::Image img = render::renderScene(sc, small(mode, 64, 64));
            // the roof covers the middle column; far left/right ground is lit
            const double lit = brightness(img, 2, 28, 10, 36);
            const double roofTop = brightness(img, 28, 28, 36, 36);
            CHECK(lit > 60 && roofTop > 60);
        }
        // look from below the roof's edge sideways is hard to test; instead check a shadowed patch under a canopy seen at an angle
        sc.objects[1].size = {6, 6, 0.1};
        sc.objects[1].position = {0, 0, 0.5};
        sc.camera.position = {0, -2.9, 0.25};
        sc.camera.target = {0, 0, 0.25};
        sc.camera.up = {0, 0, 1};
        sc.camera.orthographic = false;
        sc.camera.fov = 20;
        for (const char* mode : {"path", "preview"}) {
            raster::Image under = render::renderScene(sc, small(mode, 64, 64));
            render::Scene noRoof = sc;
            noRoof.objects[1].visible = false;
            raster::Image open = render::renderScene(noRoof, small(mode, 64, 64));
            const double shaded = brightness(under, 24, 40, 40, 50), sunny = brightness(open, 24, 40, 40, 50);
            CHECK(shaded + 20 < sunny);
        }
        // an emissive ball is bright even in the dark
        render::Scene dark;
        dark.sky.type = "none";
        render::Object glow;
        glow.name = "glow";
        glow.type = "sphere";
        render::applyPreset(glow.material, "emissive");
        dark.objects.push_back(glow);
        raster::Image img = render::renderScene(dark, small("path"));
        CHECK(brightness(img, 22, 16, 26, 20) > 150 && brightness(img, 0, 0, 4, 4) < 5);
    }

    // ---- commands ----
    Session s;
    render::registerCommands(s);
    {
        Result n = s.run("render.new", {{"width", std::int64_t{64}}, {"height", std::int64_t{48}}});
        CHECK(n.ok && str(n, "doc") == "scene1");
        Result a = s.run("render.add", {{"doc", std::string("scene1")}, {"type", std::string("sphere")}, {"radius", 0.7},
                                        {"preset", std::string("gold")}, {"preview", true}});
        CHECK(a.ok && str(a, "name") == "sphere1" && a.attachments.size() == 1 && a.attachments[0].mime == "image/png");
        CHECK(s.run("render.add", {{"doc", std::string("scene1")}, {"type", std::string("box")}, {"name", std::string("crate")},
                                   {"size", NumberList{1, 1, 1}}, {"position", NumberList{1.5, 0, 0}}, {"color", std::string("#2266cc")}})
                  .ok);
        CHECK(!s.run("render.add", {{"doc", std::string("scene1")}, {"type", std::string("box")}, {"name", std::string("crate")}}).ok);
        CHECK(!s.run("render.add", {{"doc", std::string("scene1")}, {"type", std::string("box")}, {"color", std::string("nocolour")}}).ok);
        CHECK(!s.run("render.add", {{"doc", std::string("nope")}, {"type", std::string("box")}}).ok);

        // a texture on the crate
        raster::Image checker(8, 8);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) checker.set(x, y, ((x + y) % 2) ? raster::Color{255, 255, 255, 255} : raster::Color{20, 20, 20, 255});
        }
        raster::save(checker, dir / "checker.png");
        Result tex = s.run("render.material", {{"doc", std::string("scene1")}, {"name", std::string("crate")},
                                               {"texture", (dir / "checker.png").string()}, {"roughness", 0.4}});
        CHECK(tex.ok);
        CHECK(!s.run("render.material", {{"doc", std::string("scene1")}, {"name", std::string("crate")}, {"roughness", 2.0}}).ok);
        CHECK(!s.run("render.material", {{"doc", std::string("scene1")}, {"name", std::string("ghost")}, {"roughness", 0.2}}).ok);

        // a mesh from an open mesh document, and one from a file
        auto md = mesh::newMesh(mesh::torus(1, 0.3, 24, 12));
        const std::string meshId = s.workspace().add(md, "mesh");
        Result tm = s.run("render.add", {{"doc", std::string("scene1")}, {"mesh", meshId}, {"position", NumberList{-2, 0, 0}},
                                         {"preset", std::string("glass")}});
        CHECK(tm.ok && str(tm, "name") == "mesh1");
        mesh::save(dir / "cone.stl", mesh::cone(0.5, 0, 1, 16, false));
        CHECK(s.run("render.add", {{"doc", std::string("scene1")}, {"path", (dir / "cone.stl").string()}, {"position", NumberList{0, 2, 0}}}).ok);

        CHECK(s.run("render.light", {{"doc", std::string("scene1")}, {"type", std::string("area")}, {"name", std::string("soft")}}).ok);
        CHECK(s.run("render.light", {{"doc", std::string("scene1")}, {"type", std::string("point")}, {"color", std::string("orange")}}).ok);
        CHECK(s.run("render.light", {{"doc", std::string("scene1")}, {"name", std::string("soft")}, {"intensity", 50.0}}).ok);
        CHECK(!s.run("render.light", {{"doc", std::string("scene1")}, {"name", std::string("crate")}}).ok);
        CHECK(s.run("render.sky", {{"doc", std::string("scene1")}, {"zenith", std::string("#3366aa")}, {"strength", 0.8}}).ok);

        Result tr = s.run("render.transform", {{"doc", std::string("scene1")}, {"name", std::string("crate")}, {"turn", NumberList{0, 0, 30}}});
        CHECK(tr.ok);
        CHECK(s.run("render.camera", {{"doc", std::string("scene1")}, {"view", std::string("front")}}).ok);
        CHECK(s.run("render.camera", {{"doc", std::string("scene1")}, {"orbit", 30.0}, {"fov", 35.0}}).ok);
        CHECK(!s.run("render.camera", {{"doc", std::string("scene1")}, {"fov", 500.0}}).ok);

        Result list = s.run("render.list", {{"doc", std::string("scene1")}});
        CHECK(list.ok && str(list, "objects") == "ground,sphere1,crate,mesh1,mesh2" && str(list, "lights") == "sun,soft,point1");
        Result info = s.run("render.info", {{"doc", std::string("scene1")}});
        CHECK(info.ok && intField(info, "triangles") > 1000 && intField(info, "lights") == 3);

        // undo the last transform
        const std::size_t before = s.history().labels().size();
        CHECK(before > 5);
        CHECK(s.history().undo());
        CHECK(s.history().undo());
        CHECK(s.history().undo());  // the camera view and the crate's turn
        auto doc = s.workspace().getAs<render::SceneDoc>("scene1");
        CHECK(doc->scene.findObject("crate")->rotation.z == 0);
        CHECK(doc->scene.camera.autoFit);
        CHECK(s.history().redo());

        // render to files
        Result r = s.run("render.render", {{"doc", std::string("scene1")}, {"output", (dir / "shot.png").string()},
                                           {"samples", std::int64_t{2}}, {"width", std::int64_t{40}}});
        CHECK(r.ok && fs::exists(dir / "shot.png") && intField(r, "height") == 30 && r.attachments.empty());
        CHECK(raster::load(dir / "shot.png").width() == 40);
        Result j = s.run("render.render", {{"doc", std::string("scene1")}, {"output", (dir / "shot.jpg").string()},
                                           {"mode", std::string("preview")}, {"width", std::int64_t{40}}, {"preview", true}});
        CHECK(j.ok && fs::exists(dir / "shot.jpg") && j.attachments.size() == 1);
        Result bare = s.run("render.render", {{"doc", std::string("scene1")}, {"mode", std::string("preview")}, {"width", std::int64_t{32}}});
        CHECK(bare.ok && bare.attachments.size() == 1);
        CHECK(!s.run("render.render", {{"doc", std::string("scene1")}, {"output", (dir / "x.txt").string()}}).ok);
        CHECK(!s.run("render.render", {{"doc", std::string("scene1")}, {"samples", std::int64_t{0}}}).ok);
        Result pv = s.run("render.preview", {{"doc", std::string("scene1")}, {"size", std::int64_t{64}}});
        CHECK(pv.ok && pv.attachments.size() == 1);
        CHECK(raster::decode(pv.attachments[0].bytes).width() == 64);

        // save and reopen: the document mesh becomes a .glb beside the scene
        Result sv = s.run("render.save", {{"doc", std::string("scene1")}, {"path", (dir / "shot.scene.json").string()}});
        CHECK(sv.ok && fs::exists(dir / "shot.scene.json") && fs::exists(dir / "shot.mesh1.glb"));
        Result op = s.run("render.open", {{"path", (dir / "shot.scene.json").string()}});
        CHECK(op.ok && str(op, "doc") == "scene2");
        auto reopened = s.workspace().getAs<render::SceneDoc>("scene2");
        CHECK(reopened->scene.objects.size() == 5 && reopened->scene.lights.size() == 3);
        CHECK(reopened->scene.findObject("mesh1")->mesh->triangleCount() == md->mesh.triangleCount());
        CHECK(reopened->scene.findObject("mesh2")->centered);
        CHECK(!reopened->scene.findObject("crate")->material.texture.empty());
        // the file-backed cone keeps its relative path and stays centred
        {
            std::ifstream in(dir / "shot.scene.json");
            const json sj = json::parse(in);
            CHECK(sj["objects"][4]["path"] == "cone.stl");
        }
        const mesh::Bounds cb = mesh::bounds(render::worldMesh(*reopened->scene.findObject("mesh2")));
        CHECK(std::fabs(cb.min.z) < 1e-6 && std::fabs(cb.center().y - 2) < 1e-6);
        CHECK(!s.run("render.open", {{"path", (dir / "missing.scene.json").string()}}).ok);
        // read-only commands take a scene file directly
        Result fromFile = s.run("render.render", {{"scene", (dir / "shot.scene.json").string()}, {"mode", std::string("preview")},
                                                  {"width", std::int64_t{24}}, {"output", (dir / "file.png").string()}});
        CHECK(fromFile.ok && fs::exists(dir / "file.png"));
        CHECK(s.run("render.info", {{"scene", (dir / "shot.scene.json").string()}}).ok);
        CHECK(!s.run("render.info", {}).ok);
        Result model = s.run("render.model", {{"path", (dir / "cone.stl").string()}, {"output", (dir / "cone.png").string()},
                                              {"samples", std::int64_t{2}}, {"width", std::int64_t{32}}, {"preset", std::string("copper")}});
        CHECK(model.ok && raster::load(dir / "cone.png").height() == 24);

        // turntable frames, and a gif when ffmpeg is around
        Params tt = {{"doc", std::string("scene1")}, {"output_dir", (dir / "spin").string()}, {"frames", std::int64_t{3}},
                     {"width", std::int64_t{32}}, {"height", std::int64_t{24}}};
        const bool ff = findProgram("ffmpeg").has_value();
        if (ff) tt["video"] = (dir / "spin.gif").string();
        Result t = s.run("render.turntable", tt);
        CHECK(t.ok && fs::exists(dir / "spin" / "frame_000.png") && fs::exists(dir / "spin" / "frame_002.png"));
        if (ff) {
            CHECK(fs::exists(dir / "spin.gif") && fs::file_size(dir / "spin.gif") > 100);
            Params mp = tt;
            mp["video"] = (dir / "spin.mp4").string();
            mp["output_dir"] = (dir / "spin2").string();
            mp["width"] = std::int64_t{33};
            Result m4 = s.run("render.turntable", mp);
            CHECK(m4.ok && fs::exists(dir / "spin.mp4"));
        } else {
            Params bad = tt;
            bad["video"] = (dir / "spin.mp4").string();
            CHECK(!s.run("render.turntable", bad).ok);
        }

        CHECK(s.run("render.close", {{"doc", std::string("scene2")}}).ok);
        CHECK(!s.workspace().get("scene2"));
    }

    // ---- MCP ----
    {
        Session ms;
        render::registerCommands(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "render"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        bool sawRender = false;
        for (const auto& t : list) {
            if (t["name"] == "render_info") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (t["name"] == "render_render") CHECK(t["annotations"]["readOnlyHint"] == false);
            sawRender = sawRender || t["name"] == "render_render";
        }
        CHECK(sawRender && list.size() >= 16);
        json n = call(server, 2, "render_new", {{"width", 64}, {"height", 48}});
        CHECK(n["isError"] == false && n["structuredContent"]["doc"] == "scene1");
        json a = call(server, 3, "render_add", {{"doc", "scene1"}, {"type", "torus"}, {"radius", 1}, {"preset", "copper"}, {"preview", true}});
        CHECK(a["isError"] == false && a["content"].size() == 2 && a["content"][1]["type"] == "image");
        json r = call(server, 4, "render_render", {{"doc", "scene1"}, {"samples", 2}, {"width", 32}});
        CHECK(r["isError"] == false && r["content"].size() == 2 && r["content"][1]["mimeType"] == "image/png");
        json p = call(server, 5, "render_preview", {{"doc", "scene1"}, {"size", 48}});
        CHECK(p["isError"] == false && p["content"][1]["type"] == "image");
        CHECK(call(server, 6, "undo", json::object())["isError"] == false);
    }

    fs::remove_all(dir);
    return check::summary("render");
}
