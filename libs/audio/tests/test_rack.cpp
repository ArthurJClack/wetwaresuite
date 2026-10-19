// The effects rack, automation, restoration tools and the project features
// built on them, checked on real signals: levels, frequencies and lengths.
#include <cmath>
#include <filesystem>

#include "wetwaresuite/audio/analysis.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/project.hpp"
#include "wetwaresuite/audio/rack.hpp"
#include "wetwaresuite/audio/repair.hpp"
#include "check.hpp"

using namespace wetwaresuite::audio;
namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100;

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

Effect fx(const std::string& type, const std::string& values = "") {
    Effect e;
    std::string error;
    if (!makeEffect(type, values, e, error)) std::printf("makeEffect %s: %s\n", type.c_str(), error.c_str());
    return e;
}

Buffer run(Buffer b, const Effect& e) {
    applyRack(b, {e});
    return b;
}

Buffer sum(const Buffer& a, const Buffer& b) {
    Buffer out = a;
    mixInto(out, b, 0);
    return out;
}

double rmsDb(const Buffer& b, double from = 0, double to = -1) {
    const Range r = rangeOf(b, from, to);
    double s = 0;
    std::size_t n = 0;
    for (const auto& c : b.channels)
        for (std::size_t i = r.begin; i < r.end; ++i) s += static_cast<double>(c[i]) * c[i], ++n;
    return n ? gainToDb(std::sqrt(s / static_cast<double>(n))) : -240;
}

double peakDb(const Buffer& b, double from = 0, double to = -1) {
    const Range r = rangeOf(b, from, to);
    double p = 0;
    for (const auto& c : b.channels)
        for (std::size_t i = r.begin; i < r.end; ++i) p = std::max(p, static_cast<double>(std::fabs(c[i])));
    return gainToDb(p);
}

// Level of a frequency in the steady part (skips filter start-up).
double level(const Buffer& b, double hz) { return toneLevel(b, hz, rangeOf(b, 0.3, b.seconds() - 0.1)); }

Buffer chan(std::vector<float> x) {
    Buffer b;
    b.rate = kRate;
    b.channels.push_back(std::move(x));
    return b;
}

bool finite(const Buffer& b) {
    for (const auto& c : b.channels)
        for (float v : c)
            if (!std::isfinite(v) || std::fabs(v) > 50) return false;
    return true;
}

Buffer impulse(int channels, double seconds, double at) {
    Buffer b = silence(channels, seconds, kRate);
    for (auto& c : b.channels) c[static_cast<std::size_t>(at * kRate)] = 1.0f;
    return b;
}

double windowSum(const Buffer& b, double at, int halfWidth = 30) {
    const auto c = static_cast<long>(at * kRate);
    double s = 0;
    for (long i = c - halfWidth; i <= c + halfWidth; ++i)
        if (i >= 0 && i < static_cast<long>(b.frames())) s += b.channels[0][static_cast<std::size_t>(i)];
    return s;
}

}  // namespace

