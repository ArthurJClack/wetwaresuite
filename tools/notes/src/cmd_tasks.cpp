// Tasks across the vault: - [ ] items with due dates.
#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "internal.hpp"

namespace achclwss::notes::detail {

using nlohmann::json;

namespace {

struct Found {
    const Note* note;
    const Task* task;
};

std::string taskLine(const Found& f) {
    std::string s = std::string("[") + f.task->mark + "] " + f.task->text + "  (" + f.note->rel + ":" + std::to_string(f.task->line);
    if (!f.task->due.empty()) s += ", due " + f.task->due;
    return s + ")";
}

// Rewrites the task on `line` of `text`. done: 1 = done, 0 = open, -1 = toggle.
std::string setTask(const std::string& text, int line, int done, bool stamp, const Date& today, bool& nowDone) {
    auto lines = splitLines(text);
    if (line < 1 || line > static_cast<int>(lines.size())) throw std::runtime_error("line " + std::to_string(line) + " is past the end of the note");
    std::string& l = lines[static_cast<std::size_t>(line - 1)];
    Task t;
    if (!parseTaskLine(l, t)) throw std::runtime_error("line " + std::to_string(line) + " is not a task: " + l);
    auto open = l.find('[');
    bool want = done < 0 ? !t.done : done == 1;
    l[open + 1] = want ? 'x' : ' ';
    const std::string doneMark = "\xE2\x9C\x85";  // ✅
    if (want && stamp && l.find(doneMark) == std::string::npos) l += " " + doneMark + " " + isoDate(today);
    if (!want) {
        auto m = l.find(" " + doneMark);
        if (m != std::string::npos) {
            std::size_t e = m + 1 + doneMark.size();
            while (e < l.size() && (l[e] == ' ' || std::isdigit(static_cast<unsigned char>(l[e])) || l[e] == '-')) ++e;
            l.erase(m, e - m);
        }
    }
    nowDone = want;
    std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) out += lines[i] + (i + 1 < lines.size() || endsWith(text, "\n") ? nl : "");
    return out;
}

void registerList(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.tasks", "List tasks",
             "Tasks (- [ ] lines) across the vault or one note. Due dates are read from \xF0\x9F\x93\x85 2026-10-20 (the Tasks "
             "plugin), due:2026-10-20, @due(2026-10-20) or [due:: 2026-10-20]. Filter by status, due date, note, tag "
             "(of the note or in the task), folder and text; sorted by due date, undated last.",
             {vaultParam(),
              {"status", ParamType::String, "which tasks", false, {"open", "done", "all"}, std::string("open")},
              {"due", ParamType::String, "due filter", false, {"any", "overdue", "today", "week", "dated", "undated"}, std::string("any")},
              {"before", ParamType::String, "due on or before this date (YYYY-MM-DD, today, +7, ...)", false},
              {"after", ParamType::String, "due on or after this date", false},
              noteParam("only tasks in this note", false),
              {"tag", ParamType::String, "only tasks with this tag, or in notes with it", false},
              {"folder", ParamType::String, "only notes in this folder", false},
              {"query", ParamType::String, "only tasks whose text contains this", false},
              {"sort", ParamType::String, "order", false, {"due", "note"}, std::string("due")},
              {"today", ParamType::String, "what today means for overdue/today/week (default: the clock)", false},
              limitParam(200), jsonParam("each task's note, line, text, done and due date")},
             false, true},
            reader(st, [](Vault& v, const Params& p) {
                const Date today = baseDate(p);
                const std::string todayIso = isoDate(today);
                const std::string status = stringParam(p, "status", "open");
                const std::string due = stringParam(p, "due", "any");
                std::string before, after;
                if (has(p, "before")) {
                    auto d = parseDate(stringParam(p, "before"), today);
                    if (!d) return Result::failure("before= is not a date");
                    before = isoDate(*d);
                }
                if (has(p, "after")) {
                    auto d = parseDate(stringParam(p, "after"), today);
                    if (!d) return Result::failure("after= is not a date");
                    after = isoDate(*d);
                }
                const std::string weekEnd = isoDate(addDays(today, 7));
                std::string only;
                if (has(p, "note") && !stringParam(p, "note").empty()) only = needNote(v, p);
                std::string folder = lower(trim(stringParam(p, "folder")));
                while (!folder.empty() && folder.back() == '/') folder.pop_back();
                std::string tag = lower(trim(stringParam(p, "tag")));
                while (!tag.empty() && tag[0] == '#') tag.erase(0, 1);
                const std::string query = lower(stringParam(p, "query"));
                std::vector<Found> found;
                for (const auto& [rel, n] : v.notes) {
                    if (!only.empty() && rel != only) continue;
                    if (!folder.empty() && !startsWith(lower(rel), folder + "/")) continue;
                    for (const auto& t : n.tasks) {
                        if (status == "open" && t.done) continue;
                        if (status == "done" && !t.done) continue;
                        if (due == "overdue" && (t.due.empty() || t.due >= todayIso)) continue;
                        if (due == "today" && t.due != todayIso) continue;
                        if (due == "week" && (t.due.empty() || t.due > weekEnd)) continue;
                        if (due == "dated" && t.due.empty()) continue;
                        if (due == "undated" && !t.due.empty()) continue;
                        if (!before.empty() && (t.due.empty() || t.due > before)) continue;
                        if (!after.empty() && (t.due.empty() || t.due < after)) continue;
                        if (!query.empty() && lower(t.text).find(query) == std::string::npos) continue;
                        if (!tag.empty() && !n.hasTag(tag)) {
                            std::string lt = lower(t.text);
                            auto pos = lt.find("#" + tag);
                            bool inTask = pos != std::string::npos &&
                                          (pos + 1 + tag.size() >= lt.size() || !isTagChar(static_cast<unsigned char>(lt[pos + 1 + tag.size()])) || lt[pos + 1 + tag.size()] == '/');
                            if (!inTask) continue;
                        }
                        found.push_back({&n, &t});
                    }
                }
                if (stringParam(p, "sort", "due") == "due") {
                    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
                        if (a.task->due.empty() != b.task->due.empty()) return b.task->due.empty();
                        return a.task->due < b.task->due;
                    });
                }
                const std::size_t total = found.size();
                auto limit = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "limit", 200)));
                if (limit && found.size() > limit) found.resize(limit);
                std::size_t overdue = 0;
                std::string msg = plural(total, "task") + (found.size() < total ? ", showing " + std::to_string(found.size()) : "");
                json items = json::array();
                for (const auto& f : found) {
                    bool late = !f.task->done && !f.task->due.empty() && f.task->due < todayIso;
                    overdue += late ? 1 : 0;
                    msg += "\n" + taskLine(f) + (late ? " OVERDUE" : "");
                    items.push_back({{"note", f.note->rel}, {"line", f.task->line}, {"text", f.task->text}, {"done", f.task->done}, {"due", f.task->due}, {"overdue", late}});
                }
                Result r = Result::success(msg);
                r.with("count", static_cast<std::int64_t>(found.size())).with("total", static_cast<std::int64_t>(total)).with("overdue", static_cast<std::int64_t>(overdue));
                if (boolParam(p, "json")) r.with("items", items.dump());
                return r;
            }));
}

