// Frontmatter, tags, templates, daily notes and outlines.
#include <algorithm>
#include <cstdio>
#include <cctype>

#include <nlohmann/json.hpp>

#include "internal.hpp"

namespace wetwaresuite::notes::detail {

using nlohmann::json;

namespace {

json fmJson(const Frontmatter& fm) {
    json j = json::object();
    for (const auto& [k, v] : fm) {
        if (v.isList) j[k] = v.list;
        else j[k] = v.scalar;
    }
    return j;
}

void registerFrontmatter(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.frontmatter.get", "Get frontmatter",
             "Read a note's YAML frontmatter (Obsidian's properties): every key as JSON, or one key's value.",
             {vaultParam(), noteParam(), {"key", ParamType::String, "just this key (case-insensitive)", false}}, false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::string rel = needNote(v, p);
                const Note* n = v.note(rel);
                if (has(p, "key") && !stringParam(p, "key").empty()) {
                    const FmValue* fv = fmFind(n->frontmatter, stringParam(p, "key"));
                    if (!fv) {
                        Result r = Result::success(rel + " has no " + stringParam(p, "key"));
                        return r.with("found", false);
                    }
                    Result r = Result::success(fv->text(), fv->text());
                    r.with("found", true).with("value", fv->isList ? json(fv->list).dump() : fv->scalar).with("list", fv->isList);
                    return r;
                }
                std::string msg;
                for (const auto& [k, fv] : n->frontmatter) msg += (msg.empty() ? "" : "\n") + k + ": " + fv.text();
                if (msg.empty()) msg = rel + " has no frontmatter";
                Result r = Result::success(msg);
                return r.with("note", rel).with("keys", static_cast<std::int64_t>(n->frontmatter.size())).with("frontmatter", fmJson(n->frontmatter).dump());
            }));

    reg.add({"notes.frontmatter.set", "Set frontmatter",
             "Set, add to or remove one frontmatter key, touching only that key's lines (the rest of the YAML and the "
             "note stay byte for byte). A value like [a, b] or list=true makes a YAML list; append=true adds items to "
             "an existing list (e.g. another alias); delete=true removes the key. Creates the frontmatter if missing.",
             {vaultParam(), noteParam(), {"key", ParamType::String, "the key, e.g. status, tags, aliases", true},
              {"value", ParamType::String, "the value; for lists, items separated by commas", false},
              boolParamSpec("list", "store the value as a list"), boolParamSpec("append", "add the items to the key's list"),
              boolParamSpec("delete", "remove the key")},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                std::string rel = needNote(v, p);
                const std::string key = trim(stringParam(p, "key"));
                if (key.empty() || key.find(':') != std::string::npos) return Result::failure("give a key= without ':'");
                std::string text = v.read(rel);
                std::string out;
                if (boolParam(p, "delete")) {
                    if (!fmFind(v.note(rel)->frontmatter, key)) return Result::failure(rel + " has no " + key);
                    out = setFrontmatter(text, key, nullptr);
                } else {
                    if (!has(p, "value")) return Result::failure("give value= (or delete=true)");
                    std::string raw = stringParam(p, "value");
                    FmValue fv;
                    std::string t = trim(raw);
                    bool flow = t.size() >= 2 && t.front() == '[' && t.back() == ']' && !startsWith(t, "[[");
                    if (boolParam(p, "list") || boolParam(p, "append") || flow) {
                        fv.isList = true;
                        if (flow) t = t.substr(1, t.size() - 2);
                        std::string cur;
                        for (char c : t + ",") {
                            if (c == ',') {
                                std::string item = trim(cur);
                                if (item.size() >= 2 && (item.front() == '"' || item.front() == '\'') && item.back() == item.front()) item = item.substr(1, item.size() - 2);
                                if (!item.empty()) fv.list.push_back(item);
                                cur.clear();
                            } else {
                                cur += c;
                            }
                        }
                        if (boolParam(p, "append")) {
                            FmValue merged;
                            merged.isList = true;
                            if (const FmValue* old = fmFind(v.note(rel)->frontmatter, key)) {
                                merged.list = old->isList ? old->list : std::vector<std::string>{};
                                if (!old->isList && !old->scalar.empty()) merged.list.push_back(old->scalar);
                            }
                            for (const auto& item : fv.list) {
                                bool dup = false;
                                for (const auto& m : merged.list) dup = dup || lower(m) == lower(item);
                                if (!dup) merged.list.push_back(item);
                            }
                            fv = merged;
                        }
                    } else {
                        fv.scalar = raw;
                    }
                    out = setFrontmatter(text, key, &fv);
                }
                if (out == text) return Result::success(rel + ": " + key + " unchanged").with("note", rel).with("changed", false);
                txn.write(v.abs(rel), out);
                Parsed parsed = parseNote(out);
                const FmValue* now = fmFind(parsed.frontmatter, key);
                Result r = Result::success(rel + ": " + (now ? key + ": " + now->text() : "removed " + key), rel);
                return r.with("note", rel).with("changed", true);
            }));
}

