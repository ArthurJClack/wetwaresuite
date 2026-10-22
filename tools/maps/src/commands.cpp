// Every maps.* command. Edits run inside a MapEdit, so each one is a single
// undo step and a failed edit leaves the map as it was.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#include "wetwaresuite/raster/codec.hpp"
#include "cmd.hpp"

namespace wetwaresuite::maps {

namespace cmd {

namespace fs = std::filesystem;
using namespace detail;

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string plural(std::size_t n, const std::string& word) {
    if (n == 1) return "1 " + word;
    if (word.size() > 1 && word.back() == 'y' && std::string("aeiou").find(word[word.size() - 2]) == std::string::npos)
        return std::to_string(n) + " " + word.substr(0, word.size() - 1) + "ies";
    const auto ends = [&](const char* e) { const std::string s(e); return word.size() >= s.size() && word.compare(word.size() - s.size(), s.size(), s) == 0; };
    if (ends("ch") || ends("sh") || ends("s") || ends("x")) return std::to_string(n) + " " + word + "es";
    return std::to_string(n) + " " + word + "s";
}

std::string metres(double m) {
    if (!std::isfinite(m)) return "?";
    if (std::abs(m) >= 1000) return fmt(m / 1000, std::abs(m) >= 100000 ? 1 : 3) + " km";
    return fmt(m, 2) + " m";
}

std::string squareMetres(double a) {
    if (a >= 1e6) return fmt(a / 1e6, a >= 1e8 ? 1 : 4) + " km\xC2\xB2";
    if (a >= 1e4) return fmt(a / 1e4, 3) + " ha";
    return fmt(a, 1) + " m\xC2\xB2";
}

std::string duration(double s) {
    const auto t = static_cast<long long>(std::llround(s));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", t / 3600, t / 60 % 60, t % 60);
    return buf;
}

std::vector<std::string> splitList(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + sep) {
        if (c == sep) {
            const std::string t = trim(item);
            if (!t.empty()) out.push_back(t);
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

// "name=Cafe; rating=4.5" or "name=Cafe, rating=4.5"
Props parseProps(const std::string& text) {
    Props out;
    const char sep = text.find(';') != std::string::npos ? ';' : ',';
    for (const auto& item : splitList(text, sep)) {
        const auto eq = item.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        out[trim(item.substr(0, eq))] = Prop::infer(trim(item.substr(eq + 1)));
    }
    return out;
}

// ---- shared params ------------------------------------------------------------------------

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open map, e.g. map1", true}; }
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG render of the whole map to the result", false, {}, false}; }
ParamSpec layerParam(const std::string& what) {
    return {"layer", ParamType::String, what, false};
}
ParamSpec outputParam(const std::string& what) { return {"output", ParamType::String, what, false}; }
ParamSpec whereParam(const std::string& what) {
    return {"where", ParamType::String, what, false};
}
ParamSpec crsParam(const std::string& what) { return {"crs", ParamType::String, what, false}; }

std::string crsHelp() { return "wgs84 (EPSG:4326), webmercator (EPSG:3857), utm33n / utm33s, EPSG:326xx/327xx"; }

std::optional<Crs> crsFrom(const Params& p, const std::string& key, std::string& error) {
    if (!has(p, key)) return std::nullopt;
    const std::string text = stringParam(p, key);
    auto c = Crs::parse(text);
    if (!c) error = "unknown coordinate system '" + text + "' (use " + crsHelp() + ")";
    return c;
}

std::optional<BBox> bboxFrom(const Params& p, const std::string& key, std::string& error) {
    if (!has(p, key)) return std::nullopt;
    const NumberList v = listParam(p, key);
    if (v.size() != 4) {
        error = key + " needs four numbers: min_lon,min_lat,max_lon,max_lat";
        return std::nullopt;
    }
    BBox b{std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]), std::max(v[1], v[3])};
    if (b.width() <= 0 || b.height() <= 0) {
        error = key + " is empty (min and max are the same)";
        return std::nullopt;
    }
    return b;
}

std::string bboxText(const BBox& b) {
    if (!b.valid()) return "";
    return fmt(b.minX, 6) + "," + fmt(b.minY, 6) + "," + fmt(b.maxX, 6) + "," + fmt(b.maxY, 6);
}

// ---- lookups ----------------------------------------------------------------------------

// The vector layer a command works on: "layer", else the top vector layer.
Layer* vectorLayer(Map& m, const Params& p, std::string& error, const std::string& key) {
    if (has(p, key)) {
        Layer* l = m.layer(stringParam(p, key));
        if (!l) error = "no layer called '" + stringParam(p, key) + "' (maps.info lists them)";
        else if (l->isRaster()) error = "'" + l->name + "' is a raster layer";
        else return l;
        return nullptr;
    }
    for (auto it = m.layers.rbegin(); it != m.layers.rend(); ++it) {
        if (!it->isRaster()) return &*it;
    }
    error = "the map has no vector layers yet (maps.import or maps.feature_add)";
    return nullptr;
}

// Without layer=, the top vector layer holding any feature of this type (falls back to the top vector layer).
Layer* layerWith(Map& m, const Params& p, std::string& error, GeomType type) {
    if (!has(p, "layer")) {
        for (auto it = m.layers.rbegin(); it != m.layers.rend(); ++it) {
            if (it->isRaster()) continue;
            for (const auto& f : it->features) {
                if (f.geom.type == type) return &*it;
            }
        }
    }
    return vectorLayer(m, p, error);
}

Layer* anyLayer(Map& m, const Params& p, std::string& error) {
    if (!has(p, "layer")) {
        if (m.layers.empty()) error = "the map has no layers";
        return m.layers.empty() ? nullptr : &m.layers.back();
    }
    Layer* l = m.layer(stringParam(p, "layer"));
    if (!l) error = "no layer called '" + stringParam(p, "layer") + "' (maps.info lists them)";
    return l;
}

std::optional<Expr> whereFrom(const Params& p, std::string& error, const std::string& key) {
    if (!has(p, key) || trim(stringParam(p, key)).empty()) return std::nullopt;
    try {
        return Expr::compile(stringParam(p, key));
    } catch (const std::exception& e) {
        error = key + ": " + e.what();
        return std::nullopt;
    }
}

// A note when an expression names fields no feature has (usually a missing quote).
std::string unknownFieldsNote(const Layer& l, const Expr& e) {
    const auto fields = l.fields();
    std::string missing;
    for (const auto& f : e.fields()) {
        bool found = false;
        for (const auto& k : fields) found = found || lower(k) == lower(f);
        if (!found) missing += (missing.empty() ? "" : ", ") + f;
    }
    if (missing.empty()) return {};
    return "\nnote: no feature has a field called " + missing + " (put text values in quotes: name == 'Spain')";
}

// ---- results ------------------------------------------------------------------------------

Result& maybePreview(Result& r, const MapDoc& doc, const Params& p) {
    if (r.ok && r.attachments.empty() && boolParam(p, "preview", false)) r.attach(*doc.preview());
    return r;
}

Result edited(const std::string& message, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"));
    return r;
}

ActionFactory mapCommand(Session& session, Edit edit, bool undoable) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<MapDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open map called '" + id + "' (use maps.new or maps.open first)"); });
        }
        auto withPreview = [edit](MapDoc& d, const Params& q) {
            Result r = edit(d, q);
            return maybePreview(r, d, q);
        };
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, withPreview, p] {
                try {
                    return withPreview(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<MapEdit>(doc, [withPreview, p](MapDoc& d) { return withPreview(d, p); });
    };
}

ActionFactory plainCommand(std::function<Result(const Params&)> fn) {
    return [fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([fn, p] {
            try {
                return fn(p);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        });
    };
}

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

std::string layerLine(const Layer& l, std::size_t i) {
    std::string s = std::to_string(i) + ": " + l.name + " (" + l.geometrySummary();
    if (!l.visible) s += ", hidden";
    if (l.style.mode != "single" && !l.style.field.empty()) s += ", " + l.style.mode + " by " + l.style.field;
    if (!l.style.labelField.empty()) s += ", labels " + l.style.labelField;
    s += ")";
    if (!l.isRaster()) {
        const auto f = l.fields();
        if (!f.empty()) {
            s += "\n   fields: ";
            for (std::size_t k = 0; k < f.size() && k < 30; ++k) s += (k ? ", " : "") + f[k];
            if (f.size() > 30) s += ", ... (" + std::to_string(f.size()) + " in all)";
        }
    }
    return s;
}

std::string geometryLine(const Geometry& g) {
    switch (g.type) {
        case GeomType::Point:
            if (g.points.size() == 1) return "point " + fmt(g.points[0].y, 6) + "," + fmt(g.points[0].x, 6) + " (lat,lon)";
            return plural(g.points.size(), "point");
        case GeomType::Line: return (g.lines.size() > 1 ? plural(g.lines.size(), "line") : "line") + std::string(", ") + metres(length(g));
        case GeomType::Polygon: return (g.polygons.size() > 1 ? plural(g.polygons.size(), "polygon") : "polygon") + std::string(", ") + squareMetres(area(g));
        default: return "no geometry";
    }
}

std::string propsLine(const Props& props, const std::vector<std::string>& only, std::size_t maxLen) {
    std::string s;
    for (const auto& [k, v] : props) {
        if (!only.empty() && std::find(only.begin(), only.end(), k) == only.end()) continue;
        s += (s.empty() ? "" : "; ") + k + "=" + v.text();
        if (s.size() > maxLen) return s.substr(0, maxLen) + "...";
    }
    return s;
}

Result docCreated(Session& session, std::shared_ptr<MapDoc> doc, const std::string& verb, const Params& p, const std::vector<std::string>& warnings) {
    const std::string id = session.workspace().add(doc, "map");
    Result r = Result::success(verb + " " + id + ": " + doc->summary(), id);
    r.with("doc", id).with("layers", static_cast<std::int64_t>(doc->map.layers.size()));
    for (std::size_t i = 0; i < doc->map.layers.size(); ++i) r.message += "\n" + layerLine(doc->map.layers[i], i);
    for (const auto& w : warnings) r.message += "\nwarning: " + w;
    if (auto b = doc->map.bounds(); b.valid()) r.with("bbox", bboxText(b));
    return maybePreview(r, *doc, p);
}

// Adds a new layer named after `name` (made unique) and returns it.
Layer& addLayer(Map& m, Layer l, const std::string& name) {
    l.name = m.uniqueName(name.empty() ? l.name : name);
    m.layers.push_back(std::move(l));
    return m.layers.back();
}

// ---- render options ------------------------------------------------------------------------

std::vector<ParamSpec> viewParams() {
    return {{"width", ParamType::Int, "image width in px", false, {}, std::int64_t{1024}},
            {"height", ParamType::Int, "image height in px (default: from the extent's shape)", false},
            {"crs", ParamType::String, "map projection: webmercator (default), wgs84 (plate carree), utm33n..., or auto (the UTM zone of the centre)", false, {},
             std::string("webmercator")},
            {"bbox", ParamType::NumberList, "area to show as min_lon,min_lat,max_lon,max_lat (default: all visible layers plus a margin)", false},
            {"center", ParamType::NumberList, "lat,lon to centre on (with radius_km)", false},
            {"radius_km", ParamType::Number, "with center: how far to show around it", false},
            {"bookmark", ParamType::String, "show a bookmarked area (maps.bookmarks lists them)", false},
            {"basemap", ParamType::String,
             "background: auto (an installed map pack, else countries), countries (built-in Natural Earth outlines), none, pack (the most "
             "detailed installed map pack), a .pmtiles file on disk (vector or raster map tiles, read in place, never downloaded), or osm "
             "(online tiles, cached; the only choice that can use the internet)",
             false, {}, std::string("auto")},
            {"tile_url", ParamType::String, "osm: tile server template with {z}/{x}/{y}, a local tile folder template (e.g. tiles/{z}/{x}/{y}.png) or a .pmtiles file; local ones are never downloaded to", false, {}, std::string("https://tile.openstreetmap.org/{z}/{x}/{y}.png")},
            {"tile_cache", ParamType::String, "osm: tile cache folder (default: the user cache dir, wetwaresuite/tiles)", false},
            {"offline", ParamType::Bool, "osm: use cached tiles only, never download", false, {}, false},
            {"max_tiles", ParamType::Int, "osm: most tiles one render may use (zoom drops to fit)", false, {}, std::int64_t{64}},
            {"scale_bar", ParamType::Bool, "draw a scale bar", false, {}, true},
            {"north_arrow", ParamType::Bool, "draw a north arrow", false, {}, true},
            {"legend", ParamType::Bool, "draw a legend of the visible layers", false, {}, true},
            {"graticule", ParamType::Bool, "draw latitude/longitude lines with labels", false, {}, false},
            {"graticule_step", ParamType::Number, "graticule spacing in degrees (default: automatic)", false},
            {"labels", ParamType::Bool, "draw feature labels set with maps.style label=", false, {}, true},
            {"title", ParamType::String, "title across the top (default: the map's title)", false},
            {"background", ParamType::String, "sea/background colour", false, {}, std::string("#dde8f0")},
            {"land", ParamType::String, "basemap=countries: land colour", false, {}, std::string("#f4f1ea")}};
}

bool viewOptions(const MapDoc& doc, const Params& p, RenderOptions& o, std::string& error) {
    o.width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 1024), 16, 8000));
    o.height = has(p, "height") ? static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height"), 16, 8000)) : 0;
    o.bbox = bboxFrom(p, "bbox", error);
    if (!error.empty()) return false;
    if (has(p, "bookmark")) {
        const std::string name = lower(stringParam(p, "bookmark"));
        for (const auto& b : doc.map.bookmarks) {
            if (lower(b.name) == name) o.bbox = b.box;
        }
        if (!o.bbox) {
            error = "no bookmark called '" + stringParam(p, "bookmark") + "' (maps.bookmarks lists them)";
            return false;
        }
    }
    if (has(p, "center")) {
        const NumberList c = listParam(p, "center");
        if (c.size() != 2) {
            error = "center needs lat,lon";
            return false;
        }
        const double r = numberParam(p, "radius_km", 10) * 1000;
        const Coord n = geodesicDirect(c[1], c[0], 0, r), s = geodesicDirect(c[1], c[0], 180, r), e = geodesicDirect(c[1], c[0], 90, r),
                    w = geodesicDirect(c[1], c[0], 270, r);
        o.bbox = BBox{w.x, s.y, e.x, n.y};
    }
    const std::string crs = stringParam(p, "crs", "webmercator");
    if (lower(crs) == "auto") {
        const BBox b = o.bbox ? *o.bbox : doc.map.bounds(true);
        o.crs = b.valid() ? Crs::utmFor((b.minX + b.maxX) / 2, (b.minY + b.maxY) / 2) : Crs::webMercator();
    } else {
        auto c = Crs::parse(crs);
        if (!c) {
            error = "unknown crs '" + crs + "' (use " + crsHelp() + " or auto)";
            return false;
        }
        o.crs = *c;
    }
    o.basemap = stringParam(p, "basemap", "auto");
    o.tileUrl = stringParam(p, "tile_url", o.tileUrl);
    const std::string urlExt = lower(fs::path(o.tileUrl).extension().string());
    const bool tileFile = urlExt == ".pmtiles" || urlExt == ".mbtiles";
    if (!tileFile && (o.tileUrl.find("{z}") == std::string::npos || o.tileUrl.find("{x}") == std::string::npos ||
                      o.tileUrl.find("{y}") == std::string::npos)) {
        error = "tile_url needs {z}, {x} and {y}: an http(s) tile server, or a local tile folder like D:/tiles/{z}/{x}/{y}.png (or give a .pmtiles file)";
        return false;
    }
    o.tileCache = stringParam(p, "tile_cache");
    o.offline = boolParam(p, "offline", false);
    o.maxTiles = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "max_tiles", 64), 1, 400));
    o.scaleBar = boolParam(p, "scale_bar", true);
    o.northArrow = boolParam(p, "north_arrow", true);
    o.legend = boolParam(p, "legend", true);
    o.graticule = boolParam(p, "graticule", false);
    o.graticuleStep = numberParam(p, "graticule_step", 0);
    o.labels = boolParam(p, "labels", true);
    o.title = has(p, "title") ? stringParam(p, "title") : doc.map.title;
    for (const char* key : {"background", "land"}) {
        if (!has(p, key)) continue;
        auto c = Color::parse(stringParam(p, key));
        if (!c) {
            error = std::string(key) + " isn't a colour: " + stringParam(p, key);
            return false;
        }
        (std::string(key) == "background" ? o.background : o.land) = *c;
    }
    return true;
}

