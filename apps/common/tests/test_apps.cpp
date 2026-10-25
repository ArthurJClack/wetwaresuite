// The app table, routing files to apps, the handoff manifest, and app.send /
// app.receive between two sessions, with stand-in tools (no tool libraries).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "app_commands.hpp"
#include "apps.hpp"
#include "check.hpp"
#include "gui_launch.hpp"
#include "handoff.hpp"
#include "launch.hpp"
#include "palette_commands.hpp"
#include "wetwaresuite/gui/handoff.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"

using namespace wetwaresuite;
namespace fs = std::filesystem;

namespace {

void setHome(const fs::path& p) {
#ifdef _WIN32
    _putenv_s("WETWARESUITE_HOME", p.string().c_str());
#else
    setenv("WETWARESUITE_HOME", p.string().c_str(), 1);
#endif
}

struct FakeDoc : Document {
    std::string k;
    explicit FakeDoc(std::string kind) : k(std::move(kind)) {}
    std::string kind() const override { return k; }
    std::string summary() const override { return k; }
};

// A stand-in "save" command: writes the doc's kind into the file.
void fakeSave(Session& s, const std::string& id, const std::string& pathParam = "path") {
    Session* sp = &s;
    s.registry().add({id, id, "test save", {{"doc", ParamType::String, "doc"}, {pathParam, ParamType::String, "file"}}},
                     [sp, pathParam](const Params& p) {
                         return std::make_unique<FunctionAction>([sp, p, pathParam] {
                             auto doc = sp->workspace().get(stringParam(p, "doc"));
                             if (!doc) return Result::failure("no doc");
                             std::ofstream(stringParam(p, pathParam)) << doc->kind();
                             return Result::success("saved");
                         });
                     });
}

// A stand-in "open" command: adds a doc of `kind` and returns its id.
void fakeOpen(Session& s, const std::string& id, const std::string& kind, const std::string& prefix) {
    Session* sp = &s;
    s.registry().add({id, id, "test open", {{"path", ParamType::String, "file"}}}, [sp, kind, prefix](const Params& p) {
        return std::make_unique<FunctionAction>([sp, kind, prefix, p] {
            if (!fs::exists(stringParam(p, "path"))) return Result::failure("missing");
            std::string id = sp->workspace().add(std::make_shared<FakeDoc>(kind), prefix);
            return Result::success("opened " + id, id).with("doc", id);
        });
    });
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// a stand-in 3D document: draws a solid colour so app.view's plumbing can be checked without a tool
class Fake3d : public Document {
public:
    std::string kind() const override { return "mesh"; }
    std::string summary() const override { return "fake"; }
    bool renderView(const OrbitView& v, int w, int h, std::vector<std::uint8_t>& rgba) const override {
        last = v;
        if (w == 0 && h == 0) return true;
        rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 200);
        return true;
    }
    mutable OrbitView last;
};
class Flat : public Document {
public:
    std::string kind() const override { return "image"; }
    std::string summary() const override { return "flat"; }
};

int main() {
    auto home = fs::temp_directory_path() / "wetwaresuite_test_apps";
    fs::remove_all(home);
    fs::create_directories(home);
    setHome(home);

    // ---- the table: every tool in exactly one app, and the hub agrees ----
    {
        CHECK(apps::all().size() == 13);
        CHECK(apps::appOfTool("paint")->id == "image");
        CHECK(apps::appOfTool("slicer")->id == "cad");
        CHECK(apps::appOfTool("toolkit")->id == "files");
        CHECK(apps::appOfTool("nope") == nullptr);
        CHECK(apps::find("files")->always);
        CHECK(apps::binaryName("model") == "wetwaresuite-model");
        for (const auto& a : apps::all()) CHECK(!apps::appInstructions(a.id).empty());

        std::string catalog = readFile(fs::path(WETWARESUITE_SOURCE_DIR) / "hub" / "src" / "catalog.rs");
        std::regex row(R"re(App \{ id: "([a-z]+)", summary: "[^"]*", tools: &\[([^\]]*)\])re");
        std::size_t rows = 0;
        for (std::sregex_iterator it(catalog.begin(), catalog.end(), row), end; it != end; ++it) {
            ++rows;
            const apps::App* app = apps::find((*it)[1]);
            CHECK(app != nullptr);
            if (!app) continue;
            std::string tools;
            for (const auto& t : app->tools) tools += (tools.empty() ? "" : ", ") + ("\"" + t + "\"");
            if (tools != (*it)[2].str()) std::printf("hub and C++ disagree on %s: %s\n", app->id.c_str(), (*it)[2].str().c_str());
            CHECK(tools == (*it)[2].str());
        }
        CHECK(rows == apps::all().size());
    }

    // ---- routing by extension ----
    {
        CHECK(apps::routeForFile("holiday.JPG").app == "image");
        CHECK(apps::routeForFile("holiday.JPG").command == "photo.open");
        CHECK(apps::routeForFile("/x/y/logo.svg").app == "draw");
        CHECK(apps::routeForFile("head.obj").app == "model");
        CHECK(apps::routeForFile("part.stl").app == "cad");
        CHECK(apps::routeForFile("cave.tmj").app == "game");
        CHECK(apps::routeForFile("scan.tif").command == "photo.open");  // a plain TIFF is a picture
        CHECK(apps::routeForFile("art.psd").command == "photo.open");
        CHECK(apps::routeForFile("pic.webp").command == "photo.open");
        {  // a TIFF with map tags (ModelPixelScale) is a GeoTIFF: elevation for maps
            const auto geo = std::filesystem::temp_directory_path() / "ww-route-geo.tif";
            std::ofstream(geo, std::ios::binary).write("II*\0\x08\0\0\0\x01\0\x0e\x83\x0c\0\x03\0\0\0\0\0\0\0\0\0\0\0", 26);
            CHECK(apps::routeForFile(geo.string()).app == "maps");
            std::filesystem::remove(geo);
        }
        CHECK(apps::routeForFile("song.beats.json").command == "beats.open");  // two-part extension wins
        CHECK(apps::routeForFile("board.board.json").app == "draw");
        CHECK(apps::routeForFile("voice.wav").app == "sound");
        CHECK(apps::routeForFile("clip.mp4").app == "video");
        CHECK(apps::routeForFile("budget.xlsx").command == "office.sheet_open");
        CHECK(apps::routeForFile("trail.gpx").app == "maps");
        CHECK(apps::routeForFile("blink.hex").app == "electronics");
        CHECK(apps::routeForFile("mystery.bin").app == "files");
        CHECK(apps::routeForFile("noext").app == "files");
        CHECK(apps::routeForFile("main.rs").command == "code.open");
        CHECK(apps::routeForFile("config.yaml").app == "system");
        CHECK(apps::routeForFile("backup.tar.gz").app == "system");
        CHECK(apps::routeForFile("readme.md").app == "office");  // documents stay in office
        CHECK(apps::sendRoute("image", "model")->ext == ".png");
        CHECK(apps::sendRoute("cad", "model")->ext == ".stl");
        CHECK(apps::sendRoute("sculpt", "game")->ext == ".glb");
        CHECK(apps::sendRoute("vector", "office")->ext == ".svg");
        CHECK(apps::sendRoute("song", "video")->docParam == "song");
        CHECK(!apps::sendRoute("image", "maps"));
        // every route's open command opens files of its own extension in its target app
        for (const auto& r : apps::sendRoutes()) {
            CHECK(apps::find(r.to) != nullptr);
            if (!r.open.empty()) CHECK(apps::appOfTool(r.open.substr(0, r.open.find('.')))->id == r.to);
        }
    }

    // ---- manifest round trip ----
    {
        fs::path dir = home / "manifest-test";
        CHECK(apps::readManifest(dir).empty());
        apps::Handoff h;
        h.from = "image";
        h.to = "model";
        h.kind = "texture";
        h.file = (dir / "wood \"oak\".png").string();
        h.hint = "use it";
        auto stored = apps::addHandoff(dir, h);
        CHECK(!stored.id.empty() && !stored.created.empty());
        apps::Handoff second = h;
        second.to = "game";
        apps::addHandoff(dir, second);
        auto items = apps::readManifest(dir);
        CHECK(items.size() == 2);
        CHECK(items[0].id == stored.id && items[0].file == h.file && items[0].kind == "texture" && !items[0].received);
        CHECK(items[1].to == "game" && items[1].id != items[0].id);
        items[0].received = true;
        apps::writeManifest(dir, items);
        CHECK(apps::readManifest(dir)[0].received);
        std::ofstream(dir / "manifest.json") << "{ not json";
        CHECK(apps::readManifest(dir).empty());  // a broken manifest reads as empty, never throws
    }

    // ---- app.send from image, app.receive in model ----
    {
        CHECK(apps::handoffDir() == home / "handoff");
        Session image;
        fakeSave(image, "photo.save");
        apps::registerCommands(image, "image");
        image.workspace().add(std::make_shared<FakeDoc>("image"), "img");
        Result sent = image.run("app.send", {{"doc", std::string("img1")}, {"to", std::string("model")}, {"name", std::string("wood")}});
        CHECK(sent.ok);
        std::string file = toText(*sent.field("file"));
        CHECK(fs::path(file).parent_path() == apps::handoffDir());
        CHECK(fs::path(file).filename() == "wood.png");
        CHECK(readFile(file) == "image");
        CHECK(sent.message.find("wetwaresuite-model app.receive") != std::string::npos);
        CHECK(image.run("app.send", {{"doc", std::string("img1")}, {"to", std::string("maps")}}).ok == false);
        CHECK(image.run("app.send", {{"to", std::string("model")}}).ok == false);

        Session model;
        apps::registerCommands(model, "model");
        Result inbox = model.run("app.inbox", {});
        CHECK(inbox.ok && std::get<std::int64_t>(inbox.value) == 1);
        Result got = model.run("app.receive", {});
        CHECK(got.ok);
        CHECK(toText(*got.field("file")) == file);
        CHECK(got.message.find("render.material") != std::string::npos);  // the hint says what to do
        CHECK(!model.run("app.receive", {}).ok);  // nothing left
        CHECK(std::get<std::int64_t>(model.run("app.inbox", {{"all", true}}).value) == 1);
        CHECK(fs::path(file).filename() == "wood.png");

        // a second send doesn't overwrite the first file
        Result again = image.run("app.send", {{"doc", std::string("img1")}, {"to", std::string("model")}, {"name", std::string("wood")}});
        CHECK(fs::path(toText(*again.field("file"))).filename() == "wood-2.png");
    }

    // ---- in the all-in-one binary the target opens right away ----
    {
        Session suite;
        fakeSave(suite, "vector.save");
        fakeOpen(suite, "photo.open", "image", "img");
        apps::registerCommands(suite, "suite");
        suite.workspace().add(std::make_shared<FakeDoc>("vector"), "vec");
        Result r = suite.run("app.send", {{"doc", std::string("vec1")}, {"to", std::string("image")}});
        CHECK(r.ok);
        CHECK(toText(r.value) == "img1");
        CHECK(suite.workspace().get("img1") != nullptr);
        bool received = false;
        for (const auto& h : apps::readManifest(apps::handoffDir()))
            if (h.file == toText(*r.field("file"))) received = h.received;
        CHECK(received);

        // a plain file goes too, and open_with routes it
        fs::path png = home / "photo.png";
        std::ofstream(png) << "png";
        Result w = suite.run("app.open_with", {{"path", png.string()}});
        CHECK(w.ok && toText(*w.field("app")) == "image" && toText(*w.field("command")) == "photo.open");
        Result o = suite.run("app.open_with", {{"path", png.string()}, {"open", true}});
        CHECK(o.ok && toText(o.value) == "img2");
        Result f = suite.run("app.send", {{"path", png.string()}, {"to", std::string("image")}});
        CHECK(f.ok && toText(f.value) == "img3");
        CHECK(suite.run("app.installed", {}).ok);
    }

    // ---- MCP: the app.* tools are there and the instructions explain handoff ----
    {
        Session s;
        apps::registerCommands(s, "image");
        mcp::Server server(s, {"wetwaresuite-image", apps::kVersion, apps::appInstructions("image") + apps::handoffInstructions("image")});
        auto init = nlohmann::json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"));
        std::string text = init["result"]["instructions"].get<std::string>();
        CHECK(text.find("app_send") != std::string::npos);
        CHECK(text.find("Not installed") != std::string::npos);
        auto tools = nlohmann::json::parse(*server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
        std::string names = tools["result"]["tools"].dump();
        for (const char* t : {"app_installed", "app_send", "app_receive", "app_inbox", "app_open_with"})
            CHECK(names.find(t) != std::string::npos);
    }

    // ---- window or command line: the decision is pure ----
    {
        using apps::LaunchKind;
        const std::set<std::string> files = {"photo.jpg", "b.png", "list", "my file.ora"};
        auto isFile = [&files](const std::string& a) { return files.count(a) > 0; };
        auto isCommand = [](const std::string& a) { return a == "photo.resize" || a == "app.receive"; };
        auto decide = [&](std::vector<std::string> args) { return apps::decideLaunch(args, isFile, isCommand); };
        CHECK(decide({}).kind == LaunchKind::Gui);  // double-click
        CHECK(decide({}).files.empty());
        auto dropped = decide({"photo.jpg", "my file.ora"});  // files dragged onto the program
        CHECK(dropped.kind == LaunchKind::Gui && (dropped.files == std::vector<std::string>{"photo.jpg", "my file.ora"}));
        auto opened = decide({"open", "b.png", "missing.png"});  // open doesn't check: the window says what's wrong
        CHECK(opened.kind == LaunchKind::Gui && opened.files.size() == 2);
        auto gui = decide({"gui", "--action", "photo.resize", "--screenshot", "s.bmp"});
        CHECK(gui.kind == LaunchKind::Gui && gui.action == "photo.resize" && gui.screenshot == "s.bmp" && gui.error.empty());
        CHECK(!decide({"gui", "--action"}).error.empty());
        CHECK(decide({"gui", "--all"}).all && !decide({"gui"}).all);  // --all: every tool in one window
        CHECK(!decide({"gui", "--nope"}).error.empty());
        CHECK(decide({"-psn_0_12345"}).kind == LaunchKind::Gui);  // old macOS Finder argument
        // the command line keeps every word it had, even when a file has that name
        for (const char* w : {"list", "help", "run", "batch", "mcp", "--version", "--help", "-h"})
            CHECK(decide({w}).kind == LaunchKind::Cli);
        CHECK(decide({"photo.resize", "doc=img1"}).kind == LaunchKind::Cli);
        CHECK(decide({"app.receive"}).kind == LaunchKind::Cli);
        CHECK(decide({"photo.jpg", "missing.png"}).kind == LaunchKind::Cli);  // not all files: let the CLI explain
        CHECK(decide({"photo.jpg", "--keep-going"}).kind == LaunchKind::Cli);
        CHECK(decide({"nonsense"}).kind == LaunchKind::Cli);
        CHECK(apps::isCliKeyword("batch") && !apps::isCliKeyword("open"));
    }

    // ---- the launcher: a tile per app, each started as its own program ----
    {
        auto launcher = apps::launcherInfo();
        CHECK(launcher.id == "launcher" && launcher.tools.empty());
        CHECK(launcher.launcher.size() + 1 == apps::all().size());  // every app but share
        bool sawImage = false;
        for (const auto& t : launcher.launcher) {
            CHECK(t.id != "share" && apps::find(t.id));
            CHECK(!t.name.empty() && t.name[0] >= 'A' && t.name[0] <= 'Z' && !t.summary.empty());
            if (t.id == "image") sawImage = t.name == "Image" && t.accent == apps::find("image")->accent;
        }
        CHECK(sawImage);
        CHECK(apps::guiInfo("image").id == "image" && apps::guiInfo("image").launcher.empty());
    }

    // ---- the installers' file types match the table (packaging/file_types.py makes them) ----
    {
        std::ifstream in(std::string(WETWARESUITE_SOURCE_DIR) + "/packaging/file-types.tsv");
        CHECK(in.good());
        std::set<std::string> listed;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream cols(line);
            std::string ext, app, mime, own;
            std::getline(cols, ext, '\t');
            std::getline(cols, app, '\t');
            std::getline(cols, mime, '\t');
            std::getline(cols, own, '\t');
            const auto route = apps::routeForFile("x" + ext);
            CHECK(route.app == app && !route.command.empty() && !mime.empty());
            CHECK((own == "yes") == (ext.rfind(".ww", 0) == 0 || ext.rfind(".ach", 0) == 0));
            listed.insert(ext);
        }
        std::size_t openable = 0;
        for (const auto& [ext, route] : apps::fileTypes()) {
            if (route.command.empty() || std::count(ext.begin(), ext.end(), '.') > 1) continue;
            ++openable;
            if (!listed.count(ext)) std::cerr << "packaging/file-types.tsv misses " << ext << ": run python3 packaging/file_types.py\n";
        }
        CHECK(openable == listed.size());
    }

    // ---- what the window shows for an app ----
    {
        auto image = apps::guiInfo("image");
        CHECK(image.name == "wetwaresuite image" && (image.tools == std::vector<std::string>{"photo", "paint"}));
        CHECK(image.accent == apps::find("image")->accent);
        std::set<std::uint32_t> accents;
        for (const auto& a : apps::all()) accents.insert(a.accent);
        CHECK(accents.size() == apps::all().size());  // every app has its own colour
        auto suite = apps::guiInfo("suite");
        CHECK(suite.name == "wetwaresuite" && suite.tools.size() > 20);
        CHECK(std::find(suite.tools.begin(), suite.tools.end(), "share") == suite.tools.end());
    }

    // ---- the window's handoff flow, through the same commands ----
    {
        // a fresh handoff folder so earlier sends don't show up
        setHome(home / "gui-flow");
        Session image;
        fakeSave(image, "photo.save");
        apps::registerCommands(image, "image");
        CHECK(gui::hasHandoff(image));
        image.workspace().add(std::make_shared<FakeDoc>("image"), "img");

        // Send to...: model, game, video, office and draw take a picture; none is installed in the test
        Result t = image.run("app.targets", {{"doc", std::string("img1")}});
        CHECK(t.ok && toText(*t.field("kind")) == "image");
        auto choices = gui::sendChoices(t, "image");
        CHECK(choices.size() == apps::sendTargets("image").size());
        CHECK(!choices.empty() && choices[0].app == "model" && choices[0].label == "Model" && !choices[0].installed);
        CHECK(!image.run("app.targets", {{"doc", std::string("img9")}}).ok);

        Result sent = image.run("app.send", {{"doc", std::string("img1")}, {"to", std::string("model")}, {"name", std::string("brick")}});
        CHECK(sent.ok && toText(*sent.field("opened")) == "false");
        CHECK(gui::launchAfterSend(sent, "image", choices).empty());  // model's program isn't installed here
        auto installedChoices = choices;
        installedChoices[0].installed = installedChoices[0].program = true;
        CHECK(gui::launchAfterSend(sent, "image", installedChoices) == "model");

        // model's home screen: the inbox banner, then app.receive
        Session model;
        apps::registerCommands(model, "model");
        auto banner = gui::inboxBanner(model.run("app.inbox", {}));
        CHECK(banner.show && banner.button == "Open it");
        CHECK(banner.text == "A texture from Image is waiting: brick.png.");
        CHECK(model.run("app.receive", {}).ok);
        CHECK(!gui::inboxBanner(model.run("app.inbox", {})).show);

        // Open in <app>: a model file dropped on the image app
        fs::path obj = home / "head.obj";
        std::ofstream(obj) << "v 0 0 0\n";
        Result w = image.run("app.open_with", {{"path", obj.string()}});
        auto hint = gui::openSuggestion(w, "image", obj.string(), false);
        CHECK(hint.show && hint.app == "model" && hint.button == "Open in Model" && !hint.installed);
        CHECK(hint.text.find("head.obj") != std::string::npos && hint.text.find("isn't installed") != std::string::npos);
        // its own files, files-app files and the all-in-one get no suggestion
        fs::path png = home / "own.png";
        std::ofstream(png) << "png";
        CHECK(!gui::openSuggestion(image.run("app.open_with", {{"path", png.string()}}), "image", png.string(), true).show);
        CHECK(!gui::openSuggestion(w, "suite", obj.string(), false).show);

        // starting an app that isn't installed says so plainly
        Result l = image.run("app.launch", {{"app", std::string("model")}, {"path", obj.string()}});
        CHECK(!l.ok && l.message.find("isn't installed") != std::string::npos);
        setHome(home);
    }

    // ---- app.palette_*: one colour set shared by every app ----
    {
        setHome(home);
        fs::remove(apps::paletteFile());
        CHECK(apps::paletteFile() == home / "palette.txt");
        Session image, model;
        apps::registerCommands(image, "image");
        apps::registerCommands(model, "model");
        // no file: each app reports its own accent and nothing is custom
        Result a = image.run("app.palette_get", {});
        Result b = model.run("app.palette_get", {});
        CHECK(a.ok && b.ok && !std::get<bool>(*a.field("custom")));
        CHECK(toText(*a.field("primary")) == "#" + [] {
            char buf[8];
            std::snprintf(buf, sizeof buf, "%06x", apps::find("image")->accent);
            return std::string(buf);
        }());
        CHECK(toText(*a.field("primary")) != toText(*b.field("primary")));
        // set in one app, seen by the other
        Result set = image.run("app.palette_set", {{"primary", std::string("#FF8800")}, {"secondary", std::string(" 123 ")},
                                                   {"tertiary", std::string("white")}});
        CHECK(set.ok);
        CHECK(fs::exists(apps::paletteFile()));
        Result seen = model.run("app.palette_get", {});
        CHECK(std::get<bool>(*seen.field("custom")));
        CHECK(toText(*seen.field("primary")) == "#ff8800" && toText(*seen.field("secondary")) == "#112233" &&
              toText(*seen.field("tertiary")) == "#ffffff");
        CHECK(toText(*seen.field("look")) == "dark");
        // only one colour: the others stay
        CHECK(model.run("app.palette_set", {{"tertiary", std::string("#00aa55")}}).ok);
        Result one = image.run("app.palette_get", {});
        CHECK(toText(*one.field("primary")) == "#ff8800" && toText(*one.field("tertiary")) == "#00aa55");
        // friendly errors, and nothing changes
        Result bad = image.run("app.palette_set", {{"primary", std::string("#12345")}});
        CHECK(!bad.ok && bad.message.find("primary colour") != std::string::npos && bad.message.find("#3d7eff") != std::string::npos);
        CHECK(!image.run("app.palette_set", {{"secondary", std::string("bluish")}}).ok);
        CHECK(!image.run("app.palette_set", {}).ok);
        CHECK(toText(*image.run("app.palette_get", {}).field("primary")) == "#ff8800");
        // presets
        Result light = image.run("app.palette_preset", {{"name", std::string("light")}});
        CHECK(light.ok && toText(*light.field("look")) == "light");
        CHECK(toText(*model.run("app.palette_get", {}).field("secondary")) == "#e4e7ec");
        CHECK(image.run("app.palette_preset", {{"name", std::string("high-contrast")}}).ok);
        CHECK(!image.run("app.palette_preset", {{"name", std::string("neon")}}).ok);
        // reset (and preset default) go back to each app's own colours
        CHECK(model.run("app.palette_reset", {}).ok);
        CHECK(!fs::exists(apps::paletteFile()));
        CHECK(!std::get<bool>(*image.run("app.palette_get", {}).field("custom")));
        CHECK(toText(*image.run("app.palette_get", {}).field("primary")) == toText(*a.field("primary")));
        CHECK(image.run("app.palette_reset", {}).ok);  // resetting twice is fine
        CHECK(image.run("app.palette_preset", {{"name", std::string("warm")}}).ok);
        CHECK(image.run("app.palette_preset", {{"name", std::string("default")}}).ok);
        CHECK(!fs::exists(apps::paletteFile()));
        // a file someone edited by hand, with CRLF, still reads
        std::ofstream(apps::paletteFile(), std::ios::binary) << "# mine\r\nPrimary = #010203\r\nsecondary=#fafafa\r\ntertiary=#00f\r\n";
        Result hand = model.run("app.palette_get", {});
        CHECK(std::get<bool>(*hand.field("custom")) && toText(*hand.field("tertiary")) == "#0000ff");
        CHECK(toText(*hand.field("look")) == "light");
        // in a batch script ('#' right after '=' isn't a comment)
        cli::Batch batch(image);
        CHECK(batch.runLine("app.palette_set primary=#336699 secondary=#202020 tertiary=#ffcc00  # my colours").ok);
        CHECK(toText(*model.run("app.palette_get", {}).field("primary")) == "#336699");
        // over MCP
        mcp::Server server(image, {"wetwaresuite-image", apps::kVersion, ""});
        auto tools = nlohmann::json::parse(*server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
        std::string names = tools["result"]["tools"].dump();
        for (const char* t : {"app_palette_get", "app_palette_set", "app_palette_preset", "app_palette_reset"})
            CHECK(names.find(t) != std::string::npos);
        auto call = nlohmann::json::parse(*server.handle(
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"app_palette_preset","arguments":{"name":"colour-blind"}}})"));
        CHECK(call["result"].value("isError", false) == false);
        CHECK(toText(*model.run("app.palette_get", {}).field("primary")) == "#e69f00");
        fs::remove(apps::paletteFile());
    }

#ifndef _WIN32
    // ---- starting another program doesn't wait for it ----
    {
        CHECK(apps::spawnDetached("/bin/sh", {"-c", "exit 0"}).empty());
        CHECK(!apps::spawnDetached("/no/such/program", {}).empty());
    }
#endif

    fs::remove_all(home);
    // ---- app.view: any 3D document from any angle ----
    {
        Session s;
        apps::registerCommands(s, "cad");
        auto fake = std::make_shared<Fake3d>();
        s.workspace().put("mesh1", fake);
        s.workspace().put("img1", std::make_shared<Flat>());
        const std::string out = (fs::temp_directory_path() / "wetwaresuite_test_view.png").string();
        Result r = s.run("app.view", {{"doc", std::string("mesh1")}, {"view", std::string("front")}, {"yaw", 30.0}, {"pitch", 100.0},
                                      {"zoom", 2.0}, {"width", std::int64_t{64}}, {"height", std::int64_t{48}}, {"output", out}});
        CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].bytes.substr(1, 3) == "PNG");
        CHECK(fake->last.yaw == 30 && fake->last.pitch == 89.9 && fake->last.zoom == 2);  // pitch is clamped
        CHECK(r.ok && fs::file_size(out) == r.attachments[0].bytes.size());
        fs::remove(out);
        CHECK(s.run("app.view", {{"doc", std::string("mesh1")}, {"view", std::string("top")}}).ok && fake->last.pitch > 89);
        Result flat = s.run("app.view", {{"doc", std::string("img1")}});
        CHECK(!flat.ok && flat.message.find("not a 3D document") != std::string::npos);
        CHECK(!s.run("app.view", {{"doc", std::string("nope")}}).ok);
        CHECK(!s.run("app.view", {{"doc", std::string("mesh1")}, {"zoom", 0.0}}).ok);
    }

    return check::summary("apps");
}
