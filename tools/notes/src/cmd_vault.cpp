// notes.vault.*, notes.list and notes.trash.*
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::notes::detail {

using nlohmann::json;

namespace {

std::string isoTime(std::int64_t t) {
    std::int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    std::int64_t secs = t - days * 86400;
    Date d = addDays(Date{1970, 1, 1, 0, 0, 0}, static_cast<int>(days));
    d.hour = static_cast<int>(secs / 3600);
    d.minute = static_cast<int>(secs / 60 % 60);
    return formatDate(d, "YYYY-MM-DD HH:mm");
}

void attachGraph(Result& r, const Vault& v) {
    Graph g = buildGraph(v, {});
    layoutGraph(g, g.nodes.size() > 1500 ? 40 : 0);
    r.attach({"image/png", raster::encodePng(drawGraph(g, 900, 650, -1)), "graph.png"});
}

Result vaultSummary(const std::string& verb, const std::string& id, Vault& v) {
    std::size_t open = 0, done = 0;
    for (const auto& [rel, n] : v.notes) {
        for (const auto& t : n.tasks) (t.done ? done : open)++;
    }
    Result r = Result::success(verb + " " + id + ": " + fromPath(v.root) + " (" + plural(v.notes.size(), "note") + ", " +
                               plural(v.files.size(), "attachment") + ", " + plural(open, "open task") + ")");
    r.with("doc", id)
        .with("path", fromPath(v.root))
        .with("notes", static_cast<std::int64_t>(v.notes.size()))
        .with("attachments", static_cast<std::int64_t>(v.files.size()))
        .with("open_tasks", static_cast<std::int64_t>(open));
    return r;
}

std::string trashRoot(const Vault& v) { return fromPath(v.root / kTrashDir); }

std::vector<std::string> trashFiles(const Vault& v) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path t = v.root / kTrashDir;
    if (!fs::is_directory(t, ec)) return out;
    for (fs::recursive_directory_iterator it(t, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) out.push_back(fromPath(it->path().lexically_relative(t)));
    }
    std::sort(out.begin(), out.end());
    return out;
}

void registerOpen(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.vault.open", "Open vault",
             "Open a folder of Markdown notes (an Obsidian vault or any folder of .md files) as a vault document, e.g. "
             "vault1. Every other notes.* command works on it (vault= picks one when several are open; a folder path "
             "works too). The files stay the source of truth: what the tool learns from them is cached in a hidden "
             ".wwnotes folder in the vault and checked against each file's size and time on every command, so edits "
             "made in Obsidian or any editor show up straight away. Hidden folders (.obsidian, .trash, .git) are skipped.",
             {{"path", ParamType::String, "the vault folder", true}, boolParamSpec("preview", "attach a picture of the link graph")},
             false, true},
            [st](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([st, p]() -> Result {
                    Params q = p;
                    q["vault"] = stringParam(p, "path");
                    std::string error, id;
                    auto doc = getVault(*st, q, error, &id);
                    if (!doc) return Result::failure(error);
                    Result r = vaultSummary("opened", id, doc->vault);
                    if (boolParam(p, "preview")) attachGraph(r, doc->vault);
                    return r;
                });
            });

    reg.add({"notes.vault.create", "Create vault",
             "Make a new vault folder (with an .obsidian folder so Obsidian and notes.* find it) and open it. "
             "An existing folder is just opened, and given an .obsidian folder if it has none.",
             {{"path", ParamType::String, "the folder to create", true},
              {"daily_folder", ParamType::String, "folder for daily notes (default: the vault root, like Obsidian)", false},
              {"templates_folder", ParamType::String, "folder for templates", false, {}, std::string("Templates")}},
             false, false},
            [st](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([st, p]() -> Result {
                    fs::path dir = toPath(stringParam(p, "path"));
                    std::error_code ec;
                    fs::create_directories(dir / ".obsidian", ec);
                    if (ec) return Result::failure("can't make " + fromPath(dir) + ": " + ec.message());
                    auto writeJson = [&](const char* name, const json& j) {
                        fs::path f = dir / ".obsidian" / name;
                        if (fs::exists(f, ec)) return;
                        std::ofstream out(f, std::ios::binary);
                        out << j.dump(2);
                    };
                    writeJson("daily-notes.json", {{"folder", stringParam(p, "daily_folder")}, {"format", "YYYY-MM-DD"}, {"template", ""}});
                    writeJson("templates.json", {{"folder", stringParam(p, "templates_folder", "Templates")}});
                    Params q;
                    q["vault"] = fromPath(dir);
                    std::string error, id;
                    auto doc = getVault(*st, q, error, &id);
                    if (!doc) return Result::failure(error);
                    return vaultSummary("created", id, doc->vault);
                });
            });

    reg.add({"notes.vault.info", "Vault info",
             "What is in the vault: notes, attachments, words, links (and how many go nowhere), tags, open and done "
             "tasks, folders, the most linked notes and the settings read from .obsidian (daily notes folder and "
             "format, templates folder).",
             {vaultParam(), boolParamSpec("preview", "attach a picture of the link graph")}, false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::size_t words = 0, links = 0, unresolved = 0, open = 0, done = 0;
                std::set<std::string> tags, folders;
                for (const auto& [rel, n] : v.notes) {
                    words += n.words;
                    links += n.links.size();
                    for (const auto& l : n.links) {
                        if (v.resolve(l, rel).empty()) ++unresolved;
                    }
                    for (const auto& t : n.tags) tags.insert(lower(t));
                    for (const auto& t : n.tasks) (t.done ? done : open)++;
                    if (!folderOf(rel).empty()) folders.insert(folderOf(rel));
                }
                std::vector<std::pair<std::size_t, std::string>> linked;
                for (const auto& [rel, n] : v.notes) linked.emplace_back(v.backlinks(rel).size(), rel);
                std::sort(linked.begin(), linked.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
                std::string msg = fromPath(v.root) + "\n  " + plural(v.notes.size(), "note") + ", " + plural(v.files.size(), "attachment") +
                                  ", " + plural(folders.size(), "folder") + ", " + std::to_string(words) + " words\n  " +
                                  plural(links, "link") + " (" + std::to_string(unresolved) + " unresolved), " + plural(tags.size(), "tag") +
                                  "\n  tasks: " + std::to_string(open) + " open, " + std::to_string(done) + " done";
                if (!linked.empty() && linked[0].first > 0) {
                    msg += "\n  most linked:";
                    for (std::size_t i = 0; i < linked.size() && i < 5 && linked[i].first > 0; ++i) {
                        msg += " " + withoutMd(linked[i].second) + " (" + std::to_string(linked[i].first) + ")";
                        if (i + 1 < 5 && i + 1 < linked.size() && linked[i + 1].first > 0) msg += ",";
                    }
                }
                msg += "\n  daily notes: " + (v.settings.dailyFolder.empty() ? std::string("vault root") : v.settings.dailyFolder + "/") + ", format " +
                       v.settings.dailyFormat + (v.settings.dailyTemplate.empty() ? "" : ", template " + v.settings.dailyTemplate) +
                       "\n  templates: " + v.settings.templatesFolder + "/";
                Result r = Result::success(msg);
                r.with("path", fromPath(v.root))
                    .with("notes", static_cast<std::int64_t>(v.notes.size()))
                    .with("attachments", static_cast<std::int64_t>(v.files.size()))
                    .with("folders", static_cast<std::int64_t>(folders.size()))
                    .with("words", static_cast<std::int64_t>(words))
                    .with("links", static_cast<std::int64_t>(links))
                    .with("unresolved", static_cast<std::int64_t>(unresolved))
                    .with("tags", static_cast<std::int64_t>(tags.size()))
                    .with("open_tasks", static_cast<std::int64_t>(open))
                    .with("done_tasks", static_cast<std::int64_t>(done))
                    .with("daily_folder", v.settings.dailyFolder)
                    .with("daily_format", v.settings.dailyFormat)
                    .with("templates_folder", v.settings.templatesFolder);
                if (boolParam(p, "preview")) attachGraph(r, v);
                return r;
            }));

    reg.add({"notes.vault.configure", "Configure vault",
             "Set where daily notes and templates live, in Obsidian's own settings files (.obsidian/daily-notes.json "
             "and templates.json), so Obsidian and these commands agree. Only the settings given change.",
             {vaultParam(),
              {"daily_folder", ParamType::String, "folder for daily notes (\"\" = the vault root)", false},
              {"daily_format", ParamType::String, "daily note file name format, e.g. YYYY-MM-DD or YYYY/MM/YYYY-MM-DD dddd", false},
              {"daily_template", ParamType::String, "template for new daily notes, e.g. Templates/Daily", false},
              {"templates_folder", ParamType::String, "folder holding templates", false},
              {"date_format", ParamType::String, "format of {{date}} in templates", false},
              {"time_format", ParamType::String, "format of {{time}} in templates", false}},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                auto update = [&](const char* file, const std::vector<std::pair<std::string, std::string>>& keys) {
                    bool any = false;
                    for (const auto& [param, key] : keys) any = any || has(p, param);
                    if (!any) return;
                    fs::path f = v.root / ".obsidian" / file;
                    json j = json::object();
                    std::error_code ec;
                    if (fs::exists(f, ec)) {
                        try {
                            j = json::parse(readText(f));
                        } catch (const std::exception&) {
                            j = json::object();
                        }
                    }
                    for (const auto& [param, key] : keys) {
                        if (has(p, param)) j[key] = stringParam(p, param);
                    }
                    txn.write(f, j.dump(2));
                };
                update("daily-notes.json", {{"daily_folder", "folder"}, {"daily_format", "format"}, {"daily_template", "template"}});
                update("templates.json", {{"templates_folder", "folder"}, {"date_format", "dateFormat"}, {"time_format", "timeFormat"}});
                if (txn.empty()) return Result::failure("nothing to change: give daily_folder=, daily_format=, daily_template=, templates_folder=, date_format= or time_format=");
                v.refresh();
                Result r = Result::success("daily notes: " + (v.settings.dailyFolder.empty() ? std::string("vault root") : v.settings.dailyFolder + "/") +
                                           " as " + v.settings.dailyFormat + (v.settings.dailyTemplate.empty() ? "" : " from " + v.settings.dailyTemplate) +
                                           "; templates: " + v.settings.templatesFolder + "/");
                return r.with("daily_folder", v.settings.dailyFolder).with("daily_format", v.settings.dailyFormat).with("templates_folder", v.settings.templatesFolder);
            }));

    reg.add({"notes.vault.reindex", "Rebuild index",
             "Delete the vault's .wwnotes cache and read every note again. Never needed for correctness (files are "
             "checked on every command); useful after copying a vault with changed times, or to measure a full read.",
             {vaultParam()}, false, false},
            reader(st, [](Vault& v, const Params&) {
                auto t0 = std::chrono::steady_clock::now();
                v.clearCache();
                fs::path root = v.root;
                v.open(root, false);
                double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.2f", secs);
                return Result::success("read " + plural(v.notes.size(), "note") + " in " + buf + " s").with("notes", static_cast<std::int64_t>(v.notes.size())).with("seconds", secs);
            }));
}

