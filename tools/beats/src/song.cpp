// Song model helpers: note names, chords, presets, patterns, events, JSON.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/beats/song.hpp"

namespace wetwaresuite::beats {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// "C", "C#", "Db" at the start of text -> pitch class; returns characters used (0 = none).
std::size_t pitchClass(const std::string& t, int& pc) {
    if (t.empty()) return 0;
    static const int base[] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
    if (c < 'a' || c > 'g') return 0;
    pc = base[c - 'a'];
    std::size_t used = 1;
    while (used < t.size() && (t[used] == '#' || t[used] == 'b')) {
        // 'b' is a flat only straight after the letter or another accidental
        pc += t[used] == '#' ? 1 : -1;
        ++used;
    }
    pc = ((pc % 12) + 12) % 12;
    return used;
}

}  // namespace

int parseNote(const std::string& raw) {
    std::string t;
    for (char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c))) t += c;
    }
    if (allDigits(t)) {
        const int v = std::stoi(t.size() > 4 ? "999" : t);
        return v <= 127 ? v : -1;
    }
    int pc = 0;
    const std::size_t used = pitchClass(t, pc);
    if (!used) return -1;
    // the 'b' loop above may have eaten nothing wrong: "B4" is letter B, "Bb4" is B flat
    std::string rest = t.substr(used);
    if (rest.empty()) return -1;
    bool neg = false;
    if (rest[0] == '-') {
        neg = true;
        rest.erase(0, 1);
    }
    if (!allDigits(rest) || rest.size() > 2) return -1;
    const int octave = std::stoi(rest) * (neg ? -1 : 1);
    const int midi = (octave + 1) * 12 + pc;
    return midi >= 0 && midi <= 127 ? midi : -1;
}

std::string noteName(int midi) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    midi = std::clamp(midi, 0, 127);
    return std::string(names[midi % 12]) + std::to_string(midi / 12 - 1);
}

