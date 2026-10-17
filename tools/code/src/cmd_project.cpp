// code.* commands for a project folder: open it, list its tree, search and
// replace across its files, count lines by language.
#include <algorithm>
#include <regex>

#include "internal.hpp"

namespace wetwaresuite::code::detail {

namespace {

using std::int64_t;

bool wildcard(const char* pat, const char* s) {
    for (; *pat; ++pat, ++s) {
        if (*pat == '*') {
            while (pat[1] == '*') ++pat;
            for (const char* t = s;; ++t) {
                if (wildcard(pat + 1, t)) return true;
                if (!*t) return false;
            }
        }
        if (!*s) return false;
        char a = *pat, b = *s;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != '?' && a != b) return false;
    }
    return !*s;
}

// "*.cpp;*.h" or "*.cpp, *.h"; empty matches everything
bool nameMatches(const std::string& globs, const std::string& name) {
    if (globs.empty()) return true;
    std::string cur;
    for (char c : globs + ";") {
        if (c == ';' || c == ',' || c == ' ') {
            if (!cur.empty() && wildcard(cur.c_str(), name.c_str())) return true;
            cur.clear();
        } else {
            cur += c;
        }
    }
    return false;
}

std::regex searchRegex(const Params& p) {
    std::string pattern = stringParam(p, "find");
    if (pattern.empty()) throw std::runtime_error("find is empty");
    if (!boolParam(p, "regex", false)) {
        static const std::regex special(R"([.^$|()\[\]{}*+?\\])");
        pattern = std::regex_replace(pattern, special, R"(\$&)");
    }
    if (boolParam(p, "whole_word", false)) pattern = "\\b(?:" + pattern + ")\\b";
    auto flags = std::regex::ECMAScript;
    if (!boolParam(p, "match_case", false)) flags |= std::regex::icase;
    try {
        return std::regex(pattern, flags);
    } catch (const std::regex_error& e) {
        throw std::runtime_error("bad pattern: " + std::string(e.what()));
    }
}

std::vector<ParamSpec> searchParams() {
    return {{"find", ParamType::String, "text to look for (a regular expression with regex=true)", true},
            opt("folder", ParamType::String, "folder to search (default: the open project, else the current folder)"),
            opt("files", ParamType::String, "only file names like these, e.g. \"*.cpp;*.h\""),
            opt("regex", ParamType::Bool, "treat find as a regular expression", false),
            opt("match_case", ParamType::Bool, "upper and lower case must match", false),
            opt("whole_word", ParamType::Bool, "only whole words", false)};
}

std::string rel(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    fs::path r = fs::relative(p, root, ec);
    return sysutil::fromPath(ec || r.empty() ? p : r);
}

// undo writes every changed file back as it was
class ReplaceInFiles : public Action {
public:
    ReplaceInFiles(Session& s, StatePtr st, Params p) : s_(s), st_(std::move(st)), p_(std::move(p)) {}
    Result apply() override {
        return guard([&] { return run(); });
    }
    void revert() override {
        for (const auto& [path, bytes] : before_) {
            try {
                sysutil::writeFile(path, bytes);
                refresh(path, bytes);
            } catch (const std::exception&) {
            }
        }
        before_.clear();
    }

private:
    Result run() {
        before_.clear();
        const fs::path root = folderParam(*st_, p_);
        std::regex re = searchRegex(p_);
        std::string with = stringParam(p_, "replace");
        if (!boolParam(p_, "regex", false)) {
            std::string esc;
            for (char c : with) esc += c == '$' ? std::string("$$") : std::string(1, c);
            with = esc;
        }
        const bool dry = boolParam(p_, "dry_run", false);
        const std::string globs = stringParam(p_, "files");
        std::size_t total = 0, files = 0;
        std::string text, rows;
        for (const auto& file : projectFiles(root)) {
            if (!nameMatches(globs, sysutil::nameOf(file))) continue;
            std::string bytes;
            try {
                bytes = sysutil::readFile(file);
            } catch (const std::exception&) {
                continue;
            }
            if (looksBinary(bytes) || bytes.size() > (8u << 20)) continue;
            auto n = std::distance(std::sregex_iterator(bytes.begin(), bytes.end(), re), std::sregex_iterator());
            if (!n) continue;
            ++files;
            total += static_cast<std::size_t>(n);
            text += rel(file, root) + ": " + std::to_string(n) + "\n";
            rows += sysutil::fromPath(file) + "\t" + std::to_string(n) + "\n";
            if (dry) continue;
            std::string out = std::regex_replace(bytes, re, with);
            before_.emplace_back(file, bytes);
            sysutil::writeFile(file, out);
            refresh(file, out);
        }
        if (!total) return Result::failure("no matches");
        return Result::success((dry ? "would replace " : "replaced ") + sysutil::plural(total, "match") + " in " + sysutil::plural(files, "file") + "\n" + text)
            .with("count", static_cast<int64_t>(total))
            .with("files", static_cast<int64_t>(files))
            .with("rows", rows);
    }

