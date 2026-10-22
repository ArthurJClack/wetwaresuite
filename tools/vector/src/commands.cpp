// Every vector.* command. Edits run inside a VectorEdit so each one is a single
// undo step; read-only commands (info, preview, save) stay out of the history.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "internal.hpp"

namespace wetwaresuite::vector {

Result VectorEdit::apply() {
    before_ = static_cast<const Drawing&>(*doc_);
    Result r = fn_(*doc_);
    if (!r.ok) revert();  // failed edits leave the document untouched
    else doc_->refresh();
    return r;
}

void VectorEdit::revert() { static_cast<Drawing&>(*doc_) = before_; }

using namespace detail;

namespace {

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
             false, true},
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
             "Open a drawing as an editable vector document and return its id: SVG (shapes, groups and layers, "
             "symbols, clip paths, masks, gradients, filters this tool writes), PDF and Illustrator .ai files "
             "(page= picks the page; paths, text and colours come in as shapes) and EPS/PS.",
             {{"path", ParamType::String, "path to an .svg, .pdf, .ai, .eps or .ps file", true},
              {"page", ParamType::Int, "PDF/AI page, 1 = first", false, {}, std::int64_t{1}}},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        return docCreated(session, openAny(stringParam(p, "path"), static_cast<int>(intParam(p, "page", 1))), "opened");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"vector.save", "Save Drawing",
             "Save or export a vector document. .svg writes real SVG; .pdf a vector PDF (a page per artboard); "
             ".eps vector EPS; .png, .jpg, .bmp, .tga or .ppm render pixels (scale=2 for double resolution). "
             "artboard= exports just that artboard (name or number), or all for one file per artboard "
             "(name-ARTBOARD.ext).",
             {docParam(),
              {"path", ParamType::String, "output file; the extension picks the format", true},
              numParam("scale", "pixels per unit for raster formats", false, 1.0),
              {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{90}},
              {"artboard", ParamType::String, "artboard name or number, or all", false}},
             false},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const std::string path = stringParam(p, "path");
                const double scale = numberParam(p, "scale", 1.0);
                if (!(scale > 0) || scale > 32) return Result::failure("scale must be between 0 and 32");
                const int quality = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "quality", 90), 1, 100));
                std::filesystem::path fp(path);
                std::string e = fp.extension().string();
                for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::vector<std::string> written;
                try {
                    if (!has(p, "artboard")) {
                        saveDoc(doc, path, scale, quality);
                        written.push_back(path);
                    } else if (stringParam(p, "artboard") == "all") {
                        if (e == ".pdf") {
                            saveDoc(doc, path, scale, quality, -1);
                            written.push_back(path);
                        } else {
                            for (int k = 0; k < doc.artboardCount(); ++k) {
                                std::filesystem::path out = fp.parent_path() / (fp.stem().string() + "-" + doc.artboard(k).name + fp.extension().string());
                                saveDoc(doc, out, scale, quality, k);
                                written.push_back(out.string());
                            }
                        }
                    } else {
                        const std::string name = stringParam(p, "artboard");
                        int k = -1;
                        for (int i = 0; i < doc.artboardCount(); ++i) {
                            if (doc.artboard(i).name == name || std::to_string(i + 1) == name) k = i;
                        }
                        if (k < 0) return Result::failure("no artboard '" + name + "' (vector.artboards lists them)");
                        saveDoc(doc, path, scale, quality, k);
                        written.push_back(path);
                    }
                } catch (const std::exception& ex) {
                    return Result::failure(ex.what());
                }
                if (e == ".svg" && !has(p, "artboard")) doc.path = path;
                std::string list;
                for (const auto& w : written) list += (list.empty() ? "" : ",") + w;
                return Result::success("saved " + list, written.front()).with("path", written.front()).with("files", list);
            }, false));

    reg.add({"vector.convert", "Convert Drawing",
             "Convert a drawing in one step, without opening it first: input=logo.svg output=logo.png, or "
             "input=art.ai output=art.svg. Reads svg, pdf, ai, eps; the output extension picks the format "
             "(svg, pdf, eps, png, jpg, bmp, tga, ppm).",
             {{"input", ParamType::String, "path to an .svg, .pdf, .ai or .eps file", true},
              {"page", ParamType::Int, "PDF/AI page to read, 1 = first", false, {}, std::int64_t{1}},
              {"output", ParamType::String, "output file; the extension picks the format", true},
              numParam("scale", "pixels per unit for raster formats", false, 1.0),
              {"width", ParamType::Int, "raster output width in px (sets the scale, keeps aspect)", false},
              {"height", ParamType::Int, "raster output height in px (sets the scale, keeps aspect)", false},
              {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{90}}},
             false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    const std::string input = stringParam(p, "input"), output = stringParam(p, "output");
                    try {
                        auto doc = openAny(input, static_cast<int>(intParam(p, "page", 1)));
                        double scale = numberParam(p, "scale", 1.0);
                        if (auto w = intParam(p, "width", 0); w > 0 && doc->width > 0) scale = static_cast<double>(w) / doc->width;
                        else if (auto h = intParam(p, "height", 0); h > 0 && doc->height > 0) scale = static_cast<double>(h) / doc->height;
                        if (!(scale > 0) || scale > 32) return Result::failure("scale must be between 0 and 32");
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
             false, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::string text = stringParam(p, "doc") + ": " + std::to_string(doc.width) + "x" +
                                   std::to_string(doc.height) + ", background " + paintText(doc.background) + ", " +
                                   std::to_string(doc.shapes.size()) + " shapes (bottom first)";
                std::string ids;
                for (const auto& s : doc.shapes) {
                    text += "\n" + describe(s);
                    ids += (ids.empty() ? "" : ",") + s.id;
                }
                for (const auto& a : doc.artboards) text += "\nartboard " + a.name + " " + num(a.x) + "," + num(a.y) + " " + num(a.w) + "x" + num(a.h);
                for (const auto& y : doc.symbols) text += "\nsymbol " + y.name + " (" + std::to_string(y.shapes.size()) + " shapes)";
                for (const auto& [n, c] : doc.swatches) text += "\nswatch " + n + " " + colorText(c);
                for (const auto& t : doc.textStyles) text += "\ntext style " + textStyleText(t);
                if (!doc.guidesX.empty() || !doc.guidesY.empty()) {
                    text += "\nguides: " + std::to_string(doc.guidesX.size()) + " vertical, " + std::to_string(doc.guidesY.size()) + " horizontal";
                }
                if (doc.gridSize > 0) text += "\ngrid " + num(doc.gridSize);
                if (!doc.currentLayer.empty()) text += "\ncurrent layer " + doc.currentLayer;
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
             false, true},
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
             "Change a shape's fill, outline, outline width, opacity, blend mode, fill rule, name or visibility (works on "
             "groups and layers too). Only the params you give change.",
             {docParam(), shapeParam(), paintParam("fill", "fill colour", ""), paintParam("stroke", "outline colour", ""),
              numParam("stroke_width", "outline width in px", false), numParam("opacity", "opacity 0..1", false),
              {"name", ParamType::String, "new name", false},
              {"visible", ParamType::Bool, "show or hide the shape", false},
              {"blend", ParamType::String, "blend mode: normal, multiply, screen, overlay, darken, lighten, ...", false},
              {"fill_rule", ParamType::String, "how overlapping subpaths fill", false, {"evenodd", "nonzero"}},
              previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                std::string error;
                if (!applyStyle(s, p, error)) return Result::failure(error);
                return Result::success("styled " + s.id + ": " + describe(s), s.id).with("shape", s.id);
            })));

    reg.add({"vector.delete", "Delete Shape", "Remove a shape (a group or layer goes with everything in it).",
             {docParam(), shapeParam(), previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params&) {
                std::string id = doc.shapes[static_cast<std::size_t>(idx)].id;
                eraseSubtree(doc, idx);
                return Result::success("deleted " + id, id).with("shape", id);
            })));

    reg.add({"vector.duplicate", "Duplicate Shape",
             "Copy a shape, place the copy just above the original (offset by dx, dy) and return the copy's id.",
             {docParam(), shapeParam(), numParam("dx", "offset the copy right by px", false, 0.0),
              numParam("dy", "offset the copy down by px", false, 0.0),
              {"name", ParamType::String, "name for the copy (default: original name + \" copy\")", false},
              previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                const std::string from = doc.shapes[static_cast<std::size_t>(idx)].id;
                const std::string id = duplicateSubtree(doc, idx, doc.shapes[static_cast<std::size_t>(idx)].parent);
                Shape& copy = doc.shapes[static_cast<std::size_t>(doc.find(id))];
                copy.name = stringParam(p, "name", copy.name + " copy");
                const double dx = numberParam(p, "dx", 0), dy = numberParam(p, "dy", 0);
                if (dx != 0 || dy != 0) {
                    if (copy.kind == ShapeKind::Group) applyMatrix(doc, doc.find(id), Matrix::translate(dx, dy));
                    else copy.transform.tx += dx, copy.transform.ty += dy;
                }
                return Result::success("duplicated " + from + " as " + id, id).with("shape", id);
            })));

    detail::registerTextCommands(session);
    detail::registerPathCommands(session);
    detail::registerRegionCommands(session);
    detail::registerAppearanceCommands(session);
    detail::registerOrganizeCommands(session);
    detail::registerBoardCommands(session);
}

}  // namespace wetwaresuite::vector
