#include "gui_launch.hpp"

#include <filesystem>
#include <iostream>

#include "app_commands.hpp"
#include "apps.hpp"
#include "launch.hpp"

namespace wetwaresuite::apps {

gui::AppInfo guiInfo(const std::string& appId) {
    gui::AppInfo info;
    if (appId == "suite" || appId == "all") {
        info.name = "wetwaresuite";
        for (const auto& a : all()) {
            if (a.id == "share") continue;  // share is its own Rust program, not a tool here
            info.tools.insert(info.tools.end(), a.tools.begin(), a.tools.end());
        }
        return info;
    }
    info.name = "wetwaresuite " + appId;
    info.id = appId;
    if (const App* app = find(appId)) {
        info.tools = app->tools;
        info.accent = app->accent;
    }
    return info;
}

int runGui(Session& session, const gui::AppInfo& app, const gui::RunOptions& options, const std::function<void()>& usage) {
    std::string why;
    if (!gui::canOpenWindow(why)) {
        if (usage) usage();
        std::cout << "\nNo window: " << why << ". Everything above still works from the command line.\n";
        return 2;
    }
    releaseConsoleIfOwned();
    return gui::run(session, app, options);
}

gui::AppInfo launcherInfo() {
    gui::AppInfo info;
    info.name = "wetwaresuite";
    info.id = "launcher";
    for (const auto& a : all()) {
        if (a.id == "share") continue;
        std::string name = a.id;
        if (name == "cad") name = "CAD";
        else name[0] = static_cast<char>(name[0] - 'a' + 'A');
        info.launcher.push_back({a.id, name, a.summary, a.accent});
    }
    return info;
}

int installedApps() {
    int n = 0;
    for (const auto& a : all())
        if (a.id != "share" && !locate(binaryName(a.id)).empty()) ++n;
    return n;
}

bool openInOwnApps(const std::vector<std::string>& files, std::string& error) {
    std::vector<std::pair<std::filesystem::path, std::string>> starts;
    for (const auto& f : files) {
        const OpenRoute route = routeForFile(f);
        std::filesystem::path bin = locate(binaryName(route.app));
        if (bin.empty()) {
            error = f + " opens in the " + route.app + " app, which isn't installed";
            return false;
        }
        std::error_code ec;
        starts.emplace_back(bin, std::filesystem::absolute(f, ec).string());
    }
    for (const auto& [bin, file] : starts) {
        std::string why = spawnDetached(bin, {"open", file});
        if (!why.empty()) {
            error = why;
            return false;
        }
    }
    return true;
}

int runLauncher(Session& session, const gui::RunOptions& options, const std::function<void()>& usage) {
    if (installedApps() == 0) return -1;
    if (!options.open.empty() && options.screenshot.empty()) {
        std::string error;
        if (openInOwnApps(options.open, error)) return 0;
        std::cerr << "wetwaresuite: " << error << "\n";
    }
    // a file whose app is missing: the launcher opens and says so; the others still start
    gui::RunOptions own = options;
    own.action.clear();
    return runGui(session, launcherInfo(), own, usage);
}

}  // namespace wetwaresuite::apps