    // an open, unmodified copy of the file follows the change
    void refresh(const fs::path& file, const std::string& bytes) {
        const std::string path = absText(file);
        for (const auto& id : s_.workspace().ids()) {
            auto doc = s_.workspace().getAs<CodeDoc>(id);
            if (doc && doc->path == path && !doc->modified()) loadText(*doc, bytes);
        }
    }

    Session& s_;
    StatePtr st_;
    Params p_;
    std::vector<std::pair<fs::path, std::string>> before_;
};

void treeRows(const fs::path& dir, const fs::path& root, int depth, int maxDepth, bool hidden, std::string& text, std::string& rows, std::size_t& n) {
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) entries.push_back(e);
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        std::error_code e1, e2;
        bool da = a.is_directory(e1), db = b.is_directory(e2);
        if (da != db) return da;
        return sysutil::lowerAscii(sysutil::nameOf(a.path())) < sysutil::lowerAscii(sysutil::nameOf(b.path()));
    });
    for (const auto& e : entries) {
        if (n >= 5000) return;
        const std::string name = sysutil::nameOf(e.path());
        if (!hidden && !name.empty() && name[0] == '.') continue;
        const bool isDir = e.is_directory(ec);
        ++n;
        const std::uintmax_t size = isDir ? 0 : e.file_size(ec);
        text += std::string(static_cast<std::size_t>(depth) * 2, ' ') + name + (isDir ? "/" : "") + "\n";
        rows += std::to_string(depth) + "\t" + (isDir ? "dir" : "file") + "\t" + sysutil::fromPath(e.path()) + "\t" + std::to_string(ec ? 0 : size) + "\n";
        if (isDir && depth + 1 < maxDepth && name != ".git" && name != "node_modules") treeRows(e.path(), root, depth + 1, maxDepth, hidden, text, rows, n);
    }
}

}  // namespace

void registerProjectCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"code.project", "Open folder",
             "Open a project folder (or show the open one): its build system, the files in it by language and what "
             "build/test/run would do. Search, tree, build and git default to this folder.",
             {opt("folder", ParamType::String, "the folder (empty = show the open project)")}, false, true},
            reader([st](const Params& p) {
                if (!stringParam(p, "folder").empty()) {
                    fs::path f = sysutil::toPath(stringParam(p, "folder"));
                    std::error_code ec;
                    if (!fs::is_directory(f, ec)) {
                        if (fs::is_regular_file(f, ec)) f = findProjectRoot(fs::absolute(f, ec));
                        if (f.empty() || !fs::is_directory(f, ec)) throw std::runtime_error("not a folder: " + stringParam(p, "folder"));
                    }
                    st->project = sysutil::toPath(absText(f));
                }
                if (st->project.empty()) return Result::success("no project folder open (give folder=)");
                const std::string kind = projectKind(st->project);
                auto files = projectFiles(st->project);
                std::map<std::string, std::size_t> byLang;
                for (const auto& f : files) byLang[detectLanguage(sysutil::nameOf(f)).name]++;
                std::vector<std::pair<std::size_t, std::string>> top;
                for (const auto& [l, n] : byLang) top.emplace_back(n, l);
                std::sort(top.rbegin(), top.rend());
                std::string langs;
                for (std::size_t i = 0; i < top.size() && i < 6; ++i) langs += (i ? ", " : "") + top[i].second + " " + std::to_string(top[i].first);
                std::string build;
                try {
                    build = planProject(st->project, "build").description;
                } catch (const std::exception& e) {
                    build = e.what();
                }
                std::error_code ec;
                const bool git = fs::exists(st->project / ".git", ec);
                std::string text = sysutil::fromPath(st->project) + "\n" + sysutil::plural(files.size(), "file") + (langs.empty() ? "" : ": " + langs) +
                                   "\nbuild system: " + (kind.empty() ? "none found" : kind) + "\nbuild: " + build + "\ngit: " + (git ? "yes" : "no");
                return Result::success(text)
                    .with("folder", sysutil::fromPath(st->project))
                    .with("kind", kind)
                    .with("files", static_cast<int64_t>(files.size()))
                    .with("git", git);
            }));

    reg.add({"code.tree", "File tree",
             "The files and folders of a folder as an indented tree (folders first). depth=1 lists one level.",
             {opt("folder", ParamType::String, "folder (default: the open project)"), opt("depth", ParamType::Int, "how many levels", int64_t{3}),
              opt("hidden", ParamType::Bool, "include names starting with a dot", false)},
             false, true},
            reader([st](const Params& p) {
                const fs::path root = folderParam(*st, p);
                std::error_code ec;
                if (!fs::is_directory(root, ec)) throw std::runtime_error("not a folder: " + sysutil::fromPath(root));
                std::string text, rows;
                std::size_t n = 0;
                treeRows(root, root, 0, static_cast<int>(std::max<int64_t>(1, intParam(p, "depth", 3))), boolParam(p, "hidden", false), text, rows, n);
                return Result::success(sysutil::fromPath(root) + "\n" + text).with("count", static_cast<int64_t>(n)).with("rows", rows).with("folder", sysutil::fromPath(root));
            }));

    reg.add({"code.search", "Search in files",
             "Search every text file in a folder (skips .git, node_modules, build output). Each match with file, line and text.",
             [] {
                 auto ps = searchParams();
                 ps.push_back(opt("limit", ParamType::Int, "stop after this many matches", int64_t{1000}));
                 return ps;
             }(),
             false, true},
            reader([st](const Params& p) {
                const fs::path root = folderParam(*st, p);
                std::regex re = searchRegex(p);
                const std::string globs = stringParam(p, "files");
                const std::size_t limit = static_cast<std::size_t>(std::max<int64_t>(1, intParam(p, "limit", 1000)));
                std::size_t n = 0, files = 0;
                std::string text, rows;
                for (const auto& file : projectFiles(root)) {
                    if (n >= limit) break;
                    if (!nameMatches(globs, sysutil::nameOf(file))) continue;
                    std::string bytes;
                    try {
                        bytes = sysutil::readFile(file);
                    } catch (const std::exception&) {
                        continue;
                    }
                    if (looksBinary(bytes) || bytes.size() > (8u << 20)) continue;
                    auto ls = lines(bytes);
                    bool any = false;
                    for (std::size_t i = 0; i < ls.size() && n < limit; ++i) {
                        std::smatch m;
                        if (!std::regex_search(ls[i], m, re) || m.length(0) == 0) continue;
                        ++n;
                        any = true;
                        std::string shown = ls[i].size() > 200 ? ls[i].substr(0, 200) + "..." : ls[i];
                        text += rel(file, root) + ":" + std::to_string(i + 1) + ": " + sysutil::trim(shown) + "\n";
                        rows += sysutil::fromPath(file) + "\t" + std::to_string(i + 1) + "\t" + std::to_string(m.position(0)) + "\t" + std::to_string(m.length(0)) + "\t" + shown + "\n";
                    }
                    if (any) ++files;
                }
                return Result::success(n ? sysutil::plural(n, "match") + " in " + sysutil::plural(files, "file") + (n >= limit ? " (limit reached)" : "") + "\n" + text : "no matches",
                                       static_cast<int64_t>(n))
                    .with("count", static_cast<int64_t>(n))
                    .with("files", static_cast<int64_t>(files))
                    .with("rows", rows);
            }));

    reg.add({"code.replace_in_files", "Replace in files",
             "Replace matches in every text file of a folder (undo writes the files back). dry_run=true only counts. Open "
             "files without unsaved changes are refreshed.",
             [] {
                 auto ps = searchParams();
                 ps.push_back({"replace", ParamType::String, "the replacement ($1, $2 for groups with regex=true)", true});
                 ps.push_back(opt("dry_run", ParamType::Bool, "only count what would change", false));
                 return ps;
             }(),
             true},
            [&s, st](const Params& p) -> std::unique_ptr<Action> {
                if (boolParam(p, "dry_run", false)) {
                    auto a = std::make_shared<ReplaceInFiles>(s, st, p);
                    return std::make_unique<FunctionAction>([a] { return a->apply(); });
                }
                return std::make_unique<ReplaceInFiles>(s, st, p);
            });

    reg.add({"code.count", "Count lines",
             "Count files, lines, blank lines and comment lines by language in a folder.",
             {opt("folder", ParamType::String, "folder (default: the open project)")}, false, true},
            reader([st](const Params& p) {
                const fs::path root = folderParam(*st, p);
                struct Tally {
                    std::size_t files = 0, lines = 0, blank = 0, comment = 0;
                };
                std::map<std::string, Tally> by;
                for (const auto& file : projectFiles(root)) {
                    const std::string fileName = sysutil::nameOf(file);
                    const Language* langp = &detectLanguage(fileName);
                    const Language& lang = *langp;
                    if (lang.id == "text") continue;
                    std::string bytes;
                    try {
                        bytes = sysutil::readFile(file);
                    } catch (const std::exception&) {
                        continue;
                    }
                    if (looksBinary(bytes) || bytes.size() > (8u << 20)) continue;
                    Tally& t = by[lang.name];
                    ++t.files;
                    for (const auto& line : lines(bytes)) {
                        ++t.lines;
                        std::string tl = sysutil::trim(line);
                        if (tl.empty()) ++t.blank;
                        else if (std::any_of(lang.lineComments.begin(), lang.lineComments.end(), [&](const std::string& c) { return tl.rfind(c, 0) == 0; })) ++t.comment;
                    }
                }
                std::vector<std::pair<std::string, Tally>> rowsv(by.begin(), by.end());
                std::sort(rowsv.begin(), rowsv.end(), [](const auto& a, const auto& b) { return a.second.lines > b.second.lines; });
                char buf[160];
                std::snprintf(buf, sizeof buf, "%-16s %7s %9s %8s %9s\n", "language", "files", "lines", "blank", "comment");
                std::string text = buf, rows;
                Tally all;
                for (const auto& [name, t] : rowsv) {
                    std::snprintf(buf, sizeof buf, "%-16s %7zu %9zu %8zu %9zu\n", name.c_str(), t.files, t.lines, t.blank, t.comment);
                    text += buf;
                    rows += name + "\t" + std::to_string(t.files) + "\t" + std::to_string(t.lines) + "\t" + std::to_string(t.blank) + "\t" + std::to_string(t.comment) + "\n";
                    all.files += t.files;
                    all.lines += t.lines;
                    all.blank += t.blank;
                    all.comment += t.comment;
                }
                std::snprintf(buf, sizeof buf, "%-16s %7zu %9zu %8zu %9zu\n", "total", all.files, all.lines, all.blank, all.comment);
                text += buf;
                return Result::success(text).with("files", static_cast<int64_t>(all.files)).with("lines", static_cast<int64_t>(all.lines)).with("rows", rows);
            }));
}

}  // namespace wetwaresuite::code::detail
