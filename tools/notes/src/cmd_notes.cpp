// notes.create / read / write / append / rename / move / delete
#include <algorithm>
#include <cstdio>
#include <cctype>

#include "internal.hpp"

namespace wetwaresuite::notes::detail {

namespace {

std::string extOfRel(const std::string& rel) {
    std::string name = fileNameOf(rel);
    auto dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? "" : name.substr(dot);
}

// The new path of a renamed file: a bare name stays in its folder, a path is from the vault root.
std::string renamedRel(const Vault& v, const std::string& from, const std::string& rawTo) {
    std::string to = trim(rawTo);
    for (auto& c : to) {
        if (c == '\\') c = '/';
    }
    if (to.empty()) throw std::runtime_error("give the new name: to=<name or path>");
    const bool note = v.isNote(from);
    if (!note) {
        std::string ext = extOfRel(from);
        if (extOfRel(to).empty()) to += ext;
        std::string rel = to.find('/') == std::string::npos ? joinRel(folderOf(from), to) : to;
        if (rel.find("..") != std::string::npos) throw std::runtime_error("paths can't leave the vault");
        return rel;
    }
    if (to.find('/') == std::string::npos) return newNoteRel(to, folderOf(from));
    return newNoteRel(to);
}

Result moved(const std::string& verb, const std::string& from, const std::string& to, std::size_t links, const std::vector<std::string>& changed) {
    std::string msg = verb + " " + from + " -> " + to;
    if (links) {
        msg += "; updated " + plural(links, "link") + " in " + plural(changed.size(), "note");
        for (std::size_t i = 0; i < changed.size() && i < 20; ++i) msg += "\n  " + changed[i];
        if (changed.size() > 20) msg += "\n  ... and " + std::to_string(changed.size() - 20) + " more";
    }
    Result r = Result::success(msg, to);
    return r.with("note", to).with("from", from).with("links_updated", static_cast<std::int64_t>(links)).with("notes_changed", static_cast<std::int64_t>(changed.size()));
}

void registerCreateRead(CommandRegistry& reg, const StatePtr& st) {
    std::vector<ParamSpec> create = {vaultParam(),
                                     noteParam("the new note: a name (\"Meeting notes\") or a path inside the vault (\"Projects/Plan\"); .md is added"),
                                     {"folder", ParamType::String, "put it in this folder (when note= is just a name)", false},
                                     {"template", ParamType::String, "start from this template (a name in the templates folder, or a note path)", false},
                                     {"vars", ParamType::String, "extra template variables: \"client=Acme; budget=10k\" fills {{client}} and {{budget}}", false},
                                     boolParamSpec("overwrite", "replace an existing note instead of failing"),
                                     {"today", ParamType::String, "the date {{date}} means (default: today)", false}};
    for (auto& t : textParams("content", "the note's text (after the template, if any)", false)) create.push_back(t);
    reg.add({"notes.create", "Create note",
             "Create a Markdown note. With template=, the template's {{title}}, {{date}}, {{time}}, {{date:YYYY-MM-DD}} "
             "(moment.js formats), {{yesterday}}, {{tomorrow}} and vars= are filled in, and its frontmatter is kept. "
             "Fails if the note exists unless overwrite=true. Returns the note's path (capture it with -> name).",
             create, true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                std::string rel = newNoteRel(stringParam(p, "note"), stringParam(p, "folder"));
                std::error_code ec;
                const bool exists = fs::exists(v.abs(rel), ec);
                if (exists && !boolParam(p, "overwrite")) return Result::failure(rel + " already exists (use notes.write or notes.append to change it, or overwrite=true)");
                std::string content;
                const Date date = baseDate(p);
                if (has(p, "template") && !trim(stringParam(p, "template")).empty()) {
                    std::string t = findTemplate(v, stringParam(p, "template"));
                    if (t.empty()) return Result::failure("no template called '" + stringParam(p, "template") + "' (templates live in " + v.settings.templatesFolder + "/; see notes.templates.list)");
                    content = expandTemplate(v.read(t), v, withoutMd(fileNameOf(rel)), date, parseVars(stringParam(p, "vars")), folderOf(rel));
                }
                if (hasText(p, "content")) {
                    std::string text = textParam(p, "content");
                    content = content.empty() ? text : appendText(content, text, "", false);
                }
                if (!content.empty() && content.back() != '\n') content += "\n";
                txn.write(v.abs(rel), content);
                Result r = Result::success((exists ? "replaced " : "created ") + rel, rel);
                return r.with("note", rel).with("path", fromPath(v.abs(rel))).with("bytes", static_cast<std::int64_t>(content.size()));
            }));

