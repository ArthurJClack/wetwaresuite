// office.doc_* for page furniture and review: headers and footers, the table of
// contents, footnotes, comments and tracked changes (track, list, accept, reject).
#include <algorithm>

#include "doc_cmd.hpp"
#include "text.hpp"

namespace wetwaresuite::office::doccmd {

namespace {

struct NoteRef {
    std::size_t block;
    Runs* runs;
    std::size_t index;  // in runs
};

// Footnote marks in document order (the order they are numbered in).
std::vector<NoteRef> footnotes(TextDocument& d) {
    std::vector<NoteRef> out;
    auto scan = [&](std::size_t bi, Runs& rs) {
        for (std::size_t i = 0; i < rs.size(); ++i) {
            if (!rs[i].footnote.empty()) out.push_back({bi, &rs, i});
        }
    };
    for (std::size_t bi = 0; bi < d.blocks.size(); ++bi) {
        Block& b = d.blocks[bi];
        scan(bi, b.runs);
        for (auto& it : b.items) scan(bi, it.runs);
        for (auto& row : b.rows) {
            for (auto& c : row) scan(bi, c);
        }
    }
    return out;
}

// Calls fn(block index, runs) for every run list in the document.
template <typename F>
void eachRuns(TextDocument& d, F fn) {
    for (std::size_t bi = 0; bi < d.blocks.size(); ++bi) {
        Block& b = d.blocks[bi];
        fn(bi, b.runs);
        for (auto& it : b.items) fn(bi, it.runs);
        for (auto& row : b.rows) {
            for (auto& c : row) fn(bi, c);
        }
    }
}

std::string quoteClip(std::string s, std::size_t n = 60) {
    for (auto& c : s) {
        if (c == '\n' || c == '\t') c = ' ';
    }
    s = trimCopy(s);
    if (s.size() > n) {
        std::size_t cut = n;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s = s.substr(0, cut) + "...";
    }
    return "\"" + s + "\"";
}

// One run list for a footnote or comment: the block's text, item= or row=/col=, or the last one.
bool oneTarget(Block& b, const Params& p, const std::string& find, Runs*& out, std::size_t& at, std::string& error) {
    std::vector<Runs*> runs;
    if (!targets(b, p, runs, error)) return false;
    if (runs.empty()) {
        error = "nothing there to attach to";
        return false;
    }
    if (!find.empty()) {
        for (Runs* rs : runs) {
            auto m = matches(*rs, find, false, false);
            if (!m.empty()) {
                out = rs;
                at = m.front().second;
                return true;
            }
        }
        error = "\"" + find + "\" is not in that text";
        return false;
    }
    out = runs.back();
    at = plainText(*out).size();
    return true;
}

std::string hfLine(const char* what, const std::string& v) { return std::string(what) + ": " + (v.empty() ? std::string("(none)") : v); }

void registerDocPageCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"office.doc_header_footer", "Header and Footer",
             "Set the page header and footer: inline Markdown with fields {page}, {pages}, {title}, {author}, {date}; \"left | center | "
             "right\" places three parts (\"a | b\" left and right; no | centres it). An empty value removes it. different_first=true "
             "gives the first page its own first_header/first_footer (empty = nothing on the title page). Without a footer, page "
             "numbers show when page_numbers is on (office.doc_setup).",
             {doc(), str("header", "header text"), str("footer", "footer text"), flag("different_first", "the first page has its own header and footer"),
              str("first_header", "first page header"), str("first_footer", "first page footer"), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                HeaderFooter& hf = d.data.headerFooter;
                bool any = false;
                for (const auto& [key, field] : {std::pair<const char*, std::string*>{"header", &hf.header}, {"footer", &hf.footer}, {"first_header", &hf.firstHeader},
                                                 {"first_footer", &hf.firstFooter}}) {
                    if (!has(p, key)) continue;
                    *field = trimCopy(stringParam(p, key));
                    any = true;
                }
                if (has(p, "different_first")) hf.differentFirst = boolParam(p, "different_first"), any = true;
                else if (has(p, "first_header") || has(p, "first_footer")) hf.differentFirst = true;
                if (!any) return Result::failure("give header=, footer=, different_first=, first_header= or first_footer=");
                std::string t = hfLine("header", hf.header) + "\n" +
                                hfLine("footer", hf.footer.empty() && d.data.page.pageNumbers ? std::string("(page numbers n / N)") : hf.footer);
                if (hf.differentFirst) t += "\n" + hfLine("first page header", hf.firstHeader) + "\n" + hfLine("first page footer", hf.firstFooter);
                Result r = Result::success(t);
                maybePreview(r, d, p);
                return r;
            }));

    reg.add({"office.doc_toc", "Table of Contents",
             "Insert a table of contents (headings down to levels=, with page numbers; it updates itself as headings change), or change "
             "the one there (levels=, title=), or remove=true. at= puts it before that block (default: the top, after a Title paragraph).",
             {doc(), integer("at", "insert before this block number"), integer("levels", "deepest heading level listed (1-6)", false, std::int64_t{3}),
              str("title", "heading above it (default Contents)"), flag("remove", "remove the table of contents"), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                auto& blocks = d.data.blocks;
                auto existing = std::find_if(blocks.begin(), blocks.end(), [](const Block& b) { return b.kind == Block::Toc; });
                if (boolParam(p, "remove", false)) {
                    if (existing == blocks.end()) return Result::failure("there is no table of contents");
                    blocks.erase(existing);
                    return Result::success("removed the table of contents");
                }
                const int levels = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "levels", existing != blocks.end() ? existing->level : 3), 1, 6));
                Block t;
                t.kind = Block::Toc;
                if (existing != blocks.end()) t = *existing;
                t.level = levels;
                if (has(p, "title")) t.runs = parseInline(stringParam(p, "title"));
                std::size_t at;
                if (has(p, "at")) {
                    const std::int64_t a = intParam(p, "at");
                    if (a < 1 || a > static_cast<std::int64_t>(blocks.size()) + 1) return Result::failure("at must be 1.." + std::to_string(blocks.size() + 1));
                    at = static_cast<std::size_t>(a - 1);
                    if (existing != blocks.end()) {
                        const auto old = static_cast<std::size_t>(existing - blocks.begin());
                        blocks.erase(existing);
                        if (old < at) --at;
                    }
                    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(at), t);
                } else if (existing != blocks.end()) {
                    *existing = t;
                    at = static_cast<std::size_t>(existing - blocks.begin());
                } else {
                    at = 0;
                    while (at < blocks.size() && blocks[at].kind == Block::Paragraph && (blocks[at].style == "Title" || blocks[at].style == "Subtitle")) ++at;
                    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(at), t);
                }
                const auto entries = outline(d.data, levels);
                Result r = Result::success("table of contents at block " + std::to_string(at + 1) + ": " + std::to_string(entries.size()) + " heading(s), levels 1-" +
                                           std::to_string(levels));
                r.with("block", static_cast<std::int64_t>(at + 1)).with("entries", static_cast<std::int64_t>(entries.size()));
                maybePreview(r, d, p);
                return r;
            }));

    std::vector<ParamSpec> noteSpecs = {doc(), integer("block", "block to put the note mark in"), str("text", "the note (inline Markdown)", true),
                                        str("after", "put the mark right after this text (default: at the end)")};
    for (auto& s : targetSpecs()) noteSpecs.push_back(s);
    noteSpecs.push_back(integer("number", "change footnote n's text instead of adding one"));
    noteSpecs.push_back(previewSpec());
    reg.add({"office.doc_footnote", "Add Footnote",
             "Add a footnote: a numbered mark in the text (after= some text, or at the end of the block / list item= / table cell row= col=) "
             "and the note at the foot of the page. number= changes an existing footnote's text instead.",
             noteSpecs, true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                const std::string text = trimCopy(textParam(p, "text"));
                if (text.empty()) return Result::failure("text is empty");
                if (has(p, "number")) {
                    auto notes = footnotes(d.data);
                    const std::int64_t n = intParam(p, "number");
                    if (n < 1 || n > static_cast<std::int64_t>(notes.size())) return Result::failure("number must be 1.." + std::to_string(notes.size()));
                    const NoteRef& ref = notes[static_cast<std::size_t>(n - 1)];
                    (*ref.runs)[ref.index].footnote = text;
                    return Result::success("footnote " + std::to_string(n) + " (block " + std::to_string(ref.block + 1) + "): " + text);
                }
                std::size_t bi;
                std::string error;
                if (!blockArg(d.data, p, "block", bi, error)) return Result::failure(error);
                Runs* rs;
                std::size_t at;
                if (!oneTarget(d.data.blocks[bi], p, has(p, "after") ? stringParam(p, "after") : std::string(), rs, at, error)) return Result::failure(error);
                const std::size_t idx = splitRunsAt(*rs, at);
                Run mark;
                mark.footnote = text;
                if (d.data.trackChanges) mark.change = newChange(d.data, true);
                rs->insert(rs->begin() + static_cast<std::ptrdiff_t>(idx), mark);
                mergeRuns(*rs);
                auto notes = footnotes(d.data);
                std::size_t number = 0;
                for (std::size_t k = 0; k < notes.size(); ++k) {
                    if (notes[k].runs == rs && (*rs)[notes[k].index].footnote == text) number = k + 1;
                }
                Result r = Result::success("added footnote " + std::to_string(number) + " in block " + std::to_string(bi + 1) + " (" + std::to_string(notes.size()) + " in all)");
                r.with("number", static_cast<std::int64_t>(number));
                maybePreview(r, d, p);
                return r;
            }));

    reg.add({"office.doc_footnotes", "List Footnotes", "List the footnotes in order with their numbers and blocks.", {doc(), fileSpec("document")}, false, true},
            read(session, [](TextDoc& d, const Params&, const std::string&) -> Result {
                auto notes = footnotes(d.data);
                if (notes.empty()) return Result::success("no footnotes").with("count", std::int64_t{0});
                std::string t = std::to_string(notes.size()) + " footnote(s):";
                for (std::size_t k = 0; k < notes.size(); ++k) {
                    t += "\n  " + std::to_string(k + 1) + ". (block " + std::to_string(notes[k].block + 1) + ") " + (*notes[k].runs)[notes[k].index].footnote;
                }
                return Result::success(t).with("count", static_cast<std::int64_t>(notes.size()));
            }));

    reg.add({"office.doc_footnote_delete", "Delete Footnote", "Delete footnote n (its mark and its note); the others renumber.",
             {doc(), integer("number", "footnote number (office.doc_footnotes lists them)", true), previewSpec()}, true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                auto notes = footnotes(d.data);
                const std::int64_t n = intParam(p, "number");
                if (n < 1 || n > static_cast<std::int64_t>(notes.size())) {
                    return Result::failure(notes.empty() ? std::string("there are no footnotes") : "number must be 1.." + std::to_string(notes.size()));
                }
                const NoteRef ref = notes[static_cast<std::size_t>(n - 1)];
                const std::string text = (*ref.runs)[ref.index].footnote;
                ref.runs->erase(ref.runs->begin() + static_cast<std::ptrdiff_t>(ref.index));
                mergeRuns(*ref.runs);
                Result r = Result::success("deleted footnote " + std::to_string(n) + ": " + text);
                maybePreview(r, d, p);
                return r;
            }));
}

