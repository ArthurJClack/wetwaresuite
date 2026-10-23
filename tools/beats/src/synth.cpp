// The sound: a subtractive synth voice, a synthesized drum kit, the sampler,
// per-track effects and the mixdown of the arrangement.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/rack.hpp"
#include "wetwaresuite/beats/song.hpp"

namespace wetwaresuite::beats {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Small deterministic noise source (same output on every platform).
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}
    double next() {  // -1..1
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (s / 4294967295.0) * 2.0 - 1.0;
    }
};

double polyBlep(double t, double dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

struct Oscillator {
    Osc type = Osc::Saw;
    double phase = 0;
    double tri = 0;  // integrated square for the triangle
    Rng rng{12345};

    double tick(double freq, double rate) {
        const double dt = std::min(0.5, freq / rate);
        double v = 0;
        switch (type) {
            case Osc::Saw:
                v = 2.0 * phase - 1.0 - polyBlep(phase, dt);
                break;
            case Osc::Square:
            case Osc::Triangle: {
                double sq = phase < 0.5 ? 1.0 : -1.0;
                sq += polyBlep(phase, dt);
                sq -= polyBlep(std::fmod(phase + 0.5, 1.0), dt);
                if (type == Osc::Square) {
                    v = sq;
                } else {
                    tri = dt * sq + (1.0 - dt) * tri;  // leaky integrator
                    v = tri * 4.0;
                }
                break;
            }
            case Osc::Sine:
                v = std::sin(2.0 * kPi * phase);
                break;
            case Osc::Noise:
                v = rng.next();
                break;
            case Osc::Off:
                v = 0;
                break;
        }
        phase += dt;
        if (phase >= 1.0) phase -= 1.0;
        return v;
    }
};

// Topology-preserving state variable filter (Zavalishin), low-pass output.
struct Svf {
    double ic1 = 0, ic2 = 0;
    double lowpass(double x, double cutoff, double res, double rate) {
        cutoff = std::clamp(cutoff, 20.0, rate * 0.45);
        const double g = std::tan(kPi * cutoff / rate);
        const double k = 2.0 - 1.95 * std::clamp(res, 0.0, 1.0);
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return v2;
    }
    double highpass(double x, double cutoff, double res, double rate) {
        cutoff = std::clamp(cutoff, 20.0, rate * 0.45);
        const double g = std::tan(kPi * cutoff / rate);
        const double k = 2.0 - 1.95 * std::clamp(res, 0.0, 1.0);
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return x - k * v1 - v2;
    }
    double bandpass(double x, double cutoff, double res, double rate) {
        cutoff = std::clamp(cutoff, 20.0, rate * 0.45);
        const double g = std::tan(kPi * cutoff / rate);
        const double k = 2.0 - 1.95 * std::clamp(res, 0.0, 1.0);
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return v1;
    }
};

// ADSR level at time t (seconds) for a note held `held` seconds.
double adsr(const SynthSettings& s, double t, double held) {
    auto level = [&](double u) {
        if (u < s.attack) return s.attack > 0 ? u / s.attack : 1.0;
        u -= s.attack;
        if (u < s.decay) return 1.0 - (1.0 - s.sustain) * (s.decay > 0 ? u / s.decay : 1.0);
        return s.sustain;
    };
    if (t < held) return level(t);
    const double atRelease = level(held);
    const double r = std::max(0.001, s.release);
    return t - held < r ? atRelease * (1.0 - (t - held) / r) : 0.0;
}

std::size_t framesFor(double seconds, int rate) { return static_cast<std::size_t>(std::max(0.0, seconds) * rate + 0.5); }

}  // namespace

