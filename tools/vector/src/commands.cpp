// Every vector.* command. Edits run inside a VectorEdit so each one is a single
// undo step; read-only commands (info, preview, save) stay out of the history.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/document.hpp"
#include "achclwss/vector/vector.hpp"

namespace achclwss::vector {

Result VectorEdit::apply() {
    before_ = {doc_->width, doc_->height, doc_->background, doc_->shapes, doc_->nextId};
    Result r = fn_(*doc_);
    if (!r.ok) revert();  // failed edits leave the document untouched
    return r;
}

void VectorEdit::revert() {
    doc_->width = before_.width;
    doc_->height = before_.height;
    doc_->background = before_.background;
    doc_->shapes = before_.shapes;
    doc_->nextId = before_.nextId;
}

namespace {

using Edit = std::function<Result(VectorDoc&, const Params&)>;

std::string num(double v) {
    if (std::abs(v) < 5e-4) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string paintText(const std::optional<Color>& c) {
    if (!c || c->a == 0) return "none";
    std::string hex = c->hex();
    return c->a == 255 ? hex.substr(0, 7) : hex;
}

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

// "none" -> no paint; anything else must be a colour.
bool paintArg(const std::string& text, std::optional<Color>& out, std::string& error) {
    std::string t;
    for (char c : text) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "none" || t.empty()) {
        out.reset();
        return true;
    }
    auto c = parseColor(text);
    if (!c) {
        error = "'" + text + "' isn't a colour; use #rrggbb, #rrggbbaa, rgb(r,g,b), a name like red, or none";
        return false;
    }
    out = *c;
    return true;
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open vector document, e.g. vec1", true}; }
ParamSpec shapeParam() { return {"shape", ParamType::String, "shape id (e.g. s3, from vector.info) or shape name", true}; }
ParamSpec previewParam() { return raster::previewParam(); }

ParamSpec numParam(const std::string& name, const std::string& description, bool required = true, Value fallback = {}) {
    return {name, ParamType::Number, description, required, {}, std::move(fallback)};
}

ParamSpec paintParam(const std::string& name, const std::string& what, const std::string& fallback) {
    return {name, ParamType::String, what + ": #rrggbb, #rrggbbaa, rgb(r,g,b), a name like red, or none", false, {},
            fallback.empty() ? Value{} : Value{fallback}};
}

// Style params shared by the add_* commands, with per-kind defaults.
std::vector<ParamSpec> styleParams(const std::string& fill, const std::string& stroke, double strokeWidth) {
    std::vector<ParamSpec> out;
    if (!fill.empty()) out.push_back(paintParam("fill", "fill colour", fill));
    out.push_back(paintParam("stroke", "outline colour", stroke));
    out.push_back(numParam("stroke_width", "outline width in px", false, strokeWidth));
    out.push_back(numParam("opacity", "opacity of the whole shape, 0..1", false, 1.0));
    out.push_back({"name", ParamType::String, "a name to find the shape by later (default: kind + number)", false});
    out.push_back(previewParam());
    return out;
}

std::vector<ParamSpec> join(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Applies fill/stroke/stroke_width/opacity/name/visible params that are present.
bool applyStyle(Shape& s, const Params& p, std::string& error) {
    if (has(p, "fill") && !paintArg(stringParam(p, "fill"), s.fill, error)) return false;
    if (has(p, "stroke") && !paintArg(stringParam(p, "stroke"), s.stroke, error)) return false;
    if (has(p, "stroke_width")) {
        double w = numberParam(p, "stroke_width");
        if (w < 0) {
            error = "stroke_width can't be negative";
            return false;
        }
        s.strokeWidth = w;
    }
    if (has(p, "opacity")) s.opacity = std::clamp(numberParam(p, "opacity"), 0.0, 1.0);
    if (has(p, "name")) s.name = stringParam(p, "name");
    if (has(p, "visible")) s.visible = boolParam(p, "visible", true);
    return true;
}

Result& maybePreview(Result& r, const VectorDoc& doc, const Params& p) {
    if (r.ok && boolParam(p, "preview", false)) r.attach(*doc.preview());
    return r;
}

ActionFactory vectorCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<VectorDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] {
                return Result::failure("no open vector document called '" + id + "' (use vector.new or vector.open first)");
            });
        }
        if (!undoable) return std::make_unique<FunctionAction>([doc, edit, p] { return edit(*doc, p); });
        return std::make_unique<VectorEdit>(doc, [edit, p](VectorDoc& d) { return edit(d, p); });
    };
}

