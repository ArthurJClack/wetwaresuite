// Path editing commands: anchors and handles (the pen and direct selection
// tools), joining, splitting, the knife, simplify, smooth, offset path,
// outline stroke and reversing. Anchors are numbered across all subpaths,
// in canvas coordinates (the shape's transform applied), as vector.nodes lists them.
#include <algorithm>
#include <cmath>

#include "internal.hpp"

namespace wetwaresuite::vector::detail {

namespace {

Point sub(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
Point add(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
Point mul(Point a, double k) { return {a.x * k, a.y * k}; }
double len(Point a) { return std::hypot(a.x, a.y); }

// Global anchor number -> (subpath, index). False when out of range.
bool locate(const BezierPath& bp, int n, std::size_t& sub, std::size_t& k) {
    if (n < 0) return false;
    for (std::size_t s = 0; s < bp.size(); ++s) {
        if (static_cast<std::size_t>(n) < bp[s].nodes.size()) {
            sub = s;
            k = static_cast<std::size_t>(n);
            return true;
        }
        n -= static_cast<int>(bp[s].nodes.size());
    }
    return false;
}

std::size_t countNodes(const BezierPath& bp) {
    std::size_t n = 0;
    for (const auto& sp : bp) n += sp.nodes.size();
    return n;
}

bool nodeArgs(const Params& p, const BezierPath& bp, std::vector<int>& out, std::string& error) {
    out.clear();
    for (const auto& part : splitOn(stringParam(p, "node", ""), ',')) {
        char* end = nullptr;
        const long v = std::strtol(part.c_str(), &end, 10);
        std::size_t s = 0, k = 0;
        if (end == part.c_str() || *end || !locate(bp, static_cast<int>(v), s, k)) {
            return error = "node '" + part + "' isn't an anchor number (0.." + std::to_string(countNodes(bp)) + "-1, see vector.nodes)", false;
        }
        out.push_back(static_cast<int>(v));
    }
    if (out.empty()) return error = "give node= (an anchor number from vector.nodes, or several: 0,3)", false;
    return true;
}

std::string pt(Point p) { return num(p.x) + "," + num(p.y); }

// Handles for a smooth anchor: along the line between its neighbours, a third of each side's chord.
void smoothNode(SubPath& sp, std::size_t k, bool symmetric) {
    const std::size_t n = sp.nodes.size();
    Node& node = sp.nodes[k];
    const bool hasPrev = sp.closed || k > 0, hasNext = sp.closed || k + 1 < n;
    const Point prev = hasPrev ? sp.nodes[(k + n - 1) % n].p : node.p;
    const Point next = hasNext ? sp.nodes[(k + 1) % n].p : node.p;
    Point dir = sub(next, prev);
    if (len(dir) < 1e-9) dir = {1, 0};
    dir = mul(dir, 1 / len(dir));
    double li = hasPrev ? len(sub(node.p, prev)) / 3 : 0, lo = hasNext ? len(sub(next, node.p)) / 3 : 0;
    if (symmetric) li = lo = (li + lo) / (hasPrev && hasNext ? 2 : 1);
    node.in = sub(node.p, mul(dir, li));
    node.out = add(node.p, mul(dir, lo));
}

// Reads a pen anchor list: "x,y; x,y,ox,oy; x,y,ix,iy,ox,oy" (out handle mirrored when only it is given).
bool penNodes(const std::string& text, std::vector<Node>& out, std::string& error) {
    out.clear();
    for (const auto& item : splitOn(text, ';')) {
        std::vector<double> v;
        for (const auto& part : splitOn(item, ',')) {
            char* end = nullptr;
            const double x = std::strtod(part.c_str(), &end);
            if (end == part.c_str() || *end) return error = "'" + part + "' isn't a number in nodes", false;
            v.push_back(x);
        }
        Node n;
        if (v.size() == 2) {
            n.p = n.in = n.out = {v[0], v[1]};
        } else if (v.size() == 4) {
            n.p = {v[0], v[1]};
            n.out = {v[2], v[3]};
            n.in = sub(mul(n.p, 2), n.out);
        } else if (v.size() == 6) {
            n.p = {v[0], v[1]};
            n.in = {v[2], v[3]};
            n.out = {v[4], v[5]};
        } else {
            return error = "each anchor is x,y or x,y,out_x,out_y or x,y,in_x,in_y,out_x,out_y (anchors separated by ;)", false;
        }
        out.push_back(n);
    }
    if (out.size() < 2) return error = "a path needs at least two anchors, e.g. nodes=\"10,10; 90,10\"", false;
    return true;
}

Result pathResult(const Shape& s, const std::string& what) {
    return Result::success(what + " " + s.id + ": " + describe(s), s.id).with("shape", s.id);
}

// Runs `fn` on the shape's canvas anchors, then stores them back.
Edit onAnchors(std::function<Result(VectorDoc&, int, BezierPath&, const Params&)> fn) {
    return onShape([fn](VectorDoc& doc, int idx, const Params& p) {
        std::string error;
        if (!editable(doc, idx, error)) return Result::failure(error);
        BezierPath bp;
        if (!canvasPath(doc.shapes[static_cast<std::size_t>(idx)], bp, error)) return Result::failure(error);
        Result r = fn(doc, idx, bp, p);
        if (r.ok && idx < static_cast<int>(doc.shapes.size())) storePath(doc.shapes[static_cast<std::size_t>(idx)], bp);
        return r;
    });
}

void registerAnchorCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"vector.nodes", "List Anchors",
             "List a shape's anchors (the pen tool's points) with their handles, on the canvas. Anchors are numbered "
             "from 0 across all subpaths; the other anchor commands take these numbers. type is corner, smooth "
             "(handles in line) or symmetric (in line and the same length).",
             {docParam(), shapeParam()}, false, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params&) {
                BezierPath bp;
                std::string error;
                if (!canvasPath(doc.shapes[static_cast<std::size_t>(idx)], bp, error)) return Result::failure(error);
                std::string text = doc.shapes[static_cast<std::size_t>(idx)].id + ": " + std::to_string(countNodes(bp)) + " anchors in " +
                                   std::to_string(bp.size()) + (bp.size() == 1 ? " subpath" : " subpaths");
                std::string data;
                int n = 0;
                for (std::size_t s = 0; s < bp.size(); ++s) {
                    text += "\nsubpath " + std::to_string(s) + (bp[s].closed ? " (closed)" : " (open)");
                    for (const auto& node : bp[s].nodes) {
                        const std::string type = toString(nodeType(node));
                        text += "\n  " + std::to_string(n++) + " " + type + " at " + pt(node.p) + " in " + pt(node.in) + " out " + pt(node.out);
                        data += (data.empty() ? "" : ";") + std::to_string(s) + ":" + pt(node.p) + "," + pt(node.in) + "," + pt(node.out) + ":" + type;
                    }
                }
                std::string closed;
                for (const auto& sp : bp) closed += (closed.empty() ? "" : ",") + std::string(sp.closed ? "1" : "0");
                return Result::success(text)
                    .with("count", static_cast<std::int64_t>(countNodes(bp)))
                    .with("nodes", data)
                    .with("closed", closed);
            }), false));

    reg.add({"vector.add_node", "Add Anchor",
             "Add an anchor without changing the path's shape: at the point on the path nearest x,y, or on the "
             "segment after anchor `node` at fraction t. The shape becomes a path. Returns the new anchor number.",
             {docParam(), shapeParam(), numParam("x", "near this x", false), numParam("y", "near this y", false),
              {"node", ParamType::String, "anchor number whose following segment gets the new anchor", false},
              numParam("t", "where along that segment, 0..1", false, 0.5), previewParam()},
             true},
            vectorCommand(session, onAnchors([](VectorDoc& doc, int idx, BezierPath& bp, const Params& p) {
                std::size_t sub = 0, seg = 0;
                double t = std::clamp(numberParam(p, "t", 0.5), 0.001, 0.999);
                if (has(p, "x") && has(p, "y")) {
                    const PathHit hit = nearestOnPath(bp, {numberParam(p, "x"), numberParam(p, "y")});
                    if (hit.distance > 1e200) return Result::failure("the path has no segments");
                    sub = hit.sub, seg = hit.seg, t = hit.t;
                } else {
                    std::vector<int> nodes;
                    std::string error;
                    if (!nodeArgs(p, bp, nodes, error)) return Result::failure("give x,y or node: " + error);
                    locate(bp, nodes[0], sub, seg);
                }
                SubPath& sp = bp[sub];
                if (!sp.closed && seg + 1 >= sp.nodes.size()) return Result::failure("that anchor ends an open path; there's no segment after it");
                if (t <= 1e-6 || t >= 1 - 1e-6) return Result::failure("that point is already an anchor");
                const std::size_t at = splitSegment(sp, seg, t);
                int number = static_cast<int>(at);
                for (std::size_t s = 0; s < sub; ++s) number += static_cast<int>(bp[s].nodes.size());
                Result r = pathResult(doc.shapes[static_cast<std::size_t>(idx)], "added anchor " + std::to_string(number) + " at " + pt(sp.nodes[at].p) + " to");
                return r.with("node", static_cast<std::int64_t>(number));
            })));

    reg.add({"vector.delete_node", "Delete Anchor",
             "Remove anchors (node=3 or node=1,4). The neighbours join directly; a subpath left with one anchor goes.",
             {docParam(), shapeParam(), {"node", ParamType::String, "anchor number(s) from vector.nodes", true}, previewParam()},
             true},
            vectorCommand(session, onAnchors([](VectorDoc& doc, int idx, BezierPath& bp, const Params& p) {
                std::vector<int> nodes;
                std::string error;
                if (!nodeArgs(p, bp, nodes, error)) return Result::failure(error);
                std::sort(nodes.rbegin(), nodes.rend());
                for (int n : nodes) {
                    std::size_t s = 0, k = 0;
                    if (locate(bp, n, s, k)) bp[s].nodes.erase(bp[s].nodes.begin() + static_cast<std::ptrdiff_t>(k));
                }
                bp.erase(std::remove_if(bp.begin(), bp.end(), [](const SubPath& sp) { return sp.nodes.size() < 2; }), bp.end());
                if (bp.empty()) return Result::failure("that would leave no path; use vector.delete to remove the shape");
                return pathResult(doc.shapes[static_cast<std::size_t>(idx)], "removed " + std::to_string(nodes.size()) + " anchor(s) from");
            })));

    reg.add({"vector.move_node", "Move Anchor",
             "Move an anchor (with its handles) or one of its handles: part=anchor|in|out, to x,y or by dx,dy. "
             "Moving a handle of a smooth anchor turns the other handle too (symmetric ones mirror it); "
             "corner=true breaks the pair so the handle moves alone.",
             {docParam(), shapeParam(), {"node", ParamType::String, "anchor number(s) from vector.nodes", true},
              {"part", ParamType::String, "what to move", false, {"anchor", "in", "out"}, std::string("anchor")},
              numParam("x", "new x", false), numParam("y", "new y", false), numParam("dx", "move right by", false, 0.0),
              numParam("dy", "move down by", false, 0.0), {"corner", ParamType::Bool, "break smooth handles", false, {}, false},
              previewParam()},
             true},
            vectorCommand(session, onAnchors([](VectorDoc& doc, int idx, BezierPath& bp, const Params& p) {
                std::vector<int> nodes;
                std::string error;
                if (!nodeArgs(p, bp, nodes, error)) return Result::failure(error);
                const std::string part = stringParam(p, "part", "anchor");
                for (int n : nodes) {
                    std::size_t s = 0, k = 0;
                    locate(bp, n, s, k);
                    Node& node = bp[s].nodes[k];
                    Point& target = part == "in" ? node.in : part == "out" ? node.out : node.p;
                    Point to = target;
                    if (has(p, "x")) to.x = numberParam(p, "x");
                    if (has(p, "y")) to.y = numberParam(p, "y");
                    to.x += numberParam(p, "dx", 0);
                    to.y += numberParam(p, "dy", 0);
                    if (part == "anchor") {
                        const Point d = sub(to, node.p);
                        node.p = to, node.in = add(node.in, d), node.out = add(node.out, d);
                        continue;
                    }
                    const NodeType type = nodeType(node);
                    target = to;
                    if (boolParam(p, "corner", false) || type == NodeType::Corner) continue;
                    Point& other = part == "in" ? node.out : node.in;
                    const Point dir = sub(node.p, to);
                    const double l = len(dir);
                    if (l < 1e-9) continue;
                    const double ol = type == NodeType::Symmetric ? l : len(sub(other, node.p));
                    other = add(node.p, mul(dir, ol / l));
                }
                return pathResult(doc.shapes[static_cast<std::size_t>(idx)], "moved " + part + " of anchor(s) " + stringParam(p, "node") + " on");
            })));

    reg.add({"vector.convert_node", "Convert Anchor",
             "Change anchors between corner (handles pulled in: sharp), smooth (handles in line) and symmetric "
             "(in line, equal length), like the convert-anchor tool.",
             {docParam(), shapeParam(), {"node", ParamType::String, "anchor number(s) from vector.nodes", true},
              {"type", ParamType::String, "the new kind of anchor", true, {"corner", "smooth", "symmetric"}}, previewParam()},
             true},
            vectorCommand(session, onAnchors([](VectorDoc& doc, int idx, BezierPath& bp, const Params& p) {
                std::vector<int> nodes;
                std::string error;
                if (!nodeArgs(p, bp, nodes, error)) return Result::failure(error);
                const std::string type = stringParam(p, "type");
                for (int n : nodes) {
                    std::size_t s = 0, k = 0;
                    locate(bp, n, s, k);
                    Node& node = bp[s].nodes[k];
                    if (type == "corner") node.in = node.out = node.p;
                    else smoothNode(bp[s], k, type == "symmetric");
                }
                return pathResult(doc.shapes[static_cast<std::size_t>(idx)], "made anchor(s) " + stringParam(p, "node") + " " + type + " on");
            })));

    reg.add({"vector.pen", "Pen Path",
             "Draw a path anchor by anchor, like the pen tool: nodes=\"10,90; 50,10,70,10; 90,90\" where each anchor "
             "is x,y (a corner), x,y,out_x,out_y (a smooth anchor: the in handle mirrors the out one) or "
             "x,y,in_x,in_y,out_x,out_y. smooth=true instead runs smooth curves through plain points. "
             "closed=true joins the end to the start.",
             join({docParam(), {"nodes", ParamType::String, "anchors separated by ;", true},
                   {"closed", ParamType::Bool, "close the path", false, {}, false},
                   {"smooth", ParamType::Bool, "smooth curves through the anchors (ignores handles)", false, {}, false},
                   numParam("tension", "curviness for smooth=true, 0..1", false, 0.5)},
                  styleParams("none", "black", 2)),
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<Node> nodes;
                std::string error;
                if (!penNodes(stringParam(p, "nodes"), nodes, error)) return Result::failure(error);
                SubPath sp;
                sp.closed = boolParam(p, "closed", false);
                if (boolParam(p, "smooth", false)) {
                    std::vector<Point> pts;
                    for (const auto& n : nodes) pts.push_back(n.p);
                    sp = smoothThrough(pts, sp.closed, numberParam(p, "tension", 0.5));
                } else {
                    sp.nodes = nodes;
                }
                Shape s;
                s.kind = ShapeKind::Path;
                s.d = pathData({sp});
                s.stroke = Color{0, 0, 0, 255};
                s.strokeWidth = 2;
                return addShape(doc, std::move(s), p);
            }));
}

}  // namespace

void registerPathOpCommands(Session& session);

void registerPathCommands(Session& session) {
    registerAnchorCommands(session);
    registerPathOpCommands(session);
}

}  // namespace wetwaresuite::vector::detail
