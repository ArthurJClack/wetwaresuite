// The vault: scanning the folder, the cache, link resolution, backlinks and search.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/notes/notes.hpp"

namespace wetwaresuite::notes {

using nlohmann::json;

namespace {

constexpr int kCacheVersion = 2;

std::int64_t unixSeconds(fs::file_time_type t) {
    static const auto offset = std::chrono::system_clock::now().time_since_epoch() -
                               std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                   fs::file_time_type::clock::now().time_since_epoch());
    auto sys = std::chrono::duration_cast<std::chrono::system_clock::duration>(t.time_since_epoch()) + offset;
    return std::chrono::duration_cast<std::chrono::seconds>(sys).count();
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + fromPath(p));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string withoutMd(const std::string& rel) {
    return endsWith(lower(rel), ".md") ? rel.substr(0, rel.size() - 3) : rel;
}

std::string folderOf(const std::string& rel) {
    auto slash = rel.rfind('/');
    return slash == std::string::npos ? "" : rel.substr(0, slash);
}

std::string fileNameOf(const std::string& rel) {
    auto slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

Note makeNote(const std::string& rel, const std::string& content) {
    Note n;
    n.rel = rel;
    n.name = withoutMd(fileNameOf(rel));
    Parsed p = parseNote(content);
    n.frontmatter = std::move(p.frontmatter);
    std::set<std::string> seen;
    for (const auto& t : frontmatterTags(n.frontmatter)) {
        if (seen.insert(lower(t)).second) n.tags.push_back(t);
    }
    for (const auto& t : p.inlineTags) {
        if (seen.insert(lower(t.tag)).second) n.tags.push_back(t.tag);
    }
    n.aliases = frontmatterAliases(n.frontmatter);
    const FmValue* title = fmFind(n.frontmatter, "title");
    n.title = title && !title->isList && !trim(title->scalar).empty() ? trim(title->scalar) : n.name;
    n.links = std::move(p.links);
    for (auto& l : n.links) l.begin = l.end = 0;  // spans are re-read when rewriting
    n.headings = std::move(p.headings);
    n.tasks = std::move(p.tasks);
    n.words = p.words;
    for (auto& t : tokenize(content)) {
        ++n.terms[t];
        ++n.termCount;
    }
    std::string titles = n.name + " " + n.title;
    for (const auto& a : n.aliases) titles += " " + a;
    auto tt = tokenize(titles);
    std::sort(tt.begin(), tt.end());
    tt.erase(std::unique(tt.begin(), tt.end()), tt.end());
    n.titleTerms = std::move(tt);
    return n;
}

json noteJson(const Note& n) {
    json fm = json::array();
    for (const auto& [k, v] : n.frontmatter) {
        if (v.isList) fm.push_back({k, v.list});
        else fm.push_back({k, v.scalar});
    }
    json links = json::array();
    for (const auto& l : n.links) {
        links.push_back({l.target, l.heading, l.alias, l.embed, l.wiki, l.line});
    }
    json heads = json::array();
    for (const auto& h : n.headings) heads.push_back({h.level, h.text, h.line});
    json tasks = json::array();
    for (const auto& t : n.tasks) tasks.push_back({t.line, std::string(1, t.mark), t.text, t.due, t.indent});
    return {{"size", n.size}, {"mtick", n.mtick}, {"mtime", n.mtime}, {"title", n.title}, {"fm", fm},
            {"tags", n.tags}, {"aliases", n.aliases}, {"links", links}, {"headings", heads}, {"tasks", tasks},
            {"words", n.words}, {"terms", n.terms}, {"titleTerms", n.titleTerms}};
}

Note noteFromJson(const std::string& rel, const json& j) {
    Note n;
    n.rel = rel;
    n.name = withoutMd(fileNameOf(rel));
    n.size = j.at("size").get<std::uint64_t>();
    n.mtick = j.at("mtick").get<std::int64_t>();
    n.mtime = j.at("mtime").get<std::int64_t>();
    n.title = j.at("title").get<std::string>();
    for (const auto& e : j.at("fm")) {
        FmValue v;
        if (e[1].is_array()) {
            v.isList = true;
            v.list = e[1].get<std::vector<std::string>>();
        } else {
            v.scalar = e[1].get<std::string>();
        }
        n.frontmatter.emplace_back(e[0].get<std::string>(), std::move(v));
    }
    n.tags = j.at("tags").get<std::vector<std::string>>();
    n.aliases = j.at("aliases").get<std::vector<std::string>>();
    for (const auto& e : j.at("links")) {
        Link l;
        l.target = e[0];
        l.heading = e[1];
        l.alias = e[2];
        l.embed = e[3];
        l.wiki = e[4];
        l.line = e[5];
        n.links.push_back(std::move(l));
    }
    for (const auto& e : j.at("headings")) n.headings.push_back({e[0].get<int>(), e[1].get<std::string>(), e[2].get<int>()});
    for (const auto& e : j.at("tasks")) {
        Task t;
        t.line = e[0];
        t.mark = e[1].get<std::string>().empty() ? ' ' : e[1].get<std::string>()[0];
        t.done = t.mark != ' ';
        t.text = e[2];
        t.due = e[3];
        t.indent = e[4];
        n.tasks.push_back(std::move(t));
    }
    n.words = j.at("words").get<std::size_t>();
    n.terms = j.at("terms").get<std::map<std::string, int>>();
    for (const auto& [t, c] : n.terms) n.termCount += static_cast<std::size_t>(c);
    n.titleTerms = j.at("titleTerms").get<std::vector<std::string>>();
    return n;
}

}  // namespace

bool Note::hasTag(const std::string& raw) const {
    std::string want = lower(raw);
    while (!want.empty() && want[0] == '#') want.erase(0, 1);
    for (const auto& t : tags) {
        std::string lt = lower(t);
        if (lt == want || startsWith(lt, want + "/")) return true;
    }
    return false;
}

std::string Vault::relOf(const fs::path& absolute) const {
    std::error_code ec;
    fs::path a = fs::weakly_canonical(absolute, ec);
    if (ec) a = absolute;
    fs::path r = a.lexically_relative(root);
    std::string s = fromPath(r);
    if (s.empty() || startsWith(s, "..")) return {};
    if (s == ".") return {};
    return s;
}

void Vault::open(const fs::path& folder, bool useCache) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) throw std::runtime_error("no folder at " + fromPath(folder));
    root = fs::weakly_canonical(fs::absolute(folder), ec);
    if (ec) root = fs::absolute(folder);
    notes.clear();
    files.clear();
    loadSettings();
    scan(useCache);
    rebuild();
    saveCache();
}

