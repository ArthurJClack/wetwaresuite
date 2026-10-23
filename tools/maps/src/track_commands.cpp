// GPS track tools: splits per kilometre, smoothing and spike removal,
// trimming, splitting at pauses, joining, reversing, and elevations from a DEM.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cmd.hpp"

namespace wetwaresuite::maps::cmd {

namespace {

using namespace detail;

std::string pace(double secondsPerKm) {
    if (!std::isfinite(secondsPerKm) || secondsPerKm <= 0) return "-";
    const auto s = static_cast<long long>(std::llround(secondsPerKm));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld", s / 60, s % 60);
    return buf;
}

// The line features a track command works on: index= or every line feature.
std::vector<std::size_t> trackFeatures(const Layer& l, const Params& p, std::string& error) {
    std::vector<std::size_t> out;
    if (has(p, "index")) {
        const std::int64_t i = intParam(p, "index");
        if (i < 0 || i >= static_cast<std::int64_t>(l.features.size())) {
            error = l.name + " has features 0.." + std::to_string(static_cast<std::int64_t>(l.features.size()) - 1);
            return {};
        }
        if (l.features[static_cast<std::size_t>(i)].geom.type != GeomType::Line) {
            error = "#" + std::to_string(i) + " of " + l.name + " is not a line";
            return {};
        }
        return {static_cast<std::size_t>(i)};
    }
    for (std::size_t i = 0; i < l.features.size(); ++i) {
        if (l.features[i].geom.type == GeomType::Line) out.push_back(i);
    }
    if (out.empty()) error = l.name + " has no lines (tracks)";
    return out;
}

ParamSpec trackLayerParam() { return layerParam("track layer (default: the top layer with lines)"); }
ParamSpec trackIndexParam() { return {"index", ParamType::Int, "one feature only (default: every line)", false}; }

void registerSplitsAndCleaning(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.track_splits", "Track Splits",
             "Split a track into equal stretches (every= metres, 1 km by default) and give each one's time, pace, speed and "
             "climb, like a running watch's laps. path= also writes them as a CSV.",
             {docParam(), trackLayerParam(), trackIndexParam(), {"every", ParamType::Number, "split length in metres", false, {}, 1000.0},
              {"hysteresis", ParamType::Number, "elevation change in metres to count as climbing", false, {}, 3.0},
              {"path", ParamType::String, "also write the splits to this CSV file", false}},
             false, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Line);
                if (!l) return Result::failure(error);
                const auto which = trackFeatures(*l, p, error);
                if (which.empty()) return Result::failure(error);
                std::vector<const Line*> lines;
                for (auto i : which) {
                    for (const auto& ln : l->features[i].geom.lines) lines.push_back(&ln);
                }
                const double every = numberParam(p, "every", 1000);
                const auto splits = trackSplits(lines, every, std::max(0.0, numberParam(p, "hysteresis", 3.0)));
                std::string m = l->name + ": " + plural(splits.size(), "split") + " of " + metres(every);
                std::string csv = "split,from_m,to_m,distance_m,time_s,pace_min_km,speed_kmh,gain_m,loss_m\n";
                double best = 1e300, worst = 0;
                std::size_t bestAt = 0, worstAt = 0;
                for (std::size_t k = 0; k < splits.size(); ++k) {
                    const Split& s = splits[k];
                    const bool timed = !std::isnan(s.duration) && s.distance > 0;
                    const double perKm = timed ? s.duration / (s.distance / 1000) : kNaN;
                    m += "\n" + std::to_string(k + 1) + ": " + metres(s.to);
                    if (timed) {
                        m += "  " + duration(s.duration) + "  " + pace(perKm) + " min/km  " + fmt(s.distance / s.duration * 3.6, 1) + " km/h";
                        if (s.distance >= every * 0.5) {  // a short last split isn't a fair comparison
                            if (perKm < best) best = perKm, bestAt = k;
                            if (perKm > worst) worst = perKm, worstAt = k;
                        }
                    }
                    if (s.gain > 0 || s.loss > 0) m += "  +" + fmt(s.gain, 0) + "/-" + fmt(s.loss, 0) + " m";
                    csv += std::to_string(k + 1) + "," + fmt(s.from, 1) + "," + fmt(s.to, 1) + "," + fmt(s.distance, 1) + "," + (timed ? fmt(s.duration, 1) : "") + "," +
                           (timed ? pace(perKm) : "") + "," + (timed ? fmt(s.distance / s.duration * 3.6, 2) : "") + "," + fmt(s.gain, 1) + "," + fmt(s.loss, 1) + "\n";
                }
                Result r = edited(m, p);
                if (best < 1e300) {
                    r.message += "\nfastest: split " + std::to_string(bestAt + 1) + " (" + pace(best) + " min/km), slowest: split " + std::to_string(worstAt + 1) + " (" + pace(worst) + " min/km)";
                    r.with("fastest", static_cast<std::int64_t>(bestAt + 1)).with("slowest", static_cast<std::int64_t>(worstAt + 1));
                }
                if (has(p, "path")) {
                    writeFile(stringParam(p, "path"), csv);
                    r.message += "\nwrote " + stringParam(p, "path");
                    r.with("path", stringParam(p, "path"));
                }
                return r.with("splits", static_cast<std::int64_t>(splits.size()));
            }, false));

    reg.add({"maps.track_smooth", "Smooth Track",
             "Clean a GPS track: drop jumps that would need more than max_kmh= to reach (needs times), then average each point "
             "with its neighbours over window= points (positions, elevations or both). Lowers noisy climb totals and zig-zags. "
             "In place, or into output=.",
             {docParam(), trackLayerParam(), trackIndexParam(), {"window", ParamType::Int, "points to average over (0 = no smoothing)", false, {}, std::int64_t{5}},
              {"what", ParamType::String, "what to smooth", false, {"both", "positions", "elevation"}, std::string("both")},
              {"max_kmh", ParamType::Number, "remove points needing more than this speed (0 = keep all)", false, {}, 0.0},
              outputParam("write a new layer instead of changing this one"), previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Line);
                if (!l) return Result::failure(error);
                const auto which = trackFeatures(*l, p, error);
                if (which.empty()) return Result::failure(error);
                const int window = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "window", 5), 0, 501));
                const std::string what = stringParam(p, "what", "both");
                const double maxMs = std::max(0.0, numberParam(p, "max_kmh", 0)) / 3.6;
                std::vector<std::string> warnings;
                Layer* target = l;
                if (has(p, "output")) target = &addLayer(doc.map, *l, stringParam(p, "output"));
                double gainBefore = 0, gainAfter = 0;
                std::size_t removed = 0;
                for (auto i : which) {
                    Geometry& g = target->features[i].geom;
                    std::vector<const Line*> before;
                    for (const auto& ln : g.lines) before.push_back(&ln);
                    gainBefore += trackStats(before, 0).gain;
                    for (auto& ln : g.lines) {
                        if (maxMs > 0) removed += removeSpikes(ln, maxMs);
                        if (window >= 2) ln = smoothLine(ln, window, what != "elevation", what != "positions");
                    }
                    std::vector<const Line*> after;
                    for (const auto& ln : g.lines) after.push_back(&ln);
                    gainAfter += trackStats(after, 0).gain;
                }
                std::string m = "smoothed " + plural(which.size(), "track") + " of " + target->name;
                if (removed) m += ", removed " + plural(removed, "jump");
                if (gainBefore > 0) m += "; raw climb " + fmt(gainBefore, 0) + " m -> " + fmt(gainAfter, 0) + " m";
                return edited(m, p).with("layer", target->name).with("removed", static_cast<std::int64_t>(removed)).with("gain_m", gainAfter);
            }));
}

