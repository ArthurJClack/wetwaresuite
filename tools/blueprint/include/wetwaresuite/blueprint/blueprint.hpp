// The blueprint tool: 2D technical drafting and floor plans, headless first.
// A Drawing holds entities with exact coordinates in real-world drawing units
// (mm, cm, m, in or ft, y pointing up as in CAD), organised on layers, plus a
// sheet: paper size, drawing scale (1:50) and a title block. Every edit is a
// blueprint.* command, so the CLI, batch scripts and Claude over MCP drive it
// the same way. Files: DXF in and out (native), SVG, PDF and PNG out, all to
// scale.
#pragma once

#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::blueprint {

using raster::Color;

constexpr double kPi = 3.14159265358979323846;

// ---- small geometry -------------------------------------------------------

struct Vec2 {
    double x = 0, y = 0;
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double k) const { return {x * k, y * k}; }
    Vec2 operator/(double k) const { return {x / k, y / k}; }
    Vec2 operator-() const { return {-x, -y}; }
    bool operator==(const Vec2&) const = default;
};

inline double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline double length(Vec2 v) { return std::hypot(v.x, v.y); }
inline double distance(Vec2 a, Vec2 b) { return length(b - a); }
inline Vec2 perp(Vec2 v) { return {-v.y, v.x}; }  // left normal (counter-clockwise)
inline Vec2 normalized(Vec2 v) {
    double l = length(v);
    return l > 0 ? v / l : Vec2{1, 0};
}
inline double radians(double deg) { return deg * kPi / 180.0; }
inline double degrees(double rad) { return rad * 180.0 / kPi; }
inline Vec2 direction(double deg) { return {std::cos(radians(deg)), std::sin(radians(deg))}; }
inline double angleOf(Vec2 v) { return degrees(std::atan2(v.y, v.x)); }  // -180..180
double normalizeAngle(double deg);                                       // 0..360

// Signed area (positive = counter-clockwise), perimeter and centroid of a closed polygon.
double polygonArea(const std::vector<Vec2>& pts);
double polylineLength(const std::vector<Vec2>& pts, bool closed);
Vec2 polygonCentroid(const std::vector<Vec2>& pts);
// Offsets a polyline sideways by `d` (positive = left of the direction of travel)
// with mitred corners. Closed polylines are offset all the way round.
std::vector<Vec2> offsetPolyline(const std::vector<Vec2>& pts, bool closed, double d);

struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty = true;
    void add(Vec2 p);
    void add(const Box& b);
    double width() const { return x1 - x0; }
    double height() const { return y1 - y0; }
};

// ---- units, paper, layers -------------------------------------------------

enum class Unit { Mm, Cm, M, In, Ft };
const char* toString(Unit unit);
std::optional<Unit> parseUnit(const std::string& text);
double unitToMm(Unit unit);
bool metric(Unit unit);

struct PaperSize {
    std::string name;
    double width = 0, height = 0;  // portrait, mm
};
const std::vector<PaperSize>& paperSizes();  // A4..A0, letter, legal, tabloid
std::optional<PaperSize> findPaper(const std::string& name);

enum class LineType { Continuous, Dashed, Center, Hidden };
const char* toString(LineType type);
std::optional<LineType> parseLineType(const std::string& text);
// Dash pattern on paper in mm (dash, gap, dash, gap...), empty for continuous.
std::vector<double> dashPattern(LineType type);

struct Layer {
    std::string name;
    Color color{0, 0, 0, 255};
    double weight = 0.25;  // line weight on paper, mm
    LineType linetype = LineType::Continuous;
    bool visible = true;
    bool locked = false;
};

// Colour names and codes (raster::Color::parse plus CAD names like cyan, magenta, grey).
std::optional<Color> parseColor(const std::string& text);

// AutoCAD colour index <-> RGB (7 is black on paper / white on screen).
Color aciColor(int index);
int nearestAci(Color c);

// ---- entities -------------------------------------------------------------

enum class EntityType { Line, Polyline, Circle, Arc, Text, Dimension, Hatch, Point, Wall, Door, Window, Room };
const char* toString(EntityType type);
std::optional<EntityType> parseEntityType(const std::string& text);

enum class DimKind { Aligned, Horizontal, Vertical, Radius, Diameter, Angular };
const char* toString(DimKind kind);
std::optional<DimKind> parseDimKind(const std::string& text);

