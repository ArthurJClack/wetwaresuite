// The vector tool: an Illustrator/Inkscape-style editor that works headless
// first. A VectorDoc is a canvas plus an ordered stack of shapes; every edit is
// a command (vector.add_rect, vector.style, ...) so the CLI, batch scripts and
// Claude over MCP drive it the same way. Rendering goes through the raster
// library; files are real SVG (plus PDF and EPS out, PDF/AI/EPS in).
//
// The shape list is flat, bottom first. Groups and layers are shapes of kind
// Group; a shape's `parent` names the group it sits in, and siblings stack in
// list order. Symbols keep their own shapes; Instance shapes draw them.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"
#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::vector {

using raster::Color;
using raster::Point;

enum class ShapeKind { Rect, Ellipse, Line, Polyline, Polygon, Path, Text, Group, Instance };

const char* toString(ShapeKind kind);  // "rect", "ellipse", ...

// Translate, then rotate (degrees, clockwise on screen) and scale around the
// centre of the shape's own bounding box.
struct Transform {
    double tx = 0, ty = 0;
    double rotate = 0;
    double sx = 1, sy = 1;
    bool identity() const { return tx == 0 && ty == 0 && rotate == 0 && sx == 1 && sy == 1; }
};

// 2D affine matrix in SVG order: x' = a*x + c*y + e, y' = b*x + d*y + f.
struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Point apply(Point p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    Matrix operator*(const Matrix& m) const;  // this applied after m
    bool identity() const { return a == 1 && b == 0 && c == 0 && d == 1 && e == 0 && f == 0; }
    double scaleFactor() const;               // sqrt(|det|): how much it scales lengths on average
    bool invert(Matrix& out) const;
    static Matrix translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Matrix scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    static Matrix rotate(double degrees);
    static Matrix skew(double xDegrees, double yDegrees);
};

// A colour ramp filling a shape. Positions are fractions of the shape's own box (0..1), like SVG's
// objectBoundingBox, so the gradient moves, turns and scales with the shape.
struct GradientStop {
    double offset = 0;  // 0..1
    Color color;
};
struct ColorPoint {  // a freeform gradient's colour dot, in fractions of the shape's box
    double x = 0.5, y = 0.5;
    Color color;
};
struct Gradient {
    bool radial = false;
    bool freeform = false;                       // colours blend between `points` (Illustrator's freeform)
    double x1 = 0, y1 = 0.5, x2 = 1, y2 = 0.5;  // linear: from (x1,y1) to (x2,y2)
    double cx = 0.5, cy = 0.5, r = 0.5;          // radial: centre and radius
    std::vector<GradientStop> stops;             // sorted by offset, at least one (linear and radial)
    std::vector<ColorPoint> points;              // freeform
    Color at(double t) const;                    // colour at position t (clamped to 0..1)
    Color atPoint(double u, double v) const;     // freeform colour at a box fraction
};

// A repeating fill drawn from two colours (Illustrator's basic pattern swatches).
struct Pattern {
    std::string type = "stripes";  // stripes, dots, grid, checker, hatch (crossed lines)
    Color color{0, 0, 0, 255};
    std::optional<Color> background;  // none = see-through between the marks
    double size = 10;                 // tile size in px
    double angle = 0;                 // degrees
    double weight = 0.5;              // how much of the tile the mark covers, 0..1
};

// How an outline is drawn. Defaults keep the round look the tool always had.
struct StrokeStyle {
    std::string cap = "round";      // butt, round, square
    std::string join = "round";     // miter, round, bevel
    double miterLimit = 4;
    std::vector<double> dash;       // on, off, on, off ... in px; empty = solid
    double dashOffset = 0;
    std::string align = "center";   // center, inside, outside (closed shapes)
    std::string startArrow = "none", endArrow = "none";  // none, triangle, open, circle, square, bar
    double arrowScale = 1;          // arrowhead size, times the default (4 x the width)
    std::string profile = "uniform";  // uniform, taper (both ends), taper_start, taper_end, bulge, pinch
    bool plain() const;             // drawable as an ordinary round-capped line
};

