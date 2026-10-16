// The shared command line every wetwaresuite-<app> binary runs: list, help,
// run, batch and mcp over that app's tools plus the app.* handoff commands.
#pragma once

#include <functional>
#include <set>
#include <string>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::apps {

// Registers the app's tools; `only` narrows to some of them (empty = all).
using ToolRegistrar = std::function<void(Session&, const std::set<std::string>& only)>;

struct MainSpec {
    std::string app;      // "image"
    std::string version;  // "0.7.0"
    ToolRegistrar registerTools;
};

int appMain(int argc, char** argv, const MainSpec& spec);

}  // namespace wetwaresuite::apps
