// OpenStreetMap XML (.osm) files: roads, other lines, areas (closed ways and
// multipolygon relations) and named places, with every tag as a property.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "internal.hpp"

namespace wetwaresuite::maps {

namespace {

using detail::XmlNode;

const std::set<std::string>& areaKeys() {
    static const std::set<std::string> k = {"building", "landuse", "leisure", "amenity", "shop", "tourism", "man_made", "water",
                                            "place", "aeroway", "military", "historic", "office", "parking", "landcover", "golf", "sport"};
    return k;
}

bool isArea(const Props& tags) {
    if (auto it = tags.find("area"); it != tags.end()) return it->second.text() != "no";
    for (const auto& [k, v] : tags) {
        if (areaKeys().count(k)) return true;
        if (k == "natural") return v.text() != "coastline" && v.text() != "tree_row" && v.text() != "cliff" && v.text() != "ridge";
        if (k == "waterway") return v.text() == "riverbank" || v.text() == "dock" || v.text() == "boatyard";
    }
    return false;
}

bool isPlace(const Props& tags) {
    for (const char* k : {"name", "amenity", "shop", "tourism", "place", "historic", "leisure", "office", "craft", "healthcare", "public_transport"}) {
        if (tags.count(k)) return true;
    }
    if (auto it = tags.find("railway"); it != tags.end() && (it->second.text() == "station" || it->second.text() == "halt")) return true;
    return false;
}

Props tagsOf(const XmlNode& n) {
    Props p;
    for (const XmlNode* t : n.all("tag")) {
        const std::string k = t->attr("k");
        if (k.empty() || k == "created_by") continue;
        const std::string v = t->attr("v");
        // names, refs and codes stay text even when they look like numbers
        const bool text = k == "name" || k.rfind("name:", 0) == 0 || k == "ref" || k.find("addr:") == 0 || k == "phone" || k == "opening_hours";
        p[k] = text ? Prop::string(v) : Prop::infer(v);
    }
    return p;
}

// Chains way node lists into closed rings by their shared end nodes.
std::vector<std::vector<std::int64_t>> closeRings(std::vector<std::vector<std::int64_t>> parts) {
    std::vector<std::vector<std::int64_t>> rings;
    while (!parts.empty()) {
        std::vector<std::int64_t> cur = std::move(parts.back());
        parts.pop_back();
        bool grew = true;
        while (cur.size() > 1 && cur.front() != cur.back() && grew) {
            grew = false;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                auto& p = parts[i];
                if (p.empty()) continue;
                if (p.front() == cur.back()) cur.insert(cur.end(), p.begin() + 1, p.end());
                else if (p.back() == cur.back()) cur.insert(cur.end(), p.rbegin() + 1, p.rend());
                else continue;
                parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(i));
                grew = true;
                break;
            }
        }
        if (cur.size() >= 4 && cur.front() == cur.back()) rings.push_back(std::move(cur));
    }
    return rings;
}

}  // namespace

