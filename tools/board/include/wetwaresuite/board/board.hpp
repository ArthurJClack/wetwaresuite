// The board tool: a headless whiteboard and diagram editor. A board is an
// infinite canvas of elements (shapes, sticky notes, text, images, frames,
// groups, connectors that stay attached to the shapes they join, and freehand
// ink), drawn in layers with a theme (clean, dark, a hand-drawn "sketchy"
// look, ...). Diagrams can be typed as Mermaid or as an indented outline and
// laid out automatically. Every edit is a board.* command, so the CLI, batch
// scripts and Claude over MCP drive it the same way.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::board {

using raster::Color;

struct Pt {
    double x = 0, y = 0;
};

struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty = true;
    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
    double cx() const { return (x0 + x1) / 2; }
    double cy() const { return (y0 + y1) / 2; }
    void add(double x, double y);
    void add(const Box& b);
    Box grown(double by) const;
    bool intersects(const Box& b) const;
    bool contains(const Box& b) const;
};

enum class Kind {
    Rect, Rounded, Stadium, Ellipse, Diamond, Hexagon, Parallelogram, Trapezoid, Triangle, Cylinder, Cloud, Note,
    Subroutine, Text, Image, Frame, Group, Connector, Ink
};

const char* toString(Kind kind);              // "rect", "rounded", ...
std::optional<Kind> parseKind(const std::string& name);  // also "circle" (ellipse), "sticky", "database", "rhombus", ...
const std::vector<std::string>& shapeKindNames();  // the kinds board.add makes
bool isNode(Kind kind);  // shapes connectors can attach to (everything but connectors and ink)

// Per-element overrides; anything unset comes from the board's theme.
struct Style {
    std::optional<std::string> fill;         // colour or "none"
    std::optional<std::string> stroke;       // colour or "none"
    std::optional<std::string> textColor;
    std::optional<double> strokeWidth;
    std::optional<std::string> dash;         // solid, dashed, dotted
    std::optional<double> opacity;           // 0..1
    std::optional<double> fontSize;
    std::optional<std::string> font;         // sans, serif, mono
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<std::string> align;        // left, center, right
    std::optional<std::string> valign;       // top, middle, bottom
    std::optional<std::string> fillStyle;    // solid, hachure, cross, none
    std::optional<bool> sketch;              // hand-drawn strokes
    std::optional<double> roughness;         // 0..3 for sketch
    std::optional<double> radius;            // corner radius of rounded rects
    std::optional<bool> shadow;
};

struct InkPoint {
    double x = 0, y = 0, pressure = 1;
};

struct Element {
    std::string id;    // "e1", unique within the board
    std::string name;  // free text used to find it (Mermaid node ids land here)
    Kind kind = Kind::Rect;
    double x = 0, y = 0, w = 0, h = 0;  // top-left corner and size (connectors and ink: their bounds)
    std::string text;                   // label (connectors: the label at the middle)
    std::string layer = "default";
    std::string parent;                 // id of the frame or group this belongs to
    Style style;
    std::string src;  // image: file path or data: URL
    std::string link;  // optional URL

    // connectors
    std::string from, to;                    // element ids; empty = a free end at start/end
    std::string fromAnchor = "auto", toAnchor = "auto";  // auto, top, right, bottom, left, center
    std::string route = "straight";          // straight, elbow, curved
    std::string startArrow = "none", endArrow = "arrow";  // none, arrow, triangle, diamond, circle, bar
    Pt start, end;
    std::vector<Pt> waypoints;               // straight route: pass through these

    // ink
    std::vector<InkPoint> ink;
    double inkWidth = 4;
};

struct Layer {
    std::string name = "default";
    bool visible = true;
    bool locked = false;
    double opacity = 1;
};

struct Board {
    std::string title;
    std::string theme = "default";
    std::string background;  // empty = the theme's
    std::vector<Layer> layers{Layer{}};
    std::vector<Element> elements;  // bottom first within each layer
    int nextId = 1;

