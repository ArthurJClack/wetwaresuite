// type: headless font design. A Font holds metrics (units per em, ascender,
// descender, x-height, cap height), naming info, glyphs (unicodes, advance
// width, closed contours of quadratic or cubic Bezier points, components) and
// kerning pairs. Files: the suite's own .typeface.json, TrueType/OpenType
// (glyf and CFF outlines) and WOFF in, UFO-lite in and out, TTF/WOFF/SVG font
// out. Coordinates are font units with y up and the baseline at y = 0.
// (The namespace is `typeface` because `type` reads badly in C++ code; the
// commands are `type.*`.)
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "achclwss/core/command.hpp"
#include "achclwss/raster/image.hpp"

namespace achclwss::typeface {

using raster::Color;
using raster::Image;

struct Vec {
    double x = 0, y = 0;
    bool operator==(const Vec&) const = default;
};

// On-curve points end segments. Off-curve points are Bezier controls: Quad
// (TrueType: consecutive quad controls imply an on-curve point midway) or
// Cubic (two between on-curve points).
enum class PointType : std::uint8_t { On, Quad, Cubic };

struct Point {
    double x = 0, y = 0;
    PointType type = PointType::On;
    bool on() const { return type == PointType::On; }
    bool operator==(const Point&) const = default;
};

// Always closed. TrueType convention: outer contours clockwise, holes counter-clockwise.
struct Contour {
    std::vector<Point> points;
    bool operator==(const Contour&) const = default;
};

// A reference to another glyph, drawn through x' = xx*x + yx*y + dx, y' = xy*x + yy*y + dy.
struct Component {
    std::string base;
    double xx = 1, xy = 0, yx = 0, yy = 1, dx = 0, dy = 0;
    bool operator==(const Component&) const = default;
};

struct Glyph {
    std::string name;
    std::vector<std::uint32_t> unicodes;
    double advance = 500;
    std::vector<Contour> contours;
    std::vector<Component> components;
};

struct Metrics {
    int unitsPerEm = 1000;
    double ascender = 800, descender = -200, xHeight = 500, capHeight = 700, lineGap = 0;
    double italicAngle = 0;  // degrees, negative leans right
    double underlinePosition = -100, underlineThickness = 50;
};

struct Info {
    std::string family = "Untitled", style = "Regular", version = "1.000";
    std::string copyright, license, licenseUrl, designer, manufacturer, description;
    std::string vendor = "NONE";  // OS/2 achVendID, 4 characters
    int weight = 400;             // OS/2 usWeightClass
};

using KernPair = std::pair<std::string, std::string>;

struct Font {
    Info info;
    Metrics metrics;
    std::vector<Glyph> glyphs;  // the export order; .notdef first
    std::map<KernPair, double> kerning;

    int index(const std::string& name) const;            // -1 when missing
    int indexForUnicode(std::uint32_t codepoint) const;  // -1 when missing
    // A glyph by name, else by a single character ("é"), "U+00E9", "uni00E9" or "0xE9". -1 when missing.
    int resolve(const std::string& ref) const;
    Glyph* glyph(const std::string& ref);
    const Glyph* glyph(const std::string& ref) const;
    double kern(const std::string& left, const std::string& right) const;
    std::string familyStyle() const { return info.family + " " + info.style; }
};

// x' = a*x + c*y + e, y' = b*x + d*y + f.
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Vec apply(Vec p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    Affine operator*(const Affine& m) const;  // this applied after m
    bool identity() const { return a == 1 && b == 0 && c == 0 && d == 1 && e == 0 && f == 0; }
    bool translationOnly() const { return a == 1 && b == 0 && c == 0 && d == 1; }
    static Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Affine scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    static Affine rotate(double degrees);  // counter-clockwise, y up
    static Affine skewX(double degrees);   // positive leans right (x += y * tan)
};

// ---- naming and text ------------------------------------------------------------

std::vector<std::uint32_t> utf8Decode(const std::string& text);
std::string utf8Encode(std::uint32_t codepoint);
// "A" (one character), "U+0041", "u+41", "0x41", "uni0041" or "u1F600". nullopt when it is none of those.
std::optional<std::uint32_t> parseUnicode(const std::string& text);
std::string unicodeLabel(std::uint32_t codepoint);  // "U+0041"
// Standard names for ASCII ("A", "zero", "exclam"), uniXXXX / uXXXXXX for the rest.
std::string glyphNameFor(std::uint32_t codepoint);
std::optional<std::uint32_t> unicodeForName(const std::string& name);
const std::vector<std::string>& macGlyphNames();  // the 258 standard Macintosh names of `post` format 2
bool validGlyphName(const std::string& name);

// ---- geometry -------------------------------------------------------------------

// order 1: p[0]-p[1] line; 2: quadratic p[0] p[1] p[2]; 3: cubic p[0]..p[3].
struct Segment {
    int order = 1;
    Vec p[4];
    Vec end() const { return p[order]; }
};

std::vector<Segment> segments(const Contour& contour);  // implied TrueType on-curve points made explicit
Contour fromSegments(const std::vector<Segment>& segs);  // closed; the last end should equal the first start

using Polygon = std::vector<Vec>;
// Curves split into lines no further than `tolerance` units from the curve.
Polygon flatten(const Contour& contour, double tolerance);
std::vector<Polygon> flatten(const std::vector<Contour>& contours, double tolerance);

struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty = true;
    void add(Vec p);
    void add(const Box& b);
    double width() const { return empty ? 0 : x1 - x0; }
    double height() const { return empty ? 0 : y1 - y0; }
};
Box bounds(const std::vector<Contour>& contours);  // of the curves, not just their points

