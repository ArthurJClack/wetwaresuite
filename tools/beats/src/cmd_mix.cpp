// Mixing a song: effects racks on tracks and the master (beats.fx_*),
// automation in beats, drum pads, the metronome, moving placements (what the
// arrangement timeline drags) and stems.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/audio/analysis.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/audio/project.hpp"
#include "common.hpp"

namespace wetwaresuite::beats::tool {

namespace {

namespace fs = std::filesystem;

struct RackRef {
    audio::Rack* rack = nullptr;
    std::vector<audio::Automation>* lanes = nullptr;
    std::string name;
};

std::vector<ParamSpec> ownerParams() {
    return {songParam(), trackParam(false), {"master", ParamType::Bool, "the master (the whole mix) instead of a track", false, {}, false}};
}

bool rackArg(Song& s, const Params& p, RackRef& out, std::string& error) {
    if (boolParam(p, "master", false)) {
        out = {&s.masterRack, &s.masterAutomation, "the master"};
        return true;
    }
    if (!p.count("track")) return error = "give track= (number or name) or master=true", false;
    int ti = 0;
    if (!trackArg(s, p, ti, error)) return false;
    Track& t = s.tracks[static_cast<std::size_t>(ti)];
    out = {&t.rack, &t.automation, "track " + std::to_string(ti) + " \"" + t.name + "\""};
    return true;
}

bool fxArg(const RackRef& r, const Params& p, std::size_t& out, std::string& error) {
    const auto i = intParam(p, "fx", -1);
    if (i < 0 || i >= static_cast<std::int64_t>(r.rack->size()))
        return error = "no effect " + std::to_string(i) + " on " + r.name + " (it has " + std::to_string(r.rack->size()) + ", numbered from 0)", false;
    out = static_cast<std::size_t>(i);
    return true;
}

std::string rackText(const audio::Rack& r) {
    if (r.empty()) return " (no effects)";
    std::string s;
    for (std::size_t i = 0; i < r.size(); ++i) s += "\n  " + std::to_string(i) + " " + audio::describeEffect(r[i]);
    return s;
}

std::string rackRows(const std::string& owner, const audio::Rack& rack) {
    std::string rows;
    for (std::size_t i = 0; i < rack.size(); ++i) {
        const audio::EffectType* t = audio::findEffectType(rack[i].type);
        if (!t) continue;
        for (const auto& prm : t->params) {
            std::string choices;
            for (const auto& c : prm.choices) choices += (choices.empty() ? "" : "|") + c;
            rows += owner + "\t" + std::to_string(i) + "\t" + t->name + "\t" + (rack[i].bypass ? "1" : "0") + "\t" + prm.name + "\t" + num(rack[i].get(prm.name), 8) +
                    "\t" + num(prm.min, 8) + "\t" + num(prm.max, 8) + "\t" + prm.unit + "\t" + choices + "\n";
        }
    }
    return rows;
}

ParamSpec fxParam() { return {"fx", ParamType::Int, "effect number in the rack, from 0", true}; }

std::string safeName(std::string s) {
    for (auto& c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) c = '_';
    }
    return s.empty() ? std::string("track") : s;
}

// Reordering a rack and reading automation back (the window's envelope editor polls this).
void registerLaneCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"beats.fx_move",
             "Move effect",
             "Change the order of a rack: move effect fx to position `to` (its automation follows it).",
             [] {
                 auto v = ownerParams();
                 v.push_back(fxParam());
                 v.push_back({"to", ParamType::Int, "new position, from 0", true});
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                RackRef r;
                std::size_t i = 0;
                std::string error;
                if (!rackArg(d.song, p, r, error) || !fxArg(r, p, i, error)) return failed(error);
                const auto to = intParam(p, "to");
                if (to < 0 || to >= static_cast<std::int64_t>(r.rack->size())) return failed("to must be 0.." + std::to_string(r.rack->size() - 1));
                const audio::Effect e = (*r.rack)[i];
                r.rack->erase(r.rack->begin() + static_cast<std::ptrdiff_t>(i));
                r.rack->insert(r.rack->begin() + to, e);
                audio::renumberLanes(*r.lanes, -1, -1, static_cast<int>(i), static_cast<int>(to));
                return songResult(r.name + " rack:" + rackText(*r.rack), d, p);
            }));

    reg.add({"beats.automation",
             "List automation",
             "Show the automation lanes of a track or the master (or of everything), times in beats. rows: owner (track "
             "number or master), target, beat, value. length is the song's length in beats.",
             ownerParams(),
             false, true},
            songCommand(
                session,
                [](SongDoc& d, const Params& p) {
                    std::string text, rows;
                    auto one = [&](const std::string& owner, const std::string& name, const std::vector<audio::Automation>& lanes) {
                        text += "\n" + name + ":";
                        if (lanes.empty()) text += " (no automation)";
                        for (const auto& l : lanes) {
                            text += "\n  " + l.target + ":";
                            for (const auto& pt : l.points) {
                                text += " " + num(pt.time, 4) + "=" + num(pt.value, 4);
                                rows += owner + "\t" + l.target + "\t" + num(pt.time, 8) + "\t" + num(pt.value, 8) + "\n";
                            }
                        }
                    };
                    if (p.count("track") || boolParam(p, "master", false)) {
                        RackRef r;
                        std::string error;
                        if (!rackArg(d.song, p, r, error)) return failed(error);
                        int ti = -1;
                        if (!boolParam(p, "master", false)) trackArg(d.song, p, ti, error);
                        one(ti >= 0 ? std::to_string(ti) : "master", r.name, *r.lanes);
                    } else {
                        for (std::size_t t = 0; t < d.song.tracks.size(); ++t)
                            one(std::to_string(t), "track " + std::to_string(t) + " \"" + d.song.tracks[t].name + "\"", d.song.tracks[t].automation);
                        one("master", "the master", d.song.masterAutomation);
                    }
                    Result r = Result::success("automation" + text);
                    r.with("song", stringParam(p, "song")).with("rows", rows).with("length", d.song.bars() * d.song.beatsInBar());
                    return r;
                },
                false));
}

}  // namespace

