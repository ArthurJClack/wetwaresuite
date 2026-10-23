// The terminal.* commands.
#include <algorithm>

#include "wetwaresuite/sysutil/sysutil.hpp"
#include "wetwaresuite/terminal/terminal.hpp"

namespace wetwaresuite::terminal {

namespace {

using std::int64_t;

ParamSpec opt(const std::string& name, ParamType type, const std::string& what, Value def = {}) { return {name, type, what, false, {}, std::move(def)}; }
ParamSpec docParam() { return {"doc", ParamType::String, "id of an open terminal, e.g. term1", true}; }

Result guard(const std::function<Result()>& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
}

std::shared_ptr<TerminalDoc> need(Session& s, const std::string& id) {
    auto t = s.workspace().getAs<TerminalDoc>(id);
    if (!t) throw std::runtime_error("no open terminal called '" + id + "' (terminal.open starts one)");
    return t;
}

ActionFactory onTerm(Session& s, std::function<Result(TerminalDoc&, const Params&)> fn) {
    return [&s, fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([&s, fn, p] { return guard([&] { return fn(*need(s, stringParam(p, "doc")), p); }); });
    };
}

Result status(TerminalDoc& t, Result r) {
    return std::move(r.with("running", t.busy())
        .with("exit_code", static_cast<int64_t>(t.busy() ? -1 : t.lastExitCode()))
        .with("cwd", sysutil::fromPath(t.cwd()))
        .with("shell", t.shell().id));
}

Result finished(TerminalDoc& t, bool done) {
    std::string out = t.commandOutput();
    std::string head;
    if (!done) head = "(still running; terminal.read shows more, terminal.send types into it, terminal.interrupt stops it)\n";
    Result r = Result::success(head + out + (done && t.lastExitCode() != 0 ? "[exit code " + std::to_string(t.lastExitCode()) + "]\n" : ""));
    t.takeNew();
    r.with("output", out);
    r = status(t, r);
    for (auto& [k, v] : r.data) {
        if (k == "running") v = !done;
    }
    if (done && t.lastExitCode() != 0) r.ok = false;
    return r;
}

class CloseAction : public Action {
public:
    CloseAction(Session& s, std::string id) : s_(s), id_(std::move(id)) {}
    Result apply() override {
        auto t = s_.workspace().getAs<TerminalDoc>(id_);
        if (!t) return Result::failure("no open terminal called '" + id_ + "'");
        if (t->busy()) t->interrupt();
        s_.workspace().take(id_);
        return Result::success("closed " + id_);
    }

private:
    Session& s_;
    std::string id_;
};

}  // namespace

void registerCommands(Session& session) {
    Session& s = session;
    auto& reg = s.registry();

    reg.add({"terminal.open", "Open terminal",
             "Open a terminal in a folder with the system's shell (or shell=bash, zsh, sh, cmd, powershell, pwsh). Returns its id "
             "(term1). It remembers its folder and exported variables between commands.",
             {opt("folder", ParamType::String, "folder to start in (default: home)"), opt("shell", ParamType::String, "which shell (terminal.shells lists them)")},
             false, true},
            [&s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&s, p] {
                    return guard([&] {
                        fs::path dir = stringParam(p, "folder").empty() ? sysutil::homeDir() : sysutil::toPath(stringParam(p, "folder"));
                        std::error_code ec;
                        if (!fs::is_directory(dir, ec)) throw std::runtime_error("not a folder: " + stringParam(p, "folder"));
                        dir = fs::absolute(dir, ec).lexically_normal();
                        auto t = std::make_shared<TerminalDoc>(pickShell(stringParam(p, "shell")), dir);
                        std::string id = s.workspace().add(t, "term");
                        Result r = Result::success("opened " + id + " (" + t->summary() + ")", id);
                        return status(*t, r.with("doc", id));
                    });
                });
            });

    reg.add({"terminal.run", "Run command",
             "Run a command line in a terminal, as if typed at the prompt: pipes, &&, cd and export work. Waits for it (up to "
             "timeout seconds) and returns its output and exit code; a longer one keeps running (terminal.read follows it).",
             {docParam(), {"command", ParamType::String, "the command line", true},
              opt("wait", ParamType::Bool, "wait for it to finish", true), opt("timeout", ParamType::Number, "seconds to wait", 30.0),
              opt("input", ParamType::String, "text for its input, then end of input")},
             false},
            onTerm(s, [](TerminalDoc& t, const Params& p) {
                std::string error;
                if (!t.start(stringParam(p, "command"), error)) throw std::runtime_error(error);
                if (p.count("input")) {
                    t.send(stringParam(p, "input"), false);
                    t.closeInput();
                }
                if (!boolParam(p, "wait", true)) {
                    t.pump();
                    return status(t, Result::success("started").with("output", std::string()));
                }
                return finished(t, t.wait(numberParam(p, "timeout", 30.0)));
            }));

    reg.add({"terminal.send", "Type into program", "Type text into the command that is running (answers to its questions). Enter is pressed after it unless newline=false.",
             {docParam(), {"text", ParamType::String, "what to type", true}, opt("newline", ParamType::Bool, "press Enter after it", true),
              opt("end", ParamType::Bool, "then end its input (like Ctrl+D)", false), opt("wait", ParamType::Number, "seconds to wait for its answer", 0.3)},
             false},
            onTerm(s, [](TerminalDoc& t, const Params& p) {
                if (!t.busy()) throw std::runtime_error("nothing is running; terminal.run starts a command");
                if (!t.send(stringParam(p, "text") + (boolParam(p, "newline", true) ? "\n" : ""))) throw std::runtime_error("it is not reading input");
                if (boolParam(p, "end", false)) t.closeInput();
                const bool done = t.wait(numberParam(p, "wait", 0.3));
                std::string fresh = t.takeNew();
                return status(t, Result::success(fresh)).with("output", fresh).with("running", !done);
            }));

    reg.add({"terminal.read", "Read output",
             "What a terminal has printed since the last read (all=true: the whole scrollback), and whether a command is still running.",
             {docParam(), opt("all", ParamType::Bool, "the whole scrollback", false), opt("wait", ParamType::Number, "wait up to this many seconds for the command to finish", 0.0)},
             false, true},
            onTerm(s, [](TerminalDoc& t, const Params& p) {
                double w = numberParam(p, "wait", 0.0);
                if (w > 0) t.wait(w);
                else t.pump();
                std::string text = boolParam(p, "all", false) ? t.scrollback() : t.takeNew();
                if (boolParam(p, "all", false)) t.takeNew();
                return status(t, Result::success(text)).with("output", text);
            }));

    reg.add({"terminal.interrupt", "Stop", "Stop the command that is running (and everything it started), like Ctrl+C. The terminal stays open.",
             {docParam()}, false},
            onTerm(s, [](TerminalDoc& t, const Params&) {
                if (!t.busy()) return status(t, Result::success("nothing was running"));
                t.interrupt();
                return status(t, Result::success("stopped"));
            }));

    reg.add({"terminal.close", "Close terminal", "Close a terminal, stopping anything still running in it.", {docParam()}, false},
            [&s](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<CloseAction>(s, stringParam(p, "doc")); });

    reg.add({"terminal.history", "History", "The commands run in a terminal, with their exit codes, folders and times.",
             {docParam(), opt("count", ParamType::Int, "how many of the latest", int64_t{50})}, false, true},
            onTerm(s, [](TerminalDoc& t, const Params& p) {
                const auto& h = t.history();
                const std::size_t count = static_cast<std::size_t>(std::max<int64_t>(1, intParam(p, "count", 50)));
                std::string text, rows;
                for (std::size_t i = h.size() > count ? h.size() - count : 0; i < h.size(); ++i) {
                    char secs[32];
                    std::snprintf(secs, sizeof secs, "%.2f", h[i].seconds);
                    text += std::to_string(i + 1) + "  " + h[i].command + (h[i].exitCode < 0 ? "  (running)" : h[i].exitCode ? "  (exit " + std::to_string(h[i].exitCode) + ")" : "") + "\n";
                    rows += std::to_string(h[i].exitCode) + "\t" + secs + "\t" + h[i].cwd + "\t" + h[i].command + "\n";
                }
                if (h.empty()) text = "no commands yet\n";
                return Result::success(text).with("count", static_cast<int64_t>(h.size())).with("rows", rows);
            }));

    reg.add({"terminal.clear", "Clear", "Clear a terminal's scrollback (its folder, variables and history stay).", {docParam()}, false, true},
            onTerm(s, [](TerminalDoc& t, const Params&) {
                t.clear();
                return Result::success("cleared");
            }));

    reg.add({"terminal.complete", "Complete name", "File and folder names that finish the last word of a command line (what Tab does).",
             {docParam(), {"line", ParamType::String, "the command line so far", true}}, false, true},
            onTerm(s, [](TerminalDoc& t, const Params& p) {
                auto c = complete(stringParam(p, "line"), t.cwd());
                std::string text;
                for (const auto& x : c) text += x + "\n";
                // the longest shared start, to fill in at once
                std::string common = c.empty() ? std::string() : c.front();
                for (const auto& x : c) {
                    std::size_t n = 0;
                    while (n < common.size() && n < x.size() && common[n] == x[n]) ++n;
                    common.resize(n);
                }
                return Result::success(c.empty() ? "no matches" : text).with("count", static_cast<int64_t>(c.size())).with("common", common).with("rows", text);
            }));

    reg.add({"terminal.shells", "Shells", "The shells installed on this computer; the first is the default.", {}, false, true},
            [](const Params&) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([] {
                    std::string text, rows;
                    for (const auto& sh : findShells()) {
                        text += sh.id + " (" + sh.program + ")\n";
                        rows += sh.id + "\t" + sh.family + "\t" + sh.program + "\n";
                    }
                    return Result::success(text.empty() ? "no shells found" : text).with("rows", rows);
                });
            });

    reg.add({"terminal.exec", "Run once",
             "Run one command line in a folder and return its output and exit code, without keeping a terminal open.",
             {{"command", ParamType::String, "the command line", true}, opt("folder", ParamType::String, "folder to run in (default: home)"),
              opt("shell", ParamType::String, "which shell"), opt("timeout", ParamType::Number, "stop it after this many seconds", 60.0)},
             false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    return guard([&] {
                        fs::path dir = stringParam(p, "folder").empty() ? sysutil::homeDir() : sysutil::toPath(stringParam(p, "folder"));
                        std::error_code ec;
                        if (!fs::is_directory(dir, ec)) throw std::runtime_error("not a folder: " + stringParam(p, "folder"));
                        ExecResult r = exec(stringParam(p, "command"), fs::absolute(dir, ec), pickShell(stringParam(p, "shell")), numberParam(p, "timeout", 60.0));
                        Result out = Result::success(r.output + (r.timedOut ? "[stopped: took longer than the timeout]\n" : r.exitCode ? "[exit code " + std::to_string(r.exitCode) + "]\n" : ""));
                        out.ok = r.exitCode == 0 && !r.timedOut;
                        return out.with("output", r.output).with("exit_code", static_cast<int64_t>(r.exitCode)).with("cwd", r.cwd).with("timed_out", r.timedOut);
                    });
                });
            });
}

}  // namespace wetwaresuite::terminal
