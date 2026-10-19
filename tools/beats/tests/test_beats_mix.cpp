// Piano-roll note commands, synth filter modes / unison / LFO, metronome, drum pads,
// effects racks and automation on songs, moving placements and stems. Signal checks.
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "wetwaresuite/audio/buffer.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/audio/repair.hpp"
#include "wetwaresuite/beats/beats.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::beats;
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

std::string S(const char* s) { return s; }
std::int64_t I(int v) { return v; }

double numField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    if (!v) return -9999;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return -9999;
}

std::string str(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : "";
}

double rms(const audio::Buffer& b, double from, double to) {
    const auto r = audio::rangeOf(b, from, to);
    double sum = 0;
    for (std::size_t c = 0; c < b.channels.size(); ++c)
        for (std::size_t i = r.begin; i < r.end; ++i) sum += b.channels[c][i] * b.channels[c][i];
    return r.size() ? std::sqrt(sum / static_cast<double>(r.size() * b.channels.size())) : 0;
}

double tone(const audio::Buffer& b, double hz, double from, double to) { return audio::toneLevel(b, hz, audio::rangeOf(b, from, to)); }

double db(double a, double b) { return 20 * std::log10(std::max(a, 1e-12) / std::max(b, 1e-12)); }

std::vector<Note> notesOf(Session& s, const char* pattern) {
    auto d = s.workspace().getAs<SongDoc>("song1");
    return d->song.findPattern(pattern)->notes;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_beats_mix";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ---- synth engine ----
    {
        SynthSettings st;
        st.osc1 = Osc::Sine;
        st.osc2 = Osc::Off;
        st.attack = 0.001;
        st.sustain = 1;
        st.cutoff = 20000;
        st.resonance = 0;
        const int rate = 44100;
        audio::Buffer plain = synthNote(st, rate, 440, 1.0, 1.0);
        CHECK(tone(plain, 440, 0.1, 0.9) > -12);
        // pitch LFO, square wave at 1 Hz, 12 semitones: an octave up for half a second, an octave down after
        SynthSettings v = st;
        v.lfoTarget = LfoTarget::Pitch;
        v.lfoShape = 2;
        v.lfoRate = 1;
        v.lfoDepth = 12;
        audio::Buffer vib = synthNote(v, rate, 440, 1.0, 1.0);
        CHECK(tone(vib, 880, 0.1, 0.4) > tone(vib, 440, 0.1, 0.4) + 20);
        CHECK(tone(vib, 220, 0.6, 0.9) > tone(vib, 440, 0.6, 0.9) + 20);
        // amp LFO (tremolo): sine, depth 1 -> loud at the troughs of the LFO, near silent at its peak
        SynthSettings a = st;
        a.lfoTarget = LfoTarget::Amp;
        a.lfoRate = 2;
        a.lfoDepth = 1;
        audio::Buffer trem = synthNote(a, rate, 440, 1.0, 1.0);
        // lfo = sin: peak (+1, quiet) at 0.125 s, trough (-1, full) at 0.375 s
        CHECK(rms(trem, 0.36, 0.39) > 5 * rms(trem, 0.11, 0.14));
        // filter LFO: a saw through a low-pass that wobbles 3 octaves around 500 Hz
        SynthSettings f = st;
        f.osc1 = Osc::Saw;
        f.cutoff = 500;
        f.lfoTarget = LfoTarget::Filter;
        f.lfoShape = 2;
        f.lfoRate = 1;
        f.lfoDepth = 3;
        audio::Buffer wob = synthNote(f, rate, 110, 1.0, 1.0);
        CHECK(tone(wob, 2970, 0.1, 0.4) > tone(wob, 2970, 0.6, 0.9) + 20);  // 27th harmonic: open half vs closed half
        // high-pass and band-pass
        SynthSettings hp = st;
        hp.cutoff = 3000;
        hp.filter = FilterMode::HighPass;
        audio::Buffer h = synthNote(hp, rate, 200, 0.5, 1.0);
        CHECK(tone(h, 200, 0.1, 0.4) < tone(plain, 440, 0.1, 0.4) - 30);
        hp.osc1 = Osc::Saw;
        hp.cutoff = 2000;
        hp.filter = FilterMode::BandPass;
        hp.resonance = 0.5;
        audio::Buffer bp = synthNote(hp, rate, 100, 0.5, 1.0);
        hp.filter = FilterMode::LowPass;
        hp.cutoff = 20000;
        audio::Buffer open = synthNote(hp, rate, 100, 0.5, 1.0);
        // the band around 2 kHz survives, the 100 Hz fundamental is cut by far more
        CHECK(tone(bp, 100, 0.1, 0.4) < tone(open, 100, 0.1, 0.4) - 20 && tone(bp, 2000, 0.1, 0.4) > tone(open, 2000, 0.1, 0.4) - 6);
        // unison: 5 detuned saws -> about the same loudness, but a different (beating) waveform
        SynthSettings u = st;
        u.osc1 = Osc::Saw;
        audio::Buffer one = synthNote(u, rate, 220, 1.0, 1.0);
        u.unison = 5;
        u.unisonSpread = 30;
        audio::Buffer five = synthNote(u, rate, 220, 1.0, 1.0);
        CHECK(std::fabs(db(rms(five, 0.1, 0.9), rms(one, 0.1, 0.9))) < 4);
        CHECK(std::fabs(tone(five, 220, 0.1, 0.9) - tone(one, 220, 0.1, 0.9)) < 6);
        double diff = 0;
        for (std::size_t i = 4410; i < 40000; ++i) diff += std::fabs(five.channels[0][i] - one.channels[0][i]);
        CHECK(diff / 35590 > 0.05);
        // metronome click: 1600 Hz accent, 1000 Hz otherwise, 30 ms
        audio::Buffer hi = metronomeClick(rate, true), lo = metronomeClick(rate, false);
        CHECK(std::fabs(hi.seconds() - 0.03) < 0.001 && tone(hi, 1600, 0, 0.03) > tone(hi, 1000, 0, 0.03) + 6 && tone(lo, 1000, 0, 0.03) > tone(lo, 1600, 0, 0.03) + 6);
    }

    Session s;
    registerCommands(s);
    for (const char* id : {"beats.notes", "beats.note_add", "beats.note_set", "beats.note_move", "beats.note_transpose", "beats.note_quantize", "beats.note_remove",
                           "beats.fx_add", "beats.fx_set", "beats.fx_remove", "beats.fx_list", "beats.fx_move", "beats.automation", "beats.automate", "beats.pad_set", "beats.metronome",
                           "beats.arrange_move", "beats.stems"})
        CHECK(s.registry().find(id) != nullptr);
    CHECK(s.registry().find("beats.notes")->readOnly && s.registry().find("beats.fx_list")->readOnly && s.registry().find("beats.note_add")->undoable);

    CHECK(s.run("beats.new", {{"bpm", 120.0}}).ok);
    const std::string g = "song1";
    // track 0: a pure sine lead
    CHECK(s.run("beats.track_add", {{"song", g}, {"name", S("Lead")}, {"osc1", S("sine")}, {"osc2", S("off")}, {"cutoff", 20000.0}, {"resonance", 0.0},
                                    {"attack", 0.002}, {"sustain", 1.0}, {"release", 0.02}, {"detune", 0.0}, {"filter_env", 0.0}})
              .ok);
    CHECK(s.run("beats.pattern_add", {{"song", g}, {"name", S("lead")}}).ok);

    // ---- piano roll ----
    Result na = s.run("beats.note_add", {{"song", g}, {"pattern", S("lead")}, {"pitch", S("A4")}, {"start", 2.0}, {"length", 1.0}});
    CHECK(na.ok && numField(na, "note") == 0);
    na = s.run("beats.note_add", {{"song", g}, {"pattern", S("lead")}, {"pitch", S("C4")}, {"start", 0.1}, {"length", 0.9}, {"velocity", I(80)}});
    CHECK(na.ok && numField(na, "note") == 0);  // sorted by start: the new note comes first
    CHECK(!s.run("beats.note_add", {{"song", g}, {"pattern", S("lead")}, {"pitch", S("Q9")}, {"start", 0.0}}).ok);
    CHECK(!s.run("beats.note_add", {{"song", g}, {"pattern", S("lead")}, {"pitch", S("C4")}, {"start", 0.0}, {"velocity", I(0)}}).ok);
    Result ls = s.run("beats.notes", {{"song", g}, {"pattern", S("lead")}});
    CHECK(ls.ok && numField(ls, "count") == 2 && str(ls, "rows").find("1\t69\tA4\t2\t1\t100") != std::string::npos);
    CHECK(str(ls, "rows").find("0\t60\tC4\t0.1\t0.9\t80") != std::string::npos && numField(ls, "beats_per_bar") == 4);
    // a note past the end grows the pattern
    CHECK(s.run("beats.note_add", {{"song", g}, {"pattern", S("lead")}, {"pitch", S("E4")}, {"start", 5.0}}).ok);
    CHECK(s.workspace().getAs<SongDoc>(g)->song.findPattern("lead")->bars == 2);
    CHECK(s.run("beats.note_set", {{"song", g}, {"pattern", S("lead")}, {"notes", NumberList{2}}, {"velocity", I(30)}, {"length", 0.5}}).ok);
    CHECK(notesOf(s, "lead")[2].velocity == 30 && notesOf(s, "lead")[2].length == 0.5);
    CHECK(s.run("beats.note_move", {{"song", g}, {"pattern", S("lead")}, {"notes", NumberList{1}}, {"shift", -1.0}, {"semitones", I(3)}}).ok);
    CHECK(notesOf(s, "lead")[1].pitch == 72 && notesOf(s, "lead")[1].start == 1.0);
    CHECK(s.run("beats.note_transpose", {{"song", g}, {"pattern", S("lead")}, {"semitones", I(-12)}}).ok);
    CHECK(notesOf(s, "lead")[0].pitch == 48 && notesOf(s, "lead")[1].pitch == 60);
    CHECK(s.history().undo());
    CHECK(notesOf(s, "lead")[0].pitch == 60);
    CHECK(s.run("beats.note_quantize", {{"song", g}, {"pattern", S("lead")}, {"grid", 0.5}}).ok);
    CHECK(notesOf(s, "lead")[0].start == 0.0 && notesOf(s, "lead")[0].length == 0.9);
    CHECK(s.run("beats.note_quantize", {{"song", g}, {"pattern", S("lead")}, {"grid", 0.5}, {"ends", true}}).ok);
    CHECK(notesOf(s, "lead")[0].length == 1.0);
    CHECK(!s.run("beats.note_remove", {{"song", g}, {"pattern", S("lead")}}).ok);
    CHECK(s.run("beats.note_remove", {{"song", g}, {"pattern", S("lead")}, {"from", 4.0}}).ok && notesOf(s, "lead").size() == 2);
    CHECK(s.run("beats.note_remove", {{"song", g}, {"pattern", S("lead")}, {"low", S("C5")}}).ok && notesOf(s, "lead").size() == 1);
    // now: one C4 at beat 0 for 1 beat. Make it A4 held for the whole bar.
    CHECK(s.run("beats.note_set", {{"song", g}, {"pattern", S("lead")}, {"notes", NumberList{0}}, {"pitch", S("A4")}, {"length", 4.0}, {"velocity", I(100)}}).ok);
    CHECK(s.run("beats.arrange_place", {{"song", g}, {"track", S("0")}, {"pattern", S("lead")}, {"bar", I(0)}}).ok);
    auto song = [&]() -> Song& { return s.workspace().getAs<SongDoc>(g)->song; };
    audio::Buffer dry = render(song());
    CHECK(tone(dry, 440, 0.2, 1.8) > -30);

    // ---- effects rack on a track ----
    Result fa = s.run("beats.fx_add", {{"song", g}, {"track", S("Lead")}, {"type", S("filter")}, {"values", S("mode=highpass cutoff=3000 slope=2")}});
    CHECK(fa.ok);
    CHECK(!s.run("beats.fx_add", {{"song", g}, {"track", S("Lead")}, {"type", S("nope")}}).ok);
    CHECK(!s.run("beats.fx_add", {{"song", g}, {"type", S("filter")}}).ok);  // no owner
    audio::Buffer filtered = render(song());
    CHECK(tone(filtered, 440, 0.2, 1.8) < tone(dry, 440, 0.2, 1.8) - 30);
    CHECK(s.run("beats.fx_set", {{"song", g}, {"track", S("0")}, {"fx", I(0)}, {"bypass", true}}).ok);
    CHECK(std::fabs(tone(render(song()), 440, 0.2, 1.8) - tone(dry, 440, 0.2, 1.8)) < 0.5);
    CHECK(s.run("beats.fx_set", {{"song", g}, {"track", S("0")}, {"fx", I(0)}, {"bypass", false}, {"values", S("mode=lowpass cutoff=8000")}}).ok);
    CHECK(std::fabs(tone(render(song()), 440, 0.2, 1.8) - tone(dry, 440, 0.2, 1.8)) < 1);
    CHECK(!s.run("beats.fx_set", {{"song", g}, {"track", S("0")}, {"fx", I(4)}, {"values", S("cutoff=100")}}).ok);
    CHECK(!s.run("beats.fx_set", {{"song", g}, {"track", S("0")}, {"fx", I(0)}, {"values", S("cutoff=99999")}}).ok);
    Result fl = s.run("beats.fx_list", {{"song", g}, {"track", S("0")}});
    CHECK(fl.ok && str(fl, "rows").find("\tfilter\t0\tcutoff\t8000\t20\t20000\tHz") != std::string::npos);
    // automate the cutoff: fully open on beat 0, down to 100 Hz by beat 2 and held
    CHECK(s.run("beats.automate", {{"song", g}, {"track", S("0")}, {"target", S("fx0.cutoff")}, {"points", NumberList{0, 20000, 1.5, 100, 4, 100}}}).ok);
    CHECK(!s.run("beats.automate", {{"song", g}, {"track", S("0")}, {"target", S("fx3.cutoff")}, {"points", NumberList{0, 1}}}).ok);
    CHECK(!s.run("beats.automate", {{"song", g}, {"track", S("0")}, {"target", S("volume")}, {"points", NumberList{0}}}).ok);
    {
        audio::Buffer b = render(song());
        CHECK(tone(b, 440, 0.05, 0.3) > tone(b, 440, 1.0, 1.9) + 20);
    }
    {
        Result al = s.run("beats.automation", {{"song", g}, {"track", S("0")}});
        CHECK(al.ok && str(al, "rows") == "0\tfx0.cutoff\t0\t20000\n0\tfx0.cutoff\t1.5\t100\n0\tfx0.cutoff\t4\t100\n" && numField(al, "length") == 8);
        // a second effect in front: the cutoff lane follows the filter to fx1
        CHECK(s.run("beats.fx_add", {{"song", g}, {"track", S("0")}, {"type", S("utility")}}).ok);
        CHECK(s.run("beats.fx_move", {{"song", g}, {"track", S("0")}, {"fx", I(1)}, {"to", I(0)}}).ok && song().tracks[0].rack[0].type == "utility");
        CHECK(song().tracks[0].automation[0].target == "fx1.cutoff");
        CHECK(!s.run("beats.fx_move", {{"song", g}, {"track", S("0")}, {"fx", I(1)}, {"to", I(5)}}).ok);
        CHECK(s.history().undo() && s.history().undo() && song().tracks[0].automation[0].target == "fx0.cutoff");
    }
    CHECK(s.run("beats.automate", {{"song", g}, {"track", S("0")}, {"target", S("fx0.cutoff")}, {"mode", S("clear")}}).ok);
    CHECK(s.run("beats.fx_remove", {{"song", g}, {"track", S("0")}, {"fx", I(0)}}).ok && song().tracks[0].rack.empty());
    // volume automation in beats: 0 dB until beat 2, -40 dB from beat 3
    CHECK(s.run("beats.automate", {{"song", g}, {"track", S("0")}, {"target", S("volume")}, {"points", NumberList{2, 0, 3, -40}}}).ok);
    {
        audio::Buffer b = render(song());
        CHECK(std::fabs(tone(b, 440, 0.2, 0.9) - tone(dry, 440, 0.2, 0.9)) < 0.5);
        CHECK(std::fabs(tone(b, 440, 1.55, 1.95) - (tone(dry, 440, 1.55, 1.95) - 40)) < 2);
    }
    // pan automation overrides the static pan: hard left then hard right
    CHECK(s.run("beats.automate", {{"song", g}, {"track", S("0")}, {"target", S("volume")}, {"mode", S("clear")}}).ok);
    CHECK(s.run("beats.automate", {{"song", g}, {"track", S("0")}, {"target", S("pan")}, {"points", NumberList{0, -1, 1.9, -1, 2.1, 1}}}).ok);
    {
        audio::Buffer b = render(song());
        const auto early = audio::rangeOf(b, 0.2, 0.8), late = audio::rangeOf(b, 1.2, 1.8);
        double l1 = 0, r1 = 0, l2 = 0, r2 = 0;
        for (std::size_t i = early.begin; i < early.end; ++i) l1 += std::fabs(b.channels[0][i]), r1 += std::fabs(b.channels[1][i]);
        for (std::size_t i = late.begin; i < late.end; ++i) l2 += std::fabs(b.channels[0][i]), r2 += std::fabs(b.channels[1][i]);
        CHECK(l1 > 100 * r1 && r2 > 100 * l2);
    }
    CHECK(s.run("beats.automate", {{"song", g}, {"track", S("0")}, {"target", S("pan")}, {"mode", S("clear")}}).ok && song().tracks[0].automation.empty());

    // ---- the master rack ----
    CHECK(s.run("beats.fx_add", {{"song", g}, {"master", true}, {"type", S("utility")}, {"values", S("gain=-12")}}).ok);
    CHECK(std::fabs(tone(render(song()), 440, 0.2, 1.8) - (tone(dry, 440, 0.2, 1.8) - 12)) < 0.5);
    CHECK(s.run("beats.automate", {{"song", g}, {"master", true}, {"target", S("fx0.gain")}, {"points", NumberList{0, 0, 1.9, 0, 2.1, -30}}}).ok);
    {
        audio::Buffer b = render(song());
        CHECK(std::fabs(tone(b, 440, 0.2, 0.8) - tone(dry, 440, 0.2, 0.8)) < 0.5 && std::fabs(tone(b, 440, 1.2, 1.8) - (tone(dry, 440, 1.2, 1.8) - 30)) < 1);
    }
    CHECK(str(s.run("beats.fx_list", {{"song", g}, {"master", true}}), "rows").rfind("master\t0\tutility", 0) == 0);
    CHECK(s.history().undo() && s.history().undo() && song().masterRack.empty() && song().masterAutomation.empty());

    // ---- metronome ----
    CHECK(s.run("beats.track_set", {{"song", g}, {"track", S("0")}, {"mute", true}}).ok);
    CHECK(rms(render(song()), 0, 2) < 1e-5);
    CHECK(s.run("beats.metronome", {{"song", g}, {"on", true}, {"level", -6.0}}).ok && song().metronome);
    CHECK(!s.run("beats.metronome", {{"song", g}, {"on", true}, {"level", 6.0}}).ok);
    {
        RenderOptions o;
        o.limit = false;
        audio::Buffer b = render(song(), o);
        // clicks every 0.5 s (120 bpm), accented on the bar
        CHECK(tone(b, 1600, 0, 0.03) > tone(b, 1000, 0, 0.03) + 6);
        for (double t : {0.5, 1.0, 1.5}) CHECK(tone(b, 1000, t, t + 0.03) > tone(b, 1600, t, t + 0.03) + 6 && rms(b, t, t + 0.02) > 0.05);
        CHECK(rms(b, 0.1, 0.45) < 1e-3 && rms(b, 0.6, 0.95) < 1e-3);
        o.click = 0;
        CHECK(rms(render(song(), o), 0, 2) < 1e-5);
    }
    CHECK(s.run("beats.render", {{"song", g}, {"path", (dir / "click.wav").string()}, {"click", false}}).ok);
    CHECK(rms(audio::load(dir / "click.wav"), 0, 2) < 1e-4);
    CHECK(s.run("beats.metronome", {{"song", g}, {"on", false}}).ok);
    CHECK(s.run("beats.track_set", {{"song", g}, {"track", S("0")}, {"mute", false}}).ok);

    // ---- drum pads ----
    CHECK(s.run("beats.track_add", {{"song", g}, {"kind", S("drum")}, {"name", S("Drums")}}).ok);
    CHECK(s.run("beats.pattern_add", {{"song", g}, {"name", S("hits")}, {"steps", S("snare: x...............")}, {"track", S("Drums")}, {"bar", I(1)}}).ok);
    RenderOptions drumsOnly;
    drumsOnly.onlyTrack = 1;
    drumsOnly.limit = false;
    drumsOnly.startBar = 1;
    drumsOnly.bars = 1;
    const audio::Buffer snare = render(song(), drumsOnly);
    CHECK(rms(snare, 0, 0.05) > 0.02 && rms(snare, 0.1, 0.15) > 0.002);
    CHECK(!s.run("beats.pad_set", {{"song", g}, {"track", S("Lead")}, {"lane", S("snare")}, {"volume", -6.0}}).ok);  // not a drum track
    CHECK(!s.run("beats.pad_set", {{"song", g}, {"track", S("Drums")}, {"lane", S("snare")}, {"tune", 40.0}}).ok);
    CHECK(s.run("beats.pad_set", {{"song", g}, {"track", S("Drums")}, {"lane", S("snare")}, {"volume", -12.0}}).ok);
    CHECK(std::fabs(db(rms(render(song(), drumsOnly), 0, 0.1), rms(snare, 0, 0.1)) + 12) < 0.3);
    CHECK(s.run("beats.pad_set", {{"song", g}, {"track", S("Drums")}, {"lane", S("snare")}, {"volume", 0.0}, {"decay", 0.05}}).ok);
    {
        audio::Buffer b = render(song(), drumsOnly);
        CHECK(std::fabs(db(rms(b, 0, 0.005), rms(snare, 0, 0.005))) < 2 && rms(b, 0.06, 0.15) < 1e-4);
    }
    CHECK(s.run("beats.pad_set", {{"song", g}, {"track", S("Drums")}, {"lane", S("snare")}, {"decay", 0.0}, {"tune", 12.0}}).ok);
    {
        // an octave up plays twice as fast: the hit ends in about half the time
        auto lastSound = [](const audio::Buffer& b) {
            std::size_t last = 0;
            for (std::size_t i = 0; i < b.frames(); ++i)
                if (std::fabs(b.channels[0][i]) > 1e-3) last = i;
            return static_cast<double>(last) / b.rate;
        };
        const double up = lastSound(render(song(), drumsOnly)), orig = lastSound(snare);
        CHECK(up < orig * 0.65 && up > orig * 0.35);
    }

    // ---- moving placements ----
    CHECK(s.run("beats.arrange_move", {{"song", g}, {"placement", I(0)}, {"bar", I(2)}}).ok && song().arrangement[0].bar == 2);
    {
        RenderOptions o;
        o.onlyTrack = 0;
        audio::Buffer b = render(song(), o);
        CHECK(tone(b, 440, 0.2, 1.8) < -80 && tone(b, 440, 4.2, 5.8) > -30);
    }
    CHECK(s.run("beats.arrange_move", {{"song", g}, {"placement", I(0)}, {"bar", I(0)}, {"repeat", I(2)}}).ok && song().bars() == 4);
    CHECK(!s.run("beats.arrange_move", {{"song", g}, {"placement", I(9)}, {"bar", I(0)}}).ok);
    {
        // the window's arrangement timeline: a lane per track, a clip per placement, edits become commands
        auto doc = s.workspace().getAs<SongDoc>(g);
        TimelineLayout tl;
        CHECK(doc->timeline(tl) && tl.docParam == "song" && tl.tracks.size() == 2 && tl.tracks[0].clips.size() == 1 && tl.tracks[1].clips.size() == 1);
        CHECK(tl.tracks[0].clips[0].start == 0 && std::fabs(tl.tracks[0].clips[0].length - 8.0) < 1e-9 && tl.tracks[0].clips[0].label == "lead x2");
        TimelineEdit e;
        e.clip = "0";
        e.track = "0";
        e.start = 4.1;
        CHECK(doc->timelineCommand(e) == "beats.arrange_move placement=0 bar=2");
        e.kind = TimelineEditKind::TrimEnd;
        e.length = 12.3;
        CHECK(doc->timelineCommand(e) == "beats.arrange_move placement=0 repeat=3");
        e.kind = TimelineEditKind::Delete;
        CHECK(doc->timelineCommand(e) == "beats.arrange_remove track=0 bar=0 pattern=lead");
        e.kind = TimelineEditKind::Split;
        CHECK(doc->timelineCommand(e).empty());
    }
    CHECK(!s.run("beats.arrange_move", {{"song", g}, {"placement", I(0)}, {"to_track", S("8")}}).ok);
    CHECK(s.history().undo() && song().arrangement[0].repeat == 1 && song().arrangement[0].bar == 2);

    // ---- stems ----
    Result st = s.run("beats.stems", {{"song", g}, {"folder", (dir / "stems").string()}, {"mix", true}, {"bits", S("16")}, {"rate", I(22050)}});
    CHECK(st.ok && numField(st, "written") == 3);
    {
        audio::Buffer lead = audio::load(dir / "stems" / "01_Lead.wav"), drums = audio::load(dir / "stems" / "02_Drums.wav"), mix = audio::load(dir / "stems" / "mix.wav");
        CHECK(lead.rate == 22050 && lead.frames() == drums.frames() && drums.frames() == mix.frames() && mix.seconds() >= 6.0);
        CHECK(tone(lead, 440, 4.2, 5.8) > -30 && rms(lead, 0.0, 3.9) < 1e-3);
        CHECK(rms(drums, 0, 1.9) < 1e-4 && rms(drums, 2.0, 2.1) > 0.01);
    }
    CHECK(!s.run("beats.stems", {{"song", g}, {"folder", (dir / "x").string()}, {"bits", S("12")}}).ok);

    // ---- save / open keeps everything ----
    CHECK(s.run("beats.track_set", {{"song", g}, {"track", S("Lead")}, {"filter", S("bandpass")}, {"unison", I(3)}, {"lfo_target", S("filter")}, {"lfo_shape", S("triangle")},
                                    {"lfo_rate", 0.5}, {"lfo_depth", 2.0}})
              .ok);
    CHECK(!s.run("beats.track_set", {{"song", g}, {"track", S("Lead")}, {"lfo_target", S("moon")}}).ok);
    CHECK(!s.run("beats.track_set", {{"song", g}, {"track", S("Lead")}, {"unison", I(9)}}).ok);
    CHECK(s.run("beats.fx_add", {{"song", g}, {"track", S("Lead")}, {"type", S("delay")}, {"values", S("time=250 mix=0.4")}}).ok);
    CHECK(s.run("beats.automate", {{"song", g}, {"track", S("Lead")}, {"target", S("fx0.mix")}, {"points", NumberList{0, 0, 4, 0.8}}}).ok);
    CHECK(s.run("beats.fx_add", {{"song", g}, {"master", true}, {"type", S("compressor")}}).ok);
    CHECK(s.run("beats.metronome", {{"song", g}, {"on", true}, {"level", -20.0}}).ok);
    Result sv = s.run("beats.save", {{"song", g}, {"path", (dir / "mixed.beats.json").string()}});
    if (!sv.ok) std::printf("%s\n", sv.message.c_str());
    CHECK(sv.ok);
    {
        const std::string info = s.run("beats.info", {{"song", g}}).message;
        CHECK(info.find("fx 0 ") != std::string::npos && info.find("automation fx0.mix: 2 points") != std::string::npos && info.find("metronome on") != std::string::npos &&
              info.find("pad snare") != std::string::npos && info.find("unison 3") != std::string::npos);
    }
    {
        const Song back = loadSong(dir / "mixed.beats.json");
        const Track& t = back.tracks[0];
        CHECK(t.synth.filter == FilterMode::BandPass && t.synth.unison == 3 && t.synth.lfoTarget == LfoTarget::Filter && t.synth.lfoShape == 1 &&
              t.synth.lfoRate == 0.5 && t.synth.lfoDepth == 2.0);
        CHECK(t.rack.size() == 1 && t.rack[0].type == "delay" && t.rack[0].get("time") == 250 && t.automation.size() == 1 && t.automation[0].target == "fx0.mix" &&
              t.automation[0].points.size() == 2);
        CHECK(back.tracks[1].pads.at("snare").tune == 12 && back.masterRack.size() == 1 && back.metronome && back.metronomeDb == -20);
        RenderOptions o;
        o.click = 0;
        audio::Buffer a = render(back, o), b = render(song(), o);
        CHECK(a.frames() == b.frames() && std::fabs(rms(a, 0, 4) - rms(b, 0, 4)) < 1e-6);
    }

    // ---- over MCP ----
    {
        mcp::Server server(s, "beats-test", "1");
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        bool found = false;
        for (const auto& t : list) found = found || t["name"] == "beats_note_add" || t["name"] == "beats.note_add";
        CHECK(found);
    }

    fs::remove_all(dir);
    return check::summary("beats mix");
}
