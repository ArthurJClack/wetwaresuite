// Bookmarks, the map's view (what the window and previews show) and maps.hit,
// which says what is under a point of the window's picture so the click tools
// can run the ordinary commands.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "cmd.hpp"

namespace wetwaresuite::maps::cmd {

namespace {

using namespace detail;

std::string latLonText(const Coord& c) { return fmt(c.y, 6) + "," + fmt(c.x, 6); }

const Bookmark* findBookmark(const Map& m, const std::string& name) {
    for (const auto& b : m.bookmarks) {
        if (lower(b.name) == lower(name)) return &b;
    }
    return nullptr;
}

std::string bookmarkNames(const Map& m) {
    std::string s;
    for (const auto& b : m.bookmarks) s += (s.empty() ? "" : ", ") + b.name;
    return s.empty() ? "none yet" : s;
}

// The area named by bbox=, center=+radius_km=, bookmark= or layer=; nullopt (no error) when none is given.
std::optional<BBox> areaFrom(const Map& m, const Params& p, std::string& error) {
    if (has(p, "bbox")) return bboxFrom(p, "bbox", error);
    if (has(p, "center")) {
        Coord c;
        if (!latLon(p, "center", c, error)) return std::nullopt;
        const double r = std::max(0.01, numberParam(p, "radius_km", 2)) * 1000;
        const Coord n = geodesicDirect(c.x, c.y, 0, r), s = geodesicDirect(c.x, c.y, 180, r), e = geodesicDirect(c.x, c.y, 90, r),
                    w = geodesicDirect(c.x, c.y, 270, r);
        return BBox{w.x, s.y, e.x, n.y};
    }
    if (has(p, "bookmark")) {
        const Bookmark* b = findBookmark(m, stringParam(p, "bookmark"));
        if (!b) {
            error = "no bookmark called '" + stringParam(p, "bookmark") + "' (bookmarks: " + bookmarkNames(m) + ")";
            return std::nullopt;
        }
        return b->box;
    }
    if (has(p, "layer")) {
        const Layer* l = m.layer(stringParam(p, "layer"));
        if (!l) {
            error = "no layer called '" + stringParam(p, "layer") + "'";
            return std::nullopt;
        }
        BBox b = l->bounds();
        if (!b.valid()) {
            error = "'" + l->name + "' is empty";
            return std::nullopt;
        }
        // a 6% margin of the longer side (a lone point gets about 200 m)
        const double size = std::max(b.width(), b.height()), pad = size > 1e-9 ? size * 0.06 : 0.002;
        return BBox{b.minX - pad, b.minY - pad, b.maxX + pad, b.maxY + pad};
    }
    return std::nullopt;
}

std::vector<ParamSpec> areaParams(const std::string& what) {
    return {{"bbox", ParamType::NumberList, what + " as min_lon,min_lat,max_lon,max_lat", false},
            {"center", ParamType::NumberList, what + ": lat,lon of the middle (with radius_km)", false},
            {"radius_km", ParamType::Number, "with center: how far around it", false, {}, 2.0}};
}

void registerBookmarks(Session& session) {
    auto& reg = session.registry();
    std::vector<ParamSpec> params = {docParam(), {"name", ParamType::String, "bookmark name", true}};
    for (auto& s : areaParams("the area")) params.push_back(s);
    params.push_back({"layer", ParamType::String, "the area of this layer", false});
    reg.add({"maps.bookmark", "Add Bookmark",
             "Remember a place by name: bbox=, center=+radius_km=, a layer's area, or (with none of them) what the map shows "
             "now. Go back with maps.view bookmark=NAME, or draw it with maps.render bookmark=NAME. Saved in the .wwmap.",
             params, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                auto area = areaFrom(doc.map, p, error);
                if (!error.empty()) return Result::failure(error);
                if (!area) area = shownArea(doc.map, previewOptions(doc.map));
                const std::string name = trim(stringParam(p, "name"));
                if (name.empty()) return Result::failure("name can't be empty");
                std::string verb = "added";
                bool found = false;
                for (auto& b : doc.map.bookmarks) {
                    if (lower(b.name) == lower(name)) {
                        b = {name, *area};
                        verb = "updated";
                        found = true;
                    }
                }
                if (!found) doc.map.bookmarks.push_back({name, *area});
                return edited(verb + " bookmark " + name + ": " + bboxText(*area), p).with("bbox", bboxText(*area)).with("bookmarks", static_cast<std::int64_t>(doc.map.bookmarks.size()));
            }));

