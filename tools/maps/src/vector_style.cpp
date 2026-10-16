// A small built-in light style for the Protomaps basemap layers (earth, water,
// landcover/landuse, buildings, roads, transit, boundaries, places). Speed over
// beauty: flat fills, one casing for big roads, dashed boundaries, place names.
#include <algorithm>
#include <cmath>

#include "internal.hpp"

namespace wetwaresuite::maps::detail {

namespace {

constexpr Color kWater{0xa6, 0xcb, 0xe3, 255};
constexpr Color kEarth{0xf2, 0xef, 0xe9, 255};

std::string kindOf(const Feature& f) {
    for (const char* k : {"kind", "pmap:kind", "class", "type"}) {
        auto it = f.props.find(k);
        if (it != f.props.end() && it->second.kind == Prop::String) return it->second.str;
    }
    return {};
}

double numberOf(const Feature& f, const char* key, double fallback) {
    auto it = f.props.find(key);
    if (it == f.props.end()) return fallback;
    return it->second.asNumber().value_or(fallback);
}

std::optional<Color> landColor(const std::string& kind) {
    static const std::vector<std::pair<std::vector<std::string>, Color>> table = {
        {{"park", "garden", "grass", "grassland", "meadow", "golf_course", "recreation_ground", "playground", "pitch", "village_green",
          "nature_reserve", "national_park", "protected_area", "allotments", "scrub", "heath"},
         {0xd4, 0xe8, 0xc4, 255}},
        {{"forest", "wood", "orchard", "vineyard"}, {0xbf, 0xdc, 0xae, 255}},
        {{"farmland", "farm", "farmyard", "crop"}, {0xef, 0xf0, 0xdc, 255}},
        {{"glacier", "ice"}, {0xfb, 0xfc, 0xff, 255}},
        {{"sand", "beach", "barren", "bare_rock", "scree", "desert"}, {0xf3, 0xea, 0xd0, 255}},
        {{"wetland", "marsh", "swamp", "bog"}, {0xd7, 0xe6, 0xdc, 255}},
        {{"urban_area", "residential", "neighbourhood"}, {0xea, 0xe6, 0xe0, 255}},
        {{"industrial", "railway", "commercial", "retail", "military", "landfill", "quarry"}, {0xe8, 0xe1, 0xe6, 255}},
        {{"hospital", "school", "university", "college", "kindergarten"}, {0xf3, 0xe4, 0xe2, 255}},
        {{"cemetery", "grave_yard"}, {0xdb, 0xe4, 0xd4, 255}},
        {{"aerodrome", "airport", "runway", "taxiway", "apron"}, {0xe3, 0xe2, 0xea, 255}},
        {{"pedestrian", "platform"}, {0xee, 0xec, 0xea, 255}},
        {{"water", "ocean", "lake", "reservoir", "basin", "river", "riverbank", "dock"}, kWater},
    };
    for (const auto& [kinds, c] : table) {
        if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end()) return c;
    }
    return std::nullopt;
}

struct RoadStyle {
    Color fill, casing;
    double base, minWidth;  // width in px at zoom 14, and the thinnest it gets
    int order;
    std::string dash;
};

std::optional<RoadStyle> roadStyle(const std::string& kind, const std::string& layer, double zoom) {
    if (layer == "transit" || kind == "rail") return RoadStyle{{0x9c, 0x9c, 0xa4, 255}, {0, 0, 0, 0}, 1.4, 0.6, 50, "dash"};
    if (kind == "highway") return RoadStyle{{0xf5, 0xb0, 0x6c, 255}, {0xd6, 0x8c, 0x45, 255}, 7, 0.9, 46, ""};
    if (kind == "major_road") return RoadStyle{{0xfc, 0xdf, 0x9b, 255}, {0xd5, 0xb6, 0x6e, 255}, 6, 0.7, 45, ""};
    if (kind == "medium_road") return RoadStyle{{0xff, 0xff, 0xff, 255}, {0xcc, 0xc4, 0xb8, 255}, 4.5, 0.5, 44, ""};
    if (kind == "minor_road" || kind == "other" || kind.empty()) {
        if (zoom < 11) return std::nullopt;
        return RoadStyle{{0xff, 0xff, 0xff, 255}, {0xd2, 0xcb, 0xc0, 255}, 3.5, 0.4, 43, ""};
    }
    if (kind == "path") {
        if (zoom < 13) return std::nullopt;
        return RoadStyle{{0xb2, 0xa2, 0x90, 255}, {0, 0, 0, 0}, 1.2, 0.6, 42, "dot"};
    }
    return std::nullopt;
}

// Pixel geometry of one feature, clipped to the image plus a margin.
Geometry toPixels(const Geometry& g, const TileToPixels& toPx, int W, int H, double margin) {
    Geometry px;
    px.type = g.type;
    auto conv = [&](const std::vector<Coord>& cs) {
        std::vector<Coord> out;
        out.reserve(cs.size());
        for (const auto& c : cs) {
            const auto [x, y] = toPx(c.x, c.y);
            out.push_back({x, y});
        }
        return out;
    };
    for (const auto& l : g.lines) px.lines.push_back(conv(l));
    for (const auto& poly : g.polygons) {
        Polygon pp;
        for (const auto& r : poly) pp.push_back(conv(r));
        px.polygons.push_back(std::move(pp));
    }
    px.points = conv(g.points);
    const BBox box{-margin, -margin, W + margin, H + margin};
    const BBox b = bboxOf(px);
    if (!b.valid() || !b.intersects(box)) return {};
    if (b.minX >= box.minX && b.maxX <= box.maxX && b.minY >= box.minY && b.maxY <= box.maxY) return px;
    if (px.type == GeomType::Point) return px;
    return clip(px, box);
}

void addParts(const Geometry& px, StyledPath& out) {
    auto put = [&](const std::vector<Coord>& cs) {
        std::vector<std::pair<double, double>> p;
        p.reserve(cs.size());
        for (const auto& c : cs) p.emplace_back(c.x, c.y);
        out.parts.push_back(std::move(p));
    };
    for (const auto& l : px.lines) put(l);
    for (const auto& poly : px.polygons) {
        for (const auto& r : poly) put(r);
    }
}

double scaled(double base, double minWidth, double zoom) { return std::clamp(base * std::pow(2.0, (zoom - 14) * 0.55), minWidth, base * 2.2); }

void fillFeature(const Feature& f, Color c, int order, const TileToPixels& toPx, int W, int H, std::vector<StyledPath>& paths) {
    if (f.geom.type != GeomType::Polygon) return;
    const Geometry px = toPixels(f.geom, toPx, W, H, 2);
    if (px.polygons.empty()) return;
    StyledPath p;
    p.closed = true;
    p.fill = c;
    p.width = 0;
    p.order = order;
    addParts(px, p);
    paths.push_back(std::move(p));
}

void strokeFeature(const Feature& f, Color c, double width, const std::string& dash, int order, const TileToPixels& toPx, int W, int H,
                   std::vector<StyledPath>& paths) {
    if (f.geom.type != GeomType::Line) return;
    const Geometry px = toPixels(f.geom, toPx, W, H, width + 4);
    if (px.lines.empty()) return;
    StyledPath p;
    p.stroke = c;
    p.width = width;
    p.dash = dash;
    p.order = order;
    addParts(px, p);
    paths.push_back(std::move(p));
}

void placeLabel(const Feature& f, double zoom, const TileToPixels& toPx, int W, int H, std::vector<StyledLabel>& labels) {
    if (f.geom.type != GeomType::Point || f.geom.points.empty()) return;
    std::string name;
    for (const char* k : {"name", "name:en", "pmap:name"}) {
        auto it = f.props.find(k);
        if (it != f.props.end() && !it->second.text().empty()) {
            name = it->second.text();
            break;
        }
    }
    if (name.empty()) return;
    // Protomaps' min_zoom is for 512 px tiles, the same scale as `zoom` here
    if (numberOf(f, "min_zoom", 0) > zoom + 0.5) return;
    const std::string kind = kindOf(f);
    const double pop = numberOf(f, "population_rank", 0);
    StyledLabel l;
    const auto [x, y] = toPx(f.geom.points[0].x, f.geom.points[0].y);
    if (x < 0 || y < 0 || x > W || y > H) return;
    l.x = x;
    l.y = y;
    l.text = name;
    if (kind == "country") {
        l.size = 13;
        l.bold = true;
        l.color = {0x5a, 0x4e, 0x6c, 255};
        l.rank = 1000 + pop;
    } else if (kind == "region" || kind == "state") {
        if (zoom < 4) return;
        l.size = 11;
        l.color = {0x80, 0x76, 0x8e, 255};
        l.rank = 500 + pop;
    } else if (kind == "locality" || kind == "city" || kind == "town" || kind == "village") {
        l.size = 10 + std::clamp((pop - 8) * 0.5, 0.0, 4.0);
        l.color = {0x2e, 0x2e, 0x33, 255};
        l.bold = pop >= 13;
        l.rank = 100 + pop;
    } else if (kind == "neighbourhood" || kind == "macrohood" || kind == "suburb") {
        if (zoom < 12) return;
        l.size = 9.5;
        l.color = {0x6a, 0x66, 0x6e, 255};
        l.rank = 10 + pop;
    } else {
        return;
    }
    labels.push_back(std::move(l));
}

}  // namespace