    reg.add({"notes.read", "Read note",
             "The text of a note, or of one heading's section (heading=). Also says its path, title, tags and word count.",
             {vaultParam(), noteParam(),
              {"heading", ParamType::String, "only this heading's section (to the next heading of the same level)", false},
              boolParamSpec("frontmatter", "include the YAML frontmatter", true),
              {"lines", ParamType::String, "only these lines, e.g. 10-20 (1-based, inclusive)", false}},
             false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::string rel = needNote(v, p);
                std::string text = v.read(rel);
                if (has(p, "heading") && !trim(stringParam(p, "heading")).empty()) {
                    auto range = sectionRange(text, stringParam(p, "heading"));
                    if (!range) {
                        std::string list;
                        for (const auto& h : v.note(rel)->headings) list += (list.empty() ? "" : ", ") + h.text;
                        return Result::failure("no heading '" + stringParam(p, "heading") + "' in " + rel + (list.empty() ? "" : " (it has: " + list + ")"));
                    }
                    text = text.substr(range->first, range->second - range->first);
                } else if (!boolParam(p, "frontmatter", true)) {
                    text = bodyOf(text);
                    while (!text.empty() && text[0] == '\n') text.erase(0, 1);
                }
                if (has(p, "lines")) {
                    std::string spec = stringParam(p, "lines");
                    int a = 1, b = 1 << 30;
                    if (std::sscanf(spec.c_str(), "%d-%d", &a, &b) < 1) return Result::failure("lines= looks like 10-20");
                    if (spec.find('-') == std::string::npos) b = a;
                    auto lines = splitLines(text);
                    std::string out;
                    for (int i = std::max(1, a); i <= b && i <= static_cast<int>(lines.size()); ++i) out += lines[static_cast<std::size_t>(i - 1)] + "\n";
                    text = out;
                }
                const Note* n = v.note(rel);
                std::string tags;
                for (const auto& t : n->tags) tags += (tags.empty() ? "" : " ") + std::string("#") + t;
                Result r = Result::success(text);
                return r.with("note", rel).with("title", n->title).with("tags", tags).with("words", static_cast<std::int64_t>(n->words));
            }));
}

void registerWrite(CommandRegistry& reg, const StatePtr& st) {
    std::vector<ParamSpec> write = {vaultParam(), noteParam(), boolParamSpec("create", "create the note when it doesn't exist", true),
                                    boolParamSpec("keep_frontmatter", "keep the note's frontmatter when the new text has none")};
    for (auto& t : textParams("content", "the note's new text", true)) write.push_back(t);
    reg.add({"notes.write", "Write note", "Replace a note's whole text (creating it unless create=false). Undo puts the old text back.",
             write, true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                if (!hasText(p, "content")) return Result::failure("give content= (or content_file=)");
                std::string rel = v.findNote(stringParam(p, "note"));
                if (rel.empty() || !v.isNote(rel)) {
                    if (!boolParam(p, "create", true)) return Result::failure("no note called '" + stringParam(p, "note") + "'");
                    rel = newNoteRel(stringParam(p, "note"));
                }
                std::string content = textParam(p, "content");
                if (!content.empty() && content.back() != '\n') content += "\n";
                std::error_code ec;
                if (boolParam(p, "keep_frontmatter") && fs::exists(v.abs(rel), ec) && !splitFrontmatter(content, nullptr, nullptr)) {
                    std::string old = v.read(rel);
                    std::size_t bs = 0;
                    if (splitFrontmatter(old, nullptr, &bs)) content = old.substr(0, bs) + content;
                }
                txn.write(v.abs(rel), content);
                return Result::success("wrote " + rel + " (" + std::to_string(content.size()) + " bytes)", rel).with("note", rel);
            }));

    std::vector<ParamSpec> append = {vaultParam(), noteParam(),
                                     {"heading", ParamType::String, "add it at the end of this heading's section (the heading is added when missing)", false},
                                     boolParamSpec("prepend", "add it at the start instead (after the frontmatter, or right under the heading)"),
                                     boolParamSpec("create", "create the note when it doesn't exist", true)};
    for (auto& t : textParams("content", "the text to add", true)) append.push_back(t);
    reg.add({"notes.append", "Append to note",
             "Add text to the end of a note, or to the end (or start) of one heading's section: a quick capture into an "
             "inbox, a log line under ## Log, a task under ## Todo. List items join the list above them.",
             append, true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                if (!hasText(p, "content")) return Result::failure("give content= (or content_file=)");
                std::string rel = v.findNote(stringParam(p, "note"));
                std::string content;
                if (rel.empty() || !v.isNote(rel)) {
                    if (!boolParam(p, "create", true)) return Result::failure("no note called '" + stringParam(p, "note") + "'");
                    rel = newNoteRel(stringParam(p, "note"));
                } else {
                    content = v.read(rel);
                }
                std::string out = appendText(content, textParam(p, "content"), trim(stringParam(p, "heading")), boolParam(p, "prepend"));
                txn.write(v.abs(rel), out);
                return Result::success("added to " + rel + (has(p, "heading") ? " under " + stringParam(p, "heading") : ""), rel).with("note", rel);
            }));
}