void registerCutAndJoin(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.track_trim", "Trim Track",
             "Cut the ends off a track: keep from= to to= metres along it (to may be negative: metres before the end), or "
             "between start= and end= times (ISO 8601, e.g. 2026-07-12T09:00:00Z). Cuts fall between points, interpolated.",
             {docParam(), trackLayerParam(), {"index", ParamType::Int, "the track feature", false, {}, std::int64_t{0}},
              {"from", ParamType::Number, "metres from the start to keep from", false}, {"to", ParamType::Number, "metres from the start to keep to (negative = from the end)", false},
              {"start", ParamType::String, "keep from this time", false}, {"end", ParamType::String, "keep until this time", false}, previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Line);
                if (!l) return Result::failure(error);
                Params q = p;
                if (!has(q, "index")) q["index"] = std::int64_t{0};
                const auto which = trackFeatures(*l, q, error);
                if (which.empty()) return Result::failure(error);
                Geometry& g = l->features[which.front()].geom;
                if (g.lines.size() != 1) return Result::failure("trim works on single-part tracks (split this one with maps.track_split first)");
                Line& ln = g.lines[0];
                const double total = lineLength(ln);
                double from = numberParam(p, "from", 0), to = has(p, "to") ? numberParam(p, "to") : total;
                if (to < 0) to = total + to;
                if (has(p, "start") || has(p, "end")) {
                    if (has(p, "start")) {
                        const double t = parseTime(stringParam(p, "start"));
                        if (std::isnan(t)) return Result::failure("start isn't a time: " + stringParam(p, "start"));
                        from = distanceAtTime(ln, t);
                    }
                    if (has(p, "end")) {
                        const double t = parseTime(stringParam(p, "end"));
                        if (std::isnan(t)) return Result::failure("end isn't a time: " + stringParam(p, "end"));
                        to = distanceAtTime(ln, t);
                    }
                    if (std::isnan(from) || std::isnan(to)) return Result::failure("this track has no times: trim by from=/to= metres");
                }
                if (!(to > from)) return Result::failure("nothing left: to must be after from");
                Line cut = trimLine(ln, from, to);
                if (cut.size() < 2) return Result::failure("nothing left between those points");
                ln = std::move(cut);
                return edited("trimmed #" + std::to_string(which.front()) + " of " + l->name + " to " + metres(from) + " - " + metres(to) + ": " + metres(lineLength(ln)) + " (was " + metres(total) + ")", p)
                    .with("length_m", lineLength(ln));
            }));

    reg.add({"maps.track_split", "Split Track at Pauses",
             "Split tracks where the recording paused: gaps longer than gap_minutes= in time or gap_m= in distance start a new "
             "feature (each keeps the properties, plus part=1, 2, ...). Multi-part tracks are split into their parts too. To "
             "cut at one place use maps.split.",
             {docParam(), trackLayerParam(), trackIndexParam(), {"gap_minutes", ParamType::Number, "time gap that splits (0 = ignore time)", false, {}, 10.0},
              {"gap_m", ParamType::Number, "distance gap that splits (0 = ignore)", false, {}, 0.0}, previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Line);
                if (!l) return Result::failure(error);
                const auto which = trackFeatures(*l, p, error);
                if (which.empty()) return Result::failure(error);
                const double gapS = std::max(0.0, numberParam(p, "gap_minutes", 10)) * 60, gapM = std::max(0.0, numberParam(p, "gap_m", 0));
                std::vector<Feature> out;
                std::size_t made = 0, split = 0;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    Feature& f = l->features[i];
                    if (!std::binary_search(which.begin(), which.end(), i)) {
                        out.push_back(std::move(f));
                        continue;
                    }
                    std::vector<Line> parts;
                    for (const auto& ln : f.geom.lines) {
                        for (auto& piece : splitByGaps(ln, gapS, gapM)) parts.push_back(std::move(piece));
                    }
                    if (parts.size() <= 1) {
                        out.push_back(std::move(f));
                        continue;
                    }
                    ++split;
                    for (std::size_t k = 0; k < parts.size(); ++k) {
                        Feature nf{Geometry::line(std::move(parts[k])), f.props};
                        nf.props["part"] = Prop::number(static_cast<double>(k + 1));
                        out.push_back(std::move(nf));
                        ++made;
                    }
                }
                l->features = std::move(out);
                if (!split) return edited("no pauses longer than that in " + l->name + ": nothing split", p).with("parts", std::int64_t{0});
                return edited("split " + plural(split, "track") + " of " + l->name + " into " + plural(made, "part"), p).with("parts", static_cast<std::int64_t>(made));
            }));

    reg.add({"maps.track_join", "Join Tracks",
             "Join several tracks into one continuous line, in time order when they have times (else in the order given): "
             "indexes= or where=, or every line of the layer. The first one's properties are kept; the joined track replaces them.",
             {docParam(), trackLayerParam(), {"indexes", ParamType::NumberList, "the tracks to join", false}, whereParam("join every track matching"), previewParam()},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Line);
                if (!l) return Result::failure(error);
                std::vector<std::size_t> which;
                if (has(p, "indexes")) {
                    for (double v : listParam(p, "indexes")) {
                        const auto i = static_cast<std::int64_t>(std::llround(v));
                        if (i < 0 || i >= static_cast<std::int64_t>(l->features.size())) return Result::failure(l->name + " has features 0.." + std::to_string(l->features.size() - 1));
                        which.push_back(static_cast<std::size_t>(i));
                    }
                } else {
                    auto where = whereFrom(p, error);
                    if (!error.empty()) return Result::failure(error);
                    for (std::size_t i = 0; i < l->features.size(); ++i) {
                        if (l->features[i].geom.type == GeomType::Line && (!where || where->test(l->features[i], i))) which.push_back(i);
                    }
                }
                std::vector<Line> parts;
                for (auto i : which) {
                    if (l->features[i].geom.type != GeomType::Line) return Result::failure("#" + std::to_string(i) + " is not a line");
                    for (const auto& ln : l->features[i].geom.lines) parts.push_back(ln);
                }
                if (parts.size() < 2) return Result::failure("pick at least two tracks (or a multi-part one) to join");
                const bool timed = std::all_of(parts.begin(), parts.end(), [](const Line& ln) { return !ln.empty() && !std::isnan(ln.front().t); });
                if (timed) std::stable_sort(parts.begin(), parts.end(), [](const Line& a, const Line& b) { return a.front().t < b.front().t; });
                Line joined;
                for (const auto& ln : parts) {
                    for (const auto& c : ln) {
                        if (joined.empty() || joined.back().x != c.x || joined.back().y != c.y) joined.push_back(c);
                    }
                }
                const std::size_t first = *std::min_element(which.begin(), which.end());
                Feature merged{Geometry::line(std::move(joined)), l->features[first].props};
                merged.props.erase("part");
                std::vector<Feature> out;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    if (i == first) out.push_back(merged);
                    else if (std::find(which.begin(), which.end(), i) == which.end()) out.push_back(std::move(l->features[i]));
                }
                l->features = std::move(out);
                return edited("joined " + plural(parts.size(), "track part") + (timed ? " in time order" : "") + " into #" + std::to_string(first) + " of " + l->name + ": " +
                                  metres(length(merged.geom)),
                              p)
                    .with("index", static_cast<std::int64_t>(first))
                    .with("length_m", length(merged.geom));
            }));

    reg.add({"maps.track_reverse", "Reverse Track",
             "Reverse the direction of lines (to ride a route the other way). Times are dropped, since they would run backwards.",
             {docParam(), trackLayerParam(), trackIndexParam(), previewParam()}, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = layerWith(doc.map, p, error, GeomType::Line);
                if (!l) return Result::failure(error);
                const auto which = trackFeatures(*l, p, error);
                if (which.empty()) return Result::failure(error);
                for (auto i : which) {
                    auto& lines = l->features[i].geom.lines;
                    std::reverse(lines.begin(), lines.end());
                    for (auto& ln : lines) {
                        std::reverse(ln.begin(), ln.end());
                        for (auto& c : ln) c.t = kNaN;
                    }
                }
                return edited("reversed " + plural(which.size(), "line") + " of " + l->name, p);
            }));
}

