// wetwaresuite-gui: the launcher the main shortcut opens. Each app it starts runs
// as its own program in its own window, and files given to it go to their own
// apps. `--app image` opens one app's tools in this process instead, and
// `--app all` every tool in one window. It goes through the same apps::runGui as
// every app binary, so the window, the handoff menu and the inbox behave the same.
//   wetwaresuite-gui                    the launcher
//   wetwaresuite-gui a.png song.wav     a.png in image, song.wav in sound, each in its own window
//   wetwaresuite-gui --app image a.png  photo + paint, opening a.png
//   wetwaresuite-gui --app all          every tool in one window
//   wetwaresuite-gui mcp                the same commands for Claude over MCP
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "app_commands.hpp"
#include "apps.hpp"
#include "gui_launch.hpp"
#include "launch.hpp"
#include "suite.hpp"
#include "wetwaresuite/mcp/server.hpp"

using namespace wetwaresuite;

namespace {

void usage() {
    std::cout << "usage: wetwaresuite-gui [--app NAME] [--tools a,b] [files...]\n"
                 "       wetwaresuite-gui mcp [--app NAME]\n"
                 "       wetwaresuite-gui --list-apps\n\n"
                 "Opens the wetwaresuite window. Everything it does is a command, so the same\n"
                 "steps work from `wetwaresuite run`, batch scripts and Claude over MCP.\n\n"
                 "  (no --app)         the launcher: every app opens in its own window\n"
                 "  --app NAME         open one app's tools here (see --list-apps); all = every tool in one window\n"
                 "  --tools a,b        show just these tools instead\n"
                 "  --action ID        start on this action's form, e.g. photo.resize (settings: the Aa menu)\n"
                 "  --screenshot FILE  draw a few frames, save them as a BMP and quit\n";
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + ",") {
        if (c == ',') {
            if (!item.empty()) out.push_back(item);
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

bool knownApp(const std::string& name) { return name == "all" || (name != "share" && apps::find(name)); }

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (argc > 0) apps::setSelfPath(argv[0]);
    bool mcpMode = false;
    std::string appName;  // empty: the launcher
    std::vector<std::string> tools;
    gui::RunOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= args.size()) {
                std::cerr << a << " needs " << what << "\n";
                std::exit(2);
            }
            return args[++i];
        };
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--list-apps") {
            std::cout << "all\tevery tool in one window (without --app: the launcher)\n";
            for (const auto& app : apps::all())
                if (app.id != "share") std::cout << app.id << "\t" << app.summary << "\n";
            return 0;
        } else if (a == "mcp" && i == 0) {
            mcpMode = true;
        } else if (a == "--app") {
            appName = next("an app name, e.g. image");
        } else if (a == "--tools") {
            tools = splitList(next("a list, e.g. photo,paint"));
        } else if (a == "--action") {
            options.action = next("a command id, e.g. photo.resize");
        } else if (a == "--screenshot") {
            options.screenshot = next("a file name");
        } else if (a.rfind("-psn_", 0) == 0) {
            continue;  // old macOS Finder argument
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option " << a << "\n";
            usage();
            return 2;
        } else {
            options.open.push_back(a);
        }
    }
    if (appName.empty() && tools.empty() && !mcpMode && options.action.empty()) {
        Session launcher;  // the launcher carries no tools: it only starts apps
        apps::registerCommands(launcher, "launcher");
        int code = apps::runLauncher(launcher, options, usage);
        if (code >= 0) return code;
        // no app installed next to this program: every tool here instead
    }
    if (appName.empty()) appName = "all";
    if (!knownApp(appName)) {
        std::cerr << "no app called " << appName << "; try --list-apps\n";
        return 2;
    }
    const std::string self = appName == "all" ? "suite" : appName;
    gui::AppInfo app = apps::guiInfo(self);
    if (!tools.empty()) app.tools = tools;
    for (const auto& t : app.tools) {
        if (!suite::toolNames().count(t)) {
            std::cerr << "no tool called " << t << "\n";
            return 2;
        }
    }

    Session session;
    suite::registerAll(session, std::set<std::string>(app.tools.begin(), app.tools.end()));
    apps::registerCommands(session, self);
    if (mcpMode) {
        mcp::Server server(session, app.name, apps::kVersion);
        server.run(std::cin, std::cout);
        return 0;
    }
    return apps::runGui(session, app, options, usage);
}
