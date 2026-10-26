// The apps: what people install. Each app is its own binary,
// `wetwaresuite-<id>`, carrying a few related tools; `wetwaresuite` carries
// them all. This is the one table on the C++ side; hub/src/catalog.rs has the
// same grouping for installs (test_apps checks they agree).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wetwaresuite::apps {

// set by CMake (WETWARESUITE_VERSION); release builds get the release's version
#ifdef WETWARESUITE_VERSION
inline constexpr const char* kVersion = WETWARESUITE_VERSION;
#else
inline constexpr const char* kVersion = "0.0.0";
#endif

struct App {
    std::string id;       // "image"
    std::string summary;  // plain words
    std::vector<std::string> tools;
    std::uint32_t accent = 0x3d7eff;  // 0xRRGGBB, the window's highlight colour
    bool always = false;  // installed with every other app
};

const std::vector<App>& all();
const App* find(const std::string& id);
const App* appOfTool(const std::string& tool);
std::string binaryName(const std::string& appId);  // "wetwaresuite-image" (no .exe)

// Which app opens a file, from its extension (case-insensitive, multi-part
// extensions like .beats.json count).
struct OpenRoute {
    std::string app;      // "image"
    std::string command;  // command that opens it there, taking path=..., "" if none
};
OpenRoute routeForFile(const std::string& path);
// Every extension an app knows (".png" -> image, photo.open), for the installers'
// file types (packaging/file_types.py writes them out; test_apps checks they agree).
std::vector<std::pair<std::string, OpenRoute>> fileTypes();

// How a document goes from one app to another: the command that writes it
// (with doc=... and `pathParam`=file), the file type, and what the target does with it.
struct SendRoute {
    std::string docKind;    // Document::kind(), e.g. "image", "sculpt"
    std::string to;         // target app
    std::string save;       // command in the sending app
    std::string pathParam;  // "path" (or "output")
    std::string ext;        // ".png"
    std::string what;       // "texture", "mesh", "audio"...
    std::string open;       // command in the target that opens it with path=..., "" = none
    std::string hint;       // what to do with it there; FILE stands for the file
    std::string docParam = "doc";  // the save command's document parameter
};
const std::vector<SendRoute>& sendRoutes();
std::optional<SendRoute> sendRoute(const std::string& docKind, const std::string& to);
std::vector<std::string> sendTargets(const std::string& docKind);

// One or two sentences for Claude on how a tool's commands fit together.
std::string toolGuide(const std::string& tool);
// The MCP instructions for one app's binary (without the handoff part).
std::string appInstructions(const std::string& appId);

}  // namespace wetwaresuite::apps
