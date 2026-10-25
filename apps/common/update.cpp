#include "update.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "apps.hpp"
#include "app_commands.hpp"
#include "handoff.hpp"
#include "launch.hpp"
#include "wetwaresuite/core/process.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace wetwaresuite::apps::update {

namespace {

constexpr const char* kFeed = "https://api.github.com/repos/ArthurJClack/wetwaresuite/releases/latest";
constexpr std::int64_t kDay = 20 * 3600;   // a little under a day, so a daily start still checks daily
constexpr std::int64_t kRetry = 3600;      // offline: try again an hour later
constexpr const char* kStage = ".wetwaresuite-update";

std::string env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t len = 0;
    std::string out;
    if (_dupenv_s(&value, &len, name) == 0 && value) out = value;
    std::free(value);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

std::int64_t now() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::string isoAt(std::int64_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool canWrite(const fs::path& dir) {
    std::error_code ec;
    const fs::path probe = dir / ".wetwaresuite-write-test";
    {
        std::ofstream out(probe, std::ios::binary);
        if (!out) return false;
    }
    fs::remove(probe, ec);
    return true;
}

}  // namespace

// ---- versions ------------------------------------------------------------------

std::string Version::text() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) + (pre.empty() ? "" : "-" + pre);
}

std::optional<Version> parseVersion(const std::string& text) {
    std::string s = text;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    Version v;
    const auto dash = s.find_first_of("-+");
    if (dash != std::string::npos) {
        if (s[dash] == '-') v.pre = s.substr(dash + 1, s.find('+', dash) == std::string::npos ? std::string::npos : s.find('+', dash) - dash - 1);
        s.erase(dash);
    }
    int* parts[3] = {&v.major, &v.minor, &v.patch};
    std::stringstream ss(s);
    std::string piece;
    int n = 0;
    while (std::getline(ss, piece, '.')) {
        if (n >= 3 || piece.empty() || piece.find_first_not_of("0123456789") != std::string::npos || piece.size() > 6) return std::nullopt;
        *parts[n++] = std::stoi(piece);
    }
    if (n < 2) return std::nullopt;
    return v;
}

int compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre == b.pre) return 0;
    if (a.pre.empty()) return 1;  // 1.0.0 is newer than 1.0.0-preview
    if (b.pre.empty()) return -1;
    return a.pre < b.pre ? -1 : 1;
}

// ---- how this copy was installed -----------------------------------------------

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Setup: return "windows-setup";
        case Kind::Folder: return "folder";
        case Kind::MacBundles: return "macos-dmg";
        case Kind::AppImage: return "appimage";
        case Kind::Deb: return "deb";
        case Kind::Source: break;
    }
    return "source";
}

fs::path selfExecutable() {
    const std::string forced = env("WETWARESUITE_UPDATE_EXE");
    if (!forced.empty()) return fs::path(forced);
    std::error_code ec;
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return selfDir();
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    fs::path p = fs::weakly_canonical(fs::path(buf), ec);
    return ec ? fs::path(buf) : p;
#else
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p;
#endif
}

Install detectInstall(const fs::path& exe, const std::string& appimage) {
    Install in;
    std::error_code ec;
    const fs::path dir = exe.parent_path();
    in.dir = dir;
    auto has = [&](const fs::path& p) { return fs::exists(p, ec); };
#ifdef _WIN32
    const std::string x = ".exe";
#else
    const std::string x;
#endif
    if (!appimage.empty() && has(appimage)) {
        in.kind = Kind::AppImage;
        in.target = fs::path(appimage);
        in.dir = in.target.parent_path();
    } else if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents" &&
               dir.parent_path().parent_path().extension() == ".app") {
        in.kind = Kind::MacBundles;
        in.dir = dir.parent_path().parent_path().parent_path();
    } else if (has(dir / "unins000.exe")) {
        in.kind = Kind::Setup;
    } else if (dir == fs::path("/usr/bin") && has("/var/lib/dpkg/info/wetwaresuite.list")) {
        in.kind = Kind::Deb;
    } else if (has(dir / ("wetwaresuite-hub" + x)) || fs::equivalent(dir, dataRoot() / "bin", ec)) {
        in.kind = Kind::Folder;  // an unpacked zip, or what hub installed
    }
    in.writable = in.kind != Kind::Source && in.kind != Kind::Deb && canWrite(in.dir);
    return in;
}