audio::Buffer synthNote(const SynthSettings& st, int rate, double frequency, double seconds, double velocity) {
    const double total = seconds + std::max(0.001, st.release);
    audio::Buffer out(1, framesFor(total, rate), rate);
    // unison: each oscillator is `voices` copies spread evenly over unisonSpread cents
    const int voices = std::clamp(st.unison, 1, 7);
    std::vector<Oscillator> o1(static_cast<std::size_t>(voices)), o2(static_cast<std::size_t>(voices));
    std::vector<double> spread(static_cast<std::size_t>(voices), 0.0);
    for (int v = 0; v < voices; ++v) {
        o1[static_cast<std::size_t>(v)].type = st.osc1;
        o2[static_cast<std::size_t>(v)].type = st.osc2;
        o2[static_cast<std::size_t>(v)].rng = Rng(777u + static_cast<std::uint32_t>(v));
        o1[static_cast<std::size_t>(v)].rng = Rng(12345u + static_cast<std::uint32_t>(v));
        if (voices > 1) {
            spread[static_cast<std::size_t>(v)] = (static_cast<double>(v) / (voices - 1) - 0.5) * st.unisonSpread / 1200.0;
            o1[static_cast<std::size_t>(v)].phase = std::fmod(0.37 * v, 1.0);  // start apart so they don't sum into one loud attack
            o2[static_cast<std::size_t>(v)].phase = std::fmod(0.61 * v, 1.0);
        }
    }
    const double cents = st.detune / 1200.0;
    const double f1 = frequency * std::pow(2.0, st.octave - cents * 0.5);
    const double f2 = frequency * std::pow(2.0, st.octave + st.osc2Semitones / 12.0 + cents * 0.5);
    const double l2 = st.osc2 == Osc::Off ? 0.0 : std::clamp(st.osc2Level, 0.0, 1.0);
    const double l1 = st.osc1 == Osc::Off ? 0.0 : 1.0;
    const double norm = 1.0 / std::max(1.0, l1 + l2 * 0.8) / std::sqrt(static_cast<double>(voices));
    std::vector<double> ratio(static_cast<std::size_t>(voices));
    for (int v = 0; v < voices; ++v) ratio[static_cast<std::size_t>(v)] = std::pow(2.0, spread[static_cast<std::size_t>(v)]);
    Svf f;
    auto& d = out.channels[0];
    const double fd = std::max(0.005, st.filterDecay);
    const double lfoStep = std::max(0.0, st.lfoRate) / rate;
    const double depth = st.lfoDepth;
    double lfoPhase = 0;
    for (std::size_t i = 0; i < d.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        double lfo = 0;
        if (st.lfoTarget != LfoTarget::Off && depth != 0) {
            switch (st.lfoShape) {
                case 1: lfo = lfoPhase < 0.5 ? 4 * lfoPhase - 1 : 3 - 4 * lfoPhase; break;
                case 2: lfo = lfoPhase < 0.5 ? 1 : -1; break;
                case 3: lfo = 2 * lfoPhase - 1; break;
                default: lfo = std::sin(2 * kPi * lfoPhase);
            }
            lfoPhase += lfoStep;
            if (lfoPhase >= 1) lfoPhase -= 1;
        }
        const double bend = st.lfoTarget == LfoTarget::Pitch ? std::pow(2.0, depth * lfo / 12.0) : 1.0;
        double env = adsr(st, t, seconds);
        if (st.lfoTarget == LfoTarget::Amp) env *= 1.0 - std::clamp(depth, 0.0, 1.0) * (0.5 + 0.5 * lfo);
        double x = 0;
        for (std::size_t v = 0; v < static_cast<std::size_t>(voices); ++v) {
            if (l1 > 0) x += l1 * o1[v].tick(f1 * bend * ratio[v], rate);
            if (l2 > 0) x += l2 * o2[v].tick(f2 * bend / ratio[v], rate);
        }
        x *= norm;
        double cutoff = st.cutoff * std::pow(2.0, st.filterEnv * std::exp(-t / fd));
        if (st.lfoTarget == LfoTarget::Filter) cutoff *= std::pow(2.0, depth * lfo);
        double y;
        switch (st.filter) {
            case FilterMode::HighPass: y = f.highpass(x, cutoff, st.resonance, rate); break;
            case FilterMode::BandPass: y = f.bandpass(x, cutoff, st.resonance, rate); break;
            default: y = f.lowpass(x, cutoff, st.resonance, rate);
        }
        d[i] = static_cast<float>(y * env * velocity * 0.5);
    }
    return out;
}

