#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

#include "achclwss/core/cli.hpp"
#include "achclwss/mcp/server.hpp"
#include "achclwss/notes/notes.hpp"
#include "check.hpp"

using namespace achclwss;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::int64_t intField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

std::string strField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : "";
}

void writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

json call(mcp::Server& server, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
}

std::string markdownToHtmlCrlf() { return notes::markdownToHtml("# T\r\n\r\n- a\r\n- b\r\n"); }

void unitTests() {
    using namespace notes;
    // dates
    Date d{2026, 10, 16, 9, 5, 0};
    CHECK(formatDate(d, "YYYY-MM-DD") == "2026-10-16");
    CHECK(formatDate(d, "dddd, MMMM Do YYYY") == "Friday, October 16th 2026");
    CHECK(formatDate(d, "gggg-[W]WW") == "2026-W42");
    CHECK(formatDate(d, "HH:mm") == "09:05");
    CHECK(formatDate(d, "[Week] W") == "Week 42");
    CHECK(isoDate(*parseDate("tomorrow", d)) == "2026-10-17");
    CHECK(isoDate(*parseDate("-16", d)) == "2026-09-30");
    CHECK(isoDate(*parseDate("2024-02-29", d)) == "2024-02-29");
    CHECK(!parseDate("next blue moon", d));
    CHECK(weekday(Date{2026, 10, 19}) == 0);  // a Monday

    // tokens and slugs
    auto toks = tokenize("Hello, World! naïve-test 42");
    CHECK(toks.size() == 5 && toks[0] == "hello" && toks[2] == "na\xC3\xAFve" && toks[4] == "42");
    CHECK(slugify("My Heading: Part 2!") == "my-heading-part-2");

    // frontmatter
    const std::string note = "---\ntitle: \"A: B\"\ntags: [one, two]\naliases:\n  - First\n  - Second\nnested:\n  a: 1\n---\nBody #three\n";
    std::string yaml;
    std::size_t bodyStart = 0;
    CHECK(splitFrontmatter(note, &yaml, &bodyStart));
    auto fm = parseFrontmatter(yaml);
    CHECK(fm.size() == 4);
    CHECK(fmFind(fm, "title")->scalar == "A: B");
    CHECK(fmFind(fm, "TAGS")->isList && fmFind(fm, "tags")->list.size() == 2);
    CHECK(frontmatterAliases(fm).size() == 2 && frontmatterAliases(fm)[1] == "Second");
    CHECK(note.substr(bodyStart) == "Body #three\n");
    FmValue status;
    status.scalar = "in progress";
    std::string set = setFrontmatter(note, "status", &status);
    CHECK(contains(set, "status: in progress\n---\nBody"));
    CHECK(contains(set, "nested:\n  a: 1\n"));  // untouched
    FmValue tags;
    tags.isList = true;
    tags.list = {"x", "y: z"};
    std::string set2 = setFrontmatter(set, "tags", &tags);
    CHECK(contains(set2, "tags: [x, \"y: z\"]\naliases:"));  // a flow list stays a flow list
    CHECK(contains(setFrontmatter("---\na: 1\n---\n", "k", &tags), "k:\n  - x\n  - \"y: z\"\n"));
    CHECK(!contains(setFrontmatter(set2, "aliases", nullptr), "First"));
    CHECK(setFrontmatter("Just text\n", "a", &status) == "---\na: in progress\n---\nJust text\n");

    // one note's structure
    const std::string text =
        "---\nrelated: \"[[Gamma]]\"\n---\n# Title\n\nSee [[Alpha|the alpha]] and [[Beta#Part]] and ![[pic.png|200]].\n"
        "A [md link](Sub/Delta.md#x) and [web](https://example.com) #tag1 #nested/tag2 not#tag #123\n"
        "```\n[[NotALink]] #notatag\n# not a heading\n```\n"
        "`[[inline code]]` %%[[comment]]%%\n"
        "## Tasks\n- [ ] open one \xF0\x9F\x93\x85 2026-10-20\n- [x] done one\n  - [ ] nested due:2026-11-01\n"
        "| a | b |\n|---|---|\n| [[T1\\|alias]] | 2 |\n";
    Parsed p = parseNote(text);
    CHECK(p.hasFrontmatter);
    std::vector<std::string> targets;
    for (const auto& l : p.links) targets.push_back(l.target);
    CHECK((targets == std::vector<std::string>{"Gamma", "Alpha", "Beta", "pic.png", "Sub/Delta.md", "T1"}));
    CHECK(p.links[1].alias == "the alpha" && p.links[2].heading == "Part" && p.links[3].embed && !p.links[4].wiki);
    CHECK(p.links[4].heading == "x" && p.links[5].escapedPipe && p.links[5].alias == "alias");
    CHECK(text.substr(p.links[1].begin, p.links[1].end - p.links[1].begin) == "[[Alpha|the alpha]]");
    CHECK(p.inlineTags.size() == 2 && p.inlineTags[0].tag == "tag1" && p.inlineTags[1].tag == "nested/tag2");
    CHECK(p.headings.size() == 2 && p.headings[0].text == "Title" && p.headings[1].level == 2);
    CHECK(p.tasks.size() == 3 && !p.tasks[0].done && p.tasks[0].due == "2026-10-20" && p.tasks[1].done && p.tasks[2].due == "2026-11-01");
    auto sec = sectionRange(text, "Tasks");
    CHECK(sec && startsWith(text.substr(sec->first), "## Tasks") && sec->second == text.size());

    // markdown to HTML
    std::string html = markdownToHtml(
        "# Head *one*\n\nPara with **bold**, _em_, ~~gone~~, ==mark==, `x<y` and a [link](https://a.b \"T\").\n"
        "Second line\n\n- one\n- two\n  - deep\n\n1. first\n2. second\n\n- [ ] todo\n- [x] done\n\n"
        "> [!warning] Careful\n> Inside **callout**\n\n> plain quote\n\n```js\nlet a = 1 < 2;\n```\n\n"
        "| L | R |\n|:--|--:|\n| a | b |\n\n---\n\n[[Some Note#Sec|Alias]] and ![[img.png]] and #tag and <span>raw</span> & &amp; \\*lit\\*\n"
        "%% hidden %%\nhttps://example.org/x.\n");
    CHECK(contains(html, "<h1 id=\"head-one\">Head <em>one</em></h1>"));
    CHECK(contains(html, "<strong>bold</strong>") && contains(html, "<em>em</em>") && contains(html, "<del>gone</del>") && contains(html, "<mark>mark</mark>"));
    CHECK(contains(html, "<code>x&lt;y</code>") && contains(html, "<a href=\"https://a.b\" title=\"T\">link</a>"));
    CHECK(contains(html, "<br>\nSecond line"));
    CHECK(contains(html, "<ul>\n<li>one</li>\n<li>two\n<ul>\n<li>deep</li>\n</ul></li>\n</ul>"));
    CHECK(contains(html, "<ol>\n<li>first</li>"));
    CHECK(contains(html, "<li class=\"task\"><input type=\"checkbox\" disabled> todo</li>") && contains(html, "checked> done"));
    CHECK(contains(html, "<div class=\"callout callout-warning\"><div class=\"callout-title\">Careful</div>"));
    CHECK(contains(html, "<blockquote>\n<p>plain quote</p>\n</blockquote>"));
    CHECK(contains(html, "<pre><code class=\"language-js\">let a = 1 &lt; 2;\n</code></pre>"));
    CHECK(contains(html, "<th style=\"text-align:left\">L</th><th style=\"text-align:right\">R</th>"));
    CHECK(contains(html, "<hr>"));
    CHECK(contains(html, "<a class=\"internal-link\" href=\"Some%20Note.html#sec\">Alias</a>"));
    CHECK(contains(html, "<img src=\"img.png\""));
    CHECK(contains(html, "<span class=\"tag\">#tag</span>") && contains(html, "<span>raw</span>") && contains(html, "&amp; &amp; *lit*"));
    CHECK(!contains(html, "hidden"));
    CHECK(contains(html, "<a href=\"https://example.org/x\">https://example.org/x</a>."));
    CHECK(contains(markdownToHtml("Title\n===\n\ntext"), "<h1 id=\"title\">Title</h1>"));
    CHECK(contains(markdownToHtml("    code block\n"), "<pre><code>code block\n</code></pre>"));
}

}  // namespace