double noteFrequency(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

bool parseNotes(const std::string& text, std::vector<Note>& out, std::string& error) {
    out.clear();
    std::string item;
    auto flush = [&]() -> bool {
        std::string s = item;
        item.clear();
        std::replace(s.begin(), s.end(), ':', ' ');
        std::istringstream in(s);
        std::vector<std::string> parts;
        for (std::string w; in >> w;) parts.push_back(w);
        if (parts.empty()) return true;
        if (parts.size() < 3 || parts.size() > 4) {
            error = "each note is \"pitch start length [velocity]\", e.g. \"C4 0 1 100\"; got \"" + s + "\"";
            return false;
        }
        Note n;
        n.pitch = parseNote(parts[0]);
        if (n.pitch < 0) {
            error = "\"" + parts[0] + "\" isn't a note (use a name like C4, F#3, Bb2 or a MIDI number 0..127)";
            return false;
        }
        try {
            std::size_t a = 0, b = 0;
            n.start = std::stod(parts[1], &a);
            n.length = std::stod(parts[2], &b);
            if (a != parts[1].size() || b != parts[2].size()) throw std::invalid_argument("x");
            if (parts.size() == 4) {
                std::size_t c = 0;
                n.velocity = static_cast<int>(std::lround(std::stod(parts[3], &c)));
                if (c != parts[3].size()) throw std::invalid_argument("x");
            }
        } catch (const std::exception&) {
            error = "start, length and velocity must be numbers in \"" + s + "\"";
            return false;
        }
        if (n.start < 0 || n.length <= 0 || n.velocity < 1 || n.velocity > 127) {
            error = "in \"" + s + "\": start must be >= 0, length > 0 and velocity 1..127";
            return false;
        }
        out.push_back(n);
        return true;
    };
    for (char c : text) {
        if (c == ',' || c == ';' || c == '\n') {
            if (!flush()) return false;
        } else {
            item += c;
        }
    }
    return flush();
}

std::string formatNotes(const std::vector<Note>& notes) {
    std::string s;
    char buf[96];
    for (const auto& n : notes) {
        std::snprintf(buf, sizeof buf, "%s %g %g %d", noteName(n.pitch).c_str(), n.start, n.length, n.velocity);
        s += (s.empty() ? "" : ", ") + std::string(buf);
    }
    return s;
}

std::vector<int> chordNotes(const std::string& symbol, int octave) {
    std::string t;
    for (char c : symbol) {
        if (!std::isspace(static_cast<unsigned char>(c))) t += c;
    }
    std::string bassPart;
    if (auto slash = t.find('/'); slash != std::string::npos) {
        bassPart = t.substr(slash + 1);
        t = t.substr(0, slash);
    }
    if (t.empty() || !std::isalpha(static_cast<unsigned char>(t[0]))) return {};
    int pc = 0;
    std::size_t used = 1;
    {
        static const int base[] = {9, 11, 0, 2, 4, 5, 7};
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
        if (c < 'a' || c > 'g') return {};
        pc = base[c - 'a'];
        if (used < t.size() && (t[used] == '#' || t[used] == 'b')) {
            pc += t[used] == '#' ? 1 : -1;
            ++used;
        }
    }
    const std::string q = t.substr(used);
    static const std::vector<std::pair<std::string, std::vector<int>>> qualities = {
        {"", {0, 4, 7}},          {"maj", {0, 4, 7}},       {"M", {0, 4, 7}},         {"m", {0, 3, 7}},
        {"min", {0, 3, 7}},       {"-", {0, 3, 7}},         {"7", {0, 4, 7, 10}},     {"maj7", {0, 4, 7, 11}},
        {"M7", {0, 4, 7, 11}},    {"m7", {0, 3, 7, 10}},    {"min7", {0, 3, 7, 10}},  {"dim", {0, 3, 6}},
        {"dim7", {0, 3, 6, 9}},   {"m7b5", {0, 3, 6, 10}},  {"aug", {0, 4, 8}},       {"+", {0, 4, 8}},
        {"sus2", {0, 2, 7}},      {"sus4", {0, 5, 7}},      {"sus", {0, 5, 7}},       {"add9", {0, 4, 7, 14}},
        {"madd9", {0, 3, 7, 14}}, {"6", {0, 4, 7, 9}},      {"m6", {0, 3, 7, 9}},     {"9", {0, 4, 7, 10, 14}},
        {"m9", {0, 3, 7, 10, 14}}, {"maj9", {0, 4, 7, 11, 14}}, {"5", {0, 7}},
    };
    const std::vector<int>* iv = nullptr;
    for (const auto& [name, v] : qualities) {
        if (name == q) iv = &v;
    }
    if (!iv) return {};
    const int root = (octave + 1) * 12 + ((pc % 12) + 12) % 12;
    std::vector<int> out;
    for (int i : *iv) out.push_back(root + i);
    if (!bassPart.empty()) {
        int bpc = 0;
        if (pitchClass(bassPart, bpc) != bassPart.size()) return {};
        int bass = (octave + 1) * 12 + bpc;
        while (bass >= root) bass -= 12;
        out.insert(out.begin(), bass);
    }
    for (int& n : out) n = std::clamp(n, 0, 127);
    return out;
}

// ---- kinds, oscillators, presets ----

std::string toString(TrackKind k) {
    switch (k) {
        case TrackKind::Synth: return "synth";
        case TrackKind::Drum: return "drum";
        case TrackKind::Sampler: return "sampler";
    }
    return "synth";
}

bool parseTrackKind(const std::string& text, TrackKind& out) {
    const std::string t = lower(text);
    if (t == "synth") out = TrackKind::Synth;
    else if (t == "drum" || t == "drums") out = TrackKind::Drum;
    else if (t == "sampler" || t == "sample") out = TrackKind::Sampler;
    else return false;
    return true;
}

const std::vector<std::string>& oscNames() {
    static const std::vector<std::string> v = {"saw", "square", "sine", "triangle", "noise", "off"};
    return v;
}

std::string toString(Osc o) { return oscNames()[static_cast<std::size_t>(o)]; }

bool parseOsc(const std::string& text, Osc& out) {
    const std::string t = lower(text);
    const auto& n = oscNames();
    for (std::size_t i = 0; i < n.size(); ++i) {
        if (n[i] == t) {
            out = static_cast<Osc>(i);
            return true;
        }
    }
    if (t == "sawtooth") out = Osc::Saw;
    else if (t == "tri") out = Osc::Triangle;
    else if (t == "none") out = Osc::Off;
    else return false;
    return true;
}

const std::vector<std::string>& presetNames() {
    static const std::vector<std::string> v = {"bass", "lead", "pad", "pluck", "keys", "sub", "brass"};
    return v;
}

bool preset(const std::string& name, SynthSettings& s) {
    s = {};
    const std::string n = lower(name);
    if (n == "bass") {
        s.osc1 = Osc::Saw, s.osc2 = Osc::Square, s.osc2Semitones = -12, s.osc2Level = 0.5;
        s.attack = 0.003, s.decay = 0.18, s.sustain = 0.6, s.release = 0.08;
        s.cutoff = 380, s.resonance = 0.3, s.filterEnv = 2.2, s.filterDecay = 0.18;
    } else if (n == "lead") {
        s.osc1 = Osc::Saw, s.osc2 = Osc::Square, s.detune = 12, s.osc2Level = 0.45;
        s.attack = 0.01, s.decay = 0.3, s.sustain = 0.75, s.release = 0.2;
        s.cutoff = 2400, s.resonance = 0.25, s.filterEnv = 1.2, s.filterDecay = 0.3;
    } else if (n == "pad") {
        s.osc1 = Osc::Saw, s.osc2 = Osc::Saw, s.detune = 18, s.osc2Level = 0.8;
        s.attack = 0.6, s.decay = 1.0, s.sustain = 0.8, s.release = 1.2;
        s.cutoff = 1600, s.resonance = 0.1, s.filterEnv = 0.5, s.filterDecay = 1.5;
    } else if (n == "pluck") {
        s.osc1 = Osc::Saw, s.osc2 = Osc::Triangle, s.osc2Semitones = 12, s.osc2Level = 0.3;
        s.attack = 0.001, s.decay = 0.3, s.sustain = 0.0, s.release = 0.25;
        s.cutoff = 500, s.resonance = 0.35, s.filterEnv = 3.0, s.filterDecay = 0.12;
    } else if (n == "keys") {
        s.osc1 = Osc::Triangle, s.osc2 = Osc::Sine, s.osc2Semitones = 12, s.osc2Level = 0.35;
        s.attack = 0.004, s.decay = 0.9, s.sustain = 0.35, s.release = 0.35;
        s.cutoff = 3200, s.resonance = 0.05, s.filterEnv = 0.6, s.filterDecay = 0.5;
    } else if (n == "sub") {
        s.osc1 = Osc::Sine, s.osc2 = Osc::Triangle, s.osc2Level = 0.15;
        s.attack = 0.004, s.decay = 0.3, s.sustain = 0.9, s.release = 0.1;
        s.cutoff = 900, s.resonance = 0.0;
    } else if (n == "brass") {
        s.osc1 = Osc::Saw, s.osc2 = Osc::Saw, s.detune = 8, s.osc2Level = 0.7;
        s.attack = 0.06, s.decay = 0.4, s.sustain = 0.7, s.release = 0.25;
        s.cutoff = 900, s.resonance = 0.15, s.filterEnv = 1.8, s.filterDecay = 0.35;
    } else {
        return false;
    }
    return true;
}

// ---- drums ----

const std::vector<std::string>& drumVoices() {
    static const std::vector<std::string> v = {"kick", "snare", "hat",   "openhat", "clap",  "tom",
                                               "hitom", "lotom", "rim", "crash",   "cowbell", "shaker"};
    return v;
}

bool isDrumVoice(const std::string& name) {
    const auto& v = drumVoices();
    return std::find(v.begin(), v.end(), lower(name)) != v.end();
}

int drumMidi(const std::string& voice) {
    static const std::map<std::string, int> m = {{"kick", 36},  {"snare", 38}, {"hat", 42},  {"openhat", 46},
                                                 {"clap", 39},  {"tom", 47},   {"hitom", 50}, {"lotom", 43},
                                                 {"rim", 37},   {"crash", 49}, {"cowbell", 56}, {"shaker", 70}};
    auto it = m.find(lower(voice));
    return it == m.end() ? -1 : it->second;
}

std::string drumVoiceForMidi(int n) {
    switch (n) {
        case 35: case 36: return "kick";
        case 37: return "rim";
        case 38: case 40: return "snare";
        case 39: return "clap";
        case 42: case 44: return "hat";
        case 46: return "openhat";
        case 41: case 43: case 45: return "lotom";
        case 47: case 48: return "tom";
        case 50: return "hitom";
        case 49: case 51: case 52: case 53: case 55: case 57: case 59: return "crash";
        case 56: return "cowbell";
        case 54: case 69: case 70: case 82: return "shaker";
        default: break;
    }
    if (n < 38) return "kick";
    if (n < 41) return "snare";
    if (n < 49) return "tom";
    return "shaker";
}

// ---- patterns ----

bool cleanSteps(const std::string& text, std::string& out, std::string& error) {
    out.clear();
    for (char c : text) {
        if (c == ' ' || c == '|' || c == '\t' || c == '_') continue;
        if (c == 'x' || c == 'X' || c == 'o' || c == 'O' || c == '.' || c == '-' || c == '0' || c == '1') {
            out += c == '1' ? 'x' : c == '0' ? '.' : c == 'O' ? 'o' : c;
        } else {
            error = std::string("steps use x (hit), X (accent), o (soft) and . or - (rest); '") + c + "' isn't one of them";
            return false;
        }
    }
    return true;
}

int stepVelocity(char c) { return c == 'X' ? 127 : c == 'x' ? 100 : c == 'o' ? 60 : 0; }

int Song::bars() const {
    int end = 0;
    for (const auto& pl : arrangement) {
        const Pattern* p = findPattern(pl.pattern);
        const int len = p ? p->bars : 1;
        end = std::max(end, pl.bar + len * std::max(1, pl.repeat));
    }
    return end;
}

Pattern* Song::findPattern(const std::string& n) {
    for (auto& p : patterns) {
        if (p.name == n) return &p;
    }
    for (auto& p : patterns) {
        if (lower(p.name) == lower(n)) return &p;
    }
    return nullptr;
}

const Pattern* Song::findPattern(const std::string& n) const { return const_cast<Song*>(this)->findPattern(n); }

int findTrack(const Song& s, const std::string& ref) {
    if (allDigits(ref)) {
        const long v = std::stol(ref.size() > 6 ? "999999" : ref);
        return v < static_cast<long>(s.tracks.size()) ? static_cast<int>(v) : -1;
    }
    for (std::size_t i = 0; i < s.tracks.size(); ++i) {
        if (lower(s.tracks[i].name) == lower(ref)) return static_cast<int>(i);
    }
    return -1;
}

std::vector<Event> patternEvents(const Song& s, const Pattern& p, TrackKind kind) {
    std::vector<Event> ev;
    const int steps = static_cast<int>(std::lround(p.bars * s.beatsInBar() * p.stepsPerBeat));
    const double stepBeats = 1.0 / p.stepsPerBeat;
    for (const auto& [lane, pat] : p.lanes) {
        if (pat.empty()) continue;
        int pitch = 60;
        if (kind != TrackKind::Drum) {
            pitch = parseNote(lane);
            if (pitch < 0) continue;
        }
        for (int i = 0; i < steps; ++i) {
            const char c = pat[static_cast<std::size_t>(i) % pat.size()];  // short lanes repeat
            const int vel = stepVelocity(c);
            if (!vel) continue;
            // synth lanes: a hit holds until the next hit or rest-free run ends ("x---" = 4 steps long)
            int len = 1;
            if (kind != TrackKind::Drum) {
                while (i + len < steps && pat[static_cast<std::size_t>(i + len) % pat.size()] == '-') ++len;
            }
            Event e;
            e.beat = i * stepBeats;
            e.length = len * stepBeats;
            e.velocity = vel;
            e.pitch = kind == TrackKind::Drum ? drumMidi(lane) : pitch;
            if (kind == TrackKind::Drum) e.voice = lower(lane);
            ev.push_back(e);
        }
    }
    const double patBeats = p.bars * s.beatsInBar();
    for (const auto& n : p.notes) {
        if (n.start >= patBeats - 1e-9) continue;
        Event e;
        e.beat = n.start;
        e.length = std::min(n.length, patBeats - n.start);
        e.pitch = n.pitch;
        e.velocity = n.velocity;
        if (kind == TrackKind::Drum) e.voice = drumVoiceForMidi(n.pitch);
        ev.push_back(e);
    }
    std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.beat < b.beat; });
    return ev;
}