std::string assetName(const Install& in) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (in.kind == Kind::Setup && !in.writable) return "wetwaresuite-windows-x64-setup.exe";
    if (in.kind == Kind::Setup || in.kind == Kind::Folder) return "wetwaresuite-windows-x64.zip";
#elif defined(__APPLE__) && defined(__aarch64__)
    if (in.kind == Kind::MacBundles) return "wetwaresuite-macos-arm64.dmg";
    if (in.kind == Kind::Folder) return "wetwaresuite-macos-arm64.zip";
#elif defined(__linux__) && defined(__x86_64__)
    if (in.kind == Kind::AppImage) return "wetwaresuite-linux-x86_64.AppImage";
    if (in.kind == Kind::Deb) return "wetwaresuite-linux-amd64.deb";
    if (in.kind == Kind::Folder) return "wetwaresuite-linux-x64.zip";
#endif
    (void)in;
    return {};
}

// ---- releases --------------------------------------------------------------------

std::optional<Release> parseRelease(const std::string& text, std::string& error) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "the release feed didn't answer with JSON";
        return std::nullopt;
    }
    Release r;
    r.tag = j.value("tag_name", "");
    auto v = parseVersion(r.tag);
    if (!v) {
        error = r.tag.empty() ? std::string("the release feed has no release") : "the newest release's tag " + r.tag + " isn't a version";
        return std::nullopt;
    }
    r.version = *v;
    r.page = j.value("html_url", "");
    if (j.contains("assets") && j["assets"].is_array()) {
        for (const auto& a : j["assets"]) {
            if (!a.is_object()) continue;
            Asset asset;
            asset.name = a.value("name", "");
            asset.url = a.value("browser_download_url", "");
            if (a.contains("size") && a["size"].is_number_unsigned()) asset.size = a["size"].get<std::uint64_t>();
            const std::string digest = a.contains("digest") && a["digest"].is_string() ? a["digest"].get<std::string>() : "";
            if (startsWith(digest, "sha256:")) asset.sha256 = lower(digest.substr(7));
            if (!asset.name.empty() && !asset.url.empty()) r.assets.push_back(asset);
        }
    }
    return r;
}

void applyChecksums(Release& release, const std::string& sums) {
    std::stringstream ss(sums);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto space = line.find(' ');
        if (space != 64) continue;
        std::string name = line.substr(space);
        name.erase(0, name.find_first_not_of(" *"));
        for (auto& a : release.assets)
            if (a.name == name && a.sha256.empty()) a.sha256 = lower(line.substr(0, 64));
    }
}

// ---- settings and state ---------------------------------------------------------

fs::path settingsFile() { return dataRoot() / "update.txt"; }
fs::path updatesDir() { return dataRoot() / "updates"; }

Settings loadSettings() {
    Settings s;
    std::stringstream ss(readFile(settingsFile()));
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (startsWith(line, "auto=")) s.autoUpdate = lower(line.substr(5)) != "off";
    }
    return s;
}

bool saveSettings(const Settings& s, std::string& error) {
    std::error_code ec;
    fs::create_directories(settingsFile().parent_path(), ec);
    std::ofstream out(settingsFile(), std::ios::binary | std::ios::trunc);
    out << "# wetwaresuite updates. auto=on: check once a day and install new releases by themselves.\n"
        << "# change it here, in any app's Aa menu, or with app.update_settings auto=off\n"
        << "auto=" << (s.autoUpdate ? "on" : "off") << "\n";
    if (!out) {
        error = "couldn't write " + settingsFile().string();
        return false;
    }
    return true;
}

State loadState() {
    State s;
    json j = json::parse(readFile(updatesDir() / "state.json"), nullptr, false);
    if (!j.is_object()) return s;
    s.status = j.value("status", s.status);
    s.latest = j.value("latest", "");
    s.message = j.value("message", "");
    s.file = j.value("file", "");
    s.page = j.value("page", "");
    s.checked = j.value("checked", std::int64_t{0});
    s.tried = j.value("tried", std::int64_t{0});
    return s;
}

