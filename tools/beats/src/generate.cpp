// beats.generate: a quick starting point from a genre, a key and a seed.
// Drum grooves come from a small per-genre table with seeded variations; the
// chord progression, bass line and melody are built from the key's scale.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#include "achclwss/beats/song.hpp"

namespace achclwss::beats {

namespace {

struct Rng {
    std::uint32_t s;
    explicit Rng(unsigned seed) : s(seed * 2654435761u + 0x9E3779B9u) {
        if (!s) s = 1;
        for (int i = 0; i < 4; ++i) next();
    }
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int below(int n) { return n > 0 ? static_cast<int>(next() % static_cast<std::uint32_t>(n)) : 0; }
    bool chance(double p) { return (next() % 10000u) < static_cast<std::uint32_t>(p * 10000); }
};

enum class Bass { Eighths, Offbeat, Sustain, Syncopated, Rolling };
enum class Comp { Sustain, Stabs, Pulse, Arp };

struct Genre {
    const char* name;
    double bpm;
    double swing;
    std::vector<std::vector<std::pair<const char*, const char*>>> grooves;  // variants: lane -> 16 steps
    Bass bass;
    Comp comp;
    const char* chordPreset;
    const char* bassPreset;
    const char* leadPreset;
    bool sevenths;
    double reverb;
};

const std::vector<Genre>& genres() {
    static const std::vector<Genre> g = {
        {"house", 124, 0.0,
         {{{"kick", "x...x...x...x..."}, {"clap", "....x.......x..."}, {"hat", "..x...x...x...x."}, {"shaker", "oxoooxoooxoooxoo"}},
          {{"kick", "x...x...x...x..."}, {"clap", "....x.......x..."}, {"openhat", "..x...x...x...x."}, {"hat", "x.x.x.x.x.x.x.x."}},
          {{"kick", "x...x...x...x..."}, {"snare", "....x.......x..."}, {"hat", ".xx..xx..xx..xx."}, {"rim", "...x.....x....x."}}},
         Bass::Offbeat, Comp::Stabs, "keys", "bass", "pluck", true, 0.25},
        {"techno", 130, 0.0,
         {{{"kick", "x...x...x...x..."}, {"hat", "..x...x...x...x."}, {"rim", "...x..x...x..x.."}, {"clap", "....x.......x..."}},
          {{"kick", "x...x...x...x..."}, {"openhat", "..x...x...x...x."}, {"hat", "xxxxxxxxxxxxxxxx"}, {"cowbell", "......x.......x."}}},
         Bass::Rolling, Comp::Stabs, "pluck", "bass", "lead", false, 0.3},
        {"hiphop", 90, 0.2,
         {{{"kick", "x......x..x....."}, {"snare", "....x.......x..."}, {"hat", "x.x.x.x.x.x.x.x."}},
          {{"kick", "x.....x.x.......x"}, {"snare", "....x.......x..."}, {"hat", "x.xxx.x.x.xxx.x."}},
          {{"kick", "x..x......x....."}, {"snare", "....x.......x.o."}, {"hat", "x.x.x.x.x.x.x.x."}, {"rim", ".......x......x."}}},
         Bass::Syncopated, Comp::Sustain, "keys", "sub", "lead", true, 0.15},
        {"trap", 140, 0.0,
         {{{"kick", "x.....x...x....."}, {"clap", "........x......."}, {"hat", "xxxxxxxxxxxxXxxx"}, {"openhat", "..............x."}},
          {{"kick", "x......xx.....x."}, {"snare", "........x......."}, {"hat", "x.x.x.xxx.x.x.xx"}, {"rim", "...x.......x...."}}},
         Bass::Sustain, Comp::Sustain, "pad", "sub", "pluck", false, 0.3},
        {"lofi", 78, 0.45,
         {{{"kick", "x.....x...x....."}, {"snare", "....x.......x..."}, {"hat", "x.xox.x.x.xox.x."}},
          {{"kick", "x......x.x......"}, {"rim", "....x.......x..."}, {"shaker", "x.x.x.x.x.x.x.x."}}},
         Bass::Syncopated, Comp::Sustain, "keys", "bass", "keys", true, 0.35},
        {"pop", 110, 0.0,
         {{{"kick", "x.......x.x....."}, {"snare", "....x.......x..."}, {"hat", "x.x.x.x.x.x.x.x."}},
          {{"kick", "x...x...x...x..."}, {"clap", "....x.......x..."}, {"hat", "x.x.x.x.x.x.x.x."}}},
         Bass::Eighths, Comp::Pulse, "keys", "bass", "lead", false, 0.2},
        {"rock", 120, 0.0,
         {{{"kick", "x.......x.x....."}, {"snare", "....x.......x..."}, {"hat", "x.x.x.x.x.x.x.x."}},
          {{"kick", "x.x.....x.x....."}, {"snare", "....x.......x..."}, {"openhat", "x.x.x.x.x.x.x.x."}}},
         Bass::Eighths, Comp::Pulse, "brass", "bass", "lead", false, 0.15},
        {"dnb", 174, 0.0,
         {{{"kick", "x.........x....."}, {"snare", "....x.......x..."}, {"hat", "x.x.x.x.x.x.x.x."}},
          {{"kick", "x.........xx...."}, {"snare", "....x..o....x..o"}, {"shaker", "xxxxxxxxxxxxxxxx"}}},
         Bass::Sustain, Comp::Sustain, "pad", "bass", "pluck", false, 0.3},
        {"reggaeton", 95, 0.0,
         {{{"kick", "x...x...x...x..."}, {"snare", "...x..x....x..x."}, {"hat", "x.x.x.x.x.x.x.x."}}},
         Bass::Syncopated, Comp::Pulse, "pluck", "sub", "lead", false, 0.15},
        {"ambient", 70, 0.0,
         {{{"kick", "x..............."}, {"shaker", "o.o.o.o.o.o.o.o."}},
          {{"rim", "......x.......x."}, {"shaker", "..o...o...o...o."}}},
         Bass::Sustain, Comp::Arp, "pad", "sub", "keys", true, 0.6},
    };
    return g;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string uniquePattern(const Song& s, const std::string& base) {
    if (!s.findPattern(base)) return base;
    for (int i = 2;; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!s.findPattern(n)) return n;
    }
}

std::string uniqueTrack(const Song& s, const std::string& base) {
    if (findTrack(s, base) < 0) return base;
    for (int i = 2;; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (findTrack(s, n) < 0) return n;
    }
}

}  // namespace

const std::vector<std::string>& genreNames() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const auto& g : genres()) out.push_back(g.name);
        return out;
    }();
    return v;
}