void registerTags(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.tags", "List tags",
             "Every tag in the vault (inline #tags and frontmatter tags) with how many notes use it. nested=true also "
             "counts parent tags (#project/alpha counts for #project).",
             {vaultParam(), {"prefix", ParamType::String, "only tags starting with this", false},
              {"sort", ParamType::String, "order", false, {"count", "name"}, std::string("count")},
              boolParamSpec("nested", "add parent tags of nested tags"), limitParam(0), jsonParam("tag and count pairs")},
             false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::map<std::string, std::pair<std::string, std::size_t>> counts;  // lower -> (as written, notes)
                for (const auto& [rel, n] : v.notes) {
                    std::set<std::string> mine;
                    for (const auto& t : n.tags) {
                        mine.insert(t);
                        if (boolParam(p, "nested")) {
                            for (auto slash = t.find('/'); slash != std::string::npos; slash = t.find('/', slash + 1)) mine.insert(t.substr(0, slash));
                        }
                    }
                    std::set<std::string> seen;
                    for (const auto& t : mine) {
                        if (!seen.insert(lower(t)).second) continue;
                        auto& c = counts[lower(t)];
                        if (c.first.empty()) c.first = t;
                        ++c.second;
                    }
                }
                std::string prefix = lower(stringParam(p, "prefix"));
                while (!prefix.empty() && prefix[0] == '#') prefix.erase(0, 1);
                std::vector<std::pair<std::string, std::size_t>> list;
                for (const auto& [k, c] : counts) {
                    if (startsWith(k, prefix)) list.emplace_back(c.first, c.second);
                }
                if (stringParam(p, "sort", "count") == "count") {
                    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                }
                const std::size_t total = list.size();
                auto limit = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "limit", 0)));
                if (limit && list.size() > limit) list.resize(limit);
                std::string msg = plural(total, "tag");
                json items = json::array();
                for (const auto& [t, c] : list) {
                    msg += "\n#" + t + "  " + std::to_string(c);
                    items.push_back({{"tag", t}, {"notes", c}});
                }
                Result r = Result::success(msg);
                r.with("count", static_cast<std::int64_t>(total));
                if (boolParam(p, "json")) r.with("items", items.dump());
                return r;
            }));

    reg.add({"notes.tags.find", "Notes with tag",
             "The notes that carry a tag (nested tags included: project finds #project/alpha too), with where it appears.",
             {vaultParam(), {"tag", ParamType::String, "the tag, with or without #", true}, limitParam(200)}, false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::string tag = trim(stringParam(p, "tag"));
                std::vector<std::string> found;
                for (const auto& [rel, n] : v.notes) {
                    if (n.hasTag(tag)) found.push_back(rel);
                }
                auto limit = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "limit", 200)));
                std::string msg = plural(found.size(), "note") + " tagged #" + (tag.empty() || tag[0] != '#' ? tag : tag.substr(1));
                std::string all;
                for (std::size_t i = 0; i < found.size(); ++i) {
                    all += (all.empty() ? "" : "\n") + found[i];
                    if (limit && i >= limit) continue;
                    const Note* n = v.note(found[i]);
                    msg += "\n" + found[i] + (n->title != n->name ? "  \"" + n->title + "\"" : "");
                }
                return Result::success(msg).with("count", static_cast<std::int64_t>(found.size())).with("notes", all);
            }));

    reg.add({"notes.tags.rename", "Rename tag",
             "Rename a tag everywhere: inline #tags and frontmatter tags, nested tags too (#old/x becomes #new/x).",
             {vaultParam(), {"from", ParamType::String, "the tag to rename", true}, {"to", ParamType::String, "its new name", true}},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                std::size_t n = renameTag(v, txn, stringParam(p, "from"), stringParam(p, "to"));
                if (n == 0) return Result::failure("no note has the tag " + stringParam(p, "from"));
                return Result::success("renamed the tag in " + plural(n, "note")).with("notes_changed", static_cast<std::int64_t>(n));
            }));
}

