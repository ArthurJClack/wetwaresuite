// Shared command machinery: the vault document, file transactions, params,
// templates, link rewriting and daily notes.
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::notes {

void registerCommands(Session& session) {
    auto st = std::make_shared<detail::State>();
    st->session = &session;
    auto& reg = session.registry();
    detail::registerVaultCommands(reg, st);
    detail::registerNoteCommands(reg, st);
    detail::registerMetaCommands(reg, st);
    detail::registerTaskCommands(reg, st);
    detail::registerLinkCommands(reg, st);
    detail::registerExportCommands(reg, st);
}

namespace detail {

// ---- the document ----------------------------------------------------------------------

std::string VaultDoc::summary() const {
    std::size_t tasks = 0;
    for (const auto& [rel, n] : vault.notes) tasks += n.tasks.size();
    return fromPath(vault.root) + ", " + plural(vault.notes.size(), "note") + ", " + plural(vault.files.size(), "attachment") +
           ", " + plural(tasks, "task");
}

std::unique_ptr<Attachment> VaultDoc::preview() const {
    GraphOptions o;
    Graph g = buildGraph(vault, o);
    layoutGraph(g, g.nodes.size() > 1500 ? 40 : 0);
    auto img = drawGraph(g, 768, 576, -1);
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(img), "graph.png"});
}

// ---- file transactions ----------------------------------------------------------------

namespace {

void writeBytes(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("can't write " + fromPath(p));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("can't write " + fromPath(p));
}

}  // namespace

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + fromPath(p));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void FileTxn::ensureParent(const fs::path& p) {
    std::vector<fs::path> made;
    std::error_code ec;
    for (fs::path d = p.parent_path(); !d.empty() && !fs::exists(d, ec); d = d.parent_path()) {
        made.push_back(d);
        if (d == d.parent_path()) break;
    }
    fs::create_directories(p.parent_path(), ec);
    if (ec) throw std::runtime_error("can't make folder " + fromPath(p.parent_path()) + ": " + ec.message());
    for (auto it = made.rbegin(); it != made.rend(); ++it) dirs_.push_back(*it);
}

void FileTxn::write(const fs::path& path, const std::string& bytes) {
    Op op;
    op.kind = Op::Write;
    op.a = path;
    std::error_code ec;
    op.existed = fs::exists(path, ec);
    if (op.existed) {
        op.bytes = readText(path);
        op.time = fs::last_write_time(path, ec);
    }
    ensureParent(path);
    writeBytes(path, bytes);
    ops_.push_back(std::move(op));
}

void FileTxn::remove(const fs::path& path) {
    Op op;
    op.kind = Op::Remove;
    op.a = path;
    op.existed = true;
    op.bytes = readText(path);
    std::error_code ec;
    op.time = fs::last_write_time(path, ec);
    fs::remove(path, ec);
    if (ec) throw std::runtime_error("can't delete " + fromPath(path) + ": " + ec.message());
    ops_.push_back(std::move(op));
}

void FileTxn::move(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::exists(to, ec)) throw std::runtime_error(fromPath(to) + " already exists");
    ensureParent(to);
    fs::rename(from, to, ec);
    if (ec) {
        // another drive: copy, then delete
        fs::copy_file(from, to, ec);
        if (ec) throw std::runtime_error("can't move " + fromPath(from) + ": " + ec.message());
        fs::last_write_time(to, fs::last_write_time(from, ec), ec);
        fs::remove(from, ec);
    }
    Op op;
    op.kind = Op::Move;
    op.a = from;
    op.b = to;
    ops_.push_back(std::move(op));
}

void FileTxn::restore() {
    std::error_code ec;
    for (auto it = ops_.rbegin(); it != ops_.rend(); ++it) {
        try {
            switch (it->kind) {
                case Op::Write:
                    if (it->existed) {
                        fs::create_directories(it->a.parent_path(), ec);
                        writeBytes(it->a, it->bytes);
                        fs::last_write_time(it->a, it->time, ec);
                    } else {
                        fs::remove(it->a, ec);
                    }
                    break;
                case Op::Remove:
                    fs::create_directories(it->a.parent_path(), ec);
                    writeBytes(it->a, it->bytes);
                    fs::last_write_time(it->a, it->time, ec);
                    break;
                case Op::Move:
                    fs::create_directories(it->a.parent_path(), ec);
                    fs::rename(it->b, it->a, ec);
                    if (ec) {
                        fs::copy_file(it->b, it->a, ec);
                        fs::remove(it->b, ec);
                    }
                    break;
            }
        } catch (const std::exception&) {
        }
    }
    for (auto it = dirs_.rbegin(); it != dirs_.rend(); ++it) {
        if (fs::is_directory(*it, ec) && fs::is_empty(*it, ec)) fs::remove(*it, ec);
    }
    ops_.clear();
    dirs_.clear();
}

