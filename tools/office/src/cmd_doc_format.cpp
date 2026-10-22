// office.doc_format, doc_style_define, doc_styles and doc_list: character and
// paragraph formatting, named styles and list numbering, plus the run helpers the
// other doc_* command sources share.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "doc_cmd.hpp"
#include "text.hpp"

namespace wetwaresuite::office::doccmd {

// ---- shared helpers ------------------------------------------------------------------------

std::vector<ParamSpec> targetSpecs() {
    return {integer("item", "list: only this item (1-based)"), integer("row", "table: only this row (1-based)"), integer("col", "table: only this column (1-based)")};
}

bool targets(Block& b, const Params& p, std::vector<Runs*>& out, std::string& error) {
    switch (b.kind) {
        case Block::Paragraph:
        case Block::Heading:
        case Block::Quote:
        case Block::Code:
        case Block::Toc: out.push_back(&b.runs); break;
        case Block::List: {
            if (has(p, "item")) {
                const std::int64_t n = intParam(p, "item");
                if (n < 1 || n > static_cast<std::int64_t>(b.items.size())) {
                    error = "item must be 1.." + std::to_string(b.items.size());
                    return false;
                }
                out.push_back(&b.items[static_cast<std::size_t>(n - 1)].runs);
            } else {
                for (auto& it : b.items) out.push_back(&it.runs);
            }
            break;
        }
        case Block::Table: {
            std::size_t cols = 0;
            for (const auto& r : b.rows) cols = std::max(cols, r.size());
            const std::int64_t row = intParam(p, "row", 0), col = intParam(p, "col", 0);
            if (row < 0 || row > static_cast<std::int64_t>(b.rows.size()) || col < 0 || col > static_cast<std::int64_t>(cols)) {
                error = "the table has " + std::to_string(b.rows.size()) + " rows and " + std::to_string(cols) + " columns";
                return false;
            }
            for (std::size_t r = 0; r < b.rows.size(); ++r) {
                if (row && static_cast<std::int64_t>(r) != row - 1) continue;
                for (std::size_t c = 0; c < b.rows[r].size(); ++c) {
                    if (col && static_cast<std::int64_t>(c) != col - 1) continue;
                    out.push_back(&b.rows[r][c]);
                }
            }
            break;
        }
        default: error = std::string("a ") + kindName(b.kind) + " block has no text"; return false;
    }
    return true;
}

std::size_t splitRunsAt(Runs& runs, std::size_t at) {
    std::size_t pos = 0;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (at == pos) return i;
        const std::size_t len = runs[i].text.size();
        if (at < pos + len) {
            Run tail = runs[i];
            tail.text = runs[i].text.substr(at - pos);
            runs[i].text.resize(at - pos);
            runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(i) + 1, tail);
            return i + 1;
        }
        pos += len;
    }
    return runs.size();
}

std::vector<std::pair<std::size_t, std::size_t>> matches(const Runs& runs, const std::string& find, bool matchCase, bool wholeWord) {
    const std::string text = plainText(runs);
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (find.empty()) {
        out.emplace_back(0, text.size());
        return out;
    }
    const std::string hay = matchCase ? text : lower(text), needle = matchCase ? find : lower(find);
    auto wordChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80); };
    for (std::size_t from = 0;;) {
        const std::size_t at = hay.find(needle, from);
        if (at == std::string::npos) break;
        const bool okL = !wholeWord || at == 0 || !wordChar(hay[at - 1]);
        const bool okR = !wholeWord || at + needle.size() >= hay.size() || !wordChar(hay[at + needle.size()]);
        if (okL && okR) {
            out.emplace_back(at, at + needle.size());
            from = at + needle.size();
        } else {
            from = at + 1;
        }
    }
    return out;
}

void applyRange(Runs& runs, std::size_t a, std::size_t b, const std::function<void(Run&)>& fn) {
    if (a > b) return;
    const std::size_t i = splitRunsAt(runs, a);
    const std::size_t j = a == b ? i : splitRunsAt(runs, b);
    for (std::size_t k = i; k < j && k < runs.size(); ++k) fn(runs[k]);
    // a footnote mark right at the end of the whole text belongs to it
    if (b >= plainText(runs).size()) {
        for (std::size_t k = j; k < runs.size(); ++k) {
            if (runs[k].text.empty() && !runs[k].footnote.empty()) fn(runs[k]);
        }
    }
}

