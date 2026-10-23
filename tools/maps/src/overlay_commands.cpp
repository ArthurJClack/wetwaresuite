// Layer-on-layer analysis: dissolve by attribute, polygon overlay
// (intersection, difference, union, symmetric difference) and select by location.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cmd.hpp"

namespace wetwaresuite::maps::cmd {

namespace {

using namespace detail;

int resolutionOf(const Params& p) { return static_cast<int>(std::clamp<std::int64_t>(intParam(p, "resolution", 800), 50, 4000)); }

// Every feature of a layer that matches where= (all without it).
std::vector<std::size_t> matching(const Layer& l, const std::optional<Expr>& where) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < l.features.size(); ++i) {
        if (!l.features[i].geom.empty() && (!where || where->test(l.features[i], i))) out.push_back(i);
    }
    return out;
}

// Points, lines or polygons cut by a set of polygons: what's inside (intersection) or outside (difference).
Geometry cutBy(const Geometry& g, const std::vector<const Geometry*>& by, bool inside, int resolution) {
    if (g.type == GeomType::Point) {
        Geometry out;
        out.type = GeomType::Point;
        for (const auto& c : g.points) {
            bool in = false;
            for (const Geometry* b : by) in = in || pointInGeometry(*b, c.x, c.y);
            if (in == inside) out.points.push_back(c);
        }
        if (out.points.empty()) out.type = GeomType::None;
        return out;
    }
    if (g.type == GeomType::Line) return clipLinesByPolygons(g, by, inside);
    return overlay({&g}, by, inside ? OverlayOp::Intersection : OverlayOp::Difference, resolution);
}

bool polygonal(const Layer& l) {
    for (const auto& f : l.features) {
        if (f.geom.type == GeomType::Polygon) return true;
    }
    return false;
}

void registerDissolve(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.dissolve", "Dissolve",
             "Merge a layer's features into one per value of field= (or one for everything): polygons are unioned so shared "
             "borders disappear (on a fine grid, resolution= cells across), lines are joined end to end, points become "
             "multipoints. Each result gets count and the sum of sum= fields. Writes a new layer.",
             {docParam(), layerParam(), {"field", ParamType::String, "group by this field (default: everything into one)", false},
              {"sum", ParamType::String, "numeric fields to add up per group, comma-separated", false}, whereParam(),
              {"resolution", ParamType::Int, "polygons: grid cells along the longer side", false, {}, std::int64_t{800}},
              {"exact", ParamType::Bool, "polygons: false keeps the original rings as one multipolygon instead of tracing the union", false, {}, true},
              outputParam("new layer name (default '<layer> dissolved')"), previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                const std::string field = stringParam(p, "field");
                const auto sums = splitList(stringParam(p, "sum"));
                std::map<std::string, std::vector<std::size_t>> groups;
                std::map<std::string, Prop> keyValue;
                for (auto i : matching(*l, where)) {
                    const Feature& f = l->features[i];
                    Prop v;
                    if (!field.empty()) {
                        if (auto it = f.props.find(field); it != f.props.end()) v = it->second;
                    }
                    const std::string key = v.isNull() ? std::string() : v.text();
                    groups[key].push_back(i);
                    keyValue[key] = v;
                }
                if (groups.empty()) return Result::failure("nothing to dissolve");
                Layer out;
                out.style = l->style;
                out.style.mode = "single";
                const int res = resolutionOf(p);
                for (const auto& [key, idx] : groups) {
                    std::vector<const Geometry*> gs;
                    for (auto i : idx) gs.push_back(&l->features[i].geom);
                    Feature nf;
                    bool polys = false;
                    for (const Geometry* g : gs) polys = polys || g->type == GeomType::Polygon;
                    if (polys && boolParam(p, "exact", true)) {
                        std::vector<const Geometry*> only;
                        for (const Geometry* g : gs) {
                            if (g->type == GeomType::Polygon) only.push_back(g);
                        }
                        nf.geom = overlay(only, {}, OverlayOp::Union, res);
                    } else {
                        nf.geom = mergeGeometries(gs);
                    }
                    if (!field.empty()) nf.props[field] = keyValue[key];
                    nf.props["count"] = Prop::number(static_cast<double>(idx.size()));
                    for (const auto& s : sums) {
                        double total = 0;
                        for (auto i : idx) {
                            if (auto it = l->features[i].props.find(s); it != l->features[i].props.end()) total += it->second.asNumber().value_or(0);
                        }
                        nf.props[s] = Prop::number(total);
                    }
                    if (!nf.geom.empty()) out.features.push_back(std::move(nf));
                }
                const std::string src = l->name;
                Layer& added = addLayer(doc.map, std::move(out), has(p, "output") ? stringParam(p, "output") : src + " dissolved");
                return edited("dissolved " + src + (field.empty() ? "" : " by " + field) + " into " + added.name + " (" + plural(added.features.size(), "feature") + ")", p)
                    .with("layer", added.name)
                    .with("features", static_cast<std::int64_t>(added.features.size()));
            }));
}

