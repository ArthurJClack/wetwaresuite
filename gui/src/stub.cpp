// Built instead of the window when SDL3 isn't available, so apps still link and
// say clearly why there's no window. Every command keeps working headless.
#include <iostream>

#include "wetwaresuite/gui/gui.hpp"

namespace wetwaresuite::gui {

bool available() { return false; }

int run(Session& session, const AppInfo& app) { return run(session, app, {}); }

int run(Session&, const AppInfo& app, const RunOptions&) {
    std::cerr << app.name << " was built without a window (SDL3 wasn't found when it was built).\n"
              << "Everything still works from the command line: run `wetwaresuite help`.\n";
    return 1;
}

}  // namespace wetwaresuite::gui
