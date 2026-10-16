#include <cmath>
#include <filesystem>
#include <fstream>

#include "wetwaresuite/audio/analysis.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/audio/project.hpp"
#include "wetwaresuite/core/process.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::audio;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

double rms(const std::vector<float>& x, std::size_t from = 0, std::size_t to = std::size_t(-1)) {
    to = std::min(to, x.size());
    double s = 0;
    for (std::size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
    return to > from ? std::sqrt(s / static_cast<double>(to - from)) : 0.0;
}

double peak(const Buffer& b) {
    double p = 0;
    for (const auto& c : b.channels) {
        for (float v : c) p = std::max(p, static_cast<double>(std::fabs(v)));
    }
    return p;
}

// Frequency from upward zero crossings in the middle of the channel.
double frequency(const Buffer& b, int ch = 0) {
    const auto& x = b.channels[static_cast<std::size_t>(ch)];
    const std::size_t from = x.size() / 5, to = x.size() * 4 / 5;
    int crossings = 0;
    std::size_t first = 0, last = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0 && x[i] >= 0) {
            if (!crossings) first = i;
            last = i;
            ++crossings;
        }
    }
    return crossings > 1 ? (crossings - 1) * static_cast<double>(b.rate) / static_cast<double>(last - first) : 0.0;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_audio_lib";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ---- generators and buffers ----
    Buffer t = tone(2, 1.0, 48000, 1000, 0.5);
    CHECK(t.frames() == 48000 && t.channelCount() == 2 && near(peak(t), 0.5, 1e-3));
    CHECK(near(frequency(t), 1000, 2));
    CHECK(near(rms(t.channels[0]), 0.5 / std::sqrt(2.0), 1e-3));
    Buffer pink = noise(1, 1.0, 44100, 0.5, NoiseColor::Pink);
    CHECK(near(peak(pink), 0.5, 1e-3));
    CHECK(rangeOf(t, 0.25, 0.5).begin == 12000 && rangeOf(t, 0.25, 0.5).end == 24000 && rangeOf(t, 0.5).end == 48000);
    CHECK(formatTime(62.35) == "1:02.350" && formatTime(0.5) == "0.500 s");

    // ---- WAV round trips ----
    for (const char* fmt : {"8", "16", "24", "32", "32f"}) {
        SaveOptions o;
        CHECK(parseSampleFormat(fmt, o.format));
        o.dither = false;
        const fs::path p = dir / (std::string("t") + fmt + ".wav");
        save(t, p, o);
        Buffer back = load(p);
        CHECK(back.rate == 48000 && back.channelCount() == 2 && back.frames() == t.frames());
        double err = 0;
        for (std::size_t i = 0; i < t.frames(); ++i) err = std::max(err, static_cast<double>(std::fabs(back.channels[1][i] - t.channels[1][i])));
        CHECK(err < (std::string(fmt) == "8" ? 0.01 : 1e-4));
    }
    CHECK(decode(encodeWav(t)).frames() == t.frames());
    {
        bool threw = false;
        try {
            load(dir / "missing.wav");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- compressed formats (encoded with ffmpeg, decoded built in) ----
    if (ffmpegAvailable()) {
        Buffer src = tone(2, 1.0, 44100, 440, 0.5);
        for (const char* ext : {"flac", "mp3", "ogg", "opus", "m4a"}) {
            const fs::path p = dir / (std::string("t.") + ext);
            save(src, p);
            CHECK(fs::file_size(p) > 500);
            Buffer back = load(p);
            CHECK(back.channelCount() == 2);
            CHECK(back.seconds() > 0.95 && back.seconds() < 1.15);
            CHECK(near(frequency(back), 440, 3));
        }
        // flac and ogg and mp3 decode in-process from bytes too
        for (const char* ext : {"flac", "mp3", "ogg"}) {
            std::ifstream in(dir / (std::string("t.") + ext), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            CHECK(decode(bytes).channelCount() == 2);
        }
    } else {
        bool threw = false;
        try {
            save(t, dir / "t.mp3");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- resampling ----
    {
        Buffer r = resample(t, 44100);
        CHECK(r.rate == 44100 && r.frames() == 44100);
        CHECK(near(frequency(r), 1000, 2));
        CHECK(near(rms(r.channels[0], 1000, 43000), 0.5 / std::sqrt(2.0), 0.005));
        Buffer cubic = resample(t, 22050, 0);
        CHECK(cubic.frames() == 22050 && near(frequency(cubic), 1000, 3));
        // a 15 kHz tone vanishes when downsampled to 16 kHz (anti-aliasing)
        Buffer high = tone(1, 0.5, 48000, 15000, 0.5);
        CHECK(rms(resample(high, 16000).channels[0], 400, 7600) < 0.01);
    }

    // ---- channels ----
    {
        Buffer m = t;
        setChannels(m, 1);
        CHECK(m.channelCount() == 1 && near(peak(m), 0.5, 1e-3));
        pan(m, -1.0, all(m));
        CHECK(m.channelCount() == 2 && rms(m.channels[1]) < 1e-6 && rms(m.channels[0]) > 0.3);
        swapChannels(m);
        CHECK(rms(m.channels[0]) < 1e-6);
        extractChannel(m, 1);
        CHECK(m.channelCount() == 1 && rms(m.channels[0]) > 0.3);
    }

    // ---- levels ----
    {
        Buffer b = t;
        gain(b, -6.0206, all(b));
        CHECK(near(peak(b), 0.25, 1e-3));
        CHECK(near(normalizePeak(b, -1.0, all(b)), 12.0412 - 1.0, 0.05));
        CHECK(near(gainToDb(peak(b)), -1.0, 0.01));
        // a part only
        Buffer c = t;
        gain(c, -20, rangeOf(c, 0.5), {1});
        CHECK(near(rms(c.channels[1], 0, 24000), rms(t.channels[1], 0, 24000), 1e-6));
        CHECK(rms(c.channels[1], 24000) < 0.05 && near(rms(c.channels[0], 24000), rms(t.channels[0], 24000), 1e-6));
    }

    // ---- loudness (EBU Tech 3341: stereo 1 kHz at -23 dBFS reads -23 LUFS) ----
    {
        Buffer ref = tone(2, 10.0, 48000, 1000, dbToGain(-23));
        CHECK(near(integratedLoudness(ref, all(ref)), -23.0, 0.15));
        Buffer ref44 = tone(2, 10.0, 44100, 1000, dbToGain(-23));
        CHECK(near(integratedLoudness(ref44, all(ref44)), -23.0, 0.15));
        Buffer mono = tone(1, 10.0, 48000, 1000, dbToGain(-23));
        CHECK(near(integratedLoudness(mono, all(mono)), -26.0, 0.15));
        Levels lv = measure(ref, all(ref));
        CHECK(near(lv.peakDb, -23, 0.01) && near(lv.truePeakDb, -23, 0.1) && near(lv.rmsDb, -26.01, 0.05));
        CHECK(lv.clipped == 0 && near(lv.loudnessRange, 0, 0.5) && near(lv.momentaryMaxLufs, -23, 0.2));
        Buffer quiet = silence(2, 2.0, 48000);
        CHECK(integratedLoudness(quiet, all(quiet)) <= -70);
        // normalise a voice-like signal to podcast loudness
        Buffer voice = noise(2, 6.0, 44100, 0.9, NoiseColor::Pink, 7);
        for (std::size_t i = 0; i < voice.frames(); ++i) {
            const float env = static_cast<float>(0.5 + 0.5 * std::sin(i * 2 * 3.14159 / 44100 * 2));
            for (auto& ch : voice.channels) ch[i] *= env;
        }
        normalizeLoudness(voice, -16.0, -1.0);
        const Levels v = measure(voice, all(voice));
        CHECK(near(v.lufs, -16.0, 0.6));
        CHECK(v.peakDb <= -0.99);
        Buffer clip = tone(1, 0.1, 44100, 100, 1.5);
        CHECK(measure(clip, all(clip), false).clipped > 100);
    }

    // ---- fades and editing ----
    {
        Buffer b = tone(1, 1.0, 1000, 50, 1.0, Waveform::Square);
        fadeIn(b, 0.5, all(b), FadeCurve::Linear);
        CHECK(std::fabs(b.channels[0][0]) < 1e-6 && near(std::fabs(b.channels[0][250]), 0.5, 0.01) && near(std::fabs(b.channels[0][700]), 1, 1e-6));
        fadeOut(b, 0.2, all(b), FadeCurve::SCurve);
        CHECK(std::fabs(b.channels[0][999]) < 0.01 && near(std::fabs(b.channels[0][700]), 1, 1e-6));
        FadeCurve c;
        CHECK(parseFadeCurve("equal_power", c) && c == FadeCurve::EqualPower && !parseFadeCurve("wobbly", c));

        Buffer e = tone(1, 1.0, 1000, 10, 1.0);
        Buffer removed = cut(e, rangeOf(e, 0.2, 0.5));
        CHECK(e.frames() == 700 && removed.frames() == 300);
        insert(e, 100, removed);
        CHECK(e.frames() == 1000);
        insertSilence(e, 0, 500);
        CHECK(e.frames() == 1500 && e.channels[0][10] == 0.0f);
        trim(e, rangeOf(e, 0.5));
        CHECK(e.frames() == 1000);
        Range kept = nonSilent([] {
            Buffer s = silence(1, 3.0, 1000);
            overwrite(s, 1000, tone(1, 1.0, 1000, 10, 0.5));
            return s;
        }(), -40);
        CHECK(near(static_cast<double>(kept.begin), 1000, 30) && near(static_cast<double>(kept.end), 2000, 30));
        Buffer r = tone(1, 1.0, 1000, 1, 1.0, Waveform::Saw);
        const float first = r.channels[0][1];
        reverse(r, all(r));
        CHECK(r.channels[0][998] == first);
        Buffer a = silence(2, 1.0, 1000);
        mixInto(a, tone(1, 1.0, 2000, 10, 0.5), 500, -6.0206);
        CHECK(a.frames() == 1500 && near(peak(a), 0.25, 0.01));
    }

    // ---- filters ----
    {
        Buffer hi = tone(1, 1.0, 44100, 8000, 0.5), lo = tone(1, 1.0, 44100, 60, 0.5);
        Buffer a = hi, b = lo;
        equalize(a, FilterType::LowPass, 1000, 0, 0.7071, all(a), {}, 2);
        equalize(b, FilterType::LowPass, 1000, 0, 0.7071, all(b), {}, 2);
        CHECK(rms(a.channels[0], 4410) < 0.01 && near(rms(b.channels[0], 4410), rms(lo.channels[0], 4410), 0.01));
        a = hi, b = lo;
        equalize(a, FilterType::HighPass, 200, 0, 0.7071, all(a));
        equalize(b, FilterType::HighPass, 200, 0, 0.7071, all(b));
        CHECK(near(rms(a.channels[0], 4410), rms(hi.channels[0], 4410), 0.01) && rms(b.channels[0], 4410) < 0.05);
        Buffer pk = tone(1, 1.0, 44100, 3000, 0.25);
        equalize(pk, FilterType::Peaking, 3000, 6.0206, 1.0, all(pk));
        CHECK(near(rms(pk.channels[0], 4410), 2 * 0.25 / std::sqrt(2.0), 0.01));
        Buffer sh = tone(1, 1.0, 44100, 50, 0.25);
        equalize(sh, FilterType::LowShelf, 200, -6.0206, 0.7071, all(sh));
        CHECK(near(rms(sh.channels[0], 4410), 0.5 * 0.25 / std::sqrt(2.0), 0.01));
        FilterType ft;
        CHECK(parseFilterType("highshelf", ft) && ft == FilterType::HighShelf && !parseFilterType("x", ft));
    }

    // ---- dynamics ----
    {
        Buffer b = tone(1, 1.0, 44100, 200, 1.0);
        CompressorSettings cs;
        cs.thresholdDb = -20;
        cs.ratio = 4;
        cs.kneeDb = 0;
        cs.attackMs = 1;
        const double reduction = compress(b, cs, all(b));
        CHECK(near(reduction, 15, 1.0));  // 0 dB in: 20 over, 5 over after
        CHECK(near(gainToDb(rms(b.channels[0], 22050) * std::sqrt(2.0)), -15, 1.0));
        Buffer l = tone(2, 1.0, 44100, 200, 1.0);
        gain(l, 6, all(l));
        limit(l, -1.0, 50, all(l));
        CHECK(peak(l) <= dbToGain(-1.0) + 1e-6 && peak(l) > dbToGain(-1.5));
        // gate: a tone burst over a noise floor
        Buffer g = noise(1, 2.0, 44100, dbToGain(-60), NoiseColor::White, 3);
        mixInto(g, tone(1, 0.5, 44100, 300, 0.5), 44100);
        GateSettings gs;
        gs.thresholdDb = -40;
        const double closed = gate(g, gs, all(g));
        CHECK(closed > 0.6 && closed < 0.8);
        CHECK(rms(g.channels[0], 0, 40000) < dbToGain(-90) && rms(g.channels[0], 50000, 60000) > 0.3);
    }

    // ---- noise reduction ----
    {
        Buffer hiss = noise(1, 3.0, 44100, 0.05, NoiseColor::White, 9);
        Buffer speech = hiss;
        mixInto(speech, tone(1, 1.5, 44100, 440, 0.4), 44100);
        NoiseProfile prof = learnNoise(speech, rangeOf(speech, 0, 0.9));
        CHECK(!prof.empty() && prof.rate == 44100);
        Buffer cleaned = speech;
        reduceNoise(cleaned, prof, 24, 1.5, all(cleaned));
        CHECK(rms(cleaned.channels[0], 4410, 40000) < rms(speech.channels[0], 4410, 40000) / 6);  // noise down > 15 dB
        CHECK(near(rms(cleaned.channels[0], 50000, 100000), rms(speech.channels[0], 50000, 100000), 0.03));  // tone kept
        NoiseProfile est = estimateNoise(speech);
        CHECK(!est.empty());
    }

    // ---- reverb, echo ----
    {
        Buffer imp = silence(2, 2.0, 44100);
        imp.channels[0][100] = imp.channels[1][100] = 1.0f;
        Buffer rv = imp;
        ReverbSettings rs;
        rs.wet = 0.5;
        reverb(rv, rs, all(rv));
        CHECK(rms(rv.channels[0], 10000, 40000) > 1e-4 && rms(rv.channels[1], 10000, 40000) > 1e-4);
        Buffer ec = imp;
        echo(ec, 0.25, 0.5, 1.0, all(ec));
        CHECK(near(ec.channels[0][100 + 11025], 1.0, 1e-5) && near(ec.channels[0][100 + 22050], 0.5, 1e-5));
        Buffer pp = imp;
        echo(pp, 0.25, 0.5, 1.0, all(pp), {}, true);
        CHECK(near(pp.channels[0][100 + 11025], 1.0, 1e-5) && std::fabs(pp.channels[1][100 + 11025]) < 1e-6 &&
              near(pp.channels[1][100 + 22050], 0.5, 1e-5));
    }

    // ---- time stretch and pitch ----
    {
        Buffer src = tone(2, 2.0, 44100, 440, 0.5);
        Buffer slow = stretch(src, 1.5);
        CHECK(near(slow.seconds(), 3.0, 0.01) && near(frequency(slow), 440, 4));
        Buffer fast = stretch(src, 0.5);
        CHECK(near(fast.seconds(), 1.0, 0.01) && near(frequency(fast), 440, 4));
        CHECK(near(rms(slow.channels[0], 10000, 100000), rms(src.channels[0], 10000, 80000), 0.05));
        Buffer up = pitchShift(src, 12);
        CHECK(up.frames() == src.frames() && near(frequency(up), 880, 8));
        Buffer down = pitchShift(src, -5);
        CHECK(near(frequency(down), 440 * std::pow(2.0, -5.0 / 12), 5));
        Buffer tape = changeSpeed(src, 2.0);
        CHECK(near(tape.seconds(), 1.0, 0.01) && near(frequency(tape), 880, 4));
    }

    // ---- silence ----
    {
        Buffer b = silence(1, 6.0, 8000);
        overwrite(b, 8000, tone(1, 1.0, 8000, 200, 0.5));
        overwrite(b, 32000, tone(1, 1.0, 8000, 200, 0.5));
        const auto quiet = detectSilence(b, -40, 0.5);
        CHECK(quiet.size() == 3);
        const auto parts = splitOnSilence(b, -40, 0.5, 0.25, 0.1);
        CHECK(parts.size() == 2);
        CHECK(parts.size() == 2 && near(parts[0].start, 0.9, 0.02) && near(parts[1].end, 5.1, 0.02));
    }

    // ---- pictures ----
    {
        WaveformStyle ws;
        ws.width = 400;
        ws.height = 200;
        auto img = waveform(t, all(t), ws);
        CHECK(img.width() == 400 && img.height() == 200);
        SpectrogramStyle ss;
        ss.width = 300;
        ss.height = 150;
        auto sp = spectrogram(t, all(t), ss);
        CHECK(sp.width() == 300 && sp.height() == 150);
        // the 1 kHz tone row is bright, a 10 kHz row dark (log scale 20..24000)
        ss.showGrid = false;
        sp = spectrogram(t, all(t), ss);
        auto rowOf = [&](double f) { return static_cast<int>((1 - std::log(f / 20) / std::log(24000.0 / 20)) * 150); };
        const auto bright = sp.at(150, rowOf(1000)), dark = sp.at(150, rowOf(10000));
        CHECK(bright.r + bright.g > dark.r + dark.g + 200);
    }

    // ---- multitrack ----
    {
        Project p;
        p.rate = 8000;
        p.channels = 2;
        Track music, voice;
        music.name = "music";
        voice.name = "voice";
        music.clips.push_back({"bed", std::make_shared<const Buffer>(tone(2, 6.0, 8000, 100, 0.5)), 0.0, 0.0, 0.5, 1.0});
        voice.clips.push_back({"talk", std::make_shared<const Buffer>(tone(1, 2.0, 8000, 300, 0.5)), 2.0, 0.0, 0.0, 0.0});
        music.duckUnder = 1;
        music.duckDb = -12;
        p.tracks = {music, voice};
        CHECK(near(p.seconds(), 6.0, 1e-9));
        Buffer mix = mixdown(p);
        CHECK(mix.frames() == 48000 && mix.channelCount() == 2);
        Buffer m0 = renderTrack(p, 0);
        // ducked while the voice talks (2..4 s), back up after
        CHECK(rms(m0.channels[0], 8000, 12000) > 0.3);
        CHECK(rms(m0.channels[0], 24000, 30000) < 0.1);
        CHECK(rms(m0.channels[0], 40000, 42000) > 0.3);
        CHECK(rms(m0.channels[0], 0, 100) < 0.05);  // fade in
        p.tracks[1].mute = true;
        CHECK(rms(mixdown(p).channels[0], 24000, 30000) < 0.1);
        p.tracks[1].solo = true;
        CHECK(rms(mixdown(p).channels[0], 0, 8000) < 1e-6);
        p.tracks[1].solo = false;
        p.tracks[0].envelope = {{0, 0}, {1, -60}};
        CHECK(rms(renderTrack(p, 0).channels[0], 10000, 14000) < 0.01);
        p.tracks[0].pan = 1;
        CHECK(rms(renderTrack(p, 0).channels[0]) < 1e-6);
        auto img = renderProject(p, 300, 40);
        CHECK(img.width() == 300 && img.height() == 2 * 40 + 16);
        saveProject(p, dir / "show.json");
        CHECK(fs::exists(dir / "show_clips" / "t1_c1.wav"));
        Project q = loadProject(dir / "show.json");
        CHECK(q.tracks.size() == 2 && q.tracks[0].clips.size() == 1 && q.tracks[1].clips[0].at == 2.0 && q.tracks[0].duckUnder == 1);
        CHECK(q.tracks[0].envelope.size() == 2 && q.tracks[0].pan == 1 && q.tracks[1].mute);
        CHECK(q.tracks[0].clips[0].audio->frames() == 48000);
    }

    fs::remove_all(dir);
    return check::summary("audio_lib");
}
