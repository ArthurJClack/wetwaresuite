// beats.track_*, beats.effect, beats.pattern_* and beats.arrange_*: the
// undoable edits of a song.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "common.hpp"

namespace wetwaresuite::beats::tool {

namespace {

std::vector<ParamSpec> soundParams() {
    return {{"name", ParamType::String, "track name", false},
            {"preset", ParamType::String, "synth sound to start from", false, presetNames()},
            {"osc1", ParamType::String, "first oscillator", false, oscNames()},
            {"osc2", ParamType::String, "second oscillator (off to disable)", false, oscNames()},
            {"osc2_semitones", ParamType::Number, "second oscillator pitch offset, semitones", false},
            {"osc2_level", ParamType::Number, "second oscillator level 0..1", false},
            {"detune", ParamType::Number, "cents between the oscillators", false},
            {"octave", ParamType::Int, "shift the whole instrument by octaves", false},
            {"attack", ParamType::Number, "envelope attack, seconds", false},
            {"decay", ParamType::Number, "envelope decay, seconds", false},
            {"sustain", ParamType::Number, "envelope sustain level 0..1", false},
            {"release", ParamType::Number, "envelope release, seconds", false},
            {"cutoff", ParamType::Number, "low-pass filter cutoff, Hz", false},
            {"resonance", ParamType::Number, "filter resonance 0..1", false},
            {"filter_env", ParamType::Number, "octaves the filter opens at each note's start (0 = none)", false},
            {"filter_decay", ParamType::Number, "seconds the filter envelope takes to close", false},
            {"volume", ParamType::Number, "track volume in dB (0 = unchanged)", false},
            {"pan", ParamType::Number, "-1 left .. 0 centre .. 1 right", false},
            {"mute", ParamType::Bool, "mute the track", false},
            {"solo", ParamType::Bool, "solo the track (only solo tracks play)", false},
            {"sample", ParamType::String, "sampler: the audio file it plays; drum: with slot=, the file for that lane (\"\" removes it)", false},
            {"slot", ParamType::String, "drum: lane name the sample plays on (a kit voice such as kick replaces it, or a new name like perc)", false},
            {"root", ParamType::String, "sampler: the note the sample sounds at unchanged, e.g. C4", false}};
}

bool applySound(Song& song, Track& t, const Params& p, std::string& error) {
    auto range = [&](const char* key, double lo, double hi, double& dst) {
        if (!p.count(key)) return true;
        const double v = numberParam(p, key);
        if (v < lo || v > hi) {
            error = std::string(key) + " must be " + num(lo) + ".." + num(hi);
            return false;
        }
        dst = v;
        return true;
    };
    if (p.count("preset")) {
        if (!preset(stringParam(p, "preset"), t.synth)) {
            error = "unknown preset '" + stringParam(p, "preset") + "'";
            return false;
        }
        t.presetName = stringParam(p, "preset");
    }
    if (p.count("osc1") && !parseOsc(stringParam(p, "osc1"), t.synth.osc1)) return error = "unknown osc1 shape", false;
    if (p.count("osc2") && !parseOsc(stringParam(p, "osc2"), t.synth.osc2)) return error = "unknown osc2 shape", false;
    if (!range("osc2_semitones", -48, 48, t.synth.osc2Semitones) || !range("osc2_level", 0, 1, t.synth.osc2Level) ||
        !range("detune", -100, 100, t.synth.detune) || !range("attack", 0, 10, t.synth.attack) || !range("decay", 0, 10, t.synth.decay) ||
        !range("sustain", 0, 1, t.synth.sustain) || !range("release", 0, 10, t.synth.release) || !range("cutoff", 20, 20000, t.synth.cutoff) ||
        !range("resonance", 0, 1, t.synth.resonance) || !range("filter_env", -8, 8, t.synth.filterEnv) ||
        !range("filter_decay", 0.005, 10, t.synth.filterDecay) || !range("volume", -60, 12, t.volumeDb) || !range("pan", -1, 1, t.pan)) {
        return false;
    }
    if (p.count("octave")) {
        const auto o = intParam(p, "octave");
        if (o < -4 || o > 4) return error = "octave must be -4..4", false;
        t.synth.octave = static_cast<int>(o);
    }
    if (p.count("mute")) t.mute = boolParam(p, "mute");
    if (p.count("solo")) t.solo = boolParam(p, "solo");
    if (p.count("name")) {
        const std::string n = stringParam(p, "name");
        if (n.empty()) return error = "the name can't be empty", false;
        t.name = n;
    }
    if (p.count("root")) {
        const int r = parseNote(stringParam(p, "root"));
        if (r < 0) return error = "root is a note like C4 or a MIDI number", false;
        t.root = r;
    }
    if (p.count("slot") && t.kind != TrackKind::Drum) return error = "slot= is for drum tracks; a sampler track takes sample= alone", false;
    if (p.count("sample")) {
        const std::string path = stringParam(p, "sample");
        if (t.kind == TrackKind::Synth) return error = "synth tracks don't play samples; add a sampler or drum track", false;
        if (t.kind == TrackKind::Drum && !p.count("slot")) return error = "on a drum track give slot=<lane> with sample= (e.g. slot=kick)", false;
        Sample smp;
        if (!path.empty()) smp = loadSample(path, song.rate);  // throws with a readable reason
        if (t.kind == TrackKind::Drum) {
            const std::string slot = stringParam(p, "slot");
            if (slot.empty()) return error = "slot needs a lane name", false;
            if (path.empty()) t.slots.erase(slot);
            else t.slots[slot] = smp;
        } else {
            t.sample = smp;
        }
    } else if (p.count("slot")) {
        return error = "give sample=<file> with slot= (or sample=\"\" to remove the slot)", false;
    }
    return true;
}

bool chordsInto(Pattern& pat, const Params& p, const Song& s, std::string& error) {
    if (!p.count("chords")) return true;
    const double each = p.count("chord_beats") ? numberParam(p, "chord_beats") : s.beatsInBar();
    const int octave = static_cast<int>(intParam(p, "octave", 3));
    const int vel = static_cast<int>(intParam(p, "velocity", 90));
    if (each <= 0) return error = "chord_beats must be > 0", false;
    if (octave < 0 || octave > 7) return error = "octave must be 0..7", false;
    if (vel < 1 || vel > 127) return error = "velocity must be 1..127", false;
    double at = p.count("at") ? numberParam(p, "at") : 0.0;
    std::string word;
    std::vector<std::string> words;
    for (char c : stringParam(p, "chords") + " ") {
        if (c == ' ' || c == ',' || c == ';' || c == '|' || c == '\n') {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    for (const auto& w : words) {
        if (w == "-" || w == "." || w == "_") {  // rest / hold a gap
            at += each;
            continue;
        }
        const auto notes = chordNotes(w, octave);
        if (notes.empty()) return error = "\"" + w + "\" isn't a chord I know (try C, Am, F#m7, Bbmaj7, Gsus4, D7, Edim, C/E)", false;
        for (int n : notes) pat.notes.push_back({n, at, each, vel});
        at += each;
    }
    const double needBeats = at;
    if (!p.count("bars")) {
        const int needBars = static_cast<int>(std::ceil(needBeats / s.beatsInBar() - 1e-9));
        pat.bars = std::max(pat.bars, std::min(256, needBars));
    }
    return true;
}

bool notesInto(Pattern& pat, const Params& p, const Song& s, std::string& error) {
    if (!p.count("notes")) return true;
    std::vector<Note> notes;
    if (!parseNotes(stringParam(p, "notes"), notes, error)) return false;
    const double offset = p.count("at") && !p.count("chords") ? numberParam(p, "at") : 0.0;
    double end = 0;
    for (auto& n : notes) {
        n.start += offset;
        end = std::max(end, n.start + n.length);
        pat.notes.push_back(n);
    }
    if (!p.count("bars")) {
        const int needBars = static_cast<int>(std::ceil(end / s.beatsInBar() - 1e-9));
        pat.bars = std::max(pat.bars, std::min(256, needBars));
    }
    return true;
}

std::vector<ParamSpec> noteInputParams() {
    return {{"notes", ParamType::String, "piano-roll notes: \"pitch start length [velocity]\" separated by commas, e.g. \"C4 0 1, E4 1 1 90, G4 2 2\" (start and length in beats)", false},
            {"chords", ParamType::String, "chord symbols played one after another, e.g. \"Am F C G\" (- for a gap)", false},
            {"chord_beats", ParamType::Number, "beats per chord (default: one bar)", false},
            {"octave", ParamType::Int, "octave of the chord roots (3 = the C below middle C... C3)", false, {}, std::int64_t{3}},
            {"velocity", ParamType::Int, "chord velocity 1..127", false, {}, std::int64_t{90}},
            {"at", ParamType::Number, "beat the notes or chords start from", false, {}, 0.0}};
}

int placementSpan(const Song& s, const Placement& pl) {
    const Pattern* p = s.findPattern(pl.pattern);
    return (p ? p->bars : 1) * std::max(1, pl.repeat);
}

}  // namespace

void registerTrackCommands(Session& session) {
    auto& reg = session.registry();

    // ---- tracks ----
    reg.add({"beats.track_add",
             "Add track",
             "Add a track: kind=synth (the built-in subtractive synth; start from preset=bass, lead, pad, pluck, keys, sub or "
             "brass), kind=drum (the synthesized kit: kick, snare, hat, openhat, clap, tom, hitom, lotom, rim, crash, cowbell, "
             "shaker; slot= + sample= put your own samples on lanes) or kind=sampler (sample= played at the pitch of each note, "
             "root= is the note it sounds unchanged). Every sound setting of beats.track_set can be given here too.",
             [] {
                 std::vector<ParamSpec> v = {songParam(), {"kind", ParamType::String, "what the track plays", false, {"synth", "drum", "sampler"}, std::string("synth")}};
                 for (auto& x : soundParams()) v.push_back(x);
                 v.push_back(previewParam());
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                Track t;
                if (!parseTrackKind(stringParam(p, "kind", "synth"), t.kind)) return failed("kind is synth, drum or sampler");
                if (t.kind == TrackKind::Synth && !p.count("preset")) {
                    preset("lead", t.synth);
                    t.presetName = "lead";
                }
                if (t.kind == TrackKind::Sampler) {
                    t.synth.attack = 0.002;
                    t.synth.release = 0.1;
                }
                const char* base = t.kind == TrackKind::Drum ? "Drums" : t.kind == TrackKind::Sampler ? "Sampler" : "Synth";
                t.name = base;
                for (int i = 2; findTrack(d.song, t.name) >= 0; ++i) t.name = std::string(base) + " " + std::to_string(i);
                if (p.count("preset") && !p.count("name")) {
                    std::string n = stringParam(p, "preset");
                    if (!n.empty()) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
                    t.name = n;
                    for (int i = 2; findTrack(d.song, t.name) >= 0; ++i) t.name = n + " " + std::to_string(i);
                }
                std::string error;
                if (!applySound(d.song, t, p, error)) return failed(error);
                if (findTrack(d.song, t.name) >= 0) return failed("there's already a track called '" + t.name + "'");
                d.song.tracks.push_back(t);
                const auto idx = static_cast<std::int64_t>(d.song.tracks.size() - 1);
                Result r = songResult("added track " + std::to_string(idx) + " \"" + t.name + "\" (" + toString(t.kind) + (t.presetName.empty() ? "" : ", " + t.presetName) + ")", d, p);
                r.with("track", idx).with("name", t.name);
                return r;
            }));

    reg.add({"beats.track_set",
             "Track settings",
             "Change a track: name, synth sound (preset, oscillators, envelope, filter), volume, pan, mute, solo, the sampler's "
             "sample and root note, or a drum lane's sample (slot= + sample=). Only the settings given change.",
             [] {
                 std::vector<ParamSpec> v = {songParam(), trackParam()};
                 for (auto& x : soundParams()) v.push_back(x);
                 v.push_back(previewParam());
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                int ti = 0;
                std::string error;
                if (!trackArg(d.song, p, ti, error)) return failed(error);
                Track& t = d.song.tracks[static_cast<std::size_t>(ti)];
                if (p.count("name")) {
                    const int other = findTrack(d.song, stringParam(p, "name"));
                    if (other >= 0 && other != ti) return failed("there's already a track called '" + stringParam(p, "name") + "'");
                }
                if (!applySound(d.song, t, p, error)) return failed(error);
                Result r = songResult("track " + std::to_string(ti) + " \"" + t.name + "\" updated", d, p);
                r.with("track", static_cast<std::int64_t>(ti)).with("name", t.name);
                return r;
            }));

    reg.add({"beats.track_remove",
             "Remove track",
             "Remove a track and its placements (the patterns stay). Later tracks move up one number.",
             {songParam(), trackParam(), previewParam()},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                int ti = 0;
                std::string error;
                if (!trackArg(d.song, p, ti, error)) return failed(error);
                const std::string name = d.song.tracks[static_cast<std::size_t>(ti)].name;
                d.song.tracks.erase(d.song.tracks.begin() + ti);
                auto& arr = d.song.arrangement;
                arr.erase(std::remove_if(arr.begin(), arr.end(), [ti](const Placement& pl) { return pl.track == ti; }), arr.end());
                for (auto& pl : arr) {
                    if (pl.track > ti) --pl.track;
                }
                return songResult("removed track " + std::to_string(ti) + " \"" + name + "\"", d, p);
            }));

    reg.add({"beats.effect",
             "Track effect",
             "Set one effect on a track. Every track has the chain distortion -> compress -> pan -> delay -> reverb; amount=0 "
             "turns that effect off. delay: time in beats (0.75 = dotted eighth) and feedback; reverb: size.",
             {songParam(), trackParam(),
              {"type", ParamType::String, "which effect", true, {"distortion", "compress", "delay", "reverb"}},
              {"amount", ParamType::Number, "0..1: drive, compression, delay mix or reverb wet (0 = off)", true},
              {"time", ParamType::Number, "delay: echo time in beats", false},
              {"feedback", ParamType::Number, "delay: 0..0.95", false},
              {"size", ParamType::Number, "reverb: room size 0..1", false},
              previewParam()},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                int ti = 0;
                std::string error;
                if (!trackArg(d.song, p, ti, error)) return failed(error);
                Effects& fx = d.song.tracks[static_cast<std::size_t>(ti)].fx;
                const double amount = numberParam(p, "amount");
                if (amount < 0 || amount > 1) return failed("amount is 0..1");
                const std::string type = stringParam(p, "type");
                if (type == "distortion") fx.distortion = amount;
                else if (type == "compress") fx.compress = amount;
                else if (type == "delay") {
                    fx.delayMix = amount;
                    if (p.count("time")) {
                        const double t = numberParam(p, "time");
                        if (t <= 0 || t > 16) return failed("time is 0..16 beats");
                        fx.delayBeats = t;
                    }
                    if (p.count("feedback")) {
                        const double f = numberParam(p, "feedback");
                        if (f < 0 || f > 0.95) return failed("feedback is 0..0.95");
                        fx.delayFeedback = f;
                    }
                } else {
                    fx.reverb = amount;
                    if (p.count("size")) {
                        const double z = numberParam(p, "size");
                        if (z < 0 || z > 1) return failed("size is 0..1");
                        fx.reverbSize = z;
                    }
                }
                return songResult(type + (amount > 0 ? " " + num(amount, 3) : std::string(" off")) + " on track " + std::to_string(ti) + " \"" +
                                      d.song.tracks[static_cast<std::size_t>(ti)].name + "\"",
                                  d, p);
            }));

}

void registerPatternCommands(Session& session) {
    auto& reg = session.registry();

    // ---- patterns ----
    reg.add({"beats.pattern_add",
             "Add pattern",
             "Add a pattern: step lanes for drums (steps=\"kick: x...x...x...x...; snare: ....x.......x...\"; x hit, X accent, "
             "o soft, . rest; a short lane repeats to fill the pattern) and/or piano-roll notes for synths (notes=\"C4 0 1, E4 1 "
             "1\" or chords=\"Am F C G\"). On synth tracks a lane named by a note (\"C2: x---x---\") plays that note, '-' "
             "holding it. Give track= (and bar=, repeat=) to place it right away.",
             [] {
                 std::vector<ParamSpec> v = {songParam(),
                                             {"name", ParamType::String, "pattern name (default p1, p2...)", false},
                                             {"bars", ParamType::Int, "length in bars (default 1, or what the notes need)", false},
                                             {"steps_per_beat", ParamType::Int, "step grid: 4 = 16th notes, 2 = 8ths, 3 = triplet 8ths", false, {}, std::int64_t{4}},
                                             {"steps", ParamType::String, "step lanes: \"kick: x...x...; hat: ..x...x.\"", false}};
                 for (auto& x : noteInputParams()) v.push_back(x);
                 v.push_back(trackParam(false));
                 v.push_back({"bar", ParamType::Int, "with track=: bar to place it at (default: after the track's last placement)", false});
                 v.push_back({"repeat", ParamType::Int, "with track=: times in a row", false, {}, std::int64_t{1}});
                 v.push_back(previewParam("the pattern"));
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                Song& s = d.song;
                Pattern pat;
                std::string name = stringParam(p, "name");
                if (name.empty()) {
                    int i = 1;
                    while (s.findPattern("p" + std::to_string(i))) ++i;
                    name = "p" + std::to_string(i);
                }
                if (s.findPattern(name)) return failed("there's already a pattern called '" + name + "' (use beats.pattern_steps or beats.pattern_notes to change it)");
                pat.name = name;
                pat.bars = static_cast<int>(intParam(p, "bars", 1));
                pat.stepsPerBeat = static_cast<int>(intParam(p, "steps_per_beat", 4));
                if (pat.bars < 1 || pat.bars > 256) return failed("bars must be 1..256");
                if (pat.stepsPerBeat < 1 || pat.stepsPerBeat > 16) return failed("steps_per_beat must be 1..16");
                std::string error;
                if (p.count("steps") && !parseLanes(stringParam(p, "steps"), pat.lanes, error)) return failed(error);
                pat.lanes.erase(std::remove_if(pat.lanes.begin(), pat.lanes.end(), [](const auto& l) { return l.second.empty(); }), pat.lanes.end());
                if (!notesInto(pat, p, s, error) || !chordsInto(pat, p, s, error)) return failed(error);
                std::string placed;
                if (p.count("track")) {
                    int ti = 0;
                    if (!trackArg(s, p, ti, error)) return failed(error);
                    int bar = 0;
                    if (p.count("bar")) {
                        bar = static_cast<int>(intParam(p, "bar"));
                    } else {
                        for (const auto& pl : s.arrangement) {
                            if (pl.track == ti) bar = std::max(bar, pl.bar + placementSpan(s, pl));
                        }
                    }
                    const int rep = static_cast<int>(intParam(p, "repeat", 1));
                    if (bar < 0 || rep < 1 || rep > 1024) return failed("bar must be >= 0 and repeat 1..1024");
                    s.patterns.push_back(pat);
                    s.arrangement.push_back({ti, pat.name, bar, rep});
                    placed = ", placed on track " + std::to_string(ti) + " at bar " + std::to_string(bar) + (rep > 1 ? " x" + std::to_string(rep) : "");
                } else {
                    s.patterns.push_back(pat);
                }
                return patternResult("added pattern \"" + pat.name + "\" (" + std::to_string(pat.bars) + (pat.bars == 1 ? " bar, " : " bars, ") +
                                         std::to_string(pat.lanes.size()) + " lanes, " + std::to_string(pat.notes.size()) + " notes)" + placed,
                                     d, s.patterns.back(), p);
            }));

    reg.add({"beats.pattern_set",
             "Pattern settings",
             "Rename a pattern (its placements follow), change its length in bars or its step grid resolution.",
             {songParam(), patternParam(),
              {"name", ParamType::String, "new name", false},
              {"bars", ParamType::Int, "length in bars", false},
              {"steps_per_beat", ParamType::Int, "step grid resolution", false},
              previewParam("the pattern")},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                if (!pat) return failed(error);
                if (p.count("bars")) {
                    const auto b = intParam(p, "bars");
                    if (b < 1 || b > 256) return failed("bars must be 1..256");
                    pat->bars = static_cast<int>(b);
                }
                if (p.count("steps_per_beat")) {
                    const auto v = intParam(p, "steps_per_beat");
                    if (v < 1 || v > 16) return failed("steps_per_beat must be 1..16");
                    pat->stepsPerBeat = static_cast<int>(v);
                }
                if (p.count("name")) {
                    const std::string n = stringParam(p, "name");
                    if (n.empty()) return failed("the name can't be empty");
                    const Pattern* other = d.song.findPattern(n);
                    if (other && other != pat) return failed("there's already a pattern called '" + n + "'");
                    for (auto& pl : d.song.arrangement) {
                        if (pl.pattern == pat->name) pl.pattern = n;
                    }
                    pat->name = n;
                }
                return patternResult("pattern \"" + pat->name + "\": " + std::to_string(pat->bars) + " bars, " + std::to_string(pat->stepsPerBeat) + " steps per beat", d, *pat, p);
            }));

