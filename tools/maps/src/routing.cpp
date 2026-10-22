// Offline routing on a road network made from line features: car, bike and
// foot profiles from OSM-style tags, A* for routes through stops, turn-by-turn
// directions, and Dijkstra for everything reachable within a time.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wetwaresuite/maps/maps.hpp"

namespace wetwaresuite::maps {

namespace {

double haversine(const Coord& a, const Coord& b) {
    const double p1 = a.y * kDeg, p2 = b.y * kDeg, dp = p2 - p1, dl = (b.x - a.x) * kDeg;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kAuthalicRadius * std::asin(std::min(1.0, std::sqrt(h)));
}

std::string lowerText(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string tag(const Props& p, const char* key) {
    auto it = p.find(key);
    return it == p.end() || it->second.isNull() ? std::string() : lowerText(it->second.text());
}

// km/h for a highway type and profile; 0 = not allowed.
double baseSpeed(const std::string& hw, const std::string& profile) {
    struct Row {
        const char* hw;
        double car, bike, foot;
    };
    static const Row rows[] = {
        {"motorway", 110, 0, 0},       {"motorway_link", 60, 0, 0},  {"trunk", 90, 12, 4},          {"trunk_link", 50, 12, 4},
        {"primary", 70, 14, 4.5},      {"primary_link", 45, 14, 4.5}, {"secondary", 60, 15, 4.5},    {"secondary_link", 40, 15, 4.5},
        {"tertiary", 50, 16, 5},       {"tertiary_link", 35, 16, 5},  {"unclassified", 40, 16, 5},  {"residential", 30, 16, 5},
        {"living_street", 10, 12, 5},  {"service", 15, 14, 5},        {"road", 30, 14, 5},          {"track", 15, 12, 5},
        {"cycleway", 0, 18, 4.5},      {"path", 0, 14, 5},            {"footway", 0, 6, 5},         {"pedestrian", 0, 6, 5},
        {"bridleway", 0, 8, 4.5},      {"steps", 0, 2, 3.5},          {"corridor", 0, 0, 5},        {"platform", 0, 0, 5},
        {"busway", 0, 15, 0},          {"elevator", 0, 0, 1},
    };
    for (const auto& r : rows) {
        if (hw == r.hw) return profile == "car" ? r.car : profile == "bike" ? r.bike : r.foot;
    }
    return 0;  // construction, proposed, abandoned, raceway, ...
}

double parseMaxspeed(const std::string& v) {
    if (v.empty()) return 0;
    try {
        std::size_t used = 0;
        double s = std::stod(v, &used);
        if (v.find("mph") != std::string::npos) s *= 1.609344;
        return s > 0 && s < 300 ? s : 0;
    } catch (const std::exception&) {
        return 0;  // "signals", "walk", "none"
    }
}

bool denied(const std::string& v) { return v == "no" || v == "private"; }
bool allowed(const std::string& v) { return v == "yes" || v == "designated" || v == "permissive" || v == "destination"; }

struct Usage {
    double speed = 0;  // km/h, 0 = can't use
    int oneway = 0;    // 1 along the line, -1 against it, 0 both ways
    bool roundabout = false;
};

Usage usage(const Props& tags, const std::string& profile, bool network) {
    Usage u;
    const std::string hw = tag(tags, "highway");
    if (hw.empty()) {
        if (network) return u;  // a layer with highway tags: untagged lines aren't roads
        u.speed = profile == "car" ? 50 : profile == "bike" ? 15 : 5;
    } else {
        u.speed = baseSpeed(hw, profile);
    }
    // access: the profile's own tag wins over the general one
    std::vector<std::string> own;
    if (profile == "car") own = {tag(tags, "motorcar"), tag(tags, "motor_vehicle"), tag(tags, "vehicle")};
    else if (profile == "bike") own = {tag(tags, "bicycle"), tag(tags, "vehicle")};
    else own = {tag(tags, "foot")};
    std::string specific;
    for (const auto& v : own) {
        if (!v.empty()) {
            specific = v;
            break;
        }
    }
    if (!specific.empty()) {
        if (denied(specific)) return Usage{};
        if (allowed(specific) && u.speed == 0) u.speed = profile == "car" ? 20 : profile == "bike" ? 12 : 5;
    } else if (denied(tag(tags, "access"))) {
        return Usage{};
    }
    if (u.speed <= 0) return Usage{};
    if (profile == "car") {
        const double max = parseMaxspeed(tag(tags, "maxspeed"));
        if (max > 0) u.speed = max * 0.9;  // nobody drives at the limit the whole way
    }
    u.roundabout = tag(tags, "junction") == "roundabout" || tag(tags, "junction") == "circular";
    if (profile != "foot") {
        std::string ow = tag(tags, "oneway");
        if (profile == "bike") {
            const std::string b = tag(tags, "oneway:bicycle");
            if (!b.empty()) ow = b;
            if (tag(tags, "cycleway") == "opposite" || tag(tags, "cycleway") == "opposite_lane") ow = "no";
        }
        if (ow == "yes" || ow == "true" || ow == "1") u.oneway = 1;
        else if (ow == "-1" || ow == "reverse") u.oneway = -1;
        else if (ow.empty() && (u.roundabout || hw == "motorway" || hw == "motorway_link")) u.oneway = 1;
    }
    return u;
}

std::string roadName(const Props& tags) {
    for (const char* k : {"name", "ref", "NAME", "Name"}) {
        auto it = tags.find(k);
        if (it != tags.end() && !it->second.isNull() && !it->second.text().empty()) return it->second.text();
    }
    const std::string hw = tag(tags, "highway");
    if (hw.empty()) return "the road";
    std::string kind = hw;
    std::replace(kind.begin(), kind.end(), '_', ' ');
    if (hw == "footway" || hw == "path" || hw == "cycleway" || hw == "steps" || hw == "track" || hw == "bridleway" || hw == "pedestrian") return "the " + kind;
    return "an unnamed " + kind + (kind.find("road") == std::string::npos ? " road" : "");
}

const char* compass(double az) {
    static const char* names[] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
    az = std::fmod(az + 360 + 22.5, 360.0);
    return names[static_cast<int>(az / 45) % 8];
}

double bearing(const Coord& a, const Coord& b) {
    const double p1 = a.y * kDeg, p2 = b.y * kDeg, dl = (b.x - a.x) * kDeg;
    const double y = std::sin(dl) * std::cos(p2), x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    return std::fmod(std::atan2(y, x) / kDeg + 360, 360.0);
}

Coord mix(const Coord& a, const Coord& b, double t) {
    Coord c;
    c.x = a.x + (b.x - a.x) * t;
    c.y = a.y + (b.y - a.y) * t;
    return c;
}

}  // namespace

RoadGraph::RoadGraph(const std::vector<Feature>& roads, const RouteOptions& options) : options_(options) {
    if (options_.profile != "car" && options_.profile != "bike" && options_.profile != "foot")
        throw std::runtime_error("profile must be car, bike or foot");
    bool network = false;
    for (const auto& f : roads) network = network || f.props.count("highway");
    // line ends that round to the same cell of `tolerance` metres are one node (at least 1e-7 degrees,
    // so both cell numbers fit in 32 bits)
    const double step = std::max(options_.tolerance / 111320.0, 1e-7);
    std::unordered_map<std::uint64_t, std::uint32_t> ids;
    auto node = [&](const Coord& c) {
        const auto kx = static_cast<std::uint64_t>(std::llround(c.x / step) + 2147483648LL);
        const auto ky = static_cast<std::uint64_t>(std::llround(c.y / step) + 2147483648LL);
        const std::uint64_t key = (kx << 32) | (ky & 0xFFFFFFFFull);
        auto [it, fresh] = ids.emplace(key, static_cast<std::uint32_t>(nodes_.size()));
        if (fresh) {
            nodes_.push_back({c.x, c.y});
            adj_.emplace_back();
        }
        return it->second;
    };
    roadNames_.resize(roads.size());
    roundabout_.assign(roads.size(), 0);
    roadSpeed_.assign(roads.size(), 1);
    double fastest = 0;
    for (std::size_t r = 0; r < roads.size(); ++r) {
        const Feature& f = roads[r];
        if (f.geom.type != GeomType::Line) continue;
        const Usage u = usage(f.props, options_.profile, network);
        if (u.speed <= 0) continue;
        roadNames_[r] = roadName(f.props);
        roundabout_[r] = u.roundabout ? 1 : 0;
        const double ms = u.speed / 3.6;
        roadSpeed_[r] = ms;
        fastest = std::max(fastest, ms);
        for (const auto& line : f.geom.lines) {
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                const std::uint32_t a = node(line[i]), b = node(line[i + 1]);
                if (a == b) continue;
                const double len = haversine(nodes_[a], nodes_[b]);
                const double cost = options_.shortest ? len : len / ms;
                Edge e{a, b, static_cast<std::uint32_t>(r), static_cast<float>(cost), static_cast<float>(len), u.oneway >= 0, u.oneway <= 0};
                if (e.forward) adj_[a].push_back({b, e.cost, e.length, e.road});
                if (e.backward) adj_[b].push_back({a, e.cost, e.length, e.road});
                edgeList_.push_back(e);
            }
        }
    }
    edges_ = edgeList_.size();
    maxSpeed_ = options_.shortest ? 1.0 : std::max(fastest, 0.1);
    if (edgeList_.empty()) {
        throw std::runtime_error("no usable roads for " + options_.profile +
                                 " in this layer (it needs lines; with OSM highway tags, footways don't count for cars and motorways not for bikes or walking)");
    }
}

RoadGraph::Snap RoadGraph::snap(const Coord& c) const {
    Snap best;
    best.distance = std::numeric_limits<double>::infinity();
    const double kx = std::cos(std::clamp(c.y, -89.0, 89.0) * kDeg);
    double bestD2 = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < edgeList_.size(); ++i) {
        const Coord& a = nodes_[edgeList_[i].a];
        const Coord& b = nodes_[edgeList_[i].b];
        const double ax = (a.x - c.x) * kx, ay = a.y - c.y, bx = (b.x - c.x) * kx, by = b.y - c.y;
        const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        double t = len2 > 0 ? -(ax * dx + ay * dy) / len2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        const double px = ax + t * dx, py = ay + t * dy, d2 = px * px + py * py;
        if (d2 < bestD2) {
            bestD2 = d2;
            best.edge = i;
            best.f = t;
        }
    }
    const Edge& e = edgeList_[best.edge];
    best.point = mix(nodes_[e.a], nodes_[e.b], best.f);
    best.distance = haversine(c, best.point);
    if (best.distance > options_.snapMax) {
        throw std::runtime_error("the nearest usable road is " + std::to_string(static_cast<long long>(std::lround(best.distance))) + " m from " +
                                 std::to_string(c.y).substr(0, 9) + "," + std::to_string(c.x).substr(0, 9) + " (more than snap=" +
                                 std::to_string(static_cast<long long>(options_.snapMax)) + " m)");
    }
    return best;
}

