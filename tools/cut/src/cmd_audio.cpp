// Sound in the edit: playing the mix, and levelling clips (peak normalize) and
// the whole programme (loudness target in LUFS). Clip gain, pan, fades,
// keyframed volume and crossfades are clip properties (cut.set_clip,
// cut.keyframe, cut.transition).
#include <algorithm>
#include <cmath>

#include "internal.hpp"
#include "wetwaresuite/audio/analysis.hpp"
#include "wetwaresuite/cut/render.hpp"
#include "wetwaresuite/playback/commands.hpp"

namespace wetwaresuite::cut {

namespace {

double peakOf(const audio::Buffer& b) {
    double peak = 0;
    for (const auto& ch : b.channels) {
        for (float v : ch) peak = std::max(peak, static_cast<double>(std::abs(v)));
    }
    return peak;
}

double toDb(double v) { return v > 1e-12 ? 20 * std::log10(v) : -240; }

// A clip's own sound at volume 1, with nothing else playing.
audio::Buffer isolated(const Timeline& tl, const Clip& c) {
    Timeline solo;
    solo.settings = tl.settings;
    Clip x = c;
    x.start = 0;
    x.volume = 1;
    x.mute = false;
    x.keys.erase("volume");
    x.fadeIn = x.fadeOut = 0;
    x.transition.clear();
    Track t;
    t.id = c.kind == "audio" ? "a1" : "v1";
    t.kind = c.kind == "audio" ? "audio" : "video";
    t.clips.push_back(x);
    solo.tracks.push_back(t);
    return mixAudio(solo, 0, x.length());
}

// The clips whose sound `clip` stands for: itself, or the linked sound of a video.
std::vector<Clip*> soundClips(Timeline& tl, const Params& p) {
    std::vector<Clip*> out;
    if (has(p, "clip")) {
        Clip* c = requireClip(tl, p);
        for (Clip* x : group(tl, *c, true)) {
            if (x->sound() && x->kind != "nest") out.push_back(x);
        }
        if (out.empty()) throw std::runtime_error(c->id + " has no sound of its own");
        return out;
    }
    for (auto& t : tl.tracks) {
        if (t.locked) continue;
        for (auto& c : t.clips) {
            if (c.sound() && c.kind != "nest" && !c.hold) out.push_back(&c);
        }
    }
    if (out.empty()) throw std::runtime_error("there are no clips with sound");
    return out;
}

void setGain(Clip& c, double volume) {
    const double old = c.volume;
    c.volume = std::clamp(volume, 0.0, 10.0);
    auto it = c.keys.find("volume");
    if (it != c.keys.end() && old > 1e-9) {
        for (auto& k : it->second) k.v = std::clamp(k.v * c.volume / old, 0.0, 10.0);
    }
}

}  // namespace

void registerAudioCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cut.play", "Play sound",
             "Play the timeline's sound (every audio track and the clips' own sound, mixed as cut.render would: "
             "volume, pan, fades, crossfades, mute/solo, speed with the pitch kept) through the speakers, from the "
             "playhead (or start) to the end, optionally looping. Returns at once; cut.playhead says where it is and "
             "cut.pause/resume/stop/seek control it. WAV/MP3/FLAC/Ogg sound needs no ffmpeg.",
             [] {
                 std::vector<ParamSpec> v = {docParam()};
                 for (auto& x : playback::playParams()) v.push_back(x);
                 return v;
             }(),
             false, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                if (tl.duration() <= 0) return Result::failure("the timeline is empty");
                if (!hasSound(tl)) return Result::failure("the timeline has no sound to play");
                auto mix = std::make_shared<const audio::Buffer>(mixAudio(tl, 0, tl.duration()));
                Params q = p;
                if (!has(q, "start") && tl.cursor < tl.duration() - 0.05) q["start"] = tl.cursor;
                return playback::startPlayback(std::move(mix), stringParam(p, "doc"), q);
            }, false));
    playback::registerTransportCommands(session, "cut");

    reg.add({"cut.normalize", "Normalize Clip",
             "Set a clip's gain so its loudest moment peaks at `peak` dBFS (or every sound clip's when clip is left "
             "out). A video stands for its linked sound. Keyframed volume keeps its shape.",
             {docParam(), {"clip", ParamType::String, "clip id; leave out for every clip with sound", false},
              {"peak", ParamType::Number, "target peak in dBFS", false, {}, -1.0}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const double target = numberParam(p, "peak", -1.0);
                if (target > 0 || target < -60) return Result::failure("peak must be -60..0 dBFS");
                std::string text;
                int n = 0;
                for (Clip* c : soundClips(tl, p)) {
                    const double peak = peakOf(isolated(tl, *c));
                    if (peak < 1e-6) continue;
                    setGain(*c, std::pow(10.0, target / 20) / peak);
                    text += (text.empty() ? "" : ", ") + c->id + " gain " + num(toDb(c->volume)) + " dB";
                    ++n;
                }
                if (!n) return Result::failure("those clips are silent");
                Result r = Result::success("normalized to " + num(target) + " dBFS: " + text);
                return r.with("doc", stringParam(p, "doc")).with("clips", static_cast<std::int64_t>(n));
            }));

    reg.add({"cut.loudness", "Loudness",
             "Measure the programme loudness (integrated LUFS, ITU-R BS.1770) and peak of the whole mix (or one "
             "clip), and set the master volume (or the clip's gain) so it hits a target: -14 LUFS for YouTube and "
             "Spotify, -16 for podcasts, -23 for broadcast (EBU R128). The gain stops short of pushing the peak over "
             "`ceiling`. measure_only=true just reports.",
             {docParam(), {"target", ParamType::Number, "loudness to reach, LUFS", false, {}, -14.0},
              {"clip", ParamType::String, "level just this clip", false},
              {"ceiling", ParamType::Number, "highest peak allowed, dBFS", false, {}, -1.0},
              {"measure_only", ParamType::Bool, "only measure", false, {}, false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* clip = nullptr;
                audio::Buffer b;
                if (has(p, "clip")) {
                    clip = soundClips(tl, p).front();
                    b = isolated(tl, *clip);
                } else {
                    if (tl.duration() <= 0 || !hasSound(tl)) return Result::failure("the timeline has no sound");
                    b = mixAudio(tl, 0, tl.duration());
                }
                const double lufs = audio::integratedLoudness(b, audio::all(b));
                const double peak = peakOf(b);
                std::string text = std::string(clip ? clip->id + " (at gain 1)" : "the mix") + " is " + num(lufs) + " LUFS, peak " + num(toDb(peak)) + " dBFS";
                Result r;
                if (boolParam(p, "measure_only", false) || lufs <= -69.9) {
                    if (lufs <= -69.9 && !boolParam(p, "measure_only", false)) return Result::failure(text + ": too quiet to measure (silence)");
                    r = Result::success(text, lufs);
                    return r.with("lufs", lufs).with("peak_db", toDb(peak));
                }
                const double target = numberParam(p, "target", -14.0);
                if (target > 0 || target < -60) return Result::failure("target must be -60..0 LUFS");
                double gainDb = target - lufs;
                bool limited = false;
                const double ceiling = numberParam(p, "ceiling", -1.0);
                if (toDb(peak) + gainDb > ceiling) {
                    gainDb = ceiling - toDb(peak);
                    limited = true;
                }
                const double g = std::pow(10.0, gainDb / 20);
                if (clip) {
                    setGain(*clip, g);  // measured at gain 1
                } else {
                    tl.masterVolume = std::clamp(tl.masterVolume * g, 0.0, 4.0);
                }
                text += "; " + std::string(clip ? "gain" : "master volume") + " " + (gainDb >= 0 ? "+" : "") + num(gainDb) + " dB -> " +
                        num(lufs + gainDb) + " LUFS" + (limited ? " (held back so the peak stays under " + num(ceiling) + " dBFS)" : "");
                r = Result::success(text, lufs + gainDb);
                return r.with("lufs", lufs).with("peak_db", toDb(peak)).with("gain_db", gainDb).with("limited", limited)
                    .with("master_volume", tl.masterVolume);
            }));
}

}  // namespace wetwaresuite::cut