// ---- actions --------------------------------------------------------------------------

std::shared_ptr<VaultDoc> getVault(State& st, const Params& p, std::string& error, std::string* idOut) {
    Workspace& ws = st.session->workspace();
    const std::string v = trim(stringParam(p, "vault"));
    std::shared_ptr<VaultDoc> doc;
    std::string id;
    auto openFolder = [&](const fs::path& folder) -> bool {
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(fs::absolute(folder), ec);
        for (const auto& existing : ws.ids()) {
            auto d = ws.getAs<VaultDoc>(existing);
            if (d && d->vault.root == canon) {
                doc = d;
                id = existing;
                return true;
            }
        }
        auto fresh = std::make_shared<VaultDoc>();
        try {
            fresh->vault.open(folder);
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
        fresh->path = fromPath(fresh->vault.root);
        id = ws.add(fresh, "vault");
        doc = fresh;
        return true;
    };
    if (v.empty()) {
        if (!st.current.empty()) {
            doc = ws.getAs<VaultDoc>(st.current);
            if (doc) id = st.current;
        }
        if (!doc) {
            for (const auto& existing : ws.ids()) {
                if (auto d = ws.getAs<VaultDoc>(existing)) {
                    doc = d;
                    id = existing;
                }
            }
        }
        if (!doc) {
            std::error_code ec;
            for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
                if (fs::is_directory(dir / ".obsidian", ec) || fs::is_directory(dir / kCacheDir, ec) || fs::is_directory(dir / kOldCacheDir, ec)) {
                    if (!openFolder(dir)) return nullptr;
                    break;
                }
                if (dir == dir.parent_path()) break;
            }
        }
        if (!doc) {
            error = "no vault open: run notes.vault.open path=<folder>, or pass vault=<folder>";
            return nullptr;
        }
    } else if (auto d = ws.getAs<VaultDoc>(v)) {
        doc = d;
        id = v;
    } else {
        std::error_code ec;
        if (!fs::is_directory(toPath(v), ec)) {
            error = "no vault '" + v + "': give an id from notes.vault.open (like vault1) or a folder";
            return nullptr;
        }
        if (!openFolder(toPath(v))) return nullptr;
    }
    try {
        doc->vault.refresh();
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    st.current = id;
    if (idOut) *idOut = id;
    return doc;
}

namespace {

class VaultEdit : public Action {
public:
    VaultEdit(StatePtr st, Params p, EditFn fn) : st_(std::move(st)), p_(std::move(p)), fn_(std::move(fn)) {}

    Result apply() override {
        std::string error;
        if (!doc_) {
            doc_ = getVault(*st_, p_, error);
            if (!doc_) return Result::failure(error);
        } else {
            try {
                doc_->vault.refresh();
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        }
        txn_ = FileTxn{};
        Result r;
        try {
            r = fn_(doc_->vault, txn_, p_);
        } catch (const std::exception& e) {
            r = Result::failure(e.what());
        }
        if (!r.ok) txn_.restore();
        try {
            doc_->vault.refresh();
        } catch (const std::exception&) {
        }
        return r;
    }

    void revert() override {
        txn_.restore();
        if (doc_) {
            try {
                doc_->vault.refresh();
            } catch (const std::exception&) {
            }
        }
    }

private:
    StatePtr st_;
    Params p_;
    EditFn fn_;
    std::shared_ptr<VaultDoc> doc_;
    FileTxn txn_;
};

}  // namespace

ActionFactory reader(StatePtr st, ReadFn fn) {
    return [st, fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([st, fn, p]() -> Result {
            std::string error;
            auto doc = getVault(*st, p, error);
            if (!doc) return Result::failure(error);
            try {
                return fn(doc->vault, p);
            } catch (const ParamError& e) {
                return Result::failure(e.what());
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        });
    };
}

ActionFactory editor(StatePtr st, EditFn fn) {
    return [st, fn](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<VaultEdit>(st, p, fn); };
}

// ---- params ----------------------------------------------------------------------------

ParamSpec vaultParam() {
    return {"vault", ParamType::String,
            "the vault: an id from notes.vault.open (like vault1) or a folder (opened on the spot); default: the vault used "
            "last, else a folder holding .obsidian or .wwnotes here or above",
            false};
}
ParamSpec noteParam(const std::string& what, bool required) { return {"note", ParamType::String, what, required}; }
ParamSpec boolParamSpec(const std::string& name, const std::string& what, bool fallback) {
    return {name, ParamType::Bool, what, false, {}, fallback};
}
ParamSpec jsonParam(const std::string& what) {
    return {"json", ParamType::Bool, "also return " + what + " as JSON text in `items`, for scripts", false, {}, false};
}
ParamSpec limitParam(std::int64_t fallback) {
    return {"limit", ParamType::Int, "most results to return (0 = all)", false, {}, fallback};
}
ParamSpec dateParam(const std::string& what) {
    return {"date", ParamType::String, what + ": YYYY-MM-DD, today, yesterday, tomorrow, or +N / -N days", false, {}, std::string("today")};
}
std::vector<ParamSpec> textParams(const std::string& name, const std::string& what, bool required) {
    return {{name, ParamType::String, what + " (\\n and \\t mean a line break and a tab when the text has no real ones)", false},
            {name + "_file", ParamType::String, "read " + name + " from this file instead" + (required ? std::string() : ""), false}};
}

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

bool hasText(const Params& p, const std::string& name) { return has(p, name) || has(p, name + "_file"); }

std::string textParam(const Params& p, const std::string& name) {
    if (!has(p, name) && has(p, name + "_file")) return readText(toPath(stringParam(p, name + "_file")));
    const std::string s = stringParam(p, name);
    if (s.find('\n') != std::string::npos) return s;
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == 't')) {
            out += s[i + 1] == 'n' ? '\n' : '\t';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string needNote(const Vault& v, const Params& p, const std::string& key, bool notesOnly) {
    const std::string spec = stringParam(p, key);
    if (trim(spec).empty()) throw std::runtime_error("say which note: " + key + "=<path or name>");
    std::string rel = v.findNote(spec);
    if (rel.empty() || (notesOnly && !v.isNote(rel))) {
        std::string msg = "no note called '" + spec + "' in the vault";
        // suggest close names
        std::vector<std::string> near;
        const std::string ls = lower(spec);
        for (const auto& [r, n] : v.notes) {
            if (lower(n.name).find(ls) != std::string::npos || ls.find(lower(n.name)) != std::string::npos) near.push_back(r);
            if (near.size() >= 5) break;
        }
        if (!near.empty()) {
            msg += " (did you mean ";
            for (std::size_t i = 0; i < near.size(); ++i) msg += (i ? ", " : "") + near[i];
            msg += "?)";
        }
        throw std::runtime_error(msg);
    }
    return rel;
}

Date baseDate(const Params& p) {
    Date n = now();
    std::string t = stringParam(p, "today");
    if (!t.empty()) {
        if (auto d = parseDate(t, n)) {
            Date out = *d;
            out.hour = n.hour;
            out.minute = n.minute;
            out.second = n.second;
            if (t.size() >= 16) {
                out.hour = d->hour;
                out.minute = d->minute;
                out.second = 0;
            }
            return out;
        }
    }
    return n;
}

// ---- text --------------------------------------------------------------------------------

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto nl = text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        std::string l = text.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        start = nl + 1;
    }
    return lines;
}

std::string withoutMd(const std::string& rel) { return endsWith(lower(rel), ".md") ? rel.substr(0, rel.size() - 3) : rel; }
std::string folderOf(const std::string& rel) {
    auto slash = rel.rfind('/');
    return slash == std::string::npos ? "" : rel.substr(0, slash);
}
std::string fileNameOf(const std::string& rel) {
    auto slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}
std::string joinRel(const std::string& folder, const std::string& name) { return folder.empty() ? name : folder + "/" + name; }
std::string plural(std::size_t n, const std::string& word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); }