// One extra fill or outline in a shape's appearance stack (Illustrator's Appearance panel).
struct AppearanceItem {
    bool stroke = false;          // false = a fill
    Color color{0, 0, 0, 255};
    double width = 1;             // strokes
    StrokeStyle style;            // strokes
    double opacity = 1;
    std::string blend = "normal";
    bool under = false;           // drawn below the shape's own fill instead of above its outline
    double dx = 0, dy = 0;        // offset (Illustrator's Transform effect on one item)
};

// Live effects drawn when the shape renders and written as SVG filters.
struct Effect {
    std::string type;             // drop_shadow, inner_shadow, outer_glow, inner_glow, blur
    Color color{0, 0, 0, 255};
    double opacity = 0.75;
    double dx = 4, dy = 4;        // shadows
    double radius = 4;            // blur radius (about the standard deviation), px
};

struct Shape {
    std::string id;    // "s1", unique within the document
    std::string name;  // free text, defaults to e.g. "rect 1"
    ShapeKind kind = ShapeKind::Rect;

    // rect: x, y, w, h, radius. ellipse: cx, cy, rx, ry.
    double x = 0, y = 0, w = 0, h = 0, radius = 0;
    double cx = 0, cy = 0, rx = 0, ry = 0;
    std::vector<Point> points;  // line (2 points), polyline, polygon
    std::string d;              // path data

    // text: one line per \n, baseline of the first line at (x, y)
    std::string text;
    double fontSize = 48;
    std::string font = "sans";       // sans (built in), serif, mono, a family name, or a .ttf/.otf file
    bool bold = false, italic = false;  // for system fonts (the built-in one has a single weight)
    std::string anchor = "start";    // start, middle or end of the line sits at x
    double letterSpacing = 0;        // extra px between letters
    double lineHeight = 1.2;         // times fontSize
    // area text: when boxWidth > 0 the words wrap inside the box at (x, y) (y = top), in columns
    double boxWidth = 0, boxHeight = 0;
    int columns = 1;
    double columnGap = 12;
    std::string align = "left";      // area text: left, center, right, justify
    // text on a path: the id of a path-like shape the baseline follows, starting `pathOffset` px along
    // it. `d` then holds that path (on the canvas), refreshed after every edit.
    std::string onPath;
    double pathOffset = 0;
    std::string textCase = "none";   // none, upper, lower, title, small_caps
    std::string textStyle;           // name of the character/paragraph style it follows

    std::optional<Gradient> gradient;  // fills with a gradient instead of `fill` (fill stays as the fallback colour)
    std::optional<Gradient> strokeGradient;
    std::optional<Pattern> pattern;    // fills with a pattern (over the gradient / colour)

    std::optional<Color> fill;    // nullopt = none
    std::optional<Color> stroke;  // nullopt = none
    double strokeWidth = 1;
    StrokeStyle line;
    std::string fillRule = "evenodd";  // evenodd or nonzero
    std::string fillSwatch, strokeSwatch;  // global swatch the colour comes from (empty = none)
    std::vector<AppearanceItem> appearance;
    std::vector<Effect> effects;
    std::string blend = "normal";      // a raster blend mode name: multiply, screen, overlay, ...
    double opacity = 1;
    bool visible = true;
    bool locked = false;
    Transform transform;

    // structure
    std::string parent;  // id of the group or layer it sits in; empty = top level
    bool layer = false;  // groups: listed as a layer
    std::string clip;    // groups: id of the child whose shape clips the others (it isn't drawn)
    std::string mask;    // groups: id of the child whose brightness masks the others (opacity mask)
    std::string symbol;  // instances: the symbol drawn
};

struct Artboard {
    std::string name;
    double x = 0, y = 0, w = 0, h = 0;
};

