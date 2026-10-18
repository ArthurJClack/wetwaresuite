// explorer.* commands that look without changing anything: list, info,
// preview, search, size, places, and open in the system's app.
#include <algorithm>
#include <ctime>
#include <fstream>
#include <regex>

#include "internal.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/toolkit/detect.hpp"

namespace wetwaresuite::explorer::detail {

std::vector<fs::path> splitPaths(const std::string& text) {
    std::vector<fs::path> out;
    std::string cur;
    for (char c : text + "\n") {
        if (c == '\n' || c == '|') {
            std::string t = sysutil::trim(cur);
            if (!t.empty()) out.push_back(sysutil::toPath(t));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (out.empty()) throw std::runtime_error("no paths given");
    return out;
}

fs::path pathParam(const Params& p, const std::string& key) {
    std::string v = stringParam(p, key);
    if (v.empty() && key == "folder") return sysutil::homeDir();
    if (v.empty()) throw std::runtime_error(key + " is missing");
    if (v == "~") return sysutil::homeDir();
    if (v.rfind("~/", 0) == 0) return sysutil::homeDir() / sysutil::toPath(v.substr(2));
    return sysutil::toPath(v);
}

bool wildcard(const std::string& pattern, const std::string& name) {
    // * and ?, case-insensitive; several patterns with ; or ,
    std::string pat;
    for (char c : pattern + ";") {
        if (c == ';' || c == ',') {
            std::string a = sysutil::lowerAscii(sysutil::trim(pat)), b = sysutil::lowerAscii(name);
            pat.clear();
            if (a.empty()) continue;
            if (a.find_first_of("*?") == std::string::npos) {
                if (b.find(a) != std::string::npos) return true;  // plain words match anywhere in the name
                continue;
            }
            std::size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
            while (ni < b.size()) {
                if (pi < a.size() && (a[pi] == '?' || a[pi] == b[ni])) {
                    ++pi, ++ni;
                } else if (pi < a.size() && a[pi] == '*') {
                    star = pi++;
                    mark = ni;
                } else if (star != std::string::npos) {
                    pi = star + 1;
                    ni = ++mark;
                } else {
                    break;
                }
            }
            while (pi < a.size() && a[pi] == '*') ++pi;
            if (ni == b.size() && pi == a.size()) return true;
        } else {
            pat += c;
        }
    }
    return false;
}

std::string entryRow(const Entry& e) {
    return std::string(e.dir ? "dir" : "file") + "\t" + e.name + "\t" + std::to_string(e.size) + "\t" + std::to_string(e.modified) + "\t" + e.type + "\t" + e.path + "\n";
}

namespace {

bool isText(const std::string& bytes) {
    const std::size_t n = std::min<std::size_t>(bytes.size(), 4096);
    std::size_t odd = 0;
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0) return false;
        if (c < 9 || (c > 13 && c < 32)) ++odd;
    }
    return odd * 50 < n + 1;
}

std::string readHead(const fs::path& p, std::size_t n) {
    std::ifstream in(p, std::ios::binary);
    std::string s(n, '\0');
    in.read(s.data(), static_cast<std::streamsize>(n));
    s.resize(static_cast<std::size_t>(in.gcount()));
    return s;
}

Result previewOf(const fs::path& path, const Params& p) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        auto entries = listFolder(path, false);
        std::size_t dirs = 0;
        std::string text;
        for (const auto& e : entries) {
            if (e.dir) ++dirs;
            if (text.size() < 4000) text += (e.dir ? "  " + e.name + "/" : "  " + e.name) + "\n";
        }
        return Result::success("folder with " + sysutil::plural(dirs, "folder") + " and " + sysutil::plural(entries.size() - dirs, "file") + "\n" + text)
            .with("kind", "folder");
    }
    if (!fs::is_regular_file(path, ec)) throw std::runtime_error("not found: " + sysutil::fromPath(path));
    const auto type = toolkit::detectFile(path);
    const std::uint64_t size = fs::file_size(path, ec);
    const std::string head = sysutil::fromPath(path) + "\n" + type.format + ", " + sysutil::humanSize(size) + "\n";
    if (type.category == "image" && raster::canRead(path) && size < (256u << 20)) {
        try {
            raster::Image img = raster::load(path);
            Result r = Result::success(head + std::to_string(img.width()) + " x " + std::to_string(img.height()) + " pixels");
            r.with("kind", "image").with("width", static_cast<int64_t>(img.width())).with("height", static_cast<int64_t>(img.height()));
            r.attach({"image/png", raster::previewPng(img, static_cast<int>(intParam(p, "size", 512))), "preview.png"});
            return r;
        } catch (const std::exception&) {
        }
    }
    if (!archiveKind(path).empty()) {
        auto items = archiveList(path);
        std::string text;
        std::uint64_t total = 0;
        for (const auto& i : items) {
            total += i.size;
            if (text.size() < 4000) text += "  " + i.name + (i.dir ? "" : "  (" + sysutil::humanSize(i.size) + ")") + "\n";
        }
        return Result::success(head + sysutil::plural(items.size(), "item") + ", " + sysutil::humanSize(total) + " unpacked\n" + text).with("kind", "archive");
    }
    const std::string bytes = readHead(path, 64 * 1024);
    if (isText(bytes)) {
        const int64_t want = std::max<int64_t>(1, intParam(p, "lines", 40));
        auto ls = sysutil::splitLines(bytes);
        std::string text;
        for (int64_t i = 0; i < want && i < static_cast<int64_t>(ls.size()); ++i) text += ls[static_cast<std::size_t>(i)] + "\n";
        return Result::success(head + text).with("kind", "text").with("text", text);
    }
    std::string dump = toolkit::hexDump(path, 0, static_cast<std::uint64_t>(std::max<int64_t>(16, intParam(p, "bytes", 256))));
    return Result::success(head + dump).with("kind", "binary").with("hex", dump);
}