std::string newNoteRel(const std::string& rawSpec, const std::string& rawFolder) {
    auto clean = [](std::string s) {
        for (auto& c : s) {
            if (c == '\\') c = '/';
        }
        std::string out;
        std::string part;
        auto flush = [&] {
            std::string cleanPart;
            for (char c : part) {
                if (std::string("*\"<>:|?#^[]").find(c) != std::string::npos) c = ' ';
                if (c == ' ' && (cleanPart.empty() || cleanPart.back() == ' ')) continue;
                cleanPart += c;
            }
            cleanPart = trim(cleanPart);
            while (!cleanPart.empty() && cleanPart.back() == '.') cleanPart.pop_back();
            if (cleanPart == "..") throw std::runtime_error("note paths can't leave the vault");
            if (!cleanPart.empty()) out += (out.empty() ? "" : "/") + cleanPart;
            part.clear();
        };
        for (char c : s) {
            if (c == '/') flush();
            else part += c;
        }
        flush();
        return out;
    };
    std::string spec = trim(rawSpec);
    bool md = endsWith(lower(spec), ".md");
    if (md) spec = spec.substr(0, spec.size() - 3);
    std::string rel = clean(spec);
    std::string folder = clean(rawFolder);
    if (rel.empty()) throw std::runtime_error("the note needs a name");
    if (!folder.empty() && rel.find('/') == std::string::npos) rel = folder + "/" + rel;
    if (!rel.empty() && rel[0] == '.') throw std::runtime_error("note names can't start with a dot (hidden files are skipped)");
    return rel + ".md";
}