Result renderResult(const std::string& message, const RenderInfo& info, const RenderOptions& o, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"))
        .with("width", static_cast<std::int64_t>(info.width))
        .with("height", static_cast<std::int64_t>(info.height))
        .with("bbox", bboxText(info.bbox))
        .with("crs", o.crs.id())
        .with("metres_per_pixel", info.metresPerPixel);
    if (info.zoom >= 0) {
        r.with("zoom", static_cast<std::int64_t>(info.zoom))
            .with("tiles_used", static_cast<std::int64_t>(info.tilesUsed))
            .with("tiles_fetched", static_cast<std::int64_t>(info.tilesFetched));
    }
    if (!info.tileSource.empty()) {
        r.with("tile_source", info.tileSource);
        r.message += "\nmap pack: " + info.tileSource + ", zoom " + std::to_string(info.zoom) + ", " + std::to_string(info.tilesUsed) + " tiles" +
                     (info.attribution.empty() ? std::string() : " (" + info.attribution + ")");
    } else if (info.zoom >= 0) {
        r.message += "\nOSM tiles: zoom " + std::to_string(info.zoom) + ", " + std::to_string(info.tilesUsed) + " used, " +
                     std::to_string(info.tilesFetched) + " downloaded (map data \xC2\xA9 OpenStreetMap contributors, ODbL)";
    }
    for (const auto& w : info.warnings) r.message += "\nwarning: " + w;
    return r;
}

Feature* featureAt(Layer& l, const Params& p, std::string& error, const std::string& key) {
    if (!has(p, key)) {
        error = "give " + key + "= (maps.features lists the indexes)";
        return nullptr;
    }
    const std::int64_t i = intParam(p, key);
    if (i < 0 || i >= static_cast<std::int64_t>(l.features.size())) {
        error = l.features.empty() ? l.name + " has no features" : l.name + " has features 0.." + std::to_string(static_cast<std::int64_t>(l.features.size()) - 1);
        return nullptr;
    }
    return &l.features[static_cast<std::size_t>(i)];
}

bool latLon(const Params& p, const std::string& key, Coord& out, std::string& error) {
    if (!has(p, key)) {
        error = "give " + key + "=lat,lon";
        return false;
    }
    const NumberList v = listParam(p, key);
    if (v.size() != 2 || !std::isfinite(v[0]) || !std::isfinite(v[1]) || std::abs(v[0]) > 90 || std::abs(v[1]) > 540) {
        error = key + " needs lat,lon in degrees";
        return false;
    }
    out = Coord{v[1], v[0]};
    return true;
}

std::string featureName(const Feature& f) {
    for (const char* k : {"name", "NAME", "Name", "title", "label", "ref", "id"}) {
        if (auto it = f.props.find(k); it != f.props.end() && !it->second.isNull() && !it->second.text().empty()) return it->second.text();
    }
    return {};
}

}  // namespace cmd