void registerMixCommands(Session& session) {
    registerLaneCommands(session);
    auto& reg = session.registry();

    reg.add({"beats.fx_add",
             "Add effect",
             "Add an effect to a track's rack (track=Bass) or the master (master=true), after the track's built-in effects "
             "and before its volume: eq, graphic_eq, filter, compressor, limiter, gate, expander, deesser, reverb, delay, "
             "chorus, flanger, phaser, tremolo, distortion or utility, with values=\"name=value ...\" (audio.fx_types lists "
             "the settings; the EQ also takes values=\"bands=highpass 40, peaking 900 -3 1.5\"). Returns its number.",
             [] {
                 auto v = ownerParams();
                 v.push_back({"type", ParamType::String, "effect", true, audio::effectTypeNames()});
                 v.push_back({"values", ParamType::String, "settings as name=value pairs", false});
                 v.push_back({"at", ParamType::Int, "position in the rack (default: the end)", false});
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                RackRef r;
                audio::Effect e;
                std::string error;
                if (!rackArg(d.song, p, r, error) || !audio::makeEffect(stringParam(p, "type"), stringParam(p, "values"), e, error)) return failed(error);
                if (r.rack->size() >= 32) return failed("a rack holds up to 32 effects");
                const auto at = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "at", static_cast<std::int64_t>(r.rack->size())), 0,
                                                                                  static_cast<std::int64_t>(r.rack->size())));
                r.rack->insert(r.rack->begin() + static_cast<std::ptrdiff_t>(at), e);
                if (at + 1 < r.rack->size()) audio::renumberLanes(*r.lanes, -1, static_cast<int>(at));
                Result res = songResult("added effect " + std::to_string(at) + " to " + r.name + ": " + audio::describeEffect(e), d, p);
                res.with("fx", static_cast<std::int64_t>(at));
                return res;
            }));

    reg.add({"beats.fx_set",
             "Effect settings",
             "Change an effect's settings (values=\"wet=0.4\") or bypass it (bypass=true).",
             [] {
                 auto v = ownerParams();
                 v.push_back(fxParam());
                 v.push_back({"values", ParamType::String, "settings to change, name=value pairs", false});
                 v.push_back({"bypass", ParamType::Bool, "switch it off without removing it", false});
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                RackRef r;
                std::size_t i = 0;
                std::string error;
                if (!rackArg(d.song, p, r, error) || !fxArg(r, p, i, error)) return failed(error);
                if (!p.count("values") && !p.count("bypass")) return failed("give values= and/or bypass=");
                audio::Effect& e = (*r.rack)[i];
                std::map<std::string, double> changed;
                if (!audio::parseEffectValues(e.type, stringParam(p, "values"), changed, error)) return failed(error);
                for (const auto& [k, v] : changed) e.params[k] = v;
                if (p.count("bypass")) e.bypass = boolParam(p, "bypass");
                return songResult(r.name + " effect " + std::to_string(i) + " now " + audio::describeEffect(e), d, p);
            }));

    reg.add({"beats.fx_remove",
             "Remove effect",
             "Take an effect out of a rack; its automation goes with it.",
             [] {
                 auto v = ownerParams();
                 v.push_back(fxParam());
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                RackRef r;
                std::size_t i = 0;
                std::string error;
                if (!rackArg(d.song, p, r, error) || !fxArg(r, p, i, error)) return failed(error);
                const std::string what = (*r.rack)[i].type;
                r.rack->erase(r.rack->begin() + static_cast<std::ptrdiff_t>(i));
                audio::renumberLanes(*r.lanes, static_cast<int>(i), -1);
                return songResult("removed " + what + " from " + r.name, d, p);
            }));

    reg.add({"beats.fx_list",
             "List effects",
             "Show the racks (one track's, the master's, or all). rows: owner (track number or master), fx, type, bypassed, "
             "setting, value, min, max, unit, choices.",
             ownerParams(),
             false, true},
            songCommand(
                session,
                [](SongDoc& d, const Params& p) {
                    std::string text, rows;
                    if (p.count("track") || boolParam(p, "master", false)) {
                        RackRef r;
                        std::string error;
                        if (!rackArg(d.song, p, r, error)) return failed(error);
                        int ti = -1;
                        if (!boolParam(p, "master", false)) trackArg(d.song, p, ti, error);
                        text = r.name + ":" + rackText(*r.rack);
                        rows = rackRows(ti >= 0 ? std::to_string(ti) : "master", *r.rack);
                    } else {
                        for (std::size_t t = 0; t < d.song.tracks.size(); ++t) {
                            text += "\ntrack " + std::to_string(t) + " \"" + d.song.tracks[t].name + "\":" + rackText(d.song.tracks[t].rack);
                            rows += rackRows(std::to_string(t), d.song.tracks[t].rack);
                        }
                        text += "\nthe master:" + rackText(d.song.masterRack);
                        rows += rackRows("master", d.song.masterRack);
                    }
                    Result r = Result::success("effects " + text);
                    r.with("song", stringParam(p, "song")).with("rows", rows);
                    return r;
                },
                false));

    reg.add({"beats.automate",
             "Automate",
             "Draw automation on a track or the master, in beats from the song start: target=volume (dB), pan (-1..1) or "
             "fx<number>.<setting> (fx0.cutoff), points=beat,value pairs (points=0,200,16,6000 opens a filter over 4 bars "
             "of 4/4). mode=add merges points in, mode=clear removes the lane.",
             [] {
                 auto v = ownerParams();
                 v.push_back({"target", ParamType::String, "volume, pan or fx<i>.<setting>", true});
                 v.push_back({"points", ParamType::NumberList, "beat,value pairs", false});
                 v.push_back({"mode", ParamType::String, "what to do with the lane", false, {"replace", "add", "clear"}, std::string("replace")});
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                RackRef r;
                std::string error;
                if (!rackArg(d.song, p, r, error)) return failed(error);
                const std::string target = stringParam(p, "target"), mode = stringParam(p, "mode", "replace");
                if (!audio::validTarget(target, *r.rack, error)) return failed(error);
                std::vector<audio::AutomationPoint> pts;
                if (mode != "clear") {
                    const NumberList v = listParam(p, "points");
                    if (v.empty() || v.size() % 2) return failed("points takes beat,value pairs, e.g. points=0,0,16,-12");
                    double lo = 0, hi = 0;
                    audio::targetRange(target, *r.rack, lo, hi);
                    for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
                        if (v[i] < 0) return failed("beats can't be negative");
                        if (v[i + 1] < lo - 1e-9 || v[i + 1] > hi + 1e-9) return failed(target + " values must be " + num(lo) + ".." + num(hi));
                        pts.push_back({v[i], v[i + 1]});
                    }
                    if (mode == "add") {
                        if (const audio::Automation* old = audio::findLane(*r.lanes, target))
                            for (const auto& o : old->points)
                                if (std::none_of(pts.begin(), pts.end(), [&](const audio::AutomationPoint& n) { return std::fabs(n.time - o.time) < 1e-6; })) pts.push_back(o);
                    }
                }
                audio::setLane(*r.lanes, target, pts);
                Result res = songResult(r.name + ": " + target + (pts.empty() ? " automation removed" : " automated with " + std::to_string(pts.size()) + " points"), d, p);
                res.with("points", static_cast<std::int64_t>(pts.size()));
                return res;
            }));

    reg.add({"beats.pad_set",
             "Drum pad",
             "Shape one lane of a drum track (a kit voice like snare, or a sample slot): volume in dB, tune in semitones "
             "(plays the hit faster or slower), decay = seconds to cut the hit down to (0 lets it ring).",
             {songParam(), trackParam(), {"lane", ParamType::String, "kit voice or slot name, e.g. snare", true},
              {"volume", ParamType::Number, "dB", false}, {"tune", ParamType::Number, "semitones, -24..24", false},
              {"decay", ParamType::Number, "seconds (0 = full length)", false}},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                int ti = 0;
                std::string error;
                if (!trackArg(d.song, p, ti, error)) return failed(error);
                Track& t = d.song.tracks[static_cast<std::size_t>(ti)];
                if (t.kind != TrackKind::Drum) return failed("pads are on drum tracks; \"" + t.name + "\" is a " + toString(t.kind) + " track");
                const std::string lane = stringParam(p, "lane");
                if (!isDrumVoice(lane) && !t.slots.count(lane)) return failed("\"" + lane + "\" is neither a kit voice nor a sample slot on this track");
                Pad& pad = t.pads[lane];
                if (p.count("volume")) pad.volumeDb = std::clamp(numberParam(p, "volume"), -60.0, 12.0);
                if (p.count("tune")) {
                    if (std::fabs(numberParam(p, "tune")) > 24) return failed("tune must be -24..24 semitones");
                    pad.tune = numberParam(p, "tune");
                }
                if (p.count("decay")) pad.decay = std::clamp(numberParam(p, "decay"), 0.0, 10.0);
                return songResult(t.name + " pad " + lane + ": volume " + num(pad.volumeDb, 3) + " dB, tune " + num(pad.tune, 3) + ", decay " +
                                      (pad.decay > 0 ? num(pad.decay, 3) + " s" : std::string("full")),
                                  d, p);
            }));

    reg.add({"beats.metronome",
             "Metronome",
             "Turn the click on or off for playing and rendering (a click on each beat, higher on the first beat of the bar), "
             "and set its level. beats.render click=false leaves it out of one render.",
             {songParam(), {"on", ParamType::Bool, "click on or off", true}, {"level", ParamType::Number, "click level in dB, -60..0", false}},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                if (p.count("level")) {
                    const double lv = numberParam(p, "level");
                    if (lv < -60 || lv > 0) return failed("level is -60..0 dB");
                    d.song.metronomeDb = lv;
                }
                d.song.metronome = boolParam(p, "on");
                return songResult(std::string("metronome ") + (d.song.metronome ? "on at " + num(d.song.metronomeDb, 3) + " dB" : "off"), d, p);
            }));

    reg.add({"beats.arrange_move",
             "Move placement",
             "Change one placement of the arrangement (placement= its number in beats.info): move it to bar= and/or "
             "to_track=, or set how many times it repeats.",
             {songParam(), {"placement", ParamType::Int, "placement number, from 0", true}, {"bar", ParamType::Int, "new start bar, from 0", false},
              {"to_track", ParamType::String, "move it to this track (number or name)", false}, {"repeat", ParamType::Int, "times in a row", false}, previewParam()},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                Song& s = d.song;
                const auto i = intParam(p, "placement", -1);
                if (i < 0 || i >= static_cast<std::int64_t>(s.arrangement.size())) return failed("no placement " + std::to_string(i));
                Placement& pl = s.arrangement[static_cast<std::size_t>(i)];
                if (p.count("bar")) {
                    const auto bar = intParam(p, "bar");
                    if (bar < 0 || bar > 100000) return failed("bar must be 0..100000");
                    pl.bar = static_cast<int>(bar);
                }
                if (p.count("repeat")) {
                    const auto rep = intParam(p, "repeat");
                    if (rep < 1 || rep > 1024) return failed("repeat must be 1..1024");
                    pl.repeat = static_cast<int>(rep);
                }
                if (p.count("to_track")) {
                    std::string error;
                    Params q;
                    q["track"] = stringParam(p, "to_track");
                    int ti = 0;
                    if (!trackArg(s, q, ti, error)) return failed(error);
                    pl.track = ti;
                }
                return songResult("\"" + pl.pattern + "\" now on track " + std::to_string(pl.track) + " from bar " + std::to_string(pl.bar) +
                                      (pl.repeat > 1 ? " x" + std::to_string(pl.repeat) : ""),
                                  d, p);
            }));

    reg.add({"beats.stems",
             "Export stems",
             "Render each track to its own file in folder (stems for mixing elsewhere): every track's sound with its effects "
             "and automation, mute and solo ignored, no master effects or limiter, all the length of the full mix so they line "
             "up. mix=true also writes the finished mix.",
             {songParam(), {"folder", ParamType::String, "folder for the files", true},
              {"to", ParamType::String, "file format (wav built in; others need ffmpeg)", false, {"wav", "flac", "mp3", "ogg", "opus", "m4a"}, std::string("wav")},
              {"bits", ParamType::String, "WAV/FLAC sample format", false, {"16", "24", "32", "32f"}, std::string("24")},
              {"rate", ParamType::Int, "sample rate (default: the song's)", false}, {"mix", ParamType::Bool, "also write mix.<format>", false, {}, false}},
             false},
            songCommand(
                session,
                [](SongDoc& d, const Params& p) {
                    if (d.song.arrangement.empty()) return failed("nothing is placed in the arrangement yet");
                    const fs::path dir = stringParam(p, "folder");
                    const std::string ext = stringParam(p, "to", "wav");
                    if (!audio::canWrite("x." + ext)) return failed("saving ." + ext + " needs ffmpeg on PATH (wav always works)");
                    audio::SaveOptions so;
                    if (!audio::parseSampleFormat(stringParam(p, "bits", "24"), so.format)) return failed("bits must be 16, 24, 32 or 32f");
                    const auto rate = intParam(p, "rate", 0);
                    if (p.count("rate") && (rate < 8000 || rate > 192000)) return failed("rate must be 8000..192000 Hz");
                    std::error_code ec;
                    fs::create_directories(dir, ec);
                    if (ec) return failed("can't make " + dir.string() + ": " + ec.message());
                    const audio::Buffer mix = render(d.song);
                    auto write = [&](audio::Buffer b, const fs::path& path) {
                        b.resize(mix.frames());
                        if (rate > 0 && rate != b.rate) b = audio::resample(b, static_cast<int>(rate));
                        audio::save(b, path, so);
                    };
                    std::string lines;
                    int written = 0;
                    for (std::size_t i = 0; i < d.song.tracks.size(); ++i) {
                        RenderOptions o;
                        o.onlyTrack = static_cast<int>(i);
                        o.limit = false;
                        o.master = false;
                        o.click = 0;
                        o.tailSeconds = std::max(0.0, mix.seconds() - d.song.seconds()) + 0.01;
                        char n2[8];
                        std::snprintf(n2, sizeof n2, "%02d", static_cast<int>(i + 1));
                        const fs::path path = dir / (std::string(n2) + "_" + safeName(d.song.tracks[i].name) + "." + ext);
                        write(render(d.song, o), path);
                        lines += "\n  " + path.string();
                        ++written;
                    }
                    if (boolParam(p, "mix", false)) {
                        write(mix, dir / ("mix." + ext));
                        lines += "\n  " + (dir / ("mix." + ext)).string();
                        ++written;
                    }
                    Result r = Result::success("wrote " + std::to_string(written) + " files (" + audio::formatTime(mix.seconds()) + " each) to " + dir.string() + lines);
                    r.with("written", static_cast<std::int64_t>(written)).with("seconds", mix.seconds());
                    return r;
                },
                false));
}

}  // namespace wetwaresuite::beats::tool
