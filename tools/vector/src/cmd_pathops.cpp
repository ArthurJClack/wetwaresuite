// Whole-path operations: join, split (scissors), knife, simplify, smooth,
// offset path, outline stroke, reverse and open/close.
#include <algorithm>
#include <cmath>

#include "internal.hpp"

namespace wetwaresuite::vector::detail {

namespace {

double dist(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

// A filled path made from an area, styled like `from`, at list index `at`.
std::string areaShape(VectorDoc& doc, const Shape& from, const Outline& area, int at) {
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
    s.name = from.name;
    return insertShape(doc, std::move(s), at);
}

// One side of a knife line (a polyline), as a big polygon around everything in `box`.
Outline knifeSide(const std::vector<Point>& line, const Bounds& box) {
    const double L = 2 * (box.width() + box.height()) + 100;
    auto dir = [](Point a, Point b) {
        const double l = std::max(1e-12, dist(a, b));
        return Point{(b.x - a.x) / l, (b.y - a.y) / l};
    };
    const Point d0 = dir(line[0], line[1]), d1 = dir(line[line.size() - 2], line.back());
    Contour c;
    c.closed = true;
    const Point e0{line[0].x - d0.x * L, line[0].y - d0.y * L}, e1{line.back().x + d1.x * L, line.back().y + d1.y * L};
    c.points.push_back(e0);
    for (const auto& p : line) c.points.push_back(p);
    c.points.push_back(e1);
    c.points.push_back({e1.x - d1.y * L, e1.y + d1.x * L});  // off to one side of the line
    c.points.push_back({e0.x - d0.y * L, e0.y + d0.x * L});
    return {c};
}

}  // namespace

void registerPathOpCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"vector.join", "Join Paths",
             "Join open paths: with=another shape joins its nearest end to this shape's nearest end with a straight "
             "segment (it becomes part of this shape); without `with`, an open path's two ends are joined (closed).",
             {docParam(), shapeParam(), {"with", ParamType::String, "the other open path", false}, previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                std::string error;
                BezierPath a, b;
                if (!editable(doc, idx, error) || !canvasPath(doc.shapes[static_cast<std::size_t>(idx)], a, error)) return Result::failure(error);
                auto open = [](BezierPath& bp) -> SubPath* {
                    for (auto& sp : bp) {
                        if (!sp.closed && sp.nodes.size() >= 2) return &sp;
                    }
                    return nullptr;
                };
                SubPath* sa = open(a);
                if (!sa) return Result::failure(doc.shapes[static_cast<std::size_t>(idx)].id + " has no open path to join");
                if (!has(p, "with")) {
                    sa->closed = true;
                    if (dist(sa->nodes.front().p, sa->nodes.back().p) < 1e-6) {  // ends on the same spot: one anchor
                        sa->nodes.front().in = sa->nodes.back().in;
                        sa->nodes.pop_back();
                    }
                    storePath(doc.shapes[static_cast<std::size_t>(idx)], a);
                    const Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                    return Result::success("closed " + s.id, s.id).with("shape", s.id);
                }
                const int other = doc.find(stringParam(p, "with"));
                if (other < 0 || other == idx) return Result::failure("with= must name another shape");
                if (!editable(doc, other, error) || !canvasPath(doc.shapes[static_cast<std::size_t>(other)], b, error)) return Result::failure(error);
                SubPath* sb = open(b);
                if (!sb) return Result::failure(doc.shapes[static_cast<std::size_t>(other)].id + " has no open path to join");
                // pick the closest pair of ends; a's end must meet b's start
                const double d[4] = {dist(sa->nodes.back().p, sb->nodes.front().p), dist(sa->nodes.back().p, sb->nodes.back().p),
                                     dist(sa->nodes.front().p, sb->nodes.front().p), dist(sa->nodes.front().p, sb->nodes.back().p)};
                const int best = static_cast<int>(std::min_element(d, d + 4) - d);
                if (best >= 2) reverse(*sa);
                if (best == 1 || best == 3) reverse(*sb);
                std::vector<Node> tail = sb->nodes;
                if (d[best] < 1e-6) {  // touching ends merge into one anchor
                    sa->nodes.back().out = tail.front().out;
                    tail.erase(tail.begin());
                }
                sa->nodes.insert(sa->nodes.end(), tail.begin(), tail.end());
                for (auto& sp : b) {
                    if (&sp != sb) a.push_back(sp);
                }
                storePath(doc.shapes[static_cast<std::size_t>(idx)], a);
                const std::string keep = doc.shapes[static_cast<std::size_t>(idx)].id, gone = doc.shapes[static_cast<std::size_t>(other)].id;
                eraseSubtree(doc, other);
                const Shape& s = doc.shapes[static_cast<std::size_t>(doc.find(keep))];
                return Result::success("joined " + gone + " onto " + keep + ": " + describe(s), keep).with("shape", keep);
            })));

    reg.add({"vector.split", "Split Path",
             "Cut a path at an anchor (node=) or at the point nearest x,y, like the scissors tool. A closed path "
             "opens there; an open path becomes two shapes (the second one's id is returned as `other`).",
             {docParam(), shapeParam(), {"node", ParamType::String, "anchor number to cut at", false},
              numParam("x", "cut near this x", false), numParam("y", "cut near this y", false), previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                std::string error;
                BezierPath bp;
                if (!editable(doc, idx, error) || !canvasPath(doc.shapes[static_cast<std::size_t>(idx)], bp, error)) return Result::failure(error);
                std::size_t sub = 0, k = 0;
                if (has(p, "x") && has(p, "y")) {
                    const PathHit hit = nearestOnPath(bp, {numberParam(p, "x"), numberParam(p, "y")});
                    if (hit.distance > 1e200) return Result::failure("the path has no segments");
                    sub = hit.sub;
                    k = hit.seg;
                    if (hit.t > 1e-6 && hit.t < 1 - 1e-6) k = splitSegment(bp[sub], hit.seg, hit.t);
                    else if (hit.t >= 1 - 1e-6) k = (hit.seg + 1) % bp[sub].nodes.size();
                } else {
                    int n = has(p, "node") ? std::atoi(stringParam(p, "node").c_str()) : -1;
                    bool found = false;
                    for (std::size_t s = 0; s < bp.size() && n >= 0; ++s) {
                        if (static_cast<std::size_t>(n) < bp[s].nodes.size()) {
                            sub = s, k = static_cast<std::size_t>(n), found = true;
                            break;
                        }
                        n -= static_cast<int>(bp[s].nodes.size());
                    }
                    if (!found) return Result::failure("give node= (an anchor number from vector.nodes) or x,y");
                }
                SubPath& sp = bp[sub];
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                if (sp.closed) {
                    std::rotate(sp.nodes.begin(), sp.nodes.begin() + static_cast<long>(k), sp.nodes.end());
                    Node end = sp.nodes.front();
                    sp.nodes.front().in = sp.nodes.front().p;
                    end.out = end.p;
                    sp.nodes.push_back(end);
                    sp.closed = false;
                    storePath(s, bp);
                    return Result::success("opened " + s.id + " at " + num(sp.nodes.front().p.x) + "," + num(sp.nodes.front().p.y), s.id)
                        .with("shape", s.id);
                }
                if (k == 0 || k + 1 >= sp.nodes.size()) return Result::failure("that's an end of the path already; nothing to split");
                SubPath second;
                second.nodes.assign(sp.nodes.begin() + static_cast<long>(k), sp.nodes.end());
                sp.nodes.erase(sp.nodes.begin() + static_cast<long>(k) + 1, sp.nodes.end());
                sp.nodes.back().out = sp.nodes.back().p;
                second.nodes.front().in = second.nodes.front().p;
                BezierPath rest{second};
                for (std::size_t i = sub + 1; i < bp.size(); ++i) rest.push_back(bp[i]);
                bp.erase(bp.begin() + static_cast<long>(sub) + 1, bp.end());
                storePath(s, bp);
                Shape copy = s;
                copy.id.clear();
                copy.name.clear();
                copy.d = pathData(rest);
                const std::string first = s.id;
                const std::string other = insertShape(doc, std::move(copy), static_cast<int>(doc.subtree(idx).back()) + 1);
                return Result::success("split " + first + " into " + first + " and " + other, first).with("shape", first).with("other", other);
            })));

    reg.add({"vector.cut", "Knife",
             "Cut filled shapes along a line, like the knife tool: line=x1,y1,x2,y2(,x3,y3...) runs right across "
             "them, and each shape becomes its separate pieces (with its style). Returns the pieces' ids.",
             {docParam(), {"shapes", ParamType::String, "shapes to cut (default: every unlocked visible shape the line crosses)", false},
              {"line", ParamType::NumberList, "the knife's points x,y,x,y,...", true}, previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<Point> line;
                std::string error;
                const NumberList v = listParam(p, "line");
                for (std::size_t i = 0; i + 1 < v.size(); i += 2) line.push_back({v[i], v[i + 1]});
                if (line.size() < 2 || v.size() % 2) return Result::failure("line is x,y pairs, at least two points");
                std::vector<int> idx;
                if (has(p, "shapes")) {
                    if (!shapeList(doc, stringParam(p, "shapes"), idx, error)) return Result::failure(error);
                } else {
                    idx = paintOrder(doc);
                    idx.erase(std::remove_if(idx.begin(), idx.end(), [&](int i) { return doc.lockedOrInside(i); }), idx.end());
                }
                std::vector<std::string> made, cut;
                std::vector<std::string> ids;
                for (int i : idx) ids.push_back(doc.shapes[static_cast<std::size_t>(i)].id);
                for (const auto& id : ids) {
                    const int i = doc.find(id);
                    const Shape& s = doc.shapes[static_cast<std::size_t>(i)];
                    if (s.kind == ShapeKind::Group || s.kind == ShapeKind::Instance || !editable(doc, i, error)) continue;
                    const Outline area = areaOf(doc, i);
                    if (area.empty()) continue;
                    Bounds box = bounds(area);
                    for (const auto& q : line) box.add(q);
                    const Outline side = knifeSide(line, box);
                    const bool nz = s.kind == ShapeKind::Text || s.fillRule == "nonzero";
                    std::vector<Outline> parts = pieces(booleanOp(area, side, BoolOp::Intersect, nz));
                    const auto other = pieces(booleanOp(area, side, BoolOp::Subtract, nz));
                    parts.insert(parts.end(), other.begin(), other.end());
                    if (parts.size() < 2) continue;  // the line misses it
                    const Shape from = s;
                    int at = i;
                    eraseSubtree(doc, i);
                    for (const auto& part : parts) made.push_back(areaShape(doc, from, part, at++));
                    cut.push_back(id);
                }
                if (cut.empty()) return Result::failure("the line doesn't cut across any of those filled shapes");
                std::string list;
                for (const auto& m : made) list += (list.empty() ? "" : ",") + m;
                Result r = Result::success("cut " + std::to_string(cut.size()) + " shape(s) into " + std::to_string(made.size()) + " pieces: " + list, list);
                r.with("shapes", list).with("pieces", static_cast<std::int64_t>(made.size()));
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.simplify", "Simplify Path",
             "Use fewer anchors: fits smooth curves to the path within `tolerance` px, keeping corners sharper "
             "than corner_angle degrees. Good after tracing or freehand drawing.",
             {docParam(), shapeParam(), numParam("tolerance", "how far the new path may stray, px", false, 1.0),
              numParam("corner_angle", "turns sharper than this stay corners, degrees", false, 60.0), previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                std::string error;
                BezierPath bp;
                if (!editable(doc, idx, error) || !canvasPath(doc.shapes[static_cast<std::size_t>(idx)], bp, error)) return Result::failure(error);
                const double tol = numberParam(p, "tolerance", 1.0);
                if (!(tol > 0)) return Result::failure("tolerance must be above 0");
                std::size_t before = 0, after = 0;
                BezierPath out;
                for (const auto& sp : bp) {
                    before += sp.nodes.size();
                    const Outline o = flatten({sp}, 4.0);
                    if (o.empty()) continue;
                    out.push_back(simplify(o[0].points, sp.closed, tol, numberParam(p, "corner_angle", 60)));
                    after += out.back().nodes.size();
                }
                storePath(doc.shapes[static_cast<std::size_t>(idx)], out);
                const Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                return Result::success("simplified " + s.id + " from " + std::to_string(before) + " to " + std::to_string(after) + " anchors", s.id)
                    .with("shape", s.id)
                    .with("before", static_cast<std::int64_t>(before))
                    .with("after", static_cast<std::int64_t>(after));
            })));

    reg.add({"vector.smooth", "Smooth Path",
             "Turn every anchor smooth: curves run through the existing anchors (tension 0 = straight, 0.5 = natural, 1 = loopy).",
             {docParam(), shapeParam(), numParam("tension", "curviness 0..1", false, 0.5), previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                std::string error;
                BezierPath bp;
                if (!editable(doc, idx, error) || !canvasPath(doc.shapes[static_cast<std::size_t>(idx)], bp, error)) return Result::failure(error);
                for (auto& sp : bp) {
                    std::vector<Point> pts;
                    for (const auto& n : sp.nodes) pts.push_back(n.p);
                    sp = smoothThrough(pts, sp.closed, numberParam(p, "tension", 0.5));
                }
                storePath(doc.shapes[static_cast<std::size_t>(idx)], bp);
                const Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                return Result::success("smoothed " + s.id, s.id).with("shape", s.id);
            })));

    reg.add({"vector.offset", "Offset Path",
             "Make a new shape whose edge runs `distance` px outside the shape (negative = inside), like Offset "
             "Path. The copy goes behind the original (place=front puts it on top). Returns its id.",
             {docParam(), shapeParam(), numParam("distance", "px outwards (negative shrinks)"),
              {"join", ParamType::String, "corner style", false, {"miter", "round", "bevel"}, std::string("miter")},
              numParam("miter_limit", "longest miter, times the distance", false, 4.0),
              {"place", ParamType::String, "where the new shape goes", false, {"back", "front"}, std::string("back")}, previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                const Outline area = areaOf(doc, idx);
                if (area.empty()) return Result::failure(doc.shapes[static_cast<std::size_t>(idx)].id + " has no area to offset");
                const Outline grown = offsetArea(area, numberParam(p, "distance"), stringParam(p, "join", "miter"), numberParam(p, "miter_limit", 4));
                if (grown.empty()) return Result::failure("nothing is left at that distance");
                const Shape from = doc.shapes[static_cast<std::size_t>(idx)];
                const int at = stringParam(p, "place", "back") == "front" ? doc.subtree(idx).back() + 1 : idx;
                const std::string id = areaShape(doc, from, grown, at);
                doc.shapes[static_cast<std::size_t>(doc.find(id))].name = from.name + " offset";
                return Result::success("offset " + from.id + " by " + num(numberParam(p, "distance")) + " px as " + id, id).with("shape", id);
            })));

    reg.add({"vector.outline_stroke", "Outline Stroke",
             "Turn a shape's outline into a filled shape of the same look (caps, joins, dashes, arrowheads and "
             "width profile included). The shape keeps its fill without the outline; the new shape sits on top. "
             "Returns the new shape's id.",
             {docParam(), shapeParam(), previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params&) {
                std::string error;
                if (!editable(doc, idx, error)) return Result::failure(error);
                const Shape from = doc.shapes[static_cast<std::size_t>(idx)];
                if (!from.stroke || from.strokeWidth <= 0) return Result::failure(from.id + " has no outline to outline");
                if (from.kind == ShapeKind::Group || from.kind == ShapeKind::Instance) return Result::failure("outline the shapes inside the group one by one");
                const Outline line = outlineIn(doc, idx, 4.0);
                const Outline area = outlineStroke(line, from.strokeWidth * shapeMatrix(from).scaleFactor(), from.line);
                if (area.empty()) return Result::failure(from.id + "'s outline covers nothing");
                Shape s = from;
                s.fill = from.stroke;
                s.gradient = from.strokeGradient;
                s.strokeGradient.reset();
                s.pattern.reset();
                s.fillSwatch = from.strokeSwatch;
                s.stroke.reset();
                s.strokeSwatch.clear();
                s.line = {};
                s.appearance.clear();
                const std::string id = areaShape(doc, s, area, doc.subtree(idx).back() + 1);
                Shape& made = doc.shapes[static_cast<std::size_t>(doc.find(id))];
                made.fillRule = "nonzero";
                made.name = from.name + " outline";
                Shape& orig = doc.shapes[static_cast<std::size_t>(doc.find(from.id))];
                orig.stroke.reset();
                orig.strokeGradient.reset();
                std::string removed;
                if (!orig.fill && !orig.gradient && !orig.pattern) {  // nothing left to show: the outline replaces it
                    eraseSubtree(doc, doc.find(from.id));
                    removed = " (replacing " + from.id + ")";
                }
                return Result::success("outlined the stroke of " + from.id + " as " + id + removed, id).with("shape", id);
            })));

    reg.add({"vector.reverse", "Reverse Path", "Reverse a path's direction (arrowheads swap ends; dashes start from the other end).",
             {docParam(), shapeParam(), previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params&) {
                std::string error;
                BezierPath bp;
                if (!editable(doc, idx, error) || !canvasPath(doc.shapes[static_cast<std::size_t>(idx)], bp, error)) return Result::failure(error);
                for (auto& sp : bp) reverse(sp);
                storePath(doc.shapes[static_cast<std::size_t>(idx)], bp);
                const Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                return Result::success("reversed " + s.id, s.id).with("shape", s.id);
            })));

    reg.add({"vector.close_path", "Close Path", "Close a path's open subpaths (closed=false opens closed ones at their start).",
             {docParam(), shapeParam(), {"closed", ParamType::Bool, "close (true) or open (false)", false, {}, true}, previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                std::string error;
                BezierPath bp;
                if (!editable(doc, idx, error) || !canvasPath(doc.shapes[static_cast<std::size_t>(idx)], bp, error)) return Result::failure(error);
                const bool closed = boolParam(p, "closed", true);
                for (auto& sp : bp) {
                    if (sp.closed == closed) continue;
                    if (!closed && !sp.nodes.empty()) {
                        Node end = sp.nodes.front();
                        end.out = end.p;
                        sp.nodes.front().in = sp.nodes.front().p;
                        sp.nodes.push_back(end);
                    }
                    sp.closed = closed;
                }
                storePath(doc.shapes[static_cast<std::size_t>(idx)], bp);
                const Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                return Result::success(std::string(closed ? "closed " : "opened ") + s.id, s.id).with("shape", s.id);
            })));
}

}  // namespace wetwaresuite::vector::detail