void Vault::loadSettings() {
    settings = Settings{};
    auto readJson = [&](const char* name) -> json {
        std::error_code ec;
        fs::path p = root / ".obsidian" / name;
        if (!fs::exists(p, ec)) return json::object();
        try {
            return json::parse(readFile(p));
        } catch (const std::exception&) {
            return json::object();
        }
    };
    json daily = readJson("daily-notes.json");
    if (daily.is_object()) {
        if (daily.contains("folder") && daily["folder"].is_string()) settings.dailyFolder = daily["folder"];
        if (daily.contains("format") && daily["format"].is_string() && !daily["format"].get<std::string>().empty()) settings.dailyFormat = daily["format"];
        if (daily.contains("template") && daily["template"].is_string()) settings.dailyTemplate = daily["template"];
    }
    json tmpl = readJson("templates.json");
    if (tmpl.is_object()) {
        if (tmpl.contains("folder") && tmpl["folder"].is_string()) settings.templatesFolder = tmpl["folder"];
        if (tmpl.contains("dateFormat") && tmpl["dateFormat"].is_string() && !tmpl["dateFormat"].get<std::string>().empty()) settings.dateFormat = tmpl["dateFormat"];
        if (tmpl.contains("timeFormat") && tmpl["timeFormat"].is_string() && !tmpl["timeFormat"].get<std::string>().empty()) settings.timeFormat = tmpl["timeFormat"];
    }
    auto clean = [](std::string& f) {
        while (!f.empty() && (f.back() == '/' || f.back() == '\\')) f.pop_back();
        while (!f.empty() && f[0] == '/') f.erase(0, 1);
    };
    clean(settings.dailyFolder);
    clean(settings.templatesFolder);
}

