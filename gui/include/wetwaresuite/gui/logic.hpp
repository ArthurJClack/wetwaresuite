// The window-free half of the GUI: fuzzy command search, forms generated from
// CommandInfo, the beginner shortlist, colours, recent files and plain-word
// messages. Everything here is pure so ctest covers it without a screen.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::gui {

// ---- search ----------------------------------------------------------------

struct SearchHit {
    const CommandInfo* command = nullptr;
    int score = 0;  // higher is better
};

// Scores one command for a query: every word of the query has to show up in the
// title, id or description (or, failing that, as letters in order in the title/id).
// 0 means no match. An empty query matches everything with score 1.
int matchScore(const std::string& query, const CommandInfo& info);

// Best matches first (ties keep the input order), at most `limit` (0 = all).
std::vector<SearchHit> search(const std::string& query, const std::vector<CommandInfo>& commands, std::size_t limit = 0);

// ---- forms -----------------------------------------------------------------

enum class FieldKind {
    Bool,      // checkbox
    Choice,    // dropdown of the spec's choices
    Document,  // dropdown of open document ids
    Color,     // colour picker + text
    OpenFile,  // native "open" dialog + text
    SaveFile,  // native "save" dialog + text
    Folder,    // native folder dialog + text
    Integer,   // slider when a range is known, else a number box
    Number,
    NumberList,  // comma separated numbers
    Text,
};

struct Field {
    ParamSpec spec;
    FieldKind kind = FieldKind::Text;
    std::string label;   // "Max size"
    std::string help;    // the spec's description, first letter upper-case
    bool required = false;
    bool hidden = false;  // filled in by the GUI itself (e.g. "preview")
    std::optional<double> min, max;  // a range read from the description, e.g. "0..100"

    // Current value. `set` is false for an optional field the person hasn't
    // touched and that has no default: it's left out so the command picks.
    bool set = false;
    bool flag = false;
    double number = 0;
    std::string text;  // Text, Color, files, Document, Choice and NumberList
};

struct Form {
    std::string command;
    std::string title;
    std::string description;
    std::vector<Field> fields;
};

// Plain words for a param name: "max_size" -> "Max size", "doc" -> "Document".
std::string labelFor(const std::string& name);

// Reads "0..100", "1..100" or "-90..90" style ranges out of a description.
bool parseRange(const std::string& description, double& min, double& max);

FieldKind kindFor(const ParamSpec& spec, const std::string& commandId);

// Builds a form with defaults filled in. `docs` are the open document ids;
// `currentDoc` pre-selects the document the person is looking at.
Form buildForm(const CommandInfo& info, const std::vector<std::string>& docs = {}, const std::string& currentDoc = {});

// Turns a form back into params. Returns false and sets `error` (plain words) when
// a required field is empty or a number list doesn't parse.
bool toParams(const Form& form, Params& out, std::string& error);

// Sets a field from text the way a person would type it ("12", "yes", "#ff0000").
bool setFieldText(Field& field, const std::string& text);

// ---- shortlist for beginners ------------------------------------------------

// Up to `count` common commands for one tool, picked by verbs people use most
// (open, new, save, resize, crop, adjust, stroke, fill, ...), in that order.
std::vector<std::string> beginnerCommands(const std::string& tool, const std::vector<CommandInfo>& commands,
                                          std::size_t count = 8);

// The commands of a tool: ids starting with "<tool>.".
std::vector<const CommandInfo*> toolCommands(const std::string& tool, const std::vector<CommandInfo>& commands);

// ---- opening files ---------------------------------------------------------

// Which "<tool>.open" commands to try for a file, best guess first: tools whose
// open command mentions the file's extension come before the rest. Commands
// named "<tool>.<thing>_open" that take path= and mention the extension (such
// as forge.sprite_open for .aseprite) are tried too.
std::vector<std::string> openCandidates(const std::string& path, const std::vector<std::string>& tools,
                                        const std::vector<CommandInfo>& commands);

// ---- colours ----------------------------------------------------------------

struct Rgba {
    float r = 0, g = 0, b = 0, a = 1;
};
// "#rgb", "#rgba", "#rrggbb", "#rrggbbaa" or "transparent". Names other than that aren't known here.
std::optional<Rgba> parseHex(const std::string& text);
std::string toHex(const Rgba& c);  // "#rrggbb", or "#rrggbbaa" when not opaque
Rgba fromRgb24(std::uint32_t rgb);

// ---- recent files -----------------------------------------------------------

// Puts `path` first, drops an older copy and anything past `limit`.
std::vector<std::string> pushRecent(std::vector<std::string> recent, const std::string& path, std::size_t limit = 10);
std::vector<std::string> parseRecent(const std::string& text);  // one path per line
std::string formatRecent(const std::vector<std::string>& recent);

// ---- messages ---------------------------------------------------------------

// Rewrites the core's terse errors in plain words, e.g.
// "missing parameter 'width' for photo.new" -> "Please fill in \"Width\".".
std::string friendlyError(const std::string& message);

// A message as a sentence: capital first letter, full stop at the end.
std::string sentence(const std::string& message);

// One "Label: value" line per data field of a result.
std::vector<std::string> resultLines(const Result& result);

// ---- retouching by painting ---------------------------------------------------