std::string editorName(const TextDocument& d) {
    if (!d.editor.empty()) return d.editor;
    for (const char* v : {"USER", "USERNAME", "LOGNAME"}) {
        if (const char* e = std::getenv(v); e && *e) return e;
    }
    return "Editor";
}

int newChange(TextDocument& d, bool insert) {
    TrackedChange ch;
    ch.id = d.nextChangeId();
    ch.insert = insert;
    ch.author = editorName(d);
    const std::time_t now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    ch.date = buf;
    d.changes.push_back(ch);
    return ch.id;
}

void markRuns(TextDocument& d, Runs& runs, int change) {
    // deleting text that is itself a pending insertion removes it outright
    const TrackedChange* c = d.change(change);
    const bool deleting = c && !c->insert;
    Runs out;
    for (auto& r : runs) {
        const TrackedChange* own = r.change ? d.change(r.change) : nullptr;
        if (deleting && own && own->insert) continue;
        if (!own) r.change = change;
        out.push_back(std::move(r));
    }
    runs = std::move(out);
}

bool markBlock(TextDocument& d, Block& b, int change) {
    if (b.kind == Block::Image || b.kind == Block::PageBreak || b.kind == Block::Toc) return false;
    markRuns(d, b.runs, change);
    for (auto& it : b.items) markRuns(d, it.runs, change);
    for (auto& row : b.rows) {
        for (auto& c : row) markRuns(d, c, change);
    }
    bool any = !b.runs.empty();
    for (const auto& it : b.items) any = any || !it.runs.empty();
    for (const auto& row : b.rows) {
        for (const auto& c : row) any = any || !c.empty();
    }
    return any;
}

Runs trackedReplace(const TextDocument& d, const Runs& oldRuns, const Runs& newRuns, int del, int ins) {
    const std::string a = plainText(oldRuns), b = plainText(newRuns);
    std::size_t pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) ++pre;
    while (pre > 0 && pre < a.size() && (static_cast<unsigned char>(a[pre]) & 0xC0) == 0x80) --pre;
    std::size_t suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre && a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) ++suf;
    while (suf > 0 && (static_cast<unsigned char>(a[a.size() - suf]) & 0xC0) == 0x80) --suf;
    Runs left = oldRuns, mid = newRuns;
    const std::size_t i = splitRunsAt(left, pre);
    const std::size_t j = splitRunsAt(left, a.size() - suf);
    const std::size_t m1 = splitRunsAt(mid, pre);
    const std::size_t m2 = splitRunsAt(mid, b.size() - suf);
    Runs out(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(i));
    for (std::size_t k = i; k < j; ++k) {
        Run r = left[k];
        if (r.text.empty() && r.footnote.empty()) continue;
        const TrackedChange* own = r.change ? d.change(r.change) : nullptr;
        if (own && own->insert) continue;  // a pending insertion that is taken out again just goes
        if (!own) r.change = del;
        out.push_back(r);
    }
    for (std::size_t k = m1; k < m2; ++k) {
        Run r = mid[k];
        if (r.text.empty() && r.footnote.empty()) continue;
        r.change = ins;
        out.push_back(r);
    }
    out.insert(out.end(), left.begin() + static_cast<std::ptrdiff_t>(j), left.end());
    mergeRuns(out);
    return out;
}

