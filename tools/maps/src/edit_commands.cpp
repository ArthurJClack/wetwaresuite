// Feature editing commands: attributes, moving features and vertices,
// splitting and merging. Each one is a MapEdit, so a single undo step.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "cmd.hpp"

namespace wetwaresuite::maps::cmd {

namespace {

using namespace detail;

std::string latLonText(const Coord& c) { return fmt(c.y, 6) + "," + fmt(c.x, 6); }
std::string vertexCountText(std::size_t n) { return n == 1 ? std::string("1 vertex") : std::to_string(n) + " vertices"; }

ParamSpec indexParam(const std::string& what = "feature index (maps.features shows them)") { return {"index", ParamType::Int, what, true}; }
ParamSpec vertexParam(const std::string& what) { return {"vertex", ParamType::Int, what, true}; }

// The features chosen by index= / indexes= / where=, in layer order.
std::vector<std::size_t> chosen(const Layer& l, const Params& p, std::string& error) {
    std::vector<std::size_t> out;
    const auto n = static_cast<std::int64_t>(l.features.size());
    if (has(p, "indexes")) {
        std::set<std::size_t> seen;
        for (double v : listParam(p, "indexes")) {
            const auto i = static_cast<std::int64_t>(std::llround(v));
            if (i < 0 || i >= n) {
                error = l.name + " has features 0.." + std::to_string(n - 1);
                return {};
            }
            seen.insert(static_cast<std::size_t>(i));
        }
        out.assign(seen.begin(), seen.end());
        return out;
    }
    if (has(p, "index")) {
        const std::int64_t i = intParam(p, "index");
        if (i < 0 || i >= n) {
            error = n ? l.name + " has features 0.." + std::to_string(n - 1) : l.name + " has no features";
            return {};
        }
        return {static_cast<std::size_t>(i)};
    }
    auto where = whereFrom(p, error);
    if (!error.empty()) return {};
    if (!where) {
        error = "give index=, indexes= or where=";
        return {};
    }
    for (std::size_t i = 0; i < l.features.size(); ++i) {
        if (where->test(l.features[i], i)) out.push_back(i);
    }
    if (out.empty()) error = "no feature of " + l.name + " matches" + unknownFieldsNote(l, *where);
    return out;
}

void registerAttributeCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.feature_update", "Edit Attributes",
             "Edit the attribute table: set properties on one feature (index=), several (indexes=) or every feature matching "
             "where=, from props=\"name=Home; visits=3\" (numbers and true/false are read as such); unset= removes fields. "
             "To compute a field from an expression use maps.calc.",
             {docParam(), layerParam(), {"index", ParamType::Int, "one feature", false}, {"indexes", ParamType::NumberList, "several features", false},
              whereParam("every feature matching"), {"props", ParamType::String, "key=value pairs separated by ; (or ,)", false},
              {"unset", ParamType::String, "fields to remove, comma-separated", false}, previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                const auto which = chosen(*l, p, error);
                if (which.empty()) return Result::failure(error);
                const Props set = parseProps(stringParam(p, "props"));
                const auto unset = splitList(stringParam(p, "unset"));
                if (set.empty() && unset.empty()) return Result::failure("give props=\"key=value; ...\" or unset=field,...");
                for (auto i : which) {
                    Feature& f = l->features[i];
                    for (const auto& [k, v] : set) f.props[k] = v;
                    for (const auto& k : unset) f.props.erase(k);
                }
                std::string what = propsLine(set, {});
                if (!unset.empty()) what += (what.empty() ? "" : "; ") + std::string("removed ") + stringParam(p, "unset");
                return edited("updated " + plural(which.size(), "feature") + " of " + l->name + ": " + what, p)
                    .with("layer", l->name)
                    .with("changed", static_cast<std::int64_t>(which.size()));
            }));

    reg.add({"maps.feature_move", "Move Feature",
             "Move a feature: by dx= metres east and dy= metres north, or to=lat,lon (a point goes exactly there; a line or "
             "polygon moves its centre there, keeping its shape and size on the ground).",
             {docParam(), layerParam(), indexParam(), {"dx", ParamType::Number, "metres east (negative = west)", false},
              {"dy", ParamType::Number, "metres north (negative = south)", false}, {"to", ParamType::NumberList, "lat,lon to move it to", false},
              previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                Feature* f = featureAt(*l, p, error);
                if (!f) return Result::failure(error);
                if (f->geom.empty()) return Result::failure("that feature has no geometry");
                double dx = numberParam(p, "dx", 0), dy = numberParam(p, "dy", 0);
                if (has(p, "to")) {
                    Coord to;
                    if (!latLon(p, "to", to, error)) return Result::failure(error);
                    const Coord from = f->geom.type == GeomType::Point && f->geom.points.size() == 1 ? f->geom.points[0] : centroid(f->geom);
                    const Crs crs = Crs::localTm(from.x);
                    const Coord a = crs.forward(from), b = crs.forward(to);
                    dx = b.x - a.x;
                    dy = b.y - a.y;
                    if (f->geom.type == GeomType::Point && f->geom.points.size() == 1) {
                        f->geom.points[0].x = to.x;
                        f->geom.points[0].y = to.y;
                        dx = dy = 0;
                    }
                } else if (!has(p, "dx") && !has(p, "dy")) {
                    return Result::failure("give dx=/dy= in metres or to=lat,lon");
                }
                if (dx != 0 || dy != 0) translate(f->geom, dx, dy);
                const Coord c = centroid(f->geom);
                return edited("moved #" + std::to_string(intParam(p, "index")) + " of " + l->name + ", now centred at " + latLonText(c) + " (lat,lon)", p)
                    .with("layer", l->name)
                    .with("lat", c.y)
                    .with("lon", c.x);
            }));
}

void registerVertexCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.vertices", "List Vertices",
             "List one feature's vertices with their numbers (used by maps.vertex_move/vertex_add/vertex_delete): lat,lon, "
             "elevation and time when known. Numbering runs through every part: points, each line, each polygon ring.",
             {docParam(), layerParam(), indexParam(), {"limit", ParamType::Int, "show at most this many", false, {}, std::int64_t{200}}}, false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                Feature* f = featureAt(*l, p, error);
                if (!f) return Result::failure(error);
                const auto vs = vertices(f->geom);
                const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "limit", 200), 1, 100000));
                std::string lines;
                for (std::size_t i = 0; i < vs.size() && i < limit; ++i) {
                    lines += "\n" + std::to_string(i) + ": " + latLonText(vs[i]);
                    if (!std::isnan(vs[i].z)) lines += " ele " + fmt(vs[i].z, 1) + " m";
                    if (!std::isnan(vs[i].t)) lines += " " + formatTime(vs[i].t);
                }
                if (vs.size() > limit) lines += "\n... " + std::to_string(vs.size() - limit) + " more";
                return edited("#" + std::to_string(intParam(p, "index")) + " of " + l->name + ": " + vertexCountText(vs.size()) + lines, p)
                    .with("count", static_cast<std::int64_t>(vs.size()));
            }, false));

    reg.add({"maps.vertex_move", "Move Vertex", "Move one vertex of a feature to lat,lon (maps.vertices numbers them). Elevation and time stay.",
             {docParam(), layerParam(), indexParam(), vertexParam("vertex number"), {"to", ParamType::NumberList, "lat,lon", true}, previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                Feature* f = featureAt(*l, p, error);
                if (!f) return Result::failure(error);
                Coord to;
                if (!latLon(p, "to", to, error)) return Result::failure(error);
                const std::int64_t v = intParam(p, "vertex");
                Coord* c = v >= 0 ? vertexAt(f->geom, static_cast<std::size_t>(v)) : nullptr;
                if (!c) return Result::failure("vertex must be 0.." + std::to_string(static_cast<std::int64_t>(f->geom.vertexCount()) - 1));
                const double moved = geodesicDistance(c->x, c->y, to.x, to.y);
                c->x = to.x;
                c->y = to.y;
                return edited("moved vertex " + std::to_string(v) + " of #" + std::to_string(intParam(p, "index")) + " by " + metres(moved), p).with("moved_m", moved);
            }));

    reg.add({"maps.vertex_add", "Add Vertex",
             "Add a vertex to a line or polygon at lat,lon: after= a vertex number, or (without after=) into the edge nearest to "
             "the point, which is how you bend a line by clicking on it.",
             {docParam(), layerParam(), indexParam(), {"at", ParamType::NumberList, "lat,lon of the new vertex", true},
              {"after", ParamType::Int, "insert after this vertex (default: into the nearest edge)", false}, previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                Feature* f = featureAt(*l, p, error);
                if (!f) return Result::failure(error);
                Coord at;
                if (!latLon(p, "at", at, error)) return Result::failure(error);
                std::size_t after = 0;
                if (has(p, "after")) {
                    const std::int64_t a = intParam(p, "after");
                    if (a < 0 || a >= static_cast<std::int64_t>(f->geom.vertexCount())) return Result::failure("after must be 0.." + std::to_string(static_cast<std::int64_t>(f->geom.vertexCount()) - 1));
                    after = static_cast<std::size_t>(a);
                } else {
                    if (f->geom.type == GeomType::Point) {
                        after = f->geom.points.size() - 1;
                    } else {
                        const EdgeHit hit = nearestEdge(f->geom, at.x, at.y);
                        if (!hit.found) return Result::failure("that feature has no edges");
                        after = hit.vertex;
                        // keep the elevation of the line there
                        at.z = hit.point.z;
                    }
                }
                if (!insertVertex(f->geom, after, at)) return Result::failure("couldn't insert after vertex " + std::to_string(after));
                return edited("added vertex " + std::to_string(after + 1) + " to #" + std::to_string(intParam(p, "index")) + " of " + l->name + " at " + latLonText(at), p)
                    .with("vertex", static_cast<std::int64_t>(after + 1));
            }));

    reg.add({"maps.vertex_delete", "Delete Vertex", "Remove one vertex of a feature (a line keeps at least 2, a polygon ring 3).",
             {docParam(), layerParam(), indexParam(), vertexParam("vertex number"), previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                Feature* f = featureAt(*l, p, error);
                if (!f) return Result::failure(error);
                const std::int64_t v = intParam(p, "vertex");
                if (v < 0 || !removeVertex(f->geom, static_cast<std::size_t>(v), error)) return Result::failure(error.empty() ? "vertex must be 0 or more" : error);
                return edited("deleted vertex " + std::to_string(v) + " of #" + std::to_string(intParam(p, "index")) + " (" + vertexCountText(f->geom.vertexCount()) + " left)", p);
            }));
}

void registerSplitMergeCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.split", "Split Line",
             "Split a line feature in two at=lat,lon (the nearest point on it) or at distance= metres along it. Both halves keep "
             "the properties; the second half is inserted right after the first.",
             {docParam(), layerParam(), indexParam(), {"at", ParamType::NumberList, "lat,lon near the line", false},
              {"distance", ParamType::Number, "metres from the start instead", false}, previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Line);
                if (!l) return Result::failure(error);
                Feature* f = featureAt(*l, p, error);
                if (!f) return Result::failure(error);
                if (f->geom.type != GeomType::Line) return Result::failure("only lines can be split (this is a " + std::string(toString(f->geom.type)) + ")");
                EdgeHit hit;
                if (has(p, "distance")) {
                    const double want = numberParam(p, "distance");
                    double D = 0;
                    std::size_t base = 0;
                    for (const auto& line : f->geom.lines) {
                        for (std::size_t i = 0; i + 1 < line.size() && !hit.found; ++i) {
                            const double d = geodesicDistance(line[i].x, line[i].y, line[i + 1].x, line[i + 1].y);
                            if (D + d >= want && d > 0) {
                                const double t = std::clamp((want - D) / d, 0.0, 1.0);
                                hit.found = true;
                                hit.vertex = base + i;
                                hit.point = line[i];
                                hit.point.x += (line[i + 1].x - line[i].x) * t;
                                hit.point.y += (line[i + 1].y - line[i].y) * t;
                                if (!std::isnan(line[i].z) && !std::isnan(line[i + 1].z)) hit.point.z = line[i].z + (line[i + 1].z - line[i].z) * t;
                                if (!std::isnan(line[i].t) && !std::isnan(line[i + 1].t)) hit.point.t = line[i].t + (line[i + 1].t - line[i].t) * t;
                            }
                            D += d;
                        }
                        base += line.size();
                    }
                    if (!hit.found) return Result::failure("the line is only " + metres(D) + " long");
                } else {
                    Coord at;
                    if (!latLon(p, "at", at, error)) return Result::failure("give at=lat,lon or distance=");
                    hit = nearestEdge(f->geom, at.x, at.y);
                }
                auto [a, b] = splitAt(f->geom, hit);
                if (a.empty() || b.empty()) return Result::failure("that is the end of the line: nothing to split off");
                const std::size_t i = static_cast<std::size_t>(intParam(p, "index"));
                Feature second{std::move(b), f->props};
                f->geom = std::move(a);
                const double la = length(f->geom), lb = length(second.geom);
                l->features.insert(l->features.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(second));
                return edited("split #" + std::to_string(i) + " of " + l->name + " at " + latLonText(hit.point) + ": " + metres(la) + " + " + metres(lb) + " (now #" +
                                  std::to_string(i) + " and #" + std::to_string(i + 1) + ")",
                              p)
                    .with("first_m", la)
                    .with("second_m", lb);
            }));

    reg.add({"maps.merge", "Merge Features",
             "Merge features of one layer into one: lines are joined end to end where they meet (reversed when needed; "
             "snap= metres), polygons become one multipolygon, points a multipoint. Pick them with indexes= or where=. The "
             "merged feature takes the first one's properties (or props=) and replaces them.",
             {docParam(), layerParam(), {"indexes", ParamType::NumberList, "feature indexes", false}, whereParam("merge every feature matching"),
              {"snap", ParamType::Number, "line ends this close (metres) are joined", false, {}, 1.0},
              {"props", ParamType::String, "properties for the merged feature (default: the first one's)", false}, previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                const auto which = chosen(*l, p, error);
                if (which.empty()) return Result::failure(error);
                if (which.size() < 2) return Result::failure("pick at least two features to merge");
                std::vector<const Geometry*> gs;
                for (auto i : which) gs.push_back(&l->features[i].geom);
                Feature merged;
                merged.geom = mergeGeometries(gs, std::max(0.0, numberParam(p, "snap", 1.0)));
                merged.props = has(p, "props") ? parseProps(stringParam(p, "props")) : l->features[which.front()].props;
                const std::size_t parts = merged.geom.lines.size() + merged.geom.polygons.size() + merged.geom.points.size();
                std::vector<Feature> keep;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    if (i == which.front()) keep.push_back(merged);
                    else if (!std::binary_search(which.begin(), which.end(), i)) keep.push_back(std::move(l->features[i]));
                }
                l->features = std::move(keep);
                return edited("merged " + plural(which.size(), "feature") + " of " + l->name + " into #" + std::to_string(which.front()) + " (" +
                                  plural(parts, "part") + ", " + geometryLine(merged.geom) + ")",
                              p)
                    .with("index", static_cast<std::int64_t>(which.front()))
                    .with("parts", static_cast<std::int64_t>(parts));
            }));
}

}  // namespace

void registerEditCommands(Session& session) {
    registerAttributeCommands(session);
    registerVertexCommands(session);
    registerSplitMergeCommands(session);
}

}  // namespace wetwaresuite::maps::cmd