void Vault::loadCache(std::map<std::string, Note>& out) const {
    std::error_code ec;
    fs::path p = root / kCacheDir / "cache.json";
    if (!fs::exists(p, ec)) return;
    try {
        json j = json::parse(readFile(p));
        if (j.value("version", 0) != kCacheVersion) return;
        for (const auto& [rel, v] : j.at("notes").items()) out.emplace(rel, noteFromJson(rel, v));
    } catch (const std::exception&) {
        out.clear();  // a corrupt cache is just ignored and rewritten
    }
}

void Vault::saveCache() const {
    std::error_code ec;
    fs::path dir = root / kCacheDir;
    fs::create_directories(dir, ec);
    if (ec) return;  // read-only vault: work without a cache
    fs::remove_all(root / kOldCacheDir, ec);  // replaced by the new cache folder
    json j = {{"version", kCacheVersion}, {"notes", json::object()}};
    for (const auto& [rel, n] : notes) j["notes"][rel] = noteJson(n);
    {
        std::ofstream out(dir / "cache.json.tmp", std::ios::binary);
        if (!out) return;
        out << j.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    fs::rename(dir / "cache.json.tmp", dir / "cache.json", ec);
    if (ec) {
        fs::remove(dir / "cache.json", ec);
        fs::rename(dir / "cache.json.tmp", dir / "cache.json", ec);
    }
    std::ofstream readme(dir / "README.txt", std::ios::binary);
    readme << "This folder is a cache made by wetwaresuite notes. It can be deleted at any time; it is rebuilt from the .md files.\n";
}

void Vault::clearCache() {
    std::error_code ec;
    fs::remove_all(root / kCacheDir, ec);
    fs::remove_all(root / kOldCacheDir, ec);
}

bool Vault::scan(bool useCache) {
    // Unchanged notes stay where they are in `notes`, so the indexes' pointers to them stay valid.
    std::map<std::string, Note> cached;
    if (useCache && notes.empty()) loadCache(cached);
    std::set<std::string> seenNotes;
    std::map<std::string, FileEntry> nextFiles;
    bool changed = false;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        std::string name = fromPath(path.filename());
        if (!name.empty() && name[0] == '.') {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) {
            if (name == "node_modules") it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        std::string rel = fromPath(path.lexically_relative(root));
        auto t = it->last_write_time(ec);
        if (ec) continue;
        auto size = it->file_size(ec);
        if (ec) continue;
        const auto tick = static_cast<std::int64_t>(t.time_since_epoch().count());
        if (endsWith(lower(name), ".md")) {
            seenNotes.insert(rel);
            auto old = notes.find(rel);
            if (old != notes.end() && old->second.mtick == tick && old->second.size == size) continue;
            changed = true;
            auto c = cached.find(rel);
            if (c != cached.end() && c->second.mtick == tick && c->second.size == size) {
                notes.insert_or_assign(rel, std::move(c->second));
                continue;
            }
            try {
                Note n = makeNote(rel, readFile(path));
                n.size = size;
                n.mtick = tick;
                n.mtime = unixSeconds(t);
                notes.insert_or_assign(rel, std::move(n));
            } catch (const std::exception&) {
                seenNotes.erase(rel);
            }
        } else {
            FileEntry f{rel, size, tick, unixSeconds(t)};
            auto old = files.find(rel);
            if (old == files.end() || old->second.mtick != tick || old->second.size != size) changed = true;
            nextFiles.emplace(rel, f);
        }
    }
    for (auto n = notes.begin(); n != notes.end();) {
        if (!seenNotes.count(n->first)) {
            n = notes.erase(n);
            changed = true;
        } else {
            ++n;
        }
    }
    if (nextFiles.size() != files.size()) changed = true;
    files = std::move(nextFiles);
    return changed;
}

bool Vault::refresh() {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) throw std::runtime_error("the vault folder " + fromPath(root) + " is gone");
    loadSettings();
    if (!scan(false)) return false;
    rebuild();
    saveCache();
    return true;
}

