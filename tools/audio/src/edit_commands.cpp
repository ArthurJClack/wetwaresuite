// Undoable edits and effects on one audio document. Every one takes doc=,
// a time range (start/end in seconds) and, where it makes sense, channels=.
#include <algorithm>
#include <cmath>

#include "common.hpp"

namespace achclwss::audio::tool {

namespace {

std::string spanText(const AudioDoc& d, Range r) {
    if (r.begin == 0 && r.end >= d.audio.frames()) return "";
    return " from " + formatTime(static_cast<double>(r.begin) / d.audio.rate) + " to " + formatTime(static_cast<double>(r.end) / d.audio.rate);
}

std::string chanText(const Channels& ch) {
    if (ch.empty()) return "";
    std::string s = ch.size() == 1 ? " on channel " : " on channels ";
    for (std::size_t i = 0; i < ch.size(); ++i) s += (i ? "," : "") + std::to_string(ch[i]);
    return s;
}

Result done(const std::string& what, AudioDoc& d, const Params& p, Range r, const Channels& ch) {
    return audioResult(what + spanText(d, r) + chanText(ch), d, p);
}

ParamSpec numberSpec(const std::string& name, const std::string& description, double fallback) {
    return {name, ParamType::Number, description, false, {}, fallback};
}

ParamSpec tailParam() {
    return numberSpec("tail", "seconds of silence added at the end first so the effect can ring out (whole-file edits)", 0.0);
}

// Adds `tail` seconds when the range runs to the end; returns the new range.
Range withTail(AudioDoc& d, const Params& p, Range r) {
    const double tail = std::clamp(numberParam(p, "tail", 0.0), 0.0, 60.0);
    if (tail > 0 && r.end == d.audio.frames()) {
        d.audio.resize(d.audio.frames() + static_cast<std::size_t>(tail * d.audio.rate));
        r.end = d.audio.frames();
    }
    return r;
}

}  // namespace

void registerEditCommands(Session& session, const std::shared_ptr<State>& state) {
    auto& reg = session.registry();
    Session* s = &session;

    // ---- level ----
    reg.add({"audio.gain", "Change volume", "Make the audio louder or quieter by db decibels (+6 is about twice as loud in level, -6 half).",
             concat({docParam(), {"db", ParamType::Number, "change in dB, e.g. -3 or 6", true}}, regionParams()), true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                const double db = numberParam(p, "db");
                if (std::fabs(db) > 120) return failed("db must be within -120..120");
                gain(d.audio, db, r, ch);
                return done("gain " + dbText(db), d, p, r, ch);
            }));

    reg.add({"audio.normalize",
             "Normalize",
             "Set the level: mode=peak scales so the loudest sample hits target dBFS (default -1); mode=loudness sets "
             "the integrated loudness to target LUFS (default -16, the podcast norm; -14 for streaming music, -23 for "
             "broadcast) and then limits any peaks over ceiling dBFS. Loudness works on the whole file; peak can take a "
             "range and channels.",
             concat({docParam(), {"mode", ParamType::String, "what to match", false, {"peak", "loudness"}, std::string("peak")},
                     {"target", ParamType::Number, "dBFS for peak, LUFS for loudness", false},
                     numberSpec("ceiling", "loudness mode: highest peak allowed after normalizing, dBFS", -1.0),
                     {"limit", ParamType::Bool, "loudness mode: limit peaks over ceiling (false lets them clip)", false, {}, true}},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                const std::string mode = stringParam(p, "mode", "peak");
                if (mode == "loudness") {
                    const double target = numberParam(p, "target", -16.0);
                    if (target > 0 || target < -70) return failed("loudness target must be -70..0 LUFS");
                    const double before = integratedLoudness(d.audio, all(d.audio));
                    if (before <= -69.9) return failed("the audio is silent, so there's no loudness to match");
                    const double applied = normalizeLoudness(d.audio, target, numberParam(p, "ceiling", -1.0), boolParam(p, "limit", true));
                    const Levels after = measure(d.audio, all(d.audio), false);
                    Result res = audioResult("loudness " + num(before, 3) + " -> " + num(after.lufs, 3) + " LUFS (gain " + dbText(applied) +
                                                 "), peak " + dbText(after.peakDb),
                                             d, p);
                    return res.with("lufs", after.lufs).with("peak_db", after.peakDb).with("gain_db", applied);
                }
                const double target = numberParam(p, "target", -1.0);
                if (target > 0) return failed("peak target must be 0 dBFS or below");
                const double applied = normalizePeak(d.audio, target, r, ch);
                return done("peak normalized to " + dbText(target) + " (gain " + dbText(applied) + ")", d, p, r, ch).with("gain_db", applied);
            }));