void saveState(const State& s) {
    std::error_code ec;
    fs::create_directories(updatesDir(), ec);
    json j = {{"status", s.status}, {"latest", s.latest}, {"message", s.message}, {"file", s.file},
              {"page", s.page},     {"checked", s.checked}, {"tried", s.tried}};
    const fs::path tmp = updatesDir() / "state.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump(2) << "\n";
    }
    fs::rename(tmp, updatesDir() / "state.json", ec);
}

bool due(const Settings& settings, const State& state, const Install& install, const std::string& current, std::int64_t at,
         std::string& why) {
    auto v = parseVersion(current);
    if (!settings.autoUpdate) why = "auto updates are off";
    else if (install.kind == Kind::Source) why = "this copy was built from source";
    else if (!v || !v->pre.empty()) why = "this is a preview build (" + current + "), so it doesn't update by itself";
    else if (assetName(install).empty()) why = "there's no download for this computer";
    else if (at - state.checked < kDay) why = "checked less than a day ago";
    else if (at - state.tried < kRetry) why = "tried less than an hour ago";
    else return true;
    return false;
}

// ---- doing it ---------------------------------------------------------------------

namespace {

// Only one app updates at a time; a lock older than an hour is left over from a crash.
class Lock {
public:
    Lock() : path_(updatesDir() / "lock") {
        std::error_code ec;
        fs::create_directories(updatesDir(), ec);
        if (fs::exists(path_, ec)) {
            const auto age = fs::file_time_type::clock::now() - fs::last_write_time(path_, ec);
            if (!ec && age > std::chrono::hours(1)) fs::remove_all(path_, ec);
        }
        held_ = fs::create_directory(path_, ec) && !ec;
    }
    ~Lock() {
        std::error_code ec;
        if (held_) fs::remove_all(path_, ec);
    }
    bool held() const { return held_; }

private:
    fs::path path_;
    bool held_ = false;
};

std::string feedUrl() {
    const std::string url = env("WETWARESUITE_UPDATE_URL");
    return url.empty() ? kFeed : url;
}

bool fetch(const std::string& url, const fs::path& to, int seconds, bool api, std::string& error) {
    auto curl = findProgram("curl");
    if (!curl) {
        error = "curl isn't installed, so updates can't be downloaded";
        return false;
    }
    std::vector<std::string> args = {curl->string(), "-fsSL", "--retry", "2", "--connect-timeout", "10", "--max-time",
                                     std::to_string(seconds), "-A", "wetwaresuite-updater", "-o", to.string()};
    if (api) args.insert(args.end(), {"-H", "Accept: application/vnd.github+json"});
    args.push_back(url);
    const ProcessResult r = runProgram(args);
    if (r.exitCode == 0) return true;
    std::error_code ec;
    fs::remove(to, ec);
    if (r.exitCode == 6 || r.exitCode == 7 || r.exitCode == 28 || r.exitCode == 35)
        error = "offline (couldn't reach " + std::string(startsWith(url, "file:") ? "the feed" : "github.com") + ")";
    else if (r.exitCode == 22) error = "the server said no: " + r.output.substr(0, r.output.find('\n'));
    else error = "download failed (curl " + std::to_string(r.exitCode) + "): " + r.output.substr(0, r.output.find('\n'));
    return false;
}

struct Checked {
    bool ok = false;
    bool newer = false;
    std::string error;
    Release release;
};

Checked checkFeed(const std::string& current, State& state) {
    Checked c;
    state.tried = now();
    const fs::path file = updatesDir() / "release.json";
    std::error_code ec;
    fs::create_directories(updatesDir(), ec);
    if (!fetch(feedUrl(), file, 30, true, c.error)) return c;
    auto rel = parseRelease(readFile(file), c.error);
    if (!rel) return c;
    c.release = *rel;
    for (const auto& a : c.release.assets) {
        if (a.name != "SHA256SUMS") continue;
        const fs::path sums = updatesDir() / "SHA256SUMS";
        std::string why;
        if (fetch(a.url, sums, 60, false, why)) applyChecksums(c.release, readFile(sums));
    }
    c.ok = true;
    state.checked = now();
    state.latest = c.release.version.text();
    state.page = c.release.page;
    auto have = parseVersion(current);
    c.newer = !have || compare(c.release.version, *have) > 0;
    return c;
}

// Runs a just-downloaded program with --version; it has to name the new version.
bool answers(const std::vector<std::string>& args, const std::string& version, std::string& error,
             const std::vector<std::pair<std::string, std::string>>& envs = {}) {
    ChildProcess p;
    if (!p.start(args, {}, error, envs)) return false;
    std::string out;
    if (!p.wait(60, out)) {
        p.kill();
        error = "the new version didn't start (it hung)";
        return false;
    }
    if (p.exitCode() != 0 || out.find(version) == std::string::npos) {
        error = "the new version didn't start properly (" + out.substr(0, out.find('\n')) + "), so nothing was changed";
        return false;
    }
    return true;
}

fs::path oldName(const fs::path& p) {
    std::error_code ec;
    fs::path old = p.string() + ".old";
    for (int i = 2; fs::exists(old, ec); ++i) old = p.string() + ".old" + std::to_string(i);
    return old;
}

bool ours(const std::string& name) { return startsWith(name, "wetwaresuite") && name.find(".old") == std::string::npos; }

// Moves every wetwaresuite program in `from` into `dir`, renaming what's there out
// of the way first (so running apps keep going). All or nothing: on a failure the
// old ones go back.
bool swapIn(const fs::path& from, const fs::path& dir, bool onlyExisting, int& count, std::string& error) {
    std::error_code ec;
    struct Moved {
        fs::path dest, old;
    };
    std::vector<Moved> done;
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(from, ec)) {
        const std::string name = e.path().filename().string();
        if (!ours(name)) continue;
        if (onlyExisting && !fs::exists(dir / name, ec)) continue;
        entries.push_back(e.path());
    }
    for (const auto& src : entries) {
        Moved m{dir / src.filename(), {}};
        if (fs::exists(m.dest, ec)) {
            m.old = oldName(m.dest);
            fs::rename(m.dest, m.old, ec);
            if (ec) {
                error = "couldn't move " + m.dest.string() + " aside: " + ec.message();
                break;
            }
        }
        fs::rename(src, m.dest, ec);
        if (ec) {
            error = "couldn't put the new " + m.dest.filename().string() + " in place: " + ec.message();
            if (!m.old.empty()) fs::rename(m.old, m.dest, ec);
            break;
        }
        done.push_back(m);
    }
    if (!error.empty()) {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            fs::remove_all(it->dest, ec);
            if (!it->old.empty()) fs::rename(it->old, it->dest, ec);
        }
        return false;
    }
    for (const auto& m : done)
        if (!m.old.empty()) fs::remove_all(m.old, ec);  // windows: a running .exe stays until cleanupOld
    count = static_cast<int>(done.size());
    return true;
}

