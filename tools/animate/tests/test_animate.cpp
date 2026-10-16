#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "../../../core/tests/check.hpp"
#include "wetwaresuite/animate/animate.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "stb_image.h"

using namespace wetwaresuite;
using namespace wetwaresuite::animate;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }
bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

Result run(Session& s, const std::string& line) {
    cli::Batch b(s);
    Result r = b.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

double num(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return NAN;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return NAN;
}

fs::path scratch(const std::string& name) {
    fs::path d = fs::temp_directory_path() / ("wetwaresuite-test-animate-" + name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void testEasing() {
    CHECK(near(ease(Ease::Linear, 0.25), 0.25));
    CHECK(near(ease(Ease::EaseInOut, 0.5), 0.5));
    CHECK(ease(Ease::EaseIn, 0.3) < 0.3);
    CHECK(ease(Ease::EaseOut, 0.3) > 0.3);
    CHECK(ease(Ease::Step, 0.99) == 0 && ease(Ease::Step, 1.0) == 1);
    CHECK(near(cubicBezier(0, 0, 1, 1, 0.4), 0.4));
    CHECK(cubicBezier(0.34, 1.56, 0.64, 1, 0.6) > 1.0);  // overshoot
    CHECK(parseEase("hold") == Ease::Step && !parseEase("wobble"));
}

void testModel() {
    Settings s;
    Element e = makeElement("rect", s);
    CHECK(numberAt(e, "opacity", 0) == 1 && numberAt(e, "width", 0) == 100);
    Key a;
    a.frame = 0;
    a.number = 0;
    Key b;
    b.frame = 10;
    b.number = 100;
    setKey(e, "x", b);
    setKey(e, "x", a);
    CHECK(e.keys["x"].size() == 2 && e.keys["x"][0].frame == 0);
    CHECK(near(numberAt(e, "x", 5), 50));
    CHECK(near(numberAt(e, "x", -3), 0) && near(numberAt(e, "x", 20), 100));
    Key c;
    c.frame = 0;
    c.color = {0, 0, 0, 255};
    Key d;
    d.frame = 10;
    d.color = {200, 100, 0, 255};
    setKey(e, "color", c);
    setKey(e, "color", d);
    Color m = colorAt(e, "color", 5);
    CHECK(m.r == 100 && m.g == 50);

    Element cel = makeElement("cel", s);
    cel.cels[4] = std::make_shared<const Image>(2, 2);
    cel.cels[8] = std::make_shared<const Image>(3, 3);
    int start = -1;
    CHECK(!celAt(cel, 3) && celAt(cel, 6, &start)->width() == 2 && start == 4 && celAt(cel, 100)->width() == 3);

    Affine t{1, 0, 0, 1, 10, 20};
    Affine r{0, 1, -1, 0, 0, 0};
    double x, y;
    (t * r).apply(5, 0, x, y);
    CHECK(near(x, 10) && near(y, 25));
    (t * r).inverse().apply(x, y, x, y);
    CHECK(near(x, 5) && near(y, 0));
}

void testCommands() {
    Session s;
    registerCommands(s);
    Result r = run(s, "animate.new width=200 height=100 frames=25 fps=12 background=white");
    CHECK(r.ok && toText(r.value) == "anim1" && toText(*r.field("layer")) == "L1");
    r = run(s, "animate.element_add doc=anim1 kind=rect width=20 height=20 x=20 y=50 color=red name=box");
    CHECK(r.ok && toText(r.value) == "e1");
    CHECK(run(s, "animate.element_set doc=anim1 element=box x=180 frame=24 ease=ease-in").ok);
    Result r2 = run(s, "animate.values doc=anim1 element=e1 frame=12");
    CHECK(r2.ok && num(r2, "x") < 100 && num(r2, "x") > 20);  // auto key at frame 0 = 20, eased in
    CHECK(run(s, "animate.key_set doc=anim1 element=e1 property=x frame=24 value=180 ease=linear").ok);
    r = run(s, "animate.values doc=anim1 element=e1 frame=12");
    CHECK(near(num(r, "x"), 100));
    CHECK(!run(s, "animate.element_set doc=anim1 element=e1 x=5").ok);  // keyframed: needs frame=
    CHECK(!run(s, "animate.key_set doc=anim1 element=e1 property=font_size frame=0 value=3").ok);
    CHECK(!run(s, "animate.key_set doc=anim1 element=e1 property=x frame=99 value=3").ok);
    CHECK(run(s, "animate.key_set doc=anim1 element=e1 property=color frame=24 value=#0000ff").ok);

    auto doc = s.workspace().getAs<AnimDoc>("anim1");
    Image f0 = renderFrame(*doc, 0);
    Color c = f0.at(20, 50);
    CHECK(c.r > 200 && c.g < 40 && c.b < 40);
    CHECK(f0.at(100, 50).r == 255 && f0.at(100, 50).g == 255);
    Image f24 = renderFrame(*doc, 24);
    c = f24.at(180, 50);
    CHECK(c.b > 200 && c.r < 40);

    // bones: a child on the tip of a bone follows its rotation
    CHECK(run(s, "animate.element_add doc=anim1 kind=bone x=100 y=10 length=40 name=arm").ok);
    r = run(s, "animate.element_add doc=anim1 kind=ellipse width=10 height=10 parent=arm x=40 y=0 color=black");
    CHECK(r.ok);
    const std::string hand = toText(r.value);
    r = run(s, "animate.values doc=anim1 element=" + hand + " frame=0");
    CHECK(near(num(r, "canvas_x"), 140) && near(num(r, "canvas_y"), 10));
    CHECK(run(s, "animate.element_set doc=anim1 element=arm rotation=90").ok);
    r = run(s, "animate.values doc=anim1 element=" + hand + " frame=0");
    CHECK(near(num(r, "canvas_x"), 100) && near(num(r, "canvas_y"), 50));
    CHECK(!run(s, "animate.element_set doc=anim1 element=arm parent=" + hand).ok);  // loop
    c = renderFrame(*doc, 0).at(100, 50);
    CHECK(c.r < 60 && c.g < 60);

    // text draws something dark
    CHECK(run(s, "animate.element_add doc=anim1 kind=text text=Hello font_size=24 x=60 y=85 color=black").ok);
    int dark = 0;
    Image ft = renderFrame(*doc, 0);
    for (int y = 70; y < 100; ++y) {
        for (int x = 30; x < 90; ++x) dark += ft.at(x, y).r < 100;
    }
    CHECK(dark > 30);

    // undo the text
    CHECK(s.history().undo());
    CHECK(doc->elementCount() == 3);

    // cels: draw two drawings, the first holds until the second
    r = run(s, "animate.element_add doc=anim1 kind=cel");
    const std::string cel = toText(r.value);
    CHECK(run(s, "animate.draw doc=anim1 element=" + cel + " frame=0 points=10,10,60,10 size=6 color=green").ok);
    CHECK(run(s, "animate.draw doc=anim1 element=" + cel + " frame=6 points=10,90,60,90 size=6 color=green").ok);
    CHECK(renderFrame(*doc, 3).at(30, 10).g > 100 && renderFrame(*doc, 3).at(30, 10).r < 50);
    CHECK(renderFrame(*doc, 7).at(30, 10).r == 255);
    FrameOptions onion;
    onion.onion = 1;
    c = renderFrame(*doc, 7, onion).at(30, 10);
    CHECK(c.r > c.g && c.r < 255);  // red ghost of the drawing before
    CHECK(run(s, "animate.cel doc=anim1 element=" + cel + " action=copy from=0 frame=12").ok);
    CHECK(renderFrame(*doc, 12).at(30, 10).g > 100);
    CHECK(run(s, "animate.cel doc=anim1 element=" + cel + " action=blank frame=18").ok);
    CHECK(renderFrame(*doc, 20).at(30, 10).r == 255);
    r = run(s, "animate.preview doc=anim1 frames=0,6,12,18");
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    r = run(s, "animate.preview doc=anim1 frame=7 onion=1 bones=true");
    CHECK(r.ok && r.attachments.size() == 1);
    CHECK(run(s, "animate.info doc=anim1").ok);
    CHECK(run(s, "animate.key_list doc=anim1").ok);

    // save and reopen
    fs::path dir = scratch("cmds");
    const std::string file = (dir / "t.anim.json").string();
    CHECK(run(s, "animate.save doc=anim1 path=\"" + file + "\"").ok);
    r = run(s, "animate.open path=\"" + file + "\"");
    CHECK(r.ok && toText(r.value) == "anim2");
    auto back = s.workspace().getAs<AnimDoc>("anim2");
    CHECK(back->elementCount() == doc->elementCount());
    Image a = renderFrame(*doc, 13), b = renderFrame(*back, 13);
    int diff = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) diff += std::abs(a.at(x, y).g - b.at(x, y).g) > 2;
    }
    CHECK(diff == 0);

    // exports
    r = run(s, "animate.export_png doc=anim1 dir=\"" + (dir / "seq").string() + "\" step=6");
    CHECK(r.ok && num(r, "frames") == 5 && fs::exists(dir / "seq" / "frame_0024.png"));
    r = run(s, "animate.export_sheet doc=anim1 output=\"" + (dir / "sheet.png").string() + "\" columns=5 end=9 padding=2");
    CHECK(r.ok && num(r, "width") == 5 * 200 + 4 * 2 && num(r, "rows") == 2);
    auto atlas = nlohmann::json::parse(slurp(dir / "sheet.json"));
    CHECK(atlas["frames"].size() == 10 && atlas["frames"]["sheet_0006"]["frame"]["x"] == 202);
    CHECK(atlas["meta"]["size"]["w"] == 1008 && atlas["animations"]["sheet"].size() == 10);
    r = run(s, "animate.export_gif doc=anim1 output=\"" + (dir / "a.gif").string() + "\" scale=0.5");
    CHECK(r.ok && num(r, "frames") == 25);
    const std::string gif = slurp(dir / "a.gif");
    int w = 0, h = 0, z = 0, comp = 0;
    int* delays = nullptr;
    unsigned char* px = stbi_load_gif_from_memory(reinterpret_cast<const unsigned char*>(gif.data()), static_cast<int>(gif.size()), &delays,
                                                  &w, &h, &z, &comp, 4);
    CHECK(px != nullptr && w == 100 && h == 50 && z == 25);
    if (px) {
        // frame 0: red box around (10, 25) at half size
        const unsigned char* p = px + (25 * w + 10) * 4;
        CHECK(p[0] > 180 && p[1] < 80);
        CHECK(delays && delays[0] >= 80 && delays[0] <= 90);
        stbi_image_free(px);
        stbi_image_free(delays);
    }
    CHECK(run(s, "animate.render doc=anim1 frame=3 output=\"" + (dir / "f3.png").string() + "\"").ok);
    CHECK(raster::load(dir / "f3.png").width() == 200);
    fs::remove_all(dir);
}

void testGifEdgeCases() {
    // many colours, transparency and a large frame exercise the palette and LZW resets
    Image big(300, 200, Color{0, 0, 0, 0});
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 300; ++x) {
            if ((x / 10 + y / 10) % 7 == 0) continue;
            big.set(x, y, Color{static_cast<std::uint8_t>(x * 255 / 299), static_cast<std::uint8_t>(y * 255 / 199),
                                static_cast<std::uint8_t>((x * y) % 256), 255});
        }
    }
    for (bool dither : {false, true}) {
        const std::string g = encodeGif({big, big}, {5, 5}, 0, dither);
        int w = 0, h = 0, z = 0, comp = 0;
        int* d = nullptr;
        unsigned char* px = stbi_load_gif_from_memory(reinterpret_cast<const unsigned char*>(g.data()), static_cast<int>(g.size()), &d, &w,
                                                      &h, &z, &comp, 4);
        CHECK(px && w == 300 && h == 200 && z == 2);
        if (!px) continue;
        double err = 0;
        int transparentOk = 0;
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 300; ++x) {
                const unsigned char* p = px + (y * 300 + x) * 4;
                const Color o = big.at(x, y);
                if (o.a == 0) {
                    transparentOk += p[3] == 0;
                    continue;
                }
                err += std::abs(p[0] - o.r) + std::abs(p[1] - o.g) + std::abs(p[2] - o.b);
            }
        }
        CHECK(err / (300 * 200 * 3) < 12);
        CHECK(transparentOk > 5000);
        stbi_image_free(px);
        stbi_image_free(d);
    }
    const std::string one = encodeGif({Image(1, 1, Color{9, 9, 9, 255})}, {10});
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(one.data()), static_cast<int>(one.size()), &w, &h, &comp, 4);
    CHECK(px && w == 1 && px[0] == 9);
    if (px) stbi_image_free(px);
}

