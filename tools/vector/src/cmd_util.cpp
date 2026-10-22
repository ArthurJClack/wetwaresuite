// Helpers the command files share: picking shapes from params, moving blocks
// of shapes in the stacking order, grouping, hit testing and snapping.
#include <algorithm>
#include <cmath>
#include <set>

#include "internal.hpp"

namespace wetwaresuite::vector::detail {

bool shapeList(const VectorDoc& doc, const std::string& list, std::vector<int>& out, std::string& error) {
    out.clear();
    for (const auto& name : splitOn(list, ',')) {
        const int i = doc.find(name);
        if (i < 0) return error = "no shape '" + name + "' in this document (vector.info lists them)", false;
        if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    }
    if (out.empty()) return error = "give at least one shape", false;
    return true;
}

bool selection(const VectorDoc& doc, const Params& p, std::vector<int>& out, std::string& error) {
    if (has(p, "shapes")) return shapeList(doc, stringParam(p, "shapes"), out, error);
    if (has(p, "shape")) return shapeList(doc, stringParam(p, "shape"), out, error);
    return error = "give shape= (one shape) or shapes= (a comma-separated list)", false;
}

std::vector<int> topmost(const VectorDoc& doc, std::vector<int> idx) {
    std::set<std::string> ids;
    for (int i : idx) ids.insert(doc.shapes[static_cast<std::size_t>(i)].id);
    std::vector<int> out;
    for (int i : idx) {
        bool inside = false;
        std::string parent = doc.shapes[static_cast<std::size_t>(i)].parent;
        for (int guard = 0; !parent.empty() && guard < 64; ++guard) {
            if (ids.count(parent)) {
                inside = true;
                break;
            }
            const int pi = doc.find(parent);
            if (pi < 0) break;
            parent = doc.shapes[static_cast<std::size_t>(pi)].parent;
        }
        if (!inside) out.push_back(i);
    }
    return out;
}

bool editable(const VectorDoc& doc, int idx, std::string& error) {
    if (!doc.lockedOrInside(idx)) return true;
    error = doc.shapes[static_cast<std::size_t>(idx)].id + " is locked (vector.lock locked=false unlocks it)";
    return false;
}

Outline areaOf(const VectorDoc& doc, int idx, double detail) {
    Outline o = outlineIn(doc, idx, detail);
    for (auto& c : o) c.closed = true;
    o.erase(std::remove_if(o.begin(), o.end(), [](const Contour& c) { return c.points.size() < 3; }), o.end());
    return o;
}

std::vector<Outline> pieces(const Outline& area) {
    const std::size_t n = area.size();
    std::vector<int> depth(n, 0), owner(n, -1);
    std::vector<double> size(n);
    for (std::size_t i = 0; i < n; ++i) size[i] = std::abs(signedArea(area[i].points));
    for (std::size_t i = 0; i < n; ++i) {
        if (area[i].points.empty()) continue;
        // a point just inside the contour's edge stands for it (the first point lies on other contours' edges rarely)
        const Point a = area[i].points[0], b = area[i].points[1 % area[i].points.size()];
        const Point probe{(a.x + b.x) / 2, (a.y + b.y) / 2};
        for (std::size_t j = 0; j < n; ++j) {
            if (i == j || size[j] <= size[i]) continue;
            if (insideOutline(Outline{area[j]}, probe)) {
                ++depth[i];
                if (owner[i] < 0 || size[j] < size[static_cast<std::size_t>(owner[i])]) owner[i] = static_cast<int>(j);
            }
        }
    }
    std::vector<Outline> out;
    std::vector<int> slot(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 0) {
            slot[i] = static_cast<int>(out.size());
            out.push_back({area[i]});
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 1 && owner[i] >= 0 && slot[static_cast<std::size_t>(owner[i])] >= 0) {
            out[static_cast<std::size_t>(slot[static_cast<std::size_t>(owner[i])])].push_back(area[i]);
        }
    }
    return out;
}