std::vector<Layer> parseOsmXml(const std::string& text, const std::string& name, std::vector<std::string>* warnings) {
    auto doc = detail::parseXml(text);
    const XmlNode* root = doc ? doc->child("osm") : nullptr;
    if (!root) throw std::runtime_error("not an OpenStreetMap XML file (no <osm> element)");
    std::unordered_map<std::int64_t, Coord> nodes;
    std::vector<Feature> places, roads, lines, areas;
    for (const XmlNode* n : root->all("node")) {
        try {
            const std::int64_t id = std::stoll(n->attr("id"));
            Coord c{std::stod(n->attr("lon")), std::stod(n->attr("lat"))};
            nodes[id] = c;
            Props tags = tagsOf(*n);
            if (!tags.empty() && isPlace(tags)) {
                Feature f;
                f.geom = Geometry::point(c.x, c.y);
                tags["osm_id"] = Prop::number(static_cast<double>(id));
                f.props = std::move(tags);
                places.push_back(std::move(f));
            }
        } catch (const std::exception&) {
            // a node without coordinates (deleted, or a diff file): skip it
        }
    }
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> wayNodes;
    std::size_t missing = 0;
    for (const XmlNode* w : root->all("way")) {
        std::int64_t id = 0;
        try {
            id = std::stoll(w->attr("id"));
        } catch (const std::exception&) {
            continue;
        }
        std::vector<std::int64_t> refs;
        Line line;
        for (const XmlNode* nd : w->all("nd")) {
            try {
                const std::int64_t ref = std::stoll(nd->attr("ref"));
                auto it = nodes.find(ref);
                if (it == nodes.end()) {
                    ++missing;
                    continue;
                }
                refs.push_back(ref);
                line.push_back(it->second);
            } catch (const std::exception&) {
            }
        }
        wayNodes[id] = refs;
        if (line.size() < 2) continue;
        Props tags = tagsOf(*w);
        if (tags.empty()) continue;  // a part of a relation only
        tags["osm_id"] = Prop::number(static_cast<double>(id));
        const bool closed = refs.size() >= 4 && refs.front() == refs.back();
        Feature f;
        if (closed && isArea(tags) && !(tags.count("highway") && !tags.count("area"))) {
            line.pop_back();
            f.geom = Geometry::polygon({line});
            f.props = std::move(tags);
            areas.push_back(std::move(f));
        } else {
            f.geom = Geometry::line(std::move(line));
            const bool road = tags.count("highway") != 0;
            f.props = std::move(tags);
            (road ? roads : lines).push_back(std::move(f));
        }
    }
    std::size_t skippedRelations = 0;
    for (const XmlNode* r : root->all("relation")) {
        Props tags = tagsOf(*r);
        const std::string type = tags.count("type") ? tags["type"].text() : "";
        if (type != "multipolygon" && type != "boundary") {
            ++skippedRelations;
            continue;
        }
        std::vector<std::vector<std::int64_t>> parts;
        for (const XmlNode* m : r->all("member")) {
            if (m->attr("type") != "way") continue;
            try {
                auto it = wayNodes.find(std::stoll(m->attr("ref")));
                if (it != wayNodes.end() && it->second.size() >= 2) parts.push_back(it->second);
            } catch (const std::exception&) {
            }
        }
        std::vector<Ring> rings;
        for (const auto& ids : closeRings(std::move(parts))) {
            Ring ring;
            for (std::size_t i = 0; i + 1 < ids.size(); ++i) ring.push_back(nodes[ids[i]]);
            rings.push_back(std::move(ring));
        }
        auto polys = detail::assembleRings(std::move(rings));
        if (polys.empty()) {
            ++skippedRelations;
            continue;
        }
        Feature f;
        f.geom.type = GeomType::Polygon;
        f.geom.polygons = std::move(polys);
        try {
            tags["osm_id"] = Prop::number(static_cast<double>(std::stoll(r->attr("id"))));
        } catch (const std::exception&) {
        }
        f.props = std::move(tags);
        areas.push_back(std::move(f));
    }
    if (warnings) {
        if (missing) warnings->push_back(std::to_string(missing) + " way points refer to nodes outside the file (cut at the edge of the export)");
        if (skippedRelations) warnings->push_back(std::to_string(skippedRelations) + " relations (routes, restrictions, incomplete areas) were not imported");
    }
    std::vector<Layer> out;
    auto make = [&](const std::string& suffix, std::vector<Feature> fs, auto styler) {
        if (fs.empty()) return;
        Layer l;
        l.name = name + " " + suffix;
        l.features = std::move(fs);
        styler(l.style);
        out.push_back(std::move(l));
    };
    make("areas", std::move(areas), [](Style& s) {
        s.fill = {0xc9, 0xd8, 0xb6, 255};
        s.stroke = {0x9a, 0xa8, 0x88, 255};
        s.width = 0.6;
        s.fillOpacity = 0.6;
    });
    make("lines", std::move(lines), [](Style& s) {
        s.stroke = {0x6a, 0x8f, 0xc8, 255};
        s.width = 1;
    });
    make("roads", std::move(roads), [](Style& s) {
        s.stroke = {0x80, 0x78, 0x70, 255};
        s.width = 1.6;
    });
    make("places", std::move(places), [](Style& s) {
        s.fill = {0xc0, 0x39, 0x2b, 255};
        s.stroke = {0xff, 0xff, 0xff, 255};
        s.pointSize = 6;
        s.width = 1;
    });
    if (out.empty()) throw std::runtime_error("the OSM file has no roads, areas or named places");
    return out;
}

}  // namespace wetwaresuite::maps