// Runs `fn` on the shape named by the "shape" param.
Edit onShape(std::function<Result(VectorDoc&, int, const Params&)> fn) {
    return [fn](VectorDoc& doc, const Params& p) {
        const std::string name = stringParam(p, "shape");
        int idx = doc.find(name);
        if (idx < 0) return Result::failure("no shape '" + name + "' in this document (vector.info lists them)");
        Result r = fn(doc, idx, p);
        return maybePreview(r, doc, p);
    };
}

// Shared tail of every add_* command.
Result addShape(VectorDoc& doc, Shape s, const Params& p) {
    std::string error;
    if (!applyStyle(s, p, error)) return Result::failure(error);
    const std::string kind = toString(s.kind);
    std::string id = doc.add(std::move(s));
    Result r = Result::success("added " + kind + " " + id, id);
    r.with("shape", id);
    return maybePreview(r, doc, p);
}

bool pointsArg(const Params& p, std::size_t minPoints, std::vector<Point>& out, std::string& error) {
    NumberList v = listParam(p, "points");
    if (v.size() % 2 != 0 || v.size() / 2 < minPoints) {
        error = "points must be x,y pairs (at least " + std::to_string(minPoints) + " points, e.g. 10,10,90,10,50,80)";
        return false;
    }
    for (std::size_t i = 0; i + 1 < v.size(); i += 2) out.push_back({v[i], v[i + 1]});
    return true;
}

std::string describe(const Shape& s) {
    std::string g;
    switch (s.kind) {
        case ShapeKind::Rect:
            g = "x=" + num(s.x) + " y=" + num(s.y) + " w=" + num(s.w) + " h=" + num(s.h);
            if (s.radius > 0) g += " radius=" + num(s.radius);
            break;
        case ShapeKind::Ellipse: g = "cx=" + num(s.cx) + " cy=" + num(s.cy) + " rx=" + num(s.rx) + " ry=" + num(s.ry); break;
        case ShapeKind::Line:
        case ShapeKind::Polyline:
        case ShapeKind::Polygon:
            g = "points=";
            for (std::size_t i = 0; i < s.points.size(); ++i) {
                if (i == 8) {
                    g += " ... (" + std::to_string(s.points.size()) + " points)";
                    break;
                }
                g += (i ? " " : "") + num(s.points[i].x) + "," + num(s.points[i].y);
            }
            break;
        case ShapeKind::Path: g = "d=\"" + (s.d.size() > 80 ? s.d.substr(0, 77) + "..." : s.d) + "\""; break;
    }
    std::string out = s.id + " " + toString(s.kind) + " \"" + s.name + "\" " + g + " fill=" + paintText(s.fill) +
                      " stroke=" + paintText(s.stroke);
    if (s.stroke) out += " stroke_width=" + num(s.strokeWidth);
    if (s.opacity < 1) out += " opacity=" + num(s.opacity);
    const Transform& t = s.transform;
    if (!t.identity()) {
        out += " transform=(";
        if (t.tx != 0 || t.ty != 0) out += "move " + num(t.tx) + "," + num(t.ty) + " ";
        if (t.rotate != 0) out += "rotate " + num(t.rotate) + " ";
        if (t.sx != 1 || t.sy != 1) out += "scale " + num(t.sx) + "," + num(t.sy) + " ";
        out.back() = ')';
    }
    Bounds b = bounds(outline(s));
    if (!b.empty) out += " bounds=" + num(b.x0) + "," + num(b.y0) + " " + num(b.width()) + "x" + num(b.height());
    if (!s.visible) out += " hidden";
    return out;
}

Result docCreated(Session& session, std::shared_ptr<VectorDoc> doc, const std::string& verb) {
    const auto shapes = static_cast<std::int64_t>(doc->shapes.size());
    const int w = doc->width, h = doc->height;
    std::string id = session.workspace().add(doc, "vec");
    Result r = Result::success(verb + " " + id + " (" + std::to_string(w) + "x" + std::to_string(h) + ", " +
                                   std::to_string(shapes) + (shapes == 1 ? " shape)" : " shapes)"),
                               id);
    r.with("doc", id).with("width", static_cast<std::int64_t>(w)).with("height", static_cast<std::int64_t>(h));
    r.with("shapes", shapes);
    return r;
}

// Closing is undoable: undo puts the document back under the same id.
class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

}  // namespace