void registerEdit(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.tasks.toggle", "Toggle task",
             "Tick or untick a task: by note= and line= (as notes.tasks shows them), or by match= (text in the task; it "
             "must pick one task, among open ones when ticking). done=true/false sets it instead of flipping. "
             "stamp=true adds \xE2\x9C\x85 YYYY-MM-DD like the Tasks plugin.",
             {vaultParam(), noteParam("the note holding the task (needed with line=; narrows match=)", false),
              {"line", ParamType::Int, "the task's line number", false},
              {"match", ParamType::String, "text of the task", false},
              {"done", ParamType::Bool, "set it done (true) or open (false) instead of flipping", false},
              boolParamSpec("stamp", "add a done date when ticking"), {"today", ParamType::String, "the date stamp= writes", false}},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                std::string rel;
                int line = 0;
                if (has(p, "line")) {
                    rel = needNote(v, p);
                    line = static_cast<int>(intParam(p, "line"));
                } else if (has(p, "match")) {
                    std::string only = has(p, "note") && !stringParam(p, "note").empty() ? needNote(v, p) : "";
                    const std::string m = lower(stringParam(p, "match"));
                    const int doneWanted = has(p, "done") ? (boolParam(p, "done") ? 1 : 0) : -1;
                    std::vector<Found> hits;
                    for (const auto& [r, n] : v.notes) {
                        if (!only.empty() && r != only) continue;
                        for (const auto& t : n.tasks) {
                            if (lower(t.text).find(m) == std::string::npos) continue;
                            hits.push_back({&n, &t});
                        }
                    }
                    if (hits.size() > 1) {
                        // prefer tasks that would change
                        std::vector<Found> useful;
                        for (const auto& h : hits) {
                            if (doneWanted < 0 ? !h.task->done : (h.task->done != (doneWanted == 1))) useful.push_back(h);
                        }
                        if (!useful.empty()) hits = useful;
                    }
                    if (hits.empty()) return Result::failure("no task matches '" + stringParam(p, "match") + "'");
                    if (hits.size() > 1) {
                        std::string msg = std::to_string(hits.size()) + " tasks match '" + stringParam(p, "match") + "'; say which with note= and line=:";
                        for (std::size_t i = 0; i < hits.size() && i < 10; ++i) msg += "\n  " + taskLine(hits[i]);
                        return Result::failure(msg);
                    }
                    rel = hits[0].note->rel;
                    line = hits[0].task->line;
                } else {
                    return Result::failure("say which task: note= and line=, or match=");
                }
                const int done = has(p, "done") ? (boolParam(p, "done") ? 1 : 0) : -1;
                bool nowDone = false;
                std::string out = setTask(v.read(rel), line, done, boolParam(p, "stamp"), baseDate(p), nowDone);
                txn.write(v.abs(rel), out);
                std::string shown = splitLines(out)[static_cast<std::size_t>(line - 1)];
                Result r = Result::success(std::string(nowDone ? "done: " : "open: ") + trim(shown) + "  (" + rel + ":" + std::to_string(line) + ")", rel);
                return r.with("note", rel).with("line", static_cast<std::int64_t>(line)).with("done", nowDone);
            }));

    reg.add({"notes.tasks.add", "Add task",
             "Add a - [ ] task to a note (default: today's daily note, created from its template when missing), "
             "at the end or under heading=, with an optional due date written as \xF0\x9F\x93\x85 YYYY-MM-DD.",
             {vaultParam(), noteParam("the note (default: today's daily note)", false),
              {"text", ParamType::String, "what to do", true},
              {"due", ParamType::String, "due date: YYYY-MM-DD, today, tomorrow, +3 ...", false},
              {"heading", ParamType::String, "add it under this heading (added when missing)", false},
              {"style", ParamType::String, "how to write the due date", false, {"emoji", "dataview", "plain"}, std::string("emoji")},
              {"today", ParamType::String, "what today means (default: the clock)", false}},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                const Date today = baseDate(p);
                std::string rel;
                if (has(p, "note") && !stringParam(p, "note").empty()) {
                    rel = v.findNote(stringParam(p, "note"));
                    if (rel.empty() || !v.isNote(rel)) rel = newNoteRel(stringParam(p, "note"));
                } else {
                    bool created = false;
                    Params dp;
                    rel = ensureDaily(v, txn, today, dp, created);
                }
                std::string text = trim(stringParam(p, "text"));
                if (text.empty()) return Result::failure("give the task's text=");
                std::string dueIso;
                if (has(p, "due") && !stringParam(p, "due").empty()) {
                    auto d = parseDate(stringParam(p, "due"), today);
                    if (!d) return Result::failure("due= is not a date");
                    dueIso = isoDate(*d);
                    const std::string style = stringParam(p, "style", "emoji");
                    text += style == "dataview" ? " [due:: " + dueIso + "]" : style == "plain" ? " due:" + dueIso : " \xF0\x9F\x93\x85 " + dueIso;
                }
                std::error_code ec;
                std::string cur = fs::exists(v.abs(rel), ec) ? readText(v.abs(rel)) : "";
                txn.write(v.abs(rel), appendText(cur, "- [ ] " + text, trim(stringParam(p, "heading")), false));
                Result r = Result::success("added to " + rel + ": - [ ] " + text, rel);
                r.with("note", rel);
                if (!dueIso.empty()) r.with("due", dueIso);
                return r;
            }));
}

}  // namespace

void registerTaskCommands(CommandRegistry& reg, const StatePtr& st) {
    registerList(reg, st);
    registerEdit(reg, st);
}

}  // namespace achclwss::notes::detail