int moveBlock(VectorDoc& doc, std::vector<int> idx, int before) {
    std::set<int> all;
    for (int i : idx) {
        for (int k : doc.subtree(i)) all.insert(k);
    }
    std::vector<Shape> block;
    int removedBefore = 0;
    for (int k : all) {
        block.push_back(doc.shapes[static_cast<std::size_t>(k)]);
        if (k < before) ++removedBefore;
    }
    for (auto it = all.rbegin(); it != all.rend(); ++it) doc.shapes.erase(doc.shapes.begin() + *it);
    const int at = std::clamp(before - removedBefore, 0, static_cast<int>(doc.shapes.size()));
    doc.shapes.insert(doc.shapes.begin() + at, block.begin(), block.end());
    return at;
}

std::string freshId(VectorDoc& doc) { return "s" + std::to_string(doc.nextId++); }

std::string insertShape(VectorDoc& doc, Shape s, int at) {
    if (s.id.empty()) {
        const int n = doc.nextId;
        s.id = freshId(doc);
        if (s.name.empty()) s.name = std::string(toString(s.kind)) + " " + std::to_string(n);
    }
    const std::string id = s.id;
    at = std::clamp(at, 0, static_cast<int>(doc.shapes.size()));
    doc.shapes.insert(doc.shapes.begin() + at, std::move(s));
    return id;
}

std::string groupShapes(VectorDoc& doc, std::vector<int> idx, const std::string& name) {
    idx = topmost(doc, idx);
    std::sort(idx.begin(), idx.end());
    const int top = idx.back();
    Shape g;
    g.kind = ShapeKind::Group;
    g.parent = doc.shapes[static_cast<std::size_t>(top)].parent;
    g.name = name;
    const std::string gid = insertShape(doc, std::move(g), top + 1);  // after `top`, so earlier indices stay put
    for (int i : idx) doc.shapes[static_cast<std::size_t>(i)].parent = gid;
    // the children follow their group in the list (tidy, and keeps blocks together)
    const int gi = doc.find(gid);
    moveBlock(doc, idx, gi + 1);
    return gid;
}

namespace {

void order(const VectorDoc& doc, const std::map<std::string, std::vector<int>>& kids, const std::string& parent, std::vector<int>& out,
           int depth) {
    auto it = kids.find(parent);
    if (it == kids.end() || depth > 32) return;
    const Shape* group = parent.empty() ? nullptr : &doc.shapes[static_cast<std::size_t>(doc.find(parent))];
    for (int i : it->second) {
        const Shape& s = doc.shapes[static_cast<std::size_t>(i)];
        if (!s.visible) continue;
        if (group && (s.id == group->clip || s.id == group->mask)) continue;
        if (s.kind == ShapeKind::Group) order(doc, kids, s.id, out, depth + 1);
        else out.push_back(i);
    }
}

double segDistance(Point p, Point a, Point b) {
    const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

}  // namespace

std::vector<int> paintOrder(const VectorDoc& doc) {
    std::map<std::string, std::vector<int>> kids;
    for (std::size_t i = 0; i < doc.shapes.size(); ++i) kids[doc.shapes[i].parent].push_back(static_cast<int>(i));
    std::vector<int> out;
    order(doc, kids, "", out, 0);
    return out;
}

int hitTest(const VectorDoc& doc, Point p, double tolerance) {
    const auto list = paintOrder(doc);
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        const int i = *it;
        const Shape& s = doc.shapes[static_cast<std::size_t>(i)];
        if (doc.lockedOrInside(i)) continue;
        const Outline o = outlineIn(doc, i, 1.0);
        const Bounds b = bounds(o);
        const double reach = tolerance + (s.stroke ? s.strokeWidth / 2 : 0);
        if (b.empty || p.x < b.x0 - reach || p.x > b.x1 + reach || p.y < b.y0 - reach || p.y > b.y1 + reach) continue;
        const bool filled = s.fill || s.gradient || s.pattern || s.kind == ShapeKind::Text || s.kind == ShapeKind::Instance;
        if (filled) {
            Outline closed = o;
            for (auto& c : closed) c.closed = true;
            if (insideOutline(closed, p, s.kind == ShapeKind::Text || s.fillRule == "nonzero")) return i;
        }
        for (const auto& c : o) {
            const std::size_t n = c.points.size();
            for (std::size_t k = 0; k + 1 < n + (c.closed ? 1 : 0); ++k) {
                if (segDistance(p, c.points[k], c.points[(k + 1) % n]) <= reach) return i;
            }
            if (n == 1 && std::hypot(p.x - c.points[0].x, p.y - c.points[0].y) <= reach) return i;
        }
    }
    return -1;
}