void registerTemplates(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.templates.list", "List templates",
             "The notes in the templates folder (Obsidian's setting, default Templates/), with the variables each uses.",
             {vaultParam()}, false, true},
            reader(st, [](Vault& v, const Params&) {
                const std::string folder = v.settings.templatesFolder;
                std::string msg, all;
                std::size_t count = 0;
                for (const auto& [rel, n] : v.notes) {
                    if (folder.empty() || !startsWith(rel, folder + "/")) continue;
                    ++count;
                    std::set<std::string> vars;
                    std::string text = v.read(rel);
                    for (auto a = text.find("{{"); a != std::string::npos; a = text.find("{{", a + 2)) {
                        auto b = text.find("}}", a);
                        if (b == std::string::npos) break;
                        vars.insert(trim(text.substr(a + 2, b - a - 2)));
                    }
                    msg += "\n" + rel;
                    if (!vars.empty()) {
                        msg += "  uses";
                        for (const auto& x : vars) msg += " {{" + x + "}}";
                    }
                    all += (all.empty() ? "" : "\n") + rel;
                }
                msg = plural(count, "template") + " in " + folder + "/" + msg;
                return Result::success(msg).with("count", static_cast<std::int64_t>(count)).with("templates", all).with("folder", folder);
            }));

    reg.add({"notes.templates.render", "Render template",
             "Fill in a template without writing anything: shows what notes.create template= would produce.",
             {vaultParam(), {"template", ParamType::String, "the template's name or path", true},
              {"title", ParamType::String, "what {{title}} means", false, {}, std::string("Untitled")},
              {"vars", ParamType::String, "extra variables: \"client=Acme; budget=10k\"", false},
              {"today", ParamType::String, "the date {{date}} means (default: today)", false}},
             false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::string t = findTemplate(v, stringParam(p, "template"));
                if (t.empty()) return Result::failure("no template called '" + stringParam(p, "template") + "'");
                std::string out = expandTemplate(v.read(t), v, stringParam(p, "title", "Untitled"), baseDate(p), parseVars(stringParam(p, "vars")));
                return Result::success(out).with("template", t);
            }));

    reg.add({"notes.templates.insert", "Insert template",
             "Add a filled-in template to an existing note: its body at the end, and its frontmatter keys the note lacks.",
             {vaultParam(), noteParam(), {"template", ParamType::String, "the template's name or path", true},
              {"vars", ParamType::String, "extra variables", false}, {"today", ParamType::String, "the date {{date}} means", false}},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                std::string rel = needNote(v, p);
                std::string t = findTemplate(v, stringParam(p, "template"));
                if (t.empty()) return Result::failure("no template called '" + stringParam(p, "template") + "'");
                std::string expanded = expandTemplate(v.read(t), v, v.note(rel)->name, baseDate(p), parseVars(stringParam(p, "vars")), folderOf(rel));
                txn.write(v.abs(rel), mergeTemplate(v.read(rel), expanded));
                return Result::success("inserted " + t + " into " + rel, rel).with("note", rel);
            }));
}