RoadGraph::Path RoadGraph::search(const Snap& from, const Snap& to) const {
    const std::uint32_t N = static_cast<std::uint32_t>(nodes_.size()), S = N, T = N + 1;
    const Edge& es = edgeList_[from.edge];
    const Edge& et = edgeList_[to.edge];
    // the stops sit part-way along their edges: extra arcs out of S and into T
    std::vector<Arc> fromS;
    if (es.forward) fromS.push_back({es.b, static_cast<float>(es.cost * (1 - from.f)), static_cast<float>(es.length * (1 - from.f)), es.road});
    if (es.backward) fromS.push_back({es.a, static_cast<float>(es.cost * from.f), static_cast<float>(es.length * from.f), es.road});
    if (from.edge == to.edge) {
        if (es.forward && from.f <= to.f) fromS.push_back({T, static_cast<float>(es.cost * (to.f - from.f)), static_cast<float>(es.length * (to.f - from.f)), es.road});
        if (es.backward && from.f >= to.f) fromS.push_back({T, static_cast<float>(es.cost * (from.f - to.f)), static_cast<float>(es.length * (from.f - to.f)), es.road});
    }
    std::unordered_map<std::uint32_t, std::vector<Arc>> intoT;
    if (et.forward) intoT[et.a].push_back({T, static_cast<float>(et.cost * to.f), static_cast<float>(et.length * to.f), et.road});
    if (et.backward) intoT[et.b].push_back({T, static_cast<float>(et.cost * (1 - to.f)), static_cast<float>(et.length * (1 - to.f)), et.road});

    auto at = [&](std::uint32_t n) -> const Coord& { return n == S ? from.point : n == T ? to.point : nodes_[n]; };
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> g(N + 2, inf);
    std::vector<std::uint32_t> prev(N + 2, std::numeric_limits<std::uint32_t>::max());
    std::vector<Arc> via(N + 2);
    using Item = std::pair<double, std::uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    auto h = [&](std::uint32_t n) { return haversine(at(n), to.point) / maxSpeed_; };
    g[S] = 0;
    open.push({h(S), S});
    while (!open.empty()) {
        const auto [fScore, n] = open.top();
        open.pop();
        if (n == T) break;
        if (fScore - h(n) > g[n] + 1e-6) continue;  // stale
        auto relax = [&](const Arc& a) {
            const double ng = g[n] + a.cost;
            if (ng < g[a.to]) {
                g[a.to] = ng;
                prev[a.to] = n;
                via[a.to] = a;
                open.push({ng + h(a.to), a.to});
            }
        };
        if (n == S) {
            for (const auto& a : fromS) relax(a);
        } else {
            for (const auto& a : adj_[n]) relax(a);
            if (auto it = intoT.find(n); it != intoT.end()) {
                for (const auto& a : it->second) relax(a);
            }
        }
    }
    if (g[T] == inf) {
        throw std::runtime_error("no " + options_.profile + " route between " + std::to_string(from.point.y).substr(0, 9) + "," +
                                 std::to_string(from.point.x).substr(0, 9) + " and " + std::to_string(to.point.y).substr(0, 9) + "," +
                                 std::to_string(to.point.x).substr(0, 9) +
                                 ": the roads there don't connect (one-way streets, a gap in the data, or roads this profile can't use)");
    }
    Path p;
    std::vector<std::uint32_t> chain;
    for (std::uint32_t n = T; n != S; n = prev[n]) chain.push_back(n);
    std::reverse(chain.begin(), chain.end());
    p.points.push_back(from.point);
    for (std::uint32_t n : chain) {
        p.points.push_back(at(n));
        p.roads.push_back(via[n].road);
        p.lengths.push_back(via[n].length);
        p.costs.push_back(via[n].cost);
    }
    return p;
}