audio::Buffer metronomeClick(int rate, bool accent) {
    audio::Buffer b(1, framesFor(0.03, rate), rate);
    const double hz = accent ? 1600.0 : 1000.0, amp = accent ? 1.0 : 0.6;
    for (std::size_t i = 0; i < b.frames(); ++i) {
        const double t = static_cast<double>(i) / rate;
        b.channels[0][i] = static_cast<float>(std::sin(2 * kPi * hz * t) * std::exp(-t / 0.008) * amp);
    }
    return b;
}

audio::Buffer drumHit(const std::string& voice, int rate) {
    const double r = rate;
    auto make = [&](double seconds) { return audio::Buffer(1, framesFor(seconds, rate), rate); };
    Rng rng(4242);
    audio::Buffer b;
    if (voice == "kick") {
        b = make(0.5);
        double phase = 0;
        for (std::size_t i = 0; i < b.frames(); ++i) {
            const double t = static_cast<double>(i) / r;
            const double f = 48 + 110 * std::exp(-t / 0.035);
            phase += f / r;
            const double amp = std::exp(-t / 0.22);
            const double click = std::exp(-t / 0.003) * 0.4;
            b.channels[0][i] = static_cast<float>(std::tanh(1.6 * std::sin(2 * kPi * phase) * amp) * 0.95 + click * rng.next() * 0.3);
        }
    } else if (voice == "snare") {
        b = make(0.35);
        Svf hp;
        double phase = 0;
        for (std::size_t i = 0; i < b.frames(); ++i) {
            const double t = static_cast<double>(i) / r;
            phase += (185 + 40 * std::exp(-t / 0.02)) / r;
            const double tone = std::sin(2 * kPi * phase) * std::exp(-t / 0.06) * 0.55;
            const double n = hp.highpass(rng.next(), 1800, 0.1, r) * std::exp(-t / 0.11) * 0.7;
            b.channels[0][i] = static_cast<float>(tone + n);
        }
    } else if (voice == "hat" || voice == "openhat" || voice == "crash" || voice == "shaker") {
        const double decay = voice == "hat" ? 0.04 : voice == "openhat" ? 0.28 : voice == "crash" ? 0.9 : 0.06;
        const double cut = voice == "crash" ? 5000 : voice == "shaker" ? 6500 : 8000;
        b = make(decay * 6);
        Svf hp, hp2;
        for (std::size_t i = 0; i < b.frames(); ++i) {
            const double t = static_cast<double>(i) / r;
            double env = std::exp(-t / decay);
            if (voice == "shaker") env *= std::min(1.0, t / 0.012);
            const double n = hp2.highpass(hp.highpass(rng.next(), cut, 0.2, r), cut, 0.2, r);
            b.channels[0][i] = static_cast<float>(n * env * (voice == "crash" ? 0.5 : 0.45));
        }
    } else if (voice == "clap") {
        b = make(0.4);
        Svf bp;
        for (std::size_t i = 0; i < b.frames(); ++i) {
            const double t = static_cast<double>(i) / r;
            double env = 0;
            for (int k = 0; k < 3; ++k) {
                const double tk = t - k * 0.011;
                if (tk >= 0 && tk < 0.011) env = std::max(env, std::exp(-tk / 0.004));
            }
            const double tail = t - 0.033;
            if (tail >= 0) env = std::max(env, std::exp(-tail / 0.12));
            b.channels[0][i] = static_cast<float>(bp.bandpass(rng.next(), 1200, 0.35, r) * env * 1.6);
        }
    } else if (voice == "tom" || voice == "hitom" || voice == "lotom") {
        const double base = voice == "hitom" ? 200 : voice == "lotom" ? 95 : 140;
        b = make(0.6);
        double phase = 0;
        for (std::size_t i = 0; i < b.frames(); ++i) {
            const double t = static_cast<double>(i) / r;
            phase += (base + base * 0.5 * std::exp(-t / 0.05)) / r;
            b.channels[0][i] = static_cast<float>(std::sin(2 * kPi * phase) * std::exp(-t / 0.2) * 0.8 + rng.next() * std::exp(-t / 0.004) * 0.15);
        }
    } else if (voice == "rim") {
        b = make(0.08);
        Svf bp;
        double phase = 0;
        for (std::size_t i = 0; i < b.frames(); ++i) {
            const double t = static_cast<double>(i) / r;
            phase += 1700 / r;
            const double v = bp.bandpass(rng.next() * 0.6 + std::sin(2 * kPi * phase), 1700, 0.6, r);
            b.channels[0][i] = static_cast<float>(v * std::exp(-t / 0.012) * 1.4);
        }
    } else if (voice == "cowbell") {
        b = make(0.45);
        Oscillator a, c;
        a.type = c.type = Osc::Square;
        Svf bp;
        for (std::size_t i = 0; i < b.frames(); ++i) {
            const double t = static_cast<double>(i) / r;
            const double v = bp.bandpass((a.tick(540, r) + c.tick(800, r)) * 0.5, 2600, 0.4, r);
            b.channels[0][i] = static_cast<float>(v * std::exp(-t / 0.12) * 1.3);
        }
    } else {
        b = make(0.01);
    }
    return b;
}

