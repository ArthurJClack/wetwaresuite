// terminal: output cleaning, folders and variables kept between commands,
// exit codes, typing into programs, stopping them, completion and MCP.
#include <cstdio>
#include <chrono>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"
#include "wetwaresuite/terminal/terminal.hpp"

using namespace wetwaresuite;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string field(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

}  // namespace

int main() {
    // ---- output cleaning ----
    {
        CHECK(terminal::cleanOutput("\x1b[31mred\x1b[0m text\r\n") == "red text\n");
        CHECK(terminal::cleanOutput("10%\r50%\r100%\ndone\n") == "100%\ndone\n");
        CHECK(terminal::cleanOutput("\x1b]0;title\x07ok\n") == "ok\n");
        CHECK(terminal::cleanOutput("ab\bc\n") == "ac\n");
    }

    const fs::path dir = fs::temp_directory_path() / ("ww-term-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "sub folder");
    sysutil::writeFile(dir / "sub folder" / "file one.txt", "x");
    sysutil::writeFile(dir / "notes.md", "x");

    CHECK(!terminal::findShells().empty());
    Session s;
    terminal::registerCommands(s);
    Result o = s.run("terminal.open", {{"folder", sysutil::fromPath(dir)}});
    CHECK(o.ok && field(o, "doc") == "term1");
    const std::string t = "term1";
    auto run = [&](const std::string& cmd) { return s.run("terminal.run", {{"doc", t}, {"command", cmd}, {"timeout", 20.0}}); };

#ifdef _WIN32
    Result hi = run("echo hello");
    if (!hi.ok || field(hi, "output") != "hello\n") std::printf("echo hello gave ok=%d [%s] output [%s]\n", hi.ok ? 1 : 0, hi.message.c_str(), field(hi, "output").c_str());
    CHECK(hi.ok && field(hi, "output") == "hello\n");
    CHECK(run("cd \"sub folder\"").ok);
    Result pwd = run("cd");
    CHECK(field(pwd, "output").find("sub folder") != std::string::npos && field(pwd, "cwd").find("sub folder") != std::string::npos);
    CHECK(run("set GREETING=hi there").ok);
    CHECK(field(run("echo %GREETING%"), "output") == "hi there\n");
    Result bad = run("exit /b 3");
    CHECK(!bad.ok && field(bad, "exit_code") == "3");
    Result in = s.run("terminal.run", {{"doc", t}, {"command", std::string("findstr x")}, {"input", std::string("axb\nccc\n")}, {"timeout", 20.0}});
    CHECK(field(in, "output") == "axb\n");
    Result bg = s.run("terminal.run", {{"doc", t}, {"command", std::string("ping -n 30 127.0.0.1")}, {"wait", false}});
#else
    Result hi = run("echo hello");
    if (!hi.ok || field(hi, "output") != "hello\n") std::printf("echo hello gave ok=%d [%s] output [%s]\n", hi.ok ? 1 : 0, hi.message.c_str(), field(hi, "output").c_str());
    CHECK(hi.ok && field(hi, "output") == "hello\n");
    CHECK(field(run("printf 'no newline'"), "output") == "no newline");
    CHECK(run("cd 'sub folder'").ok);
    Result pwd = run("pwd");
    CHECK(field(pwd, "output").find("sub folder") != std::string::npos && field(pwd, "cwd").find("sub folder") != std::string::npos);
    CHECK(run("export GREETING='hi there'").ok);
    CHECK(field(run("echo \"$GREETING\""), "output") == "hi there\n");
    CHECK(field(run("echo one; echo two >&2; echo three | tr a-z A-Z"), "output") == "one\ntwo\nTHREE\n");
    Result bad = run("false");
    CHECK(!bad.ok && field(bad, "exit_code") == "1");
    Result ex = run("exit 3");
    CHECK(!ex.ok && field(ex, "exit_code") == "3");
    CHECK(fs::equivalent(sysutil::toPath(field(run("cd .. && pwd"), "cwd")), dir));
    Result in = s.run("terminal.run", {{"doc", t}, {"command", std::string("grep x")}, {"input", std::string("axb\nccc\n")}, {"timeout", 20.0}});
    CHECK(field(in, "output") == "axb\n");
    // a program that asks a question
    Result ask = s.run("terminal.run", {{"doc", t}, {"command", std::string("read name; echo \"hi $name\"")}, {"wait", false}});
    CHECK(ask.ok && field(ask, "running") == "true");
    CHECK(!run("echo busy").ok);  // one at a time
    Result sent = s.run("terminal.send", {{"doc", t}, {"text", std::string("Ann")}, {"wait", 5.0}});
    CHECK(sent.ok && field(sent, "output").find("hi Ann") != std::string::npos && field(sent, "running") == "false");
    Result bg = s.run("terminal.run", {{"doc", t}, {"command", std::string("echo started; sleep 30")}, {"wait", false}});
#endif
    CHECK(bg.ok && field(bg, "running") == "true");
    Result rd = s.run("terminal.read", {{"doc", t}, {"wait", 0.5}});
    CHECK(rd.ok && field(rd, "running") == "true");
    Result stop = s.run("terminal.interrupt", {{"doc", t}});
    CHECK(stop.ok && field(stop, "running") == "false");
    CHECK(run("echo after").ok);  // the terminal still works

    Result all = s.run("terminal.read", {{"doc", t}, {"all", true}});
    CHECK(all.message.find("[stopped]") != std::string::npos && all.message.find("hello") != std::string::npos);
    Result h = s.run("terminal.history", {{"doc", t}});
    CHECK(h.ok && h.message.find("echo hello") != std::string::npos);
    CHECK(s.run("terminal.clear", {{"doc", t}}).ok);
    CHECK(field(s.run("terminal.read", {{"doc", t}, {"all", true}}), "output").empty());

    Result c = s.run("terminal.complete", {{"doc", t}, {"line", std::string("cat no")}});
    CHECK(c.ok && field(c, "count") == "1" && field(c, "common") == "notes.md");
    Result c2 = s.run("terminal.complete", {{"doc", t}, {"line", std::string("ls sub")}});
    CHECK(field(c2, "common") == "sub folder/");
    CHECK(field(s.run("terminal.complete", {{"doc", t}, {"line", std::string("ls sub folder/fi")}}), "count") == "0");  // the last word is "folder/fi"
    CHECK(field(s.run("terminal.complete", {{"doc", t}, {"line", std::string("x sub\\ folder/fi")}}), "count") == "0");

    Result once = s.run("terminal.exec", {{"command", std::string("echo once")}, {"folder", sysutil::fromPath(dir)}});
    CHECK(once.ok && field(once, "output") == "once\n");
    CHECK(s.run("terminal.shells", {}).ok);
    CHECK(s.run("terminal.close", {{"doc", t}}).ok && !s.workspace().get(t));

    {
        mcp::Server server(s, {"wetwaresuite", "test", "terminal"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        int seen = 0;
        for (const auto& tool : list) {
            if (std::string(tool["name"]).rfind("terminal_", 0) == 0) ++seen;
        }
        CHECK(seen == 11);
        json call = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                     {"params", {{"name", "terminal_exec"}, {"arguments", {{"command", "echo mcp"}, {"folder", sysutil::fromPath(dir)}}}}}};
        json res = json::parse(*server.handle(call.dump()))["result"];
        CHECK(res["isError"] == false && res["structuredContent"]["output"] == "mcp\n");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("terminal");
}
