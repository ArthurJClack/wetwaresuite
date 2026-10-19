// Effects on one audio document (chorus, flanger, phaser, distortion, the
// parametric and graphic EQ, de-esser, expander... built from the rack's
// effect table), restoration (clicks, hum, spectral ranges) and the loudness
// meter.
#include <algorithm>
#include <cmath>

#include "common.hpp"
#include "wetwaresuite/audio/rack.hpp"
#include "wetwaresuite/audio/repair.hpp"

namespace wetwaresuite::audio::tool {

std::vector<ParamSpec> effectParamSpecs(const EffectType& t) {
    std::vector<ParamSpec> v;
    for (const auto& p : t.params) {
        if (t.name == "eq") continue;  // 32 band settings: audio.param_eq takes bands= instead
        std::string help = p.help;
        if (p.choices.empty()) {
            help += " (" + num(p.min) + ".." + num(p.max) + (p.unit.empty() ? "" : " " + p.unit) + ")";
            v.push_back({p.name, ParamType::Number, help, false, {}, p.def});
        } else {
            v.push_back({p.name, ParamType::String, help, false, p.choices, p.choices[static_cast<std::size_t>(std::lround(p.def))]});
        }
    }
    return v;
}

bool effectFromParams(const EffectType& t, const Params& p, Effect& out, std::string& error) {
    out = {};
    out.type = t.name;
    std::string text;
    for (const auto& prm : t.params) {
        if (!p.count(prm.name)) continue;
        text += " " + prm.name + "=" + (prm.choices.empty() ? toText(p.at(prm.name)) : stringParam(p, prm.name));
    }
    return parseEffectValues(t.name, text, out.params, error);
}

namespace {

ParamSpec numberSpec(const std::string& name, const std::string& description, double fallback) {
    return {name, ParamType::Number, description, false, {}, fallback};
}

Result applied(const std::string& what, AudioDoc& d, const Params& p, Range r) {
    std::string span;
    if (r.begin > 0 || r.end < d.audio.frames())
        span = " from " + formatTime(static_cast<double>(r.begin) / d.audio.rate) + " to " + formatTime(static_cast<double>(r.end) / d.audio.rate);
    return audioResult(what + span, d, p);
}

// audio.<command> for an effect of the table: its settings become typed params.
void addTableEffect(Session& session, const std::string& command, const std::string& type, const std::string& title, const std::string& extra) {
    const EffectType* t = findEffectType(type);
    session.registry().add({command, title, t->description + extra, concat(concat({docParam()}, effectParamSpecs(*t)), regionParams()), true},
                           regionCommand(session, [t](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                               Effect e;
                               std::string error;
                               if (!effectFromParams(*t, p, e, error)) return failed(error);
                               applyEffect(d.audio, e, r, ch);
                               return applied(describeEffect(e), d, p, r);
                           }));
}

}  // namespace

void registerEffectCommands(Session& session) {
    auto& reg = session.registry();

    addTableEffect(session, "audio.chorus", "chorus", "Chorus", " Classic settings: rate 0.8, depth 3, delay 15, mix 0.5.");
    addTableEffect(session, "audio.flanger", "flanger", "Flanger", " More feedback makes it ring; negative feedback sounds hollow.");
    addTableEffect(session, "audio.phaser", "phaser", "Phaser", " More stages give more notches; feedback makes them sharper.");
    addTableEffect(session, "audio.tremolo", "tremolo", "Tremolo", " spread=180 makes it an auto-pan on stereo audio.");
    addTableEffect(session, "audio.distort", "distortion", "Distortion", " Turn output down as drive goes up; mix blends in the clean sound.");
    addTableEffect(session, "audio.deess", "deesser", "De-ess", " Voice: frequency 5000-7000, threshold so it acts only on the s sounds.");
    addTableEffect(session, "audio.expand", "expander", "Expand",
                   " Use it to push room noise and bleed down between phrases without the on/off of a gate.");
    addTableEffect(session, "audio.graphic_eq", "graphic_eq", "Graphic EQ", " Each band is an octave wide; a few dB is usually plenty.");
    addTableEffect(session, "audio.filter", "filter", "Filter", " slope 2..4 stacks it for a steeper cut.");

    reg.add({"audio.param_eq",
             "Parametric EQ",
             "Several EQ bands in one step (up to 8), each \"type frequency [gain dB] [q]\" separated by commas: "
             "bands=\"highpass 80, lowshelf 150 2, peaking 3000 -3 2, highshelf 10000 2\". Types: lowpass, highpass, "
             "bandpass, notch, peaking (bell), lowshelf, highshelf.",
             concat({docParam(), {"bands", ParamType::String, "the bands, e.g. \"highpass 80, peaking 2500 -4 2\"", true},
                     numberSpec("output", "output gain in dB", 0.0)},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                Effect e;
                std::string error;
                if (!makeEffect("eq", "output=" + num(numberParam(p, "output", 0.0), 6) + "; bands=" + stringParam(p, "bands"), e, error)) return failed(error);
                applyEffect(d.audio, e, r, ch);
                return applied(describeEffect(e), d, p, r);
            }));

    reg.add({"audio.effect",
             "Apply effect",
             "Apply any effect of the rack by name with its settings as text: type=compressor values=\"threshold=-20 "
             "ratio=4\". audio.fx_types lists every effect and setting.",
             concat({docParam(), {"type", ParamType::String, "effect", true, effectTypeNames()},
                     {"values", ParamType::String, "settings as name=value pairs (unset ones keep their defaults)", false}},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                Effect e;
                std::string error;
                if (!makeEffect(stringParam(p, "type"), stringParam(p, "values"), e, error)) return failed(error);
                applyEffect(d.audio, e, r, ch);
                return applied(describeEffect(e), d, p, r);
            }));

    reg.add({"audio.fx_types",
             "Effect list",
             "Every effect the rack and audio.effect know, with each setting's range and default. rows has one line per "
             "setting: effect, setting, min, max, default, unit, choices (separated by |).",
             {{"type", ParamType::String, "only this effect", false, effectTypeNames()}},
             false, true},
            [](const Params& p) {
                return std::make_unique<FunctionAction>([p] {
                    std::string text, rows;
                    for (const auto& t : effectTypes()) {
                        if (p.count("type") && findEffectType(stringParam(p, "type")) != &t) continue;
                        text += "\n" + t.name + " (" + t.title + "): " + t.description;
                        std::string settings;
                        for (const auto& prm : t.params) {
                            std::string choices;
                            for (const auto& c : prm.choices) choices += (choices.empty() ? "" : "|") + c;
                            rows += t.name + "\t" + prm.name + "\t" + num(prm.min, 6) + "\t" + num(prm.max, 6) + "\t" + num(prm.def, 6) + "\t" + prm.unit + "\t" + choices + "\n";
                            if (t.name == "eq" && prm.name.rfind("b1_", 0) != 0 && prm.name != "output") continue;
                            settings += (settings.empty() ? "" : ", ") + prm.name + " " + (choices.empty() ? num(prm.min) + ".." + num(prm.max) + (prm.unit.empty() ? "" : " " + prm.unit) : choices);
                        }
                        if (t.name == "eq") settings += " (and b2_... to b" + std::to_string(kEqBands) + "_..., or bands=\"peaking 1000 -3 2, ...\")";
                        text += "\n  " + settings;
                    }
                    if (text.empty()) return failed("no effect called " + stringParam(p, "type"));
                    return Result::success("effects:" + text).with("rows", rows);
                });
            });

    // ---- restoration ----
    reg.add({"audio.declick",
             "Remove clicks",
             "Find clicks, pops and crackle (vinyl, bad edits, mouth clicks) and rebuild each from the sound around it. "
             "Lower sensitivity finds more; disturbances longer than max_ms are left alone.",
             concat({docParam(), numberSpec("sensitivity", "how far a sample must jump out, 3 (finds more) .. 12 (only big clicks)", 6.0),
                     numberSpec("max_ms", "longest click to repair, milliseconds", 2.5)},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                ClickSettings cs;
                cs.sensitivity = numberParam(p, "sensitivity", 6.0);
                cs.maxClickMs = numberParam(p, "max_ms", 2.5);
                if (cs.sensitivity < 1 || cs.sensitivity > 50) return failed("sensitivity must be 1..50");
                if (cs.maxClickMs <= 0 || cs.maxClickMs > 20) return failed("max_ms must be 0..20");
                const int n = removeClicks(d.audio, cs, r, ch);
                return applied("repaired " + std::to_string(n) + (n == 1 ? " click" : " clicks"), d, p, r).with("clicks", static_cast<std::int64_t>(n));
            }));

    reg.add({"audio.dehum",
             "Remove hum",
             "Notch out mains hum (50 Hz in Europe/Asia, 60 Hz in the Americas) and its harmonics; frequency=0 finds out which.",
             concat({docParam(), numberSpec("frequency", "hum fundamental in Hz; 0 = detect 50 or 60", 0.0),
                     {"harmonics", ParamType::Int, "how many multiples to notch, fundamental included", false, {}, std::int64_t{8}},
                     numberSpec("q", "notch sharpness: higher takes out less of the sound around it", 30.0)},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                HumSettings hs;
                hs.frequency = numberParam(p, "frequency", 0.0);
                hs.harmonics = static_cast<int>(intParam(p, "harmonics", 8));
                hs.q = numberParam(p, "q", 30.0);
                if (hs.frequency < 0 || hs.frequency > 1000) return failed("frequency must be 0 (detect) or a hum frequency up to 1000 Hz");
                if (hs.harmonics < 1 || hs.harmonics > 40) return failed("harmonics must be 1..40");
                if (hs.q < 1 || hs.q > 200) return failed("q must be 1..200");
                const double f0 = removeHum(d.audio, hs, r, ch);
                return applied("removed " + num(f0) + " Hz hum and " + std::to_string(hs.harmonics - 1) + " harmonics", d, p, r).with("frequency", f0);
            }));

    reg.add({"audio.spectral",
             "Spectral edit",
             "Turn a box of the spectrogram up or down: frequencies low..high Hz between start and end seconds, by gain "
             "dB (-40 nearly removes a whistle, a siren or a squeak). audio.preview kind=spectrogram shows where things are.",
             concat({docParam(), {"low", ParamType::Number, "bottom of the box, Hz", true}, {"high", ParamType::Number, "top of the box, Hz", true},
                     numberSpec("gain", "dB: negative attenuates", -30.0)},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                const double lo = numberParam(p, "low"), hi = numberParam(p, "high"), g = numberParam(p, "gain", -30.0);
                if (lo < 0 || hi <= lo) return failed("give low < high, in Hz");
                if (lo >= d.audio.rate / 2.0) return failed("low is above the highest frequency (" + num(d.audio.rate / 2.0) + " Hz)");
                if (g < -96 || g > 24) return failed("gain must be -96..24 dB");
                spectralGain(d.audio, lo, hi, g, r, ch);
                return applied((g < 0 ? "attenuated " : "boosted ") + num(lo) + "-" + num(hi) + " Hz by " + dbText(std::fabs(g)), d, p, r);
            }));

    reg.add({"audio.loudness",
             "Loudness meter",
             "Measure loudness the broadcast way (EBU R128 / ITU-R BS.1770): integrated LUFS, the loudest momentary "
             "(400 ms) and short-term (3 s) loudness, loudness range and true peak, plus the short-term loudness every "
             "`step` seconds. With target= it says whether the audio meets it (e.g. -14 for streaming, -16 podcasts, -23 broadcast).",
             {docParam(), startParam(), endParam(), numberSpec("step", "seconds between values of the loudness curve", 1.0),
              {"target", ParamType::Number, "integrated LUFS to check against", false},
              numberSpec("tolerance", "LU either side of the target that still passes", 1.0),
              numberSpec("true_peak", "highest true peak allowed with target=, dBTP", -1.0)},
             false, true},
            regionCommand(
                session,
                [](AudioDoc& d, const Params& p, Range r, const Channels&) {
                    const double step = numberParam(p, "step", 1.0);
                    if (step < 0.1 || step > 60) return failed("step must be 0.1..60 seconds");
                    const Levels lv = measure(d.audio, r, true);
                    const auto curve = loudnessCurve(d.audio, r, 3.0, step);
                    Result res = Result::success("");
                    std::string msg = "integrated " + (lv.lufs <= -69.9 ? std::string("silent") : num(lv.lufs, 3) + " LUFS") + ", short-term max " +
                                      num(lv.shortTermMaxLufs, 3) + ", momentary max " + num(lv.momentaryMaxLufs, 3) + ", range " +
                                      num(lv.loudnessRange, 3) + " LU, true peak " + dbText(lv.truePeakDb);
                    res.with("lufs", lv.lufs)
                        .with("short_term_max_lufs", lv.shortTermMaxLufs)
                        .with("momentary_max_lufs", lv.momentaryMaxLufs)
                        .with("loudness_range", lv.loudnessRange)
                        .with("true_peak_db", lv.truePeakDb)
                        .with("step", step)
                        .with("short_term", NumberList(curve.begin(), curve.end()));
                    if (p.count("target")) {
                        const double target = numberParam(p, "target"), tol = numberParam(p, "tolerance", 1.0), tp = numberParam(p, "true_peak", -1.0);
                        const bool loudOk = std::fabs(lv.lufs - target) <= tol, peakOk = lv.truePeakDb <= tp + 1e-9;
                        msg += loudOk && peakOk ? "; meets " + num(target, 3) + " LUFS" : "; doesn't meet " + num(target, 3) + " LUFS:";
                        if (!loudOk) msg += " " + num(std::fabs(lv.lufs - target), 2) + " LU too " + (lv.lufs > target ? "loud" : "quiet");
                        if (!peakOk) msg += std::string(loudOk ? " " : ",") + " true peak over " + dbText(tp);
                        if (!(loudOk && peakOk)) msg += " (audio.normalize mode=loudness target=" + num(target, 3) + " ceiling=" + num(tp, 3) + " fixes both)";
                        res.with("meets_target", loudOk && peakOk).with("gain_to_target", target - lv.lufs);
                    }
                    res.message = msg;
                    return res;
                },
                false));
}

}  // namespace wetwaresuite::audio::tool
