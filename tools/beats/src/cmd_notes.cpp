// The piano roll as commands: list, add, change, move, transpose, quantize and
// remove the notes of a pattern, each one undoable step. Notes are picked by
// number (notes=0,3,4 as beats.notes lists them) or, without notes=, all of
// them (optionally only those in from..to beats or low..high pitches).
#include <algorithm>
#include <cmath>

#include "common.hpp"

namespace wetwaresuite::beats::tool {

namespace {

std::vector<ParamSpec> selectParams() {
    return {{"notes", ParamType::NumberList, "note numbers from beats.notes (default: every note, narrowed by from/to/low/high)", false},
            {"from", ParamType::Number, "only notes starting at or after this beat", false},
            {"to", ParamType::Number, "only notes starting before this beat", false},
            {"low", ParamType::String, "only notes at or above this pitch, e.g. C3", false},
            {"high", ParamType::String, "only notes at or below this pitch", false}};
}

// Indices of the chosen notes; false with error when an index is bad or nothing matches.
bool selectNotes(const Pattern& pat, const Params& p, std::vector<std::size_t>& out, std::string& error) {
    out.clear();
    if (p.count("notes")) {
        for (double v : listParam(p, "notes")) {
            const auto i = static_cast<long>(v);
            if (i < 0 || i >= static_cast<long>(pat.notes.size()) || i != v) {
                error = "no note " + toText(v) + " in \"" + pat.name + "\" (it has " + std::to_string(pat.notes.size()) + ", numbered from 0)";
                return false;
            }
            if (std::find(out.begin(), out.end(), static_cast<std::size_t>(i)) == out.end()) out.push_back(static_cast<std::size_t>(i));
        }
    } else {
        int low = 0, high = 127;
        if (p.count("low") && (low = parseNote(stringParam(p, "low"))) < 0) return error = "low is a note like C3", false;
        if (p.count("high") && (high = parseNote(stringParam(p, "high"))) < 0) return error = "high is a note like C5", false;
        const double from = numberParam(p, "from", -1e9), to = p.count("to") ? numberParam(p, "to") : 1e9;
        for (std::size_t i = 0; i < pat.notes.size(); ++i) {
            const Note& n = pat.notes[i];
            if (n.pitch >= low && n.pitch <= high && n.start >= from - 1e-9 && n.start < to - 1e-9) out.push_back(i);
        }
    }
    if (out.empty()) return error = pat.notes.empty() ? "\"" + pat.name + "\" has no notes yet (beats.note_add adds one)" : "no notes match", false;
    return true;
}

// Re-sorts the notes and maps the chosen ones to their new numbers.
NumberList resort(Pattern& pat, const std::vector<std::size_t>& chosen) {
    std::vector<std::pair<Note, bool>> tagged;
    for (std::size_t i = 0; i < pat.notes.size(); ++i) tagged.emplace_back(pat.notes[i], std::find(chosen.begin(), chosen.end(), i) != chosen.end());
    std::stable_sort(tagged.begin(), tagged.end(), [](const auto& a, const auto& b) {
        return a.first.start != b.first.start ? a.first.start < b.first.start : a.first.pitch < b.first.pitch;
    });
    NumberList now;
    for (std::size_t i = 0; i < tagged.size(); ++i) {
        pat.notes[i] = tagged[i].first;
        if (tagged[i].second) now.push_back(static_cast<double>(i));
    }
    return now;
}

// Grows the pattern when notes run past its end.
void fit(Pattern& pat, const Song& s) {
    double end = 0;
    for (const auto& n : pat.notes) end = std::max(end, n.start + n.length);
    const int need = static_cast<int>(std::ceil(end / s.beatsInBar() - 1e-9));
    pat.bars = std::clamp(std::max(pat.bars, need), 1, 256);
}

std::string notesRows(const Pattern& pat) {
    std::string rows;
    for (std::size_t i = 0; i < pat.notes.size(); ++i) {
        const Note& n = pat.notes[i];
        rows += std::to_string(i) + "\t" + std::to_string(n.pitch) + "\t" + noteName(n.pitch) + "\t" + num(n.start, 8) + "\t" + num(n.length, 8) + "\t" +
                std::to_string(n.velocity) + "\n";
    }
    return rows;
}

Result changed(const std::string& what, SongDoc& d, Pattern& pat, const NumberList& now, const Params& p) {
    Result r = patternResult(what, d, pat, p);
    r.with("notes", now).with("count", static_cast<std::int64_t>(pat.notes.size()));
    return r;
}

}  // namespace

void registerNoteCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"beats.notes",
             "List notes",
             "The piano roll of a pattern: every note's number, pitch, start and length (beats) and velocity. rows has one "
             "line per note: number, MIDI pitch, name, start, length, velocity.",
             {songParam(), patternParam(), previewParam("the piano roll")},
             false, true},
            songCommand(
                session,
                [](SongDoc& d, const Params& p) {
                    std::string error;
                    Pattern* pat = patternArg(d.song, p, error);
                    if (!pat) return failed(error);
                    std::string text;
                    for (std::size_t i = 0; i < pat->notes.size() && i < 64; ++i) {
                        const Note& n = pat->notes[i];
                        text += "\n  " + std::to_string(i) + ": " + noteName(n.pitch) + " at beat " + num(n.start) + " for " + num(n.length) + " beats, velocity " + std::to_string(n.velocity);
                    }
                    if (pat->notes.size() > 64) text += "\n  ... " + std::to_string(pat->notes.size() - 64) + " more";
                    Result r = patternResult("\"" + pat->name + "\": " + std::to_string(pat->notes.size()) + " notes over " + std::to_string(pat->bars) +
                                                 (pat->bars == 1 ? " bar" : " bars") + text,
                                             d, *pat, p);
                    r.with("rows", notesRows(*pat))
                        .with("bars", static_cast<std::int64_t>(pat->bars))
                        .with("beats_per_bar", d.song.beatsInBar())
                        .with("count", static_cast<std::int64_t>(pat->notes.size()));
                    return r;
                },
                false));

    reg.add({"beats.note_add",
             "Add note",
             "Put one note in a pattern's piano roll: pitch (C4, F#3 or a MIDI number), start and length in beats, "
             "velocity 1..127. The pattern grows to fit. Returns the note's number.",
             {songParam(), patternParam(), {"pitch", ParamType::String, "note, e.g. C4 or 60", true},
              {"start", ParamType::Number, "beat it starts on, from 0", true}, {"length", ParamType::Number, "beats", false, {}, 1.0},
              {"velocity", ParamType::Int, "1..127", false, {}, std::int64_t{100}}, previewParam("the piano roll")},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                if (!pat) return failed(error);
                Note n;
                n.pitch = parseNote(stringParam(p, "pitch"));
                n.start = numberParam(p, "start");
                n.length = numberParam(p, "length", 1.0);
                n.velocity = static_cast<int>(intParam(p, "velocity", 100));
                if (n.pitch < 0 || n.pitch > 127) return failed("pitch is a note like C4 or a MIDI number 0..127");
                if (n.start < 0 || n.start > 4096) return failed("start must be 0..4096 beats");
                if (n.length <= 0 || n.length > 1024) return failed("length must be more than 0 beats");
                if (n.velocity < 1 || n.velocity > 127) return failed("velocity must be 1..127");
                pat->notes.push_back(n);
                const NumberList now = resort(*pat, {pat->notes.size() - 1});
                fit(*pat, d.song);
                Result r = changed("added " + noteName(n.pitch) + " at beat " + num(n.start) + " to \"" + pat->name + "\"", d, *pat, now, p);
                r.with("note", static_cast<std::int64_t>(now.front()));
                return r;
            }));

    reg.add({"beats.note_set",
             "Change notes",
             "Set the pitch, start, length and/or velocity of chosen notes (notes=2 or notes=0,1,5): what the piano roll does "
             "when you drag a note, resize it or change its velocity.",
             [] {
                 std::vector<ParamSpec> v = {songParam(), patternParam()};
                 for (auto& x : selectParams()) v.push_back(x);
                 v.push_back({"pitch", ParamType::String, "new pitch", false});
                 v.push_back({"start", ParamType::Number, "new start, beats", false});
                 v.push_back({"length", ParamType::Number, "new length, beats", false});
                 v.push_back({"velocity", ParamType::Int, "new velocity 1..127", false});
                 v.push_back(previewParam("the piano roll"));
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                std::vector<std::size_t> sel;
                if (!pat || !selectNotes(*pat, p, sel, error)) return failed(error);
                if (!p.count("pitch") && !p.count("start") && !p.count("length") && !p.count("velocity")) return failed("give pitch=, start=, length= or velocity=");
                const int pitch = p.count("pitch") ? parseNote(stringParam(p, "pitch")) : 0;
                if (p.count("pitch") && (pitch < 0 || pitch > 127)) return failed("pitch is a note like C4 or a MIDI number");
                if (p.count("start") && numberParam(p, "start") < 0) return failed("start can't be negative");
                if (p.count("length") && numberParam(p, "length") <= 0) return failed("length must be more than 0");
                const auto vel = intParam(p, "velocity", 100);
                if (vel < 1 || vel > 127) return failed("velocity must be 1..127");
                for (std::size_t i : sel) {
                    Note& n = pat->notes[i];
                    if (p.count("pitch")) n.pitch = pitch;
                    if (p.count("start")) n.start = numberParam(p, "start");
                    if (p.count("length")) n.length = numberParam(p, "length");
                    if (p.count("velocity")) n.velocity = static_cast<int>(vel);
                }
                const NumberList now = resort(*pat, sel);
                fit(*pat, d.song);
                return changed("changed " + std::to_string(sel.size()) + (sel.size() == 1 ? " note" : " notes"), d, *pat, now, p);
            }));

    reg.add({"beats.note_move",
             "Move notes",
             "Move chosen notes (default all) by shift beats and/or semitones, scale their length (length_scale=0.5 halves "
             "them) and change their velocity by velocity= (+10 louder, -20 softer).",
             [] {
                 std::vector<ParamSpec> v = {songParam(), patternParam()};
                 for (auto& x : selectParams()) v.push_back(x);
                 v.push_back({"shift", ParamType::Number, "beats later (negative: earlier)", false, {}, 0.0});
                 v.push_back({"semitones", ParamType::Int, "up (negative: down)", false, {}, std::int64_t{0}});
                 v.push_back({"length_scale", ParamType::Number, "multiply lengths", false, {}, 1.0});
                 v.push_back({"velocity", ParamType::Int, "add to velocities", false, {}, std::int64_t{0}});
                 v.push_back(previewParam("the piano roll"));
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                std::vector<std::size_t> sel;
                if (!pat || !selectNotes(*pat, p, sel, error)) return failed(error);
                const double shift = numberParam(p, "shift", 0.0), scale = numberParam(p, "length_scale", 1.0);
                const auto semis = intParam(p, "semitones", 0), dv = intParam(p, "velocity", 0);
                if (!(scale > 0 && scale <= 64)) return failed("length_scale must be more than 0");
                for (std::size_t i : sel) {
                    const Note& n = pat->notes[i];
                    if (n.start + shift < -1e-9) return failed("that moves note " + std::to_string(i) + " before the start of the pattern");
                    if (n.pitch + semis < 0 || n.pitch + semis > 127) return failed("that takes note " + std::to_string(i) + " outside the MIDI range");
                }
                for (std::size_t i : sel) {
                    Note& n = pat->notes[i];
                    n.start = std::max(0.0, n.start + shift);
                    n.pitch += static_cast<int>(semis);
                    n.length *= scale;
                    n.velocity = static_cast<int>(std::clamp<std::int64_t>(n.velocity + dv, 1, 127));
                }
                const NumberList now = resort(*pat, sel);
                fit(*pat, d.song);
                return changed("moved " + std::to_string(sel.size()) + (sel.size() == 1 ? " note" : " notes"), d, *pat, now, p);
            }));

    reg.add({"beats.note_transpose",
             "Transpose notes",
             "Shift the pitch of chosen notes (default all) by semitones (12 = an octave up).",
             [] {
                 std::vector<ParamSpec> v = {songParam(), patternParam(), {"semitones", ParamType::Int, "up (negative: down)", true}};
                 for (auto& x : selectParams()) v.push_back(x);
                 v.push_back(previewParam("the piano roll"));
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                std::vector<std::size_t> sel;
                if (!pat || !selectNotes(*pat, p, sel, error)) return failed(error);
                const auto semis = intParam(p, "semitones");
                for (std::size_t i : sel)
                    if (pat->notes[i].pitch + semis < 0 || pat->notes[i].pitch + semis > 127) return failed("that takes notes outside the MIDI range 0..127");
                for (std::size_t i : sel) pat->notes[i].pitch += static_cast<int>(semis);
                const NumberList now = resort(*pat, sel);
                return changed("transposed " + std::to_string(sel.size()) + " notes by " + std::to_string(semis) + " semitones", d, *pat, now, p);
            }));

    reg.add({"beats.note_quantize",
             "Quantize notes",
             "Snap chosen notes (default all) to a grid in beats (0.25 = 16th notes, 0.5 = 8ths, 1/3 = 8th triplets): "
             "strength 1 snaps fully, 0.5 halfway (keeps some feel); ends=true also snaps where notes stop.",
             [] {
                 std::vector<ParamSpec> v = {songParam(), patternParam(), {"grid", ParamType::Number, "grid size in beats", false, {}, 0.25},
                                             {"strength", ParamType::Number, "0..1, how far to move", false, {}, 1.0},
                                             {"ends", ParamType::Bool, "also snap the note ends", false, {}, false}};
                 for (auto& x : selectParams()) v.push_back(x);
                 v.push_back(previewParam("the piano roll"));
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                std::vector<std::size_t> sel;
                if (!pat || !selectNotes(*pat, p, sel, error)) return failed(error);
                const double grid = numberParam(p, "grid", 0.25), strength = numberParam(p, "strength", 1.0);
                if (!(grid >= 1.0 / 64 && grid <= 16)) return failed("grid must be 1/64..16 beats");
                if (strength < 0 || strength > 1) return failed("strength must be 0..1");
                int movedCount = 0;
                for (std::size_t i : sel) {
                    Note& n = pat->notes[i];
                    const double end = n.start + n.length;
                    const double start = n.start + (std::round(n.start / grid) * grid - n.start) * strength;
                    double newEnd = end - n.start + start;
                    if (boolParam(p, "ends", false)) {
                        const double snapped = std::max(start + grid, std::round(end / grid) * grid);
                        newEnd = end + (snapped - end) * strength;
                    }
                    if (std::fabs(start - n.start) > 1e-9 || std::fabs(newEnd - end) > 1e-9) ++movedCount;
                    n.start = std::max(0.0, start);
                    n.length = std::max(1e-3, newEnd - n.start);
                }
                const NumberList now = resort(*pat, sel);
                fit(*pat, d.song);
                Result r = changed("quantized " + std::to_string(sel.size()) + " notes to " + num(grid) + " beats (" + std::to_string(movedCount) + " moved)", d, *pat, now, p);
                r.with("moved", static_cast<std::int64_t>(movedCount));
                return r;
            }));

    reg.add({"beats.note_remove",
             "Remove notes",
             "Delete chosen notes from a pattern (notes=1,2; or every note in from..to / low..high; all=true for all).",
             [] {
                 std::vector<ParamSpec> v = {songParam(), patternParam(), {"all", ParamType::Bool, "every note", false, {}, false}};
                 for (auto& x : selectParams()) v.push_back(x);
                 v.push_back(previewParam("the piano roll"));
                 return v;
             }(),
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                std::string error;
                Pattern* pat = patternArg(d.song, p, error);
                if (!pat) return failed(error);
                std::vector<std::size_t> sel;
                if (!boolParam(p, "all", false)) {
                    if (!p.count("notes") && !p.count("from") && !p.count("to") && !p.count("low") && !p.count("high"))
                        return failed("say which notes: notes=, from=/to=, low=/high=, or all=true");
                    if (!selectNotes(*pat, p, sel, error)) return failed(error);
                } else {
                    for (std::size_t i = 0; i < pat->notes.size(); ++i) sel.push_back(i);
                }
                std::sort(sel.rbegin(), sel.rend());
                for (std::size_t i : sel) pat->notes.erase(pat->notes.begin() + static_cast<std::ptrdiff_t>(i));
                return changed("removed " + std::to_string(sel.size()) + (sel.size() == 1 ? " note" : " notes"), d, *pat, {}, p);
            }));
}

}  // namespace wetwaresuite::beats::tool