namespace {
std::string appendTextLf(const std::string& content, const std::string& rawText, const std::string& heading, bool prepend);
}

// Keeps a CRLF note CRLF (Windows editors), whatever line breaks the added text has.
std::string appendText(const std::string& content, const std::string& rawText, const std::string& heading, bool prepend) {
    if (content.find("\r\n") == std::string::npos) return appendTextLf(content, rawText, heading, prepend);
    auto lf = [](const std::string& t) {
        std::string o;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '\r' && i + 1 < t.size() && t[i + 1] == '\n') continue;
            o += t[i];
        }
        return o;
    };
    std::string out = appendTextLf(lf(content), lf(rawText), heading, prepend);
    std::string crlf;
    for (char c : out) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    return crlf;
}

namespace {
std::string appendTextLf(const std::string& content, const std::string& rawText, const std::string& heading, bool prepend) {
    std::string text = rawText;
    while (!text.empty() && text.back() == '\n') text.pop_back();
    if (!heading.empty()) {
        auto range = sectionRange(content, heading);
        if (!range) {
            std::string out = content;
            while (!out.empty() && out.back() == '\n') out.pop_back();
            std::string h = trim(heading);
            if (h.empty() || h[0] != '#') h = "## " + h;
            return out + (out.empty() ? "" : "\n\n") + h + "\n\n" + text + "\n";
        }
        auto [begin, end] = *range;
        if (prepend) {
            std::size_t afterHeading = content.find('\n', begin);
            afterHeading = afterHeading == std::string::npos ? content.size() : afterHeading + 1;
            std::string before = content.substr(0, afterHeading);
            if (!endsWith(before, "\n")) before += "\n";
            return before + text + "\n" + content.substr(afterHeading);
        }
        // before the section's trailing blank lines
        std::size_t cut = end;
        while (cut > begin && (content[cut - 1] == '\n' || content[cut - 1] == '\r' || content[cut - 1] == ' ')) --cut;
        std::string head = content.substr(0, cut);
        std::string tail = content.substr(cut);
        if (tail.empty()) tail = "\n";
        else if (tail.find("\n\n") == std::string::npos && end < content.size()) tail = "\n\n";
        // keep lists tight: a list item goes right under the last line
        bool listy = !text.empty() && (startsWith(trim(text), "- ") || startsWith(trim(text), "* ") || startsWith(trim(text), "1. "));
        std::string lastLine = head.substr(head.rfind('\n') == std::string::npos ? 0 : head.rfind('\n') + 1);
        bool lastListy = startsWith(trim(lastLine), "- ") || startsWith(trim(lastLine), "* ") ||
                         (!trim(lastLine).empty() && std::isdigit(static_cast<unsigned char>(trim(lastLine)[0])));
        bool headingOnly = head.find('\n', begin) == std::string::npos;
        std::string sep = (listy && lastListy) ? "\n" : headingOnly ? "\n\n" : "\n\n";
        return head + sep + text + tail;
    }
    if (prepend) {
        std::size_t bodyStart = 0;
        if (splitFrontmatter(content, nullptr, &bodyStart)) {
            return content.substr(0, bodyStart) + text + "\n" + (bodyStart < content.size() ? "\n" : "") + content.substr(bodyStart);
        }
        return text + "\n" + (content.empty() ? "" : "\n") + content;
    }
    std::string out = content;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (out.empty()) return text + "\n";
    std::string lastLine = out.substr(out.rfind('\n') == std::string::npos ? 0 : out.rfind('\n') + 1);
    bool listy = startsWith(trim(text), "- ") || startsWith(trim(text), "* ");
    bool lastListy = startsWith(trim(lastLine), "- ") || startsWith(trim(lastLine), "* ");
    if (out == "---" || endsWith(out, "\n---")) lastListy = false;
    return out + (listy && lastListy ? "\n" : "\n\n") + text + "\n";
}

}  // namespace