int main() {
    unitTests();

    std::mt19937_64 rng(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path dir = fs::temp_directory_path() / ("achclwss_notes_" + std::to_string(rng() % 1000000000));
    fs::remove_all(dir);
    const fs::path vault = dir / "vault";
    writeText(vault / "Home.md",
              "---\ntitle: Home base\ntags: [hub]\naliases: [Start]\n---\n# Welcome\n\nLinks: [[Project Alpha]], [[Projects/Beta|the beta]], "
              "[[Missing]] and ![[assets/pic.png]].\n\n- [ ] Water plants \xF0\x9F\x93\x85 2026-10-10\n- [x] Pay rent\n");
    writeText(vault / "Projects" / "Project Alpha.md",
              "# Project Alpha\n\nBudget planning for the next quarter. #work #project/alpha\n\n## Tasks\n- [ ] Draft budget due:2026-10-20\n"
              "- [ ] Call Sam\n\n## Notes\nBack to [[Home]] and [Beta](Beta.md). ![[Beta#Summary]]\n");
    writeText(vault / "Projects" / "Beta.md", "# Beta\n\n## Summary\nBeta is the second project, about the garden budget.\n\n## Other\nMore text about Home base.\n");
    writeText(vault / "Loose.md", "Nothing links here. The quarterly budget is tight.\n");
    writeText(vault / "Templates" / "Daily.md", "---\ncreated: {{date}}\n---\n# {{date:dddd D MMMM YYYY}}\n\n## Log\n\n## Todo\n");
    writeText(vault / "Templates" / "Meeting.md", "# {{title}}\n\nClient: {{client}}\nDate: {{date}} {{time}}\n");
    writeText(vault / "assets" / "pic.png", "\x89PNG\r\n\x1a\nnot really");
    writeText(vault / ".obsidian" / "daily-notes.json", R"({"folder": "Journal", "format": "YYYY-MM-DD", "template": "Templates/Daily"})");
    writeText(vault / ".git" / "ignored.md", "should not be seen");

    Session s;
    notes::registerCommands(s);
    auto run = [&](const std::string& id, Params p) { return s.run(id, p); };

    // ---- open and info ----
    Result open = run("notes.vault.open", {{"path", vault.string()}, {"preview", true}});
    CHECK(open.ok && strField(open, "doc") == "vault1" && intField(open, "notes") == 6 && intField(open, "attachments") == 1);
    CHECK(open.attachments.size() == 1 && open.attachments[0].mime == "image/png");
    CHECK(fs::exists(vault / ".achnotes" / "cache.json"));
    Result info = run("notes.vault.info", {});
    CHECK(info.ok && intField(info, "unresolved") == 1 && intField(info, "open_tasks") == 3 && intField(info, "done_tasks") == 1);
    CHECK(strField(info, "daily_folder") == "Journal" && strField(info, "templates_folder") == "Templates");

    // ---- list and read ----
    CHECK(intField(run("notes.list", {{"folder", std::string("Projects")}}), "total") == 2);
    CHECK(intField(run("notes.list", {{"tag", std::string("project")}}), "total") == 1);
    Result read = run("notes.read", {{"note", std::string("Start")}});  // an alias
    CHECK(read.ok && strField(read, "note") == "Home.md" && contains(read.message, "Water plants"));
    Result section = run("notes.read", {{"note", std::string("Beta")}, {"heading", std::string("Summary")}});
    CHECK(section.ok && contains(section.message, "second project") && !contains(section.message, "More text"));
    CHECK(!run("notes.read", {{"note", std::string("Nope")}}).ok);

    // ---- search ----
    Result sr = run("notes.search", {{"query", std::string("budget")}});
    CHECK(sr.ok && intField(sr, "total") == 3 && (contains(sr.message, "**budget**") || contains(sr.message, "**Budget**")));
    CHECK(intField(run("notes.search", {{"query", std::string("budget -garden")}}), "total") == 2);
    CHECK(intField(run("notes.search", {{"query", std::string("\"garden budget\"")}}), "total") == 1);
    CHECK(intField(run("notes.search", {{"query", std::string("\"budget garden\"")}}), "total") == 0);
    CHECK(intField(run("notes.search", {{"query", std::string("budget path:projects")}}), "total") == 2);
    CHECK(intField(run("notes.search", {{"query", std::string("tag:work")}}), "total") == 1);
    CHECK(intField(run("notes.search", {{"query", std::string("budget -tag:work")}}), "total") == 2);
    CHECK(intField(run("notes.search", {{"query", std::string("quart*")}}), "total") == 2);
    Result ranked = run("notes.search", {{"query", std::string("alpha")}});
    CHECK(contains(ranked.message.substr(0, ranked.message.find('\n', ranked.message.find('\n') + 1)), "Project Alpha.md"));

    // ---- links ----
    Result bl = run("notes.backlinks", {{"note", std::string("Home")}, {"mentions", true}});
    CHECK(bl.ok && intField(bl, "count") == 1 && intField(bl, "mentions") == 1);  // Beta names "Home base" without a link
    CHECK(intField(run("notes.backlinks", {{"note", std::string("Projects/Beta.md")}}), "count") == 3);
    CHECK(intField(run("notes.links", {{"note", std::string("Home")}}), "unresolved") == 1);
    Result un = run("notes.links.unresolved", {});
    CHECK(un.ok && intField(un, "count") == 1 && contains(un.message, "Missing  <- Home.md"));
    Result orph = run("notes.orphans", {});
    CHECK(orph.ok && intField(orph, "count") == 1 && strField(orph, "notes") == "Loose.md");

    // ---- rename rewrites links, undo restores ----
    const std::string homeBefore = readText(vault / "Home.md");
    const std::string alphaBefore = readText(vault / "Projects" / "Project Alpha.md");
    Result ren = run("notes.rename", {{"note", std::string("Beta")}, {"to", std::string("Gamma")}});
    CHECK(ren.ok && strField(ren, "note") == "Projects/Gamma.md" && intField(ren, "links_updated") == 3);
    CHECK(contains(readText(vault / "Home.md"), "[[Projects/Gamma|the beta]]"));
    CHECK(contains(readText(vault / "Projects" / "Project Alpha.md"), "[Beta](Gamma.md). ![[Gamma#Summary]]"));
    CHECK(fs::exists(vault / "Projects" / "Gamma.md") && !fs::exists(vault / "Projects" / "Beta.md"));
    CHECK(intField(run("notes.backlinks", {{"note", std::string("Gamma")}}), "count") == 3);
    CHECK(s.history().undo());
    CHECK(readText(vault / "Home.md") == homeBefore && readText(vault / "Projects" / "Project Alpha.md") == alphaBefore);
    CHECK(fs::exists(vault / "Projects" / "Beta.md") && !fs::exists(vault / "Projects" / "Gamma.md"));
    CHECK(s.history().redo() && fs::exists(vault / "Projects" / "Gamma.md"));
    CHECK(s.history().undo());

    // move to another folder: relative markdown links inside it are fixed
    Result mv = run("notes.move", {{"note", std::string("Project Alpha")}, {"folder", std::string("Archive/2026")}});
    CHECK(mv.ok && fs::exists(vault / "Archive" / "2026" / "Project Alpha.md"));
    CHECK(contains(readText(vault / "Archive" / "2026" / "Project Alpha.md"), "[Beta](../../Projects/Beta.md)"));
    CHECK(intField(run("notes.backlinks", {{"note", std::string("Beta")}}), "count") == 3);
    CHECK(s.history().undo() && !fs::exists(vault / "Archive"));  // the folders it made go too
    CHECK(readText(vault / "Projects" / "Project Alpha.md") == alphaBefore);

    // ---- delete to trash, restore ----
    Result del = run("notes.delete", {{"note", std::string("Loose")}});
    CHECK(del.ok && fs::exists(vault / ".trash" / "Loose.md") && !fs::exists(vault / "Loose.md"));
    CHECK(intField(run("notes.trash.list", {}), "count") == 1);
    CHECK(run("notes.trash.restore", {{"path", std::string("Loose.md")}}).ok && fs::exists(vault / "Loose.md"));
    CHECK(s.history().undo() && s.history().undo() && fs::exists(vault / "Loose.md") && !fs::exists(vault / ".trash" / "Loose.md"));

    // ---- create from template, write, append ----
    Result cr = run("notes.create", {{"note", std::string("Meetings/Acme kickoff")}, {"template", std::string("Meeting")},
                                     {"vars", std::string("client=Acme Ltd")}, {"today", std::string("2026-10-16 14:30")}});
    CHECK(cr.ok && strField(cr, "note") == "Meetings/Acme kickoff.md");
    CHECK(readText(vault / "Meetings" / "Acme kickoff.md") == "# Acme kickoff\n\nClient: Acme Ltd\nDate: 2026-10-16 14:30\n");
    CHECK(!run("notes.create", {{"note", std::string("Meetings/Acme kickoff")}}).ok);  // exists
    CHECK(run("notes.append", {{"note", std::string("Acme kickoff")}, {"content", std::string("- decided: ship\\n- next: plan")}, {"heading", std::string("Decisions")}}).ok);
    CHECK(contains(readText(vault / "Meetings" / "Acme kickoff.md"), "\n## Decisions\n\n- decided: ship\n- next: plan\n"));
    CHECK(run("notes.append", {{"note", std::string("Acme kickoff")}, {"content", std::string("- third")}, {"heading", std::string("Decisions")}}).ok);
    CHECK(contains(readText(vault / "Meetings" / "Acme kickoff.md"), "- next: plan\n- third\n"));
    CHECK(run("notes.write", {{"note", std::string("Scratch")}, {"content", std::string("hello")}}).ok && readText(vault / "Scratch.md") == "hello\n");
    CHECK(s.history().undo() && !fs::exists(vault / "Scratch.md"));

    // ---- daily notes ----
    Result daily = run("notes.daily", {{"today", std::string("2026-10-16")}, {"append", std::string("- 09:00 standup")}, {"heading", std::string("Log")}});
    CHECK(daily.ok && strField(daily, "note") == "Journal/2026-10-16.md" && strField(daily, "created") == "true");
    const std::string dtext = readText(vault / "Journal" / "2026-10-16.md");
    CHECK(contains(dtext, "created: 2026-10-16\n") && contains(dtext, "# Friday 16 October 2026") && contains(dtext, "## Log\n\n- 09:00 standup\n"));
    Result again = run("notes.daily", {{"today", std::string("2026-10-16")}});
    CHECK(again.ok && strField(again, "created") == "false");
    CHECK(run("notes.tasks.add", {{"text", std::string("Send invoice")}, {"due", std::string("+2")}, {"heading", std::string("Todo")}, {"today", std::string("2026-10-16")}}).ok);
    CHECK(contains(readText(vault / "Journal" / "2026-10-16.md"), "## Todo\n\n- [ ] Send invoice \xF0\x9F\x93\x85 2026-10-18\n"));

    // ---- templates ----
    Result tl = run("notes.templates.list", {});
    CHECK(tl.ok && intField(tl, "count") == 2 && contains(tl.message, "{{client}}"));
    CHECK(run("notes.templates.render", {{"template", std::string("Meeting")}, {"title", std::string("X")}}).message.rfind("# X\n", 0) == 0);

    // ---- frontmatter ----
    CHECK(run("notes.frontmatter.set", {{"note", std::string("Home")}, {"key", std::string("status")}, {"value", std::string("active")}}).ok);
    CHECK(run("notes.frontmatter.set", {{"note", std::string("Home")}, {"key", std::string("aliases")}, {"value", std::string("Base")}, {"append", true}}).ok);
    Result fg = run("notes.frontmatter.get", {{"note", std::string("Base")}});  // found by the new alias
    CHECK(fg.ok && contains(strField(fg, "frontmatter"), "\"aliases\":[\"Start\",\"Base\"]") && contains(strField(fg, "frontmatter"), "\"status\":\"active\""));
    CHECK(run("notes.frontmatter.get", {{"note", std::string("Home")}, {"key", std::string("status")}}).message == "active");
    CHECK(run("notes.frontmatter.set", {{"note", std::string("Home")}, {"key", std::string("status")}, {"delete", true}}).ok);
    CHECK(!contains(readText(vault / "Home.md"), "status"));

    // ---- tags ----
    Result tags = run("notes.tags", {{"nested", true}});
    CHECK(tags.ok && contains(tags.message, "#project  1") && contains(tags.message, "#project/alpha  1") && contains(tags.message, "#hub  1"));
    CHECK(intField(run("notes.tags.find", {{"tag", std::string("#project")}}), "count") == 1);
    Result tr = run("notes.tags.rename", {{"from", std::string("project")}, {"to", std::string("proj")}});
    CHECK(tr.ok && contains(readText(vault / "Projects" / "Project Alpha.md"), "#work #proj/alpha"));
    CHECK(run("notes.tags.rename", {{"from", std::string("hub")}, {"to", std::string("center")}}).ok);
    CHECK(contains(readText(vault / "Home.md"), "tags: [center]\n"));
    CHECK(s.history().undo() && s.history().undo() && contains(readText(vault / "Projects" / "Project Alpha.md"), "#project/alpha"));

    // ---- tasks ----
    Result tasks = run("notes.tasks", {{"today", std::string("2026-10-16")}});
    CHECK(tasks.ok && intField(tasks, "total") == 4 && intField(tasks, "overdue") == 1);
    CHECK(tasks.message.find("Water plants") < tasks.message.find("Send invoice"));  // by due date
    CHECK(intField(run("notes.tasks", {{"due", std::string("overdue")}, {"today", std::string("2026-10-16")}}), "total") == 1);
    CHECK(intField(run("notes.tasks", {{"due", std::string("week")}, {"today", std::string("2026-10-16")}}), "total") == 3);
    CHECK(intField(run("notes.tasks", {{"status", std::string("done")}}), "total") == 1);
    CHECK(intField(run("notes.tasks", {{"tag", std::string("work")}}), "total") == 2);
    CHECK(!run("notes.tasks.toggle", {{"match", std::string("budget|nothing")}}).ok);
    Result tt = run("notes.tasks.toggle", {{"match", std::string("water")}, {"stamp", true}, {"today", std::string("2026-10-16")}});
    CHECK(tt.ok && contains(readText(vault / "Home.md"), "- [x] Water plants \xF0\x9F\x93\x85 2026-10-10 \xE2\x9C\x85 2026-10-16\n"));
    CHECK(run("notes.tasks.toggle", {{"note", std::string("Home")}, {"line", std::int64_t{tt.ok ? intField(tt, "line") : 0}}}).ok);
    CHECK(contains(readText(vault / "Home.md"), "- [ ] Water plants \xF0\x9F\x93\x85 2026-10-10\n"));
    CHECK(!run("notes.tasks.toggle", {{"note", std::string("Home")}, {"line", std::int64_t{1}}}).ok);

    // ---- outline, graph ----
    Result ol = run("notes.outline", {{"note", std::string("Project Alpha")}, {"json", true}});
    CHECK(ol.ok && intField(ol, "count") == 3 && contains(strField(ol, "items"), "\"anchor\":\"tasks\""));
    Result gj = run("notes.graph", {{"preview", true}});
    CHECK(gj.ok && gj.attachments.size() == 1);
    json g = json::parse(strField(gj, "graph"));
    CHECK(g["nodes"].size() == static_cast<std::size_t>(intField(gj, "nodes")) && g["edges"].size() >= 4);
    Result dot = run("notes.graph", {{"format", std::string("dot")}, {"focus", std::string("Home")}, {"depth", std::int64_t{1}}, {"unresolved", true}});
    CHECK(dot.ok && contains(strField(dot, "graph"), "digraph vault") && contains(strField(dot, "graph"), "\"?missing\"") && !contains(strField(dot, "graph"), "Loose"));
    CHECK(run("notes.graph.export", {{"out", (dir / "graph.png").string()}}).ok && fs::file_size(dir / "graph.png") > 1000);
    CHECK(run("notes.graph.export", {{"out", (dir / "graph.svg").string()}}).ok && contains(readText(dir / "graph.svg"), "<svg"));

    // ---- render and export ----
    Result rh = run("notes.render", {{"note", std::string("Project Alpha")}});
    const std::string h = strField(rh, "html");
    CHECK(rh.ok && contains(h, "<!doctype html>") && contains(h, "href=\"../Home.html\"") && contains(h, "<div class=\"embed\">") && contains(h, "second project"));
    Result rhome = run("notes.render", {{"note", std::string("Home")}, {"out", (dir / "home.html").string()}});
    CHECK(rhome.ok && contains(readText(dir / "home.html"), "data:image/png;base64,") && contains(readText(dir / "home.html"), "class=\"properties\""));
    Result book = run("notes.render", {{"out", (dir / "all.html").string()}, {"backlinks", true}});
    CHECK(book.ok && intField(book, "notes") == 6 && contains(readText(dir / "all.html"), "href=\"#note-projects-beta\""));
    Result pdf = run("notes.render", {{"out", (dir / "all.pdf").string()}, {"preview", true}});
    CHECK(pdf.ok && intField(pdf, "pages") >= 6 && readText(dir / "all.pdf").rfind("%PDF", 0) == 0 && pdf.attachments.size() == 1);
    Result site = run("notes.export.site", {{"out", (dir / "site").string()}, {"graph", true}});
    CHECK(site.ok && intField(site, "pages") == 6 && intField(site, "attachments") == 1);
    CHECK(fs::exists(dir / "site" / "index.html") && fs::exists(dir / "site" / "Projects" / "Project Alpha.html") && fs::exists(dir / "site" / "assets" / "pic.png"));
    CHECK(fs::exists(dir / "site" / "style.css") && fs::exists(dir / "site" / "tags.html") && fs::exists(dir / "site" / "graph.png"));
    const std::string page = readText(dir / "site" / "Projects" / "Beta.html");
    CHECK(contains(page, "href=\"../style.css\"") && contains(page, "Linked from") && contains(page, "href=\"Project%20Alpha.html\""));
    CHECK(contains(readText(dir / "site" / "index.html"), "Home base"));  // Home.md is the home page
    CHECK(!run("notes.export.site", {{"out", vault.string()}}).ok);

    // ---- edits made outside are picked up; the cache is used and rebuildable ----
    writeText(vault / "Outside.md", "Written by another editor about zeppelins. [[Home]]\n");
    CHECK(intField(run("notes.search", {{"query", std::string("zeppelins")}}), "total") == 1);
    fs::remove(vault / "Outside.md");
    CHECK(intField(run("notes.search", {{"query", std::string("zeppelins")}}), "total") == 0);
    {
        Session s2;
        notes::registerCommands(s2);
        Result o2 = s2.run("notes.vault.info", {{"vault", vault.string()}});
        CHECK(o2.ok && intField(o2, "notes") == intField(run("notes.vault.info", {}), "notes"));
        CHECK(s2.run("notes.vault.reindex", {}).ok && fs::exists(vault / ".achnotes" / "cache.json"));
    }

    // ---- CRLF files (Windows editors) ----
    writeText(vault / "Crlf.md", "---\r\ntags: [win]\r\n---\r\n# Windows\r\n\r\n- [ ] crlf task\r\nSee [[Home]]\r\n");
    Result cf = run("notes.frontmatter.get", {{"note", std::string("Crlf")}, {"key", std::string("tags")}});
    CHECK(cf.ok && cf.message == "win");
    CHECK(intField(run("notes.outline", {{"note", std::string("Crlf")}}), "count") == 1);
    CHECK(run("notes.tasks.toggle", {{"match", std::string("crlf task")}}).ok);
    CHECK(readText(vault / "Crlf.md") == "---\r\ntags: [win]\r\n---\r\n# Windows\r\n\r\n- [x] crlf task\r\nSee [[Home]]\r\n");
    CHECK(run("notes.frontmatter.set", {{"note", std::string("Crlf")}, {"key", std::string("status")}, {"value", std::string("ok")}}).ok);
    CHECK(contains(readText(vault / "Crlf.md"), "status: ok\r\n---\r\n# Windows"));
    CHECK(run("notes.append", {{"note", std::string("Crlf")}, {"content", std::string("- added")}}).ok);
    CHECK(contains(readText(vault / "Crlf.md"), "See [[Home]]\r\n\r\n- added\r\n"));
    CHECK(contains(markdownToHtmlCrlf(),"<h1 id=\"t\">T</h1>"));
    fs::remove(vault / "Crlf.md");

    // ---- batch capture ----
    {
        Session bs;
        notes::registerCommands(bs);
        cli::Batch batch(bs);
        std::istringstream script("notes.vault.open path=\"" + vault.generic_string() + "\" -> v\nnotes.create vault=$v note=\"Batch note\" content=hi -> n\nnotes.read note=$n\n");
        std::ostringstream out, err;
        CHECK(batch.run(script, out, err) == 0);
        CHECK(batch.vars().at("v") == "vault1" && batch.vars().at("n") == "Batch note.md" && contains(out.str(), "hi"));
        fs::remove(vault / "Batch note.md");
    }

    // ---- MCP ----
    {
        Session ms;
        notes::registerCommands(ms);
        mcp::Server server(ms, {"achclwss", "test", "notes"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        int seen = 0;
        for (const auto& t : list) {
            const std::string name = t["name"];
            if (name.rfind("notes_", 0) == 0) ++seen;
            if (name == "notes_search") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (name == "notes_rename") CHECK(t["annotations"]["readOnlyHint"] == false);
        }
        CHECK(seen == 38);
        json o = call(server, 2, "notes_vault_open", {{"path", vault.string()}});
        CHECK(o["isError"] == false && o["structuredContent"]["doc"] == "vault1");
        json q = call(server, 3, "notes_search", {{"query", "budget"}, {"limit", 1}});
        CHECK(q["isError"] == false && q["structuredContent"]["count"] == 1 && q["structuredContent"]["total"] == 3);
        json c = call(server, 4, "notes_create", {{"note", "From Claude"}, {"content", "Made over MCP"}});
        CHECK(c["isError"] == false && fs::exists(vault / "From Claude.md"));
        CHECK(call(server, 5, "undo", json::object())["isError"] == false && !fs::exists(vault / "From Claude.md"));
        json gp = call(server, 6, "notes_graph", {{"format", "png"}});
        CHECK(gp["content"].size() == 2 && gp["content"][1]["type"] == "image");
    }

    fs::remove_all(dir);
    return check::summary("notes");
}