void registerDocCommentCommands(Session& session) {
    auto& reg = session.registry();

    std::vector<ParamSpec> commentSpecs = {doc(), integer("block", "block to comment on", true), str("text", "the comment", true),
                                           str("find", "comment on this text only (default: the whole block)"), str("author", "who writes it (default: you)")};
    for (auto& s : targetSpecs()) commentSpecs.push_back(s);
    commentSpecs.push_back(previewSpec());
    reg.add({"office.doc_comment", "Add Comment",
             "Add a comment on a block, on some text in it (find=), on a list item= or a table cell row=/col=. Comments show highlighted "
             "in previews and PDFs (show_markup) and are saved in .docx and .odt.",
             commentSpecs, true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t bi;
                std::string error;
                if (!blockArg(d.data, p, "block", bi, error)) return Result::failure(error);
                const std::string text = trimCopy(textParam(p, "text"));
                if (text.empty()) return Result::failure("text is empty");
                std::vector<Runs*> runs;
                if (!targets(d.data.blocks[bi], p, runs, error)) return Result::failure(error);
                Comment c;
                c.id = d.data.nextCommentId();
                c.text = text;
                c.author = has(p, "author") ? stringParam(p, "author") : editorName(d.data);
                const std::time_t now = std::time(nullptr);
                char buf[32];
                std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
                c.date = buf;
                const std::string find = has(p, "find") ? stringParam(p, "find") : "";
                bool placed = false;
                for (Runs* rs : runs) {
                    auto m = matches(*rs, find, false, false);
                    if (m.empty()) continue;
                    const auto [a, e] = find.empty() ? std::pair<std::size_t, std::size_t>{0, plainText(*rs).size()} : m.front();
                    applyRange(*rs, a, e, [&](Run& r) { r.comment = c.id; });
                    placed = true;
                    if (!find.empty()) break;  // the first match
                }
                if (!placed) return Result::failure(find.empty() ? std::string("there is no text there to comment on") : "\"" + find + "\" is not in that text");
                d.data.comments.push_back(c);
                Result r = Result::success("comment " + std::to_string(c.id) + " by " + c.author + " on block " + std::to_string(bi + 1) + ": " + text);
                r.with("comment", static_cast<std::int64_t>(c.id));
                maybePreview(r, d, p);
                return r;
            }));

    reg.add({"office.doc_comments", "List Comments", "List the comments: number, author, open or resolved, the text they are on and what they say.",
             {doc(), fileSpec("document"), flag("open_only", "only comments not resolved", false)}, false, true},
            read(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                std::map<int, std::string> anchor;
                std::map<int, std::size_t> where;
                eachRuns(d.data, [&](std::size_t bi, Runs& rs) {
                    for (const auto& r : rs) {
                        if (!r.comment) continue;
                        anchor[r.comment] += r.text;
                        if (!where.count(r.comment)) where[r.comment] = bi;
                    }
                });
                std::string t;
                int shown = 0;
                for (const auto& c : d.data.comments) {
                    if (boolParam(p, "open_only", false) && c.resolved) continue;
                    ++shown;
                    t += "\n  " + std::to_string(c.id) + ". " + (c.author.empty() ? std::string("?") : c.author) + (c.resolved ? " (resolved)" : "") +
                         (where.count(c.id) ? " on block " + std::to_string(where[c.id] + 1) + " " + quoteClip(anchor[c.id]) : std::string(" (not anchored)")) + ": " + c.text;
                }
                if (!shown) return Result::success("no comments").with("count", std::int64_t{0});
                return Result::success(std::to_string(shown) + " comment(s):" + t).with("count", static_cast<std::int64_t>(shown));
            }));

    reg.add({"office.doc_comment_resolve", "Resolve Comment", "Mark a comment resolved (resolved=false opens it again). Resolved comments are not highlighted.",
             {doc(), integer("id", "comment number (office.doc_comments lists them)", true), flag("resolved", "resolved", true), previewSpec()}, true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                const int id = static_cast<int>(intParam(p, "id"));
                for (auto& c : d.data.comments) {
                    if (c.id != id) continue;
                    c.resolved = boolParam(p, "resolved", true);
                    Result r = Result::success("comment " + std::to_string(id) + (c.resolved ? " resolved" : " opened again"));
                    maybePreview(r, d, p);
                    return r;
                }
                return Result::failure("no comment " + std::to_string(id) + " (office.doc_comments lists them)");
            }));

    reg.add({"office.doc_comment_delete", "Delete Comment", "Delete a comment (id=), or every comment with all=true. The text stays.",
             {doc(), integer("id", "comment number"), flag("all", "delete every comment"), previewSpec()}, true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                const bool all = boolParam(p, "all", false);
                const int id = static_cast<int>(intParam(p, "id", 0));
                if (!all && !id) return Result::failure("give id= or all=true");
                const std::size_t before = d.data.comments.size();
                d.data.comments.erase(std::remove_if(d.data.comments.begin(), d.data.comments.end(), [&](const Comment& c) { return all || c.id == id; }),
                                      d.data.comments.end());
                const std::size_t gone = before - d.data.comments.size();
                if (!gone) return Result::failure(all ? std::string("there are no comments") : "no comment " + std::to_string(id));
                eachRuns(d.data, [&](std::size_t, Runs& rs) {
                    for (auto& r : rs) {
                        if (r.comment && (all || r.comment == id)) r.comment = 0;
                    }
                    mergeRuns(rs);
                });
                Result r = Result::success("deleted " + std::to_string(gone) + " comment(s)");
                maybePreview(r, d, p);
                return r;
            }));
}