void registerDaily(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.daily", "Daily note",
             "Open (and create when missing) the daily note for a date, using Obsidian's daily notes settings: folder, "
             "file name format (moment.js, default YYYY-MM-DD) and template, unless folder=, format= or template= say "
             "otherwise. {{date}} in the template is the note's date. append= adds a line straight away (journaling, "
             "quick capture), under heading= if given. Returns the note's path and text.",
             {vaultParam(), dateParam("which day"),
              {"folder", ParamType::String, "folder for daily notes (default: the vault's setting)", false},
              {"format", ParamType::String, "file name format (default: the vault's setting)", false},
              {"template", ParamType::String, "template for a new note (default: the vault's setting; \"\" for none)", false},
              {"vars", ParamType::String, "extra template variables", false},
              {"append", ParamType::String, "text to add to the note", false},
              {"heading", ParamType::String, "add append= under this heading", false},
              {"today", ParamType::String, "what today means (default: the clock)", false}},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                Date base = baseDate(p);
                auto d = parseDate(stringParam(p, "date", "today"), base);
                if (!d) return Result::failure("date= should look like 2026-10-16, today, yesterday, tomorrow, +3 or -7");
                Date day = *d;
                if (dayNumber(day) == dayNumber(base)) {
                    day.hour = base.hour;
                    day.minute = base.minute;
                    day.second = base.second;
                }
                bool created = false;
                std::string rel = ensureDaily(v, txn, day, p, created);
                std::string text;
                std::error_code ec;
                if (has(p, "append") && !stringParam(p, "append").empty()) {
                    std::string cur = fs::exists(v.abs(rel), ec) ? readText(v.abs(rel)) : "";
                    Params tp = p;
                    tp["content"] = stringParam(p, "append");
                    text = appendText(cur, textParam(tp, "content"), trim(stringParam(p, "heading")), false);
                    txn.write(v.abs(rel), text);
                } else {
                    text = readText(v.abs(rel));
                }
                Result r = Result::success((created ? "created " : "") + rel + "\n" + text, rel);
                return r.with("note", rel).with("created", created).with("date", isoDate(day));
            }));
}

void registerOutline(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.outline", "Note outline",
             "A note's headings as an indented outline with line numbers (and heading anchors for links).",
             {vaultParam(), noteParam(), jsonParam("level, text, line and anchor of each heading")}, false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::string rel = needNote(v, p);
                const Note* n = v.note(rel);
                std::string msg = rel + ": " + plural(n->headings.size(), "heading");
                json items = json::array();
                int minLevel = 6;
                for (const auto& h : n->headings) minLevel = std::min(minLevel, h.level);
                for (const auto& h : n->headings) {
                    msg += "\n" + std::string(static_cast<std::size_t>(2 * (h.level - minLevel)), ' ') + std::string(static_cast<std::size_t>(h.level), '#') + " " + h.text + "  (line " + std::to_string(h.line) + ")";
                    items.push_back({{"level", h.level}, {"text", h.text}, {"line", h.line}, {"anchor", slugify(h.text)}});
                }
                Result r = Result::success(msg);
                r.with("note", rel).with("count", static_cast<std::int64_t>(n->headings.size()));
                if (boolParam(p, "json")) r.with("items", items.dump());
                return r;
            }));
}

}  // namespace

void registerMetaCommands(CommandRegistry& reg, const StatePtr& st) {
    registerFrontmatter(reg, st);
    registerTags(reg, st);
    registerTemplates(reg, st);
    registerDaily(reg, st);
    registerOutline(reg, st);
}

}  // namespace wetwaresuite::notes::detail