namespace {

ParamSpec styleChoice(const std::string& name, const std::string& description) {
    return str(name, description + " (Normal, Title, Subtitle, Heading 1-6, Quote, Code, Caption, or a style made with office.doc_style_define)");
}

const char* alignWord(char a) { return a == 'c' ? "center" : a == 'r' ? "right" : a == 'j' ? "justify" : a == 'l' ? "left" : "default"; }

bool parseAlign(const std::string& s, char& out) {
    const std::string l = lower(s);
    if (l == "left" || l == "start") out = 'l';
    else if (l == "center" || l == "centre" || l == "middle") out = 'c';
    else if (l == "right" || l == "end") out = 'r';
    else if (l == "justify" || l == "justified" || l == "both") out = 'j';
    else return false;
    return true;
}

// The paragraph-format params shared by doc_format and doc_style_define.
std::vector<ParamSpec> paraSpecs() {
    return {str("align", "paragraph alignment", false, {}, {"left", "center", "right", "justify"}),
            number("indent", "left indent in pt"),
            number("indent_right", "right indent in pt"),
            number("first_line", "first-line indent in pt (negative: hanging)"),
            number("space_before", "space above in pt"),
            number("space_after", "space below in pt"),
            number("line_spacing", "line spacing as a multiple (1 single, 1.5, 2 double)"),
            flag("keep_with_next", "keep on the same page as the next paragraph")};
}

bool applyPara(ParaFormat& f, const Params& p, std::vector<std::string>& did, std::string& error) {
    if (has(p, "align")) {
        if (!parseAlign(stringParam(p, "align"), f.align)) {
            error = "align must be left, center, right or justify";
            return false;
        }
        did.push_back(std::string("aligned ") + alignWord(f.align));
    }
    auto num = [&](const char* key, double& v, double lo, double hi, const char* what) {
        if (!has(p, key)) return true;
        const double x = numberParam(p, key);
        if (!(x >= lo && x <= hi)) {
            error = std::string(key) + " must be " + textNumber(lo) + ".." + textNumber(hi);
            return false;
        }
        v = x;
        did.push_back(std::string(what) + " " + textNumber(x));
        return true;
    };
    if (!num("indent", f.indent, 0, 400, "indent") || !num("indent_right", f.indentRight, 0, 400, "right indent") ||
        !num("first_line", f.firstLine, -200, 200, "first line") || !num("space_before", f.before, 0, 200, "space before") ||
        !num("space_after", f.after, 0, 200, "space after") || !num("line_spacing", f.lineSpacing, 0.5, 5, "line spacing")) {
        return false;
    }
    if (has(p, "keep_with_next")) {
        f.keepWithNext = boolParam(p, "keep_with_next");
        did.push_back(f.keepWithNext ? "kept with next" : "not kept with next");
    }
    return true;
}

bool fontArg(const Params& p, std::string& out, std::string& error) {
    const std::string f = lower(stringParam(p, "font"));
    if (f == "default" || f.empty()) out.clear();
    else if (f == "sans" || f == "serif" || f == "mono") out = f;
    else {
        error = "font must be sans, serif, mono or default";
        return false;
    }
    return true;
}

// Run formatting from params into a function, and words for the message.
bool runFormat(const Params& p, std::function<void(Run&)>& fn, std::vector<std::string>& did, std::string& error) {
    std::vector<std::function<void(Run&)>> steps;
    auto boolStep = [&](const char* key, bool Run::*field) {
        if (!has(p, key)) return;
        const bool v = boolParam(p, key);
        steps.push_back([=](Run& r) { r.*field = v; });
        did.push_back(std::string(v ? "" : "not ") + key);
    };
    boolStep("bold", &Run::bold);
    boolStep("italic", &Run::italic);
    boolStep("underline", &Run::underline);
    boolStep("strike", &Run::strike);
    boolStep("code", &Run::code);
    auto colorStep = [&](const char* key, std::uint32_t Run::*field) {
        if (!has(p, key)) return true;
        std::uint32_t c = 0;
        const std::string t = stringParam(p, key);
        if (lower(t) != "none" && lower(t) != "auto" && !t.empty() && !parseColor(t, c)) {
            error = std::string(key) + " must be a colour like #c00000 or red (none removes it)";
            return false;
        }
        steps.push_back([=](Run& r) { r.*field = c; });
        did.push_back(std::string(key) + " " + colorName(c));
        return true;
    };
    if (!colorStep("color", &Run::color) || !colorStep("highlight", &Run::highlight)) return false;
    if (has(p, "size")) {
        const double s = numberParam(p, "size");
        if (!(s == 0 || (s >= 4 && s <= 144))) {
            error = "size must be 4..144 pt (0 = the paragraph's size)";
            return false;
        }
        steps.push_back([=](Run& r) { r.size = s; });
        did.push_back(s ? "size " + textNumber(s) + " pt" : std::string("paragraph size"));
    }
    if (has(p, "font")) {
        std::string f;
        if (!fontArg(p, f, error)) return false;
        steps.push_back([=](Run& r) { r.font = f; });
        did.push_back("font " + (f.empty() ? std::string("default") : f));
    }
    if (has(p, "position")) {
        const std::string v = lower(stringParam(p, "position"));
        const int pos = v == "superscript" || v == "super" ? 1 : v == "subscript" || v == "sub" ? -1 : 0;
        steps.push_back([=](Run& r) { r.vertical = pos; });
        did.push_back(pos > 0 ? "superscript" : pos < 0 ? "subscript" : "normal position");
    }
    if (has(p, "link")) {
        const std::string url = stringParam(p, "link");
        steps.push_back([=](Run& r) { r.link = url; });
        did.push_back(url.empty() ? std::string("link removed") : "link " + url);
    }
    fn = [steps](Run& r) {
        for (const auto& s : steps) s(r);
    };
    return true;
}

// Turns a block into the kind its new paragraph style stands for.
bool applyStyle(TextDocument& doc, Block& b, const std::string& name, std::string& error) {
    const std::string id = styleIdOf(name);
    const bool text = b.kind == Block::Paragraph || b.kind == Block::Heading || b.kind == Block::Quote || b.kind == Block::Code;
    if (!text) {
        if (b.kind == Block::Image && (id == "Normal" || id == "Caption" || doc.styles.count(id))) {
            b.style = id == "Normal" ? "" : id;
            return true;
        }
        error = std::string("styles apply to paragraphs, headings, quotes and code, not a ") + kindName(b.kind);
        return false;
    }
    if (!isBuiltinStyle(id) && !doc.styles.count(id)) {
        error = "no style " + name + " (office.doc_styles lists them; office.doc_style_define makes one)";
        return false;
    }
    if (id == "ListParagraph") {
        error = "use doc_edit kind=list to make a list";
        return false;
    }
    if (b.kind == Block::Code && id != "Code") {
        Run r;
        r.text = plainText(b.runs);
        b.runs = {r};
    }
    if (startsWith(id, "Heading") && id.size() == 8) {
        b.kind = Block::Heading;
        b.level = id[7] - '0';
        b.style.clear();
    } else if (id == "Quote") {
        b.kind = Block::Quote;
        b.style.clear();
    } else if (id == "Code") {
        Run r;
        r.text = plainText(b.runs);
        r.code = true;
        b.kind = Block::Code;
        b.runs = {r};
        b.style.clear();
    } else {
        b.kind = Block::Paragraph;
        b.style = id == "Normal" ? "" : id;
    }
    return true;
}

std::string styleLook(const TextDocument& doc, const std::string& id) {
    const ResolvedStyle r = resolveStyle(doc, id);
    std::string t = std::string(r.font == pdf::Family::Serif ? "serif" : r.font == pdf::Family::Mono ? "mono" : "sans") + " " + textNumber(std::round(r.size * 10) / 10) + " pt";
    if (r.bold) t += " bold";
    if (r.italic) t += " italic";
    if (r.underline) t += " underlined";
    if (hasColor(r.color)) t += " " + colorName(r.color);
    if (r.align != 'l') t += std::string(", ") + alignWord(r.align);
    if (r.indent) t += ", indent " + textNumber(r.indent);
    t += ", space " + textNumber(r.before) + "/" + textNumber(r.after) + " pt, line " + textNumber(std::round(r.lineSpacing * 100) / 100);
    return t;
}

}  // namespace