int selectable(const VectorDoc& doc, int idx) {
    int best = idx;
    int cur = idx;
    for (int guard = 0; guard < 64; ++guard) {
        const std::string& parent = doc.shapes[static_cast<std::size_t>(cur)].parent;
        if (parent.empty()) break;
        const int pi = doc.find(parent);
        if (pi < 0 || doc.shapes[static_cast<std::size_t>(pi)].layer) break;
        best = cur = pi;
    }
    return best;
}

Point snapPoint(const VectorDoc& doc, Point p, double radius, std::string* what) {
    Point best = p;
    double bx = radius, by = radius;
    std::string sx, sy;
    auto tryX = [&](double x, const std::string& w) {
        if (std::abs(x - p.x) < bx) bx = std::abs(x - p.x), best.x = x, sx = w;
    };
    auto tryY = [&](double y, const std::string& w) {
        if (std::abs(y - p.y) < by) by = std::abs(y - p.y), best.y = y, sy = w;
    };
    if (doc.snapPoints) {
        double bd = radius;
        Point hit{};
        bool found = false;
        for (int i : paintOrder(doc)) {
            BezierPath bp;
            std::string err;
            const Shape& s = doc.shapes[static_cast<std::size_t>(i)];
            if (!canvasPath(s, bp, err)) continue;
            for (const auto& sp : bp) {
                for (const auto& n : sp.nodes) {
                    const double d = std::hypot(n.p.x - p.x, n.p.y - p.y);
                    if (d < bd) bd = d, hit = n.p, found = true;
                }
            }
        }
        if (found) {
            if (what) *what = "point";
            return hit;
        }
    }
    if (doc.snapGuides) {
        for (double x : doc.guidesX) tryX(x, "guide");
        for (double y : doc.guidesY) tryY(y, "guide");
    }
    if (doc.snapGrid && doc.gridSize > 0) {
        tryX(std::round(p.x / doc.gridSize) * doc.gridSize, "grid");
        tryY(std::round(p.y / doc.gridSize) * doc.gridSize, "grid");
    }
    if (what) {
        *what = sx.empty() && sy.empty() ? "none" : (sx == sy ? sx : (sx.empty() ? sy : (sy.empty() ? sx : sx + "+" + sy)));
    }
    return best;
}

bool canvasPath(const Shape& s, BezierPath& out, std::string& error) {
    if (s.kind == ShapeKind::Text || s.kind == ShapeKind::Group || s.kind == ShapeKind::Instance) {
        error = s.id + " is a " + std::string(toString(s.kind)) + "; anchors belong to paths and shapes (vector.to_path converts text)";
        return false;
    }
    out = shapePath(s);
    transformPath(out, shapeMatrix(s));
    if (out.empty()) return error = s.id + " has no anchors", false;
    return true;
}

void storePath(Shape& s, const BezierPath& bp) {
    if (!s.transform.identity()) s.strokeWidth *= shapeMatrix(s).scaleFactor();
    s.kind = ShapeKind::Path;
    s.d = pathData(bp);
    s.points.clear();
    s.transform = {};
}

std::string idText(const VectorDoc& doc, const std::vector<int>& idx) {
    std::string out;
    for (int i : idx) out += (out.empty() ? "" : ",") + doc.shapes[static_cast<std::size_t>(i)].id;
    return out;
}

}  // namespace wetwaresuite::vector::detail