// ---- templates -------------------------------------------------------------------------

std::string findTemplate(const Vault& v, const std::string& rawName) {
    std::string name = trim(rawName);
    if (name.empty()) return "";
    std::string md = endsWith(lower(name), ".md") ? name : name + ".md";
    const std::string folder = v.settings.templatesFolder;
    if (!folder.empty() && v.isNote(joinRel(folder, md))) return joinRel(folder, md);
    if (v.isNote(md)) return md;
    // a template by name inside the templates folder
    for (const auto& [rel, n] : v.notes) {
        if (!folder.empty() && startsWith(rel, folder + "/") && lower(n.name) == lower(withoutMd(fileNameOf(md)))) return rel;
    }
    std::string rel = v.findNote(name);
    return v.isNote(rel) ? rel : "";
}

std::map<std::string, std::string> parseVars(const std::string& text) {
    std::map<std::string, std::string> out;
    std::string cur;
    auto flush = [&] {
        auto eq = cur.find('=');
        if (eq != std::string::npos) {
            std::string k = trim(cur.substr(0, eq));
            if (!k.empty()) out[k] = trim(cur.substr(eq + 1));
        }
        cur.clear();
    };
    for (char c : text) {
        if (c == ';' || c == '\n') flush();
        else cur += c;
    }
    flush();
    return out;
}

std::string expandTemplate(const std::string& text, const Vault& v, const std::string& title, const Date& date,
                           const std::map<std::string, std::string>& vars, const std::string& folder) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        auto open = text.find("{{", i);
        if (open == std::string::npos) {
            out += text.substr(i);
            break;
        }
        out += text.substr(i, open - i);
        auto close = text.find("}}", open + 2);
        if (close == std::string::npos) {
            out += text.substr(open);
            break;
        }
        std::string expr = trim(text.substr(open + 2, close - open - 2));
        std::string key = expr, fmt;
        if (auto colon = expr.find(':'); colon != std::string::npos) {
            key = trim(expr.substr(0, colon));
            fmt = trim(expr.substr(colon + 1));
        }
        const std::string lk = lower(key);
        std::string value;
        bool known = true;
        if (auto it = vars.find(key); it != vars.end()) value = it->second;
        else if (lk == "title") value = title;
        else if (lk == "date") value = formatDate(date, fmt.empty() ? v.settings.dateFormat : fmt);
        else if (lk == "time") value = formatDate(date, fmt.empty() ? v.settings.timeFormat : fmt);
        else if (lk == "yesterday") value = formatDate(addDays(date, -1), fmt.empty() ? v.settings.dateFormat : fmt);
        else if (lk == "tomorrow") value = formatDate(addDays(date, 1), fmt.empty() ? v.settings.dateFormat : fmt);
        else if (lk == "folder") value = folder;
        else known = false;
        out += known ? value : text.substr(open, close + 2 - open);
        i = close + 2;
    }
    return out;
}

std::string mergeTemplate(const std::string& note, const std::string& expanded) {
    std::string tyaml;
    std::size_t tbody = 0;
    std::string result = note;
    std::string body = expanded;
    if (splitFrontmatter(expanded, &tyaml, &tbody)) {
        body = expanded.substr(tbody);
        Frontmatter tf = parseFrontmatter(tyaml);
        std::string nyaml;
        Frontmatter nf;
        if (splitFrontmatter(note, &nyaml, nullptr)) nf = parseFrontmatter(nyaml);
        for (const auto& [k, v] : tf) {
            if (!fmFind(nf, k)) result = setFrontmatter(result, k, &v);
        }
    }
    if (trim(body).empty()) return result;
    std::string trimmedNote = bodyOf(result);
    if (trim(trimmedNote).empty()) {
        std::size_t bs = 0;
        std::string head = splitFrontmatter(result, nullptr, &bs) ? result.substr(0, bs) : "";
        while (!body.empty() && body[0] == '\n') body.erase(0, 1);
        return head + body;
    }
    return appendText(result, body, "", false);
}

