// Multitrack mixing, project pictures and the project file format.
#include "wetwaresuite/audio/project.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "draw.hpp"

namespace wetwaresuite::audio {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

double Track::end() const {
    double e = 0;
    for (const auto& c : clips) e = std::max(e, c.end());
    return e;
}

double Project::seconds() const {
    double e = 0;
    for (const auto& t : tracks) e = std::max(e, t.end());
    return e;
}

namespace {

std::size_t framesOf(const Project& p) { return static_cast<std::size_t>(std::ceil(p.seconds() * p.rate)); }

// Clips, fades, clip gain, track gain and envelope; no ducking or pan.
Buffer renderDry(const Project& p, const Track& t, std::size_t frames) {
    Buffer out(p.channels, frames, p.rate);
    for (const auto& clip : t.clips) {
        if (!clip.audio || clip.audio->empty() || clip.seconds() <= 0) continue;
        const bool trimmed = clip.offset > 0 || clip.length >= 0;
        Buffer a = conform(trimmed ? slice(*clip.audio, rangeOf(*clip.audio, clip.offset, clip.offset + clip.seconds())) : *clip.audio,
                           p.rate, p.channels);
        if (clip.fadeIn > 0) fadeIn(a, clip.fadeIn, all(a), FadeCurve::SCurve);
        if (clip.fadeOut > 0) fadeOut(a, clip.fadeOut, all(a), FadeCurve::SCurve);
        mixInto(out, a, static_cast<std::size_t>(std::llround(std::max(0.0, clip.at) * p.rate)), clip.gainDb);
    }
    out.resize(frames);
    if (t.gainDb != 0) gain(out, t.gainDb, all(out));
    if (!t.envelope.empty()) {
        auto pts = t.envelope;
        std::sort(pts.begin(), pts.end(), [](const EnvelopePoint& a, const EnvelopePoint& b) { return a.time < b.time; });
        std::size_t k = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const double time = static_cast<double>(i) / p.rate;
            while (k + 1 < pts.size() && pts[k + 1].time <= time) ++k;
            double db = pts[k].gainDb;
            if (time < pts[0].time) db = pts[0].gainDb;
            else if (k + 1 < pts.size()) {
                const double span = pts[k + 1].time - pts[k].time;
                const double f = span > 0 ? (time - pts[k].time) / span : 1.0;
                db = pts[k].gainDb + (pts[k + 1].gainDb - pts[k].gainDb) * f;
            }
            const auto g = static_cast<float>(dbToGain(db));
            for (auto& c : out.channels) c[i] *= g;
        }
    }
    return out;
}

// Gain curve (dB per frame) that drops by duckDb while `key` is sounding.
std::vector<float> duckCurve(const Track& t, const Buffer& key, int rate) {
    const std::size_t n = key.frames();
    const std::size_t win = std::max<std::size_t>(1, static_cast<std::size_t>(rate / 50));  // 20 ms
    const std::size_t windows = (n + win - 1) / win;
    std::vector<char> on(windows, 0);
    const double thr = dbToGain(t.duckThresholdDb);
    for (std::size_t w = 0; w < windows; ++w) {
        double loudest = 0;
        for (const auto& c : key.channels) {
            double s = 0;
            const std::size_t e = std::min(n, (w + 1) * win);
            for (std::size_t i = w * win; i < e; ++i) s += static_cast<double>(c[i]) * c[i];
            loudest = std::max(loudest, std::sqrt(s / static_cast<double>(e - w * win)));
        }
        on[w] = loudest > thr;
    }
    // hold 300 ms through gaps between words, and start early by the attack time
    const auto hold = static_cast<std::size_t>(0.3 * 50), lead = static_cast<std::size_t>(std::max(0.0, t.duckAttack) * 50);
    std::vector<char> held = on;
    std::size_t since = hold + 1;
    for (std::size_t w = 0; w < windows; ++w) {
        since = on[w] ? 0 : since + 1;
        if (since <= hold) held[w] = 1;
    }
    for (std::size_t w = 0; w < windows; ++w) {
        if (on[w] && (w == 0 || !on[w - 1])) {
            for (std::size_t k = w > lead ? w - lead : 0; k < w; ++k) held[k] = 1;
        }
    }
    const double depth = std::min(0.0, t.duckDb);
    const double down = -depth / std::max(0.01, t.duckAttack) / rate, up = -depth / std::max(0.01, t.duckRelease) / rate;
    std::vector<float> curve(n);
    double cur = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double target = held[i / win] ? depth : 0.0;
        if (cur > target) cur = std::max(target, cur - down);
        else cur = std::min(target, cur + up);
        curve[i] = static_cast<float>(cur);
    }
    return curve;
}

