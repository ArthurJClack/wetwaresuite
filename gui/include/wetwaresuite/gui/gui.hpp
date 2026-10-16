// The desktop window shared by every wetwaresuite app: a home screen, command
// search, generated forms, a canvas and undo history over a headless Session.
// The window never does work itself: every button runs a command through
// Session::run, so undo, scripts and MCP clients all see the same steps.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::gui {

struct AppInfo {
    std::string name;                // shown in the title bar, e.g. "wetwaresuite image"
    std::vector<std::string> tools;  // tool ids this app carries, e.g. {"photo", "paint"}; empty = every command's prefix
    std::uint32_t accent = 0x3d7eff;  // 0xRRGGBB
};

struct RunOptions {
    std::string screenshot;  // write a BMP of the first frames here and quit (for docs and smoke tests)
    std::vector<std::string> open;  // files to open at start
    std::string action;  // command whose form to show at start, e.g. "photo.resize"
};

// Opens the window and runs until it is closed. Returns a process exit code.
// In a build without a GUI (WETWARESUITE_GUI off or SDL3 missing) it prints why and returns 1.
int run(Session& session, const AppInfo& app);
int run(Session& session, const AppInfo& app, const RunOptions& options);

// True when this build has a working window (SDL3 + Dear ImGui linked in).
bool available();

}  // namespace wetwaresuite::gui
