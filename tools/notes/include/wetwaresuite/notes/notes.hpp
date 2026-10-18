// notes: markdown notes and a knowledge base, headless and Obsidian-compatible.
// A vault is a folder of .md files; the files stay the source of truth. What
// the tool learns from them (frontmatter, tags, links, headings, tasks, search
// terms) is cached in a hidden .wwnotes folder in the vault, checked against
// each file's size and modification time, and can be deleted at any time.
// Everything is a notes.* command; edits are undoable (the files they touched
// are put back exactly).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::notes {

namespace fs = std::filesystem;

inline constexpr const char* kCacheDir = ".wwnotes";  // the rebuildable cache
inline constexpr const char* kOldCacheDir = ".achnotes";  // its name before the rename: still marks a vault, then removed
inline constexpr const char* kTrashDir = ".trash";     // deleted notes, like Obsidian's own trash

// ---- text helpers --------------------------------------------------------------

// UTF-8 strings to paths and back (forward slashes), the same on Windows.
fs::path toPath(const std::string& utf8);
std::string fromPath(const fs::path& p);

std::string lower(std::string s);  // ASCII only, so byte offsets stay put
std::string trim(const std::string& s);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
std::string htmlEscape(const std::string& s);
std::string urlEncodePath(const std::string& path);  // spaces and odd bytes as %XX, keeps '/'
std::string urlDecode(const std::string& s);
// Lowercase search terms: runs of ASCII letters/digits or non-ASCII bytes.
std::vector<std::string> tokenize(const std::string& text);
// Heading anchor as the HTML renderer writes it: "My Heading!" -> "my-heading".
std::string slugify(const std::string& text);
bool isTagChar(unsigned char c);

// ---- dates ------------------------------------------------------------------------

struct Date {
    int year = 1970, month = 1, day = 1;
    int hour = 0, minute = 0, second = 0;
    bool operator==(const Date&) const = default;
};
Date now();                                    // local time
// "2026-10-16", "today", "yesterday", "tomorrow", "+3", "-7" (days from `base`). Empty if not a date.
std::optional<Date> parseDate(const std::string& text, const Date& base);
Date addDays(const Date& d, int days);
int weekday(const Date& d);                    // 0 = Monday .. 6 = Sunday
std::int64_t dayNumber(const Date& d);         // days since 1970-01-01
std::string isoDate(const Date& d);            // "2026-10-16"
// moment.js-style formats as Obsidian uses them: YYYY YY MMMM MMM MM M DD D Do dddd ddd dd
// HH H hh h mm m ss s A a WW W gggg, and [literal text].
std::string formatDate(const Date& d, const std::string& format);

// ---- frontmatter (a YAML subset) -----------------------------------------------------

struct FmValue {
    std::string scalar;             // plain value (quotes removed); raw text for nested blocks
    std::vector<std::string> list;  // for lists ([a, b] or "- a" lines)
    bool isList = false;
    std::string text() const;       // scalar, or the list joined with ", "
};
using Frontmatter = std::vector<std::pair<std::string, FmValue>>;

// Finds a leading "---" block. yaml gets its text, bodyStart the byte offset after the closing
// line, bodyLine the 1-based line the body starts on. False when there is none.
bool splitFrontmatter(const std::string& text, std::string* yaml, std::size_t* bodyStart, int* bodyLine = nullptr);
Frontmatter parseFrontmatter(const std::string& yaml);
const FmValue* fmFind(const Frontmatter& fm, const std::string& key);
// The note text with `key` set to `value` (nullptr removes it), touching only that key's lines.
std::string setFrontmatter(const std::string& text, const std::string& key, const FmValue* value);
std::string yamlScalar(const std::string& s);  // quoted when YAML would misread it
std::string bodyOf(const std::string& text);    // text without its frontmatter

// ---- one note's structure --------------------------------------------------------------

struct Link {
    std::string target;   // as written, before '#' and '|' (URL-decoded for markdown links)
    std::string heading;  // after '#': a heading, or ^block-id
    std::string alias;    // after '|' for wikilinks, the link text for markdown links
    bool embed = false;   // ![[...]] or ![...](...)
    bool wiki = true;     // false: [text](target.md)
    bool escapedPipe = false;  // [[a\|b]] inside a table
    int line = 0;         // 1-based
    std::size_t begin = 0, end = 0;  // byte span of the whole link in the file
};