void registerList(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.list", "List notes",
             "List the notes in the vault (or a folder of it), with title, word count, tags and when each changed.",
             {vaultParam(),
              {"folder", ParamType::String, "only notes in this folder (and below)", false},
              {"tag", ParamType::String, "only notes with this tag (nested tags count: project matches project/alpha)", false},
              {"sort", ParamType::String, "order", false, {"path", "name", "modified", "-modified", "words", "-words", "title"}, std::string("path")},
              limitParam(200), jsonParam("each note's path, title, tags, words and modified time")},
             false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::string folder = trim(stringParam(p, "folder"));
                while (!folder.empty() && folder.back() == '/') folder.pop_back();
                const std::string tag = stringParam(p, "tag");
                std::vector<const Note*> list;
                for (const auto& [rel, n] : v.notes) {
                    if (!folder.empty() && !startsWith(lower(rel), lower(folder) + "/")) continue;
                    if (!tag.empty() && !n.hasTag(tag)) continue;
                    list.push_back(&n);
                }
                const std::string sort = stringParam(p, "sort", "path");
                std::stable_sort(list.begin(), list.end(), [&](const Note* a, const Note* b) {
                    if (sort == "name") return lower(a->name) < lower(b->name);
                    if (sort == "title") return lower(a->title) < lower(b->title);
                    if (sort == "modified") return a->mtime < b->mtime;
                    if (sort == "-modified") return a->mtime > b->mtime;
                    if (sort == "words") return a->words < b->words;
                    if (sort == "-words") return a->words > b->words;
                    return a->rel < b->rel;
                });
                const std::size_t total = list.size();
                auto limit = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "limit", 200)));
                if (limit && list.size() > limit) list.resize(limit);
                std::string msg = plural(total, "note") + (list.size() < total ? ", showing " + std::to_string(list.size()) : "");
                json items = json::array();
                for (const Note* n : list) {
                    msg += "\n" + n->rel + (n->title != n->name ? "  \"" + n->title + "\"" : "") + "  " + std::to_string(n->words) + " words  " + isoTime(n->mtime);
                    for (const auto& t : n->tags) msg += " #" + t;
                    items.push_back({{"path", n->rel}, {"title", n->title}, {"tags", n->tags}, {"words", n->words}, {"modified", isoTime(n->mtime)}});
                }
                Result r = Result::success(msg);
                r.with("count", static_cast<std::int64_t>(list.size())).with("total", static_cast<std::int64_t>(total));
                if (boolParam(p, "json")) r.with("items", items.dump());
                return r;
            }));
}