void registerMove(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.rename", "Rename note",
             "Rename a note (or an attachment) and rewrite every [[wikilink]], ![[embed]] and [markdown](link.md) that "
             "points to it across the vault, keeping headings and aliases ([[Old#Part|text]] -> [[New#Part|text]]). "
             "A bare name keeps the folder; a path moves it too. Undo restores every file it touched.",
             {vaultParam(), noteParam("the note or attachment to rename"),
              {"to", ParamType::String, "the new name (\"Project Plan\") or path (\"Archive/Project Plan\")", true},
              boolParamSpec("update_links", "rewrite links to it", true)},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                std::string from = needNote(v, p, "note", false);
                std::string to = renamedRel(v, from, stringParam(p, "to"));
                std::vector<std::string> changed;
                std::size_t links = moveWithLinks(v, txn, from, to, boolParam(p, "update_links", true), changed);
                return moved("renamed", from, to, links, changed);
            }));

    reg.add({"notes.move", "Move note",
             "Move a note (or attachment) to another folder, rewriting links to it like notes.rename (relative markdown "
             "links inside the note are fixed too). folder=\"\" moves it to the vault root.",
             {vaultParam(), noteParam("the note or attachment to move"),
              {"folder", ParamType::String, "the destination folder inside the vault (created if missing)", true},
              boolParamSpec("update_links", "rewrite links to it", true)},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                std::string from = needNote(v, p, "note", false);
                std::string folder = trim(stringParam(p, "folder"));
                for (auto& c : folder) {
                    if (c == '\\') c = '/';
                }
                while (!folder.empty() && (folder.back() == '/')) folder.pop_back();
                while (!folder.empty() && folder[0] == '/') folder.erase(0, 1);
                if (folder.find("..") != std::string::npos || (!folder.empty() && folder[0] == '.')) return Result::failure("the folder must be inside the vault and not hidden");
                std::string to = joinRel(folder, fileNameOf(from));
                std::vector<std::string> changed;
                std::size_t links = moveWithLinks(v, txn, from, to, boolParam(p, "update_links", true), changed);
                return moved("moved", from, to, links, changed);
            }));

    reg.add({"notes.delete", "Delete note",
             "Move a note (or attachment) to the vault's .trash folder (like Obsidian), keeping its folder path there so "
             "notes.trash.restore can put it back; permanent=true deletes it outright. Says how many links now go "
             "nowhere. Undoable either way within the session.",
             {vaultParam(), noteParam("the note or attachment to delete"), boolParamSpec("permanent", "delete instead of moving to .trash")},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                std::string rel = needNote(v, p, "note", false);
                std::size_t broken = v.backlinks(rel).size();
                std::string where;
                if (boolParam(p, "permanent")) {
                    txn.remove(v.abs(rel));
                    where = "deleted " + rel;
                } else {
                    std::string dest = rel;
                    std::error_code ec;
                    for (int k = 2; fs::exists(v.root / kTrashDir / toPath(dest), ec); ++k) {
                        std::string ext = extOfRel(rel);
                        dest = rel.substr(0, rel.size() - ext.size()) + " " + std::to_string(k) + ext;
                    }
                    txn.move(v.abs(rel), v.root / kTrashDir / toPath(dest));
                    where = "moved " + rel + " to " + std::string(kTrashDir) + "/" + dest;
                }
                if (broken) where += "; " + plural(broken, "link") + " to it now go nowhere (see notes.links.unresolved)";
                return Result::success(where, rel).with("note", rel).with("broken_links", static_cast<std::int64_t>(broken));
            }));
}

}  // namespace

void registerNoteCommands(CommandRegistry& reg, const StatePtr& st) {
    registerCreateRead(reg, st);
    registerWrite(reg, st);
    registerMove(reg, st);
}

}  // namespace wetwaresuite::notes::detail