// Thins a brush stroke (x,y pairs in picture px) for a command: drops points
// closer than `minStep` to the last one kept, then, if more than `maxPoints`
// remain, keeps evenly spaced ones. The first and last points always stay.
std::vector<double> thinStroke(const std::vector<double>& points, double minStep, std::size_t maxPoints = 400);

// The clean spot a heal stroke copies from (Alt+click, or "Pick source").
// Like a clone brush: the first stroke after picking fixes the offset from the
// stroke's start to the spot, and later strokes keep that offset.
struct HealSource {
    bool picked = false;
    double x = 0, y = 0;     // the spot that was picked
    bool anchored = false;   // the offset is fixed
    double dx = 0, dy = 0;   // source minus stroke start, picture px

    void pick(double px, double py);
    void forget() { *this = HealSource{}; }
    // The offset for a stroke starting at (sx, sy); false when no spot is picked.
    bool offsetFor(double sx, double sy, double& ox, double& oy);
};

// Params for photo.heal from a painted stroke. `brushSize` is the brush
// diameter in picture px (the radius sent is half of it). With `offset`, the
// stroke copies texture from that far away (from_x / from_y); without, it fills
// smoothly from the edges. False with a plain-word `error` when it can't work.
bool healStrokeParams(const std::string& doc, const std::vector<double>& points, double brushSize, int width, int height,
                      const double* offset, Params& out, std::string& error);

// Params for photo.remove_object from a painted stroke (same rules as above).
bool removeStrokeParams(const std::string& doc, const std::vector<double>& points, double brushSize, int width, int height,
                        Params& out, std::string& error);

// The retouch brushes on the canvas toolbar: clone, dodge, burn, sponge, smudge, blur and
// sharpen. Turns a painted stroke into the command (paint.clone, paint.dodge, paint.smudge or
// paint.blur_brush) and its params. Clone needs `cloneOffset` (source minus stroke start, from
// a HealSource); without one it fails with a plain-word error saying to Alt+click a spot first.
struct BrushToolSettings {
    std::string brush = "soft";
    double size = 20, opacity = 100;
    double strength = 50;  // dodge/burn/sponge exposure, smudge and blur/sharpen strength, 0..100
    std::string range = "midtones";  // dodge and burn
};
bool brushToolParams(const std::string& tool, const std::string& doc, const std::vector<double>& points,
                     const std::vector<double>& pressures, const BrushToolSettings& settings, const double* cloneOffset,
                     std::string& command, Params& out, std::string& error);

// ---- the window's layout (layout.cpp) -------------------------------------------

// What an app's window shows on top. Every app has a menu bar; most add a ribbon of
// labeled buttons (Office, Onshape, Blender). Photo, drawing, game, map, sound and
// video apps keep it to one compact row, since their tool rail and tool options do
// most of the work, the way Photoshop, Aseprite, QGIS and Premiere do.
struct AppLayout {
    bool ribbon = true;
    bool compactRibbon = false;
};
AppLayout appLayout(const std::string& appId);

// Opening, starting, saving, closing and exporting: these live in the File menu.
bool fileCommand(const std::string& id);

// A menu or ribbon group's title from a command id's first word: "doc" -> "Document".
std::string groupTitle(const std::string& key);

// One tool's menu: commands that aren't File commands, the ones sharing a first word
// (layer_add, layer_set, layer_move...) gathered into a titled submenu when there are 3
// or more. The first group has an empty title: those go straight in the menu.
struct MenuGroup {
    std::string title;
    std::vector<const CommandInfo*> commands;
};
std::vector<MenuGroup> menuGroups(const std::string& tool, const std::vector<CommandInfo>& commands);

// The ribbon: tabs of labeled groups of buttons. Apps have curated tabs modeled on the
// tools people know; tools without one get a tab of their common actions.
struct RibbonGroup {
    std::string title;
    std::vector<const CommandInfo*> commands;
    std::vector<std::string> labels;  // one per command: short, like "Crop" for "Crop image"
};
struct RibbonTab {
    std::string title;
    std::string tool;
    std::string stem;  // documents opened with <tool>.<stem>_open or _new pick this tab
    std::vector<RibbonGroup> groups;
};
std::vector<RibbonTab> ribbonTabs(const std::string& appId, const std::vector<std::string>& tools, const std::vector<CommandInfo>& commands);

// A button's label with the words its group or tab already says dropped:
// "Set Cells" in the Cells group -> "Set". The title as is when nothing (or everything) would go.
std::string shortLabel(const std::string& title, const std::string& group, const std::string& tab);
// "office.sheet_open" -> "sheet"; "photo.open" -> "".
std::string openerStem(const std::string& opener);
// The tab for a document made by `opener`, or -1.
int ribbonTabFor(const std::vector<RibbonTab>& tabs, const std::string& opener);
// The command that does `verb` (save, close, info...) to a document made by `opener`:
// office.sheet_open + save -> office.sheet_save; photo.open + save -> photo.save. Empty if none.
std::string siblingCommand(const std::string& opener, const std::string& verb, const std::vector<CommandInfo>& commands);

// ---- tools ------------------------------------------------------------------

// A plain-word sentence about a tool for the home screen card.
std::string toolBlurb(const std::string& tool);
// Display name: "forge" -> "Asset forge".
std::string toolTitle(const std::string& tool);

}  // namespace wetwaresuite::gui