bool unzip(const fs::path& zip, const fs::path& to, std::string& error) {
    std::vector<std::vector<std::string>> tries;
#ifdef _WIN32
    tries.push_back({"tar", "-xf", zip.string(), "-C", to.string()});  // tar.exe ships with windows 10 and 11
#elif defined(__APPLE__)
    tries.push_back({"ditto", "-x", "-k", zip.string(), to.string()});
#else
    tries.push_back({"unzip", "-q", "-o", zip.string(), "-d", to.string()});
    tries.push_back({"bsdtar", "-xf", zip.string(), "-C", to.string()});
    tries.push_back({"python3", "-m", "zipfile", "-e", zip.string(), to.string()});
#endif
    for (const auto& t : tries) {
        if (!findProgram(t[0])) continue;
        const ProcessResult r = runProgram(t);
        if (r.exitCode == 0) return true;
        error = "couldn't unpack the update: " + r.output.substr(0, r.output.find('\n'));
        return false;
    }
    error = "nothing here can unpack a zip (install unzip)";
    return false;
}

void makeRunnable(const fs::path& dir) {
#ifndef _WIN32
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && ours(e.path().filename().string()))
            fs::permissions(e.path(), fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                          fs::perms::others_read | fs::perms::others_exec, ec);
#else
    (void)dir;
#endif
}

// The folder in a zip that holds the programs (zips have one wetwaresuite-<os> folder inside).
fs::path programsIn(const fs::path& root) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec) && startsWith(e.path().filename().string(), "wetwaresuite-")) return e.path();
    return root;
}