// ---- links ------------------------------------------------------------------------------

namespace {

std::size_t countName(const Vault& v, const std::string& lname, const std::string& exceptRel, bool note) {
    std::size_t n = 0;
    for (const auto& [rel, nn] : v.notes) {
        if (rel == exceptRel) continue;
        if (note ? lower(nn.name) == lname : lower(fileNameOf(rel)) == lname) ++n;
    }
    if (!note) {
        for (const auto& [rel, f] : v.files) {
            if (rel != exceptRel && lower(fileNameOf(rel)) == lname) ++n;
        }
    }
    return n;
}

std::string relativeHref(const std::string& fromFolder, const std::string& toRel) {
    fs::path r = toPath(toRel).lexically_relative(toPath(fromFolder.empty() ? "." : fromFolder));
    std::string s = fromPath(r);
    return urlEncodePath(s);
}

}  // namespace

std::size_t moveWithLinks(Vault& v, FileTxn& txn, const std::string& from, const std::string& to, bool updateLinks,
                          std::vector<std::string>& changed) {
    if (from == to) throw std::runtime_error("it is already called that");
    std::error_code ec;
    if (fs::exists(v.abs(to), ec) && lower(from) != lower(to)) throw std::runtime_error(to + " already exists");
    const bool isNote = v.isNote(from);
    const std::string newName = isNote ? withoutMd(fileNameOf(to)) : fileNameOf(to);
    const bool unique = countName(v, lower(newName), from, isNote) == 0;
    std::size_t rewritten = 0;
    std::string movedContent;
    bool movedChanged = false;
    if (updateLinks) {
        for (const auto& [rel, n] : v.notes) {
            const bool self = rel == from;
            bool mentions = self;
            for (const auto& l : n.links) mentions = mentions || v.resolve(l, rel) == from;
            if (!mentions) continue;
            std::string content = v.read(rel);
            Parsed p = parseNote(content);
            const std::string folder = self ? folderOf(to) : folderOf(rel);
            struct Edit {
                std::size_t begin, end;
                std::string text;
            };
            std::vector<Edit> edits;
            for (const auto& l : p.links) {
                std::string target = v.resolve(l, rel);
                if (target.empty()) continue;
                if (target == from) {
                    std::string t;
                    if (l.wiki) {
                        const bool hadExt = endsWith(lower(l.target), ".md");
                        const bool hadPath = l.target.find('/') != std::string::npos;
                        if (hadPath || !unique) t = isNote ? (hadExt ? to : withoutMd(to)) : to;
                        else t = isNote ? (hadExt ? newName + ".md" : newName) : newName;
                        if (l.target.empty()) continue;  // [[#heading]] stays a self link
                        std::string text = std::string(l.embed ? "!" : "") + "[[" + t;
                        if (!l.heading.empty()) text += "#" + l.heading;
                        if (!l.alias.empty() || l.escapedPipe) text += std::string(l.escapedPipe ? "\\|" : "|") + l.alias;
                        text += "]]";
                        edits.push_back({l.begin, l.end, text});
                    } else {
                        std::string href = relativeHref(folder, to);
                        if (!l.heading.empty()) href += "#" + urlEncodePath(l.heading);
                        edits.push_back({l.begin, l.end, std::string(l.embed ? "!" : "") + "[" + l.alias + "](" + href + ")"});
                    }
                } else if (self && !l.wiki && folderOf(from) != folderOf(to)) {
                    std::string href = relativeHref(folder, target);
                    if (!l.heading.empty()) href += "#" + urlEncodePath(l.heading);
                    edits.push_back({l.begin, l.end, std::string(l.embed ? "!" : "") + "[" + l.alias + "](" + href + ")"});
                } else {
                    continue;
                }
            }
            if (edits.empty()) continue;
            std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin > b.begin; });
            std::string out = content;
            std::size_t real = 0;
            for (const auto& e : edits) {
                if (out.compare(e.begin, e.end - e.begin, e.text) == 0) continue;
                out.replace(e.begin, e.end - e.begin, e.text);
                ++real;
            }
            if (real == 0) continue;
            rewritten += real;
            if (self) {
                movedContent = out;
                movedChanged = true;
            } else {
                txn.write(v.abs(rel), out);
                changed.push_back(rel);
            }
        }
    }
    if (lower(from) == lower(to)) {
        // case-only rename: go through a temporary name so case-insensitive file systems see it
        fs::path tmp = v.abs(from + ".wwnotes-tmp");
        txn.move(v.abs(from), tmp);
        txn.move(tmp, v.abs(to));
    } else {
        txn.move(v.abs(from), v.abs(to));
    }
    if (movedChanged) {
        txn.write(v.abs(to), movedContent);
        changed.push_back(to);
    }
    return rewritten;
}

