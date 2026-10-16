#include "gui_launch.hpp"

#include <iostream>

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

}  // namespace wetwaresuite::apps