void Vault::rebuild() {
    byName_.clear();
    byPath_.clear();
    byAlias_.clear();
    index_.clear();
    incoming_.clear();
    std::size_t totalTerms = 0;
    for (const auto& [rel, n] : notes) {
        byName_[lower(n.name)].push_back(rel);
        byName_[lower(fileNameOf(rel))].push_back(rel);
        byPath_[lower(rel)] = rel;
        byPath_[lower(withoutMd(rel))] = rel;
        for (const auto& a : n.aliases) byAlias_[lower(a)].push_back(rel);
        if (n.title != n.name) byAlias_[lower(n.title)].push_back(rel);
        for (const auto& [term, count] : n.terms) index_[term].emplace_back(&n, count);
        totalTerms += n.termCount;
    }
    for (const auto& [rel, f] : files) {
        byName_[lower(fileNameOf(rel))].push_back(rel);
        byPath_[lower(rel)] = rel;
    }
    avgLength_ = notes.empty() ? 1.0 : std::max(1.0, static_cast<double>(totalTerms) / static_cast<double>(notes.size()));
    for (const auto& [rel, n] : notes) {
        for (const auto& l : n.links) {
            std::string to = resolve(l, rel);
            if (!to.empty() && to != rel) incoming_[to].emplace_back(&n, &l);
        }
    }
}

const Note* Vault::note(const std::string& rel) const {
    auto it = notes.find(rel);
    return it == notes.end() ? nullptr : &it->second;
}

std::string Vault::resolve(const Link& link, const std::string& fromRel) const {
    return resolve(link.target, fromRel, !link.wiki);
}

std::string Vault::resolve(const std::string& rawTarget, const std::string& fromRel, bool markdownLink) const {
    std::string t = trim(rawTarget);
    for (auto& c : t) {
        if (c == '\\') c = '/';
    }
    if (t.empty()) return notes.count(fromRel) ? fromRel : "";
    while (startsWith(t, "./")) t = t.substr(2);
    auto lookup = [&](const std::string& key) -> std::string {
        auto it = byPath_.find(lower(key));
        return it == byPath_.end() ? "" : it->second;
    };
    const std::string from = folderOf(fromRel);
    if (markdownLink || startsWith(t, "../")) {
        std::string joined = fromPath((toPath(from.empty() ? "." : from) / toPath(t)).lexically_normal());
        if (auto r = lookup(joined); !r.empty()) return r;
    }
    if (t[0] == '/') t = t.substr(1);
    if (auto r = lookup(t); !r.empty()) return r;
    if (!from.empty() && !markdownLink) {
        if (auto r = lookup(from + "/" + t); !r.empty()) return r;
    }
    const std::string lt = lower(t);
    if (lt.find('/') != std::string::npos) {
        // a partial path: the shortest file whose path ends with it
        std::string best;
        for (const auto& [key, rel] : byPath_) {
            if (endsWith(key, "/" + lt) && (best.empty() || rel.size() < best.size())) best = rel;
        }
        if (!best.empty()) return best;
    }
    auto pick = [&](const std::vector<std::string>& cands) -> std::string {
        if (cands.empty()) return "";
        std::string best;
        for (const auto& c : cands) {
            if (folderOf(c) == from) return c;
            if (best.empty() || c.size() < best.size() || (c.size() == best.size() && c < best)) best = c;
        }
        return best;
    };
    if (auto it = byName_.find(fileNameOf(lt)); it != byName_.end() && lt.find('/') == std::string::npos) {
        return pick(it->second);
    }
    if (auto it = byAlias_.find(lt); it != byAlias_.end()) return pick(it->second);
    return "";
}

std::string Vault::findNote(const std::string& raw) const {
    std::string s = trim(raw);
    if (startsWith(s, "[[") && endsWith(s, "]]")) s = s.substr(2, s.size() - 4);
    if (auto bar = s.find('|'); bar != std::string::npos) s = s.substr(0, bar);
    if (s.empty()) return "";
    fs::path asPath = toPath(s);
    std::error_code ec;
    if (asPath.is_absolute() && fs::exists(asPath, ec)) {
        std::string rel = relOf(asPath);
        if (notes.count(rel) || files.count(rel)) return rel;
    }
    std::string r = resolve(s, "", false);
    if (!r.empty()) return r;
    // a title or alias written differently
    for (const auto& [rel, n] : notes) {
        if (lower(n.title) == lower(s)) return rel;
    }
    return "";
}

