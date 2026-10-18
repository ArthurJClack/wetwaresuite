// The app.* commands every wetwaresuite binary carries: which apps are
// installed, which app opens a file, and handing work from one app to another.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::apps {

// Remembers where this binary is (from argv[0]) so app.installed can look next to it.
void setSelfPath(const std::string& argv0);
std::filesystem::path selfDir();

// Where an app's binary is: next to this one, in <data root>/bin, or on PATH. Empty if nowhere.
std::filesystem::path locate(const std::string& binary);

// `self` is this binary's app id, or "suite" for the all-in-one wetwaresuite.
void registerCommands(Session& session, const std::string& self);

// For MCP instructions: which apps are installed and how to hand work between them.
std::string handoffInstructions(const std::string& self);

}  // namespace wetwaresuite::apps