void applyPan(Buffer& b, double position) {
    if (b.channelCount() < 2 || position == 0) return;
    pan(b, position, all(b));
}

}  // namespace

Buffer renderTrack(const Project& p, int index) {
    if (index < 0 || index >= static_cast<int>(p.tracks.size())) throw std::runtime_error("no track " + std::to_string(index));
    const std::size_t frames = framesOf(p);
    const Track& t = p.tracks[static_cast<std::size_t>(index)];
    Buffer out = renderDry(p, t, frames);
    if (t.duckUnder >= 0 && t.duckUnder < static_cast<int>(p.tracks.size()) && t.duckUnder != index) {
        const Buffer key = renderDry(p, p.tracks[static_cast<std::size_t>(t.duckUnder)], frames);
        const auto curve = duckCurve(t, key, p.rate);
        for (std::size_t i = 0; i < frames; ++i) {
            const auto g = static_cast<float>(dbToGain(curve[i]));
            for (auto& c : out.channels) c[i] *= g;
        }
    }
    applyPan(out, t.pan);
    return out;
}

Buffer mixdown(const Project& p) {
    const std::size_t frames = framesOf(p);
    Buffer out(p.channels, frames, p.rate);
    bool anySolo = false;
    for (const auto& t : p.tracks) anySolo = anySolo || t.solo;
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
        const Track& t = p.tracks[i];
        if (anySolo ? !t.solo : t.mute) continue;
        mixInto(out, renderTrack(p, static_cast<int>(i)), 0, 0.0);
    }
    if (p.masterGainDb != 0) gain(out, p.masterGainDb, all(out));
    return out;
}

raster::Image renderProject(const Project& p, int width, int laneHeight) {
    using raster::Color;
    width = std::clamp(width, 64, 8192);
    laneHeight = std::clamp(laneHeight, 24, 1024);
    const int lanes = std::max<int>(1, static_cast<int>(p.tracks.size()));
    const int ruler = 16;
    raster::Image img(width, lanes * laneHeight + ruler, {24, 26, 31, 255});
    const double total = std::max(1.0, p.seconds());
    bool anySolo = false;
    for (const auto& t : p.tracks) anySolo = anySolo || t.solo;
    const Color palette[] = {{90, 190, 255, 255}, {255, 170, 80, 255}, {120, 220, 140, 255}, {230, 120, 200, 255}, {240, 220, 100, 255}};
    for (std::size_t ti = 0; ti < p.tracks.size(); ++ti) {
        const Track& t = p.tracks[ti];
        const int top = static_cast<int>(ti) * laneHeight;
        const bool audible = anySolo ? t.solo : !t.mute;
        const Color col = audible ? palette[ti % 5] : Color{110, 112, 118, 255};
        detail::hline(img, 0, width - 1, top + laneHeight - 1, {70, 74, 82, 255});
        // clip boxes, then the track as it will sound: fades, gain, envelope and ducking included
        for (const auto& clip : t.clips) {
            if (!clip.audio) continue;
            const int x0 = static_cast<int>(clip.at / total * (width - 1));
            const int x1 = std::max(x0 + 1, static_cast<int>(clip.end() / total * (width - 1)));
            for (int x = x0; x <= x1; ++x) detail::vline(img, x, top + 3, top + laneHeight - 4, col, 0.18f);
            detail::vline(img, x0, top + 3, top + laneHeight - 4, col);
            detail::vline(img, x1, top + 3, top + laneHeight - 4, col, 0.5f);
        }
        if (t.clips.empty()) continue;
        const Buffer a = renderTrack(p, static_cast<int>(ti));
        const double mid = top + laneHeight / 2.0, half = laneHeight / 2.0 - 6;
        const double perPixel = total * p.rate / width;
        for (int x = 0; x < width; ++x) {
            const auto i0 = static_cast<std::size_t>(x * perPixel);
            const auto i1 = std::min(a.frames(), std::max(i0 + 1, static_cast<std::size_t>((x + 1) * perPixel)));
            if (i0 >= a.frames()) break;
            float lo = 0, hi = 0;
            for (const auto& c : a.channels) {
                for (std::size_t i = i0; i < i1; ++i) lo = std::min(lo, c[i]), hi = std::max(hi, c[i]);
            }
            if (hi - lo < 1e-4f) continue;
            const int y0 = static_cast<int>(mid - std::clamp(static_cast<double>(hi), -1.0, 1.0) * half);
            const int y1 = static_cast<int>(mid - std::clamp(static_cast<double>(lo), -1.0, 1.0) * half);
            detail::vline(img, x, y0, y1, col);
        }
        // track number in the corner
        detail::text(img, 4, top + 4, std::to_string(ti), {230, 230, 235, 255});
    }
    detail::timeGrid(img, 0, total, {60, 64, 72, 255}, true);
    return img;
}