    reg.add({"maps.bookmarks", "List Bookmarks", "List the map's bookmarks with their areas and sizes.", {docParam()}, false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string m = plural(doc.map.bookmarks.size(), "bookmark");
                for (const auto& b : doc.map.bookmarks) {
                    const double midY = (b.box.minY + b.box.maxY) / 2;
                    const double w = geodesicDistance(b.box.minX, midY, b.box.maxX, midY);
                    m += "\n" + b.name + ": " + bboxText(b.box) + " (about " + metres(w) + " across)";
                }
                return edited(m, p).with("count", static_cast<std::int64_t>(doc.map.bookmarks.size())).with("names", bookmarkNames(doc.map));
            }, false));

    reg.add({"maps.bookmark_delete", "Delete Bookmark", "Forget a bookmark.", {docParam(), {"name", ParamType::String, "bookmark name", true}}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                auto& bs = doc.map.bookmarks;
                const auto before = bs.size();
                bs.erase(std::remove_if(bs.begin(), bs.end(), [&](const Bookmark& b) { return lower(b.name) == lower(name); }), bs.end());
                if (bs.size() == before) return Result::failure("no bookmark called '" + name + "' (bookmarks: " + bookmarkNames(doc.map) + ")");
                return edited("deleted bookmark " + name, p);
            }));
}

void registerView(Session& session) {
    auto& reg = session.registry();
    std::vector<ParamSpec> params = {docParam(), {"bookmark", ParamType::String, "go to this bookmark", false}};
    for (auto& s : areaParams("show this area")) params.push_back(s);
    params.push_back({"layer", ParamType::String, "show this layer's area", false});
    params.push_back({"zoom", ParamType::Number, "zoom by this factor (2 = in, 0.5 = out)", false});
    params.push_back({"at", ParamType::NumberList, "with zoom: lat,lon to zoom around (default: the middle)", false});
    params.push_back({"pan", ParamType::NumberList, "move by east,north as fractions of the view (0.5,0 = half a view east)", false});
    params.push_back({"reset", ParamType::Bool, "show everything again", false, {}, false});
    params.push_back(previewParam());
    reg.add({"maps.view", "Set View",
             "Choose what the map window, maps.preview and maps.render show when no bbox= is given: a bookmark, an area "
             "(bbox=, center=+radius_km=, layer=), zoom= in or out (around at=lat,lon), pan= by a fraction of the view, or "
             "reset=true for everything. Saved in the .wwmap.",
             params, false},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                if (boolParam(p, "reset", false)) {
                    doc.map.view.reset();
                    return edited("showing the whole map", p);
                }
                auto area = areaFrom(doc.map, p, error);
                if (!error.empty()) return Result::failure(error);
                BBox v = area ? *area : shownArea(doc.map, previewOptions(doc.map));
                if (has(p, "zoom")) {
                    const double z = numberParam(p, "zoom");
                    if (!(z > 0)) return Result::failure("zoom must be more than 0 (2 zooms in, 0.5 out)");
                    Coord c{(v.minX + v.maxX) / 2, (v.minY + v.maxY) / 2};
                    if (has(p, "at") && !latLon(p, "at", c, error)) return Result::failure(error);
                    // keep the point under `at` where it is on screen
                    const double fx = v.width() > 0 ? (c.x - v.minX) / v.width() : 0.5, fy = v.height() > 0 ? (c.y - v.minY) / v.height() : 0.5;
                    const double w = v.width() / z, h = v.height() / z;
                    v = BBox{c.x - w * fx, c.y - h * fy, c.x + w * (1 - fx), c.y + h * (1 - fy)};
                }
                if (has(p, "pan")) {
                    const NumberList d = listParam(p, "pan");
                    if (d.size() != 2) return Result::failure("pan needs east,north fractions, e.g. 0.5,0");
                    const double dx = d[0] * v.width(), dy = d[1] * v.height();
                    v = BBox{v.minX + dx, v.minY + dy, v.maxX + dx, v.maxY + dy};
                }
                if (!area && !has(p, "zoom") && !has(p, "pan")) return Result::failure("give bookmark=, bbox=, center=, layer=, zoom=, pan= or reset=true");
                v.minY = std::max(v.minY, -89.9);
                v.maxY = std::min(v.maxY, 89.9);
                if (!(v.width() > 1e-7 && v.height() > 1e-7) || v.width() > 720) return Result::failure("that view is too small or too big");
                doc.map.view = v;
                const double midY = (v.minY + v.maxY) / 2;
                const double across = geodesicDistance(v.minX, midY, v.maxX, midY);
                return edited("showing " + bboxText(v) + " (about " + metres(across) + " across)", p).with("bbox", bboxText(v)).with("width_m", across);
            }, false));
}