Route RoadGraph::route(const std::vector<Coord>& stops) const {
    if (stops.size() < 2) throw std::runtime_error("a route needs a start and an end");
    std::vector<Snap> snaps;
    Route r;
    for (const auto& s : stops) {
        snaps.push_back(snap(s));
        r.snapped.push_back(snaps.back().distance);
    }
    // the whole way as steps of (road, length, seconds) between points
    std::vector<Coord> pts;
    std::vector<std::uint32_t> roads;
    std::vector<double> lens, secs;
    std::vector<std::size_t> stopAt;  // step index where each via stop is reached
    for (std::size_t i = 0; i + 1 < snaps.size(); ++i) {
        Path leg = search(snaps[i], snaps[i + 1]);
        if (pts.empty()) pts.push_back(leg.points.front());
        for (std::size_t k = 0; k < leg.roads.size(); ++k) {
            pts.push_back(leg.points[k + 1]);
            roads.push_back(leg.roads[k]);
            lens.push_back(leg.lengths[k]);
            // seconds even for the shortest route, from the road's speed for this profile
            secs.push_back(options_.shortest ? leg.lengths[k] / roadSpeed_[leg.roads[k]] : leg.costs[k]);
        }
        stopAt.push_back(roads.size());
    }
    for (const auto& c : pts) {
        if (r.line.empty() || r.line.back().x != c.x || r.line.back().y != c.y) r.line.push_back(c);
    }
    for (double l : lens) r.distance += l;
    for (double s : secs) r.duration += s;

    // directions: one step per stretch of the same road name
    std::size_t k = 0, nextStop = 0;
    double prevBearing = 0;
    bool first = true;
    while (k < roads.size()) {
        if (lens[k] <= 0.01 && k + 1 < roads.size()) {  // zero-length joins at stops
            ++k;
            continue;
        }
        const std::string name = roadNames_[roads[k]];
        const bool round = roundabout_[roads[k]] != 0;
        RouteStep st;
        st.road = name;
        st.at = pts[k];
        const double inBearing = bearing(pts[k], pts[k + 1]);
        if (first) {
            st.instruction = "Head " + std::string(compass(inBearing)) + " on " + name;
        } else if (round) {
            st.instruction = "Enter the roundabout";
        } else {
            double turn = std::fmod(inBearing - prevBearing + 540, 360.0) - 180;
            const std::string side = turn < 0 ? "left" : "right";
            turn = std::abs(turn);
            if (turn < 25) st.instruction = "Continue onto " + name;
            else if (turn < 60) st.instruction = "Bear " + side + " onto " + name;
            else if (turn < 140) st.instruction = "Turn " + side + " onto " + name;
            else st.instruction = "Turn sharp " + side + " onto " + name;
        }
        std::size_t j = k;
        while (j < roads.size() && roadNames_[roads[j]] == name && (roundabout_[roads[j]] != 0) == round) {
            if (nextStop + 1 < stopAt.size() && j == stopAt[nextStop] && j != k) break;
            st.distance += lens[j];
            st.duration += secs[j];
            ++j;
        }
        if (j == k) {  // a via stop right at the start of this stretch
            ++j;
        }
        prevBearing = bearing(pts[j - 1], pts[j]);
        r.steps.push_back(st);
        first = false;
        while (nextStop + 1 < stopAt.size() && j >= stopAt[nextStop]) {
            RouteStep via;
            via.instruction = "Reach stop " + std::to_string(nextStop + 1);
            via.at = pts[stopAt[nextStop]];
            r.steps.push_back(via);
            ++nextStop;
        }
        k = j;
    }
    RouteStep arrive;
    arrive.instruction = "Arrive at your destination";
    arrive.at = r.line.empty() ? stops.back() : r.line.back();
    r.steps.push_back(arrive);
    return r;
}