    reg.add({"beats.pattern_steps",
             "Set steps",
             "Set step lanes of a pattern: lane=kick steps=\"x...x...x...x...\" sets one lane (steps=\"\" removes it), or "
             "steps=\"kick: x...x...; hat: ..x...x.\" sets several. x hit (velocity 100), X accent (127), o soft (60), . or - "
             "rest; spaces and | are ignored, so \"x... x... | x... x...\" is fine. A lane shorter than the pattern repeats.",
             {songParam(), patternParam(),
              {"lane", ParamType::String, "lane name: a drum voice (kick, snare...), a sample slot, or a note for synths (C2)", false},
              {"steps", ParamType::String, "the steps", true},
              previewParam("the pattern")},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                if (!pat) return failed(error);
                const std::string text = stringParam(p, "steps");
                if (p.count("lane")) {
                    std::string clean;
                    if (!cleanSteps(text, clean, error)) return failed(error);
                    const std::string lane = stringParam(p, "lane");
                    if (lane.empty()) return failed("lane needs a name");
                    setLane(*pat, lane, clean);
                } else {
                    std::vector<std::pair<std::string, std::string>> lanes;
                    if (!parseLanes(text, lanes, error)) return failed(error);
                    if (lanes.empty()) return failed("no lanes given");
                    for (const auto& [lane, steps] : lanes) setLane(*pat, lane, steps);
                }
                std::string m = "pattern \"" + pat->name + "\":";
                for (const auto& [lane, steps] : pat->lanes) m += "\n  " + lane + ": " + steps;
                return patternResult(m, d, *pat, p);
            }));

    reg.add({"beats.pattern_notes",
             "Set notes",
             "Add piano-roll notes to a pattern (mode=replace clears its notes first): notes=\"C4 0 1 100, E4 1 0.5\" (pitch as a "
             "name like C4/F#3/Bb2 or MIDI number, start and length in beats, optional velocity 1..127) and/or chords=\"Am F C "
             "G\" with chord_beats= each. The pattern grows to fit unless bars= is set with beats.pattern_set.",
             [] {
                 std::vector<ParamSpec> v = {songParam(), patternParam()};
                 for (auto& x : noteInputParams()) v.push_back(x);
                 v.push_back({"mode", ParamType::String, "add to the notes or replace them", false, {"add", "replace"}, std::string("add")});
                 v.push_back({"transpose", ParamType::Int, "shift every note of the pattern by semitones (after adding)", false});
                 v.push_back(previewParam("the pattern"));
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                if (!pat) return failed(error);
                if (!p.count("notes") && !p.count("chords") && !p.count("transpose")) return failed("give notes=, chords= or transpose=");
                if (stringParam(p, "mode", "add") == "replace") pat->notes.clear();
                Params q = p;
                q.erase("bars");
                if (!notesInto(*pat, q, d.song, error) || !chordsInto(*pat, q, d.song, error)) return failed(error);
                if (p.count("transpose")) {
                    const auto t = intParam(p, "transpose");
                    for (const auto& n : pat->notes) {
                        if (n.pitch + t < 0 || n.pitch + t > 127) return failed("transposing by " + std::to_string(t) + " takes notes outside 0..127");
                    }
                    for (auto& n : pat->notes) n.pitch += static_cast<int>(t);
                }
                std::sort(pat->notes.begin(), pat->notes.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
                return patternResult("pattern \"" + pat->name + "\" has " + std::to_string(pat->notes.size()) + " notes over " + std::to_string(pat->bars) +
                                         (pat->bars == 1 ? " bar" : " bars"),
                                     d, *pat, p);
            }));

    reg.add({"beats.pattern_clear",
             "Clear pattern",
             "Empty a pattern: what=all (steps and notes), steps, or notes; lane= clears just one step lane; from=/to= (beats) "
             "clears only the notes starting in that range.",
             {songParam(), patternParam(),
              {"what", ParamType::String, "what to clear", false, {"all", "steps", "notes"}, std::string("all")},
              {"lane", ParamType::String, "only this step lane", false},
              {"from", ParamType::Number, "notes: first beat", false},
              {"to", ParamType::Number, "notes: beat to stop at", false},
              previewParam("the pattern")},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                if (!pat) return failed(error);
                const std::string what = stringParam(p, "what", "all");
                if (p.count("lane")) {
                    const std::string lane = stringParam(p, "lane");
                    const auto before = pat->lanes.size();
                    setLane(*pat, lane, "");
                    if (pat->lanes.size() == before) return failed("the pattern has no lane '" + lane + "'");
                } else {
                    if (what != "notes") pat->lanes.clear();
                    if (what != "steps") {
                        const double from = numberParam(p, "from", 0), to = p.count("to") ? numberParam(p, "to") : 1e9;
                        auto& n = pat->notes;
                        n.erase(std::remove_if(n.begin(), n.end(), [&](const Note& x) { return x.start >= from - 1e-9 && x.start < to - 1e-9; }), n.end());
                    }
                }
                return patternResult("pattern \"" + pat->name + "\" now has " + std::to_string(pat->lanes.size()) + " lanes and " + std::to_string(pat->notes.size()) + " notes",
                                     d, *pat, p);
            }));

    reg.add({"beats.pattern_remove",
             "Remove pattern",
             "Delete a pattern and every placement of it.",
             {songParam(), patternParam(), previewParam()},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                if (!pat) return failed(error);
                const std::string name = pat->name;
                auto& arr = d.song.arrangement;
                arr.erase(std::remove_if(arr.begin(), arr.end(), [&](const Placement& pl) { return pl.pattern == name; }), arr.end());
                auto& pats = d.song.patterns;
                pats.erase(std::remove_if(pats.begin(), pats.end(), [&](const Pattern& x) { return x.name == name; }), pats.end());
                return songResult("removed pattern \"" + name + "\"", d, p);
            }));

}