int main() {
    // ---- the table and parsing ----
    CHECK(effectTypes().size() >= 16);
    for (const char* n : {"eq", "graphic_eq", "filter", "compressor", "limiter", "gate", "expander", "deesser", "reverb", "delay", "chorus",
                          "flanger", "phaser", "tremolo", "distortion", "utility"})
        CHECK(findEffectType(n) != nullptr);
    CHECK(findEffectType("echo")->name == "delay" && findEffectType("Compress")->name == "compressor");
    {
        Effect e;
        std::string error;
        CHECK(makeEffect("compressor", "threshold=-20, ratio=4 attack=5ms", e, error) && e.get("threshold") == -20 && e.get("ratio") == 4 && e.get("attack") == 5);
        CHECK(e.get("release") == 120);  // default
        CHECK(!makeEffect("compressor", "ratio=40", e, error) && error.find("1..20") != std::string::npos);
        CHECK(!makeEffect("compressor", "speed=2", e, error) && error.find("no setting") != std::string::npos);
        CHECK(!makeEffect("wobble", "", e, error) && error.find("unknown effect") != std::string::npos);
        CHECK(makeEffect("distortion", "mode=hard", e, error) && e.get("mode") == 1);
        CHECK(!makeEffect("distortion", "mode=crunchy", e, error) && error.find("soft") != std::string::npos);
        CHECK(makeEffect("delay", "time=0.5s pingpong=on", e, error) && e.get("time") == 500 && e.get("pingpong") == 1);
        CHECK(makeEffect("eq", "bands=highpass 80, peaking 2.5k -4 2, lowshelf 120 3; output=-1", e, error));
        CHECK(e.get("b1_type") == 2 && e.get("b2_freq") == 2500 && e.get("b2_gain") == -4 && e.get("b2_q") == 2 && e.get("b3_type") == 6 && e.get("b4_type") == 0);
        CHECK(e.get("output") == -1);
        const std::string d = describeEffect(e);
        CHECK(d.find("band 2 peaking 2500 Hz -4 dB Q 2") != std::string::npos && d.find("band 4") == std::string::npos);
        CHECK(makeEffect("eq", "b1_type=bell b1_freq=1000 b1_gain=6", e, error) && e.get("b1_type") == 5);
        CHECK(!makeEffect("eq", "bands=wiggle 100", e, error));
    }

    // ---- EQ ----
    const Buffer lowHigh = sum(tone(1, 2, kRate, 100, 0.25), tone(1, 2, kRate, 1000, 0.25));
    {
        const double in100 = level(lowHigh, 100), in1k = level(lowHigh, 1000);
        CHECK(near(in1k, gainToDb(0.25), 0.2));
        Buffer b = run(lowHigh, fx("eq", "bands=peaking 1000 12 2"));
        CHECK(near(level(b, 1000) - in1k, 12, 0.7) && near(level(b, 100), in100, 0.5));
        b = run(lowHigh, fx("eq", "bands=highpass 300"));
        CHECK(level(b, 100) < in100 - 15 && near(level(b, 1000), in1k, 0.6));
        b = run(lowHigh, fx("graphic_eq", "g1k=9"));
        CHECK(near(level(b, 1000) - in1k, 9, 1.0) && near(level(b, 100), in100, 0.5));
        b = run(sum(tone(1, 2, kRate, 4000, 0.25), tone(1, 2, kRate, 200, 0.25)), fx("filter", "mode=lowpass cutoff=500 slope=2"));
        CHECK(level(b, 4000) < gainToDb(0.25) - 40 && near(level(b, 200), gainToDb(0.25), 0.6));
    }

    // ---- dynamics ----
    {
        Buffer loud = tone(2, 2, kRate, 1000, 0.5);  // -6 dBFS
        Buffer b = run(loud, fx("compressor", "threshold=-24 ratio=4 knee=0 attack=1 release=200"));
        CHECK(near(peakDb(b, 1.0), -24 + 18.0 / 4, 1.5));
        b = run(loud, fx("compressor", "threshold=-24 ratio=4 knee=0 attack=1 release=200 makeup=10"));
        CHECK(near(peakDb(b, 1.0), -24 + 18.0 / 4 + 10, 1.5));
        // a quiet tone under the threshold passes untouched
        b = run(tone(1, 1, kRate, 1000, 0.01), fx("compressor", "threshold=-24"));
        CHECK(near(peakDb(b, 0.5), gainToDb(0.01), 0.05));

        Buffer hot = tone(2, 1, kRate, 440, 2.0);  // +6 dBFS
        b = run(hot, fx("limiter", "ceiling=-1"));
        CHECK(peakDb(b) <= -1 + 1e-3 && peakDb(b, 0.5) > -1.5);
        CHECK(b.frames() == hot.frames());
        // no added delay: the output lines up with the input (correlation at lag 0)
        double xy = 0, xx = 0, yy = 0;
        for (std::size_t i = 4410; i < 40000; ++i) xy += hot.channels[0][i] * b.channels[0][i], xx += hot.channels[0][i] * hot.channels[0][i], yy += b.channels[0][i] * b.channels[0][i];
        CHECK(xy / std::sqrt(xx * yy) > 0.99);
        b = run(tone(1, 1, kRate, 440, 0.25), fx("limiter", "ceiling=-6 gain=12"));
        CHECK(near(peakDb(b, 0.3), -6, 0.3));

        // gate: noise at -60 dB between phrases goes away, the tone stays
        Buffer g = noise(1, 1, kRate, 0.001, NoiseColor::White, 3);
        Buffer t = tone(1, 1, kRate, 500, 0.3);
        insert(g, g.frames(), t);
        b = run(g, fx("gate", "threshold=-40"));
        CHECK(rmsDb(b, 0.2, 0.9) < rmsDb(g, 0.2, 0.9) - 40 && near(rmsDb(b, 1.2, 1.9), rmsDb(g, 1.2, 1.9), 0.3));
        // expander: 10 dB under the threshold at 2:1 comes out 20 dB under
        b = run(tone(1, 1.5, kRate, 500, dbToGain(-50) * std::sqrt(2.0)), fx("expander", "threshold=-40 ratio=2 range=-40"));
        CHECK(near(rmsDb(b, 0.7, 1.4), -60, 2.5));
        b = run(tone(1, 1.5, kRate, 500, 0.2), fx("expander", "threshold=-40 ratio=2"));
        CHECK(near(rmsDb(b, 0.7, 1.4), gainToDb(0.2 / std::sqrt(2.0)), 0.2));

        // de-esser: loud highs come down, the lows don't
        Buffer s = sum(tone(1, 2, kRate, 8000, 0.3), tone(1, 2, kRate, 300, 0.3));
        b = run(s, fx("deesser", "frequency=5000 threshold=-30 reduction=12"));
        CHECK(level(b, 8000) < level(s, 8000) - 6 && near(level(b, 300), level(s, 300), 1.0));
        b = run(tone(1, 2, kRate, 300, 0.3), fx("deesser", "frequency=5000 threshold=-30"));
        CHECK(near(level(b, 300), gainToDb(0.3), 0.5));
    }

    // ---- time effects ----
    {
        Buffer imp = impulse(2, 3, 0.1);
        Buffer b = run(imp, fx("reverb", "room=0.8 wet=0.5"));
        CHECK(rmsDb(b, 0.6, 1.2) > -70 && finite(b));
        CHECK(effectTail(fx("reverb", "room=0.8")) > 3);
        b = run(imp, fx("reverb", "wet=0 dry=1"));
        CHECK(peakDb(b) > -0.01 && rmsDb(b, 0.2) < -200);

        Buffer one = impulse(1, 2, 0.1);
        b = run(one, fx("delay", "time=250 feedback=0.5 mix=1 tone=20000"));
        CHECK(near(windowSum(b, 0.35), 1.0, 0.05) && near(windowSum(b, 0.6), 0.5, 0.05) && near(windowSum(b, 0.85), 0.25, 0.05));
        CHECK(near(windowSum(b, 0.1), 1.0, 1e-6));  // the dry impulse stays
        Buffer two = impulse(2, 2, 0.1);
        two.channels[1][static_cast<std::size_t>(0.1 * kRate)] = 1.0f;
        b = run(two, fx("delay", "time=250 feedback=0.6 mix=1 pingpong=on tone=20000"));
        // first repeat on the left, the second on the right
        auto win = [&](int ch, double at) {
            const auto c = static_cast<std::size_t>(at * kRate);
            double s = 0;
            for (std::size_t i = c - 30; i <= c + 30; ++i) s += b.channels[static_cast<std::size_t>(ch)][i];
            return s;
        };
        CHECK(win(0, 0.35) > 0.9 && std::fabs(win(1, 0.35)) < 0.01 && win(1, 0.6) > 0.5 && std::fabs(win(0, 0.6)) < 0.01);

        Buffer sine = tone(2, 2, kRate, 440, 0.3);
        for (const char* t : {"chorus", "flanger", "phaser"}) {
            Buffer dry = run(sine, fx(t, "mix=0"));
            CHECK(near(peakDb(dry), peakDb(sine), 1e-4));
            Buffer wet = run(sine, fx(t));
            double diff = 0;
            for (std::size_t i = 0; i < wet.frames(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(wet.channels[0][i] - sine.channels[0][i])));
            CHECK(diff > 0.01 && finite(wet) && near(rmsDb(wet, 0.5), rmsDb(sine, 0.5), 6));
        }
        // the flanger's comb: white noise picks up peaks and notches
        Buffer wn = noise(1, 2, kRate, 0.3, NoiseColor::White, 9);
        Buffer fl = run(wn, fx("flanger", "rate=0.05 depth=0 delay=1 feedback=0.8 mix=0.5"));
        CHECK(level(fl, 1000) > level(fl, 500) + 10);  // 1 ms comb: peaks at 1 kHz multiples, notches at 500 Hz odd multiples

        b = run(tone(1, 2, kRate, 1000, 0.5), fx("tremolo", "rate=4 depth=1"));
        double lo = 1e9, hi = 0;
        for (double t = 0.2; t < 1.8; t += 0.01) {
            const double r = rmsDb(b, t, t + 0.01);
            lo = std::min(lo, r), hi = std::max(hi, r);
        }
        CHECK(hi - lo > 20);
    }

    // ---- distortion and utility ----
    {
        Buffer s = tone(1, 1, kRate, 200, 0.5);
        CHECK(level(s, 600) < -80);
        Buffer b = run(s, fx("distortion", "drive=24 mode=soft"));
        CHECK(level(b, 600) > level(b, 200) - 20 && peakDb(b) <= 0.01);
        b = run(s, fx("distortion", "drive=24 mode=fuzz"));
        CHECK(level(b, 400) > level(b, 200) - 30);  // asymmetric: even harmonics
        b = run(s, fx("distortion", "drive=0 mode=bitcrush bits=3 tone=20000"));
        CHECK(level(b, 600) > -40);
        b = run(s, fx("distortion", "mix=0"));
        CHECK(near(level(b, 200), level(s, 200), 0.3));

        Buffer st = tone(2, 1, kRate, 300, 0.5);
        st.channels[1] = tone(1, 1, kRate, 700, 0.5).channels[0];
        b = run(st, fx("utility", "gain=-6"));
        CHECK(near(peakDb(b), gainToDb(0.5) - 6, 0.05));
        b = run(st, fx("utility", "width=0"));
        double d = 0;
        for (std::size_t i = 0; i < b.frames(); ++i) d = std::max(d, static_cast<double>(std::fabs(b.channels[0][i] - b.channels[1][i])));
        CHECK(d < 1e-6);
        b = run(st, fx("utility", "pan=-1"));
        CHECK(peakDb(chan(b.channels[1])) < -200 && near(peakDb(chan(b.channels[0])), gainToDb(0.5), 0.05));
        Effect off = fx("utility", "gain=-20");
        off.bypass = true;
        b = st;
        applyRack(b, {off});
        CHECK(near(peakDb(b), peakDb(st), 1e-6));
    }

    // ---- automation ----
    {
        Automation a{"fx0.cutoff", {{0, 200}, {2, 8000}}};
        CHECK(near(a.valueAt(-1), 200, 1e-9) && near(a.valueAt(1), 4100, 1e-9) && near(a.valueAt(5), 8000, 1e-9));
        int i = -1;
        std::string prm, error;
        CHECK(parseFxTarget("fx12.b3_gain", i, prm) && i == 12 && prm == "b3_gain" && !parseFxTarget("fx.cutoff", i, prm) && !parseFxTarget("volume", i, prm));
        Rack rack = {fx("filter", "mode=lowpass")};
        CHECK(validTarget("fx0.cutoff", rack, error) && validTarget("pan", rack, error) && !validTarget("fx1.cutoff", rack, error) && !validTarget("fx0.speed", rack, error));
        double lo = 0, hi = 0;
        CHECK(targetRange("fx0.cutoff", rack, lo, hi) && lo == 20 && hi == 20000);
        // a filter sweep: dark at the start, bright at the end
        Buffer wn = noise(1, 2, kRate, 0.3, NoiseColor::White, 5);
        Buffer b = wn;
        applyRack(b, rack, {a});
        CHECK(rmsDb(b, 1.6, 2.0) > rmsDb(b, 0.0, 0.4) + 8);
        // lane time can be in beats: 120 bpm = 0.5 s a beat, so beat 4 is at 2 s
        Buffer b2 = wn;
        applyRack(b2, rack, {Automation{"fx0.cutoff", {{0, 200}, {4, 8000}}}}, 0, 0.5);
        CHECK(near(rmsDb(b2, 1.6, 2.0), rmsDb(b, 1.6, 2.0), 0.5));

        Buffer t = tone(2, 2, kRate, 500, 0.5);
        applyVolumeLane(t, Automation{"volume", {{0, 0}, {1, -20}}});
        CHECK(near(peakDb(t, 0, 0.05), gainToDb(0.5), 0.2) && near(peakDb(t, 1.5), gainToDb(0.5) - 20, 0.05));
        Buffer p = tone(2, 2, kRate, 500, 0.5);
        applyPanLane(p, Automation{"pan", {{0, -1}, {2, 1}}});
        CHECK(peakDb(chan(std::vector<float>(p.channels[1].begin(), p.channels[1].begin() + 2000))) < -30);
        CHECK(peakDb(chan(std::vector<float>(p.channels[0].end() - 2000, p.channels[0].end()))) < -30);

        std::vector<Automation> lanes = {{"fx0.cutoff", {{0, 1}}}, {"fx2.mix", {{0, 1}}}, {"pan", {{0, 1}}}};
        renumberLanes(lanes, 0, -1);
        CHECK(lanes.size() == 2 && lanes[0].target == "fx1.mix" && lanes[1].target == "pan");
        renumberLanes(lanes, -1, 0);
        CHECK(lanes[0].target == "fx2.mix");
        renumberLanes(lanes, -1, -1, 2, 0);
        CHECK(lanes[0].target == "fx0.mix");
    }

    // ---- restoration ----
    {
        const Buffer clean = tone(1, 2, kRate, 440, 0.5);
        CHECK(near(toneLevel(clean, 440, all(clean)), gainToDb(0.5), 0.05));
        Buffer b = clean;
        const std::size_t at[] = {5000, 20000, 33333, 50001, 61000, 77000};
        for (std::size_t k = 0; k < 6; ++k) {
            b.channels[0][at[k]] += k % 2 ? -0.8f : 0.8f;
            if (k >= 4) b.channels[0][at[k] + 1] += 0.6f, b.channels[0][at[k] + 2] -= 0.4f;  // wider clicks
        }
        const int fixedCount = removeClicks(b, {}, all(b));
        double err = 0;
        for (std::size_t i = 0; i < b.frames(); ++i) err = std::max(err, static_cast<double>(std::fabs(b.channels[0][i] - clean.channels[0][i])));
        CHECK(fixedCount == 6 && err < 0.02);
        Buffer untouched = clean;
        CHECK(removeClicks(untouched, {}, all(untouched)) == 0);
        // music-like material (two tones and a little noise) keeps its clicks found and nothing else
        Buffer mus = sum(sum(tone(1, 2, kRate, 220, 0.3), tone(1, 2, kRate, 1320, 0.15)), noise(1, 2, kRate, 0.01, NoiseColor::Pink, 2));
        Buffer musClick = mus;
        musClick.channels[0][30000] += 0.7f;
        CHECK(removeClicks(musClick, {}, all(musClick)) == 1 && std::fabs(musClick.channels[0][30000] - mus.channels[0][30000]) < 0.05);

        Buffer hum = sum(sum(tone(1, 3, kRate, 1000, 0.3), tone(1, 3, kRate, 60, 0.2)), tone(1, 3, kRate, 180, 0.1));
        CHECK(detectHum(hum, all(hum)) == 60);
        const double f0 = removeHum(hum, {}, all(hum));
        CHECK(f0 == 60);
        CHECK(level(hum, 60) < gainToDb(0.2) - 30 && level(hum, 180) < gainToDb(0.1) - 30 && near(level(hum, 1000), gainToDb(0.3), 0.3));
        Buffer fifty = sum(tone(1, 2, kRate, 50, 0.2), tone(1, 2, kRate, 100, 0.1));
        CHECK(detectHum(fifty, all(fifty)) == 50);

        Buffer two = sum(tone(2, 2, kRate, 500, 0.3), tone(2, 2, kRate, 3000, 0.3));
        Buffer cutBand = two;
        spectralGain(cutBand, 2500, 3500, -30, all(cutBand));
        CHECK(level(cutBand, 3000) < gainToDb(0.3) - 25 && near(level(cutBand, 500), gainToDb(0.3), 0.5));
        // only inside the time range
        Buffer part = two;
        spectralGain(part, 2500, 3500, -30, rangeOf(part, 1.0, 2.0));
        CHECK(near(toneLevel(part, 3000, rangeOf(part, 0.1, 0.8)), gainToDb(0.3), 0.5) && toneLevel(part, 3000, rangeOf(part, 1.2, 1.9)) < gainToDb(0.3) - 25);
    }

    // ---- loudness over time ----
    {
        Buffer t = tone(2, 6, kRate, 1000, 0.1);
        const double integrated = integratedLoudness(t, all(t));
        const auto st = loudnessCurve(t, all(t), 3.0, 1.0);
        CHECK(st.size() == 6 && near(st[4], integrated, 0.2) && near(st[0], integrated, 0.3));
        const auto mom = loudnessCurve(t, all(t), 0.4, 0.1);
        CHECK(mom.size() == 60 && near(mom[30], integrated, 0.2));
        Buffer quietLoud = tone(2, 3, kRate, 1000, 0.01);
        insert(quietLoud, quietLoud.frames(), tone(2, 3, kRate, 1000, 0.3));
        const auto c = loudnessCurve(quietLoud, all(quietLoud), 0.4, 0.5);
        CHECK(c.back() > c.front() + 25);
    }

    // ---- projects with racks, automation, curves and markers ----
    {
        Project p;
        p.channels = 2;
        Track a;
        a.name = "music";
        Clip c;
        c.audio = std::make_shared<const Buffer>(tone(2, 2, kRate, 440, 0.5));
        a.clips.push_back(c);
        p.tracks.push_back(a);
        CHECK(near(mixdown(p).seconds(), 2.0, 1e-3));
        // effect on the track: -6 dB by the utility, then the master limiter at -12
        p.tracks[0].fx.push_back(fx("utility", "gain=-6"));
        CHECK(near(peakDb(mixdown(p)), gainToDb(0.5) - 6, 0.05));
        p.masterFx.push_back(fx("limiter", "ceiling=-12"));
        CHECK(peakDb(mixdown(p)) <= -12 + 1e-3);
        p.masterFx.clear();
        // a reverb rings past the end of the clips
        p.tracks[0].fx.push_back(fx("reverb", "room=0.7 wet=0.4"));
        CHECK(p.tailSeconds() > 2);
        Buffer withTail = mixdown(p);
        CHECK(withTail.seconds() > 2.5 && rmsDb(withTail, 2.1, 2.4) > -80);
        p.tracks[0].fx.pop_back();
        // pan automation sweeps the track from left to right; it wins over the static pan
        p.tracks[0].pan = 0.5;
        setTrackLane(p.tracks[0], "pan", {{0, -1}, {2, 1}});
        Buffer swept = mixdown(p);
        CHECK(rmsDb(chan(swept.channels[1]), 0, 0.1) < -30 && rmsDb(chan(swept.channels[0]), 1.9, 2.0) < -30);
        // volume lane is the envelope
        setTrackLane(p.tracks[0], "volume", {{0, 0}, {1, -12}});
        CHECK(p.tracks[0].envelope.size() == 2 && trackLanes(p.tracks[0]).size() == 2 && trackLanes(p.tracks[0])[0].target == "volume");
        setTrackLane(p.tracks[0], "pan", {});
        p.tracks[0].pan = 0;
        CHECK(trackLanes(p.tracks[0]).size() == 1);
        // master volume automation
        p.tracks[0].envelope.clear();
        setLane(p.masterAutomation, "volume", {{0, 0}, {2, -40}});
        Buffer faded = mixdown(p);
        CHECK(peakDb(faded, 1.9, 2.0) < peakDb(faded, 0, 0.1) - 30);
        p.masterAutomation.clear();

        // fade curves: equal power at the middle of a fade is -3 dB, linear -6 dB
        Track t;
        Clip f;
        f.audio = std::make_shared<const Buffer>(tone(2, 2, kRate, 1000, 0.5));
        f.fadeIn = 2;
        f.fadeInCurve = FadeCurve::EqualPower;
        t.clips.push_back(f);
        Project q;
        q.tracks.push_back(t);
        CHECK(near(peakDb(mixdown(q), 0.98, 1.02), gainToDb(0.5) - 3.0, 0.15));
        q.tracks[0].clips[0].fadeInCurve = FadeCurve::Linear;
        CHECK(near(peakDb(mixdown(q), 0.98, 1.02), gainToDb(0.5) - 6.0, 0.15));

        // save and load keep the rack, the lanes, the curves and the markers
        p.tracks[0].fx = {fx("eq", "bands=peaking 1000 -6 1"), fx("compressor", "threshold=-30")};
        p.tracks[0].fx[1].bypass = true;
        setTrackLane(p.tracks[0], "fx0.b1_gain", {{0, -6}, {2, 6}});
        p.tracks[0].clips[0].fadeOutCurve = FadeCurve::Exponential;
        p.tracks[0].clips[0].fadeOut = 0.5;
        p.masterFx = {fx("limiter", "ceiling=-2")};
        setLane(p.masterAutomation, "volume", {{0, -3}});
        p.markers = {{"intro", 0.5, -1}, {"chorus", 1.0, 1.8}};
        const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_rack";
        fs::remove_all(dir);
        fs::create_directories(dir);
        saveProject(p, dir / "p.json");
        const Project back = loadProject(dir / "p.json");
        CHECK(back.tracks[0].fx.size() == 2 && back.tracks[0].fx[0].type == "eq" && back.tracks[0].fx[0].get("b1_gain") == -6 && back.tracks[0].fx[1].bypass);
        CHECK(back.tracks[0].automation.size() == 1 && back.tracks[0].automation[0].target == "fx0.b1_gain" && back.tracks[0].automation[0].points.size() == 2);
        CHECK(back.tracks[0].clips[0].fadeOutCurve == FadeCurve::Exponential && back.tracks[0].clips[0].fadeInCurve == FadeCurve::SCurve);
        CHECK(back.masterFx.size() == 1 && back.masterFx[0].get("ceiling") == -2 && back.masterAutomation.size() == 1);
        CHECK(back.markers.size() == 2 && back.markers[1].name == "chorus" && back.markers[1].region() && !back.markers[0].region());
        CHECK(near(peakDb(mixdown(back)), peakDb(mixdown(p)), 1e-3));
        CHECK(rackFromJson(rackToJson(p.tracks[0].fx)).size() == 2 && lanesFromJson(lanesToJson(p.tracks[0].automation)).size() == 1);
        bool threw = false;
        try {
            rackFromJson("[{\"type\": \"wobble\"}]");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        fs::remove_all(dir);
    }

    return check::summary("audio_rack");
}