namespace {

// Adds `src` scaled by gain at frame `at`, growing `dst` as needed.
void addAt(audio::Buffer& dst, const audio::Buffer& src, std::size_t at, double gain) {
    if (src.empty()) return;
    if (dst.frames() < at + src.frames()) dst.resize(at + src.frames());
    auto& d = dst.channels[0];
    const auto& s = src.channels[0];
    for (std::size_t i = 0; i < s.size(); ++i) d[at + i] += static_cast<float>(s[i] * gain);
}

// Pitched playback of a mono sample with linear interpolation and the synth's attack/release.
audio::Buffer samplerNote(const Track& t, double semitones, double seconds, double velocity, int rate) {
    if (!t.sample.audio || t.sample.audio->empty()) return {};
    const auto& src = t.sample.audio->channels[0];
    const double ratio = std::pow(2.0, semitones / 12.0);
    const double rel = std::max(0.005, t.synth.release);
    const double maxSeconds = static_cast<double>(src.size()) / ratio / rate;
    const double total = std::min(seconds + rel, maxSeconds);
    audio::Buffer out(1, framesFor(total, rate), rate);
    SynthSettings env = t.synth;
    env.decay = 0;
    env.sustain = 1;
    for (std::size_t i = 0; i < out.frames(); ++i) {
        const double pos = static_cast<double>(i) * ratio;
        const auto k = static_cast<std::size_t>(pos);
        if (k + 1 >= src.size()) break;
        const double frac = pos - static_cast<double>(k);
        const double v = src[k] * (1.0 - frac) + src[k + 1] * frac;
        out.channels[0][i] = static_cast<float>(v * adsr(env, static_cast<double>(i) / rate, seconds) * velocity);
    }
    return out;
}

void distort(audio::Buffer& b, double drive) {
    if (drive <= 0) return;
    const double g = 1.0 + drive * 24.0;
    const double makeup = 1.0 / std::tanh(g * 0.5);
    for (auto& ch : b.channels) {
        for (auto& x : ch) x = static_cast<float>(std::tanh(x * g) * makeup * 0.5);
    }
}

// The track, mono voices to stereo with effects, gain and pan; empty when it plays nothing.
audio::Buffer renderTrackBuffer(const Song& s, int index, double fromBeat, double toBeat, double tail) {
    const Track& t = s.tracks[static_cast<std::size_t>(index)];
    const int rate = s.rate;
    const double spb = s.secondsPerBeat();
    const std::size_t total = framesFor((toBeat - fromBeat) * spb + tail, rate);
    audio::Buffer mono(1, total, rate);
    std::map<std::string, audio::Buffer> hits;
    bool any = false;
    for (const Event& e : trackEvents(s, index)) {
        if (e.beat < fromBeat - 1e-9 || e.beat >= toBeat - 1e-9) continue;
        const std::size_t at = framesFor((e.beat - fromBeat) * spb, rate);
        if (at >= total) continue;
        const double vel = e.velocity / 127.0;
        const double len = e.length * spb;
        any = true;
        if (t.kind == TrackKind::Drum) {
            auto it = hits.find(e.voice);
            if (it == hits.end()) {
                audio::Buffer hit;
                if (auto slot = t.slots.find(e.voice); slot != t.slots.end()) {
                    if (slot->second.audio) hit = *slot->second.audio;
                } else {
                    hit = drumHit(e.voice, rate);
                }
                if (auto pad = t.pads.find(e.voice); pad != t.pads.end() && !hit.empty()) {
                    const Pad& pd = pad->second;
                    if (pd.tune != 0) hit = audio::changeSpeed(hit, std::pow(2.0, pd.tune / 12.0));
                    if (pd.decay > 0) {
                        hit.resize(std::min(hit.frames(), framesFor(pd.decay, rate)));
                        audio::fadeOut(hit, pd.decay, audio::all(hit), audio::FadeCurve::Exponential);
                    }
                    if (pd.volumeDb != 0) audio::gain(hit, pd.volumeDb, audio::all(hit));
                }
                it = hits.emplace(e.voice, std::move(hit)).first;
            }
            addAt(mono, it->second, at, vel);
        } else if (t.kind == TrackKind::Synth) {
            addAt(mono, synthNote(t.synth, rate, noteFrequency(e.pitch), len, vel), at, 1.0);
        } else {
            addAt(mono, samplerNote(t, e.pitch - t.root + 12.0 * t.synth.octave, len, vel, rate), at, 1.0);
        }
    }
    if (!any) return {};
    mono.resize(total);
    distort(mono, t.fx.distortion);
    if (t.fx.compress > 0) {
        audio::CompressorSettings c;
        c.thresholdDb = -6 - 24 * t.fx.compress;
        c.ratio = 2 + 6 * t.fx.compress;
        c.attackMs = 5;
        c.releaseMs = 100;
        c.autoMakeup = true;
        audio::compress(mono, c, audio::all(mono));
    }
    audio::Buffer st = mono;
    const audio::Automation* panLane = audio::findLane(t.automation, "pan");
    audio::pan(st, panLane ? 0.0 : std::clamp(t.pan, -1.0, 1.0), audio::all(st));  // mono -> constant-power stereo
    if (st.channelCount() == 1) audio::setChannels(st, 2);
    if (t.fx.delayMix > 0) {
        audio::echo(st, std::max(0.01, t.fx.delayBeats * spb), std::clamp(t.fx.delayFeedback, 0.0, 0.95), std::clamp(t.fx.delayMix, 0.0, 1.0),
                    audio::all(st), {}, true);
    }
    if (t.fx.reverb > 0) {
        audio::ReverbSettings rv;
        rv.wet = std::clamp(t.fx.reverb, 0.0, 1.0);
        rv.roomSize = std::clamp(t.fx.reverbSize, 0.0, 1.0);
        audio::reverb(st, rv, audio::all(st));
    }
    audio::applyRack(st, t.rack, t.automation, fromBeat, spb);
    audio::gain(st, t.volumeDb, audio::all(st));
    if (const audio::Automation* vol = audio::findLane(t.automation, "volume")) audio::applyVolumeLane(st, *vol, fromBeat, spb);
    if (panLane) audio::applyPanLane(st, *panLane, fromBeat, spb);
    return st;
}

}  // namespace

