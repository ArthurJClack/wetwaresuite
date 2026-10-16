// The layout tool: print layout and publishing in the spirit of Scribus or
// InDesign, headless first. A LayoutDoc holds pages (sizes in mm), master
// pages, frames (text, image, shapes, vector art), stories that flow through
// linked text frames, paragraph/character styles and colour swatches (RGB or
// CMYK). It exports print PDFs (bleed, crop marks, CMYK, embedded fonts) and
// PNG pages, and previews pages as PNG for Claude.
//
// Coordinates are millimetres from the top-left corner of the page's trim
// box, y down. Font sizes, leading and stroke widths are in points.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "achclwss/core/command.hpp"
#include "achclwss/raster/image.hpp"

namespace achclwss::layout {

using raster::Color;

enum class FrameType { Text, Image, Rect, Ellipse, Line, Vector };
enum class Align { Left, Center, Right, Justify, Inside, Outside };
enum class VAlign { Top, Middle, Bottom };
enum class Fit { Fit, Fill, Stretch, Manual };

const char* toString(FrameType t);
const char* toString(Align a);
const char* toString(VAlign a);
const char* toString(Fit f);
std::optional<FrameType> parseFrameType(const std::string& s);
std::optional<Align> parseAlign(const std::string& s);
std::optional<VAlign> parseVAlign(const std::string& s);
std::optional<Fit> parseFit(const std::string& s);

// Character formatting; unset fields inherit (run -> character style -> paragraph style -> base style).
struct CharProps {
    std::optional<std::string> font;   // helvetica, times, courier (or sans/serif/mono), or a family from layout.font_add
    std::optional<double> size;        // pt
    std::optional<bool> bold, italic, underline;
    std::optional<std::string> color;  // swatch name or colour (#hex, name, cmyk(c,m,y,k))
    std::optional<double> tracking;    // extra space per character, 1/1000 em
    void merge(const CharProps& over);  // fields set in `over` win
};

struct ParaProps {
    std::optional<double> leading;      // pt baseline to baseline; 0 = auto (1.2 x size)
    std::optional<Align> align;
    std::optional<double> spaceBefore;  // pt
    std::optional<double> spaceAfter;   // pt
    std::optional<double> indent;       // first-line indent, mm
    std::optional<double> leftIndent;   // mm
    std::optional<bool> breakBefore;    // start in the next linked frame
    void merge(const ParaProps& over);
};

struct Style {
    std::string name;
    bool character = false;  // character style (only CharProps apply) or paragraph style
    std::string basedOn;
    CharProps chars;
    ParaProps para;
};

struct Run {
    std::string text;   // UTF-8
    std::string style;  // character style, may be empty
    CharProps props;    // local overrides
};

struct Paragraph {
    std::string style;  // paragraph style, empty = "body"
    ParaProps props;
    std::vector<Run> runs;
    std::string text() const;
};

struct Story {
    std::vector<Paragraph> paragraphs;
    std::size_t length() const;  // characters, counting one per paragraph break
};

struct Swatch {
    std::string name;
    bool cmyk = false;
    double c = 0, m = 0, y = 0, k = 0;  // 0..100
    Color rgb;                          // display colour (from CMYK when cmyk)
};

struct FontFamily {
    std::string name;
    std::string regular, bold, italic, boldItalic;  // .ttf paths; empty variants fall back to regular
};

struct Frame {
    std::string id;    // "f1", unique in the document
    std::string name;  // optional label
    FrameType type = FrameType::Text;
    double x = 0, y = 0, w = 0, h = 0;  // mm; a line runs from (x, y) to (x + w, y + h)
    double rotation = 0;                // degrees clockwise around the centre
    std::string fill, stroke;           // colours, empty = none
    double strokeWidth = 1;             // pt
    double opacity = 1;
    double radius = 0;                  // rect corner radius, mm
    bool mirror = false;                // master frames: mirror x on left-hand pages of facing documents
    // text frames
    std::string story;  // story id; linked frames share one
    std::string next;   // frame the story continues in
    int columns = 1;
    double gutter = 5;  // mm
    double inset = 0;   // mm
    VAlign valign = VAlign::Top;
    // image and vector frames
    std::string path;
    Fit fit = Fit::Fit;
    double imageScale = 100;  // percent of the 300 dpi (vector: natural) size, manual fit
    double imageX = 0, imageY = 0;  // mm offset of the picture inside the frame, manual fit
};

struct Page {
    double width = 210, height = 297;
    std::string master;  // empty = none
    std::vector<Frame> frames;  // bottom first
};

struct Master {
    std::string name;
    std::vector<Frame> frames;
};

struct Margins {
    double top = 12.7, bottom = 12.7, left = 12.7, right = 12.7;  // with facing pages left = inside, right = outside
};

// Everything that undo snapshots.
struct Layout {
    std::string title;
    double width = 210, height = 297;  // default page size, mm
    Margins margins;
    double bleed = 3;
    int columns = 1;
    double gutter = 5;
    bool facing = false;
    std::vector<Page> pages;
    std::vector<Master> masters;
    std::map<std::string, Story> stories;
    std::vector<Style> styles;
    std::vector<Swatch> swatches;
    std::vector<FontFamily> fonts;
    int nextFrame = 1;
    int nextStory = 1;
};

class LayoutDoc : public Document, public Layout {
public:
    std::string kind() const override { return "layout"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;

    // Frame by id or name, on pages or masters. `page` gets the 0-based page index (-1 on a master).
    Frame* findFrame(const std::string& idOrName, int* page = nullptr, Master** master = nullptr);
    const Frame* findFrame(const std::string& idOrName, int* page = nullptr) const;
    const Style* findStyle(const std::string& name) const;
    const Swatch* findSwatch(const std::string& name) const;
    Master* findMaster(const std::string& name);
    const Master* findMaster(const std::string& name) const;
    const FontFamily* findFont(const std::string& name) const;
    std::string newFrameId() { return "f" + std::to_string(nextFrame++); }
    std::string newStoryId() { return "t" + std::to_string(nextStory++); }
};

// ---- documents ------------------------------------------------------------

struct PaperSize {
    std::string name;
    double width = 0, height = 0;  // mm, portrait
};
const std::vector<PaperSize>& paperSizes();  // A0..A6, B4, B5, letter, legal, tabloid, card, ...
std::optional<PaperSize> findPaper(const std::string& name);
const std::vector<std::string>& presetNames();  // poster_a2, flyer_a5, card, a4_report, book_6x9, ...

// A new document with default styles; `preset` (empty = plain A4) sets size, margins, bleed and masters.
std::shared_ptr<LayoutDoc> newLayout(const std::string& preset);
void addDefaultStyles(Layout& doc);

std::string toJson(const LayoutDoc& doc);
std::shared_ptr<LayoutDoc> fromJson(const std::string& text);  // throws std::runtime_error
std::shared_ptr<LayoutDoc> openLayout(const std::filesystem::path& path);
void saveLayout(LayoutDoc& doc, const std::filesystem::path& path);

// Page margin box in mm (x, y, w, h); facing documents mirror it on left-hand (even) pages.
struct Box {
    double x = 0, y = 0, w = 0, h = 0;
};
Box marginBox(const Layout& doc, int pageIndex);

// ---- text -------------------------------------------------------------------

// Parses the little markup text params accept: paragraphs split on new lines (or a literal \n),
// **bold**, *italic*, __underline__ and `code`. markdown=true also reads #/##/### headings,
// > quotes, - and 1. lists, ``` code blocks and [links](url) (keeping the link text).
std::vector<Paragraph> parseText(const std::string& text, bool markdown, const std::string& baseStyle = {});

// Colours: a swatch name, "none", #hex, a colour name, rgb(...), or cmyk(c,m,y,k) in percent.
struct Paint {
    bool none = true;
    bool cmyk = false;
    double c = 0, m = 0, y = 0, k = 0;  // 0..1
    Color rgb;
};
std::optional<Paint> resolvePaint(const Layout& doc, const std::string& spec);
Color cmykToRgb(double c, double m, double y, double k);  // 0..1 each

// A resolved font face: a standard PDF font or an embedded TrueType.
struct Face;

struct GlyphRun {
    std::string text;  // UTF-8, already limited to what the font encodes
    std::shared_ptr<const Face> face;
    double size = 10;       // mm (the font's em)
    std::string color;      // colour spec
    double x = 0;           // mm from the frame's left edge
    double width = 0;       // mm, including justification and tracking
    double wordSpace = 0;   // extra mm per space (justified lines)
    double tracking = 0;    // extra mm per character
    bool underline = false;
};

struct Line {
    double baseline = 0;  // mm from the frame's top edge
    double left = 0;      // mm from the frame's left edge (column start + indents)
    std::vector<GlyphRun> runs;
};

struct FrameText {
    std::vector<Line> lines;
    std::size_t chars = 0;  // characters placed in this frame
};

struct StoryLayout {
    std::map<std::string, FrameText> frames;  // by frame id
    std::size_t total = 0;    // characters in the story
    std::size_t overset = 0;  // characters that didn't fit in any frame
};

// The frames a story flows through, first to last.
std::vector<const Frame*> storyChain(const Layout& doc, const std::string& storyId);
// Lays a story out through its frames. `pageIndex` resolves inside/outside alignment and
// {page}/{pages}/{title} fields (for master frames).
StoryLayout layoutStory(const Layout& doc, const Story& story, const std::vector<const Frame*>& chain, int pageIndex = 0);
// Every story in the document.
std::map<std::string, StoryLayout> layoutAll(const Layout& doc);
// A master frame's story with {page}, {pages} and {title} filled in for one page.
Story fieldsFilled(const Layout& doc, const Story& story, int pageIndex);

// Line widths in mm of `text` in a family/style at a size in pt (for sizing frames).
double textWidth(const Layout& doc, const std::string& text, const std::string& font, double sizePt, bool bold = false,
                 bool italic = false);

// ---- pictures -----------------------------------------------------------------

struct PictureInfo {
    bool ok = false;
    std::string error;
    int pxWidth = 0, pxHeight = 0;  // images
    double naturalW = 0, naturalH = 0;  // mm: images at 300 dpi, vectors at 96 px per inch
    bool vector = false;
};
PictureInfo pictureInfo(const std::string& path);
// Where the picture's full extent lands in frame-local mm (x, y, w, h) for the frame's fit.
Box pictureBox(const Frame& f, const PictureInfo& info);
// Effective resolution of an image frame at print size (0 for non-images).
double effectiveDpi(const Frame& f);

// ---- output -------------------------------------------------------------------

struct RenderOptions {
    double dpi = 96;
    bool bleed = false;   // include the bleed area around the trim
    bool guides = false;  // margins, columns, bleed and frame edges
};
// One page (0-based) as an image.
raster::Image renderPage(const LayoutDoc& doc, int pageIndex, const RenderOptions& options);

struct PdfOptions {
    bool bleed = false;        // extend the page by the bleed (TrimBox/BleedBox set)
    bool marks = false;        // crop marks outside the bleed
    bool embedFonts = false;   // embed system TrueType substitutes for the standard fonts
    std::vector<int> pages;    // 0-based, empty = all
    bool compress = true;      // Flate-compress page content (off: readable PDF operators)
};
struct PdfReport {
    int pages = 0;
    std::vector<std::string> fonts;     // font names used
    std::vector<std::string> embedded;  // fonts embedded in the file
    std::vector<std::string> warnings;
    int cols = 0, rows = 0;  // imposition grid
};
std::string toPdf(const LayoutDoc& doc, const PdfOptions& options, PdfReport* report = nullptr);

// N-up imposition: the chosen pages repeated (or in turn) in a grid on a sheet, with crop marks.
struct ImposeOptions {
    double sheetW = 210, sheetH = 297;
    int cols = 0, rows = 0;  // 0 = as many as fit
    double gap = 0;          // mm between items; >= 2 x bleed keeps each item's bleed
    bool marks = true;
    bool repeat = true;      // fill each sheet with copies of one page, else place pages in turn
    std::vector<int> pages;  // 0-based, empty = all
    bool embedFonts = false;
};
std::string imposePdf(const LayoutDoc& doc, const ImposeOptions& options, PdfReport* report = nullptr);

// ---- preflight ------------------------------------------------------------------

struct Issue {
    std::string severity;  // error, warning, info
    std::string kind;      // overset, low_res, missing_file, off_page, bleed, margin, rgb, fonts, empty
    std::string where;     // "page 2, f7"
    std::string message;
};
std::vector<Issue> preflight(const LayoutDoc& doc, double minDpi = 300);

// ---- commands ---------------------------------------------------------------

// Undoable edit: snapshots the document, restores it on undo or when the edit fails.
class LayoutEdit : public Action {
public:
    LayoutEdit(std::shared_ptr<LayoutDoc> doc, std::function<Result(LayoutDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<LayoutDoc> doc_;
    std::function<Result(LayoutDoc&)> fn_;
    Layout before_;
};

// Registers every layout.* command.
void registerCommands(Session& session);

}  // namespace achclwss::layout