struct TagSpan {
    std::string tag;  // without '#'
    int line = 0;
    std::size_t begin = 0, end = 0;  // span of "#tag"
};

struct Heading {
    int level = 1;
    std::string text;
    int line = 0;
};

struct Task {
    int line = 0;
    bool done = false;
    char mark = ' ';
    std::string text;  // after "[ ] "
    std::string due;   // YYYY-MM-DD: "📅 date", "due:date", "@due(date)", "[due:: date]"
    int indent = 0;
};

struct Parsed {
    Frontmatter frontmatter;
    bool hasFrontmatter = false;
    int bodyLine = 1;
    std::vector<Link> links;
    std::vector<TagSpan> inlineTags;
    std::vector<Heading> headings;
    std::vector<Task> tasks;
    std::size_t words = 0;
};
// Skips fenced code, inline code and %% comments %% for links, tags, headings and tasks.
Parsed parseNote(const std::string& text);
// One "- [ ] task" line (also "* [x]", "1. [ ]", inside quotes). False when it is not a task.
bool parseTaskLine(const std::string& line, Task& task);
std::vector<std::string> frontmatterTags(const Frontmatter& fm);
std::vector<std::string> frontmatterAliases(const Frontmatter& fm);

// The lines of a heading's section (from the heading to the next heading of the same or a
// higher level), as [first, last) byte offsets. Nullopt when there is no such heading.
std::optional<std::pair<std::size_t, std::size_t>> sectionRange(const std::string& text, const std::string& heading);

// ---- the vault ----------------------------------------------------------------------

struct Note {
    std::string rel;    // path inside the vault with '/', e.g. "Projects/Plan.md"
    std::string name;   // file name without .md
    std::string title;  // frontmatter title, else name
    std::uint64_t size = 0;
    std::int64_t mtick = 0;   // raw file-clock ticks, for change detection
    std::int64_t mtime = 0;   // seconds since 1970
    Frontmatter frontmatter;
    std::vector<std::string> tags;     // frontmatter and inline, as written first, deduplicated case-insensitively
    std::vector<std::string> aliases;
    std::vector<Link> links;
    std::vector<Heading> headings;
    std::vector<Task> tasks;
    std::size_t words = 0;
    std::map<std::string, int> terms;  // term -> count (whole file)
    std::size_t termCount = 0;          // sum of the counts
    std::vector<std::string> titleTerms;
    bool hasTag(const std::string& tag) const;  // case-insensitive; "a" also matches "a/b"
};

struct FileEntry {
    std::string rel;
    std::uint64_t size = 0;
    std::int64_t mtick = 0;
    std::int64_t mtime = 0;
};

struct SearchHit {
    const Note* note = nullptr;
    double score = 0;
    std::string snippet;  // matches in **bold**
    int line = 0;         // line of the snippet
};

struct SearchResults {
    std::vector<SearchHit> hits;
    std::size_t total = 0;
};

struct Settings {  // read from .obsidian when present
    std::string dailyFolder;               // "" = vault root
    std::string dailyFormat = "YYYY-MM-DD";
    std::string dailyTemplate;             // a note path or name
    std::string templatesFolder = "Templates";
    std::string dateFormat = "YYYY-MM-DD";
    std::string timeFormat = "HH:mm";
};

class Vault {
public:
    fs::path root;  // absolute
    std::map<std::string, Note> notes;       // by rel
    std::map<std::string, FileEntry> files;  // every other file (attachments), by rel
    Settings settings;

    // Opens a folder (it must exist) and reads it, using the cache when `useCache`.
    void open(const fs::path& folder, bool useCache = true);
    // Re-reads whatever changed on disk. Returns true when anything did. Saves the cache.
    bool refresh();
    void clearCache();  // deletes .wwnotes (and an old .achnotes); the next refresh reads every note
    fs::path abs(const std::string& rel) const { return root / toPath(rel); }
    std::string relOf(const fs::path& absolute) const;  // "" when outside the vault