std::string changeText(TextDocument& d, int id, std::size_t& block, bool& found) {
    std::string t;
    found = false;
    eachRuns(d, [&](std::size_t bi, Runs& rs) {
        for (const auto& r : rs) {
            if (r.change != id) continue;
            if (!found) block = bi;
            found = true;
            t += r.footnote.empty() ? r.text : "[footnote]";
        }
    });
    return t;
}

void registerDocTrackCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"office.doc_track", "Track Changes",
             "Turn track changes on or off (on=), choose whether previews and PDFs show the markup (show_markup=: insertions underlined, "
             "deletions struck through, commented text highlighted) and who the edits are by (author=). While on, doc_add, doc_edit, "
             "doc_delete, doc_find_replace, doc_table_cell text and doc_footnote record insertions and deletions.",
             {doc(), flag("on", "record changes"), flag("show_markup", "show changes and comments in previews and PDFs"), str("author", "name on new changes and comments"),
              previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                if (!has(p, "on") && !has(p, "show_markup") && !has(p, "author")) return Result::failure("give on=, show_markup= or author=");
                if (has(p, "on")) d.data.trackChanges = boolParam(p, "on");
                if (has(p, "show_markup")) d.data.showMarkup = boolParam(p, "show_markup");
                if (has(p, "author")) d.data.editor = trimCopy(stringParam(p, "author"));
                Result r = Result::success(std::string("track changes ") + (d.data.trackChanges ? "on" : "off") + ", markup " + (d.data.showMarkup ? "shown" : "hidden") +
                                           ", author " + editorName(d.data) + "; " + std::to_string(d.data.changes.size()) + " change(s) pending");
                maybePreview(r, d, p);
                return r;
            }));

    reg.add({"office.doc_changes", "List Changes", "List the tracked changes waiting to be accepted or rejected: number, insertion or deletion, author, date and text.",
             {doc(), fileSpec("document")}, false, true},
            read(session, [](TextDoc& d, const Params&, const std::string&) -> Result {
                std::string t;
                int shown = 0;
                for (const auto& c : d.data.changes) {
                    std::size_t block = 0;
                    bool found = false;
                    const std::string text = changeText(d.data, c.id, block, found);
                    if (!found) continue;
                    ++shown;
                    t += "\n  " + std::to_string(c.id) + ". " + (c.insert ? "inserted" : "deleted") + " by " + (c.author.empty() ? std::string("?") : c.author) +
                         (c.date.empty() ? "" : " " + c.date.substr(0, 10)) + " in block " + std::to_string(block + 1) + ": " + quoteClip(text);
                }
                std::string head = std::string("track changes ") + (d.data.trackChanges ? "on" : "off") + "; ";
                if (!shown) return Result::success(head + "no changes pending").with("count", std::int64_t{0});
                return Result::success(head + std::to_string(shown) + " change(s):" + t).with("count", static_cast<std::int64_t>(shown));
            }));

    for (const bool accept : {true, false}) {
        reg.add({accept ? "office.doc_accept" : "office.doc_reject", accept ? "Accept Changes" : "Reject Changes",
                 accept ? "Accept a tracked change (id=) or all of them: insertions stay as normal text, deletions go."
                        : "Reject a tracked change (id=) or all of them: insertions go, deleted text comes back.",
                 {doc(), integer("id", "change number (office.doc_changes lists them; default all)"), previewSpec()}, true},
                edit(session, [accept](TextDoc& d, const Params& p, const std::string&) -> Result {
                    const int id = static_cast<int>(intParam(p, "id", 0));
                    if (id && !d.data.change(id)) return Result::failure("no change " + std::to_string(id) + " (office.doc_changes lists them)");
                    const int n = accept ? acceptChanges(d.data, id) : rejectChanges(d.data, id);
                    if (!n) return Result::success("no changes to " + std::string(accept ? "accept" : "reject"));
                    Result r = Result::success(std::string(accept ? "accepted " : "rejected ") + std::to_string(n) + " change(s); " + std::to_string(d.data.changes.size()) +
                                               " pending");
                    r.with("count", static_cast<std::int64_t>(n));
                    maybePreview(r, d, p);
                    return r;
                }));
    }
}

}  // namespace

void registerDocReviewCommands(Session& session) {
    registerDocPageCommands(session);
    registerDocCommentCommands(session);
    registerDocTrackCommands(session);
}

}  // namespace wetwaresuite::office::doccmd
