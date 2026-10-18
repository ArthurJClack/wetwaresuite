// The handoff folder: how one app passes a file to another. `app.send` writes
// the file there and adds an entry to manifest.json; the target app picks it up
// with `app.receive` (or a person opens the file, or Claude does).
//
//   Linux    $XDG_DATA_HOME/wetwaresuite/handoff (default ~/.local/share/...)
//   macOS    ~/Library/Application Support/wetwaresuite/handoff
//   Windows  %APPDATA%\wetwaresuite\handoff
//
// $WETWARESUITE_HOME replaces the wetwaresuite folder on every OS (the hub uses
// the same rule, see hub/src/paths.rs).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace wetwaresuite::apps {

// The per-user wetwaresuite folder (bin/, handoff/, packs/ live under it).
std::filesystem::path dataRoot();
std::filesystem::path handoffDir();

struct Handoff {
    std::string id;       // "h20261016-142501-1"
    std::string from;     // sending app, e.g. "image"
    std::string to;       // target app, e.g. "model"
    std::string kind;     // what the file is for the target: "texture", "mesh", "audio"...
    std::string file;     // absolute path of the handed-off file
    std::string created;  // ISO 8601 UTC
    std::string command;  // the exact command line to run next, for people
    std::string open;     // command id the target runs to open it ("" = use `hint`)
    std::string hint;     // one line: what to do with it in the target
    bool received = false;
};

// Reads <dir>/manifest.json; missing or unreadable -> empty.
std::vector<Handoff> readManifest(const std::filesystem::path& dir);
// Writes the whole manifest (atomically: temp file + rename).
void writeManifest(const std::filesystem::path& dir, const std::vector<Handoff>& items);
// Appends one entry, filling id and created if empty. Returns the stored entry.
Handoff addHandoff(const std::filesystem::path& dir, Handoff item);

std::string nowIso();

}  // namespace wetwaresuite::apps
