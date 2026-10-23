#include "wetwaresuite/paint/paint.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "common.hpp"

namespace wetwaresuite::paint {

using namespace raster;

namespace {

Image& targetLayer(ImageDoc& doc, int idx) { return doc.image.layers[static_cast<std::size_t>(idx)].image; }

}  // namespace

void registerCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"paint.new",
             "New canvas",
             "Create a blank painting canvas with one background layer and an empty paint layer on top, ready to draw.",
             {{"width", ParamType::Int, "width in px", true},
              {"height", ParamType::Int, "height in px", true},
              colorParam("background", "background colour (use transparent for none)", false, "#ffffff")},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    auto w = intParam(p, "width"), h = intParam(p, "height");
                    if (w < 1 || h < 1 || w > 30000 || h > 30000) return Result::failure("width and height must be 1..30000");
                    Color bg;
                    std::string error;
                    if (!colorArg(p, "background", "#ffffff", bg, error)) return Result::failure(error);
                    auto doc = newImage(static_cast<int>(w), static_cast<int>(h), bg);
                    doc->image.layers.push_back({"Layer 1", Image(static_cast<int>(w), static_cast<int>(h))});
                    doc->active = 1;
                    std::string id = s->workspace().add(doc, "img");
                    return Result::success("created " + id + " with layers Background and Layer 1")
                        .with("doc", id).with("width", w).with("height", h).with("layer", std::int64_t{1});
                });
            });

    reg.add({"paint.fill",
             "Bucket fill",
             "Flood fill the area around a point with a colour, like a paint bucket.",
             {docParam(),
              {"x", ParamType::Int, "x in px", true},
              {"y", ParamType::Int, "y in px", true},
              colorParam("color", "fill colour", true, ""),
              {"tolerance", ParamType::Int, "how different a colour can be and still fill, 0..255", false, {}, std::int64_t{32}},
              {"contiguous", ParamType::Bool, "only fill connected pixels (false = every matching pixel)", false, {}, true},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = pixelLayerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Color c;
                if (!colorArg(p, "color", "", c, error)) return Result::failure(error);
                int x = static_cast<int>(intParam(p, "x")), y = static_cast<int>(intParam(p, "y"));
                if (x < 0 || y < 0 || x >= doc.image.width || y >= doc.image.height) return Result::failure("point is outside the image");
                auto n = floodFill(targetLayer(doc, idx), x, y, c, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "tolerance", 32), 0, 255)),
                                   boolParam(p, "contiguous", true));
                Result r = done("filled " + std::to_string(n) + " pixels", doc, p);
                return r.with("pixels", static_cast<std::int64_t>(n));
            }));

    reg.add({"paint.shape",
             "Draw shape",
             "Draw a rectangle, ellipse or line inside the box x,y,width,height (for a line: from x,y to x+width,y+height). "
             "filled=false draws just the outline.",
             {docParam(),
              {"kind", ParamType::String, "shape", true, {"rect", "ellipse", "line"}},
              {"x", ParamType::Number, "left (or line start x)", true},
              {"y", ParamType::Number, "top (or line start y)", true},
              {"width", ParamType::Number, "box width (or line dx)", true},
              {"height", ParamType::Number, "box height (or line dy)", true},
              colorParam("color", "colour", false, "#000000"),
              {"filled", ParamType::Bool, "fill the shape (ignored for lines)", false, {}, true},
              {"line_width", ParamType::Number, "outline or line width in px", false, {}, 2.0},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = pixelLayerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Color c;
                if (!colorArg(p, "color", "#000000", c, error)) return Result::failure(error);
                const std::string kind = stringParam(p, "kind");
                double x = numberParam(p, "x"), y = numberParam(p, "y"), w = numberParam(p, "width"), h = numberParam(p, "height");
                double lw = std::max(0.5, numberParam(p, "line_width", 2));
                bool filled = boolParam(p, "filled", true);
                Image& img = targetLayer(doc, idx);
                if (kind == "line") {
                    drawLine(img, x, y, x + w, y + h, c, lw);
                } else {
                    if (w < 0) {
                        x += w;
                        w = -w;
                    }
                    if (h < 0) {
                        y += h;
                        h = -h;
                    }
                    if (kind == "rect") {
                        filled ? fillRect(img, x, y, w, h, c) : strokeRect(img, x, y, w, h, c, lw);
                    } else {
                        filled ? fillEllipse(img, x + w / 2, y + h / 2, w / 2, h / 2, c)
                               : strokeEllipse(img, x + w / 2, y + h / 2, w / 2, h / 2, c, lw);
                    }
                }
                doc.active = idx;
                return done(std::string(kind == "ellipse" ? "drew an " : "drew a ") + kind, doc, p);
            }));

    reg.add({"paint.polygon",
             "Draw polygon",
             "Fill a polygon through the given points (x,y pairs).",
             {docParam(), {"points", ParamType::NumberList, "corners as x,y pairs, at least 3", true},
              colorParam("color", "fill colour", false, "#000000"), layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = pixelLayerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Color c;
                if (!colorArg(p, "color", "#000000", c, error)) return Result::failure(error);
                NumberList xy = listParam(p, "points");
                if (xy.size() < 6 || xy.size() % 2) return Result::failure("points must be at least 3 x,y pairs");
                std::vector<Point> pts;
                for (std::size_t i = 0; i < xy.size(); i += 2) pts.push_back({xy[i], xy[i + 1]});
                fillPolygon(targetLayer(doc, idx), pts, c);
                return done("drew a polygon with " + std::to_string(pts.size()) + " corners", doc, p);
            }));

    reg.add({"paint.clear",
             "Clear layer",
             "Wipe a layer to transparent, or fill it entirely with a colour.",
             {docParam(), colorParam("color", "colour to fill with", false, "transparent"), layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = pixelLayerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Color c;
                if (!colorArg(p, "color", "transparent", c, error)) return Result::failure(error);
                targetLayer(doc, idx).fill(c);
                return done("cleared layer " + std::to_string(idx), doc, p);
            }));

    registerLayerCommands(session);
    registerGroupCommands(session);
    registerStyleCommands(session);
    registerTextCommands(session);
    registerRetouchBrushes(session);
    registerFillCommands(session);
}

}  // namespace wetwaresuite::paint