void registerArrangeCommands(Session& session) {
    auto& reg = session.registry();

    // ---- arrangement ----
    reg.add({"beats.arrange_place",
             "Place pattern",
             "Place a pattern on a track starting at a bar (from 0), repeat= times in a row. Without bar= it goes after the "
             "track's last placement. Placements may overlap (both play).",
             {songParam(), trackParam(), patternParam(),
              {"bar", ParamType::Int, "bar to start at, from 0 (default: after the track's last placement)", false},
              {"repeat", ParamType::Int, "times in a row", false, {}, std::int64_t{1}},
              previewParam()},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                Song& s = d.song;
                int ti = 0;
                std::string error;
                if (!trackArg(s, p, ti, error)) return failed(error);
                Pattern* pat = patternArg(s, p, error);
                if (!pat) return failed(error);
                int bar = 0;
                if (p.count("bar")) {
                    bar = static_cast<int>(intParam(p, "bar"));
                } else {
                    for (const auto& pl : s.arrangement) {
                        if (pl.track == ti) bar = std::max(bar, pl.bar + placementSpan(s, pl));
                    }
                }
                const auto rep = intParam(p, "repeat", 1);
                if (bar < 0 || bar > 100000) return failed("bar must be 0..100000");
                if (rep < 1 || rep > 1024) return failed("repeat must be 1..1024");
                s.arrangement.push_back({ti, pat->name, bar, static_cast<int>(rep)});
                const int last = bar + pat->bars * static_cast<int>(rep) - 1;
                Result r = songResult("placed \"" + pat->name + "\" on track " + std::to_string(ti) + " \"" + s.tracks[static_cast<std::size_t>(ti)].name +
                                          "\", bars " + std::to_string(bar) + "-" + std::to_string(last),
                                      d, p);
                r.with("bar", static_cast<std::int64_t>(bar)).with("end_bar", static_cast<std::int64_t>(last + 1));
                return r;
            }));

    reg.add({"beats.arrange_remove",
             "Remove placement",
             "Remove placements from the arrangement: those on track= that play at bar= (or every one on that track without "
             "bar=), optionally only of pattern=. all=true clears the whole arrangement.",
             {songParam(), trackParam(false),
              {"bar", ParamType::Int, "remove the placement playing at this bar", false},
              patternParam(false),
              {"all", ParamType::Bool, "clear the whole arrangement", false, {}, false},
              previewParam()},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                Song& s = d.song;
                auto& arr = s.arrangement;
                const auto before = arr.size();
                if (boolParam(p, "all", false)) {
                    arr.clear();
                } else {
                    if (!p.count("track") && !p.count("pattern")) return failed("give track= (and bar=), pattern=, or all=true");
                    int ti = -1;
                    std::string error;
                    if (p.count("track") && !trackArg(s, p, ti, error)) return failed(error);
                    const bool byBar = p.count("bar") > 0;
                    const int bar = static_cast<int>(intParam(p, "bar", 0));
                    const std::string pname = stringParam(p, "pattern");
                    arr.erase(std::remove_if(arr.begin(), arr.end(),
                                             [&](const Placement& pl) {
                                                 if (ti >= 0 && pl.track != ti) return false;
                                                 if (!pname.empty() && pl.pattern != pname) return false;
                                                 if (byBar && (bar < pl.bar || bar >= pl.bar + placementSpan(s, pl))) return false;
                                                 return true;
                                             }),
                              arr.end());
                }
                const auto removed = before - arr.size();
                if (!removed) return failed("no placement matches");
                Result r = songResult("removed " + std::to_string(removed) + (removed == 1 ? " placement" : " placements"), d, p);
                r.with("removed", static_cast<std::int64_t>(removed));
                return r;
            }));
}

}  // namespace wetwaresuite::beats::tool