std::vector<Event> trackEvents(const Song& s, int track) {
    std::vector<Event> out;
    if (track < 0 || track >= static_cast<int>(s.tracks.size())) return out;
    const TrackKind kind = s.tracks[static_cast<std::size_t>(track)].kind;
    for (const auto& pl : s.arrangement) {
        if (pl.track != track) continue;
        const Pattern* p = s.findPattern(pl.pattern);
        if (!p) continue;
        const auto ev = patternEvents(s, *p, kind);
        const double half = 0.5 / p->stepsPerBeat;  // swing pushes odd steps up to half a step late
        for (int r = 0; r < std::max(1, pl.repeat); ++r) {
            const double offset = (pl.bar + r * p->bars) * s.beatsInBar();
            for (Event e : ev) {
                const double step = e.beat * p->stepsPerBeat;
                const double nearest = std::round(step);
                if (s.swing > 0 && std::fabs(step - nearest) < 1e-6 && static_cast<long long>(nearest) % 2 == 1) e.beat += s.swing * half;
                e.beat += offset;
                out.push_back(e);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Event& a, const Event& b) { return a.beat < b.beat; });
    return out;
}

// ---- files ----

Sample loadSample(const std::string& path, int rate, const fs::path& relativeTo) {
    fs::path p = path;
    std::error_code ec;
    if (!fs::exists(p, ec) && p.is_relative() && !relativeTo.empty()) p = relativeTo / p;
    audio::Buffer b = audio::load(p);
    b = audio::conform(b, rate, 1);
    Sample s;
    s.path = path;
    s.audio = std::make_shared<const audio::Buffer>(std::move(b));
    return s;
}

namespace {

std::string samplePathForFile(const std::string& path, const fs::path& songFile) {
    if (songFile.empty()) return path;
    std::error_code ec;
    const fs::path abs = fs::absolute(path, ec);
    const fs::path dir = fs::absolute(songFile, ec).parent_path();
    const fs::path rel = abs.lexically_relative(dir);
    if (!rel.empty() && rel.native()[0] != '.') return rel.generic_string();
    return path;
}

json synthJson(const SynthSettings& s) {
    return {{"osc1", toString(s.osc1)},        {"osc2", toString(s.osc2)},          {"osc2_semitones", s.osc2Semitones},
            {"detune", s.detune},              {"osc2_level", s.osc2Level},         {"octave", s.octave},
            {"attack", s.attack},              {"decay", s.decay},                  {"sustain", s.sustain},
            {"release", s.release},            {"cutoff", s.cutoff},                {"resonance", s.resonance},
            {"filter_env", s.filterEnv},       {"filter_decay", s.filterDecay}};
}

SynthSettings synthFrom(const json& j, SynthSettings s) {
    if (!j.is_object()) return s;
    parseOsc(j.value("osc1", std::string("saw")), s.osc1);
    parseOsc(j.value("osc2", std::string("off")), s.osc2);
    s.osc2Semitones = j.value("osc2_semitones", s.osc2Semitones);
    s.detune = j.value("detune", s.detune);
    s.osc2Level = j.value("osc2_level", s.osc2Level);
    s.octave = j.value("octave", s.octave);
    s.attack = j.value("attack", s.attack);
    s.decay = j.value("decay", s.decay);
    s.sustain = j.value("sustain", s.sustain);
    s.release = j.value("release", s.release);
    s.cutoff = j.value("cutoff", s.cutoff);
    s.resonance = j.value("resonance", s.resonance);
    s.filterEnv = j.value("filter_env", s.filterEnv);
    s.filterDecay = j.value("filter_decay", s.filterDecay);
    return s;
}

}  // namespace

std::string toJson(const Song& s, const fs::path& songFile) {
    json j;
    j["format"] = "wetwaresuite-beats";
    j["version"] = 1;
    j["name"] = s.name;
    j["bpm"] = s.bpm;
    j["time_signature"] = std::to_string(s.beatsPerBar) + "/" + std::to_string(s.beatUnit);
    j["swing"] = s.swing;
    j["rate"] = s.rate;
    j["master_db"] = s.masterDb;
    if (!s.key.empty()) j["key"] = s.key;
    j["tracks"] = json::array();
    for (const auto& t : s.tracks) {
        json tj = {{"name", t.name}, {"kind", toString(t.kind)}};
        if (!t.presetName.empty()) tj["preset"] = t.presetName;
        if (t.kind != TrackKind::Drum) tj["synth"] = synthJson(t.synth);
        if (t.kind == TrackKind::Sampler) {
            tj["sample"] = samplePathForFile(t.sample.path, songFile);
            tj["root"] = noteName(t.root);
        }
        if (!t.slots.empty()) {
            json sj = json::object();
            for (const auto& [lane, smp] : t.slots) sj[lane] = samplePathForFile(smp.path, songFile);
            tj["slots"] = sj;
        }
        tj["volume_db"] = t.volumeDb;
        tj["pan"] = t.pan;
        tj["mute"] = t.mute;
        tj["solo"] = t.solo;
        tj["effects"] = {{"distortion", t.fx.distortion}, {"compress", t.fx.compress},     {"delay", t.fx.delayMix},
                         {"delay_beats", t.fx.delayBeats}, {"delay_feedback", t.fx.delayFeedback}, {"reverb", t.fx.reverb},
                         {"reverb_size", t.fx.reverbSize}};
        j["tracks"].push_back(tj);
    }
    j["patterns"] = json::array();
    for (const auto& p : s.patterns) {
        json pj = {{"name", p.name}, {"bars", p.bars}, {"steps_per_beat", p.stepsPerBeat}};
        json lanes = json::object();
        for (const auto& [lane, steps] : p.lanes) lanes[lane] = steps;
        pj["steps"] = lanes;
        json notes = json::array();
        for (const auto& n : p.notes) notes.push_back({noteName(n.pitch), n.start, n.length, n.velocity});
        pj["notes"] = notes;
        j["patterns"].push_back(pj);
    }
    j["arrangement"] = json::array();
    for (const auto& pl : s.arrangement) {
        j["arrangement"].push_back({{"track", pl.track}, {"pattern", pl.pattern}, {"bar", pl.bar}, {"repeat", pl.repeat}});
    }
    return j.dump(2) + "\n";
}

Song fromJson(const std::string& text, const fs::path& songFile) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not a valid .beats.json file: ") + e.what());
    }
    const std::string format = j.is_object() ? j.value("format", std::string()) : std::string();
    if (format != "wetwaresuite-beats" && format != "achclwss-beats") throw std::runtime_error("not a beats song (no \"format\": \"wetwaresuite-beats\")");
    const fs::path dir = songFile.empty() ? fs::path() : songFile.parent_path();
    try {
        Song s;
        s.name = j.value("name", s.name);
        s.bpm = std::clamp(j.value("bpm", s.bpm), 20.0, 400.0);
        const std::string sig = j.value("time_signature", std::string("4/4"));
        if (auto slash = sig.find('/'); slash != std::string::npos) {
            s.beatsPerBar = std::clamp(std::stoi(sig.substr(0, slash)), 1, 32);
            s.beatUnit = std::clamp(std::stoi(sig.substr(slash + 1)), 1, 32);
        }
        s.swing = std::clamp(j.value("swing", 0.0), 0.0, 1.0);
        s.rate = std::clamp(j.value("rate", 44100), 8000, 192000);
        s.masterDb = j.value("master_db", 0.0);
        s.key = j.value("key", std::string());
        for (const auto& tj : j.value("tracks", json::array())) {
            Track t;
            t.name = tj.value("name", std::string("track"));
            parseTrackKind(tj.value("kind", std::string("synth")), t.kind);
            t.presetName = tj.value("preset", std::string());
            if (!t.presetName.empty()) preset(t.presetName, t.synth);  // the preset first, then any settings written out
            if (tj.contains("synth")) t.synth = synthFrom(tj["synth"], t.synth);
            auto tryLoad = [&](const std::string& path) {
                try {
                    return loadSample(path, s.rate, dir);
                } catch (const std::exception&) {
                    Sample missing;
                    missing.path = path;
                    return missing;
                }
            };
            if (tj.contains("sample") && tj["sample"].is_string() && !tj["sample"].get<std::string>().empty()) t.sample = tryLoad(tj["sample"].get<std::string>());
            if (tj.contains("root")) {
                const int r = tj["root"].is_number() ? tj["root"].get<int>() : parseNote(tj["root"].get<std::string>());
                if (r >= 0) t.root = r;
            }
            if (tj.contains("slots") && tj["slots"].is_object()) {
                for (const auto& [lane, path] : tj["slots"].items()) t.slots[lane] = tryLoad(path.get<std::string>());
            }
            t.volumeDb = tj.value("volume_db", 0.0);
            t.pan = std::clamp(tj.value("pan", 0.0), -1.0, 1.0);
            t.mute = tj.value("mute", false);
            t.solo = tj.value("solo", false);
            if (tj.contains("effects")) {
                const auto& e = tj["effects"];
                t.fx.distortion = e.value("distortion", 0.0);
                t.fx.compress = e.value("compress", 0.0);
                t.fx.delayMix = e.value("delay", 0.0);
                t.fx.delayBeats = e.value("delay_beats", t.fx.delayBeats);
                t.fx.delayFeedback = e.value("delay_feedback", t.fx.delayFeedback);
                t.fx.reverb = e.value("reverb", 0.0);
                t.fx.reverbSize = e.value("reverb_size", t.fx.reverbSize);
            }
            s.tracks.push_back(std::move(t));
        }
        for (const auto& pj : j.value("patterns", json::array())) {
            Pattern p;
            p.name = pj.value("name", std::string("p") + std::to_string(s.patterns.size() + 1));
            p.bars = std::clamp(pj.value("bars", 1), 1, 256);
            p.stepsPerBeat = std::clamp(pj.value("steps_per_beat", 4), 1, 16);
            if (pj.contains("steps") && pj["steps"].is_object()) {
                for (const auto& [lane, steps] : pj["steps"].items()) {
                    std::string clean, err;
                    if (cleanSteps(steps.get<std::string>(), clean, err)) p.lanes.emplace_back(lane, clean);
                }
            }
            for (const auto& nj : pj.value("notes", json::array())) {
                if (!nj.is_array() || nj.size() < 3) continue;
                Note n;
                n.pitch = nj[0].is_number() ? nj[0].get<int>() : parseNote(nj[0].get<std::string>());
                n.start = nj[1].get<double>();
                n.length = nj[2].get<double>();
                if (nj.size() > 3) n.velocity = std::clamp(nj[3].get<int>(), 1, 127);
                if (n.pitch >= 0 && n.pitch <= 127 && n.length > 0 && n.start >= 0) p.notes.push_back(n);
            }
            s.patterns.push_back(std::move(p));
        }
        for (const auto& aj : j.value("arrangement", json::array())) {
            Placement pl;
            pl.track = aj.value("track", 0);
            pl.pattern = aj.value("pattern", std::string());
            pl.bar = std::max(0, aj.value("bar", 0));
            pl.repeat = std::max(1, aj.value("repeat", 1));
            if (pl.track >= 0 && pl.track < static_cast<int>(s.tracks.size())) s.arrangement.push_back(pl);
        }
        return s;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("bad value in the song file: ") + e.what());
    }
}

void saveSong(const Song& s, const fs::path& path) {
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary);
    const std::string text = toJson(s, path);
    if (!out.write(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("can't write " + path.string());
}

Song loadSong(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("file not found: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return fromJson(ss.str(), path);
}

}  // namespace wetwaresuite::beats