void saveProject(const Project& p, const fs::path& path) {
    const fs::path dir = path.parent_path() / (path.stem().string() + "_clips");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error("can't create " + dir.string() + ": " + ec.message());
    json j = {{"wetwaresuite_audio_project", 1}, {"rate", p.rate}, {"channels", p.channels}, {"master_gain", p.masterGainDb}};
    json tracks = json::array();
    SaveOptions wav;
    wav.format = SampleFormat::Float32;
    for (std::size_t ti = 0; ti < p.tracks.size(); ++ti) {
        const Track& t = p.tracks[ti];
        json clips = json::array();
        for (std::size_t ci = 0; ci < t.clips.size(); ++ci) {
            const Clip& c = t.clips[ci];
            const std::string file = "t" + std::to_string(ti + 1) + "_c" + std::to_string(ci + 1) + ".wav";
            if (c.audio) save(*c.audio, dir / file, wav);
            clips.push_back({{"name", c.name}, {"file", (fs::path(dir.filename()) / file).generic_string()}, {"at", c.at}, {"gain", c.gainDb},
                             {"fade_in", c.fadeIn}, {"fade_out", c.fadeOut}, {"offset", c.offset}, {"length", c.length}});
        }
        json env = json::array();
        for (const auto& e : t.envelope) env.push_back({e.time, e.gainDb});
        tracks.push_back({{"name", t.name}, {"gain", t.gainDb}, {"pan", t.pan}, {"mute", t.mute}, {"solo", t.solo},
                          {"duck_under", t.duckUnder}, {"duck_db", t.duckDb}, {"duck_threshold", t.duckThresholdDb},
                          {"duck_attack", t.duckAttack}, {"duck_release", t.duckRelease}, {"envelope", env}, {"clips", clips}});
    }
    j["tracks"] = tracks;
    std::ofstream out(path);
    if (!(out << j.dump(2) << "\n")) throw std::runtime_error("can't write " + path.string());
}

Project loadProject(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("can't open " + path.string());
    json j;
    try {
        j = json::parse(in);
    } catch (const std::exception& e) {
        throw std::runtime_error(path.string() + " isn't a project file: " + e.what());
    }
    // achclwss_audio_project: projects saved before the rename
    if (!j.contains("wetwaresuite_audio_project") && !j.contains("achclwss_audio_project")) {
        throw std::runtime_error(path.string() + " isn't a wetwaresuite audio project");
    }
    Project p;
    p.rate = j.value("rate", 44100);
    p.channels = std::clamp(j.value("channels", 2), 1, 32);
    p.masterGainDb = j.value("master_gain", 0.0);
    for (const auto& jt : j.value("tracks", json::array())) {
        Track t;
        t.name = jt.value("name", std::string());
        t.gainDb = jt.value("gain", 0.0);
        t.pan = jt.value("pan", 0.0);
        t.mute = jt.value("mute", false);
        t.solo = jt.value("solo", false);
        t.duckUnder = jt.value("duck_under", -1);
        t.duckDb = jt.value("duck_db", -12.0);
        t.duckThresholdDb = jt.value("duck_threshold", -40.0);
        t.duckAttack = jt.value("duck_attack", 0.15);
        t.duckRelease = jt.value("duck_release", 0.6);
        for (const auto& e : jt.value("envelope", json::array())) {
            if (e.is_array() && e.size() == 2) t.envelope.push_back({e[0].get<double>(), e[1].get<double>()});
        }
        for (const auto& jc : jt.value("clips", json::array())) {
            Clip c;
            c.name = jc.value("name", std::string());
            c.at = jc.value("at", 0.0);
            c.gainDb = jc.value("gain", 0.0);
            c.fadeIn = jc.value("fade_in", 0.0);
            c.fadeOut = jc.value("fade_out", 0.0);
            c.offset = std::max(0.0, jc.value("offset", 0.0));
            c.length = jc.value("length", -1.0);
            const fs::path file = path.parent_path() / jc.value("file", std::string());
            Buffer a = load(file);
            if (a.rate != p.rate) a = resample(a, p.rate);
            c.audio = std::make_shared<const Buffer>(std::move(a));
            t.clips.push_back(std::move(c));
        }
        p.tracks.push_back(std::move(t));
    }
    return p;
}

}  // namespace wetwaresuite::audio