void registerHit(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.hit", "What's Here",
             "What is at a point of the map window's picture (the PNG maps.preview and the window show; x,y in its pixels from "
             "the top left): the lat,lon there and the topmost feature within tolerance= pixels (layer, index, name, and the "
             "vertex number when the point is on one). Changes nothing: the window's click tools use it and then run the "
             "ordinary commands.",
             {docParam(), {"x", ParamType::Number, "pixels from the left", true}, {"y", ParamType::Number, "pixels from the top", true},
              {"tolerance", ParamType::Number, "how close counts, in pixels", false, {}, 8.0},
              {"width", ParamType::Int, "the picture's width in pixels (the window uses 800)", false, {}, std::int64_t{800}},
              {"layer", ParamType::String, "only look at this layer", false}},
             false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                RenderOptions o = previewOptions(doc.map);
                o.width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 800), 16, 8000));
                const double x = numberParam(p, "x"), y = numberParam(p, "y");
                const Coord at = pixelToLonLat(doc.map, o, x, y);
                const double mpp = renderMetresPerPixel(doc.map, o);
                const double tol = std::max(0.5, numberParam(p, "tolerance", 8)) * mpp;
                Result r = edited("", p);
                r.with("lat", at.y).with("lon", at.x).with("metres_per_pixel", mpp);
                const Layer* only = has(p, "layer") ? doc.map.layer(stringParam(p, "layer")) : nullptr;
                if (has(p, "layer") && !only) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                // the topmost visible vector layer with something close enough; within it the closest, lines and points before areas
                for (auto it = doc.map.layers.rbegin(); it != doc.map.layers.rend(); ++it) {
                    const Layer& l = *it;
                    if (l.isRaster() || (!only && !l.visible) || (only && &l != only)) continue;
                    double best = std::numeric_limits<double>::infinity();
                    std::size_t bestI = 0;
                    bool bestArea = true;
                    const double padDeg = tol / 111320.0 / std::max(0.05, std::cos(at.y * kDeg));
                    for (std::size_t i = 0; i < l.features.size(); ++i) {
                        const Geometry& g = l.features[i].geom;
                        BBox b = bboxOf(g);
                        if (!b.valid() || at.x < b.minX - padDeg || at.x > b.maxX + padDeg || at.y < b.minY - padDeg * 2 || at.y > b.maxY + padDeg * 2) continue;
                        double d = nearestPoint(g, at.x, at.y).distance;
                        if (g.type == GeomType::Polygon) {  // an area's outline counts as close, its inside as a weaker hit
                            const EdgeHit e = nearestEdge(g, at.x, at.y);
                            if (e.found && e.distance <= tol) d = e.distance;
                        }
                        const bool isArea = g.type == GeomType::Polygon && d == 0;
                        if (d > tol) continue;
                        if ((bestArea && !isArea) || (bestArea == isArea && d < best)) {
                            best = d;
                            bestI = i;
                            bestArea = isArea;
                        }
                    }
                    if (!std::isfinite(best)) continue;
                    const Feature& f = l.features[bestI];
                    r.with("layer", l.name).with("index", static_cast<std::int64_t>(bestI)).with("type", std::string(toString(f.geom.type)));
                    r.with("distance_m", best);
                    // a vertex under the point?
                    const auto vs = vertices(f.geom);
                    double vBest = tol;
                    std::int64_t vIdx = -1;
                    for (std::size_t k = 0; k < vs.size(); ++k) {
                        const double d = geodesicDistance(at.x, at.y, vs[k].x, vs[k].y);
                        if (d <= vBest) {
                            vBest = d;
                            vIdx = static_cast<std::int64_t>(k);
                        }
                    }
                    const std::string name = featureName(f);
                    r.with("kind", std::string(vIdx >= 0 && f.geom.type != GeomType::Point ? "vertex" : "feature")).with("vertex", vIdx).with("name", name);
                    r.message = l.name + " #" + std::to_string(bestI) + (name.empty() ? "" : " " + name) + ": " + geometryLine(f.geom);
                    if (vIdx >= 0 && f.geom.type != GeomType::Point) r.message += ", vertex " + std::to_string(vIdx);
                    const std::string props = propsLine(f.props, {}, 300);
                    if (!props.empty()) r.message += " {" + props + "}";
                    r.message += " at " + latLonText(at) + " (lat,lon)";
                    return r;
                }
                r.with("kind", std::string("none")).with("index", std::int64_t{-1}).with("vertex", std::int64_t{-1});
                r.message = latLonText(at) + " (lat,lon), nothing here";
                return r;
            }, false));
}

}  // namespace

void registerPlaceCommands(Session& session) {
    registerBookmarks(session);
    registerView(session);
    registerHit(session);
}

}  // namespace wetwaresuite::maps::cmd
