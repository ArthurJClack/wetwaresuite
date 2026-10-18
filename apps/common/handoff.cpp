#include "handoff.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wetwaresuite::apps {

namespace {

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

}  // namespace

fs::path dataRoot() {
    std::string home = env("WETWARESUITE_HOME");
    if (!home.empty()) return fs::path(home);
#ifdef _WIN32
    std::string appdata = env("APPDATA");
    if (!appdata.empty()) return fs::path(appdata) / "wetwaresuite";
    return fs::path(env("USERPROFILE")) / "AppData" / "Roaming" / "wetwaresuite";
#else
    fs::path user = env("HOME").empty() ? fs::path(".") : fs::path(env("HOME"));
#ifdef __APPLE__
    return user / "Library" / "Application Support" / "wetwaresuite";
#else
    std::string xdg = env("XDG_DATA_HOME");
    if (!xdg.empty()) return fs::path(xdg) / "wetwaresuite";
    return user / ".local" / "share" / "wetwaresuite";
#endif
#endif
}

fs::path handoffDir() { return dataRoot() / "handoff"; }

std::string nowIso() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::vector<Handoff> readManifest(const fs::path& dir) {
    std::vector<Handoff> out;
    std::ifstream in(dir / "manifest.json", std::ios::binary);
    if (!in) return out;
    std::stringstream ss;
    ss << in.rdbuf();
    json doc = json::parse(ss.str(), nullptr, false);
    if (!doc.is_object() || !doc.contains("items") || !doc["items"].is_array()) return out;
    for (const auto& j : doc["items"]) {
        if (!j.is_object()) continue;
        Handoff h;
        h.id = j.value("id", "");
        h.from = j.value("from", "");
        h.to = j.value("to", "");
        h.kind = j.value("kind", "");
        h.file = j.value("file", "");
        h.created = j.value("created", "");
        h.command = j.value("command", "");
        h.open = j.value("open", "");
        h.hint = j.value("hint", "");
        h.received = j.value("received", false);
        out.push_back(std::move(h));
    }
    return out;
}

void writeManifest(const fs::path& dir, const std::vector<Handoff>& items) {
    fs::create_directories(dir);
    json arr = json::array();
    for (const auto& h : items) {
        arr.push_back({{"id", h.id}, {"from", h.from}, {"to", h.to}, {"kind", h.kind}, {"file", h.file},
                       {"created", h.created}, {"command", h.command}, {"open", h.open}, {"hint", h.hint},
                       {"received", h.received}});
    }
    json doc = {{"version", 1}, {"items", arr}};
    fs::path tmp = dir / "manifest.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << doc.dump(2) << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, dir / "manifest.json", ec);
    if (ec) {  // Windows can refuse to rename over an open file; fall back to a copy
        fs::copy_file(tmp, dir / "manifest.json", fs::copy_options::overwrite_existing);
        fs::remove(tmp, ec);
    }
}

Handoff addHandoff(const fs::path& dir, Handoff item) {
    auto items = readManifest(dir);
    if (item.created.empty()) item.created = nowIso();
    if (item.id.empty()) {
        std::string stamp;
        for (char c : item.created)
            if (c >= '0' && c <= '9') stamp += c;
        item.id = "h" + stamp.substr(0, 8) + "-" + stamp.substr(8) + "-" + std::to_string(items.size() + 1);
    }
    items.push_back(item);
    writeManifest(dir, items);
    return item;
}

}  // namespace wetwaresuite::apps