void registerOverlay(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.overlay", "Overlay",
             "Combine a layer with polygons of another (with=): intersection keeps the parts of each feature inside them (one "
             "result per overlapping pair, with both sets of properties; clashing names from with= get a with_ prefix); "
             "difference keeps the parts outside (erase); union merges both layers' polygons into one shape; sym_difference "
             "keeps what only one of them covers. Points and lines are cut exactly; polygon results are traced on a fine grid "
             "(resolution= cells across). Writes a new layer.",
             {docParam(), layerParam("the layer to cut"), {"with", ParamType::String, "the polygon layer to cut it with", true},
              {"op", ParamType::String, "what to keep", false, {"intersection", "difference", "union", "sym_difference"}, std::string("intersection")},
              whereParam("only these features of layer="), {"with_where", ParamType::String, "only these features of with=", false},
              {"resolution", ParamType::Int, "polygons: grid cells along the longer side", false, {}, std::int64_t{800}},
              outputParam("new layer name (default '<layer> <op>')"), previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* a = vectorLayer(doc.map, p, error);
                if (!a) return Result::failure(error);
                Layer* b = vectorLayer(doc.map, p, error, "with");
                if (!b) return Result::failure(error);
                auto whereA = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                auto whereB = whereFrom(p, error, "with_where");
                if (!error.empty()) return Result::failure(error);
                if (!polygonal(*b)) return Result::failure("'" + b->name + "' has no polygons to cut with");
                const std::string op = stringParam(p, "op", "intersection");
                const int res = resolutionOf(p);
                const auto ia = matching(*a, whereA), ib = matching(*b, whereB);
                std::vector<const Geometry*> bs;
                for (auto i : ib) {
                    if (b->features[i].geom.type == GeomType::Polygon) bs.push_back(&b->features[i].geom);
                }
                Layer out;
                out.style = a->style;
                out.style.mode = "single";
                if (op == "intersection") {
                    for (auto i : ia) {
                        const Feature& fa = a->features[i];
                        const BBox ba = bboxOf(fa.geom);
                        for (auto j : ib) {
                            const Feature& fb = b->features[j];
                            if (fb.geom.type != GeomType::Polygon || !ba.intersects(bboxOf(fb.geom)) || !intersects(fa.geom, fb.geom)) continue;
                            Feature nf;
                            nf.geom = cutBy(fa.geom, {&fb.geom}, true, res);
                            if (nf.geom.empty()) continue;
                            nf.props = fa.props;
                            for (const auto& [k, v] : fb.props) nf.props[fa.props.count(k) ? "with_" + k : k] = v;
                            out.features.push_back(std::move(nf));
                        }
                    }
                } else if (op == "difference") {
                    for (auto i : ia) {
                        const Feature& fa = a->features[i];
                        std::vector<const Geometry*> near;
                        for (const Geometry* g : bs) {
                            if (bboxOf(*g).intersects(bboxOf(fa.geom))) near.push_back(g);
                        }
                        Feature nf{near.empty() ? fa.geom : cutBy(fa.geom, near, false, res), fa.props};
                        if (!nf.geom.empty()) out.features.push_back(std::move(nf));
                    }
                } else if (op == "union") {
                    std::vector<const Geometry*> as;
                    for (auto i : ia) {
                        if (a->features[i].geom.type == GeomType::Polygon) as.push_back(&a->features[i].geom);
                    }
                    if (as.empty()) return Result::failure("union needs polygons in both layers ('" + a->name + "' has none)");
                    Feature nf;
                    nf.geom = overlay(as, bs, OverlayOp::Union, res);
                    nf.props["count"] = Prop::number(static_cast<double>(as.size() + bs.size()));
                    if (!nf.geom.empty()) out.features.push_back(std::move(nf));
                } else {  // sym_difference
                    for (auto i : ia) {
                        Feature nf{cutBy(a->features[i].geom, bs, false, res), a->features[i].props};
                        if (!nf.geom.empty()) out.features.push_back(std::move(nf));
                    }
                    std::vector<const Geometry*> as;
                    for (auto i : ia) {
                        if (a->features[i].geom.type == GeomType::Polygon) as.push_back(&a->features[i].geom);
                    }
                    for (auto j : ib) {
                        if (b->features[j].geom.type != GeomType::Polygon) continue;
                        Feature nf{as.empty() ? b->features[j].geom : overlay({&b->features[j].geom}, as, OverlayOp::Difference, res), b->features[j].props};
                        if (!nf.geom.empty()) out.features.push_back(std::move(nf));
                    }
                }
                if (out.features.empty()) return Result::failure(op + " of " + a->name + " and " + b->name + " is empty (do they overlap?)");
                const std::string an = a->name, bn = b->name;
                Layer& added = addLayer(doc.map, std::move(out), has(p, "output") ? stringParam(p, "output") : an + " " + op);
                double area = 0, len = 0;
                for (const auto& f : added.features) {
                    area += maps::area(f.geom);
                    len += f.geom.type == GeomType::Line ? length(f.geom) : 0;
                }
                std::string size = area > 0 ? squareMetres(area) : len > 0 ? metres(len) : plural(added.features.size(), "feature");
                return edited(op + " of " + an + " with " + bn + " -> " + added.name + " (" + plural(added.features.size(), "feature") + ", " + size + ")", p)
                    .with("layer", added.name)
                    .with("features", static_cast<std::int64_t>(added.features.size()))
                    .with("area_m2", area)
                    .with("length_m", len);
            }));
}