bool parseKey(const std::string& text, int& root, bool& minor) {
    std::string t;
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) t += c;
    }
    if (t.empty()) return false;
    static const int base[] = {9, 11, 0, 2, 4, 5, 7};
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
    if (c < 'a' || c > 'g') return false;
    root = base[c - 'a'];
    std::size_t i = 1;
    if (i < t.size() && (t[i] == '#' || t[i] == 'b')) {
        root += t[i] == '#' ? 1 : -1;
        ++i;
    }
    root = (root + 12) % 12;
    const std::string q = lower(t.substr(i));
    if (q.empty() || q == "maj" || q == "major" || q == "ionian") minor = false;
    else if (q == "m" || q == "min" || q == "minor" || q == "aeolian") minor = true;
    else return false;
    return true;
}

std::string generate(Song& s, const GenerateOptions& o) {
    const Genre* g = nullptr;
    for (const auto& x : genres()) {
        if (lower(o.genre) == x.name) g = &x;
    }
    if (!g) throw std::runtime_error("unknown genre '" + o.genre + "'");
    int root = 0;
    bool minor = true;
    if (!parseKey(o.key, root, minor)) throw std::runtime_error("unknown key '" + o.key + "' (try \"A minor\", \"Am\", \"F# major\" or \"Eb\")");
    Rng rng(o.seed);
    s.bpm = o.bpm > 0 ? o.bpm : g->bpm;
    s.swing = g->swing;
    s.beatsPerBar = 4;
    s.beatUnit = 4;
    static const char* pcNames[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    s.key = std::string(pcNames[root]) + (minor ? " minor" : " major");
    const int bars = std::clamp(o.bars, 1, 256);

    static const int majorScale[] = {0, 2, 4, 5, 7, 9, 11};
    static const int minorScale[] = {0, 2, 3, 5, 7, 8, 10};
    const int* scale = minor ? minorScale : majorScale;
    auto degreePitch = [&](int degree, int octaveBase) {  // degree may exceed 6
        const int oct = degree >= 0 ? degree / 7 : -((-degree + 6) / 7);
        const int d = ((degree % 7) + 7) % 7;
        return octaveBase + root + scale[d] + 12 * oct;
    };
    static const std::vector<std::vector<int>> majorProgs = {{0, 4, 5, 3}, {0, 5, 3, 4}, {5, 3, 0, 4}, {0, 3, 4, 3}, {0, 3, 5, 4}};
    static const std::vector<std::vector<int>> minorProgs = {{0, 5, 2, 6}, {0, 3, 4, 0}, {0, 6, 5, 6}, {0, 3, 6, 2}, {0, 5, 3, 4}};
    const auto& progs = minor ? minorProgs : majorProgs;
    const std::vector<int> prog = progs[static_cast<std::size_t>(rng.below(static_cast<int>(progs.size())))];
    const int progBars = static_cast<int>(prog.size());

    // chord tones (as scale degrees stacked in thirds)
    auto chordDegrees = [&](int degree) {
        std::vector<int> d = {degree, degree + 2, degree + 4};
        if (g->sevenths) d.push_back(degree + 6);
        return d;
    };
    std::string progText;
    static const char* roman[] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    for (int d : prog) {
        // name each chord by its quality
        const int r3 = (scale[(d + 2) % 7] - scale[d] + 12) % 12;
        const int r5 = (scale[(d + 4) % 7] - scale[d] + 12) % 12;
        std::string name = pcNames[(root + scale[d]) % 12];
        if (r5 == 6) name += "dim";
        else if (r3 == 3) name += "m";
        if (g->sevenths) {
            const int r7 = (scale[(d + 6) % 7] - scale[d] + 12) % 12;
            name += r7 == 11 ? (r3 == 4 ? "maj7" : "7") : "7";
        }
        std::string rn = roman[d];
        if (r3 == 3) {
            for (auto& ch : rn) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        progText += (progText.empty() ? "" : " - ") + name + " (" + rn + ")";
    }

    const int startTrack = static_cast<int>(s.tracks.size());
    std::string made;
    auto addTrack = [&](const std::string& name, TrackKind k, const std::string& pre) {
        Track t;
        t.name = uniqueTrack(s, name);
        t.kind = k;
        if (!pre.empty()) {
            preset(pre, t.synth);
            t.presetName = pre;
        }
        s.tracks.push_back(t);
        return static_cast<int>(s.tracks.size()) - 1;
    };
    const int melodyStart = bars >= 8 ? std::min(4, bars - 4) : 0;

    if (o.drums) {
        const int ti = addTrack("Drums", TrackKind::Drum, "");
        Pattern p;
        p.name = uniquePattern(s, lower(g->name) + " beat");
        const auto& groove = g->grooves[static_cast<std::size_t>(rng.below(static_cast<int>(g->grooves.size())))];
        for (const auto& [lane, steps] : groove) {
            std::string st = steps;
            st.resize(16, '.');
            // seeded ghost notes on the hats and an occasional extra kick
            for (std::size_t i = 0; i < 16; ++i) {
                if (st[i] != '.') continue;
                const std::string l = lane;
                if ((l == "hat" || l == "shaker") && rng.chance(0.12)) st[i] = 'o';
                if (l == "kick" && i % 2 == 1 && rng.chance(0.06)) st[i] = 'o';
            }
            p.lanes.emplace_back(lane, st);
        }
        s.patterns.push_back(p);
        // a fill: the last bar of every 4 gets a snare/tom run at the end
        Pattern fill = p;
        fill.name = uniquePattern(s, lower(g->name) + " fill");
        bool hasSnare = false;
        for (auto& [lane, st] : fill.lanes) {
            if (lane == "snare") {
                st = st.substr(0, 12) + "xxXX";
                hasSnare = true;
            }
        }
        if (!hasSnare) fill.lanes.emplace_back("snare", "............oxxX");
        fill.lanes.emplace_back("tom", rng.chance(0.5) ? "..........x.x..." : "........x...x...");
        s.patterns.push_back(fill);
        for (int b = 0; b < bars; ++b) {
            const bool isFill = bars >= 4 && b % 4 == 3 && std::string(g->name) != "ambient";
            const std::string& name = isFill ? fill.name : p.name;
            if (!s.arrangement.empty() && s.arrangement.back().track == ti && s.arrangement.back().pattern == name &&
                s.arrangement.back().bar + s.arrangement.back().repeat == b) {
                ++s.arrangement.back().repeat;  // runs of the same bar become one placement
            } else {
                s.arrangement.push_back({ti, name, b, 1});
            }
        }
        // a crash where the melody comes in and every 8 bars after it
        std::vector<int> crashes;
        for (int b = std::string(g->name) == "rock" ? 0 : (melodyStart > 0 ? melodyStart : 8); b < bars; b += 8) crashes.push_back(b);
        if (!crashes.empty()) {
            Pattern crash;
            crash.name = uniquePattern(s, "crash");
            crash.lanes.emplace_back("crash", "x...............");
            s.patterns.push_back(crash);
            for (int b : crashes) s.arrangement.push_back({ti, crash.name, b, 1});
        }
        made += "drums (" + std::to_string(groove.size()) + " lanes)";
    }

    if (o.bass) {
        const int ti = addTrack("Bass", TrackKind::Synth, g->bassPreset);
        Pattern p;
        p.name = uniquePattern(s, "bassline");
        p.bars = progBars;
        for (int b = 0; b < progBars; ++b) {
            const int rootNote = 36 + (root + scale[prog[static_cast<std::size_t>(b)]]) % 12;  // C2..B2
            const double t0 = b * 4.0;
            auto add = [&](double at, double len, int pitch, int vel) { p.notes.push_back({pitch, t0 + at, len, vel}); };
            switch (g->bass) {
                case Bass::Eighths:
                    for (int i = 0; i < 8; ++i) add(i * 0.5, 0.45, (i == 7 && rng.chance(0.5)) ? rootNote + 7 : rootNote, i % 2 ? 90 : 110);
                    break;
                case Bass::Offbeat:
                    for (int i = 0; i < 4; ++i) add(i + 0.5, 0.4, (i == 3 && rng.chance(0.4)) ? rootNote + 12 : rootNote, 105);
                    break;
                case Bass::Sustain:
                    add(0, 2.75, rootNote, 110);
                    add(3, 0.9, rng.chance(0.5) ? rootNote + 7 : rootNote, 95);
                    break;
                case Bass::Syncopated:
                    add(0, 0.7, rootNote, 110);
                    add(0.75, 0.5, rootNote, 85);
                    add(1.5, 1.0, rng.chance(0.5) ? rootNote + 12 : rootNote, 95);
                    add(3.0, 0.4, rootNote, 90);
                    if (rng.chance(0.6)) add(3.5, 0.4, rootNote + (minor ? 3 : 4), 85);
                    break;
                case Bass::Rolling:
                    for (int i = 0; i < 16; ++i) {
                        if (i % 4 == 0) continue;  // leave room for the kick
                        add(i * 0.25, 0.2, rootNote, i % 4 == 2 ? 110 : 80);
                    }
                    break;
            }
        }
        s.patterns.push_back(p);
        s.arrangement.push_back({ti, p.name, 0, (bars + progBars - 1) / progBars});
        if (!made.empty()) made += ", ";
        made += "bass";
    }

    if (o.chords) {
        const int ti = addTrack("Chords", TrackKind::Synth, g->chordPreset);
        s.tracks[static_cast<std::size_t>(ti)].fx.reverb = g->reverb;
        s.tracks[static_cast<std::size_t>(ti)].volumeDb = -4;
        Pattern p;
        p.name = uniquePattern(s, "chords");
        p.bars = progBars;
        for (int b = 0; b < progBars; ++b) {
            std::vector<int> voicing;
            for (int d : chordDegrees(prog[static_cast<std::size_t>(b)])) {
                int n = degreePitch(d, 48);
                while (n < 55) n += 12;
                while (n >= 67) n -= 12;
                voicing.push_back(n);
            }
            std::sort(voicing.begin(), voicing.end());
            voicing.erase(std::unique(voicing.begin(), voicing.end()), voicing.end());
            const double t0 = b * 4.0;
            for (int n : voicing) {
                switch (g->comp) {
                    case Comp::Sustain: p.notes.push_back({n, t0, 4.0, 85}); break;
                    case Comp::Stabs:
                        for (int i = 0; i < 4; ++i) p.notes.push_back({n, t0 + i + 0.5, 0.3, 95});
                        break;
                    case Comp::Pulse:
                        for (int i = 0; i < 4; ++i) p.notes.push_back({n, t0 + i, 0.9, i == 0 ? 100 : 80});
                        break;
                    case Comp::Arp: break;
                }
            }
            if (g->comp == Comp::Arp) {
                for (int i = 0; i < 8; ++i) {
                    const int n = voicing[static_cast<std::size_t>(i) % voicing.size()] + (i >= static_cast<int>(voicing.size()) ? 12 : 0);
                    p.notes.push_back({n, t0 + i * 0.5, 1.5, 70 + (i % 2 ? 0 : 15)});
                }
            }
        }
        s.patterns.push_back(p);
        s.arrangement.push_back({ti, p.name, 0, (bars + progBars - 1) / progBars});
        if (!made.empty()) made += ", ";
        made += "chords";
    }

    if (o.melody && bars - melodyStart >= 1) {
        const int ti = addTrack("Melody", TrackKind::Synth, g->leadPreset);
        s.tracks[static_cast<std::size_t>(ti)].fx.delayMix = 0.25;
        s.tracks[static_cast<std::size_t>(ti)].fx.reverb = g->reverb * 0.6;
        s.tracks[static_cast<std::size_t>(ti)].volumeDb = -5;
        Pattern p;
        p.name = uniquePattern(s, "melody");
        p.bars = progBars;
        // one-bar rhythm motif of eighth-note slots, reused each bar with new pitches
        std::vector<int> slots;
        for (int i = 0; i < 8; ++i) {
            if (i == 0 || rng.chance(i % 2 == 0 ? 0.55 : 0.3)) slots.push_back(i);
        }
        static const int pentaMajor[] = {0, 1, 2, 4, 5};  // scale degrees of the pentatonic
        static const int pentaMinor[] = {0, 2, 3, 4, 6};
        const int* penta = minor ? pentaMinor : pentaMajor;
        int prev = 7;  // degree index around the octave above the root
        for (int b = 0; b < progBars; ++b) {
            const int chordRoot = prog[static_cast<std::size_t>(b)];
            for (std::size_t k = 0; k < slots.size(); ++k) {
                const int slot = slots[k];
                int degree;
                if (slot % 4 == 0) {
                    // strong beats: a chord tone near the last note
                    const int choices[] = {chordRoot, chordRoot + 2, chordRoot + 4};
                    degree = choices[0];
                    int best = 99;
                    for (int c : choices) {
                        for (int oct = 0; oct <= 14; oct += 7) {
                            const int cand = c + oct;
                            const int dist = std::abs(cand - prev) + rng.below(2);
                            if (dist < best && cand >= 4 && cand <= 13) best = dist, degree = cand;
                        }
                    }
                } else {
                    const int step = rng.below(5) - 2;
                    degree = std::clamp(prev + step, 4, 13);
                    const int d7 = ((degree % 7) + 7) % 7;
                    bool inPenta = false;
                    for (int i = 0; i < 5; ++i) inPenta = inPenta || penta[i] == d7;
                    if (!inPenta) degree += (step >= 0 ? 1 : -1);
                }
                if (b == progBars - 1 && k + 1 == slots.size()) degree = 7;  // end on the root
                prev = degree;
                const int next = k + 1 < slots.size() ? slots[k + 1] : 8;
                const double len = std::max(0.25, (next - slot) * 0.5 - 0.05);
                p.notes.push_back({degreePitch(degree, 48), b * 4.0 + slot * 0.5, std::min(len, 1.5), 90 + (slot % 4 == 0 ? 15 : 0)});
            }
        }
        s.patterns.push_back(p);
        const int reps = (bars - melodyStart + progBars - 1) / progBars;
        s.arrangement.push_back({ti, p.name, melodyStart, reps});
        if (!made.empty()) made += ", ";
        made += "melody";
    }
    // trim placements that would run past `bars`
    for (auto& pl : s.arrangement) {
        if (pl.track < startTrack) continue;
        const Pattern* p = s.findPattern(pl.pattern);
        const int len = p ? p->bars : 1;
        while (pl.repeat > 1 && pl.bar + pl.repeat * len > bars) --pl.repeat;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", s.bpm);
    return std::string(g->name) + " in " + s.key + " at " + buf + " bpm, " + std::to_string(bars) + " bars: " + made + "; chords " + progText;
}

}  // namespace achclwss::beats