std::size_t renameTag(Vault& v, FileTxn& txn, const std::string& rawFrom, const std::string& rawTo) {
    auto clean = [](std::string t) {
        t = trim(t);
        while (!t.empty() && t[0] == '#') t.erase(0, 1);
        return t;
    };
    const std::string from = clean(rawFrom), to = clean(rawTo);
    if (from.empty() || to.empty()) throw std::runtime_error("give from= and to= tags");
    for (unsigned char c : to) {
        if (!isTagChar(c)) throw std::runtime_error("tags can't hold spaces or punctuation other than - _ /: '" + to + "'");
    }
    const std::string lf = lower(from);
    auto renamed = [&](const std::string& tag, std::string& out) {
        std::string lt = lower(tag);
        if (lt == lf) {
            out = to;
            return true;
        }
        if (startsWith(lt, lf + "/")) {
            out = to + tag.substr(from.size());
            return true;
        }
        return false;
    };
    std::size_t notesChanged = 0;
    for (const auto& [rel, n] : v.notes) {
        if (!n.hasTag(from)) continue;
        std::string content = v.read(rel);
        Parsed p = parseNote(content);
        std::string out = content;
        for (auto it = p.inlineTags.rbegin(); it != p.inlineTags.rend(); ++it) {
            std::string nt;
            if (renamed(it->tag, nt)) out.replace(it->begin, it->end - it->begin, "#" + nt);
        }
        for (const char* key : {"tags", "tag"}) {
            const FmValue* fv = fmFind(p.frontmatter, key);
            if (!fv) continue;
            FmValue nv;
            nv.isList = true;
            bool any = false;
            for (const auto& t : frontmatterTags({{key, *fv}})) {
                std::string nt;
                if (renamed(t, nt)) {
                    any = true;
                    nv.list.push_back(nt);
                } else {
                    nv.list.push_back(t);
                }
            }
            if (any) out = setFrontmatter(out, key, &nv);
        }
        if (out != content) {
            txn.write(v.abs(rel), out);
            ++notesChanged;
        }
    }
    return notesChanged;
}

// ---- daily notes ---------------------------------------------------------------------------

std::string dailyRel(const Vault& v, const Date& d, const std::string& folder, const std::string& format) {
    std::string f = folder.empty() ? v.settings.dailyFolder : folder;
    std::string name = formatDate(d, format.empty() ? v.settings.dailyFormat : format);
    return newNoteRel(joinRel(f, name));
}

std::string ensureDaily(Vault& v, FileTxn& txn, const Date& d, const Params& p, bool& created) {
    std::string rel = dailyRel(v, d, stringParam(p, "folder"), stringParam(p, "format"));
    created = false;
    std::error_code ec;
    if (fs::exists(v.abs(rel), ec)) return rel;
    std::string tmplName = has(p, "template") ? stringParam(p, "template") : v.settings.dailyTemplate;
    std::string content;
    if (!trim(tmplName).empty()) {
        std::string t = findTemplate(v, tmplName);
        if (t.empty()) throw std::runtime_error("no template called '" + tmplName + "' (templates live in " + v.settings.templatesFolder + "/)");
        content = expandTemplate(v.read(t), v, withoutMd(fileNameOf(rel)), d, parseVars(stringParam(p, "vars")), folderOf(rel));
    }
    txn.write(v.abs(rel), content);
    created = true;
    return rel;
}

}  // namespace detail
}  // namespace wetwaresuite::notes