namespace {

namespace fs = std::filesystem;
using namespace detail;
using namespace cmd;

// ---- command groups -------------------------------------------------------------------

void registerFileCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.new", "New Map",
             "Create an empty map document and return its id (e.g. map1). Add data with maps.import (GeoJSON, KML, GPX, CSV, "
             "shapefile), maps.add_countries, maps.feature_add or maps.raster_add, then style it and maps.render it.",
             {{"title", ParamType::String, "map title (shown on renders)", false}, previewParam()}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto doc = std::make_shared<MapDoc>();
                    doc->map.title = stringParam(p, "title");
                    return docCreated(session, doc, "created", p);
                });
            });

    reg.add({"maps.open", "Open Map",
             "Open a map: a .wwmap project (layers with styles, saved by maps.save; older .achmap files too), or any data file as a new map with "
             "it as its layers: .geojson/.json, .kml, .gpx (tracks, routes and waypoints become separate layers), .fit (Garmin "
             "and other sports watches), .osm (OpenStreetMap XML: roads, areas and places layers, ready for maps.route), .csv "
             "with lat/lon columns, .shp (with .dbf/.prj), .wkt, a GeoTIFF or an image with a world file.",
             {{"path", ParamType::String, "file to open", true},
              crsParam("coordinates' system when the file doesn't say (CSV x/y, shapefile without .prj, world files): " + crsHelp()),
              previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        const fs::path path = stringParam(p, "path");
                        std::string error;
                        auto crs = crsFrom(p, "crs", error);
                        if (!error.empty()) return Result::failure(error);
                        auto doc = std::make_shared<MapDoc>();
                        std::vector<std::string> warnings;
                        const std::string ext = lower(path.extension().string());
                        if (ext == ".wwmap" || ext == ".achmap") {  // .achmap: saved before the rename
                            doc->map = fromProject(readFile(path), path.parent_path());
                            doc->path = path.string();
                        } else if (vectorFormatFor(path).empty() || ext == ".tif" || ext == ".tiff") {
                            if (!raster::canRead(path) && ext != ".tif" && ext != ".tiff")
                                return Result::failure("can't open " + path.string() + ": not a map project, vector file or raster");
                            Layer l;
                            l.name = path.stem().string();
                            l.grid = readRaster(path, crs, std::nullopt, &warnings);
                            l.source = path.string();
                            doc->map.layers.push_back(std::move(l));
                        } else {
                            ImportOptions o;
                            o.crs = crs;
                            for (auto& l : importVector(path, "auto", o, &warnings)) doc->map.layers.push_back(std::move(l));
                        }
                        if (doc->map.title.empty()) doc->map.title = "";
                        return docCreated(session, doc, "opened " + path.string() + " as", p, warnings);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"maps.save", "Save Map",
             "Save the whole map as a .wwmap project (JSON: every layer with its features, style and rasters), to reopen "
             "with maps.open. To write one layer in a GIS format use maps.export; for a picture use maps.render.",
             {docParam(), {"path", ParamType::String, "output .wwmap file", true}}, false},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                fs::path path = stringParam(p, "path");
                if (path.extension().empty()) path += ".wwmap";
                writeFile(path, toProject(doc.map));
                doc.path = path.string();
                return edited("saved " + path.string(), p).with("path", path.string());
            }, false));

    reg.add({"maps.close", "Close Map", "Close an open map (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<CloseAction>(session, stringParam(p, "doc")); });

    reg.add({"maps.info", "Map Info",
             "Describe a map: its layers bottom to top with geometry counts, fields and styles, rasters with their size, value "
             "range and CRS, and the overall extent.",
             {docParam(), previewParam()}, false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                const Map& m = doc.map;
                Result r = edited(doc.summary(), p);
                for (std::size_t i = 0; i < m.layers.size(); ++i) r.message += "\n" + layerLine(m.layers[i], i);
                const BBox b = m.bounds();
                if (b.valid()) r.message += "\nextent (lon/lat): " + bboxText(b);
                std::size_t features = 0;
                for (const auto& l : m.layers) features += l.features.size();
                r.with("layers", static_cast<std::int64_t>(m.layers.size())).with("features", static_cast<std::int64_t>(features)).with("bbox", bboxText(b));
                std::string names;
                for (const auto& l : m.layers) names += (names.empty() ? "" : ",") + l.name;
                r.with("layer_names", names);
                return r;
            }, false));

    reg.add({"maps.import", "Import Layer",
             "Add data from a file as new layer(s): GeoJSON (.geojson/.json, any 'crs' member honoured), KML (placemarks, "
             "folders, ExtendedData, gx:Track), GPX (tracks with times and elevation, routes, waypoints as separate layers), "
             "FIT activities (positions, elevation, times; sport and heart rate as properties), OSM XML (roads, areas, places), "
             "CSV with lat/lon columns (auto-detected: lat/latitude/y and lon/lng/long/longitude/x; or a WKT column; "
             "degrees-minutes-seconds and N/S/E/W suffixes read too), shapefile .shp with .shx/.dbf/.prj (UTM and Web Mercator "
             ".prj are reprojected), or a .wkt file with one geometry per line.",
             {docParam(),
              {"path", ParamType::String, "file to import", true},
              {"format", ParamType::String, "override the extension", false, {"auto", "geojson", "kml", "gpx", "fit", "osm", "csv", "shp", "wkt"}, std::string("auto")},
              {"name", ParamType::String, "layer name (default: the file name)", false},
              crsParam("source coordinates' system when the file doesn't say: " + crsHelp()),
              {"lat", ParamType::String, "CSV: latitude (or y/northing) column", false},
              {"lon", ParamType::String, "CSV: longitude (or x/easting) column", false},
              {"wkt_column", ParamType::String, "CSV: column holding WKT geometry", false},
              {"delimiter", ParamType::String, "CSV: comma, semicolon, tab or pipe (default: detected)", false},
              {"what", ParamType::String, "GPX: which parts to import", false, {"all", "tracks", "routes", "waypoints"}, std::string("all")},
              previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                ImportOptions o;
                o.crs = crsFrom(p, "crs", error);
                if (!error.empty()) return Result::failure(error);
                o.latColumn = stringParam(p, "lat");
                o.lonColumn = stringParam(p, "lon");
                o.wktColumn = stringParam(p, "wkt_column");
                o.delimiter = stringParam(p, "delimiter");
                o.what = stringParam(p, "what", "all");
                std::vector<std::string> warnings;
                auto layers = importVector(stringParam(p, "path"), stringParam(p, "format", "auto"), o, &warnings);
                std::string names;
                std::size_t total = 0;
                for (auto& l : layers) {
                    std::string name = l.name;
                    if (has(p, "name")) name = layers.size() == 1 ? stringParam(p, "name") : stringParam(p, "name") + " " + l.name.substr(l.name.rfind(' ') + 1);
                    total += l.features.size();
                    Layer& added = addLayer(doc.map, std::move(l), name);
                    names += (names.empty() ? "" : ", ") + added.name + " (" + added.geometrySummary() + ")";
                }
                Result r = edited("imported " + plural(total, "feature") + " into " + names, p);
                for (const auto& w : warnings) r.message += "\nwarning: " + w;
                const Layer& last = doc.map.layers.back();
                const auto fields = last.fields();
                if (!fields.empty()) {
                    r.message += "\nfields: ";
                    for (std::size_t i = 0; i < fields.size() && i < 30; ++i) r.message += (i ? ", " : "") + fields[i];
                }
                return r.with("layer", last.name).with("features", static_cast<std::int64_t>(total)).with("bbox", bboxText(last.bounds()));
            }));

    reg.add({"maps.export", "Export Layer",
             "Write one vector layer to a file; the extension picks the format: .geojson (RFC 7946, with per-vertex times of "
             "tracks as coordTimes), .kml (with the layer's colours), .gpx (points as waypoints, lines as tracks), .csv (lat/lon "
             "columns for points, a WKT column otherwise), .shp (writes .shp/.shx/.dbf/.prj/.cpg; mixed layers write one set "
             "per geometry type) or .wkt. crs= reprojects GeoJSON, CSV, shapefile and WKT output.",
             {docParam(),
              layerParam(),
              {"path", ParamType::String, "output file", true},
              {"format", ParamType::String, "override the extension", false, {"auto", "geojson", "kml", "gpx", "csv", "shp", "wkt"}, std::string("auto")},
              crsParam("output coordinate system (default wgs84): " + crsHelp()),
              {"precision", ParamType::Int, "decimals for degrees", false, {}, std::int64_t{7}},
              whereParam("only export features matching this expression")},
             false},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                ExportOptions o;
                if (auto c = crsFrom(p, "crs", error)) o.crs = *c;
                if (!error.empty()) return Result::failure(error);
                o.precision = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "precision", 7), 0, 12));
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                Layer out = *l;
                if (where) {
                    out.features.clear();
                    for (std::size_t i = 0; i < l->features.size(); ++i) {
                        if (where->test(l->features[i], i)) out.features.push_back(l->features[i]);
                    }
                }
                const auto files = exportVector(out, stringParam(p, "path"), stringParam(p, "format", "auto"), o);
                std::string list;
                for (const auto& f : files) list += (list.empty() ? "" : ", ") + f;
                return edited("wrote " + plural(out.features.size(), "feature") + " of " + l->name + " to " + list, p)
                    .with("files", list)
                    .with("features", static_cast<std::int64_t>(out.features.size()));
            }, false));

    reg.add({"maps.add_countries", "Add Countries",
             "Add the built-in Natural Earth 1:110m countries (public domain, 177 countries) as a vector layer with fields "
             "name, iso_a3, continent and pop_est: handy for joins (maps.join a CSV by iso_a3), choropleths and clipping "
             "context. It works offline.",
             {docParam(), {"name", ParamType::String, "layer name", false, {}, std::string("countries")}, whereParam("only countries matching, e.g. \"continent == 'Africa'\""),
              previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                Layer l;
                l.source = "Natural Earth 1:110m countries (public domain)";
                l.style.fill = {0xe0, 0xd8, 0xc8, 255};
                l.style.stroke = {0x88, 0x80, 0x70, 255};
                l.style.width = 0.8;
                l.style.fillOpacity = 0.9;
                const auto& all = countries();
                for (std::size_t i = 0; i < all.size(); ++i) {
                    if (!where || where->test(all[i], i)) l.features.push_back(all[i]);
                }
                if (l.features.empty()) return Result::failure("no country matches " + stringParam(p, "where"));
                Layer& added = addLayer(doc.map, std::move(l), stringParam(p, "name", "countries"));
                return edited("added " + plural(added.features.size(), "country") + " as " + added.name, p)
                    .with("layer", added.name)
                    .with("features", static_cast<std::int64_t>(added.features.size()));
            }));
}

void registerRasterCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.raster_add", "Add Raster",
             "Add a georeferenced raster layer: a GeoTIFF (strips or tiles; none/LZW/Deflate/PackBits; 8/16/32-bit or float; "
             "EPSG 4326, 3857 and UTM geokeys; single-band rasters such as elevation keep their values), or a PNG/JPG/GIF/BMP "
             "placed by its world file (.pgw/.jgw/.wld + optional .prj) or by bbox=.",
             {docParam(),
              {"path", ParamType::String, "GeoTIFF or image file", true},
              {"name", ParamType::String, "layer name (default: the file name)", false},
              crsParam("the raster's coordinate system when the file doesn't say: " + crsHelp()),
              {"bbox", ParamType::NumberList, "place an image without a world file: min_lon,min_lat,max_lon,max_lat", false},
              {"ramp", ParamType::String, "value rasters: colour ramp (viridis, terrain, heat, gray, blues, reds, greens, spectral or #rrggbb,#rrggbb)", false,
               {}, std::string("terrain")},
              {"hillshade", ParamType::Bool, "value rasters: shade the relief", false, {}, false},
              {"opacity", ParamType::Number, "0..1", false, {}, 1.0},
              previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                auto crs = crsFrom(p, "crs", error);
                auto bbox = bboxFrom(p, "bbox", error);
                if (!error.empty()) return Result::failure(error);
                std::vector<std::string> warnings;
                const fs::path path = stringParam(p, "path");
                Layer l;
                l.grid = readRaster(path, crs, bbox, &warnings);
                l.source = path.string();
                l.style.rasterRamp = stringParam(p, "ramp", "terrain");
                l.style.hillshade = boolParam(p, "hillshade", false);
                l.style.opacity = std::clamp(numberParam(p, "opacity", 1.0), 0.0, 1.0);
                Layer& added = addLayer(doc.map, std::move(l), has(p, "name") ? stringParam(p, "name") : path.stem().string());
                Result r = edited("added raster " + added.name + ": " + added.geometrySummary() + ", extent " + bboxText(added.bounds()), p);
                for (const auto& w : warnings) r.message += "\nwarning: " + w;
                const Grid& g = *added.grid;
                r.with("layer", added.name).with("width", static_cast<std::int64_t>(g.width)).with("height", static_cast<std::int64_t>(g.height)).with("crs", g.crs.id()).with("bbox", bboxText(added.bounds()));
                if (g.hasValues()) r.with("min", g.minValue).with("max", g.maxValue);
                return r;
            }));

    reg.add({"maps.raster_save", "Save Raster",
             "Write a raster layer (e.g. a heatmap): .tif writes a GeoTIFF (float32 values, or RGBA for images, Deflate, "
             "geokeys); .png/.jpg writes the picture (value rasters coloured with the ramp) plus a world file and .prj.",
             {docParam(), {"layer", ParamType::String, "raster layer name or index", true}, {"path", ParamType::String, "output file", true},
              {"ramp", ParamType::String, "colour ramp for value rasters saved as pictures (default: the layer's)", false}},
             false},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                Layer* l = doc.map.layer(stringParam(p, "layer"));
                if (!l) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                if (!l->grid) return Result::failure("'" + l->name + "' is a vector layer: use maps.export");
                const auto files = writeRaster(*l->grid, stringParam(p, "path"), stringParam(p, "ramp", l->style.rasterRamp));
                std::string list;
                for (const auto& f : files) list += (list.empty() ? "" : ", ") + f;
                return edited("wrote " + list, p).with("files", list);
            }, false));

    reg.add({"maps.raster_sample", "Sample Raster",
             "Read a raster's value (elevation, density, or the brightness of an image pixel) at points.",
             {docParam(), {"layer", ParamType::String, "raster layer name or index (default: the top raster)", false},
              {"points", ParamType::NumberList, "lat,lon pairs: lat1,lon1,lat2,lon2,...", true}},
             false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                const Layer* l = nullptr;
                if (has(p, "layer")) {
                    l = doc.map.layer(stringParam(p, "layer"));
                } else {
                    for (auto it = doc.map.layers.rbegin(); it != doc.map.layers.rend() && !l; ++it) {
                        if (it->grid) l = &*it;
                    }
                }
                if (!l || !l->grid) return Result::failure("no raster layer to sample (maps.raster_add)");
                const NumberList pts = listParam(p, "points");
                if (pts.size() < 2 || pts.size() % 2) return Result::failure("points needs lat,lon pairs");
                Result r = edited("", p);
                NumberList vals;
                for (std::size_t i = 0; i + 1 < pts.size(); i += 2) {
                    const double v = l->grid->sample(pts[i + 1], pts[i]);
                    vals.push_back(v);
                    r.message += (r.message.empty() ? "" : "\n") + fmt(pts[i], 6) + "," + fmt(pts[i + 1], 6) + ": " + (std::isnan(v) ? "no data" : fmt(v, 3));
                }
                r.with("values", vals);
                if (vals.size() == 1) r.value = vals[0];
                return r;
            }, false));
}

void registerLayerCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.layer_remove", "Remove Layer", "Delete a layer.", {docParam(), {"layer", ParamType::String, "layer name or index", true}, previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                const int i = doc.map.layerIndex(stringParam(p, "layer"));
                if (i < 0) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                const std::string name = doc.map.layers[static_cast<std::size_t>(i)].name;
                doc.map.layers.erase(doc.map.layers.begin() + i);
                return edited("removed layer " + name, p);
            }));

    reg.add({"maps.layer_rename", "Rename Layer", "Rename a layer.",
             {docParam(), {"layer", ParamType::String, "layer name or index", true}, {"name", ParamType::String, "new name", true}}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                Layer* l = doc.map.layer(stringParam(p, "layer"));
                if (!l) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                const std::string name = trim(stringParam(p, "name"));
                if (name.empty()) return Result::failure("give the layer a name");
                if (doc.map.layer(name) && doc.map.layer(name) != l) return Result::failure("there's already a layer called " + name);
                const std::string old = l->name;
                l->name = name;
                return edited("renamed " + old + " to " + name, p).with("layer", name);
            }));

    reg.add({"maps.layer_move", "Move Layer", "Change a layer's drawing order (0 = bottom).",
             {docParam(), {"layer", ParamType::String, "layer name or index", true},
              {"to", ParamType::String, "top, bottom, up, down, or a 0-based index", true}, previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                auto& ls = doc.map.layers;
                const int i = doc.map.layerIndex(stringParam(p, "layer"));
                if (i < 0) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                const std::string to = lower(stringParam(p, "to"));
                int j = 0;
                const int last = static_cast<int>(ls.size()) - 1;
                if (to == "top") j = last;
                else if (to == "bottom") j = 0;
                else if (to == "up") j = std::min(last, i + 1);
                else if (to == "down") j = std::max(0, i - 1);
                else if (!to.empty() && std::all_of(to.begin(), to.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) j = std::min(last, std::stoi(to));
                else return Result::failure("to= must be top, bottom, up, down or an index");
                Layer l = std::move(ls[static_cast<std::size_t>(i)]);
                ls.erase(ls.begin() + i);
                ls.insert(ls.begin() + j, std::move(l));
                return edited("moved " + ls[static_cast<std::size_t>(j)].name + " to position " + std::to_string(j), p).with("index", static_cast<std::int64_t>(j));
            }));

    reg.add({"maps.style", "Style Layer",
             "Change how a layer draws. Single colour, or mode=categorized (a colour per value of field=) or mode=graduated "
             "(classes of a numeric field= by method= quantile/equal/natural (Jenks), coloured along ramp=); polygons in those modes get "
             "fill_opacity 0.85 unless you set it. label= shows a field as text. Only the parameters you pass change.",
             {docParam(), layerParam("layer name or index (default: the top layer)"),
              {"stroke", ParamType::String, "line/outline colour, e.g. #2b5cab or red", false},
              {"width", ParamType::Number, "line/outline width in px (0 = no outline)", false},
              {"fill", ParamType::String, "polygon and point fill colour", false},
              {"fill_opacity", ParamType::Number, "polygon fill opacity 0..1", false},
              {"opacity", ParamType::Number, "whole layer opacity 0..1", false},
              {"point_size", ParamType::Number, "point marker diameter in px", false},
              {"marker", ParamType::String, "point marker shape", false, {"circle", "square", "triangle", "diamond"}},
              {"dash", ParamType::String, "line pattern", false, {"solid", "dash", "dot"}},
              {"label", ParamType::String, "field to label features with (empty = no labels)", false},
              {"label_size", ParamType::Number, "label text size in px", false},
              {"label_color", ParamType::String, "label colour", false},
              {"halo", ParamType::Bool, "white halo around labels", false},
              {"mode", ParamType::String, "single colour, a colour per category, or graduated classes", false, {"single", "categorized", "graduated"}},
              {"field", ParamType::String, "field to categorize or graduate by", false},
              {"ramp", ParamType::String, "graduated colours: viridis, blues, reds, greens, spectral, heat, terrain, gray, or #rrggbb,#rrggbb", false},
              {"palette", ParamType::String, "categorized colours, comma-separated (default: a 12-colour palette)", false},
              {"classes", ParamType::Int, "graduated: number of classes (1-12)", false},
              {"method", ParamType::String, "graduated class breaks: quantile (equal counts), equal (equal widths), natural or jenks (Jenks natural breaks)", false, {"quantile", "equal", "natural", "jenks"}},
              {"size_by_value", ParamType::Bool, "graduated points: bigger markers for bigger values", false},
              {"raster_ramp", ParamType::String, "value rasters: colour ramp", false},
              {"hillshade", ParamType::Bool, "value rasters: shade the relief", false},
              {"visible", ParamType::Bool, "show or hide the layer", false},
              previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = anyLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                Style& s = l->style;
                std::vector<std::string> changed;
                auto color = [&](const char* key, Color& out) {
                    if (!has(p, key)) return true;
                    auto c = Color::parse(stringParam(p, key));
                    if (!c) {
                        error = std::string(key) + " isn't a colour: " + stringParam(p, key);
                        return false;
                    }
                    out = *c;
                    changed.push_back(key);
                    return true;
                };
                auto number = [&](const char* key, double& out, double lo, double hi) {
                    if (!has(p, key)) return;
                    out = std::clamp(numberParam(p, key), lo, hi);
                    changed.push_back(key);
                };
                if (!color("stroke", s.stroke) || !color("fill", s.fill) || !color("label_color", s.labelColor)) return Result::failure(error);
                number("width", s.width, 0, 100);
                number("fill_opacity", s.fillOpacity, 0, 1);
                number("opacity", s.opacity, 0, 1);
                number("point_size", s.pointSize, 0.5, 200);
                number("label_size", s.labelSize, 4, 200);
                if (has(p, "marker")) s.marker = stringParam(p, "marker"), changed.push_back("marker");
                if (has(p, "dash")) s.dash = stringParam(p, "dash") == "solid" ? "" : stringParam(p, "dash"), changed.push_back("dash");
                if (has(p, "label")) s.labelField = stringParam(p, "label"), changed.push_back("label");
                if (has(p, "halo")) s.halo = boolParam(p, "halo"), changed.push_back("halo");
                if (has(p, "field")) s.field = stringParam(p, "field"), changed.push_back("field");
                if (has(p, "ramp")) s.ramp = stringParam(p, "ramp"), changed.push_back("ramp");
                if (has(p, "method")) s.method = stringParam(p, "method"), changed.push_back("method");
                if (has(p, "classes")) s.classes = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "classes"), 1, 12)), changed.push_back("classes");
                if (has(p, "size_by_value")) s.sizeByValue = boolParam(p, "size_by_value"), changed.push_back("size_by_value");
                if (has(p, "raster_ramp")) s.rasterRamp = stringParam(p, "raster_ramp"), changed.push_back("raster_ramp");
                if (has(p, "hillshade")) s.hillshade = boolParam(p, "hillshade"), changed.push_back("hillshade");
                if (has(p, "visible")) l->visible = boolParam(p, "visible"), changed.push_back("visible");
                if (has(p, "palette")) {
                    s.palette.clear();
                    for (const auto& c : splitList(stringParam(p, "palette"))) {
                        auto col = Color::parse(c);
                        if (!col) return Result::failure("palette colour isn't a colour: " + c);
                        s.palette.push_back(*col);
                    }
                    changed.push_back("palette");
                }
                if (has(p, "mode")) {
                    const std::string m = stringParam(p, "mode");
                    if (m != "single" && s.mode == "single" && !has(p, "fill_opacity")) s.fillOpacity = 0.85;
                    s.mode = m;
                    changed.push_back("mode");
                }
                if (s.mode != "single") {
                    if (s.field.empty()) return Result::failure(s.mode + " styling needs field=");
                    const auto fields = l->fields();
                    if (std::find(fields.begin(), fields.end(), s.field) == fields.end())
                        return Result::failure("no feature of " + l->name + " has a field called '" + s.field + "'");
                }
                if (!s.labelField.empty() && !l->isRaster()) {
                    const auto fields = l->fields();
                    if (std::find(fields.begin(), fields.end(), s.labelField) == fields.end())
                        return Result::failure("no feature of " + l->name + " has a field called '" + s.labelField + "' to label with");
                }
                std::string list;
                for (const auto& c : changed) list += (list.empty() ? "" : ", ") + c;
                Result r = edited("styled " + l->name + (list.empty() ? " (nothing changed)" : ": " + list), p);
                const auto classes = styleClasses(*l);
                if (!classes.empty()) {
                    r.message += "\nclasses:";
                    for (const auto& c : classes) r.message += "\n  " + c.color.hex().substr(0, 7) + " " + c.label;
                    r.with("classes", static_cast<std::int64_t>(classes.size()));
                }
                return r.with("layer", l->name);
            }));
}

void registerFeatureCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.features", "List Features",
             "List a layer's features with their index, geometry (point position, line length or polygon area) and "
             "properties; where= filters, offset/limit page through big layers.",
             {docParam(), layerParam(), whereParam(),
              {"fields", ParamType::String, "only show these fields (comma-separated)", false},
              {"offset", ParamType::Int, "skip this many matches", false, {}, std::int64_t{0}},
              {"limit", ParamType::Int, "show at most this many", false, {}, std::int64_t{50}}},
             false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                const auto only = splitList(stringParam(p, "fields"));
                const std::int64_t offset = std::max<std::int64_t>(0, intParam(p, "offset", 0)), limit = std::clamp<std::int64_t>(intParam(p, "limit", 50), 1, 5000);
                std::int64_t matched = 0, shown = 0;
                std::string lines;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    const Feature& f = l->features[i];
                    if (where && !where->test(f, i)) continue;
                    if (matched++ < offset || shown >= limit) continue;
                    ++shown;
                    lines += "\n#" + std::to_string(i) + " " + geometryLine(f.geom);
                    const std::string props = propsLine(f.props, only);
                    if (!props.empty()) lines += " {" + props + "}";
                }
                Result r = edited(l->name + ": " + plural(static_cast<std::size_t>(matched), "feature") + (where ? " match" : "") +
                                      (shown < matched ? ", showing " + std::to_string(shown) + " from " + std::to_string(offset) : "") + lines,
                                  p);
                if (where) r.message += unknownFieldsNote(*l, *where);
                return r.with("layer", l->name).with("count", matched).with("shown", shown);
            }, false));

    reg.add({"maps.feature_add", "Add Feature",
             "Add one feature: a point from lat= and lon=, or any geometry as wkt= (POINT/LINESTRING/POLYGON/MULTI..., "
             "lon lat order), or points= as lat,lon pairs with kind= line or polygon. The layer is created when missing.",
             {docParam(), {"layer", ParamType::String, "layer name (created when missing; default: the top vector layer, or 'features')", false},
              {"lat", ParamType::Number, "point latitude", false}, {"lon", ParamType::Number, "point longitude", false},
              {"wkt", ParamType::String, "geometry as WKT, e.g. LINESTRING (2.35 48.85, 13.4 52.52)", false},
              {"points", ParamType::NumberList, "lat,lon pairs for kind=line/polygon (or several points)", false},
              {"kind", ParamType::String, "what points= draws", false, {"point", "line", "polygon"}, std::string("line")},
              {"props", ParamType::String, "properties as key=value pairs separated by ; (or ,), e.g. \"name=Home; visits=3\"", false},
              previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                Feature f;
                if (has(p, "wkt")) {
                    f.geom = parseWkt(stringParam(p, "wkt"));
                } else if (has(p, "points")) {
                    const NumberList v = listParam(p, "points");
                    if (v.size() < 2 || v.size() % 2) return Result::failure("points needs lat,lon pairs");
                    std::vector<Coord> cs;
                    for (std::size_t i = 0; i + 1 < v.size(); i += 2) cs.push_back({v[i + 1], v[i]});
                    const std::string kind = stringParam(p, "kind", "line");
                    if (kind == "point") {
                        f.geom.type = GeomType::Point;
                        f.geom.points = cs;
                    } else if (kind == "polygon") {
                        if (cs.size() > 1 && cs.front().x == cs.back().x && cs.front().y == cs.back().y) cs.pop_back();
                        if (cs.size() < 3) return Result::failure("a polygon needs at least 3 points");
                        if (signedArea(cs) < 0) std::reverse(cs.begin(), cs.end());
                        f.geom = Geometry::polygon({cs});
                    } else {
                        if (cs.size() < 2) return Result::failure("a line needs at least 2 points");
                        f.geom = Geometry::line(cs);
                    }
                } else if (has(p, "lat") && has(p, "lon")) {
                    f.geom = Geometry::point(numberParam(p, "lon"), numberParam(p, "lat"));
                } else {
                    return Result::failure("give lat= and lon=, wkt=, or points=");
                }
                if (f.geom.empty()) return Result::failure("the geometry is empty");
                bool bad = false;
                f.geom.forEach([&](const Coord& c) { bad = bad || std::abs(c.y) > 90 || std::abs(c.x) > 540 || !std::isfinite(c.x) || !std::isfinite(c.y); });
                if (bad) return Result::failure("coordinates must be degrees (lat -90..90); WKT is lon lat order");
                f.props = parseProps(stringParam(p, "props"));
                Layer* l = nullptr;
                if (has(p, "layer")) {
                    l = doc.map.layer(stringParam(p, "layer"));
                    if (l && l->isRaster()) return Result::failure("'" + l->name + "' is a raster layer");
                    if (!l) {
                        Layer nl;
                        l = &addLayer(doc.map, nl, stringParam(p, "layer"));
                    }
                } else {
                    std::string error;
                    l = vectorLayer(doc.map, p, error);
                    if (!l) l = &addLayer(doc.map, Layer{}, "features");
                }
                l->features.push_back(std::move(f));
                const std::size_t idx = l->features.size() - 1;
                return edited("added #" + std::to_string(idx) + " to " + l->name + ": " + geometryLine(l->features.back().geom), p)
                    .with("layer", l->name)
                    .with("index", static_cast<std::int64_t>(idx));
            }));

    reg.add({"maps.feature_delete", "Delete Features", "Delete features by index= or every feature matching where=.",
             {docParam(), layerParam(), {"index", ParamType::Int, "feature index (maps.features shows them)", false}, whereParam(), previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                if (has(p, "index")) {
                    const std::int64_t i = intParam(p, "index");
                    if (i < 0 || i >= static_cast<std::int64_t>(l->features.size()))
                        return Result::failure(l->name + " has features 0.." + std::to_string(static_cast<std::int64_t>(l->features.size()) - 1));
                    l->features.erase(l->features.begin() + i);
                    return edited("deleted #" + std::to_string(i) + " from " + l->name, p).with("deleted", std::int64_t{1});
                }
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                if (!where) return Result::failure("give index= or where=");
                std::vector<Feature> keep;
                std::size_t i = 0;
                for (auto& f : l->features) {
                    if (!where->test(f, i++)) keep.push_back(std::move(f));
                }
                const std::size_t n = l->features.size() - keep.size();
                l->features = std::move(keep);
                return edited("deleted " + plural(n, "feature") + " from " + l->name + unknownFieldsNote(*l, *where), p).with("deleted", static_cast<std::int64_t>(n));
            }));

    reg.add({"maps.calc", "Calculate Field",
             "Set a field on every feature (or those matching where=) to an expression: arithmetic on fields (pop_est / 1e6), "
             "$area (m\xC2\xB2), $length (m), $x/$y (centroid lon/lat), $id, $type, $vertices, functions round(x, n), lower, upper, "
             "len, abs, floor, ceil, sqrt, log10, min, max, coalesce, str, num, and text in quotes. delete=true removes the field.",
             {docParam(), layerParam(), {"field", ParamType::String, "field to set", true},
              {"expr", ParamType::String, "expression, e.g. \"round($area / 1e6, 1)\"", false}, whereParam(),
              {"delete", ParamType::Bool, "remove the field instead", false, {}, false}}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                const std::string field = trim(stringParam(p, "field"));
                if (field.empty()) return Result::failure("field can't be empty");
                std::size_t n = 0;
                if (boolParam(p, "delete", false)) {
                    for (std::size_t i = 0; i < l->features.size(); ++i) {
                        if (where && !where->test(l->features[i], i)) continue;
                        n += l->features[i].props.erase(field);
                    }
                    return edited("removed " + field + " from " + plural(n, "feature"), p).with("changed", static_cast<std::int64_t>(n));
                }
                if (!has(p, "expr")) return Result::failure("give expr= (or delete=true)");
                const Expr e = Expr::compile(stringParam(p, "expr"));
                std::size_t nulls = 0;
                std::string sample;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    Feature& f = l->features[i];
                    if (where && !where->test(f, i)) continue;
                    const Prop v = e.eval(f, i);
                    if (v.isNull()) ++nulls;
                    if (sample.empty() && !v.isNull()) sample = v.text();
                    f.props[field] = v;
                    ++n;
                }
                Result r = edited("set " + field + " on " + plural(n, "feature") + " of " + l->name + (sample.empty() ? "" : " (first: " + sample + ")"), p);
                if (nulls) r.message += "\n" + plural(nulls, "feature") + " got no value (missing fields or division by zero)";
                r.message += unknownFieldsNote(*l, e);
                return r.with("changed", static_cast<std::int64_t>(n)).with("layer", l->name);
            }));
}

void registerMeasureCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.measure", "Measure",
             "Geodesic measurements on the WGS84 ellipsoid (Vincenty distances, areas on the authalic sphere): the distance "
             "and bearings between two points (from=lat,lon to=lat,lon), the length of a path or the area of a polygon "
             "(points=lat,lon,...), or the total length/area of a layer's features (per feature when few). No map needed for "
             "points; layer= needs doc=.",
             {{"doc", ParamType::String, "map id (for layer=)", false},
              layerParam("layer to measure (its lines' lengths, polygons' areas)"),
              whereParam("only these features"),
              {"from", ParamType::NumberList, "lat,lon of the start", false},
              {"to", ParamType::NumberList, "lat,lon of the end", false},
              {"points", ParamType::NumberList, "lat,lon pairs of a path or polygon", false},
              {"polygon", ParamType::Bool, "points= is a closed polygon: report area and perimeter", false, {}, false}},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        if (has(p, "from") || has(p, "to")) {
                            const NumberList a = listParam(p, "from"), b = listParam(p, "to");
                            if (a.size() != 2 || b.size() != 2) return Result::failure("from= and to= each need lat,lon");
                            const Inverse inv = geodesicInverse(a[1], a[0], b[1], b[0]);
                            Result r = Result::success(metres(inv.distance) + ", initial bearing " + fmt(inv.azimuth1, 2) + "\xC2\xB0, final bearing " +
                                                           fmt(inv.azimuth2, 2) + "\xC2\xB0",
                                                       inv.distance);
                            return r.with("distance_m", inv.distance).with("bearing", inv.azimuth1).with("final_bearing", inv.azimuth2);
                        }
                        if (has(p, "points")) {
                            const NumberList v = listParam(p, "points");
                            if (v.size() < 4 || v.size() % 2) return Result::failure("points needs at least two lat,lon pairs");
                            Line l;
                            for (std::size_t i = 0; i + 1 < v.size(); i += 2) l.push_back({v[i + 1], v[i]});
                            if (boolParam(p, "polygon", false)) {
                                if (l.size() > 1 && l.front().x == l.back().x && l.front().y == l.back().y) l.pop_back();
                                const double a = ringArea(l), per = lineLength(l, true);
                                Result r = Result::success("area " + squareMetres(a) + ", perimeter " + metres(per), a);
                                return r.with("area_m2", a).with("perimeter_m", per);
                            }
                            const double d = lineLength(l);
                            return Result::success("length " + metres(d) + " over " + plural(l.size() - 1, "segment"), d).with("length_m", d);
                        }
                        const std::string id = stringParam(p, "doc");
                        auto doc = session.workspace().getAs<MapDoc>(id);
                        if (!doc) return Result::failure(id.empty() ? "give from= and to=, points=, or doc= with layer=" : "no open map called '" + id + "'");
                        std::string error;
                        Layer* l = vectorLayer(doc->map, p, error);
                        if (!l) return Result::failure(error);
                        auto where = whereFrom(p, error);
                        if (!error.empty()) return Result::failure(error);
                        double totalA = 0, totalL = 0;
                        std::size_t n = 0, pts = 0;
                        std::string lines;
                        for (std::size_t i = 0; i < l->features.size(); ++i) {
                            const Feature& f = l->features[i];
                            if (where && !where->test(f, i)) continue;
                            ++n;
                            const double a = area(f.geom), len = length(f.geom);
                            totalA += a;
                            totalL += len;
                            pts += f.geom.points.size();
                            if (n <= 25 && f.geom.type != GeomType::Point) {
                                std::string name;
                                for (const char* k : {"name", "NAME", "Name", "title", "id"}) {
                                    if (auto it = f.props.find(k); it != f.props.end() && name.empty()) name = " " + it->second.text();
                                }
                                lines += "\n#" + std::to_string(i) + name + ": " + (f.geom.type == GeomType::Polygon ? squareMetres(a) + ", perimeter " + metres(len) : metres(len));
                            }
                        }
                        Result r = Result::success(l->name + ": " + plural(n, "feature") + (totalA > 0 ? ", area " + squareMetres(totalA) : "") +
                                                   (totalL > 0 ? ", " + std::string(totalA > 0 ? "perimeter " : "length ") + metres(totalL) : "") +
                                                   (pts ? ", " + plural(pts, "point") : "") + lines);
                        return r.with("features", static_cast<std::int64_t>(n)).with("area_m2", totalA).with("length_m", totalL);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"maps.transform", "Transform Coordinates",
             "Convert coordinates between WGS84 lon/lat, Web Mercator and UTM (Kr\xC3\xBCger series, sub-millimetre). points= are "
             "x,y pairs in the from= system (lon,lat for wgs84; easting,northing for UTM). to=utm picks the UTM zone of the "
             "first point.",
             {{"points", ParamType::NumberList, "x,y pairs (lon,lat for wgs84)", true},
              {"from", ParamType::String, "source system: " + crsHelp(), false, {}, std::string("wgs84")},
              {"to", ParamType::String, "target system, or utm for the automatic zone", false, {}, std::string("utm")}},
             false, true},
            plainCommand([](const Params& p) {
                const auto from = Crs::parse(stringParam(p, "from", "wgs84"));
                if (!from) return Result::failure("unknown from= system (use " + crsHelp() + ")");
                const NumberList v = listParam(p, "points");
                if (v.size() < 2 || v.size() % 2) return Result::failure("points needs x,y pairs");
                const std::string toName = stringParam(p, "to", "utm");
                std::optional<Crs> to;
                if (lower(toName) == "utm" || lower(toName) == "auto") {
                    const Coord ll = from->inverse({v[0], v[1]});
                    to = Crs::utmFor(ll.x, ll.y);
                } else {
                    to = Crs::parse(toName);
                }
                if (!to) return Result::failure("unknown to= system (use " + crsHelp() + " or utm)");
                NumberList out;
                std::string lines;
                for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
                    const Coord q = to->forward(from->inverse({v[i], v[i + 1]}));
                    out.push_back(q.x);
                    out.push_back(q.y);
                    const int prec = to->geographic() ? 8 : 3;
                    lines += "\n" + fmt(v[i], 8) + "," + fmt(v[i + 1], 8) + " -> " + fmt(q.x, prec) + "," + fmt(q.y, prec);
                }
                Result r = Result::success(from->name() + " to " + to->name() + " (" + to->id() + "):" + lines, out);
                return r.with("points", out).with("crs", to->id()).with("crs_name", to->name());
            }));

    reg.add({"maps.bbox", "Bounding Box", "The extent of a layer (or the whole map) in lon/lat, plus its size in km and a matching UTM zone.",
             {docParam(), layerParam("layer name or index (default: every layer)"), whereParam()}, false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                BBox b;
                std::string what = "map";
                std::string error;
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                auto addLayer = [&](const Layer& l) {
                    if (where && !l.isRaster()) {
                        for (std::size_t i = 0; i < l.features.size(); ++i) {
                            if (where->test(l.features[i], i)) b.add(bboxOf(l.features[i].geom));
                        }
                    } else {
                        b.add(l.bounds());
                    }
                };
                if (has(p, "layer")) {
                    Layer* l = anyLayer(doc.map, p, error);
                    if (!l) return Result::failure(error);
                    what = l->name;
                    addLayer(*l);
                } else if (where) {
                    for (const auto& l : doc.map.layers) {
                        if (!l.isRaster()) addLayer(l);
                    }
                } else {
                    b = doc.map.bounds();
                }
                if (!b.valid()) return Result::failure(what + (where ? " has no geometry matching where=" : " has no geometry"));
                const double midY = (b.minY + b.maxY) / 2;
                // east-west size along the middle latitude, in two halves so spans over 180 degrees measure right
                const double midX = (b.minX + b.maxX) / 2;
                const double w = geodesicDistance(b.minX, midY, midX, midY) + geodesicDistance(midX, midY, b.maxX, midY);
                const double h = geodesicDistance(midX, b.minY, midX, b.maxY);
                const Crs utm = Crs::utmFor((b.minX + b.maxX) / 2, midY);
                Result r = edited(what + ": " + bboxText(b) + " (min_lon,min_lat,max_lon,max_lat), about " + metres(w) + " x " + metres(h) + ", centre in " + utm.name(), p);
                return r.with("bbox", bboxText(b))
                    .with("min_lon", b.minX).with("min_lat", b.minY).with("max_lon", b.maxX).with("max_lat", b.maxY)
                    .with("width_m", w).with("height_m", h).with("utm", utm.id());
            }, false));
}

void registerAnalysisCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.buffer", "Buffer",
             "Grow features by a distance in metres into polygons (negative shrinks polygons). Points become geodesic circles; "
             "lines and polygons are buffered on a local metric grid (resolution= cells across) and traced back, so overlaps "
             "merge cleanly. dissolve=true merges everything into one feature. Writes a new layer.",
             {docParam(), layerParam(), {"distance", ParamType::Number, "buffer distance in metres", true},
              {"dissolve", ParamType::Bool, "merge all buffers into one polygon", false, {}, false},
              {"resolution", ParamType::Int, "grid cells along the longer side (more = finer, slower)", false, {}, std::int64_t{600}},
              {"segments", ParamType::Int, "points: segments per circle", false, {}, std::int64_t{64}}, whereParam(),
              outputParam("new layer name (default: '<layer> buffer')"), previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                const double d = numberParam(p, "distance");
                const int res = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "resolution", 600), 50, 4000));
                const int seg = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "segments", 64), 8, 720));
                Layer out;
                out.style.fill = {0xe0, 0x7b, 0x39, 255};
                out.style.stroke = {0xb3, 0x58, 0x1b, 255};
                std::vector<const Geometry*> gs;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    const Feature& f = l->features[i];
                    if (where && !where->test(f, i)) continue;
                    if (f.geom.empty()) continue;
                    if (boolParam(p, "dissolve", false)) {
                        gs.push_back(&f.geom);
                        continue;
                    }
                    Feature nf;
                    nf.props = f.props;
                    nf.geom = buffer(f.geom, d, res, seg);
                    if (!nf.geom.empty()) out.features.push_back(std::move(nf));
                }
                if (boolParam(p, "dissolve", false)) {
                    Feature nf;
                    if (gs.size() == 1) nf.geom = buffer(*gs[0], d, res, seg);
                    else nf.geom = bufferUnion(gs, d, res);
                    nf.props["count"] = Prop::number(static_cast<double>(gs.size()));
                    if (!nf.geom.empty()) out.features.push_back(std::move(nf));
                }
                if (out.features.empty()) return Result::failure("the buffer is empty (a negative distance removed everything?)");
                const std::string srcName = l->name;
                Layer& added = addLayer(doc.map, std::move(out), has(p, "output") ? stringParam(p, "output") : srcName + " buffer");
                double a = 0;
                for (const auto& f : added.features) a += area(f.geom);
                return edited("buffered " + srcName + " by " + metres(d) + " into " + added.name + " (" + plural(added.features.size(), "feature") + ", " + squareMetres(a) + ")", p)
                    .with("layer", added.name)
                    .with("features", static_cast<std::int64_t>(added.features.size()))
                    .with("area_m2", a);
            }));

    reg.add({"maps.simplify", "Simplify",
             "Douglas-Peucker simplification with a tolerance in metres: drops vertices that stay within tolerance of the "
             "simplified shape. In place, or into output=.",
             {docParam(), layerParam(), {"tolerance", ParamType::Number, "tolerance in metres", true}, outputParam("write a new layer instead of changing this one"),
              previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                const double tol = numberParam(p, "tolerance");
                if (tol <= 0) return Result::failure("tolerance must be more than 0 metres");
                std::size_t before = 0, after = 0, dropped = 0;
                std::vector<Feature> fs;
                for (const auto& f : l->features) {
                    before += f.geom.vertexCount();
                    Feature nf{simplify(f.geom, tol), f.props};
                    if (nf.geom.empty() && !f.geom.empty()) ++dropped;
                    after += nf.geom.vertexCount();
                    fs.push_back(std::move(nf));
                }
                std::string name = l->name;
                if (has(p, "output")) {
                    Layer nl = *l;
                    nl.features = std::move(fs);
                    name = addLayer(doc.map, std::move(nl), stringParam(p, "output")).name;
                } else {
                    l->features = std::move(fs);
                }
                Result r = edited("simplified " + name + ": " + std::to_string(before) + " -> " + std::to_string(after) + " vertices", p);
                if (dropped) r.message += "\n" + plural(dropped, "feature") + " collapsed to nothing at this tolerance";
                return r.with("layer", name).with("vertices_before", static_cast<std::int64_t>(before)).with("vertices_after", static_cast<std::int64_t>(after));
            }));

    reg.add({"maps.clip", "Clip",
             "Cut a layer to a lon/lat box: points outside are dropped, lines and polygons are cut at the edges. In place, or "
             "into output=.",
             {docParam(), layerParam(), {"bbox", ParamType::NumberList, "min_lon,min_lat,max_lon,max_lat", true}, outputParam("write a new layer instead"),
              previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                auto box = bboxFrom(p, "bbox", error);
                if (!box) return Result::failure(error);
                std::vector<Feature> fs;
                for (const auto& f : l->features) {
                    if (!bboxOf(f.geom).intersects(*box)) continue;
                    Feature nf{clip(f.geom, *box), f.props};
                    if (!nf.geom.empty()) fs.push_back(std::move(nf));
                }
                const std::size_t before = l->features.size(), kept = fs.size();
                std::string name = l->name;
                if (has(p, "output")) {
                    Layer nl = *l;
                    nl.features = std::move(fs);
                    name = addLayer(doc.map, std::move(nl), stringParam(p, "output")).name;
                } else {
                    l->features = std::move(fs);
                }
                return edited("clipped " + name + " to " + bboxText(*box) + ": " + std::to_string(kept) + " of " + plural(before, "feature") + " kept", p)
                    .with("layer", name)
                    .with("features", static_cast<std::int64_t>(kept));
            }));

    reg.add({"maps.filter", "Filter",
             "Keep the features matching an expression: comparisons (== != < <= > >=), and/or/not, in (...), contains, "
             "startswith, like ('%land'), is null, arithmetic, $area/$length/$x/$y/$id/$type. Text values need quotes: "
             "\"continent == 'Europe' and pop_est > 5e6\". Writes output= (default '<layer> filtered'), or in_place=true.",
             {docParam(), layerParam(), {"where", ParamType::String, "the expression", true}, outputParam("new layer name"),
              {"in_place", ParamType::Bool, "drop non-matching features from this layer instead", false, {}, false}, previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                auto where = whereFrom(p, error);
                if (!where) return Result::failure(error.empty() ? "where= is empty" : error);
                std::vector<Feature> fs;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    if (where->test(l->features[i], i)) fs.push_back(l->features[i]);
                }
                const std::size_t n = fs.size(), total = l->features.size();
                const std::string note = unknownFieldsNote(*l, *where);
                std::string name = l->name;
                if (boolParam(p, "in_place", false)) {
                    l->features = std::move(fs);
                } else {
                    Layer nl = *l;
                    nl.features = std::move(fs);
                    name = addLayer(doc.map, std::move(nl), has(p, "output") ? stringParam(p, "output") : l->name + " filtered").name;
                }
                return edited(std::to_string(n) + " of " + plural(total, "feature") + " match -> " + name + note, p)
                    .with("layer", name)
                    .with("count", static_cast<std::int64_t>(n));
            }));

    reg.add({"maps.join", "Join CSV",
             "Attach columns of a CSV table to features by a shared key (e.g. a country layer's iso_a3 and a CSV's code column). "
             "Keys match exactly, then case- and space-insensitively; numbers match numerically (\"7\" = \"007\" stays text). "
             "Reports how many features matched and which CSV keys found nothing.",
             {docParam(), layerParam(), {"csv", ParamType::String, "CSV file", true},
              {"key", ParamType::String, "the layer's key field", true},
              {"csv_key", ParamType::String, "the CSV's key column (default: same name as key)", false},
              {"fields", ParamType::String, "CSV columns to copy, comma-separated (default: all but the key)", false},
              {"prefix", ParamType::String, "prefix for copied field names", false},
              {"delimiter", ParamType::String, "CSV delimiter (default: detected)", false}, previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                const Table t = readTable(readFile(stringParam(p, "csv")), stringParam(p, "delimiter"));
                const std::string key = stringParam(p, "key"), csvKey = has(p, "csv_key") ? stringParam(p, "csv_key") : key;
                int kc = -1;
                for (std::size_t i = 0; i < t.header.size(); ++i) {
                    if (t.header[i] == csvKey || (kc < 0 && lower(t.header[i]) == lower(csvKey))) kc = static_cast<int>(i);
                }
                if (kc < 0) {
                    std::string cols;
                    for (const auto& h : t.header) cols += (cols.empty() ? "" : ", ") + h;
                    return Result::failure("the CSV has no column '" + csvKey + "' (columns: " + cols + ")");
                }
                const auto wanted = splitList(stringParam(p, "fields"));
                std::vector<std::size_t> cols;
                for (std::size_t i = 0; i < t.header.size(); ++i) {
                    if (static_cast<int>(i) == kc) continue;
                    if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), t.header[i]) == wanted.end()) continue;
                    cols.push_back(i);
                }
                for (const auto& w : wanted) {
                    if (std::find(t.header.begin(), t.header.end(), w) == t.header.end()) return Result::failure("the CSV has no column '" + w + "'");
                }
                auto norm = [](const std::string& s) {
                    std::string out;
                    for (char c : s) {
                        if (!std::isspace(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    return out;
                };
                std::map<std::string, std::size_t> exact, loose;
                for (std::size_t r = 0; r < t.rows.size(); ++r) {
                    if (static_cast<std::size_t>(kc) >= t.rows[r].size()) continue;
                    const std::string k = trim(t.rows[r][static_cast<std::size_t>(kc)]);
                    exact.emplace(k, r);
                    loose.emplace(norm(k), r);
                    if (auto n = Prop::infer(k); n.kind == Prop::Number) loose.emplace("#" + Prop::number(n.num).text(), r);
                }
                const std::string prefix = stringParam(p, "prefix");
                std::size_t matched = 0;
                std::set<std::size_t> usedRows;
                for (auto& f : l->features) {
                    auto it = f.props.find(key);
                    if (it == f.props.end() || it->second.isNull()) continue;
                    const std::string k = it->second.text();
                    std::optional<std::size_t> row;
                    if (auto e = exact.find(k); e != exact.end()) row = e->second;
                    else if (auto lo = loose.find(norm(k)); lo != loose.end()) row = lo->second;
                    else if (it->second.kind == Prop::Number) {
                        if (auto ln = loose.find("#" + it->second.text()); ln != loose.end()) row = ln->second;
                    }
                    if (!row) continue;
                    ++matched;
                    usedRows.insert(*row);
                    const auto& rr = t.rows[*row];
                    for (std::size_t c : cols) f.props[prefix + t.header[c]] = Prop::infer(c < rr.size() ? trim(rr[c]) : std::string());
                }
                std::string unmatched;
                std::size_t un = 0;
                for (std::size_t r = 0; r < t.rows.size(); ++r) {
                    if (usedRows.count(r) || static_cast<std::size_t>(kc) >= t.rows[r].size()) continue;
                    if (++un <= 10) unmatched += (unmatched.empty() ? "" : ", ") + t.rows[r][static_cast<std::size_t>(kc)];
                }
                Result r = edited("joined " + std::to_string(cols.size()) + " column(s) onto " + std::to_string(matched) + " of " + plural(l->features.size(), "feature") +
                                      " of " + l->name,
                                  p);
                if (un) r.message += "\n" + plural(un, "CSV row") + " matched no feature: " + unmatched + (un > 10 ? ", ..." : "");
                return r.with("matched", static_cast<std::int64_t>(matched)).with("unmatched_rows", static_cast<std::int64_t>(un)).with("layer", l->name);
            }));
}

void registerSpatialCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.contains", "Point in Polygon", "Which polygon features of a layer contain a point (e.g. which country is 48.85,2.35 in).",
             {docParam(), layerParam("polygon layer (default: the top vector layer)"), {"lat", ParamType::Number, "latitude", true},
              {"lon", ParamType::Number, "longitude", true}},
             false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                const double lat = numberParam(p, "lat"), lon = numberParam(p, "lon");
                std::string lines;
                NumberList idx;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    if (!pointInGeometry(l->features[i].geom, lon, lat)) continue;
                    idx.push_back(static_cast<double>(i));
                    lines += "\n#" + std::to_string(i) + " {" + propsLine(l->features[i].props, {}, 200) + "}";
                }
                Result r = edited(fmt(lat, 6) + "," + fmt(lon, 6) + " is inside " + plural(idx.size(), "feature") + " of " + l->name + lines, p);
                return r.with("count", static_cast<std::int64_t>(idx.size())).with("indexes", idx);
            }, false));

    reg.add({"maps.nearest", "Nearest Features",
             "The features of a layer closest to a point, by geodesic distance to their nearest edge or vertex (0 inside a "
             "polygon), with the closest point on each.",
             {docParam(), layerParam(), {"lat", ParamType::Number, "latitude", true}, {"lon", ParamType::Number, "longitude", true},
              {"count", ParamType::Int, "how many to list", false, {}, std::int64_t{1}}, whereParam()},
             false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                const double lat = numberParam(p, "lat"), lon = numberParam(p, "lon");
                std::vector<std::pair<double, std::size_t>> hits;
                std::vector<Coord> at(l->features.size());
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    if (where && !where->test(l->features[i], i)) continue;
                    const Nearest n = nearestPoint(l->features[i].geom, lon, lat);
                    if (!std::isfinite(n.distance)) continue;
                    hits.push_back({n.distance, i});
                    at[i] = n.point;
                }
                if (hits.empty()) return Result::failure(l->name + " has no features to measure to");
                std::sort(hits.begin(), hits.end());
                const std::size_t k = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "count", 1), 1, 1000));
                std::string lines;
                NumberList idx, dist;
                for (std::size_t j = 0; j < hits.size() && j < k; ++j) {
                    const auto [d, i] = hits[j];
                    idx.push_back(static_cast<double>(i));
                    dist.push_back(d);
                    lines += "\n#" + std::to_string(i) + " " + metres(d) + " (at " + fmt(at[i].y, 6) + "," + fmt(at[i].x, 6) + ") {" + propsLine(l->features[i].props, {}, 200) + "}";
                }
                Result r = edited("nearest in " + l->name + " to " + fmt(lat, 6) + "," + fmt(lon, 6) + ":" + lines, p);
                r.value = dist[0];
                return r.with("index", static_cast<std::int64_t>(hits[0].second)).with("distance_m", dist[0]).with("indexes", idx).with("distances_m", dist);
            }, false));

    reg.add({"maps.spatial_join", "Spatial Join",
             "Copy fields between layers by location. mode=within: each feature of target= gets fields of the polygon of join= "
             "it lies in (by its centroid), and count_field= counts target features per polygon on the join layer. "
             "mode=nearest: each target feature gets fields of the nearest join feature plus <prefix>distance_m.",
             {docParam(), {"target", ParamType::String, "layer that receives fields (e.g. points)", true},
              {"join", ParamType::String, "layer the fields come from (e.g. regions)", true},
              {"mode", ParamType::String, "match by containment or by distance", false, {"within", "nearest"}, std::string("within")},
              {"fields", ParamType::String, "fields of join= to copy, comma-separated (default: all)", false},
              {"prefix", ParamType::String, "prefix for copied fields", false, {}, std::string("")},
              {"count_field", ParamType::String, "within: field on join= that receives how many target features fall inside", false},
              {"max_distance", ParamType::Number, "nearest: ignore join features farther than this (metres)", false}, previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                Layer* t = doc.map.layer(stringParam(p, "target"));
                Layer* j = doc.map.layer(stringParam(p, "join"));
                if (!t || !j) return Result::failure("no layer called '" + stringParam(p, !t ? "target" : "join") + "' (maps.info lists them)");
                if (t->isRaster() || j->isRaster()) return Result::failure("spatial joins need vector layers");
                const auto wanted = splitList(stringParam(p, "fields"));
                const std::string prefix = stringParam(p, "prefix");
                const bool nearest = stringParam(p, "mode", "within") == "nearest";
                const double maxD = has(p, "max_distance") ? numberParam(p, "max_distance") : std::numeric_limits<double>::infinity();
                std::vector<double> counts(j->features.size(), 0);
                std::vector<BBox> boxes;
                for (const auto& f : j->features) boxes.push_back(bboxOf(f.geom));
                std::size_t matched = 0;
                for (auto& f : t->features) {
                    if (f.geom.empty()) continue;
                    const Coord c = f.geom.type == GeomType::Point ? f.geom.points[0] : centroid(f.geom);
                    int best = -1;
                    double bestD = maxD;
                    for (std::size_t k = 0; k < j->features.size(); ++k) {
                        if (nearest) {
                            const double d = nearestPoint(j->features[k].geom, c.x, c.y).distance;
                            if (d <= bestD) {
                                bestD = d;
                                best = static_cast<int>(k);
                            }
                        } else if (boxes[k].contains(c.x, c.y) && pointInGeometry(j->features[k].geom, c.x, c.y)) {
                            best = static_cast<int>(k);
                            break;
                        }
                    }
                    if (best < 0) continue;
                    ++matched;
                    counts[static_cast<std::size_t>(best)] += 1;
                    for (const auto& [k, v] : j->features[static_cast<std::size_t>(best)].props) {
                        if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), k) == wanted.end()) continue;
                        f.props[prefix + k] = v;
                    }
                    if (nearest) f.props[prefix + "distance_m"] = Prop::number(bestD);
                }
                if (has(p, "count_field") && !nearest) {
                    for (std::size_t k = 0; k < j->features.size(); ++k) j->features[k].props[stringParam(p, "count_field")] = Prop::number(counts[k]);
                }
                return edited(std::to_string(matched) + " of " + plural(t->features.size(), "feature") + " of " + t->name + (nearest ? " matched a nearest " : " fall inside ") +
                                  j->name,
                              p)
                    .with("matched", static_cast<std::int64_t>(matched));
            }));

    reg.add({"maps.heatmap", "Heatmap",
             "Point density as a raster layer (quartic kernel on a Web Mercator grid): radius= in metres (default 1/20 of the "
             "extent), weight= a numeric field, drawn with the transparent-to-red heat ramp. Lines (GPS tracks) count all along "
             "their length, every spacing= metres, so a layer of many tracks shows the most used routes. Save it with maps.raster_save.",
             {docParam(), layerParam("point layer (default: the top layer with points)"), {"radius", ParamType::Number, "kernel radius in metres", false},
              {"resolution", ParamType::Int, "cells along the longer side", false, {}, std::int64_t{400}},
              {"weight", ParamType::String, "numeric field weighting each point", false}, whereParam(),
              {"spacing", ParamType::Number, "lines and outlines: a point every this many metres (default: a quarter of the radius; 0 = one point per feature)", false},
              {"bbox", ParamType::NumberList, "grid extent min_lon,min_lat,max_lon,max_lat (default: the points plus the radius)", false},
              {"ramp", ParamType::String, "colour ramp", false, {}, std::string("heat")},
              {"opacity", ParamType::Number, "0..1", false, {}, 0.85}, outputParam("new layer name (default '<layer> heatmap')"), previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Point);
                if (!l) return Result::failure(error);
                auto where = whereFrom(p, error);
                if (!error.empty()) return Result::failure(error);
                HeatmapOptions o;
                o.radius = numberParam(p, "radius", 0);
                o.resolution = static_cast<int>(intParam(p, "resolution", 400));
                o.weightField = stringParam(p, "weight");
                o.lineSpacing = has(p, "spacing") ? std::max(0.0, numberParam(p, "spacing")) : -1;
                o.bbox = bboxFrom(p, "bbox", error);
                if (!error.empty()) return Result::failure(error);
                std::vector<Feature> pts;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    if (!where || where->test(l->features[i], i)) pts.push_back(l->features[i]);
                }
                Layer h;
                h.grid = heatmap(pts, o);
                h.style.rasterRamp = stringParam(p, "ramp", "heat");
                h.style.opacity = std::clamp(numberParam(p, "opacity", 0.85), 0.0, 1.0);
                h.source = "heatmap of " + l->name;
                // keep markers above the heat
                Layer& added = addLayer(doc.map, std::move(h), has(p, "output") ? stringParam(p, "output") : l->name + " heatmap");
                const Grid& g = *added.grid;
                return edited("heatmap " + added.name + ": " + std::to_string(g.width) + "x" + std::to_string(g.height) + " cells, peak " + fmt(g.maxValue, 3), p)
                    .with("layer", added.name)
                    .with("width", static_cast<std::int64_t>(g.width))
                    .with("height", static_cast<std::int64_t>(g.height))
                    .with("max", g.maxValue);
            }));
}