// The glyph's contours with every component drawn in (nested components too).
std::vector<Contour> outline(const Font& font, const Glyph& glyph);
Box glyphBounds(const Font& font, const Glyph& glyph);
void transform(std::vector<Contour>& contours, const Affine& m);
void transform(Contour& contour, const Affine& m);

double signedArea(const Polygon& polygon);  // positive = counter-clockwise with y up
double signedArea(const Contour& contour);
void reverse(Contour& contour);  // keeps an on-curve point first
// Outer contours clockwise and holes counter-clockwise (the TrueType rule), by nesting depth.
// Returns how many contours were reversed.
int fixDirections(std::vector<Contour>& contours);

// Cubic segments become quadratic ones within `tolerance` units; quadratics are kept.
Contour toQuadratic(const Contour& contour, double tolerance = 0.5);
// Quadratic segments become exact cubics.
Contour toCubic(const Contour& contour);

// SVG path data (M L H V C S Q T A Z, absolute and relative). Curves are kept: Q/T become
// quadratic points, C/S cubic, arcs cubic. Every subpath is closed. On malformed data the
// path is kept up to the error and `error` is set. pathData writes contours back, through `m`.
std::vector<Contour> parsePathData(const std::string& d, std::string* error = nullptr);
std::string pathData(const std::vector<Contour>& contours, const Affine& m = {});

// Moves every point outward from the ink by dx horizontally and dy vertically (negative thins).
std::vector<Contour> embolden(const std::vector<Contour>& contours, double dx, double dy);
// Union of the filled area (nonzero rule): contours that cross are rebuilt from their
// flattened edges and refitted with curves; contours that touch nothing are kept as they are.
std::vector<Contour> removeOverlaps(const std::vector<Contour>& contours, double tolerance);

// Fits cubic Beziers through a closed polyline (Schneider's method), keeping corners sharper
// than `cornerDegrees`. Straight runs become lines.
Contour fitCurves(const Polygon& closed, double tolerance, double cornerDegrees = 70);
// A smooth closed curve through the points (Catmull-Rom as cubics).
Contour smoothThrough(const Polygon& points);
Contour polygonContour(const Polygon& points);
Contour rectContour(double x, double y, double w, double h);  // clockwise
Contour ellipseContour(double cx, double cy, double rx, double ry);  // clockwise, 4 cubics

// ---- font-level operations ------------------------------------------------------

// A new font with .notdef (a box) and space.
Font newFont(const std::string& family, const std::string& style, int unitsPerEm, double ascender, double descender,
             double xHeight, double capHeight);
