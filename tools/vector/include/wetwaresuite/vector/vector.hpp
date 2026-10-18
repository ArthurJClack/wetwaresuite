// The vector tool: an Illustrator/Inkscape-style editor that works headless
// first. A VectorDoc is a canvas plus an ordered stack of shapes; every edit is
// a command (vector.add_rect, vector.style, ...) so the CLI, batch scripts and
// Claude over MCP drive it the same way. Rendering goes through the raster
// library; files are real SVG.
#pragma once

#include <filesystem>
#include <functional>
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

enum class ShapeKind { Rect, Ellipse, Line, Polyline, Polygon, Path };

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
    static Matrix translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Matrix scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    static Matrix rotate(double degrees);
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

    std::optional<Color> fill;    // nullopt = none
    std::optional<Color> stroke;  // nullopt = none
    double strokeWidth = 1;
    double opacity = 1;
    bool visible = true;
    Transform transform;
};

class VectorDoc : public Document {
public:
    int width = 800;
    int height = 600;
    Color background{255, 255, 255, 255};
    std::vector<Shape> shapes;  // bottom first
    int nextId = 1;

    std::string kind() const override { return "vector"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;

    // Finds by id ("s3") first, then by name. Returns -1 when missing.
    int find(const std::string& idOrName) const;
    // Gives the shape a fresh id (and a default name if it has none), appends it, returns the id.
    std::string add(Shape shape);
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
};
Bounds bounds(const Outline& outline);

// Parses SVG path data (M L H V C S Q T A Z, absolute and relative) and
// flattens curves into line segments. `detail` is the zoom the result will be
// drawn at (more segments when larger). On malformed data the path is kept up
// to the error and `error`, when given, is set.
Outline flattenPath(const std::string& d, double detail = 1.0, std::string* error = nullptr);

// The shape in its own coordinates, before its transform.
Outline localOutline(const Shape& shape, double detail = 1.0);
// The matrix the shape's transform stands for (needs the local bounding box centre).
Matrix shapeMatrix(const Shape& shape);
// The shape on the canvas, after its transform.
Outline outline(const Shape& shape, double detail = 1.0);

// Colour names and codes: everything raster::Color::parse reads plus more CSS names (teal, navy, gold, ...).
std::optional<Color> parseColor(const std::string& text);

// ---- rendering and files --------------------------------------------------

// Rasterises the document at `scale` (1 = one pixel per unit), antialiased.
raster::Image render(const VectorDoc& doc, double scale = 1.0);

std::string toSvg(const VectorDoc& doc);
// Reads a practical SVG subset: rect, circle, ellipse, line, polyline, polygon,
// path and g, with fill/stroke/opacity attributes or style, and transforms.
// Unknown elements are skipped. Throws std::runtime_error when it isn't SVG.
std::shared_ptr<VectorDoc> fromSvg(const std::string& text);

std::shared_ptr<VectorDoc> openSvg(const std::filesystem::path& path);
// .svg writes SVG; png/jpg/bmp/tga/ppm rasterise at `scale`. Throws std::runtime_error.
void saveDoc(const VectorDoc& doc, const std::filesystem::path& path, double scale = 1.0, int quality = 90);

// ---- commands -------------------------------------------------------------

// Undoable edit: snapshots the document before running `fn`, restores it on undo.
class VectorEdit : public Action {
public:
    VectorEdit(std::shared_ptr<VectorDoc> doc, std::function<Result(VectorDoc&)> fn)
        : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    struct Snapshot {
        int width = 0, height = 0;
        Color background;
        std::vector<Shape> shapes;
        int nextId = 1;
    };
    std::shared_ptr<VectorDoc> doc_;
    std::function<Result(VectorDoc&)> fn_;
    Snapshot before_;
};

// Registers every vector.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::vector