struct Symbol {
    std::string name;
    std::vector<Shape> shapes;  // in the symbol's own coordinates, bottom first
};

// A named set of text settings; text that follows it changes when it does.
struct TextStyle {
    std::string name;
    std::string font = "sans";
    double size = 48;
    bool bold = false, italic = false;
    double letterSpacing = 0, lineHeight = 1.2;
    std::string align = "left";
    std::string textCase = "none";
    std::optional<Color> fill;
};

// Everything an edit can change; VectorEdit snapshots it for undo.
struct Drawing {
    int width = 800;
    int height = 600;
    Color background{255, 255, 255, 255};
    std::vector<Shape> shapes;  // bottom first
    int nextId = 1;
    std::vector<Artboard> artboards;  // empty = the canvas is the only artboard
    std::vector<Symbol> symbols;
    std::vector<std::pair<std::string, Color>> swatches;
    std::vector<TextStyle> textStyles;
    std::vector<double> guidesX, guidesY;  // vertical guides at x, horizontal at y
    double gridSize = 0;                   // grid spacing in px, 0 = no grid
    bool snapGrid = false, snapGuides = false, snapPoints = false;
    std::string currentLayer;  // new shapes go into this layer (a group id)
};

class VectorDoc : public Document, public Drawing {
public:
    std::string kind() const override { return "vector"; }
    std::string summary() const override;
    // Brings cached values up to date: text on a path copies its path, instances their symbol's box.
    void refresh();
    std::unique_ptr<Attachment> preview() const override;

    // Finds by id ("s3") first, then by name. Returns -1 when missing.
    int find(const std::string& idOrName) const;
    // Gives the shape a fresh id (and a default name if it has none), appends it, returns the id.
    // A shape without a parent goes into the current layer.
    std::string add(Shape shape);
    // Ids of the shapes directly inside a group, bottom first.
    std::vector<int> children(const std::string& groupId) const;
    // A shape and everything nested inside it (indices, bottom first, the shape itself included).
    std::vector<int> subtree(int index) const;
    bool hiddenOrInside(int index) const;  // hidden itself or inside a hidden group
    bool lockedOrInside(int index) const;
    const Symbol* findSymbol(const std::string& name) const;
    std::optional<Color> swatch(const std::string& name) const;
    Artboard artboard(int index) const;  // index 0 with no artboards = the canvas
    int artboardCount() const { return artboards.empty() ? 1 : static_cast<int>(artboards.size()); }
};

// ---- geometry -------------------------------------------------------------

// A flattened subpath.
struct Contour {
    std::vector<Point> points;
    bool closed = false;
};
using Outline = std::vector<Contour>;

struct Bounds {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty = true;
    double width() const { return x1 - x0; }
    double height() const { return y1 - y0; }
    void add(Point p);
    void add(const Bounds& b);
};
Bounds bounds(const Outline& outline);

// Parses SVG path data (M L H V C S Q T A Z, absolute and relative) and
// flattens curves into line segments. `detail` is the zoom the result will be
// drawn at (more segments when larger). On malformed data the path is kept up
// to the error and `error`, when given, is set.
Outline flattenPath(const std::string& d, double detail = 1.0, std::string* error = nullptr);

// ---- bezier paths: the pen tool's anchors and handles ------------------------

// An anchor with its two handles, all absolute. A handle equal to the anchor means "no handle".
struct Node {
    Point p, in, out;
};
enum class NodeType { Corner, Smooth, Symmetric };
const char* toString(NodeType type);
NodeType nodeType(const Node& n);  // read from the handles: mirrored = symmetric, in line = smooth
struct SubPath {
    std::vector<Node> nodes;
    bool closed = false;
};
using BezierPath = std::vector<SubPath>;

