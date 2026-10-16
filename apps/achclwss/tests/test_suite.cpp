// End-to-end: batch scripts and the MCP server driving the real tools.
#include <cstdio>
#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

#include "achclwss/core/cli.hpp"
#include "achclwss/mcp/server.hpp"
#include "check.hpp"
#include "suite.hpp"

using namespace achclwss;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

json call(mcp::Server& server, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
}

}  // namespace

int main() {
    auto dir = fs::temp_directory_path() / "achclwss_test_suite";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ---- batch scripting ----
    {
        Session s;
        suite::registerAll(s);
        cli::Batch batch(s);
        std::ostringstream out, err;
        std::string script =
            "# make a poster\n"
            "paint.new width=120 height=80 background=\"#202040\" -> art\n"
            "paint.stroke doc=$art points=10,40,110,40 color=gold size=6\n"
            "paint.shape doc=$art kind=ellipse x=40 y=20 width=40 height=40 color=\"rgb(255, 0, 0)\"\n"
            "undo\n"
            "photo.save doc=$art path=\"" + (dir / "poster one.png").generic_string() + "\"\n"
            "echo done with $art\n";
        std::istringstream in(script);
        int failures = batch.run(in, out, err);
        CHECK(failures == 0);
        CHECK(err.str().empty());
        CHECK(batch.vars().at("art") == "img1");
        CHECK(out.str().find("done with img1") != std::string::npos);
        CHECK(fs::exists(dir / "poster one.png"));

        std::istringstream bad("photo.resize doc=img1 width=lots\nphoto.info doc=img1\n");
        std::ostringstream o2, e2;
        CHECK(batch.run(bad, o2, e2) == 1);
        CHECK(e2.str().find("line 1") != std::string::npos);
        CHECK(o2.str().empty());  // stopped at the first failure

        std::istringstream keep("nope.command\nphoto.info doc=img1\n");
        std::ostringstream o3, e3;
        CHECK(batch.run(keep, o3, e3, true) == 1);
        CHECK(o3.str().find("120x80") != std::string::npos);

        // trailing comments after hex colours, # inside quotes, and capturing a plain field
        std::istringstream extra(
            "photo.new width=5 height=5 background=#ff0000   # red\n"
            "paint.layer_add doc=img2 name=\"My #1 layer\"\n"
            "photo.pick_color doc=img2 x=1 y=1 -> c\n"
            "echo colour $c\n");
        std::ostringstream o4, e4;
        CHECK(batch.run(extra, o4, e4) == 0);
        if (!e4.str().empty()) std::printf("%s", e4.str().c_str());
        CHECK(o4.str().find("colour #ff0000ff") != std::string::npos);

        CHECK(cli::split("a \"b c\" d\\ e").size() == 3);
        auto win = cli::split(R"(photo.open path=C:\Users\me\new\a.png other=\\server\share x=\$y)");
        CHECK(win.size() == 4 && win[1] == R"(path=C:\Users\me\new\a.png)" && win[2] == R"(other=\\server\share)" && win[3] == "x=$y");
        bool threw = false;
        try {
            cli::split("unterminated \"quote");
        } catch (const ParamError&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- MCP ----
    {
        Session s;
        suite::registerAll(s);
        mcp::Server server(s, {"achclwss", "test", "use the tools"});
        json init = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})"));
        CHECK(init["result"]["instructions"] == "use the tools");
        CHECK(init["result"]["capabilities"].contains("resources"));

        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"))["result"]["tools"];
        CHECK(list.size() > 35);
        bool sawEnum = false, sawArray = false;
        for (const auto& t : list) {
            CHECK(t["name"].get<std::string>().find('.') == std::string::npos);
            CHECK(!t["description"].get<std::string>().empty());
            if (t["name"] == "photo_adjust") sawEnum = t["inputSchema"]["properties"]["kind"].contains("enum");
            if (t["name"] == "paint_stroke") sawArray = t["inputSchema"]["properties"]["points"]["type"] == "array";
        }
        CHECK(sawEnum && sawArray);
        for (const auto& t : list) {
            if (t["name"] == "photo_info") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (t["name"] == "photo_save" || t["name"] == "convert_file" || t["name"] == "photo_batch") {
                CHECK(t["annotations"]["readOnlyHint"] == false);
            }
            if (t["name"] == "photo_close") CHECK(t["annotations"]["destructiveHint"] == true);
        }

        json made = call(server, 3, "paint_new", {{"width", 64}, {"height", 64}});
        CHECK(made["isError"] == false);
        CHECK(made["structuredContent"]["doc"] == "img1");
        CHECK(call(server, 30, "photo_resize", {{"doc", "img1"}, {"width", 32.0}})["isError"] == false);  // whole doubles are integers
        CHECK(call(server, 31, "undo", json::object())["isError"] == false);
        json stroke = call(server, 4, "paint_stroke",
                           {{"doc", "img1"}, {"points", {5, 5, 60, 60}}, {"pressures", {0.3, 1.0}}, {"color", "#00f"}, {"preview", true}});
        CHECK(stroke["isError"] == false);
        CHECK(stroke["content"].size() == 2 && stroke["content"][1]["type"] == "image");
        CHECK(stroke["content"][1]["mimeType"] == "image/png");
        json bad = call(server, 5, "photo_adjust", {{"doc", "img1"}, {"kind", "sparkle"}, {"amount", 1}});
        CHECK(bad["isError"] == true);
        json missing = call(server, 6, "photo_info", {{"doc", "img9"}});
        CHECK(missing["isError"] == true);

        json docs = call(server, 7, "documents", json::object());
        CHECK(docs["structuredContent"]["documents"].size() == 1);
        json resources = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":8,"method":"resources/list"})"))["result"]["resources"];
        CHECK(resources.size() == 1 && resources[0]["uri"] == "achclwss://doc/img1");
        json read = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":9,"method":"resources/read","params":{"uri":"achclwss://doc/img1"}})"));
        CHECK(read["result"]["contents"].size() == 2);
        CHECK(read["result"]["contents"][1]["mimeType"] == "image/png");
        json gone = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":10,"method":"resources/read","params":{"uri":"achclwss://doc/zzz"}})"));
        CHECK(gone.contains("error"));

        CHECK(call(server, 11, "undo", json::object())["isError"] == false);
        CHECK(call(server, 12, "history", json::object())["content"][0]["text"] == "history is empty");
        CHECK(mcp::base64("hello") == "aGVsbG8=");
        CHECK(mcp::base64("hi") == "aGk=");
        CHECK(mcp::base64("") == "");
    }

    // ---- convert hands SVG to the vector tool ----
    {
        Session s;
        suite::registerAll(s);
        CHECK(s.run("vector.new", {{"width", std::int64_t{40}}, {"height", std::int64_t{20}}}).ok);
        CHECK(s.run("vector.add_rect", {{"doc", std::string("vec1")}, {"x", 0.0}, {"y", 0.0}, {"width", 20.0}, {"height", 20.0}}).ok);
        CHECK(s.run("vector.save", {{"doc", std::string("vec1")}, {"path", (dir / "box.svg").string()}}).ok);
        Result r = s.run("convert.file", {{"input", (dir / "box.svg").string()}, {"output", (dir / "box.png").string()}, {"scale", 2.0}});
        CHECK(r.ok);
        CHECK(fs::exists(dir / "box.png"));
    }

    // ---- --tools filtering ----
    {
        Session s;
        suite::registerAll(s, {"convert"});
        CHECK(s.registry().find("convert.file") != nullptr);
        CHECK(s.registry().find("photo.open") == nullptr);
        CHECK(s.registry().find("vector.convert") != nullptr);  // convert brings vector for SVG
        Session p;
        suite::registerAll(p, {"paint"});
        CHECK(p.registry().find("photo.save") != nullptr);  // paint saves through photo
    }

    fs::remove_all(dir);
    return check::summary("suite");
}