enum class HAlign { Left, Center, Right };
enum class VAlign { Baseline, Middle, Top };

// One drawing entity. Which fields matter depends on the type:
//   line: pts[0..1]           polyline: pts, closed     circle: c, r
//   arc: c, r, a0 -> a1 (degrees, counter-clockwise)    point: c
//   text: c (insertion), text, height, rotation, halign, valign
//   dimension: dim; linear: pts = {p1, p2, location}; radius/diameter: c, r,
//              rotation (leader angle); angular: pts = {vertex, p1, p2, location};
//              text = override ("<>" stands for the measured value)
//   hatch: pts (closed boundary), pattern solid|lines|cross, angle, spacing (paper mm)
//   wall: pts (centre line), closed, thickness, fill none|solid|lines
//   door/window: host (wall id), at (distance along the wall to the opening centre),
//                width; door: hingeEnd, swingRight
//   room: pts (closed), name, c (label position), showArea, outline
struct Entity {
    std::string id;  // "e1", unique within the drawing
    EntityType type = EntityType::Line;
    std::string layer = "0";
    std::optional<Color> color;  // nullopt = by layer

    std::vector<Vec2> pts;
    bool closed = false;
    Vec2 c;
    double r = 0, a0 = 0, a1 = 0;

    std::string text;
    double height = 0, rotation = 0;
    HAlign halign = HAlign::Left;
    VAlign valign = VAlign::Baseline;

    DimKind dim = DimKind::Aligned;

    std::string pattern = "solid";
    double angle = 45, spacing = 2;

    double thickness = 0;
    std::string fill = "none";

    std::string host;
    double at = 0, width = 0;
    bool hingeEnd = false, swingRight = false;

    std::string name;
    bool showArea = true, outline = true;
};

// ---- the document ---------------------------------------------------------

struct DimStyle {
    double textHeight = 2.5;  // paper mm
    double arrowSize = 2.5;   // paper mm
    double extOffset = 1.5;   // gap between the measured point and the extension line, paper mm
    double extBeyond = 2.0;   // extension line overshoot past the dimension line, paper mm
    double textGap = 1.0;     // gap between the dimension line and its text, paper mm
    std::string arrows = "arrow";  // arrow | tick | dot
    int precision = -1;            // decimals; -1 = by unit (mm 0, cm 1, m 2, in 2)
    bool suffix = false;           // append the unit (mm, m, ") to values
};

struct TitleBlock {
    bool show = true;  // border frame and title block on the sheet
    std::string title, author, date, sheet = "1/1";
    std::string scale;  // override for the scale cell, empty = the drawing scale
};

struct Grid {
    double spacing = 0;  // drawing units, 0 = off
    double snap = 0;     // input coordinates are rounded to this, 0 = off
    bool show = false;   // grid lines in previews
};

class Drawing : public Document {
public:
    Unit units = Unit::Mm;
    double scaleNum = 1, scaleDen = 50;  // paper : real, e.g. 1:50
    std::string paper = "A3";
    bool landscape = true;
    Vec2 origin;  // drawing coordinates at the paper's bottom-left corner
    TitleBlock title;
    DimStyle dims;
    Grid grid;
    std::vector<Layer> layers{Layer{"0"}};
    std::string current = "0";
    std::vector<Entity> entities;
    int nextId = 1;

