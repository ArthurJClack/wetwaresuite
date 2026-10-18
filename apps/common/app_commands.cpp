#include "app_commands.hpp"

#include <cstdlib>
#include <sstream>

#include "apps.hpp"
#include "handoff.hpp"
#include "launch.hpp"

namespace fs = std::filesystem;

namespace wetwaresuite::apps {

namespace {

fs::path& selfPathStore() {
    static fs::path p;
    return p;
}

std::string envPath() {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t len = 0;
    std::string out;
    if (_dupenv_s(&value, &len, "PATH") == 0 && value) out = value;
    std::free(value);
    return out;
#else
    const char* v = std::getenv("PATH");
    return v ? v : "";
#endif
}

std::vector<fs::path> pathDirs() {
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::vector<fs::path> out;
    std::stringstream ss(envPath());
    std::string item;
    while (std::getline(ss, item, sep))
        if (!item.empty()) out.emplace_back(item);
    return out;
}

std::string exe(const std::string& binary) {
#ifdef _WIN32
    return binary + ".exe";
#else
    return binary;
#endif
}

std::string shellQuote(const std::string& s) {
    if (s.find_first_of(" \t\"'$") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\' || c == '$') out += '\\';
        out += c;
    }
    return out + "\"";
}

// The command a person runs in the target app.
std::string commandLine(const std::string& app, const std::string& rest) {
    fs::path bin = locate(binaryName(app));
    std::string program = bin.empty() ? binaryName(app) : shellQuote(bin.string());
    return program + " " + rest;
}

std::string replaceFile(std::string hint, const std::string& file) {
    const std::string q = shellQuote(file);
    for (auto at = hint.find("FILE"); at != std::string::npos; at = hint.find("FILE", at + q.size())) hint.replace(at, 4, q);
    return hint;
}

bool serves(const std::string& self, const std::string& app) { return self == "suite" || self == app; }

fs::path uniqueFile(const fs::path& dir, std::string stem, const std::string& ext) {
    for (auto& c : stem)
        if (c == ' ' || c == '/' || c == '\\' || c == ':') c = '-';
    if (stem.empty()) stem = "handoff";
    fs::path p = dir / (stem + ext);
    for (int i = 2; fs::exists(p); ++i) p = dir / (stem + "-" + std::to_string(i) + ext);
    return p;
}

std::string stemOf(const std::string& path) {
    std::string s = fs::path(path).filename().string();
    auto dot = s.find('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

std::vector<std::string> appChoices() {
    std::vector<std::string> out;
    for (const auto& a : all()) out.push_back(a.id);
    return out;
}

Result installedResult(const std::string& self) {
    std::ostringstream msg;
    std::string have, missing;
    msg << "this is " << (self == "suite" ? "wetwaresuite (every app)" : binaryName(self)) << "\n";
    for (const auto& a : all()) {
        fs::path p = locate(a.id == "share" ? "wetwaresuite-share" : binaryName(a.id));
        bool here = self != "suite" && a.id == self;
        if (!p.empty() || here) {
            have += (have.empty() ? "" : ",") + a.id;
            msg << "  " << a.id << "  installed  " << (p.empty() ? "(this binary)" : p.string()) << "\n";
        } else {
            missing += (missing.empty() ? "" : ",") + a.id;
            msg << "  " << a.id << "  not installed  (" << a.summary << ")\n";
        }
    }
    fs::path suite = locate("wetwaresuite");
    if (!suite.empty()) msg << "  wetwaresuite (every app in one) " << suite.string() << "\n";
    msg << "handoff folder: " << handoffDir().string();
    Result r = Result::success(msg.str());
    r.with("self", self).with("installed", have).with("missing", missing).with("suite", suite.string());
    r.with("handoff", handoffDir().string());
    return r;
}

Result openWith(Session& session, const std::string& self, const std::string& path, bool open) {
    OpenRoute route = routeForFile(path);
    const App* app = find(route.app);
    std::string abs = fs::absolute(fs::path(path)).string();
    std::string next = route.command.empty() ? commandLine(route.app, "run toolkit.inspect path=" + shellQuote(abs))
                                             : commandLine(route.app, "run " + route.command + " path=" + shellQuote(abs));
    if (route.command.empty() && route.app != "files")
        next = commandLine(route.app, "list   # " + fs::path(path).extension().string() + " files are used by commands, not opened");
    Result r;
    if (open && serves(self, route.app) && !route.command.empty() && session.registry().find(route.command)) {
        r = session.run(route.command, {{"path", abs}});
        if (!r.ok) return r;
        r.message = route.app + " opens it: " + r.message;
    } else {
        r = Result::success(fs::path(path).filename().string() + " opens in " + route.app + " (" + (app ? app->summary : "") +
                            ")\nnext: " + next);
    }
    fs::path bin = locate(binaryName(route.app));
    r.with("app", route.app).with("command", route.command).with("installed", !bin.empty() || serves(self, route.app));
    r.with("program", !bin.empty());
    r.with("next", next);
    return r;
}

Result send(Session& session, const std::string& self, const Params& p) {
    const std::string to = stringParam(p, "to");
    const std::string docId = stringParam(p, "doc");
    const std::string file = stringParam(p, "path");
    if (docId.empty() == file.empty()) return Result::failure("give either doc=<id> (an open document) or path=<file>");
    fs::path dir = handoffDir();
    fs::create_directories(dir);

    Handoff h;
    h.from = self;
    h.to = to;
    if (!docId.empty()) {
        auto doc = session.workspace().get(docId);
        if (!doc) return Result::failure("no open document called '" + docId + "' (`documents` lists them)");
        auto route = sendRoute(doc->kind(), to);
        if (!route) {
            std::string targets;
            for (const auto& t : sendTargets(doc->kind())) targets += (targets.empty() ? "" : ", ") + t;
            return Result::failure("a " + doc->kind() + " document can't go to " + to +
                                   (targets.empty() ? std::string(" (save it with its tool and use app.send path=...)")
                                                    : "; it can go to: " + targets));
        }
        if (!session.registry().find(route->save)) return Result::failure(route->save + " isn't in this app");
        std::string stem = stringParam(p, "name", doc->path.empty() ? docId : stemOf(doc->path));
        fs::path out = uniqueFile(dir, stem, route->ext);
        Result saved = session.run(route->save, {{route->docParam, docId}, {route->pathParam, out.string()}});
        if (!saved.ok) return saved;
        h.file = out.string();
        h.kind = route->what;
        h.open = route->open;
        h.hint = replaceFile(route->hint, h.file);
    } else {
        if (!fs::is_regular_file(file)) return Result::failure("no file at " + file);
        fs::path src(file);
        std::string ext = src.extension().string();
        std::string stem = stringParam(p, "name", src.stem().string());
        fs::path out = uniqueFile(dir, stem, ext);
        fs::copy_file(src, out);
        OpenRoute route = routeForFile(out.string());
        h.file = out.string();
        h.kind = "file";
        h.open = route.app == to ? route.command : "";
        h.hint = h.open.empty() ? "use " + shellQuote(h.file) + " with " + to + "'s commands (`list` shows them)"
                                : "opened with " + h.open;
    }
    h.command = commandLine(to, "app.receive");
    h = addHandoff(dir, h);

    std::ostringstream msg;
    msg << "sent to " << to << " (" << h.kind << "): " << h.file;
    Result r;
    bool openHere = boolParam(p, "open", true) && serves(self, to) && !h.open.empty() && session.registry().find(h.open);
    if (openHere) {
        Result opened = session.run(h.open, {{"path", h.file}});
        if (opened.ok) {
            auto items = readManifest(dir);
            for (auto& it : items)
                if (it.id == h.id) it.received = true;
            writeManifest(dir, items);
            msg << "\nopened here: " << opened.message;
            r = Result::success(msg.str(), opened.value);
            for (const auto& [k, v] : opened.data) r.with(k, v);
        }
    }
    if (!openHere || r.message.empty()) {
        msg << "\nnext: " << h.command << "\nthen: " << h.hint;
        r = Result::success(msg.str(), h.file);
    }
    r.with("file", h.file).with("to", to).with("handoff", h.id).with("next", h.command).with("hint", h.hint);
    r.with("opened", openHere && r.message.find("\nopened here: ") != std::string::npos);
    return r;
}

std::vector<Handoff> pending(const std::string& self, const std::string& app, bool includeReceived) {
    std::vector<Handoff> out;
    std::string want = app.empty() ? (self == "suite" ? "all" : self) : app;
    for (auto& h : readManifest(handoffDir())) {
        if (want != "all" && h.to != want) continue;
        if (h.received && !includeReceived) continue;
        out.push_back(h);
    }
    return out;
}

Result inbox(const std::string& self, const Params& p) {
    auto items = pending(self, stringParam(p, "app"), boolParam(p, "all"));
    std::ostringstream msg;
    if (items.empty()) msg << "nothing waiting (handoff folder: " << handoffDir().string() << ")";
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        msg << it->id << "  " << it->from << " -> " << it->to << "  " << it->kind << "  " << it->file
            << (it->received ? "  (received)" : "") << "\n    " << it->hint << "\n";
    }
    std::string text = msg.str();
    if (!text.empty() && text.back() == '\n') text.pop_back();
    Result r = Result::success(text, static_cast<std::int64_t>(items.size()));
    r.with("count", static_cast<std::int64_t>(items.size()));
    if (!items.empty()) {
        const Handoff& latest = items.back();
        r.with("latest", latest.id).with("file", latest.file).with("from", latest.from).with("kind", latest.kind);
    }
    return r;
}

Result receive(Session& session, const std::string& self, const Params& p) {
    const std::string id = stringParam(p, "id");
    auto all = readManifest(handoffDir());
    Handoff* pick = nullptr;
    for (auto& h : all) {
        if (!id.empty() ? h.id == id : (!h.received && (self == "suite" || h.to == self))) pick = &h;
    }
    if (!pick) {
        return Result::failure(id.empty() ? "nothing waiting for " + (self == "suite" ? std::string("any app") : self) +
                                                " (app.inbox lists handoffs; app.send makes one)"
                                          : "no handoff called " + id);
    }
    if (!fs::exists(pick->file)) return Result::failure("the handed-off file is gone: " + pick->file);
    Result r;
    if (boolParam(p, "open", true) && !pick->open.empty() && session.registry().find(pick->open)) {
        r = session.run(pick->open, {{"path", pick->file}});
        if (!r.ok) return r;
        r.message = "received from " + pick->from + ": " + r.message;
    } else {
        r = Result::success("received " + pick->kind + " from " + pick->from + ": " + pick->file + "\n" + pick->hint,
                            pick->file);
    }
    pick->received = true;
    writeManifest(handoffDir(), all);
    r.with("file", pick->file).with("from", pick->from).with("kind", pick->kind).with("hint", pick->hint);
    return r;
}

// Where a document can be sent from here: the apps whose send route takes its
// kind and whose save command this binary has.
Result targets(Session& session, const std::string& self, const std::string& docId) {
    auto doc = session.workspace().get(docId);
    if (!doc) return Result::failure("no open document called '" + docId + "' (`documents` lists them)");
    std::string list, installed, programs, missing;
    for (const auto& route : sendRoutes()) {
        if (route.docKind != doc->kind() || !session.registry().find(route.save)) continue;
        const bool program = !locate(binaryName(route.to)).empty();
        const bool here = program || serves(self, route.to);
        list += (list.empty() ? "" : ",") + route.to;
        if (here) installed += (installed.empty() ? "" : ",") + route.to;
        else missing += (missing.empty() ? "" : ", ") + route.to;
        if (program) programs += (programs.empty() ? "" : ",") + route.to;
    }
    std::string msg = list.empty() ? "a " + doc->kind() + " document has no other app to go to (app.send path=<file> sends any file)"
                                   : docId + " (" + doc->kind() + ") can go to: " + list;
    if (!missing.empty()) msg += "\nnot installed: " + missing;
    Result r = Result::success(msg, list);
    r.with("doc", docId).with("kind", doc->kind()).with("targets", list).with("installed", installed).with("programs", programs);
    r.with("self", self);
    return r;
}

Result launch(const std::string& self, const std::string& app, const std::string& path) {
    if (!find(app)) return Result::failure("no app called " + app);
    fs::path bin = locate(binaryName(app));
    if (bin.empty()) {
        return Result::failure("the " + app + " app isn't installed (" + binaryName(app) +
                               " isn't next to this program, in the wetwaresuite bin folder or on PATH); add it with the hub");
    }
    std::vector<std::string> args{"open"};
    std::string abs;
    if (!path.empty()) {
        std::error_code ec;
        if (!fs::exists(path, ec)) return Result::failure("no file at " + path);
        abs = fs::absolute(fs::path(path), ec).string();
        args.push_back(abs);
    }
    std::string error = spawnDetached(bin, args);
    if (!error.empty()) return Result::failure(error);
    Result r = Result::success("started " + binaryName(app) + (abs.empty() ? std::string() : " with " + abs), bin.string());
    r.with("app", app).with("program", bin.string()).with("from", self);
    if (!abs.empty()) r.with("file", abs);
    return r;
}

}  // namespace

void setSelfPath(const std::string& argv0) {
    fs::path p(argv0);
    std::error_code ec;
#ifdef __linux__
    fs::path proc = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        selfPathStore() = proc;
        return;
    }
#endif
    if (!p.has_parent_path()) {
        for (const auto& d : pathDirs()) {
            if (fs::is_regular_file(d / exe(p.string()), ec) || fs::is_regular_file(d / p, ec)) {
                p = d / p;
                break;
            }
        }
    }
    selfPathStore() = fs::absolute(p, ec);
}

fs::path selfDir() { return selfPathStore().empty() ? fs::path() : selfPathStore().parent_path(); }

fs::path locate(const std::string& binary) {
    std::vector<fs::path> dirs;
    if (!selfDir().empty()) dirs.push_back(selfDir());
    dirs.push_back(dataRoot() / "bin");
    for (auto& d : pathDirs()) dirs.push_back(d);
    std::error_code ec;
    for (const auto& d : dirs) {
        fs::path candidate = d / exe(binary);
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

std::string handoffInstructions(const std::string& self) {
    std::string have, missing;
    for (const auto& a : all()) {
        if (a.id == self) continue;
        bool found = !locate(a.id == "share" ? "wetwaresuite-share" : binaryName(a.id)).empty();
        std::string& list = found ? have : missing;
        list += (list.empty() ? "" : "; ") + a.id + " (" + a.summary + ")";
    }
    std::string text = " Other apps: ";
    if (self == "suite") {
        text = " This binary carries every app (image, draw, model, game, cad, electronics, sound, video, office, maps, files), "
               "so app_send opens the result in the target app's tools right here.";
    } else {
        text += have.empty() ? "none installed besides this one" : "installed: " + have;
        if (!missing.empty()) text += ". Not installed: " + missing;
        text += ".";
    }
    text += " Handing work to another app: app_send doc=<id> to=<app> saves the document in the format that app "
            "wants (image->model or game: PNG texture; model/game->cad: STL; cad->model: STL; model->game: glTF; "
            "vector/board->office: SVG; sound->video: WAV) into the handoff folder " + handoffDir().string() +
            " and returns the file, a hint, and the command to open it there. In the other app (its own MCP server, "
            "or `<app binary> app.receive` for a person) app_receive opens the newest handoff and app_inbox lists them. "
            "app_open_with path=<file> says which app opens a file, app_installed lists what's installed, app_targets "
            "doc=<id> lists where a document can go, and app_launch app=<id> [path=<file>] opens another app's window for "
            "the person. "
            "Everything works offline.";
    return text;
}

void registerCommands(Session& session, const std::string& self) {
    auto& reg = session.registry();
    Session* s = &session;
    reg.add({"app.installed", "Installed apps",
             "List the wetwaresuite apps and where each is installed (next to this binary, in the per-user bin folder, "
             "or on PATH), plus the handoff folder.",
             {}, false, true},
            [self](const Params&) { return std::make_unique<FunctionAction>([self] { return installedResult(self); }); });
    reg.add({"app.open_with", "Open with",
             "Say which app opens a file (by its extension) and the exact command to open it there. With open=true and "
             "the app in this binary, opens it now.",
             {{"path", ParamType::String, "the file"},
              {"open", ParamType::Bool, "open it here when this binary has that app", false, {}, false}},
             false},
            [s, self](const Params& p) {
                return std::make_unique<FunctionAction>(
                    [s, self, p] { return openWith(*s, self, stringParam(p, "path"), boolParam(p, "open")); });
            });
    reg.add({"app.send", "Send to app",
             "Hand an open document (doc=) or a file (path=) to another app: saves it in the format that app wants "
             "(e.g. image->model as a PNG texture, model->game as glTF, cad->model as STL, vector->office as SVG, "
             "sound->video as WAV) into the handoff folder, records it in manifest.json, and prints the command to run "
             "next. If this binary has the target app, it opens it right away.",
             {{"to", ParamType::String, "target app", true, appChoices()},
              {"doc", ParamType::String, "open document to send", false},
              {"path", ParamType::String, "or a file to send as it is", false},
              {"name", ParamType::String, "file name for the handoff, without extension", false},
              {"open", ParamType::Bool, "open it in the target app here when this binary has it", false, {}, true}},
             false},
            [s, self](const Params& p) { return std::make_unique<FunctionAction>([s, self, p] { return send(*s, self, p); }); });
    reg.add({"app.targets", "Where it can go",
             "List the apps an open document can be sent to with app.send (and which of them are installed), for "
             "the window's Send to menu.",
             {{"doc", ParamType::String, "open document", true}},
             false, true},
            [s, self](const Params& p) {
                return std::make_unique<FunctionAction>([s, self, p] { return targets(*s, self, stringParam(p, "doc")); });
            });
    reg.add({"app.launch", "Start another app",
             "Start another installed wetwaresuite app in its own window, optionally opening a file there. It runs "
             "on its own: closing this one leaves it open.",
             {{"app", ParamType::String, "which app to start", true, appChoices()},
              {"path", ParamType::String, "a file to open there", false}},
             false},
            [self](const Params& p) {
                return std::make_unique<FunctionAction>(
                    [self, p] { return launch(self, stringParam(p, "app"), stringParam(p, "path")); });
            });
    reg.add({"app.inbox", "Handoff inbox",
             "List files other apps sent here (newest first) with what to do with each.",
             {{"app", ParamType::String, "whose inbox: an app id or all (default this app)", false},
              {"all", ParamType::Bool, "include ones already received", false, {}, false}},
             false, true},
            [self](const Params& p) { return std::make_unique<FunctionAction>([self, p] { return inbox(self, p); }); });
    reg.add({"app.receive", "Receive handoff",
             "Pick up the newest file sent to this app (or the one with id=) and open it with the right command.",
             {{"id", ParamType::String, "handoff id from app.inbox (default the newest waiting one)", false},
              {"open", ParamType::Bool, "open it (otherwise just return the file)", false, {}, true}},
             false},
            [s, self](const Params& p) { return std::make_unique<FunctionAction>([s, self, p] { return receive(*s, self, p); }); });
}

}  // namespace wetwaresuite::apps