void registerSelectLocation(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.select_location", "Select by Location",
             "Keep the features of a layer by where they are compared with another layer (by=): intersects (touching counts), "
             "within (wholly inside a polygon of by=), contains (holding a feature of by= wholly), disjoint (touching none), or "
             "near (within distance= metres). Writes output= (default '<layer> selected'), or in_place=true.",
             {docParam(), layerParam("the layer to select from"), {"by", ParamType::String, "the layer to compare with", true},
              {"mode", ParamType::String, "the test", false, {"intersects", "within", "contains", "disjoint", "near"}, std::string("intersects")},
              {"distance", ParamType::Number, "near: metres", false, {}, 1000.0}, {"by_where", ParamType::String, "only these features of by=", false},
              outputParam("new layer name"), {"in_place", ParamType::Bool, "drop the others from this layer instead", false, {}, false}, previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                Layer* by = vectorLayer(doc.map, p, error, "by");
                if (!by) return Result::failure(error);
                auto whereB = whereFrom(p, error, "by_where");
                if (!error.empty()) return Result::failure(error);
                const std::string byName = by->name;
                const std::string mode = stringParam(p, "mode", "intersects");
                const double dist = numberParam(p, "distance", 1000);
                std::vector<const Feature*> others;
                for (auto j : matching(*by, whereB)) others.push_back(&by->features[j]);
                auto inside = [](const Geometry& inner, const Geometry& outer) {
                    if (outer.type != GeomType::Polygon || !intersects(inner, outer)) return false;
                    bool all = true;
                    inner.forEach([&](const Coord& c) { all = all && pointInGeometry(outer, c.x, c.y); });
                    // no edge of the outer polygon may cross the inner shape's interior: check the inner's edge midpoints too
                    if (all && inner.type != GeomType::Point) {
                        const auto vs = vertices(inner);
                        for (std::size_t k = 0; k + 1 < vs.size() && all; ++k) all = pointInGeometry(outer, (vs[k].x + vs[k + 1].x) / 2, (vs[k].y + vs[k + 1].y) / 2);
                    }
                    return all;
                };
                std::vector<Feature> keep;
                for (const auto& f : l->features) {
                    const BBox fb = bboxOf(f.geom);
                    bool hit = false;
                    for (const Feature* o : others) {
                        if (mode == "near") {
                            hit = distanceBetween(f.geom, o->geom) <= dist;
                        } else if (mode == "within") {
                            hit = fb.intersects(bboxOf(o->geom)) && inside(f.geom, o->geom);
                        } else if (mode == "contains") {
                            hit = fb.intersects(bboxOf(o->geom)) && inside(o->geom, f.geom);
                        } else {
                            hit = fb.intersects(bboxOf(o->geom)) && intersects(f.geom, o->geom);
                        }
                        if (hit) break;
                    }
                    if (mode == "disjoint") hit = !hit;
                    if (hit) keep.push_back(f);
                }
                const std::size_t n = keep.size(), total = l->features.size();
                std::string name = l->name;
                if (boolParam(p, "in_place", false)) {
                    l->features = std::move(keep);
                } else {
                    Layer nl = *l;
                    nl.features = std::move(keep);
                    name = addLayer(doc.map, std::move(nl), has(p, "output") ? stringParam(p, "output") : l->name + " selected").name;
                }
                return edited(std::to_string(n) + " of " + plural(total, "feature") + " " + mode + " " + byName + " -> " + name, p)
                    .with("layer", name)
                    .with("count", static_cast<std::int64_t>(n));
            }));
}

}  // namespace

void registerOverlayCommands(Session& session) {
    registerDissolve(session);
    registerOverlay(session);
    registerSelectLocation(session);
}

}  // namespace wetwaresuite::maps::cmd
