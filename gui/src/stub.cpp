// Built instead of the window when SDL3 isn't available (or WETWARESUITE_GUI is
// off), so apps still link and say clearly why there's no window. Every command
// keeps working headless.
#include <iostream>

#include "wetwaresuite/gui/gui.hpp"

namespace wetwaresuite::gui {

namespace {

const char* reason() {
#ifdef WW_GUI_DISABLED
    return "this copy was built without the window (WETWARESUITE_GUI was off)";
#else
    return "this copy was built without the window (SDL3 wasn't available when it was built)";
#endif
}

}  // namespace

bool available() { return false; }

bool canOpenWindow(std::string& why) {
    why = reason();
    return false;
}

int run(Session& session, const AppInfo& app) { return run(session, app, {}); }

int run(Session&, const AppInfo& app, const RunOptions&) {
    std::cerr << app.name << ": " << reason() << ".\n"
              << "Everything still works from the command line: run it with `help`.\n";
    return 1;
}

}  // namespace wetwaresuite::gui