std::string exe(const std::string& name) {
#ifdef _WIN32
    return name + ".exe";
#else
    return name;
#endif
}

bool applyFolder(const Install& in, const fs::path& zip, const Release& rel, int& count, std::string& error) {
    std::error_code ec;
    const fs::path stage = in.dir / kStage;
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    if (ec) {
        error = "can't write to " + in.dir.string();
        return false;
    }
    bool ok = unzip(zip, stage, error);
    const fs::path progs = programsIn(stage);
    if (ok) {
        makeRunnable(progs);
        ok = answers({(progs / exe("wetwaresuite")).string(), "--version"}, rel.version.text(), error);
    }
    const bool hub = fs::equivalent(in.dir, dataRoot() / "bin", ec);
    if (ok) ok = swapIn(progs, in.dir, hub, count, error);
    fs::remove_all(stage, ec);
#ifdef _WIN32
    if (ok && in.kind == Kind::Setup) {  // keep Settings > Apps showing the right version
        runProgram({"reg", "add", "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{6F1D2C4E-8A3B-4F7E-9C51-2B7D0E4A9F13}_is1",
                    "/v", "DisplayVersion", "/d", rel.version.text(), "/f"});
    }
#endif
    return ok;
}

bool applyMac(const Install& in, const fs::path& dmg, const Release& rel, int& count, std::string& error) {
    std::error_code ec;
    const fs::path mnt = updatesDir() / "mnt";
    fs::create_directories(mnt, ec);
    ProcessResult r = runProgram({"hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mnt.string(), dmg.string()});
    if (r.exitCode != 0) {
        error = "couldn't open the update's dmg: " + r.output.substr(0, r.output.find('\n'));
        return false;
    }
    const fs::path stage = in.dir / kStage;
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    fs::path from = mnt / "wetwaresuite";
    bool ok = !ec;
    if (!ok) error = "can't write to " + in.dir.string() + " (move the wetwaresuite folder into Applications)";
    if (ok) ok = answers({(from / "wetwaresuite.app" / "Contents" / "MacOS" / "wetwaresuite").string(), "--version"}, rel.version.text(), error);
    for (const auto& e : fs::directory_iterator(from, ec)) {
        if (!ok) break;
        if (e.path().extension() != ".app") continue;
        r = runProgram({"ditto", e.path().string(), (stage / e.path().filename()).string()});
        if (r.exitCode != 0) {
            error = "couldn't copy " + e.path().filename().string() + ": " + r.output.substr(0, r.output.find('\n'));
            ok = false;
        }
    }
    runProgram({"hdiutil", "detach", mnt.string(), "-force"});
    if (ok) ok = swapIn(stage, in.dir, false, count, error);
    fs::remove_all(stage, ec);
    return ok;
}

bool applyAppImage(const Install& in, const fs::path& file, const Release& rel, std::string& error) {
    std::error_code ec;
    const fs::path next = in.dir / ("." + in.target.filename().string() + ".new");
    fs::copy_file(file, next, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "can't write to " + in.dir.string() + ": " + ec.message();
        return false;
    }
    fs::permissions(next, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                              fs::perms::others_exec, ec);
    bool ok = answers({next.string(), "--run", "wetwaresuite", "--version"}, rel.version.text(), error, {{"APPIMAGE_EXTRACT_AND_RUN", "1"}});
    if (ok) {
        fs::rename(next, in.target, ec);  // the running AppImage keeps its old copy open
        if (ec) error = "couldn't replace " + in.target.string() + ": " + ec.message();
        ok = !ec;
    }
    fs::remove(next, ec);
    return ok;
}

// What can't be done without a password: the .deb, and windows installs for every user.
bool needsPassword(const Install& in) { return in.kind == Kind::Deb || (in.kind == Kind::Setup && !in.writable); }

bool runInstaller(const Install& in, const fs::path& file, std::string& message, std::string& error) {
    if (in.kind == Kind::Deb) {
        if (!findProgram("pkexec") || (env("DISPLAY").empty() && env("WAYLAND_DISPLAY").empty())) {
            error = "installing the .deb needs your password: run  sudo apt install " + file.string();
            return false;
        }
        const ProcessResult r = runProgram({"pkexec", "apt-get", "install", "-y", file.string()});
        if (r.exitCode != 0) {
            error = "the .deb wasn't installed (password cancelled?). Run  sudo apt install " + file.string();
            return false;
        }
        message = "installed";
        return true;
    }
    // windows, all users: the setup asks for admin rights itself and closes the open apps
    std::string why = spawnDetached(file, {"/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS", "/ALLUSERS",
                                           "/DIR=" + in.dir.string()});
    if (!why.empty()) {
        error = why;
        return false;
    }
    message = "started";
    return true;
}

bool download(const Release& rel, const Asset& asset, fs::path& file, std::string& error) {
    std::error_code ec;
    const fs::path dir = updatesDir() / rel.tag;
    fs::create_directories(dir, ec);
    file = dir / asset.name;
    auto good = [&](const fs::path& p) {
        if (asset.size && fs::file_size(p, ec) != asset.size) return false;
        return sha256File(p) == asset.sha256;
    };
    if (fs::exists(file, ec) && good(file)) return true;  // downloaded earlier
    const fs::path part = file.string() + ".part";
    if (!fetch(asset.url, part, 3600, false, error)) return false;
    if (!good(part)) {
        fs::remove(part, ec);
        error = "the download of " + asset.name + " didn't match the release's SHA-256, so it was thrown away";
        return false;
    }
    fs::rename(part, file, ec);
    if (ec) error = "couldn't keep the download: " + ec.message();
    return !ec;
}

// Old releases' downloads go once a newer one is in.
void dropOldDownloads(const std::string& keepTag) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(updatesDir(), ec))
        if (e.is_directory(ec) && startsWith(e.path().filename().string(), "v") && e.path().filename() != keepTag) fs::remove_all(e.path(), ec);
}