struct SearchSpec {
    std::string name, contains;
    bool regex = false, hidden = false;
    std::string kind = "any";
    std::uint64_t minSize = 0, maxSize = 0;
    double newerDays = 0, olderDays = 0;
};

bool matches(const fs::directory_entry& e, const SearchSpec& s, const std::regex* re, std::int64_t now) {
    std::error_code ec;
    const bool dir = e.is_directory(ec);
    if (s.kind == "file" && dir) return false;
    if (s.kind == "folder" && !dir) return false;
    const std::string name = sysutil::nameOf(e.path());
    if (!s.name.empty() && !wildcard(s.name, name)) return false;
    if (s.minSize || s.maxSize || !s.contains.empty()) {
        if (dir) return false;
        const std::uint64_t size = e.file_size(ec);
        if (ec || size < s.minSize || (s.maxSize && size > s.maxSize)) return false;
    }
    if (s.newerDays > 0 || s.olderDays > 0) {
        const double age = static_cast<double>(now - modifiedTime(e.path())) / 86400.0;
        if (s.newerDays > 0 && age > s.newerDays) return false;
        if (s.olderDays > 0 && age < s.olderDays) return false;
    }
    if (!s.contains.empty()) {
        if (e.file_size(ec) > (64u << 20)) return false;
        std::string bytes;
        try {
            bytes = sysutil::readFile(e.path());
        } catch (const std::exception&) {
            return false;
        }
        if (re) return std::regex_search(bytes, *re);
        return sysutil::lowerAscii(bytes).find(sysutil::lowerAscii(s.contains)) != std::string::npos;
    }
    return true;
}

}  // namespace

