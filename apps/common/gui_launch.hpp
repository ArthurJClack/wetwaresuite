// Opening the window from an app binary. wetwaresuite-<app>, the all-in-one
// wetwaresuite and the wetwaresuite-gui launcher all come through here, so the
// window behaves the same however it was started.
#pragma once

#include <functional>
#include <string>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/gui/gui.hpp"

namespace wetwaresuite::apps {

// The window's name, tools and accent colour from the app table. "suite" (or
// "all") is the all-in-one: every tool, in the table's order.
gui::AppInfo guiInfo(const std::string& appId);

// Opens the window over `session` (which should already carry the app's tools and
// the app.* commands). When this build has no window, or there is no screen, it
// calls `usage`, prints one plain line saying why and returns 2, so a double-click
// on a server or a stub build still explains itself.
int runGui(Session& session, const gui::AppInfo& app, const gui::RunOptions& options, const std::function<void()>& usage);

}  // namespace wetwaresuite::apps