// Every coordinate, advance, metric and kerning value times `factor` (for a new units per em).
void scaleFont(Font& font, double factor);
// Moves the glyph so its left side bearing is `lsb`, and sets the advance so the right side bearing is `rsb`.
void setSidebearings(const Font& font, Glyph& glyph, std::optional<double> lsb, std::optional<double> rsb);
double leftBearing(const Font& font, const Glyph& glyph);
double rightBearing(const Font& font, const Glyph& glyph);
// The glyph's components drawn into its contours.
void decompose(const Font& font, Glyph& glyph);
// Removes the glyph's name from kerning and draws it into glyphs that use it as a component.
void removeGlyph(Font& font, int index);
// Removes many glyphs at once (indexes in any order): their kerning goes, glyphs that keep using them
// as components get them drawn in.
void removeGlyphs(Font& font, const std::vector<int>& indexes);
void renameGlyph(Font& font, int index, const std::string& name);

// Reads x-height, cap height, ascender and descender off x/H/d/p-like glyphs. Returns what it changed.
std::vector<std::string> detectVerticalMetrics(Font& font);

struct AutoKernOptions {
    double target = 0;      // wanted gap between shapes in units; 0 = measured from "HH" (or "nn")
    double strength = 1.0;  // how much of the measured difference to apply
    double minimum = 0;     // smallest kern kept (absolute); 0 = 1.5% of the em
    double maximum = 0;     // largest kern allowed (absolute); 0 = 30% of the em
    int bands = 24;         // horizontal slices compared between the shapes
};
// Kerning for every ordered pair of `glyphs` (names), from their horizontal profiles.
std::map<KernPair, double> autoKern(const Font& font, const std::vector<std::string>& glyphs,
                                    const AutoKernOptions& options, double* measuredTarget = nullptr);

// ---- files ----------------------------------------------------------------------

// .typeface.json, .ttf/.otf/.ttc/.woff, a .ufo folder. Throws std::runtime_error. Warnings collect
// what couldn't be read (hinting, unsupported kerning formats, ...).
Font loadFont(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);

std::string toJson(const Font& font);
Font fromJson(const std::string& text);

// sfnt bytes (TrueType, OpenType CFF, collections: the first font) or WOFF 1.0.
Font fromSfnt(const std::string& bytes, std::vector<std::string>* warnings = nullptr);
std::string woffToSfnt(const std::string& woff);

struct TtfOptions {
    std::string kerning = "both";  // both (kern + GPOS), gpos, kern, none
    double tolerance = 0.5;        // cubic to quadratic conversion, in units
    std::vector<std::string>* warnings = nullptr;
};
// Every table of a TrueType font, by tag ("head", "glyf", "OS/2", ...), before assembly.
std::map<std::string, std::string> buildTables(const Font& font, const TtfOptions& options = {});
// The sfnt file: directory, 4-byte aligned tables, checksums, head.checkSumAdjustment.
std::string assembleSfnt(const std::map<std::string, std::string>& tables, std::uint32_t flavor = 0x00010000);
std::string toTtf(const Font& font, const TtfOptions& options = {});
std::string toWoff(const std::string& sfnt);
std::string toSvgFont(const Font& font);

Font loadUfo(const std::filesystem::path& dir, std::vector<std::string>* warnings = nullptr);
void saveUfo(const Font& font, const std::filesystem::path& dir);

// SVG drawing -> contours in font units. fit: "canvas" (the SVG's height spans descender to
// ascender), "cap" / "xheight" / "ascender" (the ink's height spans baseline to that line),
// "none" (one SVG unit is one font unit, the SVG's bottom edge on the baseline).
// `advance`, when given, gets the width the drawing implies (canvas: the SVG's width scaled;
// none: the SVG's width; other fits: 0, set the sidebearings instead).
std::vector<Contour> importSvg(const std::string& svgText, const Metrics& metrics, const std::string& fit, double* advance = nullptr);

// ---- rendering ------------------------------------------------------------------

// Fills contours with the nonzero rule, antialiased; `toPixels` maps font units to pixels
// (usually with a y flip).
void fillContours(Image& image, const std::vector<Contour>& contours, const Affine& toPixels, Color color);