std::vector<Line> RoadGraph::reachable(const Coord& c, double limit) const {
    const Snap s = snap(c);
    const std::uint32_t N = static_cast<std::uint32_t>(nodes_.size());
    const Edge& e = edgeList_[s.edge];
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> g(N, inf);
    using Item = std::pair<double, std::uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    std::vector<Line> out;
    // the stop's own edge, both ways as far as allowed
    auto partial = [&](const Coord& a, const Coord& b, double cost, double budget) {
        if (budget <= 0) return;
        const double t = cost > 0 ? std::min(1.0, budget / cost) : 1.0;
        out.push_back({a, mix(a, b, t)});
    };
    if (e.forward) {
        const double cost = e.cost * (1 - s.f);
        partial(s.point, nodes_[e.b], cost, limit);
        if (cost <= limit) {
            g[e.b] = cost;
            open.push({cost, e.b});
        }
    }
    if (e.backward) {
        const double cost = e.cost * s.f;
        partial(s.point, nodes_[e.a], cost, limit);
        if (cost <= limit && cost < g[e.a]) {
            g[e.a] = cost;
            open.push({cost, e.a});
        }
    }
    while (!open.empty()) {
        const auto [d, n] = open.top();
        open.pop();
        if (d > g[n]) continue;
        for (const auto& a : adj_[n]) {
            const double nd = d + a.cost;
            if (nd <= limit) {
                out.push_back({nodes_[n], nodes_[a.to]});
                if (nd < g[a.to]) {
                    g[a.to] = nd;
                    open.push({nd, a.to});
                }
            } else {
                partial(nodes_[n], nodes_[a.to], a.cost, limit - d);
            }
        }
    }
    return out;
}

}  // namespace wetwaresuite::maps
