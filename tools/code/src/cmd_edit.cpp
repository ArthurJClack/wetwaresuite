// code.* commands for one open file: open, new, edit, find and replace, save,
// close, go to line, outline and highlighting.
#include <algorithm>
#include <regex>

#include "internal.hpp"

namespace wetwaresuite::code::detail {

namespace {

using std::int64_t;

class CloseAction : public Action {
public:
    CloseAction(Session& s, std::string id) : s_(s), id_(std::move(id)) {}
    Result apply() override {
        auto doc = s_.workspace().getAs<CodeDoc>(id_);
        if (!doc) return Result::failure("no open file called '" + id_ + "'");
        doc_ = s_.workspace().take(id_);
        return Result::success("closed " + id_ + (doc->modified() ? " (it had unsaved changes; undo brings them back)" : ""));
    }
    void revert() override {
        if (doc_) s_.workspace().put(id_, doc_);
    }

private:
    Session& s_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

Result opened(Session& s, std::shared_ptr<CodeDoc> doc, const std::string& verb) {
    std::string id = s.workspace().add(doc, "code");
    const Language* lang = languageById(doc->language);
    Result r = Result::success(verb + " " + id + " (" + doc->summary() + ")", id);
    return r.with("doc", id)
        .with("language", doc->language)
        .with("language_name", lang ? lang->name : doc->language)
        .with("lines", static_cast<int64_t>(lines(doc->text).size()))
        .with("path", doc->path)
        .with("eol", doc->eol == "\r\n" ? "crlf" : "lf");
}

std::regex makeRegex(const Params& p, const std::string& key = "find") {
    std::string pattern = stringParam(p, key);
    if (pattern.empty()) throw std::runtime_error(key + " is empty");
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

std::vector<ParamSpec> findParams() {
    return {{"find", ParamType::String, "text to look for (a regular expression with regex=true)", true},
            opt("regex", ParamType::Bool, "treat find as a regular expression (ECMAScript)", false),
            opt("match_case", ParamType::Bool, "upper and lower case must match", false),
            opt("whole_word", ParamType::Bool, "only whole words", false)};
}

// a line range: 1-based, inclusive; end 0 or past the end = the last line
void clampRange(const std::vector<std::string>& ls, int64_t& from, int64_t& to) {
    const int64_t n = static_cast<int64_t>(ls.size());
    if (from < 1) from = 1;
    if (to <= 0 || to > n) to = n;
    if (from > n) throw std::runtime_error("line " + std::to_string(from) + " is past the end (the file has " + std::to_string(n) + " lines)");
    if (to < from) throw std::runtime_error("end is before start");
}

std::string numbered(const std::vector<std::string>& ls, int64_t from, int64_t to) {
    std::string out;
    for (int64_t i = from; i <= to; ++i) out += std::to_string(i) + "\t" + ls[static_cast<std::size_t>(i - 1)] + "\n";
    return out;
}

Result changed(CodeDoc& doc, const std::string& what) {
    return Result::success(what).with("lines", static_cast<int64_t>(lines(doc.text).size())).with("modified", doc.modified());
}

void registerFileCommands(Session& s) {
    auto& reg = s.registry();
    reg.add({"code.open", "Open file",
             "Open a text or source file to read and edit. Returns the document id (code1). The language is detected from the "
             "name and a #! line; line ends (LF/CRLF) and a byte order mark are kept when saving. Folders: use code.project.",
             {{"path", ParamType::String, "the file", true},
              opt("language", ParamType::String, "language id to use instead of the detected one (code.languages lists them)")},
             false, true},
            reader([&s](const Params& p) {
                const fs::path path = sysutil::toPath(stringParam(p, "path"));
                std::error_code ec;
                if (fs::is_directory(path, ec)) throw std::runtime_error(stringParam(p, "path") + " is a folder; code.project opens folders");
                if (!fs::is_regular_file(path, ec)) throw std::runtime_error("file not found: " + stringParam(p, "path"));
                if (fs::file_size(path, ec) > (64u << 20)) throw std::runtime_error("that file is over 64 MB, too big to edit here");
                std::string bytes = sysutil::readFile(path);
                if (looksBinary(bytes)) throw std::runtime_error(stringParam(p, "path") + " is not a text file (explorer.preview shows it as hex)");
                auto doc = std::make_shared<CodeDoc>();
                loadText(*doc, bytes);
                doc->path = absText(path);
                std::string want = stringParam(p, "language");
                if (!want.empty() && !languageById(want)) throw std::runtime_error("unknown language '" + want + "' (code.languages lists them)");
                doc->language = want.empty() ? detectLanguage(sysutil::nameOf(path), doc->text.substr(0, 200)).id : want;
                return opened(s, doc, "opened");
            }));

    reg.add({"code.new", "New file",
             "Start a new, unsaved file (code.save with path= writes it). Give a name or language for highlighting.",
             {opt("language", ParamType::String, "language id, e.g. python, cpp, rust (default plain text)"),
              opt("name", ParamType::String, "a file name to guess the language from, e.g. main.go"),
              opt("text", ParamType::String, "starting text")},
             false, true},
            reader([&s](const Params& p) {
                auto doc = std::make_shared<CodeDoc>();
                doc->text = stringParam(p, "text");
                std::string want = stringParam(p, "language"), name = stringParam(p, "name");
                if (!want.empty() && !languageById(want)) throw std::runtime_error("unknown language '" + want + "'");
                doc->language = !want.empty() ? want : detectLanguage(name, doc->text.substr(0, 200)).id;
                doc->saved = "\x01";  // never saved: counts as modified
                return opened(s, doc, "new");
            }));

    reg.add({"code.save", "Save file",
             "Save an open file, to its own path or to path= (save as). Keeps its line ends and byte order mark unless "
             "eol= says otherwise.",
             {docParam(), opt("path", ParamType::String, "save to this file instead (it becomes the file's path)"),
              {"eol", ParamType::String, "line ends to write", false, {"keep", "lf", "crlf"}, std::string("keep")}},
             false},
            docReader(s, [](CodeDoc& doc, const Params& p) {
                std::string target = stringParam(p, "path", doc.path);
                if (target.empty()) throw std::runtime_error("this file has never been saved; give path=");
                std::string eol = stringParam(p, "eol", "keep");
                if (eol == "lf") doc.eol = "\n";
                if (eol == "crlf") doc.eol = "\r\n";
                const fs::path path = sysutil::toPath(target);
                std::error_code ec;
                if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
                const std::string bytes = saveText(doc);
                sysutil::writeFile(path, bytes);
                const bool renamed = doc.path != absText(path);
                doc.path = absText(path);
                doc.saved = doc.text;
                if (renamed && doc.language == "text") doc.language = detectLanguage(sysutil::nameOf(path), doc.text.substr(0, 200)).id;
                return Result::success("saved " + doc.path + " (" + sysutil::humanSize(bytes.size()) + ")")
                    .with("path", doc.path)
                    .with("bytes", static_cast<int64_t>(bytes.size()));
            }));

    reg.add({"code.close", "Close file", "Close an open file (undo reopens it, unsaved changes included).", {docParam()}, true},
            [&s](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<CloseAction>(s, stringParam(p, "doc")); });

    reg.add({"code.files", "Open files", "List the open files: id, path, language, lines, and whether they have unsaved changes.", {}, false, true},
            reader([&s](const Params&) {
                std::string text, rows;
                std::size_t n = 0;
                for (const auto& id : s.workspace().ids()) {
                    auto doc = s.workspace().getAs<CodeDoc>(id);
                    if (!doc) continue;
                    ++n;
                    text += id + ": " + doc->summary() + "\n";
                    rows += id + "\t" + doc->path + "\t" + doc->language + "\t" + (doc->modified() ? "modified" : "") + "\n";
                }
                if (!n) text = "no files open\n";
                return Result::success(text).with("count", static_cast<int64_t>(n)).with("rows", rows);
            }));

    reg.add({"code.read", "Read file",
             "Show an open file's text with line numbers, or a range of lines (start/end, 1-based). raw=true gives the text "
             "without numbers.",
             {docParam(), opt("start", ParamType::Int, "first line", int64_t{1}), opt("end", ParamType::Int, "last line (0 = the end)", int64_t{0}),
              opt("raw", ParamType::Bool, "no line numbers", false)},
             false, true},
            docReader(s, [](CodeDoc& doc, const Params& p) {
                auto ls = lines(doc.text);
                int64_t from = intParam(p, "start", 1), to = intParam(p, "end", 0);
                clampRange(ls, from, to);
                std::string body;
                if (boolParam(p, "raw", false)) {
                    body = join(std::vector<std::string>(ls.begin() + (from - 1), ls.begin() + to));
                } else {
                    body = numbered(ls, from, to);
                }
                return Result::success(body)
                    .with("text", from == 1 && to == static_cast<int64_t>(ls.size()) ? doc.text : join(std::vector<std::string>(ls.begin() + (from - 1), ls.begin() + to)))
                    .with("lines", static_cast<int64_t>(ls.size()))
                    .with("language", doc.language)
                    .with("modified", doc.modified())
                    .with("path", doc.path);
            }));

    reg.add({"code.set_language", "Set language", "Highlight and outline an open file as another language.",
             {docParam(), {"language", ParamType::String, "language id (code.languages lists them)", true}}, false, true},
            docReader(s, [](CodeDoc& doc, const Params& p) {
                std::string want = stringParam(p, "language");
                const Language* lang = languageById(want);
                if (!lang) throw std::runtime_error("unknown language '" + want + "' (code.languages lists them)");
                doc.language = lang->id;
                return Result::success("now " + lang->name).with("language", lang->id);
            }));
}

void registerTextCommands(Session& s) {
    auto& reg = s.registry();
    reg.add({"code.set_text", "Replace all text", "Replace the whole text of an open file (undoable). The GUI editor uses this.",
             {docParam(), {"text", ParamType::String, "the new text", true}}, true},
            docEditor(s, [](CodeDoc& doc, const Params& p) {
                std::string t = stringParam(p, "text");
                t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
                if (t == doc.text) return Result::success("no change").with("lines", static_cast<int64_t>(lines(t).size()));
                doc.text = std::move(t);
                return changed(doc, "text replaced");
            }));

    reg.add({"code.insert", "Insert lines",
             "Insert text before a line (1-based; 0 or past the end appends at the end).",
             {docParam(), {"text", ParamType::String, "text to insert (may hold several lines)", true},
              opt("line", ParamType::Int, "insert before this line (0 = at the end)", int64_t{0})},
             true},
            docEditor(s, [](CodeDoc& doc, const Params& p) {
                auto ls = lines(doc.text);
                std::string t = stringParam(p, "text");
                t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
                if (!t.empty() && t.back() == '\n') t.pop_back();
                auto add = lines(t);
                int64_t at = intParam(p, "line", 0);
                const bool emptyDoc = ls.size() == 1 && ls[0].empty();
                if (emptyDoc) ls.clear();
                if (at <= 0 || at > static_cast<int64_t>(ls.size())) at = static_cast<int64_t>(ls.size()) + 1;
                ls.insert(ls.begin() + (at - 1), add.begin(), add.end());
                doc.text = join(ls);
                return changed(doc, "inserted " + sysutil::plural(add.size(), "line") + " at line " + std::to_string(at));
            }));

    reg.add({"code.replace_lines", "Replace lines",
             "Replace lines start..end (1-based, inclusive) with new text; empty text deletes them.",
             {docParam(), {"start", ParamType::Int, "first line to replace", true}, opt("end", ParamType::Int, "last line (default = start)"),
              opt("text", ParamType::String, "the new lines (empty deletes)", std::string())},
             true},
            docEditor(s, [](CodeDoc& doc, const Params& p) {
                auto ls = lines(doc.text);
                int64_t from = intParam(p, "start", 1), to = intParam(p, "end", from);
                clampRange(ls, from, to);
                std::string t = stringParam(p, "text");
                t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
                if (!t.empty() && t.back() == '\n') t.pop_back();
                std::vector<std::string> add = p.count("text") && !t.empty() ? lines(t) : std::vector<std::string>{};
                ls.erase(ls.begin() + (from - 1), ls.begin() + to);
                ls.insert(ls.begin() + (from - 1), add.begin(), add.end());
                if (ls.empty()) ls.push_back("");
                doc.text = join(ls);
                return changed(doc, "replaced lines " + std::to_string(from) + "-" + std::to_string(to) + " with " + sysutil::plural(add.size(), "line"));
            }));

    reg.add({"code.edit", "Edit text",
             "Replace an exact piece of text with another (old must appear exactly once unless all=true). The safest way "
             "for a script to change code.",
             {docParam(), {"old", ParamType::String, "the exact text to replace", true}, {"new", ParamType::String, "what to put instead", true},
              opt("all", ParamType::Bool, "replace every occurrence", false)},
             true},
            docEditor(s, [](CodeDoc& doc, const Params& p) {
                std::string oldText = stringParam(p, "old"), newText = stringParam(p, "new");
                oldText.erase(std::remove(oldText.begin(), oldText.end(), '\r'), oldText.end());
                newText.erase(std::remove(newText.begin(), newText.end(), '\r'), newText.end());
                if (oldText.empty()) throw std::runtime_error("old is empty");
                std::size_t count = 0;
                for (std::size_t at = doc.text.find(oldText); at != std::string::npos; at = doc.text.find(oldText, at + oldText.size())) ++count;
                if (!count) throw std::runtime_error("old text not found");
                if (count > 1 && !boolParam(p, "all", false))
                    throw std::runtime_error("old text appears " + std::to_string(count) + " times; add more context or all=true");
                std::string out;
                std::size_t pos = 0;
                for (std::size_t at = doc.text.find(oldText); at != std::string::npos; at = doc.text.find(oldText, pos)) {
                    out += doc.text.substr(pos, at - pos) + newText;
                    pos = at + oldText.size();
                }
                out += doc.text.substr(pos);
                doc.text = std::move(out);
                return changed(doc, "replaced " + sysutil::plural(count, "place"));
            }));

    reg.add({"code.find", "Find", "Find text in an open file: every match with line, column and the line's text.",
             [] {
                 auto ps = findParams();
                 ps.insert(ps.begin(), docParam());
                 return ps;
             }(),
             false, true},
            docReader(s, [](CodeDoc& doc, const Params& p) {
                std::regex re = makeRegex(p);
                auto ls = lines(doc.text);
                std::string text, rows;
                std::size_t n = 0;
                for (std::size_t i = 0; i < ls.size(); ++i) {
                    for (std::sregex_iterator it(ls[i].begin(), ls[i].end(), re), end; it != end; ++it) {
                        if (it->length(0) == 0) continue;
                        ++n;
                        if (n <= 500) {
                            text += std::to_string(i + 1) + ":" + std::to_string(it->position(0) + 1) + ": " + ls[i] + "\n";
                            rows += std::to_string(i + 1) + "\t" + std::to_string(it->position(0)) + "\t" + std::to_string(it->length(0)) + "\n";
                        }
                    }
                }
                return Result::success(n ? sysutil::plural(n, "match") + (n > 500 ? " (first 500 shown)" : "") + "\n" + text : "no matches", static_cast<int64_t>(n))
                    .with("count", static_cast<int64_t>(n))
                    .with("rows", rows);
            }));

    reg.add({"code.replace", "Replace",
             "Replace matches in an open file (undoable). With regex=true the replacement can use $1, $2 for groups.",
             [] {
                 auto ps = findParams();
                 ps.insert(ps.begin(), docParam());
                 ps.push_back({"replace", ParamType::String, "the replacement", true});
                 ps.push_back(opt("line", ParamType::Int, "only on this line (0 = everywhere)", int64_t{0}));
                 return ps;
             }(),
             true},
            docEditor(s, [](CodeDoc& doc, const Params& p) {
                std::regex re = makeRegex(p);
                std::string with = stringParam(p, "replace");
                if (!boolParam(p, "regex", false)) {
                    std::string esc;
                    for (char c : with) esc += c == '$' ? std::string("$$") : std::string(1, c);
                    with = esc;
                }
                const int64_t only = intParam(p, "line", 0);
                auto ls = lines(doc.text);
                std::size_t n = 0;
                for (std::size_t i = 0; i < ls.size(); ++i) {
                    if (only > 0 && static_cast<int64_t>(i + 1) != only) continue;
                    auto count = std::distance(std::sregex_iterator(ls[i].begin(), ls[i].end(), re), std::sregex_iterator());
                    if (!count) continue;
                    n += static_cast<std::size_t>(count);
                    ls[i] = std::regex_replace(ls[i], re, with);
                }
                if (!n) throw std::runtime_error("no matches");
                doc.text = join(ls);
                return changed(doc, "replaced " + sysutil::plural(n, "match")).with("count", static_cast<int64_t>(n));
            }));
}

void registerViewCommands(Session& s) {
    auto& reg = s.registry();
    reg.add({"code.goto", "Go to line", "Show the lines around a line of an open file (and move the editor there).",
             {docParam(), {"line", ParamType::Int, "line number (1-based)", true}, opt("context", ParamType::Int, "lines shown before and after", int64_t{5})},
             false, true},
            docReader(s, [](CodeDoc& doc, const Params& p) {
                auto ls = lines(doc.text);
                int64_t line = std::clamp<int64_t>(intParam(p, "line", 1), 1, static_cast<int64_t>(ls.size()));
                int64_t ctx = std::max<int64_t>(0, intParam(p, "context", 5));
                int64_t from = std::max<int64_t>(1, line - ctx), to = std::min<int64_t>(static_cast<int64_t>(ls.size()), line + ctx);
                doc.cursorLine = static_cast<int>(line);
                return Result::success(numbered(ls, from, to)).with("line", line).with("text", ls[static_cast<std::size_t>(line - 1)]);
            }));

    reg.add({"code.outline", "Outline",
             "The functions, classes, types, headings and so on in an open file, with their line numbers.",
             {docParam()}, false, true},
            docReader(s, [](CodeDoc& doc, const Params&) {
                const Language* lang = languageById(doc.language);
                auto syms = outline(doc.text, lang ? *lang : plainText());
                std::string text, rows;
                for (const auto& sy : syms) {
                    text += std::string(static_cast<std::size_t>(sy.depth) * 2, ' ') + sy.kind + " " + sy.name + "  (line " + std::to_string(sy.line) + ")\n";
                    rows += std::to_string(sy.line) + "\t" + sy.kind + "\t" + std::to_string(sy.depth) + "\t" + sy.name + "\n";
                }
                if (syms.empty()) text = "nothing to outline\n";
                return Result::success(text).with("count", static_cast<int64_t>(syms.size())).with("rows", rows);
            }));

    reg.add({"code.highlight", "Highlight",
             "Syntax colouring of an open file (doc=) or of text= in a language: as spans (line, column, length, kind), as an "
             "HTML page, or with ANSI colours for a terminal.",
             {opt("doc", ParamType::String, "an open file"), opt("text", ParamType::String, "or this text"),
              opt("language", ParamType::String, "language id for text= (or a file name to guess from)"),
              {"format", ParamType::String, "what to return", false, {"spans", "html", "ansi"}, std::string("spans")},
              opt("output", ParamType::String, "write the HTML or ANSI text to this file")},
             false},
            [&s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&s, p] {
                    return guard([&] {
                        std::string text = stringParam(p, "text"), langId = stringParam(p, "language");
                        if (!stringParam(p, "doc").empty()) {
                            auto doc = needDoc(s, stringParam(p, "doc"));
                            text = doc->text;
                            if (langId.empty()) langId = doc->language;
                        }
                        const Language* lang = languageById(langId);
                        if (!lang) lang = &detectLanguage(langId, text.substr(0, 200));
                        const std::string format = stringParam(p, "format", "spans");
                        if (format == "spans") {
                            auto spans = highlight(text, *lang);
                            NumberList flat;
                            flat.reserve(spans.size() * 4);
                            std::string rows;
                            for (const auto& sp : spans) {
                                flat.insert(flat.end(), {double(sp.line), double(sp.col), double(sp.len), double(static_cast<int>(sp.kind))});
                                if (rows.size() < 20000) rows += std::to_string(sp.line + 1) + ":" + std::to_string(sp.col + 1) + " " + std::to_string(sp.len) + " " + tokenName(sp.kind) + "\n";
                            }
                            return Result::success(sysutil::plural(spans.size(), "span") + " (" + lang->name + ")\n" + rows)
                                .with("spans", flat)
                                .with("language", lang->id);
                        }
                        std::string out = format == "html" ? highlightHtml(text, *lang) : highlightAnsi(text, *lang);
                        std::string file = stringParam(p, "output");
                        if (!file.empty()) {
                            sysutil::writeFile(sysutil::toPath(file), out);
                            return Result::success("wrote " + file).with("path", file);
                        }
                        return Result::success(out).with("language", lang->id);
                    });
                });
            });

    reg.add({"code.languages", "Languages", "Every language the editor knows: id, name, file extensions and whether it can be run here.", {}, false, true},
            reader([](const Params&) {
                std::string text, rows;
                for (const auto& l : languages()) {
                    std::string exts;
                    for (const auto& e : l.extensions) exts += (exts.empty() ? "" : " ") + e;
                    for (const auto& f : l.filenames) exts += (exts.empty() ? "" : " ") + f;
                    text += l.id + " - " + l.name + (exts.empty() ? "" : " (" + exts + ")") + "\n";
                    rows += l.id + "\t" + l.name + "\t" + exts + "\n";
                }
                return Result::success(text).with("count", static_cast<int64_t>(languages().size())).with("rows", rows);
            }));
}

}  // namespace

void registerEditCommands(Session& s, StatePtr) {
    registerFileCommands(s);
    registerTextCommands(s);
    registerViewCommands(s);
}

}  // namespace wetwaresuite::code::detail