    // Resolves a link target like Obsidian: exact path, path relative to `fromRel`'s folder,
    // file name (unique or the closest), aliases. Returns the rel of a note or file, or "".
    std::string resolve(const std::string& target, const std::string& fromRel = {}, bool markdownLink = false) const;
    std::string resolve(const Link& link, const std::string& fromRel) const;
    // What a command's note= means: a path, a name, a title or an alias. "" if none.
    std::string findNote(const std::string& spec) const;
    const Note* note(const std::string& rel) const;

    // Incoming links: (from note, link) for every link that resolves to `rel`.
    std::vector<std::pair<const Note*, const Link*>> backlinks(const std::string& rel) const;
    std::size_t linkCount() const;

    SearchResults search(const std::string& query, std::size_t limit, bool snippets) const;
    std::string read(const std::string& rel) const;  // whole file, throws std::runtime_error

    // The shortest wikilink target that resolves to `rel` from anywhere ("Plan" or "Projects/Plan").
    std::string linkText(const std::string& rel) const;
    bool isNote(const std::string& rel) const { return notes.count(rel) > 0; }

private:
    bool scan(bool useCache);
    void rebuild();
    void loadSettings();
    void saveCache() const;
    void loadCache(std::map<std::string, Note>& out) const;

    std::map<std::string, std::vector<std::string>> byName_;   // lowercase file name (and stem for notes) -> rels
    std::map<std::string, std::string> byPath_;                // lowercase rel and rel without .md -> rel
    std::map<std::string, std::vector<std::string>> byAlias_;  // lowercase alias/title -> rels
    std::map<std::string, std::vector<std::pair<const Note*, int>>> index_;  // term -> (note, count)
    double avgLength_ = 1;
    std::map<std::string, std::vector<std::pair<const Note*, const Link*>>> incoming_;
};

// ---- rendering -----------------------------------------------------------------------

struct RenderHooks {
    // href for a wikilink or internal markdown link; nullopt when it doesn't resolve.
    std::function<std::optional<std::string>(const Link&)> link;
    // HTML for an embed ![[...]]; nullopt to render it as a missing embed.
    std::function<std::optional<std::string>(const Link&, int depth)> embed;
    // href for a tag page, e.g. "tags.html#project"; empty = no link.
    std::function<std::string(const std::string&)> tag;
    // src for a markdown image ![alt](src) with a relative src.
    std::function<std::string(const std::string&)> image;
    int depth = 0;  // embed nesting
};
// A CommonMark-ish renderer with the Obsidian extensions: headings with ids, emphasis,
// strikethrough, ==highlights==, lists and task lists, code (fenced and indented), quotes and
// > [!callouts], pipe tables, links, autolinks, images, [[wikilinks]], ![[embeds]], #tags,
// %% comments %% and $math$ left for the reader's KaTeX. Raw HTML passes through.
std::string markdownToHtml(const std::string& markdown, const RenderHooks& hooks = {});
std::string pageCss();
std::string htmlPage(const std::string& title, const std::string& body, const std::string& cssHref = {},
                     const std::string& nav = {});

// ---- graph ------------------------------------------------------------------------------

struct GraphNode {
    std::string id;     // rel, "tag:x" or the unresolved target
    std::string label;
    std::string kind;   // note, attachment, unresolved, tag
    int degree = 0;
    double x = 0, y = 0;
};
struct Graph {
    std::vector<GraphNode> nodes;
    std::vector<std::pair<int, int>> edges;  // directed: from, to
};
struct GraphOptions {
    std::string focus;       // rel: only notes within `depth` links of it
    int depth = 2;
    bool tags = false;       // add tag nodes
    bool unresolved = false; // add nodes for links that go nowhere
    bool attachments = false;
    bool orphans = true;     // keep notes without links
    std::string path;        // only notes whose path contains this
};
Graph buildGraph(const Vault& vault, const GraphOptions& options);
void layoutGraph(Graph& graph, int iterations = 0, unsigned seed = 1);  // 0 = picked by size
std::string graphJson(const Graph& graph);
std::string graphDot(const Graph& graph);
raster::Image drawGraph(const Graph& graph, int width, int height, int labels /* -1 auto, 0 none, 1 all */,
                        const std::string& focus = {});

// ---- commands ----------------------------------------------------------------------------

// Registers every notes.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::notes