struct TextStyle {
    double size = 48;  // px per em
    Color color{0, 0, 0, 255};
    bool kerning = true;
    double tracking = 0;    // extra units between glyphs
    double lineHeight = 0;  // px; 0 = from the font's ascender, descender and line gap
};
// Glyph indexes for the text: missing characters map to .notdef (or -1 when there's none).
std::vector<int> glyphRun(const Font& font, const std::string& text);
// Width in font units of one line, with kerning and tracking.
double textAdvance(const Font& font, const std::string& text, const TextStyle& style);
// Draws one line with its baseline at y. Returns the advance in pixels.
double drawText(Image& image, const Font& font, const std::string& text, double x, double y, const TextStyle& style);
// Text (lines split at \n, wrapped at maxWidth px when > 0) on a new image.
Image renderText(const Font& font, const std::string& text, const TextStyle& style, Color background, int padding = 16,
                 double maxWidth = 0);

struct GlyphViewOptions {
    int size = 512;
    bool points = true;   // on-curve squares, off-curve circles and handles
    bool metrics = true;  // baseline, x-height, cap height, ascender, descender, advance
    bool fill = true;
};
Image glyphView(const Font& font, const Glyph& glyph, const GlyphViewOptions& options = {});
// A grid of glyphs with names: what a font's preview shows.
Image overview(const Font& font, int cell = 96, int columns = 10, int maxGlyphs = 160);

struct ProofOptions {
    std::string title;  // default: family and style
    std::string sample = "The quick brown fox jumps over the lazy dog.";
    std::string paragraph;  // default: a pangram paragraph
    std::vector<double> sizes = {72, 48, 36, 24, 18, 14, 12};
    bool charset = true;   // every encoded glyph
    bool grid = true;      // glyph cells with metrics and names
    bool kerning = true;   // the strongest kerning pairs, with and without
    double width = 1200;   // PNG width in px
};
Image proofImage(const Font& font, const ProofOptions& options);
std::string proofPdf(const Font& font, const ProofOptions& options);

// ---- tracing (font from handwriting) --------------------------------------------

struct TraceOptions {
    double threshold = 0.5;  // ink above this (0..1, darkness times alpha) is inside
    bool invert = false;     // light ink on a dark ground
    double tolerance = 1.0;  // curve fit error in pixels
    double minArea = 12;     // blobs smaller than this many px are dropped
    double corner = 70;      // turns sharper than this (degrees) stay corners
};
// Outlines of the ink in an image, in pixel coordinates (y down).
std::vector<Polygon> traceOutlines(const Image& image, const TraceOptions& options);
// Traced, curve-fitted contours mapped through `pixelToFont`, with fixed directions.
std::vector<Contour> traceImage(const Image& image, const TraceOptions& options, const Affine& pixelToFont);

struct GridLayout {
    int rows = 1, cols = 1;
    double marginX = 0, marginY = 0;  // px around the whole grid
    double inset = 0.06;              // fraction of each cell ignored at its edges (grid lines, labels)
    double baseline = -1;             // baseline from the cell top as a fraction; <0 = from the metrics
};
struct TracedCell {
    std::uint32_t codepoint = 0;
    std::vector<Contour> contours;  // font units, baseline at y = 0, ink starting near x = 0
};
// Slices a scanned sheet into rows x cols cells and traces each one. Cells map to the em:
// the cell's top is the ascender and its bottom the descender (unless layout.baseline says
// where the baseline is). `chars` lists the cells in reading order; 0 skips a cell.
std::vector<TracedCell> traceGrid(const Image& sheet, const std::vector<std::uint32_t>& chars, const GridLayout& layout,
                                  const Metrics& metrics, const TraceOptions& options);
// A template sheet to draw letters on: cells with baseline, x-height and cap guides and a faint label.
Image gridTemplate(const Font& font, const std::vector<std::uint32_t>& chars, int cols, int cell);

// ---- documents and commands -----------------------------------------------------

class FontDoc : public Document {
public:
    Font font;
    std::string kind() const override { return "font"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
};

// Undoable edit: snapshots the font before `fn`, restores it on undo or failure.
class FontEdit : public Action {
public:
    FontEdit(std::shared_ptr<FontDoc> doc, std::function<Result(FontDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<FontDoc> doc_;
    std::function<Result(FontDoc&)> fn_;
    Font before_;
};

// Registers every type.* command.
void registerCommands(Session& session);

}  // namespace achclwss::typeface
