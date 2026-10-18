// Helpers shared by the system tools. Strings are UTF-8 everywhere; these turn
// them into paths and back the same way on Windows, macOS and Linux.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace wetwaresuite::sysutil {

namespace fs = std::filesystem;

inline fs::path toPath(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

inline std::string fromPath(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// the file name, extension or stem as UTF-8 (never lower-cased inside a lambda: MSVC trips on that)
inline std::string nameOf(const fs::path& p) { return fromPath(p.filename()); }

inline std::string lowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline std::string extensionOf(const fs::path& p) { return lowerAscii(fromPath(p.extension())); }

inline std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + fromPath(p));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline void writeFile(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("can't write " + fromPath(p));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("can't write " + fromPath(p));
}

inline std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

inline fs::path homeDir() {
#ifdef _WIN32
    std::string h = env("USERPROFILE");
#else
    std::string h = env("HOME");
#endif
    return h.empty() ? fs::current_path() : toPath(h);
}

// $WETWARESUITE_HOME, else ~/.local/share/wetwaresuite ($XDG_DATA_HOME), ~/Library/Application
// Support/wetwaresuite on macOS, %APPDATA%\wetwaresuite on Windows: the same folder the hub uses.
inline fs::path dataDir() {
    std::string home = env("WETWARESUITE_HOME");
    if (!home.empty()) return toPath(home);
#ifdef _WIN32
    std::string appdata = env("APPDATA");
    return (appdata.empty() ? homeDir() : toPath(appdata)) / "wetwaresuite";
#elif defined(__APPLE__)
    return homeDir() / "Library" / "Application Support" / "wetwaresuite";
#else
    std::string xdg = env("XDG_DATA_HOME");
    return (xdg.empty() ? homeDir() / ".local" / "share" : toPath(xdg)) / "wetwaresuite";
#endif
}

inline std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

inline std::string plural(std::size_t n, const std::string& word) {
    if (n == 1) return "1 " + word;
    auto endsWith = [&](const char* tail) {
        const std::string t(tail);
        return word.size() >= t.size() && word.compare(word.size() - t.size(), t.size(), t) == 0;
    };
    const bool es = endsWith("s") || endsWith("x") || endsWith("z") || endsWith("ch") || endsWith("sh");
    return std::to_string(n) + " " + word + (es ? "es" : "s");
}

// 1536 -> "1.5 KB" (1024-based, as file managers show it)
inline std::string humanSize(std::uint64_t bytes) {
    const char* units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024 && u < 5) {
        v /= 1024;
        ++u;
    }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu %s", static_cast<unsigned long long>(bytes), bytes == 1 ? "byte" : "bytes");
    else std::snprintf(buf, sizeof buf, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
    return buf;
}

// lines without their line breaks; "\r\n" and "\n" both work
inline std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

}  // namespace wetwaresuite::sysutil
