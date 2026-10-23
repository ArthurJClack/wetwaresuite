// Region commands: compound paths, break apart, divide, the shape builder,
// live-paint-style region fills, clipping masks and opacity masks.
#include <algorithm>
#include <cmath>

#include "internal.hpp"

namespace wetwaresuite::vector::detail {

namespace {

struct Region {
    Outline area;
    int owner;  // index of the shape whose style it takes
};

double areaSize(const Outline& o) {
    double a = 0;
    for (const auto& c : o) a += signedArea(c.points);
    return std::abs(a);
}

bool nonzeroFill(const Shape& s) { return s.kind == ShapeKind::Text || s.fillRule == "nonzero"; }

// Every region where the shapes overlap differently, each styled by the topmost shape covering it.
std::vector<Region> divide(const VectorDoc& doc, std::vector<int> idx) {
    std::sort(idx.begin(), idx.end());
    std::vector<Region> regions;
    Outline covered;
    for (int i : idx) {
        Outline a = areaOf(doc, i);
        if (a.empty()) continue;
        a = booleanOp(a, {}, BoolOp::Union, nonzeroFill(doc.shapes[static_cast<std::size_t>(i)]));  // clean contours
        std::vector<Region> next;
        for (const auto& r : regions) {
            Outline in = booleanOp(r.area, a, BoolOp::Intersect);
            Outline out = booleanOp(r.area, a, BoolOp::Subtract);
            if (!in.empty()) next.push_back({std::move(in), i});
            if (!out.empty()) next.push_back({std::move(out), r.owner});
        }
        Outline fresh = covered.empty() ? a : booleanOp(a, covered, BoolOp::Subtract);
        if (!fresh.empty()) next.push_back({fresh, i});
        covered = covered.empty() ? a : booleanOp(covered, a, BoolOp::Union);
        regions = std::move(next);
    }
    std::vector<Region> out;
    for (const auto& r : regions) {
        for (auto& piece : pieces(r.area)) {
            if (areaSize(piece) >= 0.25) out.push_back({std::move(piece), r.owner});
        }
    }
    return out;
}

Shape styledPath(const Shape& from, const Outline& area) {
    Shape s = from;
    s.id.clear();
    s.kind = ShapeKind::Path;
    s.d = outlinePathData(area);
    s.text.clear();
    s.points.clear();
    if (!from.transform.identity()) s.strokeWidth *= shapeMatrix(from).scaleFactor();
    s.transform = {};
    s.onPath.clear();
    s.fillRule = "evenodd";
    return s;
}

std::vector<Point> pointsParam(const Params& p, const std::string& key) {
    std::vector<Point> out;
    const NumberList v = listParam(p, key);
    for (std::size_t i = 0; i + 1 < v.size(); i += 2) out.push_back({v[i], v[i + 1]});
    return out;
}

// Replaces the shapes `idx` with `made` (styled paths) at the bottom-most one's place; returns their ids.
std::vector<std::string> replaceWith(VectorDoc& doc, std::vector<int> idx, std::vector<Shape> made) {
    std::sort(idx.begin(), idx.end());
    const std::string parent = doc.shapes[static_cast<std::size_t>(idx.front())].parent;
    std::vector<std::string> ids;
    for (const auto& s : idx) ids.push_back(doc.shapes[static_cast<std::size_t>(s)].id);
    int at = idx.front();
    for (const auto& id : ids) {
        const int i = doc.find(id);
        if (i < 0) continue;
        if (i < at) at = i;
        eraseSubtree(doc, i);
    }
    std::vector<std::string> out;
    for (auto& s : made) {
        s.parent = parent;
        out.push_back(insertShape(doc, std::move(s), at++));
    }
    return out;
}

std::string joinIds(const std::vector<std::string>& ids) {
    std::string out;
    for (const auto& id : ids) out += (out.empty() ? "" : ",") + id;
    return out;
}

bool leafSelection(const VectorDoc& doc, const Params& p, std::size_t minimum, std::vector<int>& idx, std::string& error) {
    if (!selection(doc, p, idx, error)) return false;
    if (idx.size() < minimum) return error = "give at least " + std::to_string(minimum) + " shapes, e.g. shapes=s1,s2", false;
    for (int i : idx) {
        if (!editable(doc, i, error)) return false;
    }
    return true;
}

}  // namespace

void registerRegionCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"vector.compound", "Make Compound Path",
             "Combine shapes into one compound path: overlapping parts make holes (even-odd), like a donut. It "
             "takes the bottom shape's style. vector.break_apart separates it again.",
             {docParam(), {"shapes", ParamType::String, "two or more shapes, comma separated", true}, previewParam()}, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (!leafSelection(doc, p, 2, idx, error)) return Result::failure(error);
                std::sort(idx.begin(), idx.end());
                BezierPath all;
                for (int i : idx) {
                    BezierPath bp;
                    if (!canvasPath(doc.shapes[static_cast<std::size_t>(i)], bp, error)) return Result::failure(error);
                    all.insert(all.end(), bp.begin(), bp.end());
                }
                Shape s = doc.shapes[static_cast<std::size_t>(idx.front())];
                s.id.clear();
                storePath(s, all);
                s.fillRule = "evenodd";
                const auto ids = replaceWith(doc, idx, {s});
                Result r = Result::success("made compound path " + ids[0] + " from " + std::to_string(idx.size()) + " shapes", ids[0]);
                r.with("shape", ids[0]);
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.break_apart", "Break Apart",
             "Split a path into one shape per subpath (releasing a compound path). holes=true keeps holes with "
             "the shape around them, so a letter O stays an O. Returns the new ids.",
             {docParam(), shapeParam(), {"holes", ParamType::Bool, "keep holes inside their outer shape", false, {}, false}, previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                std::string error;
                if (!editable(doc, idx, error)) return Result::failure(error);
                const Shape from = doc.shapes[static_cast<std::size_t>(idx)];
                std::vector<Shape> made;
                if (boolParam(p, "holes", false)) {
                    for (const auto& piece : pieces(areaOf(doc, idx))) made.push_back(styledPath(from, piece));
                } else {
                    BezierPath bp;
                    if (from.kind == ShapeKind::Text) bp = parsePath(toPathData(from));
                    else if (!canvasPath(from, bp, error)) return Result::failure(error);
                    if (from.kind == ShapeKind::Text) transformPath(bp, shapeMatrix(from));
                    for (const auto& sp : bp) {
                        Shape s = from;
                        s.id.clear();
                        storePath(s, {sp});
                        made.push_back(s);
                    }
                }
                if (made.size() < 2) return Result::failure(from.id + " is a single piece already");
                const auto ids = replaceWith(doc, {idx}, made);
                return Result::success("broke " + from.id + " into " + std::to_string(ids.size()) + " shapes: " + joinIds(ids), joinIds(ids))
                    .with("shapes", joinIds(ids));
            })));

    reg.add({"vector.divide", "Divide",
             "Cut overlapping shapes into every separate region they make (Pathfinder divide). Each region takes "
             "the style of the topmost shape over it. Returns the regions' ids.",
             {docParam(), {"shapes", ParamType::String, "two or more shapes, comma separated", true}, previewParam()}, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (!leafSelection(doc, p, 2, idx, error)) return Result::failure(error);
                std::vector<Shape> made;
                for (const auto& r : divide(doc, idx)) made.push_back(styledPath(doc.shapes[static_cast<std::size_t>(r.owner)], r.area));
                if (made.empty()) return Result::failure("those shapes have no area");
                const auto ids = replaceWith(doc, idx, made);
                Result r = Result::success("divided into " + std::to_string(ids.size()) + " regions: " + joinIds(ids), joinIds(ids));
                r.with("shapes", joinIds(ids)).with("count", static_cast<std::int64_t>(ids.size()));
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.shape_builder", "Shape Builder",
             "Build shapes from the regions overlapping shapes make, like the shape builder tool: every region "
             "under a merge point joins into one shape (styled like the first one), regions under an erase point "
             "go, the rest stay as separate regions. merge=x,y,x,y,... erase=x,y,...",
             {docParam(), {"shapes", ParamType::String, "the overlapping shapes, comma separated", true},
              {"merge", ParamType::NumberList, "points (x,y,...) in regions to merge", false},
              {"erase", ParamType::NumberList, "points (x,y,...) in regions to delete", false}, previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (!leafSelection(doc, p, 1, idx, error)) return Result::failure(error);
                const auto merge = pointsParam(p, "merge"), erase = pointsParam(p, "erase");
                if (merge.empty() && erase.empty()) return Result::failure("give merge= and/or erase= points");
                const auto regions = divide(doc, idx);
                auto hits = [](const Region& r, const std::vector<Point>& pts) {
                    for (const auto& q : pts) {
                        if (insideOutline(r.area, q)) return true;
                    }
                    return false;
                };
                Outline merged;
                int mergedOwner = -1;
                std::vector<Shape> made;
                std::size_t erased = 0, joined = 0;
                for (const auto& r : regions) {
                    if (hits(r, erase)) {
                        ++erased;
                        continue;
                    }
                    if (hits(r, merge)) {
                        // the region under the first merge point gives the style
                        if (mergedOwner < 0 || insideOutline(r.area, merge.front())) mergedOwner = r.owner;
                        merged = merged.empty() ? r.area : booleanOp(merged, r.area, BoolOp::Union);
                        ++joined;
                        continue;
                    }
                    made.push_back(styledPath(doc.shapes[static_cast<std::size_t>(r.owner)], r.area));
                }
                std::string mergedId;
                if (!merged.empty()) made.insert(made.begin(), styledPath(doc.shapes[static_cast<std::size_t>(mergedOwner)], merged));
                if (made.empty()) {
                    std::sort(idx.rbegin(), idx.rend());
                    for (int i : idx) eraseSubtree(doc, i);
                    return Result::success("erased every region");
                }
                const auto ids = replaceWith(doc, idx, made);
                if (!merged.empty()) mergedId = ids.front();
                Result r = Result::success("merged " + std::to_string(joined) + " region(s)" + (mergedId.empty() ? "" : " into " + mergedId) +
                                               ", erased " + std::to_string(erased) + "; shapes now " + joinIds(ids),
                                           mergedId.empty() ? joinIds(ids) : mergedId);
                r.with("shapes", joinIds(ids));
                if (!mergedId.empty()) r.with("shape", mergedId);
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.paint_region", "Paint Region",
             "Fill the area enclosed by lines and shape edges around x,y, like a live-paint bucket: the lines can "
             "be separate open paths that only meet. The fill is a new shape tucked under the lines. Returns its id.",
             {docParam(), numParam("x", "a point inside the area"), numParam("y", "a point inside the area"),
              paintParam("fill", "fill colour", "black"),
              {"shapes", ParamType::String, "shapes whose edges bound the area (default: every visible shape)", false}, previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (has(p, "shapes")) {
                    if (!shapeList(doc, stringParam(p, "shapes"), idx, error)) return Result::failure(error);
                } else {
                    idx = paintOrder(doc);
                }
                std::optional<Color> fill;
                if (!paintArg(stringParam(p, "fill", "black"), fill, error)) return Result::failure(error);
                const Point at{numberParam(p, "x"), numberParam(p, "y")};
                Outline walls;
                double thinnest = 1e9;
                for (int i : idx) {
                    const Shape& s = doc.shapes[static_cast<std::size_t>(i)];
                    if (s.kind == ShapeKind::Group) continue;
                    const double w = s.stroke ? std::max(0.5, s.strokeWidth * shapeMatrix(s).scaleFactor()) : 0.5;
                    thinnest = std::min(thinnest, w);
                    StrokeStyle st;
                    const Outline a = strokeArea(outlineIn(doc, i, 2.0), w, st);
                    walls.insert(walls.end(), a.begin(), a.end());
                }
                if (walls.empty()) return Result::failure("there are no lines to fill between");
                walls = booleanOp(walls, {}, BoolOp::Union, true);
                Bounds b = bounds(walls);
                Contour box;
                box.closed = true;
                box.points = {{b.x0 - 2, b.y0 - 2}, {b.x1 + 2, b.y0 - 2}, {b.x1 + 2, b.y1 + 2}, {b.x0 - 2, b.y1 + 2}};
                const Outline open = booleanOp({box}, walls, BoolOp::Subtract, false, true);
                for (const auto& piece : pieces(open)) {
                    if (!insideOutline(piece, at)) continue;
                    const Bounds pb = bounds(piece);
                    if (pb.x0 <= b.x0 - 1 || pb.y0 <= b.y0 - 1 || pb.x1 >= b.x1 + 1 || pb.y1 >= b.y1 + 1) {
                        return Result::failure("that point isn't enclosed (the area runs off past the lines)");
                    }
                    Shape s;
                    s.kind = ShapeKind::Path;
                    s.d = outlinePathData(offsetArea(piece, thinnest / 2, "round"));
                    s.fill = fill;
                    s.stroke.reset();
                    s.name = "paint";
                    int lowest = static_cast<int>(doc.shapes.size());
                    for (int i : idx) lowest = std::min(lowest, i);
                    if (lowest < static_cast<int>(doc.shapes.size())) s.parent = doc.shapes[static_cast<std::size_t>(lowest)].parent;
                    const std::string id = insertShape(doc, std::move(s), lowest);
                    Result r = Result::success("painted the region around " + num(at.x) + "," + num(at.y) + " as " + id, id);
                    r.with("shape", id);
                    return maybePreview(r, doc, p);
                }
                return Result::failure("that point is on a line or outside every enclosed area");
            }));

    for (int pass = 0; pass < 2; ++pass) {
        const bool clip = pass == 0;
        const std::string what = clip ? "clip" : "mask";
        reg.add({clip ? "vector.clip" : "vector.mask", clip ? "Clipping Mask" : "Opacity Mask",
                 clip ? "Show the shapes only inside another shape (a clipping mask). The clipping shape is the topmost "
                        "listed one unless clip= names it; it isn't drawn. The shapes go into a new group; returns its id."
                      : "Fade shapes by another shape's brightness (an opacity mask): white shows, black hides, greys and "
                        "gradients fade. The mask is the topmost listed shape unless mask= names it. Returns the new group's id.",
                 {docParam(), {"shapes", ParamType::String, "the shapes to " + what, true},
                  {what, ParamType::String, "the " + what + " shape (default: the topmost listed)", false},
                  {"name", ParamType::String, "group name", false}, previewParam()},
                 true},
                vectorCommand(session, [clip, what](VectorDoc& doc, const Params& p) {
                    std::vector<int> idx;
                    std::string error;
                    if (!leafSelection(doc, p, 1, idx, error)) return Result::failure(error);
                    int by = -1;
                    if (has(p, what)) {
                        by = doc.find(stringParam(p, what));
                        if (by < 0) return Result::failure("no shape '" + stringParam(p, what) + "'");
                        if (std::find(idx.begin(), idx.end(), by) == idx.end()) idx.push_back(by);
                    } else {
                        by = *std::max_element(idx.begin(), idx.end());
                    }
                    if (idx.size() < 2) return Result::failure("give the shapes and a separate " + what + " shape");
                    const std::string byId = doc.shapes[static_cast<std::size_t>(by)].id;
                    const std::string gid = groupShapes(doc, idx, stringParam(p, "name", clip ? "clip group" : "mask group"));
                    Shape& g = doc.shapes[static_cast<std::size_t>(doc.find(gid))];
                    (clip ? g.clip : g.mask) = byId;
                    Result r = Result::success((clip ? "clipped by " : "masked by ") + byId + " in group " + gid, gid);
                    r.with("shape", gid).with(what, byId);
                    return maybePreview(r, doc, p);
                }));
        reg.add({clip ? "vector.release_clip" : "vector.release_mask", clip ? "Release Clipping Mask" : "Release Opacity Mask",
                 clip ? "Stop a group clipping: its shapes show whole again; the clipping shape stays, unpainted."
                      : "Stop a group masking: its shapes show fully again and the mask shape draws normally.",
                 {docParam(), shapeParam(), previewParam()}, true},
                vectorCommand(session, onShape([clip](VectorDoc& doc, int idx, const Params&) {
                    Shape& g = doc.shapes[static_cast<std::size_t>(idx)];
                    std::string& link = clip ? g.clip : g.mask;
                    if (link.empty()) return Result::failure(g.id + (clip ? " isn't clipped" : " isn't masked"));
                    const std::string by = link;
                    link.clear();
                    if (clip) {
                        if (int c = doc.find(by); c >= 0) {
                            doc.shapes[static_cast<std::size_t>(c)].fill.reset();
                            doc.shapes[static_cast<std::size_t>(c)].stroke.reset();
                        }
                    }
                    return Result::success("released " + by + " from " + g.id, g.id).with("shape", g.id);
                })));
    }
}

}  // namespace wetwaresuite::vector::detail