    reg.add({"audio.fade",
             "Fade",
             "Fade in over the first `in` seconds and/or out over the last `out` seconds of the range (default the whole "
             "file). Curves: linear, exponential (slow start), logarithmic (fast start), s_curve, equal_power (for crossfades).",
             concat({docParam(), numberSpec("in", "fade-in length in seconds", 0.0), numberSpec("out", "fade-out length in seconds", 0.0),
                     {"curve", ParamType::String, "fade shape", false, fadeCurveNames(), std::string("linear")}},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                const double in = numberParam(p, "in", 0.0), out = numberParam(p, "out", 0.0);
                if (in <= 0 && out <= 0) return failed("give in= and/or out= in seconds");
                FadeCurve c = FadeCurve::Linear;
                parseFadeCurve(stringParam(p, "curve", "linear"), c);
                if (in > 0) fadeIn(d.audio, in, r, c, ch);
                if (out > 0) fadeOut(d.audio, out, r, c, ch);
                std::string what = in > 0 && out > 0 ? "faded in " + formatTime(in) + " and out " + formatTime(out)
                                   : in > 0        ? "faded in over " + formatTime(in)
                                                   : "faded out over " + formatTime(out);
                return done(what, d, p, r, ch);
            }));

    reg.add({"audio.remove_dc", "Remove DC offset", "Centre the waveform on zero (subtracts each channel's average).",
             concat({docParam()}, regionParams()), true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                for (int c : resolveChannels(d.audio, ch)) {
                    auto& x = d.audio.channels[static_cast<std::size_t>(c)];
                    double mean = 0;
                    for (std::size_t i = r.begin; i < r.end; ++i) mean += x[i];
                    mean /= static_cast<double>(std::max<std::size_t>(1, r.size()));
                    for (std::size_t i = r.begin; i < r.end; ++i) x[i] -= static_cast<float>(mean);
                }
                return done("removed DC offset", d, p, r, ch);
            }));

    // ---- editing ----
    reg.add({"audio.trim",
             "Trim",
             "Keep only start..end and drop the rest, or with silence=true cut the leading and trailing silence "
             "(quieter than threshold), keeping pad seconds.",
             {docParam(), startParam(), endParam(), {"silence", ParamType::Bool, "trim silence at both ends instead", false, {}, false},
              numberSpec("threshold", "dBFS treated as silence", -50.0), numberSpec("pad", "silence to keep at each end, seconds", 0.05),
              previewParam()},
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels&) {
                const double before = d.audio.seconds();
                if (boolParam(p, "silence", false)) {
                    Range keep = nonSilent(d.audio, numberParam(p, "threshold", -50.0));
                    if (keep.size() == 0) return failed("it's all silence at that threshold");
                    const auto pad = static_cast<std::size_t>(std::max(0.0, numberParam(p, "pad", 0.05)) * d.audio.rate);
                    keep.begin = keep.begin > pad ? keep.begin - pad : 0;
                    keep.end = std::min(d.audio.frames(), keep.end + pad);
                    trim(d.audio, keep);
                } else {
                    if (!p.count("start") && !p.count("end")) return failed("give start= and/or end=, or silence=true");
                    trim(d.audio, r);
                }
                if (d.audio.seconds() == before) return audioResult("nothing to trim (still " + formatTime(before) + ")", d, p);
                return audioResult("trimmed " + formatTime(before) + " to " + formatTime(d.audio.seconds()), d, p);
            }));

    reg.add({"audio.cut", "Cut", "Remove start..end and keep it on the clipboard for audio.paste.",
             {docParam(), {"start", ParamType::Number, "seconds", true}, {"end", ParamType::Number, "seconds", true}, previewParam()}, true},
            regionCommand(session, [state](AudioDoc& d, const Params& p, Range r, const Channels&) {
                state->clipboard = cut(d.audio, r);
                state->hasClipboard = true;
                return audioResult("cut " + formatTime(state->clipboard.seconds()) + " to the clipboard", d, p);
            }));

    reg.add({"audio.copy", "Copy", "Copy start..end (default all) to the clipboard for audio.paste.", {docParam(), startParam(), endParam()}, false, true},
            regionCommand(
                session,
                [state](AudioDoc& d, const Params&, Range r, const Channels&) {
                    state->clipboard = slice(d.audio, r);
                    state->hasClipboard = true;
                    return Result::success("copied " + formatTime(state->clipboard.seconds()) + " to the clipboard")
                        .with("seconds", state->clipboard.seconds());
                },
                false));

    reg.add({"audio.paste",
             "Paste",
             "Put the clipboard (or another document, source=aud2) into the audio at `at` seconds (default: the end). "
             "mode=insert pushes the rest later, overwrite replaces what's there, mix layers it on top.",
             {docParam(), {"at", ParamType::Number, "position in seconds (default: the end)", false},
              {"mode", ParamType::String, "how to put it in", false, {"insert", "overwrite", "mix"}, std::string("insert")},
              {"source", ParamType::String, "paste this whole document instead of the clipboard", false},
              numberSpec("gain", "dB applied to what's pasted", 0.0), previewParam()},
             true},
            [s, state](const Params& p) -> std::unique_ptr<Action> {
                Buffer src;
                bool have = false;
                if (p.count("source")) {
                    if (auto other = s->workspace().getAs<AudioDoc>(stringParam(p, "source"))) src = other->audio, have = true;
                    else return std::make_unique<FunctionAction>([id = stringParam(p, "source")] { return failed("no open audio document called '" + id + "'"); });
                } else if (state->hasClipboard) {
                    src = state->clipboard;
                    have = true;
                }
                return audioCommand(*s, [src, have](AudioDoc& d, const Params& q) {
                    if (!have) return failed("the clipboard is empty: run audio.copy or audio.cut first, or pass source=");
                    const double at = numberParam(q, "at", d.audio.seconds());
                    if (at < 0 || at > d.audio.seconds() + 1e-9) return failed("at must be within 0.." + num(d.audio.seconds()) + " seconds");
                    const std::size_t pos = d.audio.toFrame(at);
                    Buffer clip = src;
                    gain(clip, numberParam(q, "gain", 0.0), all(clip));
                    const std::string mode = stringParam(q, "mode", "insert");
                    if (mode == "overwrite") overwrite(d.audio, pos, clip);
                    else if (mode == "mix") mixInto(d.audio, clip, pos);
                    else insert(d.audio, pos, clip);
                    return audioResult("pasted " + formatTime(clip.seconds()) + " at " + formatTime(at) + " (" + mode + ")", d, q);
                })(p);
            });

    reg.add({"audio.insert_silence", "Insert silence", "Insert `seconds` of silence at `at` seconds (default: the end).",
             {docParam(), {"seconds", ParamType::Number, "how much silence", true}, {"at", ParamType::Number, "position in seconds (default: the end)", false},
              previewParam()},
             true},
            audioCommand(session, [](AudioDoc& d, const Params& p) {
                const double sec = numberParam(p, "seconds"), at = numberParam(p, "at", d.audio.seconds());
                if (sec <= 0 || sec > 3600) return failed("seconds must be 0..3600");
                if (at < 0 || at > d.audio.seconds() + 1e-9) return failed("at must be within the audio");
                insertSilence(d.audio, d.audio.toFrame(at), static_cast<std::size_t>(std::llround(sec * d.audio.rate)));
                return audioResult("inserted " + formatTime(sec) + " of silence at " + formatTime(at), d, p);
            }));

    reg.add({"audio.reverse", "Reverse", "Play the range backwards.", concat({docParam()}, regionParams()), true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                reverse(d.audio, r, ch);
                return done("reversed", d, p, r, ch);
            }));

    reg.add({"audio.mix",
             "Mix in",
             "Layer another document (source=aud2) or file (input=) on top of this one, starting at `at` seconds, with a "
             "gain in dB. It is converted to this document's rate and channels; the document grows if needed.",
             {docParam(), {"source", ParamType::String, "id of the audio to mix in", false}, {"input", ParamType::String, "or a file to mix in", false},
              numberSpec("at", "start position in seconds", 0.0), numberSpec("gain", "dB applied to what's mixed in", 0.0), previewParam()},
             true},
            [s](const Params& p) -> std::unique_ptr<Action> {
                Buffer src;
                std::string error;
                if (p.count("source")) {
                    if (auto other = s->workspace().getAs<AudioDoc>(stringParam(p, "source"))) src = other->audio;
                    else error = "no open audio document called '" + stringParam(p, "source") + "'";
                } else if (p.count("input")) {
                    try {
                        src = load(stringParam(p, "input"));
                    } catch (const std::exception& e) {
                        error = e.what();
                    }
                } else {
                    error = "give source=aud2 or input=file.wav";
                }
                if (!error.empty()) return std::make_unique<FunctionAction>([error] { return failed(error); });
                return audioCommand(*s, [src](AudioDoc& d, const Params& q) {
                    const double at = numberParam(q, "at", 0.0);
                    if (at < 0) return failed("at can't be negative");
                    mixInto(d.audio, src, static_cast<std::size_t>(std::llround(at * d.audio.rate)), numberParam(q, "gain", 0.0));
                    return audioResult("mixed in " + formatTime(src.seconds()) + " at " + formatTime(at), d, q);
                })(p);
            });

    // ---- channels and rate ----
    reg.add({"audio.pan", "Pan", "Place the sound left (-1) to right (1). Mono audio becomes stereo first.",
             concat({docParam(), {"position", ParamType::Number, "-1 = hard left, 0 = centre, 1 = hard right", true}}, regionParams(false)), true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels&) {
                const double pos = numberParam(p, "position");
                if (pos < -1 || pos > 1) return failed("position must be -1..1");
                pan(d.audio, pos, r);
                return done("panned to " + num(pos, 3), d, p, r, {});
            }));

    reg.add({"audio.channels",
             "Change channels",
             "mono mixes all channels down to one, stereo makes two (mono is copied to both), swap trades left and "
             "right, left/right keep just that side as mono, count sets any channel count.",
             {docParam(), {"mode", ParamType::String, "what to do", true, {"mono", "stereo", "swap", "left", "right", "count"}},
              {"count", ParamType::Int, "for mode=count: how many channels", false}, previewParam()},
             true},
            audioCommand(session, [](AudioDoc& d, const Params& p) {
                const std::string mode = stringParam(p, "mode");
                if (mode == "mono") setChannels(d.audio, 1);
                else if (mode == "stereo") setChannels(d.audio, 2);
                else if (mode == "swap") {
                    if (d.audio.channelCount() < 2) return failed("swap needs at least two channels");
                    swapChannels(d.audio);
                } else if (mode == "left") extractChannel(d.audio, 0);
                else if (mode == "right") {
                    if (d.audio.channelCount() < 2) return failed("there's no right channel in mono audio");
                    extractChannel(d.audio, 1);
                } else {
                    const auto n = intParam(p, "count", 0);
                    if (n < 1 || n > 32) return failed("count must be 1..32");
                    setChannels(d.audio, static_cast<int>(n));
                }
                return audioResult("now " + std::to_string(d.audio.channelCount()) + " channel" + (d.audio.channelCount() == 1 ? "" : "s"), d, p);
            }));

    reg.add({"audio.resample",
             "Resample",
             "Change the sample rate (e.g. 48000 for video, 44100 for CDs) with a windowed-sinc filter (quality=best) or "
             "fast cubic interpolation. The sound stays the same length and pitch.",
             {docParam(), {"rate", ParamType::Int, "new sample rate in Hz", true},
              {"quality", ParamType::String, "best = windowed sinc, fast = cubic", false, {"best", "fast"}, std::string("best")}, previewParam()},
             true},
            audioCommand(session, [](AudioDoc& d, const Params& p) {
                const auto rate = intParam(p, "rate");
                if (rate < 1000 || rate > 768000) return failed("rate must be 1000..768000 Hz");
                const int before = d.audio.rate;
                d.audio = resample(d.audio, static_cast<int>(rate), stringParam(p, "quality", "best") == "fast" ? 0 : 1);
                return audioResult("resampled " + std::to_string(before) + " -> " + std::to_string(rate) + " Hz", d, p);
            }));

    // ---- filters ----
    reg.add({"audio.eq",
             "Equalizer band",
             "One EQ band. highpass removes rumble below frequency (80-100 Hz for voice), lowpass removes hiss above it, "
             "peaking boosts or cuts gain dB around frequency (q = narrowness, 0.7 wide .. 8 narrow), lowshelf/highshelf "
             "tilt everything below/above it, notch removes one frequency (mains hum: 50 or 60 Hz), bandpass keeps a band. "
             "Run it again for more bands.",
             concat({docParam(), {"type", ParamType::String, "filter type", true, filterTypeNames()},
                     {"frequency", ParamType::Number, "centre or corner frequency in Hz", true},
                     numberSpec("gain", "dB, for peaking and shelves", 0.0), numberSpec("q", "bandwidth/resonance; 0.707 is flat", 0.7071),
                     {"order", ParamType::Int, "stack this many for a steeper slope (pass filters: 12 dB/octave each)", false, {}, std::int64_t{1}}},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                FilterType t;
                parseFilterType(stringParam(p, "type"), t);
                const double f = numberParam(p, "frequency"), g = numberParam(p, "gain", 0.0), q = numberParam(p, "q", 0.7071);
                if (f <= 0 || f >= d.audio.rate / 2.0) return failed("frequency must be between 0 and " + num(d.audio.rate / 2.0) + " Hz");
                if (q <= 0 || q > 40) return failed("q must be 0..40");
                if (std::fabs(g) > 48) return failed("gain must be within -48..48 dB");
                const auto order = intParam(p, "order", 1);
                if (order < 1 || order > 8) return failed("order must be 1..8");
                equalize(d.audio, t, f, g, q, r, ch, static_cast<int>(order));
                std::string what = stringParam(p, "type") + " at " + num(f) + " Hz";
                if (g != 0) what += ", " + dbText(g);
                return done(what, d, p, r, ch);
            }));

    // ---- dynamics ----
    reg.add({"audio.compress",
             "Compress",
             "Even out the level: anything louder than threshold dBFS is turned down by ratio (4 = 4 dB over becomes 1). "
             "Voice: threshold -20, ratio 3, attack 10, release 150, then normalize. auto_makeup adds back level.",
             concat({docParam(), numberSpec("threshold", "dBFS where compression starts", -18.0), numberSpec("ratio", "1 = off, 20+ = limiting", 3.0),
                     numberSpec("attack", "ms to react", 10.0), numberSpec("release", "ms to recover", 120.0),
                     numberSpec("knee", "dB of soft knee around the threshold", 6.0), numberSpec("makeup", "dB added afterwards", 0.0),
                     {"auto_makeup", ParamType::Bool, "add back about half the reduction", false, {}, false}},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                CompressorSettings cs;
                cs.thresholdDb = numberParam(p, "threshold", -18.0);
                cs.ratio = numberParam(p, "ratio", 3.0);
                cs.attackMs = numberParam(p, "attack", 10.0);
                cs.releaseMs = numberParam(p, "release", 120.0);
                cs.kneeDb = numberParam(p, "knee", 6.0);
                cs.makeupDb = numberParam(p, "makeup", 0.0);
                cs.autoMakeup = boolParam(p, "auto_makeup", false);
                if (cs.ratio < 1 || cs.ratio > 100) return failed("ratio must be 1..100");
                if (cs.attackMs < 0 || cs.releaseMs < 0) return failed("attack and release can't be negative");
                const double gr = compress(d.audio, cs, r, ch);
                return done("compressed " + num(cs.ratio, 3) + ":1 above " + dbText(cs.thresholdDb) + ", up to " + dbText(gr) + " of reduction", d, p, r, ch)
                    .with("max_reduction_db", gr);
            }));

    reg.add({"audio.limit",
             "Limit",
             "Stop peaks going over ceiling dBFS with a look-ahead brickwall limiter (no clipping, no distortion "
             "on the way in). Use after normalizing or gain.",
             concat({docParam(), numberSpec("ceiling", "highest level allowed, dBFS", -1.0), numberSpec("release", "ms to recover", 60.0)},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                const double c = numberParam(p, "ceiling", -1.0);
                if (c > 0 || c < -60) return failed("ceiling must be -60..0 dBFS");
                const double gr = limit(d.audio, c, std::max(1.0, numberParam(p, "release", 60.0)), r, ch);
                return done("limited to " + dbText(c) + ", up to " + dbText(gr) + " of reduction", d, p, r, ch).with("max_reduction_db", gr);
            }));

    reg.add({"audio.gate",
             "Noise gate",
             "Silence (or turn down by range dB) everything quieter than threshold dBFS: hiss and room noise between "
             "phrases. It opens a little early so words don't lose their start.",
             concat({docParam(), numberSpec("threshold", "dBFS below which the gate closes", -45.0), numberSpec("attack", "ms to open", 2.0),
                     numberSpec("hold", "ms to stay open after the sound drops", 60.0), numberSpec("release", "ms to close", 150.0),
                     numberSpec("range", "dB of reduction when closed (-80 = silent, -12 = gentle)", -80.0)},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                GateSettings gs;
                gs.thresholdDb = numberParam(p, "threshold", -45.0);
                gs.attackMs = std::max(0.0, numberParam(p, "attack", 2.0));
                gs.holdMs = std::max(0.0, numberParam(p, "hold", 60.0));
                gs.releaseMs = std::max(0.0, numberParam(p, "release", 150.0));
                gs.rangeDb = numberParam(p, "range", -80.0);
                const double closed = gate(d.audio, gs, r, ch);
                return done("gated below " + dbText(gs.thresholdDb) + " (closed " + num(closed * 100, 3) + "% of the time)", d, p, r, ch)
                    .with("closed_fraction", closed);
            }));

    // ---- noise reduction ----
    reg.add({"audio.noise_profile",
             "Learn noise",
             "Learn what the background noise sounds like from start..end, a stretch with only noise (room tone, hiss, "
             "hum) and no voice or music. audio.denoise then uses it.",
             {docParam(), {"start", ParamType::Number, "seconds", true}, {"end", ParamType::Number, "seconds", true},
              {"fft", ParamType::Int, "analysis size; 2048 suits most material", false, {}, std::int64_t{2048}}},
             false, true},
            regionCommand(
                session,
                [](AudioDoc& d, const Params& p, Range r, const Channels&) {
                    d.noise = learnNoise(d.audio, r, static_cast<int>(intParam(p, "fft", 2048)));
                    return Result::success("learned the noise from " + formatTime(static_cast<double>(r.begin) / d.audio.rate) + " to " +
                                           formatTime(static_cast<double>(r.end) / d.audio.rate))
                        .with("doc", stringParam(p, "doc"));
                },
                false));

    reg.add({"audio.denoise",
             "Reduce noise",
             "Spectral noise reduction: every frequency is turned down by `reduction` dB wherever it isn't clearly above "
             "the noise. The noise comes from noise_start..noise_end if given, else the profile from audio.noise_profile, "
             "else it's estimated from the quietest parts. Higher sensitivity removes more but can dull the sound; "
             "12-18 dB of reduction sounds natural, 24+ is aggressive.",
             concat({docParam(), numberSpec("reduction", "dB to take the noise down", 12.0), numberSpec("sensitivity", "how far above the noise counts as signal (0.5..3)", 1.5),
                     {"noise_start", ParamType::Number, "start of a noise-only stretch, seconds", false},
                     {"noise_end", ParamType::Number, "end of a noise-only stretch, seconds", false},
                     {"fft", ParamType::Int, "analysis size; 2048 suits most material, 4096 for steady hum", false, {}, std::int64_t{2048}}},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                const int fftSize = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "fft", 2048), 256, 16384));
                NoiseProfile prof;
                std::string source;
                if (p.count("noise_start") || p.count("noise_end")) {
                    const double ns = numberParam(p, "noise_start", 0.0), ne = numberParam(p, "noise_end", d.audio.seconds());
                    if (ne <= ns) return failed("noise_end must be after noise_start");
                    prof = learnNoise(d.audio, rangeOf(d.audio, ns, ne), fftSize);
                    d.noise = prof;
                    source = "noise from " + formatTime(ns) + " to " + formatTime(ne);
                } else if (!d.noise.empty() && d.noise.rate == d.audio.rate) {
                    prof = d.noise;
                    source = "the learned noise profile";
                } else {
                    prof = estimateNoise(d.audio, fftSize);
                    source = "noise estimated from the quietest parts";
                }
                const double red = std::clamp(numberParam(p, "reduction", 12.0), 0.0, 60.0);
                const double sens = std::clamp(numberParam(p, "sensitivity", 1.5), 0.0, 6.0);
                reduceNoise(d.audio, prof, red, sens, r, ch);
                return done("reduced noise by up to " + num(red, 3) + " dB using " + source, d, p, r, ch);
            }));

    // ---- time effects ----
    reg.add({"audio.reverb",
             "Reverb",
             "Add room ambience (Freeverb). room 0..1 sets the size, damping 0..1 darkens the tail, wet/dry set the mix, "
             "width the stereo spread. tail adds silence first so it can ring out.",
             concat({docParam(), numberSpec("room", "room size 0..1", 0.6), numberSpec("damping", "0 = bright, 1 = dark", 0.5),
                     numberSpec("wet", "reverb level 0..1", 0.25), numberSpec("dry", "original level 0..1", 1.0),
                     numberSpec("width", "stereo width 0..1", 1.0), numberSpec("predelay", "ms before the reverb starts", 0.0), tailParam()},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                ReverbSettings rs;
                rs.roomSize = numberParam(p, "room", 0.6);
                rs.damping = numberParam(p, "damping", 0.5);
                rs.wet = numberParam(p, "wet", 0.25);
                rs.dry = numberParam(p, "dry", 1.0);
                rs.width = numberParam(p, "width", 1.0);
                rs.preDelayMs = std::clamp(numberParam(p, "predelay", 0.0), 0.0, 500.0);
                r = withTail(d, p, r);
                reverb(d.audio, rs, r, ch);
                return done("reverb (room " + num(rs.roomSize, 2) + ", wet " + num(rs.wet, 2) + ")", d, p, r, ch);
            }));

    reg.add({"audio.echo",
             "Echo",
             "Repeat the sound every `delay` seconds, each repeat `feedback` times the last (0..0.95), mixed in at `mix`. "
             "ping_pong bounces stereo repeats between left and right.",
             concat({docParam(), numberSpec("delay", "seconds between repeats", 0.3), numberSpec("feedback", "0..0.95", 0.4),
                     numberSpec("mix", "echo level 0..1", 0.5), {"ping_pong", ParamType::Bool, "alternate repeats left/right", false, {}, false}, tailParam()},
                    regionParams()),
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels& ch) {
                const double delay = numberParam(p, "delay", 0.3);
                if (delay <= 0 || delay > 10) return failed("delay must be 0..10 seconds");
                r = withTail(d, p, r);
                echo(d.audio, delay, numberParam(p, "feedback", 0.4), numberParam(p, "mix", 0.5), r, ch, boolParam(p, "ping_pong", false));
                return done("echo every " + formatTime(delay), d, p, r, ch);
            }));

    reg.add({"audio.stretch",
             "Change tempo",
             "Make it longer or shorter without changing the pitch (WSOLA). Give one of: factor (2 = twice as long), "
             "tempo (1.25 = 25% faster), seconds (new length) or from_bpm + to_bpm.",
             {docParam(), {"factor", ParamType::Number, "length multiplier", false}, {"tempo", ParamType::Number, "speed multiplier", false},
              {"seconds", ParamType::Number, "target length of the range", false}, {"from_bpm", ParamType::Number, "current tempo", false},
              {"to_bpm", ParamType::Number, "wanted tempo", false}, startParam(), endParam(), previewParam()},
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels&) {
                const double len = static_cast<double>(r.size()) / d.audio.rate;
                double factor = 0;
                if (p.count("factor")) factor = numberParam(p, "factor");
                else if (p.count("tempo")) factor = numberParam(p, "tempo") > 0 ? 1.0 / numberParam(p, "tempo") : 0;
                else if (p.count("seconds")) factor = len > 0 ? numberParam(p, "seconds") / len : 0;
                else if (p.count("from_bpm") && p.count("to_bpm")) factor = numberParam(p, "to_bpm") > 0 ? numberParam(p, "from_bpm") / numberParam(p, "to_bpm") : 0;
                else return failed("give factor=, tempo=, seconds= or from_bpm= with to_bpm=");
                if (!(factor >= 0.1 && factor <= 10)) return failed("that's a stretch of " + num(factor) + "x; it must be 0.1..10");
                replaceRange(d.audio, r, stretch(slice(d.audio, r), factor));
                return audioResult("stretched " + formatTime(len) + " to " + formatTime(len * factor) + " (" + num(factor, 4) + "x), pitch unchanged", d, p)
                    .with("factor", factor);
            }));

    reg.add({"audio.pitch",
             "Change pitch",
             "Shift the pitch by semitones (+12 = an octave up) and cents without changing the length.",
             {docParam(), numberSpec("semitones", "-24..24", 0.0), numberSpec("cents", "fine tune, -100..100", 0.0), startParam(), endParam(), previewParam()},
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels&) {
                const double st = numberParam(p, "semitones", 0.0) + numberParam(p, "cents", 0.0) / 100.0;
                if (std::fabs(st) > 24) return failed("pitch shift is limited to 24 semitones either way");
                if (st == 0) return failed("give semitones= and/or cents=");
                Buffer part = slice(d.audio, r);
                Buffer shifted = pitchShift(part, st);
                replaceRange(d.audio, r, shifted);
                return audioResult("pitch " + std::string(st > 0 ? "+" : "") + num(st, 4) + " semitones", d, p);
            }));

    reg.add({"audio.speed",
             "Change speed",
             "Play faster or slower like a tape or record: pitch and length change together (factor 2 = double speed, "
             "an octave up, half as long).",
             {docParam(), {"factor", ParamType::Number, "0.1..10", true}, startParam(), endParam(), previewParam()},
             true},
            regionCommand(session, [](AudioDoc& d, const Params& p, Range r, const Channels&) {
                const double f = numberParam(p, "factor");
                if (!(f >= 0.1 && f <= 10)) return failed("factor must be 0.1..10");
                replaceRange(d.audio, r, changeSpeed(slice(d.audio, r), f));
                return audioResult("speed x" + num(f, 4), d, p);
            }));
}

}  // namespace achclwss::audio::tool