std::vector<std::pair<const Note*, const Link*>> Vault::backlinks(const std::string& rel) const {
    auto it = incoming_.find(rel);
    if (it == incoming_.end()) return {};
    return it->second;
}

std::size_t Vault::linkCount() const {
    std::size_t n = 0;
    for (const auto& [rel, note] : notes) n += note.links.size();
    return n;
}

std::string Vault::read(const std::string& rel) const { return readFile(abs(rel)); }

std::string Vault::linkText(const std::string& rel) const {
    const bool note = notes.count(rel) > 0;
    std::string name = note ? withoutMd(fileNameOf(rel)) : fileNameOf(rel);
    auto it = byName_.find(lower(name));
    std::size_t same = 0;
    if (it != byName_.end()) {
        std::set<std::string> uniq(it->second.begin(), it->second.end());
        same = uniq.size();
    }
    if (same <= 1) return name;
    return note ? withoutMd(rel) : rel;
}

// ---- search -----------------------------------------------------------------------------

namespace {

struct QTerm {
    enum Kind { Word, Phrase, Prefix, Tag, Path, File } kind = Word;
    bool neg = false;
    std::string text;                 // raw (lowercase)
    std::vector<std::string> tokens;  // for words and phrases
};

std::vector<QTerm> parseQuery(const std::string& q) {
    std::vector<QTerm> out;
    std::size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && std::isspace(static_cast<unsigned char>(q[i]))) ++i;
        if (i >= q.size()) break;
        QTerm t;
        if (q[i] == '-' && i + 1 < q.size() && !std::isspace(static_cast<unsigned char>(q[i + 1]))) {
            t.neg = true;
            ++i;
        }
        std::string word;
        bool quoted = false;
        // field:"quoted value" or "phrase" or word
        while (i < q.size() && !std::isspace(static_cast<unsigned char>(q[i]))) {
            if (q[i] == '"') {
                auto close = q.find('"', i + 1);
                if (close == std::string::npos) close = q.size();
                word += q.substr(i + 1, close - i - 1);
                quoted = true;
                i = close + 1;
                continue;
            }
            word += q[i++];
        }
        std::string lw = lower(word);
        auto field = [&](const char* name, QTerm::Kind kind) {
            std::string prefix = std::string(name) + ":";
            if (!quoted || startsWith(lw, prefix)) {
                if (startsWith(lw, prefix)) {
                    t.kind = kind;
                    t.text = trim(lw.substr(prefix.size()));
                    return true;
                }
            }
            return false;
        };
        if (field("tag", QTerm::Tag) || field("path", QTerm::Path) || field("file", QTerm::File)) {
            while (t.kind == QTerm::Tag && !t.text.empty() && t.text[0] == '#') t.text.erase(0, 1);
            if (!t.text.empty()) out.push_back(t);
            continue;
        }
        if (!quoted && lw.size() > 1 && lw[0] == '#') {
            t.kind = QTerm::Tag;
            t.text = lw.substr(1);
            out.push_back(t);
            continue;
        }
        if (!quoted && lw.size() > 1 && lw.back() == '*') {
            auto toks = tokenize(lw.substr(0, lw.size() - 1));
            if (toks.size() == 1) {
                t.kind = QTerm::Prefix;
                t.text = toks[0];
                t.tokens = toks;
                out.push_back(t);
                continue;
            }
        }
        t.tokens = tokenize(lw);
        if (t.tokens.empty()) continue;
        t.text = lw;
        t.kind = t.tokens.size() > 1 || quoted ? QTerm::Phrase : QTerm::Word;
        if (t.tokens.size() == 1) t.kind = QTerm::Word;
        out.push_back(t);
    }
    return out;
}

bool containsSequence(const std::vector<std::string>& hay, const std::vector<std::string>& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < needle.size() && ok; ++k) ok = hay[i + k] == needle[k];
        if (ok) return true;
    }
    return false;
}

bool wordStart(const std::string& s, std::size_t pos) {
    return pos == 0 || !(std::isalnum(static_cast<unsigned char>(s[pos - 1])) || static_cast<unsigned char>(s[pos - 1]) >= 0x80);
}

