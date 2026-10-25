// Updates: every installed wetwaresuite keeps itself current from the GitHub
// releases the downloads workflow makes. The window starts a quiet background
// check at most once a day (`app.update_auto` in its own process, so closing
// the window never cuts it off); people and Claude use app.update_check,
// app.update_install, app.update_status and app.update_settings.
//
// How each install updates itself (the file it downloads is checked against the
// release's SHA-256 before anything is touched, and the new build has to answer
// --version with the new version before it replaces the old one):
//
//   windows setup.exe / zip / hub   swap the files in place from the zip (a running .exe
//                                   can be renamed, so open apps keep going); an all-users
//                                   install in Program Files runs the new setup.exe instead
//   macOS dmg                       swap each .app bundle in the folder from the new dmg
//   linux AppImage                  swap the one file
//   linux zip / hub                 swap the files in place from the zip
//   linux .deb                      downloads; installing needs the password (pkexec), so it
//                                   waits for a click or app.update_install
//   built from source               never: update with git pull and rebuild
//
// Settings live in <data root>/update.txt (auto=on|off), the last check's state in
// <data root>/updates/state.json, downloads in <data root>/updates/.
// $WETWARESUITE_NO_UPDATE=1 turns the background check off (packagers, CI);
// $WETWARESUITE_UPDATE_URL points at another release feed (a fork, or a file:// one in tests).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::apps::update {

// "v1.2.3", "1.2", "0.2.0-preview.4"; a version with a -suffix sorts before the plain one.
struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string pre;
    std::string text() const;
};
std::optional<Version> parseVersion(const std::string& text);
int compare(const Version& a, const Version& b);  // <0, 0, >0

enum class Kind { Source, Setup, Folder, MacBundles, AppImage, Deb };
const char* kindName(Kind kind);

struct Install {
    Kind kind = Kind::Source;
    std::filesystem::path dir;     // the folder whose programs get replaced
    std::filesystem::path target;  // the AppImage file, for Kind::AppImage
    bool writable = false;         // this user can replace the programs without a password
};
// Works out how this copy was installed from where the program is. `appimage`
// is $APPIMAGE (set by the AppImage runtime), "" otherwise.
Install detectInstall(const std::filesystem::path& exe, const std::string& appimage);
// This program, as an absolute path ($WETWARESUITE_UPDATE_EXE replaces it in tests).
std::filesystem::path selfExecutable();

struct Asset {
    std::string name, url, sha256;  // sha256 in lowercase hex, "" when the release didn't say
    std::uint64_t size = 0;
};
struct Release {
    std::string tag;  // "v0.2.0"
    Version version;
    std::string page;  // the release's web page
    std::vector<Asset> assets;
};
// A GitHub release as JSON (the /releases/latest answer).
std::optional<Release> parseRelease(const std::string& json, std::string& error);
// The download an install of `kind` uses ("" when there's no build for this computer).
std::string assetName(const Install& install);
// "<hex>  <name>" lines, as sha256sum writes them; fills in assets that have no hash.
void applyChecksums(Release& release, const std::string& sums);

std::string sha256Hex(const std::string& bytes);
std::string sha256File(const std::filesystem::path& file);  // "" when unreadable

struct Settings {
    bool autoUpdate = true;
};
std::filesystem::path settingsFile();
Settings loadSettings();
bool saveSettings(const Settings& settings, std::string& error);

// What the last check found and did, shared by every app.
struct State {
    std::string status = "idle";  // idle, checking, downloading, ready, installed, uptodate, failed
    std::string latest;           // newest release seen ("0.2.0")
    std::string message;          // one plain line
    std::string file;             // a verified download waiting to be installed
    std::string page;             // release page
    std::int64_t checked = 0;     // unix time of the last answer from the release feed
    std::int64_t tried = 0;       // unix time of the last attempt (offline ones too)
};
std::filesystem::path updatesDir();
State loadState();
void saveState(const State& state);

// Whether the background check should run now: on, an installed copy of a
// release, and a day since the last answer (an hour since the last try).
bool due(const Settings& settings, const State& state, const Install& install, const std::string& current, std::int64_t now,
         std::string& why);

// Started by the window: runs `<this program> app.update_auto` in the background
// when it's due. Never waits and never fails loudly.
void startBackgroundCheck();
// Deletes programs a past update renamed out of the way (windows can't delete them while they run).
void cleanupOld(const std::filesystem::path& dir);

// The app.update_* commands.
void registerCommands(Session& session);

}  // namespace wetwaresuite::apps::update
