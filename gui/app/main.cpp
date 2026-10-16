// wetwaresuite-gui: the desktop window over every tool, or over one app's tools
// (`--app image` is photo + paint, a preview of the per-app builds).
//   wetwaresuite-gui                    every tool
//   wetwaresuite-gui --app image a.png  photo + paint, opening a.png
//   wetwaresuite-gui mcp                the same commands for Claude over MCP
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "suite.hpp"
#include "wetwaresuite/gui/gui.hpp"
#include "wetwaresuite/mcp/server.hpp"

using namespace wetwaresuite;

namespace {

struct Preset {
    std::string title;
    std::vector<std::string> tools;  // empty = every tool
    std::uint32_t accent;
    std::string about;
};

const std::map<std::string, Preset>& presets() {
    static const std::map<std::string, Preset> p = {
        {"all", {"wetwaresuite", {}, 0x3d7eff, "every tool in one window"}},
        {"image", {"wetwaresuite image", {"photo", "paint"}, 0xe0559a, "photo editing and painting"}},
        {"vector", {"wetwaresuite vector", {"vector", "convert"}, 0x2fb37a, "vector drawing and file conversion"}},
        {"3d", {"wetwaresuite 3d", {"cad", "forge", "sculpt", "render", "slicer"}, 0xf08c2e, "3D parts, models and prints"}},
        {"sound", {"wetwaresuite sound", {"audio", "beats"}, 0x9b6bff, "sound editing and music"}},
        {"office", {"wetwaresuite office", {"office", "layout", "notes"}, 0x1f9bd1, "documents, layouts and notes"}},
    };
    return p;
}

void usage() {
    std::cout << "usage: wetwaresuite-gui [--app NAME] [--tools a,b] [files...]\n"
                 "       wetwaresuite-gui mcp [--app NAME]\n"
                 "       wetwaresuite-gui --list-apps\n\n"
                 "Opens the wetwaresuite window. Everything it does is a command, so the same\n"
                 "steps work from `wetwaresuite run`, batch scripts and Claude over MCP.\n\n"
                 "  --app NAME         which app to show (see --list-apps), default all\n"
                 "  --tools a,b        show just these tools instead\n"
                 "  --action ID        start on this action's form, e.g. photo.resize\n"
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

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    bool mcpMode = false;
    std::string appName = "all";
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
            for (const auto& [name, p] : presets()) std::cout << name << "\t" << p.about << "\n";
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
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option " << a << "\n";
            usage();
            return 2;
        } else {
            options.open.push_back(a);
        }
    }
    auto preset = presets().find(appName);
    if (preset == presets().end()) {
        std::cerr << "no app called " << appName << "; try --list-apps\n";
        return 2;
    }
    gui::AppInfo app{preset->second.title, tools.empty() ? preset->second.tools : tools, preset->second.accent};
    for (const auto& t : app.tools) {
        if (!suite::toolNames().count(t)) {
            std::cerr << "no tool called " << t << "\n";
            return 2;
        }
    }

    Session session;
    suite::registerAll(session, std::set<std::string>(app.tools.begin(), app.tools.end()));
    if (mcpMode) {
        mcp::Server server(session, app.name, "0.1.0");
        server.run(std::cin, std::cout);
        return 0;
    }
    return gui::run(session, app, options);
}