void registerElevation(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.track_elevation", "Elevation from DEM",
             "Set the elevation of every vertex of a layer (tracks, routes, waypoints) from a raster layer of heights (a DEM "
             "GeoTIFF added with maps.raster_add), for files with no or poor GPS elevations. Points outside the raster keep "
             "theirs.",
             {docParam(), layerParam("the layer to fill (default: the top vector layer)"), {"raster", ParamType::String, "the elevation raster layer", true},
              trackIndexParam(), {"overwrite", ParamType::Bool, "replace elevations that are already there", false, {}, true}},
             true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* l = vectorLayer(doc.map, p, error);
                if (!l) return Result::failure(error);
                const Layer* dem = doc.map.layer(stringParam(p, "raster"));
                if (!dem || !dem->isRaster()) return Result::failure("no raster layer called '" + stringParam(p, "raster") + "'");
                if (!dem->grid->hasValues()) return Result::failure("'" + dem->name + "' is a picture, not a grid of heights");
                const bool overwrite = boolParam(p, "overwrite", true);
                std::size_t set = 0, outside = 0;
                for (std::size_t i = 0; i < l->features.size(); ++i) {
                    if (has(p, "index") && static_cast<std::int64_t>(i) != intParam(p, "index")) continue;
                    l->features[i].geom.forEach([&](Coord& c) {
                        if (!overwrite && !std::isnan(c.z)) return;
                        const double z = dem->grid->sample(c.x, c.y);
                        if (std::isnan(z)) {
                            ++outside;
                            return;
                        }
                        c.z = z;
                        ++set;
                    });
                }
                Result r = edited("set " + plural(set, "elevation") + " of " + l->name + " from " + dem->name, p);
                if (outside) r.message += "\n" + plural(outside, "point") + " fell outside the raster (or on nodata)";
                return r.with("set", static_cast<std::int64_t>(set)).with("outside", static_cast<std::int64_t>(outside));
            }));
}

}  // namespace

void registerTrackToolCommands(Session& session) {
    registerSplitsAndCleaning(session);
    registerCutAndJoin(session);
    registerElevation(session);
}

}  // namespace wetwaresuite::maps::cmd