void registerDocFormatCommands(Session& session) {
    auto& reg = session.registry();

    std::vector<ParamSpec> formatSpecs = {doc(), integer("block", "block number (office.doc_info lists them)", true), integer("to", "format blocks block..to"),
                                          str("find", "only this text (every match); default: all the text"), flag("match_case", "find: case-sensitive", false),
                                          flag("whole_word", "find: whole words only", false)};
    for (auto& s : targetSpecs()) formatSpecs.push_back(s);
    for (auto& s : std::vector<ParamSpec>{flag("bold", "bold"), flag("italic", "italic"), flag("underline", "underline"), flag("strike", "strikethrough"),
                                          flag("code", "code (monospace) text"), str("color", "text colour (#rrggbb or a name; none = automatic)"),
                                          str("highlight", "highlight colour behind the text (none removes it)"), number("size", "text size in pt (0 = the paragraph's)"),
                                          str("font", "font", false, {}, {"sans", "serif", "mono", "default"}),
                                          str("position", "raised or lowered text", false, {}, {"normal", "superscript", "subscript"}),
                                          str("link", "make the text a link to this URL (empty removes the link)"), styleChoice("style", "paragraph style")}) {
        formatSpecs.push_back(s);
    }
    for (auto& s : paraSpecs()) formatSpecs.push_back(s);
    formatSpecs.push_back(flag("page_break_before", "start the block on a new page"));
    formatSpecs.push_back(flag("clear", "first remove direct formatting (run styles and paragraph format)"));
    formatSpecs.push_back(previewSpec());

    reg.add({"office.doc_format", "Format Text",
             "Format a block (or blocks block..to): character formatting (bold, italic, underline, strike, color, highlight, size, font, "
             "superscript/subscript, link) on all its text or only on find= matches, list item= or table row=/col=; paragraph formatting "
             "(align, indents, spacing, line spacing, keep with next, page break before); style= applies a paragraph style (Heading 2 turns "
             "the block into a heading). align also places images. clear=true removes direct formatting first.",
             formatSpecs, true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t first, last;
                std::string error;
                if (!blockArg(d.data, p, "block", first, error)) return Result::failure(error);
                last = first;
                if (has(p, "to") && !blockArg(d.data, p, "to", last, error)) return Result::failure(error);
                if (last < first) std::swap(first, last);
                std::function<void(Run&)> fn;
                std::vector<std::string> did;
                if (!runFormat(p, fn, did, error)) return Result::failure(error);
                const bool runChanges = !did.empty();
                const bool clear = boolParam(p, "clear", false);
                const std::string find = has(p, "find") ? stringParam(p, "find") : "";
                int found = 0;
                for (std::size_t i = first; i <= last; ++i) {
                    Block& b = d.data.blocks[i];
                    if (has(p, "style") && !applyStyle(d.data, b, stringParam(p, "style"), error)) return Result::failure("block " + std::to_string(i + 1) + ": " + error);
                    if (clear) b.fmt = ParaFormat{};
                    std::vector<std::string> pd;
                    if (!applyPara(b.fmt, p, pd, error)) return Result::failure(error);
                    if (has(p, "page_break_before")) b.fmt.pageBreakBefore = boolParam(p, "page_break_before");
                    if (i == first) {
                        did.insert(did.end(), pd.begin(), pd.end());
                        if (has(p, "page_break_before")) did.push_back(b.fmt.pageBreakBefore ? "page break before" : "no page break before");
                    }
                    if (!runChanges && !clear) continue;
                    std::vector<Runs*> runs;
                    if (!targets(b, p, runs, error)) {
                        if (first == last) return Result::failure("block " + std::to_string(i + 1) + ": " + error);
                        continue;
                    }
                    for (Runs* rs : runs) {
                        for (const auto& [a, e] : matches(*rs, find, boolParam(p, "match_case", false), boolParam(p, "whole_word", false))) {
                            // ranges shift as runs split, but byte offsets of the plain text do not
                            applyRange(*rs, a, e, [&](Run& r) {
                                if (clear) {
                                    r.bold = r.italic = r.underline = r.strike = r.code = false;
                                    r.vertical = 0;
                                    r.color = r.highlight = 0;
                                    r.size = 0;
                                    r.font.clear();
                                }
                                if (fn) fn(r);
                            });
                            ++found;
                        }
                        mergeRuns(*rs);
                    }
                }
                if (has(p, "style")) did.insert(did.begin(), "style " + stringParam(p, "style"));
                if (clear) did.insert(did.begin(), "cleared formatting");
                if (did.empty()) return Result::failure("nothing to change: give bold=, color=, align=, style=, ... (office.help office.doc_format)");
                if (!find.empty() && !found) return Result::failure("\"" + find + "\" is not in the block(s)");
                std::string what = first == last ? "block " + std::to_string(first + 1) : "blocks " + std::to_string(first + 1) + "-" + std::to_string(last + 1);
                if (!find.empty()) what += " (" + std::to_string(found) + " match(es) of \"" + find + "\")";
                std::string list;
                for (const auto& s : did) list += (list.empty() ? "" : ", ") + s;
                Result r = Result::success("formatted " + what + ": " + list);
                if (!find.empty()) r.with("matches", static_cast<std::int64_t>(found));
                maybePreview(r, d, p);
                return r;
            }));

    std::vector<ParamSpec> defineSpecs = {doc(), str("name", "style name: a built-in (Normal, Title, Heading 1, Quote, ...) to change it, or a new name", true),
                                          styleChoice("based_on", "style it starts from (default Normal)"), str("font", "font", false, {}, {"sans", "serif", "mono", "default"}),
                                          number("size", "text size in pt"), str("color", "text colour (#rrggbb or a name)"), flag("bold", "bold"), flag("italic", "italic"),
                                          flag("underline", "underline")};
    for (auto& s : paraSpecs()) defineSpecs.push_back(s);
    defineSpecs.push_back(flag("reset", "built-in: back to its default look; your own: delete it (blocks using it become Normal)"));
    defineSpecs.push_back(previewSpec());

    reg.add({"office.doc_style_define", "Define Style",
             "Create a paragraph style or change one (built-in or your own): font, size, colour, bold/italic/underline, alignment, indents, "
             "spacing. Every block with the style follows. Apply it with office.doc_format style=.",
             defineSpecs, true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                const std::string id = styleIdOf(stringParam(p, "name"));
                if (id.empty()) return Result::failure("name is empty");
                if (id == "ListParagraph") return Result::failure("List Paragraph is set by the list itself");
                if (boolParam(p, "reset", false)) {
                    if (!d.data.styles.erase(id)) return Result::failure(isBuiltinStyle(id) ? id + " already has its default look" : "no style " + id);
                    int used = 0;
                    if (!isBuiltinStyle(id)) {
                        for (auto& b : d.data.blocks) {
                            if (b.style == id) b.style.clear(), ++used;
                        }
                        for (auto& [k, s] : d.data.styles) {
                            if (s.basedOn == id) s.basedOn.clear();
                        }
                    }
                    return Result::success(isBuiltinStyle(id) ? "reset " + id + " to its default look" : "deleted style " + id + " (" + std::to_string(used) + " block(s) now Normal)");
                }
                const bool exists = d.data.styles.count(id) > 0;
                StyleDef s = exists ? d.data.styles[id] : StyleDef{};
                if (!exists) s.name = isBuiltinStyle(id) ? "" : trimCopy(stringParam(p, "name"));
                std::vector<std::string> did;
                std::string error;
                if (has(p, "based_on")) {
                    std::string base = styleIdOf(stringParam(p, "based_on"));
                    if (base == "Normal") base.clear();
                    if (!base.empty() && !isBuiltinStyle(base) && !d.data.styles.count(base)) return Result::failure("no style " + base + " to base it on");
                    for (std::string b = base; !b.empty();) {  // no loops
                        if (b == id) return Result::failure("a style can't be based on itself");
                        auto it = d.data.styles.find(b);
                        b = it == d.data.styles.end() ? "" : it->second.basedOn;
                    }
                    if (isBuiltinStyle(id) && id != "Normal" && !base.empty()) return Result::failure("built-in styles stay based on Normal");
                    s.basedOn = base;
                    did.push_back("based on " + (base.empty() ? std::string("Normal") : base));
                }
                if (has(p, "font")) {
                    if (!fontArg(p, s.font, error)) return Result::failure(error);
                    did.push_back("font " + (s.font.empty() ? std::string("default") : s.font));
                }
                if (has(p, "size")) {
                    const double v = numberParam(p, "size");
                    if (!(v >= 4 && v <= 144)) return Result::failure("size must be 4..144 pt");
                    s.size = v;
                    did.push_back("size " + textNumber(v) + " pt");
                }
                if (has(p, "color")) {
                    std::uint32_t c = 0;
                    if (!parseColor(stringParam(p, "color"), c)) return Result::failure("color must be like #1f3864 or navy");
                    s.color = c;
                    did.push_back("color " + colorName(c));
                }
                for (const auto& [key, field] : {std::pair<const char*, int StyleDef::*>{"bold", &StyleDef::bold}, {"italic", &StyleDef::italic}, {"underline", &StyleDef::underline}}) {
                    if (!has(p, key)) continue;
                    s.*field = boolParam(p, key) ? 1 : 0;
                    did.push_back(std::string(boolParam(p, key) ? "" : "not ") + key);
                }
                if (!applyPara(s.para, p, did, error)) return Result::failure(error);
                if (did.empty() && exists) return Result::failure("nothing to change (give font=, size=, color=, bold=, align=, ...)");
                d.data.styles[id] = s;
                std::string list;
                for (const auto& x : did) list += (list.empty() ? "" : ", ") + x;
                int used = 0;
                for (const auto& b : d.data.blocks) used += blockStyleId(b) == id;
                Result r = Result::success(std::string(exists || isBuiltinStyle(id) ? "changed" : "created") + " style " + id + (list.empty() ? "" : ": " + list) + " (" +
                                           styleLook(d.data, id) + "; used by " + std::to_string(used) + " block(s))");
                r.with("style", id);
                maybePreview(r, d, p);
                return r;
            }));

    reg.add({"office.doc_styles", "List Styles", "List the paragraph styles: the built-in ones and your own, how each looks and how many blocks use it.",
             {doc(), fileSpec("document")}, false, true},
            read(session, [](TextDoc& d, const Params&, const std::string&) -> Result {
                std::map<std::string, int> used;
                for (const auto& b : d.data.blocks) ++used[blockStyleId(b)];
                std::string t;
                auto line = [&](const std::string& id) {
                    const auto it = d.data.styles.find(id);
                    std::string name = id;
                    if (startsWith(id, "Heading") && id.size() == 8) name = "Heading " + id.substr(7);
                    if (id == "ListParagraph") name = "List Paragraph";
                    t += "\n  " + name + ": " + styleLook(d.data, id);
                    if (it != d.data.styles.end()) t += isBuiltinStyle(id) ? " (changed)" : it->second.basedOn.empty() ? " (yours)" : " (yours, based on " + it->second.basedOn + ")";
                    if (used.count(id)) t += "; " + std::to_string(used[id]) + " block(s)";
                };
                for (const auto& id : builtinStyleIds()) line(id);
                int own = 0;
                for (const auto& [id, s] : d.data.styles) {
                    if (!isBuiltinStyle(id)) line(id), ++own;
                }
                return Result::success(std::to_string(builtinStyleIds().size() + static_cast<std::size_t>(own)) + " styles:" + t).with("custom", static_cast<std::int64_t>(own));
            }));

    reg.add({"office.doc_list", "List Numbering",
             "Change a list: numbering (bullet, decimal 1., lowerLetter a), upperLetter A., lowerRoman i., upperRoman I.), the first number, "
             "or one item's level (item= with level= 1 for top, 2 for a sub-item, ...).",
             {doc(), integer("block", "the list's block number", true),
              str("numbering", "list numbering", false, {}, {"bullet", "decimal", "lowerLetter", "upperLetter", "lowerRoman", "upperRoman"}),
              integer("start", "first number"), integer("item", "item to move in or out (1-based)"), integer("level", "the item's level, 1 = top"), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t i;
                std::string error;
                if (!blockArg(d.data, p, "block", i, error)) return Result::failure(error);
                Block& b = d.data.blocks[i];
                if (b.kind != Block::List) return Result::failure("block " + std::to_string(i + 1) + " is not a list");
                std::vector<std::string> did;
                if (has(p, "numbering")) {
                    const std::string n = stringParam(p, "numbering");
                    b.ordered = n != "bullet";
                    b.numbering = n == "bullet" || n == "decimal" ? "" : n;
                    did.push_back(n);
                }
                if (has(p, "start")) {
                    b.start = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "start"), 0, 100000));
                    did.push_back("starts at " + std::to_string(b.start));
                }
                if (has(p, "item") || has(p, "level")) {
                    const std::int64_t n = intParam(p, "item", 0), lv = intParam(p, "level", 0);
                    if (n < 1 || n > static_cast<std::int64_t>(b.items.size())) return Result::failure("item must be 1.." + std::to_string(b.items.size()));
                    if (lv < 1 || lv > 9) return Result::failure("level must be 1..9");
                    b.items[static_cast<std::size_t>(n - 1)].level = static_cast<int>(lv - 1);
                    did.push_back("item " + std::to_string(n) + " at level " + std::to_string(lv));
                }
                if (did.empty()) return Result::failure("nothing to change (numbering=, start=, or item= with level=)");
                std::string list;
                for (const auto& x : did) list += (list.empty() ? "" : ", ") + x;
                Result r = Result::success("list " + std::to_string(i + 1) + ": " + list);
                maybePreview(r, d, p);
                return r;
            }));
}

}  // namespace wetwaresuite::office::doccmd