void test3d() {
    Session s;
    registerCommands(s);
    CHECK(run(s, "animate.new width=160 height=120 frames=10 layer=3d background=black").ok);
    CHECK(!run(s, "animate.element_add doc=anim1 kind=rect layer=L1").ok);
    Result r = run(s, "animate.element_add doc=anim1 kind=mesh source=box size=2 color=orange");
    CHECK(r.ok);
    auto doc = s.workspace().getAs<AnimDoc>("anim1");
    // no camera: auto-framed, the box sits in the middle
    Image img = renderFrame(*doc, 0);
    CHECK(img.at(80, 60).r > 60 && img.at(2, 2).r == 0);
    // a camera looking at the box from the front, then the box moves right
    CHECK(run(s, "animate.element_add doc=anim1 kind=camera x=0 y=-10 z=0 fov=40").ok);
    CHECK(run(s, "animate.element_set doc=anim1 element=e1 x=0 frame=0").ok);
    CHECK(run(s, "animate.element_set doc=anim1 element=e1 x=3 frame=9 rotation=45").ok);
    img = renderFrame(*doc, 0);
    CHECK(img.at(80, 60).r > 60);
    Image end = renderFrame(*doc, 9);
    // box moved right: centre column empty-ish, right side lit
    int left = 0, right = 0;
    for (int y = 0; y < 120; ++y) {
        for (int x = 0; x < 160; ++x) (x < 80 ? left : right) += end.at(x, y).r > 40;
    }
    CHECK(right > left * 3);
    // the box behind the camera is clipped away, not drawn
    CHECK(run(s, "animate.element_add doc=anim1 kind=mesh source=sphere y=-20 color=white").ok);
    img = renderFrame(*doc, 0);
    CHECK(img.at(2, 2).r == 0);
    // camera inside the cube's far side still renders without crashing
    CHECK(run(s, "animate.element_set doc=anim1 element=e3 y=-10.5").ok);
    renderFrame(*doc, 0);
    r = run(s, "animate.values doc=anim1 element=e1 frame=9");
    CHECK(near(num(r, "world_x"), 3) && near(num(r, "rotation"), 45));
    r = run(s, "animate.preview doc=anim1 every=3");
    CHECK(r.ok && r.attachments.size() == 1);
}

