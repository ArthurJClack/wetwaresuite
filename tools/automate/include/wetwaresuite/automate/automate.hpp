// automate: scripting across every tool, headless. Batch scripts with
// $variables, pipelines (JSON or a small YAML subset) that loop over files,
// branch on extensions and pass results between steps, and a polling folder
// watcher that runs a script for every new or changed file. There is
// deliberately no shell command: automation can only do what the suite's own
// commands can.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::automate {

namespace fs = std::filesystem;

using Vars = std::map<std::string, std::string>;

// Registers the tools into a fresh session. Background watchers run their
// scripts in sessions made with it, so they never touch the caller's session
// from another thread. Without one, watch_start is unavailable and the
// foreground watcher runs scripts in the caller's session.
using Registrar = std::function<void(Session&)>;

// Registers every automate.* command.
void registerCommands(Session& session, Registrar fresh = {});

// "name=value,other=2" (commas inside values escaped as \,), one per line, or a JSON object.
// Throws ParamError when malformed.
Vars parseVars(const std::string& text);

// Replaces $name, ${name} and $name.field with values from vars. A dotted name
// that isn't defined falls back to its longest defined prefix, so $stem.png works.
// Unknown names are left alone. With `escape`, substituted values are
// backslash-escaped so they stay one word on a command line.
std::string substitute(const std::string& text, const Vars& vars, bool escape = false);

// Wildcards: * and ? within one path segment, ** across segments. Case-insensitive.
bool globMatch(const std::string& pattern, const std::string& path);
// Files matching a pattern such as "in/*.png" or "assets/**/*.svg", relative to base, sorted naturally.
std::vector<fs::path> expandGlob(const std::string& pattern, const fs::path& base = {});

// Parses a pipeline written in JSON or the YAML subset (mappings, "- " lists, inline
// [a, b] lists, quoted strings, # comments) and returns it as JSON text.
// Throws ParamError with the line number when malformed.
std::string pipelineToJson(const std::string& text);

// Evaluates an `if:` condition such as "$file.ext in png,jpg", "$w > 1000", "exists out/x.png".
bool evalCondition(const std::string& condition, const Vars& vars);

}  // namespace wetwaresuite::automate