void registerTrackCommands(Session& session) {
    auto& reg = session.registry();
    auto trackLines = [](Layer& l, const Params& p, std::string& error) {
        std::vector<const Line*> lines;
        std::vector<std::size_t> which;
        if (has(p, "index")) {
            const std::int64_t i = intParam(p, "index");
            if (i < 0 || i >= static_cast<std::int64_t>(l.features.size())) {
                error = l.name + " has features 0.." + std::to_string(static_cast<std::int64_t>(l.features.size()) - 1);
                return lines;
            }
            which.push_back(static_cast<std::size_t>(i));
        } else {
            for (std::size_t i = 0; i < l.features.size(); ++i) which.push_back(i);
        }
        for (auto i : which) {
            for (const auto& ln : l.features[i].geom.lines) lines.push_back(&ln);
        }
        if (lines.empty()) error = l.name + " has no lines (tracks)";
        return lines;
    };
    reg.add({"maps.track_stats", "Track Stats",
             "GPS track statistics for a line layer (e.g. a GPX import): distance, elevation gain and loss (with a hysteresis "
             "against GPS noise), min/max elevation, start/end time, total and moving time (above 0.5 m/s), average, moving "
             "and max speed (smoothed over 5 points).",
             {docParam(), layerParam("track layer (default: the top layer with lines)"), {"index", ParamType::Int, "one feature only (default: all lines)", false},
              {"hysteresis", ParamType::Number, "elevation change in metres to count as climbing", false, {}, 3.0}},
             false, true},
            mapCommand(session, [trackLines](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Line);
                if (!l) return Result::failure(error);
                const auto lines = trackLines(*l, p, error);
                if (lines.empty()) return Result::failure(error);
                const TrackStats s = trackStats(lines, std::max(0.0, numberParam(p, "hysteresis", 3.0)));
                std::string m = l->name + ": " + metres(s.distance) + " over " + plural(s.points, "point");
                if (s.hasEle) m += "\nelevation: +" + fmt(s.gain, 1) + " m / -" + fmt(s.loss, 1) + " m, " + fmt(s.minEle, 1) + " to " + fmt(s.maxEle, 1) + " m";
                if (s.hasTime) {
                    m += "\ntime: " + formatTime(s.startTime) + " to " + formatTime(s.endTime) + ", " + duration(s.duration) + " total, " + duration(s.movingTime) + " moving";
                    m += "\nspeed: average " + fmt(s.avgSpeed * 3.6, 2) + " km/h, moving " + fmt(s.movingSpeed * 3.6, 2) + " km/h, max " + fmt(s.maxSpeed * 3.6, 2) + " km/h";
                    if (s.movingSpeed > 0) {
                        const double pace = 1000 / s.movingSpeed;
                        char buf[32];
                        std::snprintf(buf, sizeof buf, "%d:%02d", static_cast<int>(pace / 60), static_cast<int>(pace) % 60);
                        m += ", pace " + std::string(buf) + " min/km";
                    }
                } else {
                    m += "\nno timestamps, so no speeds";
                }
                Result r = edited(m, p);
                r.value = s.distance;
                r.with("distance_m", s.distance).with("points", static_cast<std::int64_t>(s.points));
                if (s.hasEle) r.with("gain_m", s.gain).with("loss_m", s.loss).with("min_ele", s.minEle).with("max_ele", s.maxEle);
                if (s.hasTime) {
                    r.with("start", formatTime(s.startTime)).with("end", formatTime(s.endTime)).with("duration_s", s.duration).with("moving_s", s.movingTime);
                    r.with("avg_kmh", s.avgSpeed * 3.6).with("moving_kmh", s.movingSpeed * 3.6).with("max_kmh", s.maxSpeed * 3.6);
                }
                return r;
            }, false));

    reg.add({"maps.profile", "Elevation Profile",
             "Draw a track's elevation against distance as a chart (.png or .svg). Without path= it only attaches the PNG.",
             {docParam(), layerParam("track layer (default: the top layer with lines)"), {"index", ParamType::Int, "one feature only (default: all lines joined)", false},
              {"path", ParamType::String, "output .png or .svg (optional)", false}, {"width", ParamType::Int, "px", false, {}, std::int64_t{900}},
              {"height", ParamType::Int, "px", false, {}, std::int64_t{300}}, {"title", ParamType::String, "chart title", false}},
             false},
            mapCommand(session, [trackLines](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Line);
                if (!l) return Result::failure(error);
                const auto lines = trackLines(*l, p, error);
                if (lines.empty()) return Result::failure(error);
                std::vector<std::pair<double, double>> prof;
                double d = 0;
                const Coord* prev = nullptr;
                for (const Line* ln : lines) {
                    for (const auto& c : *ln) {
                        if (prev) d += geodesicDistance(prev->x, prev->y, c.x, c.y);
                        prev = &c;
                        if (!std::isnan(c.z)) prof.push_back({d, c.z});
                    }
                }
                if (prof.size() < 2) return Result::failure(l->name + " has no elevations");
                const int w = static_cast<int>(intParam(p, "width", 900)), h = static_cast<int>(intParam(p, "height", 300));
                const std::string title = has(p, "title") ? stringParam(p, "title") : l->name;
                Result r = edited("profile of " + l->name + ": " + metres(d) + ", " + plural(prof.size(), "point"), p);
                if (has(p, "path")) {
                    const fs::path path = stringParam(p, "path");
                    const std::string fmtName = lower(path.extension().string()) == ".svg" ? "svg" : "png";
                    writeFile(path, renderProfile(prof, w, h, fmtName, title));
                    r.message += " -> " + path.string();
                    r.with("path", path.string());
                }
                r.attach({"image/png", renderProfile(prof, w, h, "png", title), "profile.png"});
                return r;
            }, false));
}

std::string zoomText(int a, int b) { return a == b ? std::to_string(a) : std::to_string(a) + "-" + std::to_string(b); }

void registerTilesCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.tiles_info", "Map Tiles Info",
             "Describe an offline map tile file (.pmtiles) or, with no path, the installed map packs: area, zoom levels, tile format "
             "(vector mvt, png, jpg), compression, layers and credit. Use the file with maps.render basemap=<path>.",
             {{"path", ParamType::String, "a .pmtiles file (default: the most detailed installed map pack)", false}}, false, true},
            plainCommand([](const Params& p) {
                fs::path path = stringParam(p, "path");
                const auto packs = findMapPacks();
                if (path.empty()) {
                    const auto best = bestMapPack();
                    if (!best)
                        return Result::failure("no map pack is installed in " + packsDir().string() +
                                               ". Install one from an offline edition: hub install maps --from <the edition folder> "
                                               "(or pass path= to a .pmtiles file)");
                    path = *best;
                }
                const std::string ext = lower(path.extension().string());
                if (ext == ".mbtiles")
                    return Result::failure("MBTiles files need SQLite, which the maps app leaves out: convert it once with "
                                           "'pmtiles convert in.mbtiles out.pmtiles' and use the .pmtiles file");
                const auto reader = openPmtiles(path);
                const TilesInfo info = describePmtiles(*reader);
                const PmtilesHeader& h = info.header;
                const BBox b{h.minLon, h.minLat, h.maxLon, h.maxLat};
                std::string layers;
                for (const auto& l : info.layers) layers += (layers.empty() ? "" : ",") + l;
                std::string msg = path.string() + ": " + std::string(toString(h.tileType)) + " tiles" + (h.tileType == TileType::Mvt ? " (vector)" : "") +
                                  ", zoom " + zoomText(h.minZoom, h.maxZoom) + ", area " + bboxText(b) + " (lon,lat), " +
                                  std::to_string(h.addressedTiles) + " tiles, " + std::string(toString(h.tileCompression)) + " compressed";
                if (!info.name.empty()) msg += "\nname: " + info.name;
                if (!layers.empty()) msg += "\nlayers: " + layers;
                if (!info.attribution.empty()) msg += "\ncredit: " + info.attribution;
                if (h.tileType != TileType::Mvt && h.tileType != TileType::Png && h.tileType != TileType::Jpeg)
                    msg += "\nwarning: the maps app can't draw " + std::string(toString(h.tileType)) + " tiles (only vector, PNG and JPEG)";
                if (h.tileCompression == TileCompression::Brotli || h.tileCompression == TileCompression::Zstd)
                    msg += "\nwarning: the maps app can't read " + std::string(toString(h.tileCompression)) + " tiles (only gzip or none)";
                if (packs.size() > 1) {
                    msg += "\ninstalled map packs:";
                    for (const auto& pk : packs) msg += "\n  " + pk.string();
                }
                Result r = Result::success(msg, path.string());
                r.with("path", path.string())
                    .with("format", std::string(toString(h.tileType)))
                    .with("compression", std::string(toString(h.tileCompression)))
                    .with("min_zoom", static_cast<std::int64_t>(h.minZoom))
                    .with("max_zoom", static_cast<std::int64_t>(h.maxZoom))
                    .with("bbox", bboxText(b))
                    .with("center", fmt(h.centerLat, 6) + "," + fmt(h.centerLon, 6))
                    .with("tiles", static_cast<std::int64_t>(h.addressedTiles))
                    .with("layers", layers)
                    .with("attribution", info.attribution);
                return r;
            }));
}

void registerRenderCommands(Session& session) {
    auto& reg = session.registry();
    std::vector<ParamSpec> renderParams = {docParam(), {"path", ParamType::String, "output .png, .jpg, .svg or .pdf", true},
                                           {"format", ParamType::String, "override the extension", false, {"auto", "png", "jpg", "svg", "pdf"}, std::string("auto")},
                                           {"dpi", ParamType::Number, "PDF: pixels per inch for the page size", false, {}, 96.0},
                                           {"preview", ParamType::Bool, "also attach a PNG of the render", false, {}, false}};
    for (auto& s : viewParams()) renderParams.push_back(s);
    reg.add({"maps.render", "Render Map",
             "Draw the map to a file: .png/.jpg (antialiased raster), .svg or .pdf (vector). Visible layers bottom to top over "
             "a basemap (auto = an installed map pack if there is one, else countries; countries = built-in Natural Earth outlines; "
             "a .pmtiles file = offline map tiles from disk; osm = OpenStreetMap tiles, downloaded with curl only when "
             "asked, one at a time, cached under the user cache dir; offline=true uses the cache only), with a scale bar, north "
             "arrow, legend, optional graticule and title. Choose the projection with crs= and the area with bbox= or "
             "center=+radius_km=.",
             renderParams, false},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                RenderOptions o;
                std::string error;
                if (!viewOptions(doc, p, o, error)) return Result::failure(error);
                o.dpi = std::clamp(numberParam(p, "dpi", 96), 10.0, 2400.0);
                const fs::path path = stringParam(p, "path");
                std::string format = stringParam(p, "format", "auto");
                if (format == "auto") {
                    std::string ext = lower(path.extension().string());
                    format = ext == ".svg" ? "svg" : ext == ".pdf" ? "pdf" : (ext == ".jpg" || ext == ".jpeg") ? "jpg" : ext == ".png" ? "png" : "";
                    if (format.empty()) return Result::failure("use a .png, .jpg, .svg or .pdf path (or format=)");
                }
                RenderInfo info;
                writeFile(path, renderMap(doc.map, o, format, &info));
                Result r = renderResult("rendered " + std::to_string(info.width) + "x" + std::to_string(info.height) + " " + format + " to " + path.string(), info, o, p);
                r.with("path", path.string());
                if (boolParam(p, "preview", false)) r.attach({"image/png", raster::previewPng(renderImage(doc.map, o), 1024), "preview.png"});
                return r;
            }, false));

    std::vector<ParamSpec> previewParams = {docParam()};
    for (auto& s : viewParams()) previewParams.push_back(s);
    previewParams[1].defaultValue = std::int64_t{800};
    reg.add({"maps.preview", "Preview Map",
             "Render the map to a PNG attachment (nothing written to disk) with the same view options as maps.render: use it "
             "to look at the map, zoom with bbox= or center=, or try a projection.",
             previewParams, false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                RenderOptions o;
                std::string error;
                Params q = p;
                if (!has(q, "width")) q["width"] = std::int64_t{800};
                if (!viewOptions(doc, q, o, error)) return Result::failure(error);
                RenderInfo info;
                const std::string png = renderMap(doc.map, o, "png", &info);
                Result r = renderResult("preview " + std::to_string(info.width) + "x" + std::to_string(info.height) + ", showing " + bboxText(info.bbox), info, o, p);
                r.attach({"image/png", png, "map.png"});
                return r;
            }, false));
}

}  // namespace

void registerCommands(Session& session) {
    registerFileCommands(session);
    registerRasterCommands(session);
    registerLayerCommands(session);
    registerFeatureCommands(session);
    registerMeasureCommands(session);
    registerAnalysisCommands(session);
    registerSpatialCommands(session);
    registerTrackCommands(session);
    registerRenderCommands(session);
    registerTilesCommands(session);
    cmd::registerEditCommands(session);
    cmd::registerOverlayCommands(session);
    cmd::registerTrackToolCommands(session);
    cmd::registerNavCommands(session);
    cmd::registerPlaceCommands(session);
}

}  // namespace wetwaresuite::maps