std::string makeSnippet(const std::string& text, const std::vector<std::string>& needles, int& lineOut) {
    std::size_t bodyStart = 0;
    splitFrontmatter(text, nullptr, &bodyStart);
    const std::string body = text.substr(bodyStart);
    const std::string lb = lower(body);
    std::size_t best = std::string::npos;
    for (const auto& n : needles) {
        if (n.empty()) continue;
        for (auto p = lb.find(n); p != std::string::npos; p = lb.find(n, p + 1)) {
            if (wordStart(lb, p)) {
                best = std::min(best, p);
                break;
            }
        }
    }
    if (best == std::string::npos) best = 0;
    // from the start of the matching line, unless the match is far into a long line
    std::size_t lineStart = body.rfind('\n', best == 0 ? 0 : best - 1);
    lineStart = lineStart == std::string::npos || best == 0 ? 0 : lineStart + 1;
    std::size_t start = lineStart;
    if (best - lineStart > 80) {
        start = best - 60;
        auto sp = body.find_first_of(" \t", start);
        if (sp != std::string::npos && sp < best) start = sp + 1;
    }
    if (best == 0) {  // no match in the body (a title match): skip blank lines and the first heading
        while (start < body.size() && (body[start] == '\n' || body[start] == '\r')) ++start;
    }
    while (start < body.size() && (static_cast<unsigned char>(body[start]) & 0xC0) == 0x80) ++start;
    std::size_t end = std::min(body.size(), best + 140);
    if (end < body.size()) {
        auto sp = body.find_last_of(" \n\t", end);
        if (sp != std::string::npos && sp > best) end = sp;
        while (end < body.size() && (static_cast<unsigned char>(body[end]) & 0xC0) == 0x80) ++end;
    }
    lineOut = static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(bodyStart + best), '\n')) + 1;
    std::string raw = body.substr(start, end - start);
    std::string flat;
    bool space = false;
    for (char c : raw) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = true;
            continue;
        }
        if (space && !flat.empty()) flat += ' ';
        space = false;
        flat += c;
    }
    // bold the matches
    const std::string lf = lower(flat);
    std::vector<char> mark(flat.size(), 0);
    for (const auto& n : needles) {
        if (n.empty()) continue;
        for (auto p = lf.find(n); p != std::string::npos; p = lf.find(n, p + n.size())) {
            if (!wordStart(lf, p)) continue;
            std::size_t e = p + n.size();
            while (e < lf.size() && (std::isalnum(static_cast<unsigned char>(lf[e])) || static_cast<unsigned char>(lf[e]) >= 0x80)) ++e;
            for (std::size_t k = p; k < e; ++k) mark[k] = 1;
        }
    }
    std::string out = start > lineStart ? "..." : "";
    for (std::size_t k = 0; k < flat.size(); ++k) {
        if (mark[k] && (k == 0 || !mark[k - 1])) out += "**";
        out += flat[k];
        if (mark[k] && (k + 1 == flat.size() || !mark[k + 1])) out += "**";
    }
    if (end < body.size()) out += "...";
    return out;
}

}  // namespace