void registerCommands(Session& session) {
    auto& reg = session.registry();

    // ---- documents ------------------------------------------------------------

    reg.add({"vector.new", "New Drawing",
             "Create a new, empty vector drawing and return its document id (e.g. vec1). Units are pixels; "
             "the origin is the top-left corner and y grows downwards.",
             {{"width", ParamType::Int, "canvas width in px", false, {}, std::int64_t{800}},
              {"height", ParamType::Int, "canvas height in px", false, {}, std::int64_t{600}},
              paintParam("background", "background colour", "white")},
             false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto w = intParam(p, "width", 800), h = intParam(p, "height", 600);
                    if (w < 1 || h < 1 || w > 30000 || h > 30000) return Result::failure("width and height must be 1..30000");
                    auto doc = std::make_shared<VectorDoc>();
                    doc->width = static_cast<int>(w);
                    doc->height = static_cast<int>(h);
                    std::optional<Color> bg;
                    std::string error;
                    if (!paintArg(stringParam(p, "background", "white"), bg, error)) return Result::failure(error);
                    doc->background = bg.value_or(Color{0, 0, 0, 0});
                    return docCreated(session, doc, "created");
                });
            });

    reg.add({"vector.open", "Open Drawing",
             "Open an SVG file as a vector document and return its id. Supports rect, circle, ellipse, line, polyline, "
             "polygon, path and groups with fill/stroke/opacity and transforms; gradients, text and images are skipped.",
             {{"path", ParamType::String, "path to an .svg file", true}},
             false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        return docCreated(session, openSvg(stringParam(p, "path")), "opened");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"vector.save", "Save Drawing",
             "Save a vector document. .svg writes real SVG; .png, .jpg, .bmp, .tga or .ppm render it to pixels "
             "(use scale for higher resolution, e.g. scale=2).",
             {docParam(),
              {"path", ParamType::String, "output file; the extension picks the format", true},
              numParam("scale", "pixels per unit for raster formats", false, 1.0),
              {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{90}}},
             false},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const std::string path = stringParam(p, "path");
                const double scale = numberParam(p, "scale", 1.0);
                if (!(scale > 0) || scale > 32) return Result::failure("scale must be between 0 and 32");
                try {
                    saveDoc(doc, path, scale, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "quality", 90), 1, 100)));
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
                std::filesystem::path fp(path);
                std::string e = fp.extension().string();
                for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (e == ".svg") doc.path = path;
                return Result::success("saved " + path, path).with("path", path);
            }, false));

    reg.add({"vector.convert", "Convert Drawing",
             "Render or re-save an SVG file in one step, without opening it first: input=logo.svg output=logo.png. "
             "The output extension picks the format (svg, png, jpg, bmp, tga, ppm).",
             {{"input", ParamType::String, "path to an .svg file", true},
              {"output", ParamType::String, "output file; the extension picks the format", true},
              numParam("scale", "pixels per unit for raster formats", false, 1.0),
              {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{90}}},
             false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    const std::string input = stringParam(p, "input"), output = stringParam(p, "output");
                    const double scale = numberParam(p, "scale", 1.0);
                    if (!(scale > 0) || scale > 32) return Result::failure("scale must be between 0 and 32");
                    try {
                        auto doc = openSvg(input);
                        saveDoc(*doc, output, scale, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "quality", 90), 1, 100)));
                        return Result::success("converted " + input + " to " + output + " (" +
                                                   std::to_string(doc->shapes.size()) + " shapes)",
                                               output)
                            .with("path", output)
                            .with("shapes", static_cast<std::int64_t>(doc->shapes.size()));
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"vector.close", "Close Drawing", "Close a vector document (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                const std::string id = stringParam(p, "doc");
                if (!session.workspace().getAs<VectorDoc>(id)) {
                    return std::make_unique<FunctionAction>(
                        [id] { return Result::failure("no open vector document called '" + id + "'"); });
                }
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"vector.info", "Drawing Info",
             "Describe a vector document: canvas size, background and every shape bottom to top with its id, name, "
             "geometry, colours, transform and bounding box on the canvas.",
             {docParam()},
             false},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::string text = stringParam(p, "doc") + ": " + std::to_string(doc.width) + "x" +
                                   std::to_string(doc.height) + ", background " + paintText(doc.background) + ", " +
                                   std::to_string(doc.shapes.size()) + " shapes (bottom first)";
                std::string ids;
                for (const auto& s : doc.shapes) {
                    text += "\n" + describe(s);
                    ids += (ids.empty() ? "" : ",") + s.id;
                }
                Result r = Result::success(text);
                r.with("doc", stringParam(p, "doc"))
                    .with("width", static_cast<std::int64_t>(doc.width))
                    .with("height", static_cast<std::int64_t>(doc.height))
                    .with("background", paintText(doc.background))
                    .with("shapes", static_cast<std::int64_t>(doc.shapes.size()))
                    .with("ids", ids);
                return r;
            }, false));

    reg.add({"vector.preview", "Preview Drawing",
             "Render the drawing to a PNG image so you can see it. size is the longest side in px.",
             {docParam(), {"size", ParamType::Int, "longest side of the preview in px", false, {}, std::int64_t{512}}},
             false},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const auto size = std::clamp<std::int64_t>(intParam(p, "size", 512), 16, 4096);
                const double scale = static_cast<double>(size) / std::max(doc.width, doc.height);
                raster::Image img = render(doc, scale);
                Result r = Result::success("preview " + std::to_string(img.width()) + "x" + std::to_string(img.height()));
                r.attach({"image/png", raster::encodePng(img), "preview.png"});
                return r;
            }, false));

    reg.add({"vector.canvas", "Canvas Size",
             "Change the canvas size and/or background colour. Shapes keep their positions.",
             {docParam(),
              {"width", ParamType::Int, "new width in px", false},
              {"height", ParamType::Int, "new height in px", false},
              paintParam("background", "background colour", ""),
              previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                auto w = intParam(p, "width", doc.width), h = intParam(p, "height", doc.height);
                if (w < 1 || h < 1 || w > 30000 || h > 30000) return Result::failure("width and height must be 1..30000");
                doc.width = static_cast<int>(w);
                doc.height = static_cast<int>(h);
                if (has(p, "background")) {
                    std::optional<Color> bg;
                    std::string error;
                    if (!paintArg(stringParam(p, "background"), bg, error)) return Result::failure(error);
                    doc.background = bg.value_or(Color{0, 0, 0, 0});
                }
                Result r = Result::success("canvas " + std::to_string(w) + "x" + std::to_string(h) + ", background " +
                                           paintText(doc.background));
                return maybePreview(r, doc, p);
            }));

    // ---- adding shapes --------------------------------------------------------------

    reg.add({"vector.add_rect", "Add Rectangle",
             "Add a rectangle (optionally with rounded corners) on top of the drawing. Returns the new shape id.",
             join({docParam(), numParam("x", "left edge"), numParam("y", "top edge"), numParam("width", "width"), numParam("height", "height"),
                   numParam("radius", "corner radius", false, 0.0)},
                  styleParams("black", "none", 1)),
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                Shape s;
                s.kind = ShapeKind::Rect;
                s.x = numberParam(p, "x");
                s.y = numberParam(p, "y");
                s.w = numberParam(p, "width");
                s.h = numberParam(p, "height");
                s.radius = std::max(0.0, numberParam(p, "radius", 0));
                if (s.w <= 0 || s.h <= 0) return Result::failure("width and height must be positive");
                s.fill = Color{0, 0, 0, 255};
                return addShape(doc, std::move(s), p);
            }));

    reg.add({"vector.add_ellipse", "Add Ellipse",
             "Add an ellipse or circle (leave ry out for a circle) centred on cx,cy. Returns the new shape id.",
             join({docParam(), numParam("cx", "centre x"), numParam("cy", "centre y"), numParam("rx", "horizontal radius"),
                   numParam("ry", "vertical radius (default: rx, a circle)", false)},
                  styleParams("black", "none", 1)),
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                Shape s;
                s.kind = ShapeKind::Ellipse;
                s.cx = numberParam(p, "cx");
                s.cy = numberParam(p, "cy");
                s.rx = numberParam(p, "rx");
                s.ry = numberParam(p, "ry", s.rx);
                if (s.rx <= 0 || s.ry <= 0) return Result::failure("rx and ry must be positive");
                s.fill = Color{0, 0, 0, 255};
                return addShape(doc, std::move(s), p);
            }));

    reg.add({"vector.add_line", "Add Line", "Add a straight line from x1,y1 to x2,y2. Returns the new shape id.",
             join({docParam(), numParam("x1", "start x"), numParam("y1", "start y"), numParam("x2", "end x"), numParam("y2", "end y")},
                  styleParams("", "black", 2)),
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                Shape s;
                s.kind = ShapeKind::Line;
                s.points = {{numberParam(p, "x1"), numberParam(p, "y1")}, {numberParam(p, "x2"), numberParam(p, "y2")}};
                s.stroke = Color{0, 0, 0, 255};
                s.strokeWidth = 2;
                return addShape(doc, std::move(s), p);
            }));

    reg.add({"vector.add_polygon", "Add Polygon",
             "Add a closed polygon through the given points (x,y,x,y,...; at least 3 points). Returns the new shape id.",
             join({docParam(), {"points", ParamType::NumberList, "corner coordinates as x,y,x,y,...", true}},
                  styleParams("black", "none", 1)),
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                Shape s;
                s.kind = ShapeKind::Polygon;
                std::string error;
                if (!pointsArg(p, 3, s.points, error)) return Result::failure(error);
                s.fill = Color{0, 0, 0, 255};
                return addShape(doc, std::move(s), p);
            }));

    reg.add({"vector.add_polyline", "Add Polyline",
             "Add an open line through the given points (x,y,x,y,...; at least 2 points). Returns the new shape id.",
             join({docParam(), {"points", ParamType::NumberList, "coordinates as x,y,x,y,...", true}},
                  styleParams("none", "black", 2)),
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                Shape s;
                s.kind = ShapeKind::Polyline;
                std::string error;
                if (!pointsArg(p, 2, s.points, error)) return Result::failure(error);
                s.stroke = Color{0, 0, 0, 255};
                s.strokeWidth = 2;
                return addShape(doc, std::move(s), p);
            }));

    reg.add({"vector.add_path", "Add Path",
             "Add a shape from SVG path data, e.g. d=\"M10 10 L90 10 Q50 90 10 10 Z\". Supports M L H V C S Q T A Z "
             "(uppercase absolute, lowercase relative). Fill uses the even-odd rule, so inner subpaths make holes.",
             join({docParam(), {"d", ParamType::String, "SVG path data", true}}, styleParams("black", "none", 1)),
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                Shape s;
                s.kind = ShapeKind::Path;
                s.d = stringParam(p, "d");
                std::string error;
                Outline o = flattenPath(s.d, 1.0, &error);
                if (!error.empty()) return Result::failure("bad path data: " + error);
                if (o.empty()) return Result::failure("path data draws nothing (it needs at least M x y L x y)");
                s.fill = Color{0, 0, 0, 255};
                return addShape(doc, std::move(s), p);
            }));

    // ---- editing shapes -------------------------------------------------------------

    reg.add({"vector.style", "Style Shape",
             "Change a shape's fill, outline, outline width, opacity, name or visibility. Only the params you give change.",
             {docParam(), shapeParam(), paintParam("fill", "fill colour", ""), paintParam("stroke", "outline colour", ""),
              numParam("stroke_width", "outline width in px", false), numParam("opacity", "opacity 0..1", false),
              {"name", ParamType::String, "new name", false},
              {"visible", ParamType::Bool, "show or hide the shape", false},
              previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                std::string error;
                if (!applyStyle(s, p, error)) return Result::failure(error);
                return Result::success("styled " + s.id + ": " + describe(s), s.id).with("shape", s.id);
            })));

    reg.add({"vector.transform", "Transform Shape",
             "Move, rotate or scale a shape. dx/dy move it, rotate turns it clockwise by degrees around its centre, "
             "scale (or scale_x/scale_y) multiplies its size around its centre. Changes add up; reset=true clears "
             "the transform first.",
             {docParam(), shapeParam(), numParam("dx", "move right by px (negative = left)", false, 0.0),
              numParam("dy", "move down by px (negative = up)", false, 0.0),
              numParam("rotate", "degrees clockwise to add", false, 0.0), numParam("scale", "uniform scale factor", false, 1.0),
              numParam("scale_x", "horizontal scale factor", false, 1.0), numParam("scale_y", "vertical scale factor", false, 1.0),
              {"reset", ParamType::Bool, "clear the existing transform before applying these", false, {}, false},
              previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                const double k = numberParam(p, "scale", 1), kx = numberParam(p, "scale_x", 1), ky = numberParam(p, "scale_y", 1);
                if (k == 0 || kx == 0 || ky == 0) return Result::failure("scale factors can't be 0");
                if (boolParam(p, "reset", false)) s.transform = {};
                Transform& t = s.transform;
                t.tx += numberParam(p, "dx", 0);
                t.ty += numberParam(p, "dy", 0);
                t.rotate = std::fmod(t.rotate + numberParam(p, "rotate", 0), 360.0);
                t.sx *= k * kx;
                t.sy *= k * ky;
                return Result::success("transformed " + s.id + ": " + describe(s), s.id).with("shape", s.id);
            })));

    reg.add({"vector.align", "Align Shape",
             "Align a shape's bounding box to the canvas: horizontal left/center/right, vertical top/middle/bottom.",
             {docParam(), shapeParam(),
              {"horizontal", ParamType::String, "where to put it across", false, {"none", "left", "center", "right"}, std::string("none")},
              {"vertical", ParamType::String, "where to put it up/down", false, {"none", "top", "middle", "bottom"}, std::string("none")},
              numParam("margin", "gap from the canvas edge in px", false, 0.0),
              previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                Bounds b = bounds(outline(s));
                if (b.empty) return Result::failure(s.id + " has no area to align");
                const std::string hz = stringParam(p, "horizontal", "none"), vt = stringParam(p, "vertical", "none");
                const double m = numberParam(p, "margin", 0);
                if (hz == "left") s.transform.tx += m - b.x0;
                else if (hz == "center") s.transform.tx += (doc.width - b.width()) / 2 - b.x0;
                else if (hz == "right") s.transform.tx += doc.width - m - b.x1;
                if (vt == "top") s.transform.ty += m - b.y0;
                else if (vt == "middle") s.transform.ty += (doc.height - b.height()) / 2 - b.y0;
                else if (vt == "bottom") s.transform.ty += doc.height - m - b.y1;
                return Result::success("aligned " + s.id + ": " + describe(s), s.id).with("shape", s.id);
            })));

    reg.add({"vector.delete", "Delete Shape", "Remove a shape from the drawing.", {docParam(), shapeParam(), previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params&) {
                std::string id = doc.shapes[static_cast<std::size_t>(idx)].id;
                doc.shapes.erase(doc.shapes.begin() + idx);
                return Result::success("deleted " + id, id).with("shape", id);
            })));

    reg.add({"vector.reorder", "Reorder Shape",
             "Move a shape in the stacking order: front (top), back (bottom), up or down one step.",
             {docParam(), shapeParam(),
              {"to", ParamType::String, "where to move it", true, {"front", "back", "up", "down"}},
              previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                const std::string to = stringParam(p, "to");
                auto& v = doc.shapes;
                const auto i = static_cast<std::size_t>(idx);
                Shape s = v[i];
                if (to == "front") {
                    v.erase(v.begin() + idx);
                    v.push_back(s);
                } else if (to == "back") {
                    v.erase(v.begin() + idx);
                    v.insert(v.begin(), s);
                } else if (to == "up" && i + 1 < v.size()) {
                    std::swap(v[i], v[i + 1]);
                } else if (to == "down" && i > 0) {
                    std::swap(v[i], v[i - 1]);
                }
                std::size_t pos = static_cast<std::size_t>(doc.find(s.id));
                return Result::success("moved " + s.id + " to position " + std::to_string(pos + 1) + " of " +
                                           std::to_string(v.size()) + " (1 = bottom)",
                                       s.id)
                    .with("shape", s.id)
                    .with("index", static_cast<std::int64_t>(pos));
            })));

    reg.add({"vector.duplicate", "Duplicate Shape",
             "Copy a shape, place the copy just above the original (offset by dx, dy) and return the copy's id.",
             {docParam(), shapeParam(), numParam("dx", "offset the copy right by px", false, 0.0),
              numParam("dy", "offset the copy down by px", false, 0.0),
              {"name", ParamType::String, "name for the copy (default: original name + \" copy\")", false},
              previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                Shape copy = doc.shapes[static_cast<std::size_t>(idx)];
                copy.name = stringParam(p, "name", copy.name + " copy");
                copy.transform.tx += numberParam(p, "dx", 0);
                copy.transform.ty += numberParam(p, "dy", 0);
                std::string id = doc.add(std::move(copy));
                Shape moved = std::move(doc.shapes.back());
                doc.shapes.pop_back();
                doc.shapes.insert(doc.shapes.begin() + idx + 1, std::move(moved));
                return Result::success("duplicated " + doc.shapes[static_cast<std::size_t>(idx)].id + " as " + id, id)
                    .with("shape", id);
            })));
}

}  // namespace achclwss::vector