audio::Buffer render(const Song& s, const RenderOptions& o) {
    const int endBar = o.bars < 0 ? s.bars() : o.startBar + o.bars;
    const double fromBeat = o.startBar * s.beatsInBar();
    const double toBeat = std::max(fromBeat, endBar * s.beatsInBar());
    double tail = o.tailSeconds;
    if (tail < 0) {
        tail = 0.25;
        for (const auto& t : s.tracks) {
            tail = std::max(tail, t.kind == TrackKind::Drum ? 0.6 : t.synth.release + 0.05);
            if (t.fx.reverb > 0) tail = std::max(tail, 1.0 + 2.0 * t.fx.reverbSize);
            if (t.fx.delayMix > 0) tail = std::max(tail, std::min(3.0, t.fx.delayBeats * s.secondsPerBeat() * 4));
            tail = std::max(tail, audio::rackTail(t.rack));
        }
        tail = std::min(tail + (o.master ? audio::rackTail(s.masterRack) : 0.0), 8.0);
    }
    const std::size_t frames = framesFor((toBeat - fromBeat) * s.secondsPerBeat() + tail, s.rate);
    audio::Buffer mix(2, frames, s.rate);
    bool anySolo = false;
    for (const auto& t : s.tracks) anySolo = anySolo || t.solo;
    for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
        const Track& t = s.tracks[static_cast<std::size_t>(i)];
        if (o.onlyTrack >= 0) {
            if (i != o.onlyTrack) continue;
        } else if (t.mute || (anySolo && !t.solo)) {
            continue;
        }
        audio::Buffer tb = renderTrackBuffer(s, i, fromBeat, toBeat, tail);
        if (tb.empty()) continue;
        audio::mixInto(mix, tb, 0, 0.0);
    }
    mix.resize(frames);
    audio::gain(mix, s.masterDb - 3.0, audio::all(mix));  // a little headroom for summing tracks
    if (o.master) {
        audio::applyRack(mix, s.masterRack, s.masterAutomation, fromBeat, s.secondsPerBeat());
        if (const audio::Automation* vol = audio::findLane(s.masterAutomation, "volume")) audio::applyVolumeLane(mix, *vol, fromBeat, s.secondsPerBeat());
    }
    if (o.click > 0 || (o.click < 0 && s.metronome)) {
        // a click per beat of the time signature (eighths in 6/8), accented on each bar
        const double step = 4.0 / std::max(1, s.beatUnit);
        const audio::Buffer hi = metronomeClick(s.rate, true), lo = metronomeClick(s.rate, false);
        const double g = audio::dbToGain(s.metronomeDb);
        int k = static_cast<int>(std::ceil(fromBeat / step - 1e-9));
        for (double beat = k * step; beat < toBeat - 1e-9; beat = ++k * step) {
            const bool accent = k % std::max(1, s.beatsPerBar) == 0;
            const std::size_t at = framesFor((beat - fromBeat) * s.secondsPerBeat(), s.rate);
            const audio::Buffer& c = accent ? hi : lo;
            for (std::size_t i = 0; i < c.frames() && at + i < mix.frames(); ++i) {
                mix.channels[0][at + i] += static_cast<float>(c.channels[0][i] * g);
                mix.channels[1][at + i] += static_cast<float>(c.channels[0][i] * g);
            }
        }
    }
    if (o.limit) audio::limit(mix, -1.0, 80, audio::all(mix));
    // drop the silent part of the tail
    if (o.tailSeconds < 0) {
        const std::size_t musicEnd = framesFor((toBeat - fromBeat) * s.secondsPerBeat(), s.rate);
        std::size_t last = musicEnd;
        for (std::size_t i = frames; i > musicEnd; --i) {
            if (std::fabs(mix.channels[0][i - 1]) > 1e-4f || std::fabs(mix.channels[1][i - 1]) > 1e-4f) {
                last = i;
                break;
            }
        }
        mix.resize(std::min(frames, last + static_cast<std::size_t>(s.rate / 50)));
    }
    return mix;
}

}  // namespace wetwaresuite::beats