SearchResults Vault::search(const std::string& query, std::size_t limit, bool snippets) const {
    auto terms = parseQuery(query);
    // candidates from the positive text terms
    std::map<const Note*, double> scores;
    bool haveText = false;
    std::vector<const QTerm*> phrases;
    const double N = static_cast<double>(std::max<std::size_t>(1, notes.size()));
    auto bm25 = [&](const Note& n, int tf, std::size_t df) {
        double idf = std::log(1.0 + (N - static_cast<double>(df) + 0.5) / (static_cast<double>(df) + 0.5));
        const double len = static_cast<double>(n.termCount);
        double k1 = 1.2, b = 0.75;
        return idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * len / avgLength_));
    };
    std::vector<std::map<const Note*, double>> perTerm;
    for (const auto& t : terms) {
        if (t.neg || (t.kind != QTerm::Word && t.kind != QTerm::Phrase && t.kind != QTerm::Prefix)) continue;
        haveText = true;
        if (t.kind == QTerm::Phrase) phrases.push_back(&t);
        std::map<const Note*, double> m;
        if (t.kind == QTerm::Prefix) {
            for (auto it = index_.lower_bound(t.text); it != index_.end() && startsWith(it->first, t.text); ++it) {
                for (const auto& [n, c] : it->second) m[n] += bm25(*n, c, it->second.size());
            }
            perTerm.push_back(std::move(m));
            continue;
        }
        bool first = true;
        for (const auto& tok : t.tokens) {
            std::map<const Note*, double> mm;
            auto it = index_.find(tok);
            if (it != index_.end()) {
                for (const auto& [n, c] : it->second) {
                    double s = bm25(*n, c, it->second.size());
                    if (std::binary_search(n->titleTerms.begin(), n->titleTerms.end(), tok)) s += 2.0 + s;
                    mm[n] = s;
                }
            }
            if (first) {
                m = std::move(mm);
                first = false;
            } else {
                std::map<const Note*, double> both;
                for (const auto& [n, s] : m) {
                    if (auto f = mm.find(n); f != mm.end()) both[n] = s + f->second;
                }
                m = std::move(both);
            }
        }
        perTerm.push_back(std::move(m));
    }
    if (haveText) {
        for (std::size_t i = 0; i < perTerm.size(); ++i) {
            if (i == 0) {
                scores = perTerm[0];
                continue;
            }
            std::map<const Note*, double> both;
            for (const auto& [n, s] : scores) {
                if (auto f = perTerm[i].find(n); f != perTerm[i].end()) both[n] = s + f->second;
            }
            scores = std::move(both);
        }
    } else {
        for (const auto& [rel, n] : notes) scores[&n] = 0;
    }
    std::vector<SearchHit> hits;
    for (const auto& [n, s] : scores) {
        bool keep = true;
        for (const auto& t : terms) {
            bool match = true;
            switch (t.kind) {
                case QTerm::Tag: match = n->hasTag(t.text); break;
                case QTerm::Path: match = lower(n->rel).find(t.text) != std::string::npos; break;
                case QTerm::File: match = lower(n->name).find(t.text) != std::string::npos; break;
                case QTerm::Word:
                    if (!t.neg) continue;
                    match = n->terms.count(t.tokens[0]) > 0;
                    break;
                case QTerm::Prefix: {
                    if (!t.neg) continue;
                    auto it = n->terms.lower_bound(t.text);
                    match = it != n->terms.end() && startsWith(it->first, t.text);
                    break;
                }
                case QTerm::Phrase: {
                    if (!t.neg) continue;
                    bool all = true;
                    for (const auto& tok : t.tokens) all = all && n->terms.count(tok);
                    if (all) {
                        try {
                            match = containsSequence(tokenize(read(n->rel)), t.tokens);
                        } catch (const std::exception&) {
                            match = false;
                        }
                    } else {
                        match = false;
                    }
                    break;
                }
            }
            if (t.neg ? match : !match) {
                keep = false;
                break;
            }
        }
        if (!keep) continue;
        double score = s;
        if (!phrases.empty()) {
            std::vector<std::string> toks;
            try {
                toks = tokenize(read(n->rel));
            } catch (const std::exception&) {
                continue;
            }
            bool all = true;
            for (const auto* ph : phrases) all = all && containsSequence(toks, ph->tokens);
            if (!all) continue;
            score += 1.0;
        }
        hits.push_back({n, score, {}, 0});
    }
    std::sort(hits.begin(), hits.end(), [&](const SearchHit& a, const SearchHit& b) {
        if (!haveText && a.note->mtime != b.note->mtime) return a.note->mtime > b.note->mtime;
        if (a.score != b.score) return a.score > b.score;
        return a.note->rel < b.note->rel;
    });
    SearchResults out;
    out.total = hits.size();
    if (limit > 0 && hits.size() > limit) hits.resize(limit);
    if (snippets) {
        std::vector<std::string> needles;
        for (const auto& t : terms) {
            if (t.neg) continue;
            if (t.kind == QTerm::Phrase) needles.push_back(t.text);
            if (t.kind == QTerm::Word || t.kind == QTerm::Prefix) needles.push_back(t.tokens[0]);
        }
        for (auto& h : hits) {
            try {
                h.snippet = makeSnippet(read(h.note->rel), needles, h.line);
            } catch (const std::exception&) {
            }
        }
    }
    out.hits = std::move(hits);
    return out;
}

}  // namespace wetwaresuite::notes