// Any SVG path data as cubic segments (lines keep their handles on the anchors, arcs become curves).
BezierPath parsePath(const std::string& d, std::string* error = nullptr);
std::string pathData(const BezierPath& path);  // M/L/C/Z, lines where the handles sit on the anchors
Outline flatten(const BezierPath& path, double detail = 1.0);
void transformPath(BezierPath& path, const Matrix& m);
Bounds pathBounds(const BezierPath& path);  // of the curves themselves (control points ignored)
// The shape's geometry as anchors and handles, in its own coordinates (text: its letters).
BezierPath shapePath(const Shape& shape);
// Point and tangent at t (0..1) on the cubic from a to b.
Point cubicPoint(const Node& a, const Node& b, double t);
// Splits the segment after node i of a subpath at t, inserting a node; returns the new node's index.
std::size_t splitSegment(SubPath& sp, std::size_t i, double t);
// Nearest point on the path: which subpath, which segment (starting at that node) and where along it.
struct PathHit {
    std::size_t sub = 0, seg = 0;
    double t = 0, distance = 1e300;
    Point at;
};
PathHit nearestOnPath(const BezierPath& path, Point p);
void reverse(SubPath& sp);
// Smooth curves through points (Catmull-Rom handles), `tension` 0..1 (0.5 = natural).
SubPath smoothThrough(const std::vector<Point>& pts, bool closed, double tension = 0.5);
// Fewer anchors: fits cubic curves to the path within `tolerance` px (Schneider), corners kept
// where the direction turns by more than `cornerAngle` degrees.
SubPath simplify(const std::vector<Point>& pts, bool closed, double tolerance, double cornerAngle = 60);

// Text laid out as SVG path data (glyph curves kept), in the shape's own coordinates: lines from
// (x, y), wrapped in its box (area text), or along the path in `d` (text on a path).
std::string textPathData(const Shape& text);
// Area text: the lines it wraps into (one string per line, column breaks included).
std::vector<std::string> wrapText(const Shape& text);
// Any shape as path data: text keeps its glyph curves, the rest are flattened outlines.
std::string toPathData(const Shape& shape, double detail = 4.0);

// Boolean operations on filled areas. The result is non-overlapping closed contours: outer edges
// and holes. Each input is read even-odd unless its nonzero flag is set (overlapping pieces such
// as an outlined stroke then merge instead of cancelling).
enum class BoolOp { Union, Subtract, Intersect, Exclude };
Outline booleanOp(const Outline& a, const Outline& b, BoolOp op, bool nonzeroA = false, bool nonzeroB = false);
std::string outlinePathData(const Outline& outline);  // "M x y L ... Z" per contour
double signedArea(const std::vector<Point>& pts);     // positive = clockwise on screen (y down)
bool insideOutline(const Outline& o, Point p, bool nonzero = false);

// ---- strokes as areas ---------------------------------------------------------

// The area a stroke covers, as overlapping pieces that all wind the same way (fill them nonzero).
// Width profiles, dashes, caps, joins and arrowheads are all applied. `closedArea` (the filled
// shape) is only used by inside/outside alignment, through boolean ops.
Outline strokeArea(const Outline& line, double width, const StrokeStyle& style);
// The stroke as clean outlines (the pieces merged; inside/outside alignment applied).
Outline outlineStroke(const Outline& line, double width, const StrokeStyle& style);
// Grows (positive) or shrinks (negative) a filled area by `distance` px.
Outline offsetArea(const Outline& area, double distance, const std::string& join = "miter", double miterLimit = 4);

// The shape in its own coordinates, before its transform.
Outline localOutline(const Shape& shape, double detail = 1.0);
// The matrix the shape's transform stands for (needs the local bounding box centre).
Matrix shapeMatrix(const Shape& shape);
// The shape on the canvas, after its transform. Groups and instances have none of their own.
Outline outline(const Shape& shape, double detail = 1.0);
// The shape's outline in a document: text on a path finds its path, instances draw their symbol,
// groups give everything inside them.
Outline outlineIn(const VectorDoc& doc, int index, double detail = 1.0);
Bounds shapeBounds(const VectorDoc& doc, int index);
// Puts `m` (applied after the shape's own transform) onto the shape: kept in its transform when
// that can say it, else baked into its geometry (the shape becomes a path).
void applyMatrix(VectorDoc& doc, int index, const Matrix& m);