void testVideo() {
    if (!findProgram("ffmpeg")) {
        std::printf("ffmpeg not found; skipping video export\n");
        Session s;
        registerCommands(s);
        run(s, "animate.new");
        Result r = run(s, "animate.export_video doc=anim1 output=x.mp4");
        CHECK(!r.ok && contains(r.message, "ffmpeg"));
        return;
    }
    Session s;
    registerCommands(s);
    CHECK(run(s, "animate.new width=121 height=81 frames=12 fps=12 background=transparent").ok);
    CHECK(run(s, "animate.element_add doc=anim1 kind=ellipse width=30 height=30 x=20 y=40").ok);
    CHECK(run(s, "animate.element_set doc=anim1 element=e1 x=100 frame=11").ok);
    fs::path dir = scratch("video");
    for (const char* ext : {"mp4", "webm"}) {
        const fs::path out = dir / (std::string("v.") + ext);
        Result r = run(s, "animate.export_video doc=anim1 quality=draft output=\"" + out.string() + "\"");
        CHECK(r.ok && fs::file_size(out) > 100);
        if (auto probe = findProgram("ffprobe")) {
            ProcessResult pr = runProgram({probe->string(), "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                                           "stream=nb_read_frames,width", "-of", "csv=p=0", out.string()});
            CHECK(pr.exitCode == 0 && contains(pr.output, "12"));
        }
    }
    fs::remove_all(dir);
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
    for (const char* tool : {"animate_new", "animate_open", "animate_save", "animate_element_add", "animate_element_set", "animate_key_set",
                             "animate_key_delete", "animate_key_list", "animate_preview", "animate_render", "animate_export_png",
                             "animate_export_sheet", "animate_export_gif", "animate_export_video", "animate_draw", "animate_cel",
                             "animate_layer_add", "animate_values"}) {
        CHECK(contains(list, std::string("\"") + tool + "\""));
    }
    std::string r = call("animate_new", R"({"width":100,"height":80})");
    CHECK(contains(r, "\"doc\":\"anim1\""));
    r = call("animate_element_add", R"({"doc":"anim1","kind":"rect","x":50,"y":40,"preview":true})");
    CHECK(contains(r, "\"isError\":false") && contains(r, "\"type\":\"image\""));
    r = call("animate_key_set", R"({"doc":"anim1","element":"e1","property":"rotation","frame":10,"value":90,"ease":"ease-out"})");
    CHECK(contains(r, "\"isError\":false"));
    r = call("animate_key_list", R"({"doc":"anim1","element":"e1"})");
    CHECK(contains(r, "10=90 ease-out"));
    r = call("undo", "{}");
    CHECK(contains(r, "undone"));
    CHECK(session.workspace().getAs<AnimDoc>("anim1")->element("e1")->keys.empty());
    r = call("animate_preview", R"({"doc":"anim1","frames":[0,5]})");
    CHECK(contains(r, "\"type\":\"image\""));
    r = call("animate_values", R"({"doc":"anim9","frame":0})");
    CHECK(contains(r, "\"isError\":true"));
}

}  // namespace

int main() {
    testEasing();
    testModel();
    testCommands();
    testGifEdgeCases();
    test3d();
    testVideo();
    testMcp();
    return check::summary("animate");
}
