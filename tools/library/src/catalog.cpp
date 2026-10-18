// Catalog storage: one JSON object per line. The first line holds the roots
// and collections; every other line is one file. Paths inside the catalog's
// folder are stored relative to it, so a whole drive can move and keep its
// catalog.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/library/library.hpp"

namespace wetwaresuite::library {

using nlohmann::json;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string extOf(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower(e);
}

std::string normPath(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    std::string out = abs.lexically_normal().generic_string();
    while (out.size() > 1 && out.back() == '/' && !(out.size() == 3 && out[1] == ':')) out.pop_back();
    return out;
}

std::string Item::name() const {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool Item::hasTag(const std::string& tag) const {
    const std::string t = lower(tag);
    for (const auto& x : tags) {
        if (lower(x) == t) return true;
    }
    return false;
}

std::int64_t unixSeconds(fs::file_time_type t) {
    // file_clock has no portable epoch before C++20 clock_cast everywhere; offset from "now" instead
    static const auto offset = std::chrono::system_clock::now().time_since_epoch() -
                               std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                   fs::file_time_type::clock::now().time_since_epoch());
    auto sys = std::chrono::duration_cast<std::chrono::system_clock::duration>(t.time_since_epoch()) + offset;
    return std::chrono::duration_cast<std::chrono::seconds>(sys).count();
}

// ---- json -------------------------------------------------------------------

namespace {

fs::path baseDir(const fs::path& catalogFile) {
    fs::path dir = catalogFile.parent_path();
    return dir.empty() ? fs::path(".") : dir;
}

std::string storePath(const std::string& path, const std::string& base) {
    // inside the catalog's folder: relative, so the folder can move
    if (path.size() > base.size() && path.compare(0, base.size(), base) == 0 && path[base.size()] == '/') {
        return path.substr(base.size() + 1);
    }
    if (path == base) return ".";
    return path;
}

std::string loadPath(const std::string& stored, const fs::path& base) {
    fs::path p(stored);
    if (p.is_absolute() || (stored.size() > 1 && stored[1] == ':') || (!stored.empty() && stored[0] == '/')) return stored;
    return normPath(base / p);
}

json toJson(const Item& it, const std::string& base) {
    json j = {{"path", storePath(it.path, base)}, {"size", it.size}, {"mtime", it.mtime}, {"mtick", it.mtick},
              {"hash", it.hash}, {"type", it.type}, {"format", it.format}, {"ext", it.ext}};
    if (it.width) j["width"] = it.width;
    if (it.height) j["height"] = it.height;
    if (it.duration > 0) j["duration"] = it.duration;
    if (it.pages) j["pages"] = it.pages;
    if (it.triangles) j["triangles"] = it.triangles;
    if (!it.bounds.empty()) j["bounds"] = it.bounds;
    if (!it.tags.empty()) j["tags"] = it.tags;
    if (it.rating) j["rating"] = it.rating;
    if (!it.label.empty()) j["label"] = it.label;
    if (!it.notes.empty()) j["notes"] = it.notes;
    if (!it.dhash.empty()) j["dhash"] = it.dhash;
    if (!it.phash.empty()) j["phash"] = it.phash;
    j["indexed"] = it.indexed;
    return j;
}

Item fromJson(const json& j, const fs::path& base) {
    Item it;
    it.path = loadPath(j.value("path", ""), base);
    it.size = j.value("size", std::uint64_t{0});
    it.mtime = j.value("mtime", std::int64_t{0});
    it.mtick = j.value("mtick", std::int64_t{0});
    it.hash = j.value("hash", "");
    it.type = j.value("type", "");
    it.format = j.value("format", "");
    it.ext = j.value("ext", "");
    it.width = j.value("width", 0);
    it.height = j.value("height", 0);
    it.duration = j.value("duration", 0.0);
    it.pages = j.value("pages", 0);
    it.triangles = j.value("triangles", std::int64_t{0});
    if (j.contains("bounds")) it.bounds = j["bounds"].get<std::vector<double>>();
    if (j.contains("tags")) it.tags = j["tags"].get<std::vector<std::string>>();
    it.rating = j.value("rating", 0);
    it.label = j.value("label", "");
    it.notes = j.value("notes", "");
    it.dhash = j.value("dhash", "");
    it.phash = j.value("phash", "");
    it.indexed = j.value("indexed", std::int64_t{0});
    return it;
}

}  // namespace

Catalog Catalog::load(const fs::path& file) {
    Catalog c;
    c.file = file;
    std::ifstream in(file, std::ios::binary);
    if (!in) return c;
    const fs::path base = baseDir(fs::absolute(file));
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            throw std::runtime_error(file.string() + " line " + std::to_string(lineNo) + " is not valid JSON");
        }
        try {
            if (j.contains("wetwaresuite_library")) {
                for (const auto& r : j.value("roots", json::array())) c.roots.push_back(loadPath(r.get<std::string>(), base));
                for (const auto& cj : j.value("collections", json::array())) {
                    Collection col;
                    col.name = cj.value("name", "");
                    col.query = cj.value("query", "");
                    for (const auto& p : cj.value("paths", json::array())) col.paths.push_back(loadPath(p.get<std::string>(), base));
                    if (!col.name.empty()) c.collections[col.name] = std::move(col);
                }
                continue;
            }
            c.items.push_back(fromJson(j, base));
        } catch (const json::exception& e) {
            throw std::runtime_error(file.string() + " line " + std::to_string(lineNo) + ": " + e.what());
        }
    }
    c.rebuildIndex();
    return c;
}

void Catalog::save() const {
    const std::string base = normPath(baseDir(fs::absolute(file)));
    json header = {{"wetwaresuite_library", 1}, {"roots", json::array()}, {"collections", json::array()}};
    for (const auto& r : roots) header["roots"].push_back(storePath(r, base));
    for (const auto& [name, col] : collections) {
        json cj = {{"name", col.name}};
        if (col.smart()) cj["query"] = col.query;
        else {
            cj["paths"] = json::array();
            for (const auto& p : col.paths) cj["paths"].push_back(storePath(p, base));
        }
        header["collections"].push_back(cj);
    }
    std::error_code ec;
    if (auto parent = file.parent_path(); !parent.empty()) fs::create_directories(parent, ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << header.dump() << '\n';
        for (const auto& it : items) out << toJson(it, base).dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(file, ec);
        fs::rename(tmp, file, ec);
        if (ec) throw std::runtime_error("cannot replace " + file.string() + ": " + ec.message());
    }
}

Item* Catalog::find(const std::string& path) {
    auto it = byPath_.find(path);
    if (it == byPath_.end() || it->second >= items.size() || items[it->second].path != path) return nullptr;
    return &items[it->second];
}

const Item* Catalog::find(const std::string& path) const { return const_cast<Catalog*>(this)->find(path); }

void Catalog::rebuildIndex() {
    byPath_.clear();
    for (std::size_t i = 0; i < items.size(); ++i) byPath_[items[i].path] = i;
}

}  // namespace wetwaresuite::library