void registerFileCommands(Session& s) {
    auto& reg = s.registry();
    reg.add({"explorer.list", "List folder",
             "The files and folders in a folder (folders first): name, size, date modified and type.",
             {opt("folder", ParamType::String, "the folder (default: home; ~ works)", std::string("~")),
              opt("hidden", ParamType::Bool, "include hidden files", false),
              {"sort", ParamType::String, "order", false, {"name", "size", "date", "type"}, std::string("name")},
              opt("descending", ParamType::Bool, "biggest, newest or Z first", false)},
             false, true},
            reader([](const Params& p) {
                const fs::path dir = pathParam(p, "folder");
                auto entries = listFolder(dir, boolParam(p, "hidden", false), stringParam(p, "sort", "name"), boolParam(p, "descending", false));
                std::string text = sysutil::fromPath(dir) + "\n", rows;
                std::uint64_t total = 0;
                for (const auto& e : entries) {
                    char line[64];
                    std::snprintf(line, sizeof line, "%10s  %16s  ", e.dir ? "<folder>" : sysutil::humanSize(e.size).c_str(), formatTime(e.modified).c_str());
                    text += line + e.name + (e.dir ? "/" : "") + "\n";
                    rows += entryRow(e);
                    total += e.size;
                }
                if (entries.empty()) text += "(empty)\n";
                return Result::success(text)
                    .with("folder", sysutil::fromPath(dir))
                    .with("count", static_cast<int64_t>(entries.size()))
                    .with("bytes", static_cast<int64_t>(total))
                    .with("rows", rows);
            }));

    reg.add({"explorer.info", "File info",
             "What a file or folder is: type (from its contents), size, dates, permissions, and for folders how many files.",
             {req("path", ParamType::String, "file or folder")}, false, true},
            reader([](const Params& p) {
                const fs::path path = pathParam(p, "path");
                std::error_code ec;
                auto st = fs::symlink_status(path, ec);
                if (ec || !fs::exists(st)) throw std::runtime_error("not found: " + stringParam(p, "path"));
                const bool dir = fs::is_directory(path, ec);
                std::uint64_t files = 0;
                const std::uint64_t size = sizeOf(path, &files);
                std::string type = dir ? "folder" : toolkit::detectFile(path).format;
                auto perms = fs::status(path, ec).permissions();
                std::string mode;
                for (int i = 8; i >= 0; --i) mode += (static_cast<unsigned>(perms) >> i) & 1 ? "rwx"[(8 - i) % 3] : '-';
                std::string text = sysutil::fromPath(fs::absolute(path, ec)) + "\ntype: " + type + "\nsize: " + sysutil::humanSize(size) +
                                   (dir ? " in " + sysutil::plural(files, "file") : "") + "\nmodified: " + formatTime(modifiedTime(path)) +
                                   "\npermissions: " + mode + (fs::is_symlink(st) ? "\nlink to: " + sysutil::fromPath(fs::read_symlink(path, ec)) : "");
                return Result::success(text)
                    .with("type", type)
                    .with("bytes", static_cast<int64_t>(size))
                    .with("files", static_cast<int64_t>(files))
                    .with("modified", static_cast<int64_t>(modifiedTime(path)))
                    .with("folder", dir)
                    .with("permissions", mode);
            }));

    reg.add({"explorer.preview", "Preview",
             "A quick look inside a file: the first lines of text, a picture of an image, what is in an archive or a folder, "
             "or a hex dump of anything else.",
             {req("path", ParamType::String, "file or folder"), opt("lines", ParamType::Int, "text lines to show", int64_t{40}),
              opt("bytes", ParamType::Int, "bytes of hex to show", int64_t{256}), opt("size", ParamType::Int, "image preview size in px", int64_t{512})},
             false, true},
            reader([](const Params& p) { return previewOf(pathParam(p, "path"), p); }));

    reg.add({"explorer.search", "Find files",
             "Find files and folders under a folder by name (wildcards like *.jpg, or any part of the name), by text inside, "
             "size and age.",
             {opt("folder", ParamType::String, "where to look (default: home)", std::string("~")),
              opt("name", ParamType::String, "name pattern, e.g. *.pdf;*.docx or report"),
              opt("contains", ParamType::String, "text inside the file"), opt("regex", ParamType::Bool, "contains is a regular expression", false),
              {"kind", ParamType::String, "files, folders or both", false, {"any", "file", "folder"}, std::string("any")},
              opt("min_size", ParamType::Number, "at least this many bytes"), opt("max_size", ParamType::Number, "at most this many bytes"),
              opt("newer_days", ParamType::Number, "changed in the last N days"), opt("older_days", ParamType::Number, "not changed for N days"),
              opt("hidden", ParamType::Bool, "look in hidden folders too", false), opt("limit", ParamType::Int, "stop after this many", int64_t{500})},
             false, true},
            reader([](const Params& p) {
                const fs::path root = pathParam(p, "folder");
                SearchSpec sp;
                sp.name = stringParam(p, "name");
                sp.contains = stringParam(p, "contains");
                sp.regex = boolParam(p, "regex", false);
                sp.hidden = boolParam(p, "hidden", false);
                sp.kind = stringParam(p, "kind", "any");
                sp.minSize = static_cast<std::uint64_t>(std::max(0.0, numberParam(p, "min_size", 0)));
                sp.maxSize = static_cast<std::uint64_t>(std::max(0.0, numberParam(p, "max_size", 0)));
                sp.newerDays = numberParam(p, "newer_days", 0);
                sp.olderDays = numberParam(p, "older_days", 0);
                std::unique_ptr<std::regex> re;
                if (sp.regex && !sp.contains.empty()) re = std::make_unique<std::regex>(sp.contains, std::regex::ECMAScript | std::regex::icase);
                const std::size_t limit = static_cast<std::size_t>(std::max<int64_t>(1, intParam(p, "limit", 500)));
                const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
                std::string text, rows;
                std::size_t n = 0, looked = 0;
                std::error_code ec;
                for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && n < limit; it.increment(ec)) {
                    if (ec) break;
                    ++looked;
                    const std::string name = sysutil::nameOf(it->path());
                    std::error_code e2;
                    if (it->is_directory(e2) && !sp.hidden && !name.empty() && name[0] == '.') {
                        it.disable_recursion_pending();
                        continue;
                    }
                    if (!sp.hidden && !name.empty() && name[0] == '.') continue;
                    if (!matches(*it, sp, re.get(), now)) continue;
                    ++n;
                    Entry e;
                    e.name = name;
                    e.path = sysutil::fromPath(it->path());
                    e.dir = it->is_directory(e2);
                    e.size = e.dir ? 0 : it->file_size(e2);
                    e.modified = modifiedTime(it->path());
                    text += e.path + (e.dir ? "/" : "") + "\n";
                    rows += entryRow(e);
                }
                return Result::success((n ? sysutil::plural(n, "match") + (n >= limit ? " (limit reached)" : "") + "\n" : "nothing found\n") + text, static_cast<int64_t>(n))
                    .with("count", static_cast<int64_t>(n))
                    .with("looked_at", static_cast<int64_t>(looked))
                    .with("rows", rows);
            }));

    reg.add({"explorer.size", "Folder size",
             "How much space a folder takes, and its biggest files and folders: what to clean up first.",
             {opt("folder", ParamType::String, "the folder (default: home)", std::string("~")), opt("top", ParamType::Int, "how many of the biggest to list", int64_t{10})},
             false, true},
            reader([](const Params& p) {
                const fs::path root = pathParam(p, "folder");
                std::vector<std::pair<std::uint64_t, std::string>> parts;
                std::uint64_t total = 0, files = 0;
                std::error_code ec;
                for (const auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
                    std::uint64_t f = 0;
                    std::uint64_t sz = sizeOf(e.path(), &f);
                    total += sz;
                    files += f;
                    std::error_code e2;
                    parts.emplace_back(sz, sysutil::nameOf(e.path()) + (e.is_directory(e2) ? "/" : ""));
                }
                if (ec) throw std::runtime_error("can't open " + sysutil::fromPath(root));
                std::sort(parts.rbegin(), parts.rend());
                std::string text = sysutil::fromPath(root) + ": " + sysutil::humanSize(total) + " in " + sysutil::plural(files, "file") + "\n", rows;
                const std::size_t top = static_cast<std::size_t>(std::max<int64_t>(1, intParam(p, "top", 10)));
                for (std::size_t i = 0; i < parts.size() && i < top; ++i) {
                    double pct = total ? 100.0 * static_cast<double>(parts[i].first) / static_cast<double>(total) : 0;
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%10s %5.1f%%  ", sysutil::humanSize(parts[i].first).c_str(), pct);
                    text += buf + parts[i].second + "\n";
                    rows += std::to_string(parts[i].first) + "\t" + parts[i].second + "\n";
                }
                return Result::success(text).with("bytes", static_cast<int64_t>(total)).with("files", static_cast<int64_t>(files)).with("rows", rows);
            }));

    reg.add({"explorer.places", "Places",
             "Shortcuts: home, desktop, documents, downloads, pictures, music, videos, the suite's data folder, temporary "
             "files and every drive.",
             {}, false, true},
            reader([](const Params&) {
                std::string text, rows;
                for (const auto& [name, path] : places()) {
                    text += name + ": " + sysutil::fromPath(path) + "\n";
                    rows += name + "\t" + sysutil::fromPath(path) + "\n";
                }
                return Result::success(text).with("rows", rows);
            }));

    reg.add({"explorer.open", "Open in its app", "Open a file or folder with the program the system uses for it (photos in the photo viewer, and so on).",
             {req("path", ParamType::String, "file or folder")}, false},
            reader([](const Params& p) {
                std::string error;
                if (!launch(pathParam(p, "path"), error)) throw std::runtime_error(error);
                return Result::success("opened " + stringParam(p, "path"));
            }));
}

void registerCommands(Session& session) {
    registerFileCommands(session);
    registerChangeCommands(session);
    registerArchiveCommands(session);
    registerToolCommands(session);
}

}  // namespace wetwaresuite::explorer::detail

namespace wetwaresuite::explorer {
void registerCommands(Session& session) { detail::registerCommands(session); }
}  // namespace wetwaresuite::explorer