Result stateResult(const State& s, const std::string& current, const Install& in) {
    Result r = s.status == "failed" ? Result::failure(s.message) : Result::success(s.message, s.status);
    r.with("status", s.status).with("current", current).with("latest", s.latest).with("install", std::string(kindName(in.kind)));
    r.with("auto", loadSettings().autoUpdate).with("file", s.file).with("page", s.page);
    r.with("checked", s.checked ? isoAt(s.checked) : std::string());
    return r;
}

enum class Mode { Check, Auto, Install };

Result run(const std::string& current, Mode mode) {
    const Install in = detectInstall(selfExecutable(), env("APPIMAGE"));
    State st = loadState();
    if (mode == Mode::Auto) {
        std::string why;
        if (!env("WETWARESUITE_NO_UPDATE").empty()) why = "WETWARESUITE_NO_UPDATE is set";
        if (!why.empty() || !due(loadSettings(), st, in, current, now(), why)) return Result::success("Not checking: " + why + ".", "skipped");
    }
    Lock lock;
    if (!lock.held()) return Result::success("Another wetwaresuite app is updating right now.", "busy");
    cleanupOld(in.dir);
    st.status = "checking";
    saveState(st);
    Checked c = checkFeed(current, st);
    auto fail = [&](const std::string& msg) {
        st.status = "failed";
        st.message = msg;
        saveState(st);
        return stateResult(st, current, in);
    };
    if (!c.ok) return fail("Couldn't check for updates: " + c.error + ". It tries again later.");
    if (!c.newer) {
        st.status = "uptodate";
        st.file.clear();
        st.message = "You have the newest version (" + current + ").";
        saveState(st);
        return stateResult(st, current, in);
    }
    const std::string latest = c.release.version.text();
    if (mode == Mode::Check || in.kind == Kind::Source) {
        st.status = "available";
        st.message = latest + " is out (you have " + current + ")." +
                     (in.kind == Kind::Source ? " This copy was built from source: git pull and rebuild, or get an installer from " + c.release.page
                                              : " app.update_install installs it.");
        saveState(st);
        return stateResult(st, current, in);
    }
    const std::string name = assetName(in);
    if (name.empty()) return fail(latest + " is out, but there's no download for this computer. Build it from source.");
    if (!in.writable && !needsPassword(in))
        return fail(latest + " is out, but " + in.dir.string() + " can't be changed by you. Move wetwaresuite somewhere you can write to, or reinstall it.");
    auto asset = std::find_if(c.release.assets.begin(), c.release.assets.end(), [&](const Asset& a) { return a.name == name; });
    if (asset == c.release.assets.end()) return fail(latest + " is out, but its release has no " + name + " yet.");
    if (asset->sha256.size() != 64) return fail(latest + " is out, but its release doesn't say " + name + "'s SHA-256, so it isn't installed.");

    st.status = "downloading";
    st.message = "Downloading " + latest + "...";
    saveState(st);
    fs::path file;
    std::string error;
    if (!download(c.release, *asset, file, error)) return fail("Couldn't download " + latest + ": " + error + ". It tries again later.");
    dropOldDownloads(c.release.tag);
    st.file = file.string();

    if (needsPassword(in) && mode == Mode::Auto) {
        st.status = "ready";
        st.message = latest + " is downloaded and checked. Installing it needs your password: click Install update in the Aa menu, or run app.update_install.";
        saveState(st);
        return stateResult(st, current, in);
    }
    st.status = "installing";
    saveState(st);
    int count = 0;
    bool ok = false;
    std::string how;
    switch (in.kind) {
        case Kind::Setup:
        case Kind::Folder: ok = in.writable ? applyFolder(in, file, c.release, count, error) : runInstaller(in, file, how, error); break;
        case Kind::MacBundles: ok = applyMac(in, file, c.release, count, error); break;
        case Kind::AppImage: ok = applyAppImage(in, file, c.release, error); break;
        case Kind::Deb: ok = runInstaller(in, file, how, error); break;
        case Kind::Source: break;
    }
    if (!ok) {
        if (needsPassword(in)) {
            st.status = "ready";
            st.message = latest + " is downloaded and checked, but not installed: " + error;
            saveState(st);
            return stateResult(st, current, in);
        }
        return fail("Couldn't install " + latest + ": " + error + ". Nothing was changed.");
    }
    st.status = how == "started" ? "installing" : "installed";
    st.message = how == "started" ? "The " + latest + " installer is running; it asks for admin rights."
                                  : "Updated to " + latest + ". Apps that are open keep the old version until you start them again.";
    if (how != "started") {
        std::error_code ec;
        fs::remove_all(file.parent_path(), ec);
        st.file.clear();
    }
    saveState(st);
    Result r = stateResult(st, current, in);
    r.with("replaced", static_cast<std::int64_t>(count));
    return r;
}