    // By id first, then by name. -1 when missing.
    int find(const std::string& idOrName) const;
    Element* get(const std::string& idOrName);
    const Element* get(const std::string& idOrName) const;
    // Gives the element a fresh id (and adds its layer if new); returns the id.
    std::string add(Element e);
    Layer* layer(const std::string& name);
    const Layer* layer(const std::string& name) const;
    Layer& ensureLayer(const std::string& name);
    std::vector<std::string> children(const std::string& id) const;     // direct
    std::vector<std::string> descendants(const std::string& id) const;  // all levels
    // Removes elements (with their descendants and connectors attached to them). Returns how many went.
    int remove(const std::vector<std::string>& ids);
};

class BoardDoc : public Document {
public:
    Board board;
    std::string kind() const override { return "board"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
};

// Undoable edit: snapshots the board before running `fn`, restores it on undo.
class BoardEdit : public Action {
public:
    BoardEdit(std::shared_ptr<BoardDoc> doc, std::function<Result(BoardDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<BoardDoc> doc_;
    std::function<Result(BoardDoc&)> fn_;
    Board before_;
};

// ---- themes and styles ---------------------------------------------------------------

struct Theme {
    std::string name;
    std::string background, stroke, fill, text, line, noteFill, noteText, frameStroke, frameFill;
    std::string font = "sans";
    double strokeWidth = 2;
    double fontSize = 16;
    bool sketch = false;
    double roughness = 1;
    std::string fillStyle = "solid";
    std::vector<std::string> palette;  // branch colours for mind maps
};
const Theme& theme(const std::string& name);  // unknown names give "default"
const std::vector<std::string>& themeNames();

// Every style property with the theme's value filled in.
struct Resolved {
    std::optional<Color> fill, stroke;
    Color text;
    double strokeWidth = 2;
    std::string dash = "solid";
    double opacity = 1;
    double fontSize = 16;
    std::string font = "sans";
    bool bold = false, italic = false;
    std::string align = "center", valign = "middle";
    std::string fillStyle = "solid";
    bool sketch = false;
    double roughness = 1;
    double radius = 12;
    bool shadow = false;
};
Resolved resolve(const Board& board, const Element& e);
Color boardBackground(const Board& board);
// CSS names and codes (#rgb, #rrggbb, rgb(), red, teal, ...). nullopt for "none"/"transparent" or garbage.
std::optional<Color> parseColor(const std::string& text);
bool validColor(const std::string& text);  // a colour or "none"

// ---- geometry --------------------------------------------------------------------------

struct Contour {
    std::vector<Pt> pts;
    bool closed = true;
};

struct ShapeGeom {
    std::vector<Contour> body;   // filled and stroked
    std::vector<Contour> lines;  // extra strokes (a cylinder's rim, a note's fold)
};
ShapeGeom shapeGeometry(const Element& e, double radius = 12);

Box elementBox(const Board& board, const Element& e);  // connectors and groups included
Box contentBounds(const Board& board, bool visibleOnly = true);
// The connector's polyline on the canvas, from its start to its end (curves flattened).
std::vector<Pt> connectorPath(const Board& board, const Element& c);
// Moves an element and everything inside it (frames, groups); ink points move too.
void moveBy(Board& board, Element& e, double dx, double dy);
// Sets a new box; ink is scaled to fit.
void resizeTo(Board& board, Element& e, double x, double y, double w, double h);
// Groups take their children's bounds; frames grow to fit theirs when `fitFrames`.
void refreshGroups(Board& board, bool fitFrames = false, double padding = 24);
// Exponential + Chaikin smoothing of raw ink.
std::vector<InkPoint> smoothInk(const std::vector<InkPoint>& pts, double amount);

// Text measurement with the standard-font metrics used by every backend.
double textWidth(const std::string& line, const std::string& font, bool bold, bool italic, double size);
std::vector<std::string> wrapText(const std::string& text, const std::string& font, bool bold, bool italic, double size, double maxWidth);
// A size that fits the text on a shape of this kind.
Pt fitText(Kind kind, const std::string& text, const Resolved& style, double minW, double minH);

// ---- layout ----------------------------------------------------------------------------

struct LayoutOptions {
    std::string algorithm = "layered";  // layered, tree, mindmap, force, grid, radial
    std::string direction = "TB";       // TB, BT, LR, RL
    double gapX = 60, gapY = 80;        // between nodes in a rank / between ranks
    int columns = 0;                    // grid: 0 = square-ish
    std::uint64_t seed = 1;
    int iterations = 300;               // force
    std::vector<std::string> ids;       // nodes to move; empty = every top-level node
    bool fitFrames = true;
    double originX = 0, originY = 0;
    bool keepOrigin = true;             // keep the nodes' old top-left corner
};
// Moves the nodes; connectors follow. Returns how many moved (or -1 with error set).
int layout(Board& board, const LayoutOptions& options, std::string& error);

// ---- text to diagram -------------------------------------------------------------------

struct ImportOptions {
    std::string route;        // connector route; empty = the diagram type's default
    std::string layout;       // empty = default for the diagram; "none" to skip
    std::string direction;    // overrides the diagram's own
    double x = 0, y = 0;      // where the diagram's top-left goes
    std::string layer;
    bool colorBranches = true;  // mind maps: a palette colour per branch
};

struct ImportStats {
    std::string kind;  // flowchart, sequence, mindmap, outline
    int nodes = 0, edges = 0, frames = 0;
    std::vector<std::string> ids;  // every element added
    std::vector<std::string> warnings;
};

// Mermaid flowchart/graph, sequenceDiagram and mindmap. Throws std::runtime_error on unusable input.
ImportStats fromMermaid(Board& board, const std::string& text, const ImportOptions& options);
// Indented bullet list (-, *, +, 1.) or plain indented lines -> a mind map. `title` makes a root when there are several top items.
ImportStats fromOutline(Board& board, const std::string& text, const ImportOptions& options, const std::string& title = {});

// ---- rendering and files ---------------------------------------------------------------

struct RenderOptions {
    double scale = 1;
    double padding = 32;
    std::optional<Box> region;   // canvas area; default: everything plus padding
    bool transparent = false;    // PNG/SVG without the background
    std::vector<std::string> only;  // element ids to draw (with descendants); empty = all
    std::vector<std::string> layers;  // layer names to draw; empty = every visible layer
};

Box renderRegion(const Board& board, const RenderOptions& options);
raster::Image renderImage(const Board& board, const RenderOptions& options);
std::string toSvg(const Board& board, const RenderOptions& options);
std::string toPdf(const Board& board, const RenderOptions& options);

std::string toJson(const Board& board);
Board fromJson(const std::string& text);  // throws std::runtime_error
std::string toExcalidraw(const Board& board);
Board fromExcalidraw(const std::string& text);
std::string toDrawio(const Board& board);
Board fromDrawio(const std::string& text);  // uncompressed or compressed <diagram> pages (the first page)

// By extension: .board.json/.json (native), .excalidraw, .drawio/.xml. Throws std::runtime_error.
Board loadBoard(const std::filesystem::path& path);
// Native, excalidraw, drawio, svg, pdf, png/jpg/bmp/tga by extension (or `format`).
void saveBoard(const Board& board, const std::filesystem::path& path, const std::string& format = "auto",
               const RenderOptions& options = {});
std::string formatFor(const std::filesystem::path& path);  // "board", "excalidraw", "drawio", "svg", "pdf", "png", ... or ""

std::string base64Encode(const std::string& bytes);
std::string base64Decode(const std::string& text);  // skips whitespace; stops at bad input
// Pixels for an image element's src (file path or data: URL). Null when it can't be read.
std::shared_ptr<raster::Image> loadImageSrc(const std::string& src);

// Registers every board.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::board