    std::string kind() const override { return "blueprint"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;

    double paperWidth() const;   // mm, oriented
    double paperHeight() const;  // mm, oriented
    double k() const;            // paper mm per drawing unit
    Vec2 toPaper(Vec2 p) const { return (p - origin) * k(); }
    Vec2 fromPaper(Vec2 p) const { return p / k() + origin; }
    std::string scaleText() const;  // "1:50"

    int find(const std::string& id) const;  // index or -1
    Layer* layer(const std::string& name);
    const Layer* layer(const std::string& name) const;
    Layer& ensureLayer(const std::string& name);  // creates with defaults when missing
    std::string add(Entity e);                   // assigns a fresh id, returns it
    Vec2 snap(Vec2 p) const;
};

// Parses "1:50", "1/50", "2:1" or "50" (= 1:50). Returns false when malformed.
bool parseScale(const std::string& text, double& num, double& den);

// ---- measuring and formatting ---------------------------------------------

// The value a dimension measures: a length in drawing units, or degrees for angular.
double dimensionValue(const Entity& dim);
std::string formatLength(const Drawing& doc, double value);
std::string formatArea(const Drawing& doc, double area);  // m² for metric, ft² for imperial
std::string formatAngle(double degrees);
std::string dimensionText(const Drawing& doc, const Entity& dim);
// The polygon a wall's faces trace on one side (+1 left, -1 right of its centre line).
std::vector<Vec2> wallFace(const Entity& wall, int side);
// Where an opening (door/window) can sit: segment index and the distance along the
// wall where that segment starts. Returns -1 when the opening doesn't fit in one segment.
int openingSegment(const Entity& wall, double at, double width, double* segStart = nullptr);
// Nearest distance along the wall's centre line to a point.
double wallDistanceAt(const Entity& wall, Vec2 p);

// Simple entities a compound one (wall, door, window, room, dimension) is drawn with,
// in drawing units. `thin` pieces draw with a fine pen in a continuous line.
struct Piece {
    Entity e;
    bool thin = false;
};
std::vector<Piece> explode(const Drawing& doc, const Entity& e);
Box extents(const Drawing& doc);  // drawing units, visible layers only

// Similarity transforms (move, rotate, uniform scale, mirror) applied to entities.
struct Xform {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;  // x' = a x + c y + e, y' = b x + d y + f
    Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    Vec2 applyDir(Vec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
    double det() const { return a * d - b * c; }
    double scale() const { return std::sqrt(std::abs(det())); }
    bool mirrors() const { return det() < 0; }
    static Xform translate(double dx, double dy) { return {1, 0, 0, 1, dx, dy}; }
    static Xform rotate(Vec2 center, double deg);
    static Xform scaleAbout(Vec2 center, double factor);
    static Xform mirror(Vec2 p1, Vec2 p2);
};
void transformEntity(Entity& e, const Xform& xf);

// ---- files ----------------------------------------------------------------

// DXF (AC1015 / R2000 ASCII). `sheet` adds the border and title block on layer BP_SHEET.
std::string toDxf(const Drawing& doc, bool sheet = false);
// Reads LINE, LWPOLYLINE, POLYLINE/VERTEX, CIRCLE, ARC, ELLIPSE, SPLINE, TEXT, MTEXT,
// ATTRIB, POINT, DIMENSION, HATCH and SOLID, layers with colours and line types, and
// expands blocks (INSERT, nested, arrays). `notes`, when given, gets a summary of
// what was skipped. Throws std::runtime_error when it isn't DXF.
std::shared_ptr<Drawing> fromDxf(const std::string& text, std::string* notes = nullptr, bool* hadSheetInfo = nullptr);
// Reads a .dxf, or a .dwg through dwgToDxf.
std::shared_ptr<Drawing> openDxf(const std::filesystem::path& path, std::string* notes = nullptr, bool* hadSheetInfo = nullptr);

// DWG through LibreDWG (dwg2dxf / dxf2dwg) or the ODA File Converter on PATH.
// Both throw std::runtime_error saying what to install when neither is there.
bool dwgSupported();
std::string dwgToDxf(const std::filesystem::path& dwg);
void dxfToDwg(const std::string& dxf, const std::filesystem::path& dwg);

std::string toSvg(const Drawing& doc);  // whole sheet, real-world mm size
std::string toPdf(const Drawing& doc);  // one vector page at the paper size, to scale

// A paper-space window to render, mm from the paper's bottom-left corner.
struct View {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
View sheetView(const Drawing& doc);
View extentsView(const Drawing& doc);  // the drawing's extents plus a margin (whole sheet when empty)
// Rasterises a view so its longer side is `size` px (or at `dpi` when size is 0).
raster::Image render(const Drawing& doc, const View& view, int size, double dpi = 0, bool grid = false);

// .svg, .pdf, .png, .jpg, .bmp, .tga, .ppm, .dxf. Throws std::runtime_error.
void exportFile(const Drawing& doc, const std::filesystem::path& path, double dpi = 150, int quality = 90);

// Helvetica advance width of UTF-8 text at a cap height of 1.
double textWidth(const std::string& text);

// ---- commands -------------------------------------------------------------

// Undoable edit: snapshots the drawing before running `fn`, restores it on undo.
class DrawingEdit : public Action {
public:
    DrawingEdit(std::shared_ptr<Drawing> doc, std::function<Result(Drawing&)> fn)
        : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<Drawing> doc_;
    std::function<Result(Drawing&)> fn_;
    std::shared_ptr<Drawing> before_;
};

// Registers every blueprint.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::blueprint