std::string statusLine(const State& s, const std::string& current, const Install& in) {
    std::string line = "wetwaresuite " + current + " (" + kindName(in.kind) + " install in " + in.dir.string() + ")\n";
    line += "updates by itself: " + std::string(loadSettings().autoUpdate ? "on" : "off");
    std::string why;
    if (loadSettings().autoUpdate && !due(loadSettings(), State{}, in, current, now(), why)) line += " (but not here: " + why + ")";
    line += "\n";
    if (!s.message.empty()) line += "last check: " + s.message;
    else line += "not checked yet";
    return line;
}

}  // namespace

void cleanupOld(const fs::path& dir) {
    std::error_code ec;
    if (dir.empty()) return;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if ((startsWith(name, "wetwaresuite") && name.find(".old") != std::string::npos) || name == kStage) fs::remove_all(e.path(), ec);
    }
}

namespace {

// Runs `command` in its own process from this install, so closing the window doesn't stop it.
std::string spawnWorker(const std::string& command) {
    const fs::path self = selfExecutable();
    const Install in = detectInstall(self, env("APPIMAGE"));
    if (in.kind == Kind::AppImage) return spawnDetached(in.target, {"--run", "files", command});  // its own mount
    // the launcher (wetwaresuite-gui) has no command line, so the always-installed files app does it
    std::error_code ec;
    fs::path worker = locate(binaryName("files"));
    const bool sameInstall = !worker.empty() && (in.kind == Kind::MacBundles || fs::equivalent(worker.parent_path(), self.parent_path(), ec));
    if (!sameInstall) worker = self;
    if (startsWith(worker.filename().string(), "wetwaresuite-gui")) return "the files app isn't installed next to this one";
    return spawnDetached(worker, {command});
}

}  // namespace

void startBackgroundCheck() {
    if (!env("WETWARESUITE_NO_UPDATE").empty()) return;
    const Install in = detectInstall(selfExecutable(), env("APPIMAGE"));
    std::string why;
    if (!due(loadSettings(), loadState(), in, kVersion, now(), why)) return;
    cleanupOld(in.dir);
    spawnWorker("app.update_auto");
}