Color vectorWaterColor() { return kWater; }

void styleVectorTile(const std::vector<MvtLayer>& layers, double zoom, const TileToPixels& toPx, int W, int H, std::vector<StyledPath>& paths,
                     std::vector<StyledLabel>& labels) {
    for (const auto& layer : layers) {
        const std::string& name = layer.name;
        const double e = layer.extent;
        // tile units -> pixels, normalised to a 4096 extent so one transform serves every layer
        const TileToPixels px = [&toPx, e](double x, double y) { return toPx(x * 4096.0 / e, y * 4096.0 / e); };
        for (const auto& f : layer.features) {
            const std::string kind = kindOf(f);
            if (name == "earth") {
                fillFeature(f, kEarth, 10, px, W, H, paths);
            } else if (name == "landcover" || name == "landuse" || name == "natural") {
                if (auto c = landColor(kind)) fillFeature(f, *c, name == "landcover" ? 20 : 25, px, W, H, paths);
            } else if (name == "water") {
                fillFeature(f, kWater, 30, px, W, H, paths);
                const bool big = kind == "river" || kind == "canal";
                if (big || zoom >= 12) strokeFeature(f, kWater, scaled(big ? 3 : 1.2, big ? 0.8 : 0.5, zoom), "", 31, px, W, H, paths);
            } else if (name == "buildings" || name == "building") {
                if (zoom >= 13) fillFeature(f, {0xdc, 0xd4, 0xcb, 255}, 35, px, W, H, paths);
            } else if (name == "roads" || name == "transit") {
                const auto st = roadStyle(kind, name, zoom);
                if (!st) continue;
                const double w = scaled(st->base, st->minWidth, zoom);
                if (st->casing.a && w >= 1.6) strokeFeature(f, st->casing, w + 1.4, "", 40, px, W, H, paths);
                strokeFeature(f, st->fill, w, st->dash, st->order, px, W, H, paths);
            } else if (name == "boundaries") {
                const auto mar = f.props.find("maritime");
                if (mar != f.props.end() && mar->second.truthy()) continue;
                const double level = numberOf(f, "kind_detail", kind == "country" ? 2 : 4);
                if (kind == "country" || level <= 2) strokeFeature(f, {0x96, 0x8c, 0xa6, 255}, zoom < 5 ? 1 : 1.4, "dash", 60, px, W, H, paths);
                else if (zoom >= 4) strokeFeature(f, {0xb8, 0xb0, 0xc4, 255}, 0.8, "dash", 59, px, W, H, paths);
            } else if (name == "places") {
                placeLabel(f, zoom, px, W, H, labels);
            }
        }
    }
}

}  // namespace wetwaresuite::maps::detail