// Colour names and codes: everything raster::Color::parse reads plus more CSS names (teal, navy, gold, ...).
std::optional<Color> parseColor(const std::string& text);

// ---- rendering and files --------------------------------------------------

// Rasterises the document at `scale` (1 = one pixel per unit), antialiased.
raster::Image render(const VectorDoc& doc, double scale = 1.0);
// Just one area of the canvas (an artboard): x, y, w, h in canvas units.
raster::Image renderArea(const VectorDoc& doc, double x, double y, double w, double h, double scale = 1.0,
                         bool background = true);
// One shape (and what's inside it) on a transparent picture of the canvas area.
raster::Image renderShape(const VectorDoc& doc, int index, double x, double y, double w, double h, double scale);

std::string toSvg(const VectorDoc& doc, int artboard = -1);  // -1 = the whole canvas
// Reads a practical SVG subset: rect, circle, ellipse, line, polyline, polygon,
// path, text, g, use/symbol, clip paths and masks, gradients, patterns (as
// colour), fill/stroke styles, filters this tool writes, and transforms.
// Unknown elements are skipped. Throws std::runtime_error when it isn't SVG.
std::shared_ptr<VectorDoc> fromSvg(const std::string& text);

std::shared_ptr<VectorDoc> openSvg(const std::filesystem::path& path);
// PDF pages (and Illustrator .ai files, which are PDF inside) and simple EPS as editable shapes.
// `page` is 1-based. Throws std::runtime_error.
std::shared_ptr<VectorDoc> openPdf(const std::filesystem::path& path, int page = 1);
std::shared_ptr<VectorDoc> openEps(const std::filesystem::path& path);
// Picks the reader by extension (.svg, .pdf, .ai, .eps).
std::shared_ptr<VectorDoc> openAny(const std::filesystem::path& path, int page = 1);
// Vector PDF (one page per artboard, or just `artboard`), EPS (one artboard).
std::string toPdf(const VectorDoc& doc, int artboard = -1);
std::string toEps(const VectorDoc& doc, int artboard = 0);
// .svg/.pdf/.eps write vectors; png/jpg/bmp/tga/ppm rasterise at `scale`. `artboard` -1 = whole
// canvas (PDF: every artboard). Throws std::runtime_error.
void saveDoc(const VectorDoc& doc, const std::filesystem::path& path, double scale = 1.0, int quality = 90,
             int artboard = -1);

// Bitmap to vector: colour regions as filled paths (image trace).
struct TraceSettings {
    int colors = 2;            // 2 = black and white (by threshold), more = that many colours
    int threshold = 128;       // black and white: darker than this is ink
    double smooth = 1.0;       // 0 = pixel corners, 1 = smooth curves
    double tolerance = 1.0;    // px the curves may stray from the pixel edges
    double minArea = 4;        // regions smaller than this (px) are dropped
    bool ignoreWhite = true;   // leave white areas empty
};
struct TracedRegion {
    Color color;
    std::string d;  // path data in image pixels
    double area = 0;
};
std::vector<TracedRegion> traceImage(const raster::Image& image, const TraceSettings& settings);

// ---- commands -------------------------------------------------------------

// Undoable edit: snapshots the document before running `fn`, restores it on undo.
class VectorEdit : public Action {
public:
    VectorEdit(std::shared_ptr<VectorDoc> doc, std::function<Result(VectorDoc&)> fn)
        : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<VectorDoc> doc_;
    std::function<Result(VectorDoc&)> fn_;
    Drawing before_;
};

// Registers every vector.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::vector