namespace {

// The command, here or (background=true) in its own process that outlives this window.
Result runOrStart(const std::string& current, Mode mode, bool background) {
    if (!background) return run(current, mode);
    const std::string why = spawnWorker(mode == Mode::Check ? "app.update_check" : "app.update_install");
    if (!why.empty()) return Result::failure("Couldn't start the update: " + why);
    State s = loadState();
    s.status = "checking";
    s.message = "Checking for updates...";
    s.tried = now();
    saveState(s);
    return Result::success("Started in the background; app.update_status shows how it goes.", std::string("started"));
}

}  // namespace

void registerCommands(Session& session) {
    auto& reg = session.registry();
    const std::string current = kVersion;
    reg.add({"app.update_status", "Update status",
             "Show this copy's version, how it was installed, whether it updates by itself, and what the last update check "
             "found (no network).",
             {}, false, true},
            [current](const Params&) {
                return std::make_unique<FunctionAction>([current] {
                    const Install in = detectInstall(selfExecutable(), env("APPIMAGE"));
                    State s = loadState();
                    std::error_code ec;
                    const bool working = s.status == "checking" || s.status == "downloading" || s.status == "installing";
                    if (working && !fs::exists(updatesDir() / "lock", ec) && now() - s.tried > 60) s.status = "idle";  // it stopped half way
                    Result r = stateResult(s, current, in);
                    r.ok = true;
                    r.message = statusLine(s, current, in);
                    std::string why;
                    Settings on;  // whether this copy could update by itself, whatever the setting and the clock say
                    const bool can = due(on, State{}, in, current, now(), why);
                    r.with("note", s.message).with("can_auto", can).with("why", why);
                    return r;
                });
            });
    const ParamSpec background{"background", ParamType::Bool, "do it in its own process and return straight away", false, {}, false};
    reg.add({"app.update_check", "Check for updates",
             "Ask GitHub for the newest wetwaresuite release and say whether it's newer than this one. Downloads nothing.",
             {background}, false, true},
            [current](const Params& p) {
                const bool bg = boolParam(p, "background", false);
                return std::make_unique<FunctionAction>([current, bg] { return runOrStart(current, Mode::Check, bg); });
            });
    reg.add({"app.update_install", "Install update",
             "Download the newest release for this computer, check its SHA-256, make sure the new version starts, then "
             "put it in place of this one (open apps keep running and get the new version when started again). A .deb "
             "or an all-users windows install asks for the password. background=true does it in its own process and "
             "returns at once; app.update_status shows how it went.",
             {background}, false},
            [current](const Params& p) {
                const bool bg = boolParam(p, "background", false);
                return std::make_unique<FunctionAction>([current, bg] { return runOrStart(current, Mode::Install, bg); });
            });
    reg.add({"app.update_auto", "Background update",
             "What the apps run quietly when they open: when updates are on and it's been a day, check and install the "
             "newest release (a .deb or an all-users windows install only downloads; installing those needs a password). "
             "Otherwise does nothing.",
             {}, false},
            [current](const Params&) { return std::make_unique<FunctionAction>([current] { return run(current, Mode::Auto); }); });
    reg.add({"app.update_settings", "Update settings",
             "Turn updating by itself on or off for every wetwaresuite app (saved in update.txt in the data folder). "
             "On, the apps check once a day, quietly, and install new releases; offline they just try again later.",
             {{"auto", ParamType::Bool, "update by itself", true}},
             false},
            [current](const Params& p) {
                const bool on = boolParam(p, "auto", true);
                return std::make_unique<FunctionAction>([current, on] {
                    Settings s = loadSettings();
                    s.autoUpdate = on;
                    std::string error;
                    if (!saveSettings(s, error)) return Result::failure(error);
                    Result r = Result::success(on ? "Updates on: every app checks once a day and installs new releases by itself."
                                                  : "Updates off. Check by hand with app.update_check, or turn them back on.",
                                               on);
                    r.with("auto", on).with("file", settingsFile().string());
                    return r;
                });
            });
}

}  // namespace wetwaresuite::apps::update