void registerTrash(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.trash.list", "List trash",
             "Files in the vault's .trash folder (where notes.delete puts notes), as paths inside it.",
             {vaultParam()}, false, true},
            reader(st, [](Vault& v, const Params&) {
                auto files = trashFiles(v);
                std::string msg = plural(files.size(), "file") + " in " + trashRoot(v);
                std::string all;
                for (const auto& f : files) {
                    msg += "\n" + f;
                    all += (all.empty() ? "" : "\n") + f;
                }
                return Result::success(msg).with("count", static_cast<std::int64_t>(files.size())).with("files", all);
            }));

    reg.add({"notes.trash.restore", "Restore from trash",
             "Move a file from .trash back into the vault, to where it was (the trash keeps folder paths) or to `to`.",
             {vaultParam(), {"path", ParamType::String, "the file inside .trash, as notes.trash.list shows it", true},
              {"to", ParamType::String, "where to put it in the vault (default: its old place)", false}},
             true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params& p) {
                std::string path = trim(stringParam(p, "path"));
                for (auto& c : path) {
                    if (c == '\\') c = '/';
                }
                if (startsWith(path, std::string(kTrashDir) + "/")) path = path.substr(std::string(kTrashDir).size() + 1);
                fs::path src = v.root / kTrashDir / toPath(path);
                std::error_code ec;
                if (!fs::is_regular_file(src, ec)) return Result::failure("no " + path + " in the trash (see notes.trash.list)");
                std::string to = has(p, "to") ? trim(stringParam(p, "to")) : path;
                // drop a " 2" style suffix added when the trash already held that name
                if (!has(p, "to")) {
                    std::string stem = withoutMd(to);
                    auto sp = stem.rfind(' ');
                    if (sp != std::string::npos && stem.find_first_not_of("0123456789", sp + 1) == std::string::npos && sp + 1 < stem.size()) {
                        std::string base = stem.substr(0, sp) + (endsWith(lower(to), ".md") ? ".md" : "");
                        if (!fs::exists(v.abs(base), ec)) to = base;
                    }
                }
                if (fs::exists(v.abs(to), ec)) return Result::failure(to + " already exists in the vault: pass to=<another path>");
                txn.move(src, v.abs(to));
                return Result::success("restored " + to, to).with("note", to);
            }));

    reg.add({"notes.trash.empty", "Empty trash",
             "Delete everything in the vault's .trash folder for good (undo in the same session still brings it back).",
             {vaultParam()}, true, false},
            editor(st, [](Vault& v, FileTxn& txn, const Params&) {
                auto files = trashFiles(v);
                for (const auto& f : files) txn.remove(v.root / kTrashDir / toPath(f));
                std::error_code ec;
                fs::path t = v.root / kTrashDir;
                // remove now-empty folders inside the trash
                for (int pass = 0; pass < 8; ++pass) {
                    bool removed = false;
                    for (fs::recursive_directory_iterator it(t, ec), end; !ec && it != end; it.increment(ec)) {
                        if (it->is_directory(ec) && fs::is_empty(it->path(), ec)) {
                            fs::remove(it->path(), ec);
                            removed = true;
                            break;
                        }
                    }
                    if (!removed) break;
                }
                return Result::success("deleted " + plural(files.size(), "file") + " from the trash").with("deleted", static_cast<std::int64_t>(files.size()));
            }));
}

}  // namespace

void registerVaultCommands(CommandRegistry& reg, const StatePtr& st) {
    registerOpen(reg, st);
    registerList(reg, st);
    registerTrash(reg, st);
}

}  // namespace wetwaresuite::notes::detail
