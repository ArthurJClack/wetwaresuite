// Crossfades between clips, markers and regions (on audio documents and
// projects), and batch exports: every region to its own file, every track of
// a project as a stem.
#include <algorithm>
#include <cmath>

#include "common.hpp"

namespace wetwaresuite::audio::tool {

namespace {

ParamSpec anyDocParam() { return {"doc", ParamType::String, "an open audio document (aud1) or project (proj1)", true}; }

std::vector<ParamSpec> exportParams() {
    return {{"to", ParamType::String, "file format (wav built in; others need ffmpeg)", false, {"wav", "flac", "mp3", "ogg", "opus", "m4a", "aiff"}, std::string("wav")},
            {"bits", ParamType::String, "WAV/FLAC sample format", false, {"8", "16", "24", "32", "32f"}, std::string("16")},
            {"bitrate", ParamType::Int, "kbit/s for lossy formats (0 = default)", false, {}, std::int64_t{0}},
            {"rate", ParamType::Int, "resample to this sample rate (default: unchanged)", false},
            {"overwrite", ParamType::Bool, "replace files that exist", false, {}, false}};
}

// Checks format, sample rate and the folder; makes the folder.
bool exportSetup(const Params& p, fs::path& dir, std::string& ext, SaveOptions& o, int& rate, std::string& error) {
    dir = stringParam(p, "folder");
    ext = stringParam(p, "to", "wav");
    if (!canWrite("x." + ext)) return error = "saving ." + ext + " needs ffmpeg on PATH (wav always works)", false;
    if (!saveOptionsArg(p, o, error)) return false;
    rate = static_cast<int>(intParam(p, "rate", 0));
    if (p.count("rate") && (rate < 1000 || rate > 768000)) return error = "rate must be 1000..768000 Hz", false;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return error = "can't make " + dir.string() + ": " + ec.message(), false;
    return true;
}

std::string safeName(std::string s) {
    for (auto& c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) c = '_';
    }
    return s.empty() ? std::string("part") : s;
}

// Writes `b` (resampled when asked) and adds a line to `lines`; false when it exists and overwrite is off.
bool writeOne(const Buffer& b, const fs::path& path, const SaveOptions& o, int rate, bool overwrite, std::string& lines) {
    std::error_code ec;
    if (fs::exists(path, ec) && !overwrite) return false;
    save(rate > 0 && rate != b.rate ? resample(b, rate) : b, path, o);
    lines += "\n  " + path.string() + " (" + formatTime(b.seconds()) + ")";
    return true;
}

// Edits the markers of an audio document or a project, with undo.
ActionFactory markerCommand(Session& session, std::function<Result(std::vector<Marker>&, double, const Params&)> fn, bool undoable) {
    return [&session, fn, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().get(id);
        auto run = [fn, p](std::vector<Marker>& m, double len) {
            try {
                return fn(m, len, p);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        };
        if (auto a = std::dynamic_pointer_cast<AudioDoc>(doc)) {
            if (!undoable) return std::make_unique<FunctionAction>([a, run] { return run(a->markers, a->audio.seconds()); });
            return std::make_unique<AudioEdit>(a, [run](AudioDoc& d) { return run(d.markers, d.audio.seconds()); });
        }
        if (auto pr = std::dynamic_pointer_cast<ProjectDoc>(doc)) {
            if (!undoable) return std::make_unique<FunctionAction>([pr, run] { return run(pr->project.markers, pr->project.seconds()); });
            return std::make_unique<ProjectEdit>(pr, [run](ProjectDoc& d) { return run(d.project.markers, d.project.seconds()); });
        }
        return std::make_unique<FunctionAction>([id] { return Result::failure("no open audio document or project called '" + id + "'"); });
    };
}

std::string markerLine(std::size_t i, const Marker& m) {
    return std::to_string(i) + " \"" + m.name + "\" " + (m.region() ? formatTime(m.start) + " to " + formatTime(m.end) : "at " + formatTime(m.start));
}

void sortMarkers(std::vector<Marker>& m) {
    std::stable_sort(m.begin(), m.end(), [](const Marker& a, const Marker& b) { return a.start < b.start; });
}

// The parts to export: the regions, or else the stretches between markers.
std::vector<Marker> partsOf(const std::vector<Marker>& markers, double length) {
    std::vector<Marker> regions;
    for (const auto& m : markers)
        if (m.region()) regions.push_back(m);
    if (!regions.empty()) return regions;
    std::vector<Marker> cuts = markers;
    sortMarkers(cuts);
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        const double end = i + 1 < cuts.size() ? cuts[i + 1].start : length;
        if (end - cuts[i].start > 1e-3) regions.push_back({cuts[i].name, cuts[i].start, end});
    }
    return regions;
}

}  // namespace

void registerMixCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"audio.crossfade",
             "Crossfade",
             "Crossfade a clip into the next clip on its track: the first fades out while the second fades in over the "
             "part where they overlap. With seconds= the second clip is first slid left so they overlap by that much. "
             "curve=equal_power (default) keeps the level steady for different material, linear suits two takes of the same sound.",
             {projectParam(), {"track", ParamType::Int, "track number, from 0", true}, {"clip", ParamType::Int, "the first clip, from 0", true},
              {"seconds", ParamType::Number, "overlap to make, seconds (default: the overlap there is)", false},
              {"curve", ParamType::String, "fade shape", false, fadeCurveNames(), std::string("equal_power")}, previewParam()},
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                const auto ti = intParam(p, "track", -1), ci = intParam(p, "clip", -1);
                if (ti < 0 || ti >= static_cast<std::int64_t>(d.project.tracks.size())) return failed("no track " + std::to_string(ti));
                Track& t = d.project.tracks[static_cast<std::size_t>(ti)];
                if (ci < 0 || ci >= static_cast<std::int64_t>(t.clips.size())) return failed("no clip " + std::to_string(ci) + " on that track");
                FadeCurve curve = FadeCurve::EqualPower;
                if (!parseFadeCurve(stringParam(p, "curve", "equal_power"), curve)) return failed("unknown curve");
                Clip& a = t.clips[static_cast<std::size_t>(ci)];
                // the next clip by start time
                Clip* b = nullptr;
                std::int64_t bi = -1;
                for (std::size_t k = 0; k < t.clips.size(); ++k) {
                    if (static_cast<std::int64_t>(k) == ci || t.clips[k].at < a.at - 1e-9) continue;
                    if (t.clips[k].at == a.at && static_cast<std::int64_t>(k) < ci) continue;
                    if (!b || t.clips[k].at < b->at) b = &t.clips[k], bi = static_cast<std::int64_t>(k);
                }
                if (!b) return failed("clip " + std::to_string(ci) + " is the last on its track: nothing to crossfade into");
                if (p.count("seconds")) {
                    const double want = numberParam(p, "seconds");
                    if (want <= 0) return failed("seconds must be more than 0");
                    if (want >= std::min(a.seconds(), b->seconds())) return failed("the crossfade must be shorter than both clips");
                    b->at = std::max(a.at, a.end() - want);
                }
                const double overlap = a.end() - b->at;
                if (overlap <= 1e-4) return failed("the clips don't overlap; give seconds= to slide the second one under the first");
                const double len = std::min({overlap, a.seconds(), b->seconds()});
                a.fadeOut = len, a.fadeOutCurve = curve;
                b->fadeIn = len, b->fadeInCurve = curve;
                Result r = Result::success("crossfaded clip " + std::to_string(ci) + " into clip " + std::to_string(bi) + " over " + formatTime(len) + " (" +
                                           fadeCurveNames()[static_cast<std::size_t>(curve)] + ")");
                r.with("project", stringParam(p, "project")).with("seconds", len).with("next_clip", bi).with("next_at", b->at);
                return maybePreview(r, d, p);
            }));

    reg.add({"audio.marker_add",
             "Add marker",
             "Mark a point (at=) or a region (at= to end=) on an audio document or project, with a name: chapters, "
             "takes, songs on a long recording. audio.export_regions writes each region (or each stretch between markers) to its own file.",
             {anyDocParam(), {"name", ParamType::String, "marker name", false}, {"at", ParamType::Number, "seconds", true},
              {"end", ParamType::Number, "end of a region, seconds", false}},
             true},
            markerCommand(
                session,
                [](std::vector<Marker>& m, double len, const Params& p) {
                    Marker mk;
                    mk.start = numberParam(p, "at");
                    mk.end = p.count("end") ? numberParam(p, "end") : -1;
                    mk.name = stringParam(p, "name", (p.count("end") ? "region " : "marker ") + std::to_string(m.size() + 1));
                    if (mk.start < 0 || mk.start > len + 1e-6) return failed("at must be within 0.." + formatTime(len));
                    if (p.count("end") && (mk.end <= mk.start || mk.end > len + 1e-6)) return failed("end must be after at and within " + formatTime(len));
                    m.push_back(mk);
                    sortMarkers(m);
                    std::int64_t index = 0;
                    for (std::size_t i = 0; i < m.size(); ++i)
                        if (m[i].name == mk.name && m[i].start == mk.start) index = static_cast<std::int64_t>(i);
                    return Result::success("added " + markerLine(static_cast<std::size_t>(index), mk)).with("marker", index).with("markers", static_cast<std::int64_t>(m.size()));
                },
                true));

    reg.add({"audio.marker_remove",
             "Remove marker",
             "Delete a marker or region by number (marker=) or name (name=); all=true removes every one.",
             {anyDocParam(), {"marker", ParamType::Int, "marker number, from 0", false}, {"name", ParamType::String, "or its name", false},
              {"all", ParamType::Bool, "remove them all", false, {}, false}},
             true},
            markerCommand(
                session,
                [](std::vector<Marker>& m, double, const Params& p) {
                    if (boolParam(p, "all", false)) {
                        const std::size_t n = m.size();
                        m.clear();
                        return Result::success("removed " + std::to_string(n) + " markers");
                    }
                    std::int64_t i = intParam(p, "marker", -1);
                    if (p.count("name")) {
                        i = -1;
                        for (std::size_t k = 0; k < m.size(); ++k)
                            if (m[k].name == stringParam(p, "name")) i = static_cast<std::int64_t>(k);
                        if (i < 0) return failed("no marker called \"" + stringParam(p, "name") + "\"");
                    }
                    if (i < 0 || i >= static_cast<std::int64_t>(m.size())) return failed("no marker " + std::to_string(i) + " (there are " + std::to_string(m.size()) + ")");
                    const std::string line = markerLine(static_cast<std::size_t>(i), m[static_cast<std::size_t>(i)]);
                    m.erase(m.begin() + i);
                    return Result::success("removed " + line);
                },
                true));

    reg.add({"audio.markers",
             "List markers",
             "List the markers and regions of an audio document or project. rows: number, name, start, end (-1 for a marker).",
             {anyDocParam()},
             false, true},
            markerCommand(
                session,
                [](std::vector<Marker>& m, double, const Params&) {
                    std::string text, rows;
                    for (std::size_t i = 0; i < m.size(); ++i) {
                        text += "\n  " + markerLine(i, m[i]);
                        rows += std::to_string(i) + "\t" + m[i].name + "\t" + num(m[i].start, 8) + "\t" + num(m[i].end, 8) + "\n";
                    }
                    return Result::success(m.empty() ? "no markers" : std::to_string(m.size()) + " markers:" + text)
                        .with("rows", rows)
                        .with("markers", static_cast<std::int64_t>(m.size()));
                },
                false));

    reg.add({"audio.export_regions",
             "Export regions",
             "Write every region of an audio document or project (its mix) to its own file in folder, named after the "
             "region; with no regions, every stretch from one marker to the next. Choose the format, bit depth and sample rate.",
             concat({anyDocParam(), {"folder", ParamType::String, "folder for the files", true},
                     {"prefix", ParamType::String, "put this before each file name", false}},
                    exportParams()),
             false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string id = stringParam(p, "doc");
                    auto doc = s->workspace().get(id);
                    Buffer whole;
                    std::vector<Marker> markers;
                    if (auto a = std::dynamic_pointer_cast<AudioDoc>(doc)) {
                        whole = a->audio, markers = a->markers;
                    } else if (auto pr = std::dynamic_pointer_cast<ProjectDoc>(doc)) {
                        if (pr->project.seconds() <= 0) return failed("the project has no clips yet");
                        whole = mixdown(pr->project), markers = pr->project.markers;
                    } else {
                        return failed("no open audio document or project called '" + id + "'");
                    }
                    const auto parts = partsOf(markers, whole.seconds());
                    if (parts.empty()) return failed(id + " has no regions or markers (add them with audio.marker_add)");
                    fs::path dir;
                    std::string ext, error, lines;
                    SaveOptions o;
                    int rate = 0;
                    if (!exportSetup(p, dir, ext, o, rate, error)) return failed(error);
                    int written = 0, skipped = 0;
                    for (std::size_t i = 0; i < parts.size(); ++i) {
                        char num3[8];
                        std::snprintf(num3, sizeof num3, "%02d", static_cast<int>(i + 1));
                        const fs::path path = dir / (stringParam(p, "prefix") + num3 + "_" + safeName(parts[i].name) + "." + ext);
                        const Buffer part = slice(whole, rangeOf(whole, parts[i].start, parts[i].end));
                        if (writeOne(part, path, o, rate, boolParam(p, "overwrite", false), lines)) ++written;
                        else ++skipped;
                    }
                    std::string msg = "wrote " + std::to_string(written) + " files to " + dir.string() + lines;
                    if (skipped) msg += "\n" + std::to_string(skipped) + " skipped because they exist (overwrite=true replaces them)";
                    return Result::success(msg).with("written", static_cast<std::int64_t>(written)).with("skipped", static_cast<std::int64_t>(skipped));
                });
            });

    reg.add({"audio.export_stems",
             "Export stems",
             "Write each track of a project to its own file in folder, exactly as it plays in the mix (effects, volume, "
             "automation and pan included, mute and solo ignored), all the same length so they line up. mix=true also "
             "writes the full mix.",
             concat({projectParam(), {"folder", ParamType::String, "folder for the stems", true},
                     {"mix", ParamType::Bool, "also write the full mix as mix.<format>", false, {}, false}},
                    exportParams()),
             false},
            projectCommand(
                session,
                [](ProjectDoc& d, const Params& p) {
                    if (d.project.tracks.empty() || d.project.seconds() <= 0) return failed("the project has no clips yet");
                    fs::path dir;
                    std::string ext, error, lines;
                    SaveOptions o;
                    int rate = 0;
                    if (!exportSetup(p, dir, ext, o, rate, error)) return failed(error);
                    int written = 0, skipped = 0;
                    for (std::size_t i = 0; i < d.project.tracks.size(); ++i) {
                        const Track& t = d.project.tracks[i];
                        char num3[8];
                        std::snprintf(num3, sizeof num3, "%02d", static_cast<int>(i + 1));
                        const fs::path path = dir / (std::string(num3) + "_" + safeName(t.name.empty() ? "track" : t.name) + "." + ext);
                        if (writeOne(renderTrack(d.project, static_cast<int>(i)), path, o, rate, boolParam(p, "overwrite", false), lines)) ++written;
                        else ++skipped;
                    }
                    if (boolParam(p, "mix", false)) {
                        if (writeOne(mixdown(d.project), dir / ("mix." + ext), o, rate, boolParam(p, "overwrite", false), lines)) ++written;
                        else ++skipped;
                    }
                    std::string msg = "wrote " + std::to_string(written) + " stems to " + dir.string() + lines;
                    if (skipped) msg += "\n" + std::to_string(skipped) + " skipped because they exist (overwrite=true replaces them)";
                    return Result::success(msg).with("written", static_cast<std::int64_t>(written)).with("skipped", static_cast<std::int64_t>(skipped));
                },
                false));
}

}  // namespace wetwaresuite::audio::tool
