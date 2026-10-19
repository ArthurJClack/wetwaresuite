// The sound mixer: every audible clip's source range (WAV/MP3/FLAC/Ogg read
// built in, anything else and the sound of video files through ffmpeg), played
// backwards or time-stretched with its pitch kept, shaped by its keyframed
// volume, pan, fades and crossfades, and summed with track and master volume.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#include "media.hpp"
#include "render_internal.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/io.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

namespace {

constexpr double kEps = 1e-6;
constexpr double kPi = 3.14159265358979323846;

struct Source {
    std::shared_ptr<const audio::Buffer> buffer;
    double base = 0;  // source time of the buffer's first frame
};

std::mutex& cacheLock() {
    static std::mutex m;
    return m;
}
std::map<std::string, Source>& cache() {
    static std::map<std::string, Source> c;
    return c;
}

std::string lowerExt(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (auto& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return e;
}

std::string tnum(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    return buf;
}

Source sourceFor(const Clip& c, int rate) {
    const auto& builtin = audio::builtinReadExtensions();
    const bool native = std::find(builtin.begin(), builtin.end(), lowerExt(c.source)) != builtin.end();
    const std::string key = native ? c.source + "|" + std::to_string(rate) : c.source + "|" + std::to_string(rate) + "|" + tnum(c.in) + "|" + tnum(c.out);
    {
        std::lock_guard<std::mutex> g(cacheLock());
        if (auto it = cache().find(key); it != cache().end()) return it->second;
    }
    Source s;
    if (native) {
        s.buffer = std::make_shared<const audio::Buffer>(audio::conform(audio::load(c.source), rate, 2));
    } else {
        if (!ffmpegPath()) throw std::runtime_error("the sound of " + fs::path(c.source).filename().string() + " needs ffmpeg: " + missingFfmpeg());
        TempDir tmp;
        const fs::path wav = tmp.path() / "sound.wav";
        const std::string err = runFfmpeg({"ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-y", "-ss", tnum(c.in), "-t",
                                           tnum(std::max(0.01, c.out - c.in)), "-i", c.source, "-vn", "-ac", "2", "-ar", std::to_string(rate),
                                           "-c:a", "pcm_f32le", wav.string()});
        if (!err.empty()) throw std::runtime_error("couldn't read the sound of " + c.source + ": " + err);
        s.buffer = std::make_shared<const audio::Buffer>(audio::conform(audio::load(wav), rate, 2));
        s.base = c.in;
    }
    std::lock_guard<std::mutex> g(cacheLock());
    if (cache().size() > 24) cache().clear();
    cache()[key] = s;
    return s;
}

// Seconds this clip overlaps the one before it when they crossfade (0 when they don't).
double overlapIn(const Timeline& tl, const Track& t, const Clip& c) {
    if (c.transition.empty() || c.transition == "none") return 0;
    const Clip* prev = c.media() ? tl.previousMedia(t, c) : tl.previousClip(t, c);
    if (!prev) return 0;
    const double o = prev->end() - c.start;
    if (o <= 1e-3 || o > std::min(prev->length(), c.length()) + kEps) return 0;
    return o;
}

double overlapOut(const Timeline& tl, const Track& t, const Clip& c) {
    for (const auto& n : t.clips) {
        if (n.id == c.id || n.start < c.start - kEps) continue;
        const Clip* prev = n.media() ? tl.previousMedia(t, n) : tl.previousClip(t, n);
        if (prev && prev->id == c.id) return overlapIn(tl, t, n);
    }
    return 0;
}

}  // namespace

void clearSoundCache() {
    std::lock_guard<std::mutex> g(cacheLock());
    cache().clear();
}

bool hasSound(const Timeline& tl) {
    for (const auto& t : tl.tracks) {
        if (!tl.sounds(t)) continue;
        for (const auto& c : t.clips) {
            if (c.sound() && !c.mute && !c.hold && (c.kind != "nest" || (c.nested && hasSound(*c.nested)))) return true;
        }
    }
    return false;
}

audio::Buffer mixAudio(const Timeline& tl, double from, double to, int rate) {
    if (to <= from) to = from;
    const std::size_t total = static_cast<std::size_t>(std::llround((to - from) * rate));
    audio::Buffer out(2, total, rate);
    for (const auto& track : tl.tracks) {
        if (!tl.sounds(track)) continue;
        const bool audioTrack = track.kind == "audio";
        for (const auto& c : track.clips) {
            if (!c.sound() || c.mute || c.hold) continue;
            const double len = c.length();
            if (c.end() <= from + kEps || c.start >= to - kEps || len <= kEps) continue;
            // the clip's sound, as long as the clip
            audio::Buffer seg;
            if (c.kind == "nest") {
                if (!c.nested) continue;
                seg = mixAudio(*c.nested, c.in, c.out, rate);
            } else {
                Source src = sourceFor(c, rate);
                const auto& b = *src.buffer;
                const std::size_t a = std::min(b.frames(), static_cast<std::size_t>(std::max(0.0, (c.in - src.base) * rate)));
                const std::size_t e = std::min(b.frames(), static_cast<std::size_t>(std::max(0.0, (c.out - src.base) * rate)));
                seg = audio::slice(b, {a, std::max(a, e)});
            }
            if (c.reverse && !seg.empty()) audio::reverse(seg, audio::all(seg));
            if (std::abs(c.speed - 1) > kEps && !seg.empty() && seg.frames() > 64) {
                seg = audio::stretch(seg, std::clamp(1.0 / c.speed, 0.0501, 19.99));
            }
            const std::size_t frames = static_cast<std::size_t>(std::llround(len * rate));
            seg.resize(frames);
            if (seg.channelCount() != 2) audio::setChannels(seg, 2);
            const double tin = overlapIn(tl, track, c), tout = overlapOut(tl, track, c);
            const bool equalPower = audioTrack;
            const auto volKeys = c.keys.find("volume"), panKeys = c.keys.find("pan");
            const bool volAnimated = volKeys != c.keys.end() && !volKeys->second.empty();
            const bool panAnimated = panKeys != c.keys.end() && !panKeys->second.empty();
            const double level = track.volume * tl.masterVolume;
            // where it lands in the output
            const double startOut = c.start - from;
            const long first = static_cast<long>(std::llround(startOut * rate));
            const std::size_t i0 = first < 0 ? static_cast<std::size_t>(-first) : 0;
            double g = 0, pl = 1, pr = 1;
            for (std::size_t i = i0; i < frames; ++i) {
                const long o = first + static_cast<long>(i);
                if (o >= static_cast<long>(total)) break;
                if ((i - i0) % 32 == 0) {
                    // the envelope every 32 samples (keyframes, fades and crossfades change slowly)
                    const double local = static_cast<double>(i) / rate;
                    double v = volAnimated ? evaluateKeys(volKeys->second, local) : c.volume;
                    double f = 1;
                    if (c.fadeIn > kEps && local < c.fadeIn) f *= local / c.fadeIn;
                    if (c.fadeOut > kEps && local > len - c.fadeOut) f *= std::max(0.0, (len - local) / c.fadeOut);
                    if (tin > kEps && local < tin) {
                        const double u = local / tin;
                        f *= equalPower ? std::sin(u * kPi / 2) : u;
                    }
                    if (tout > kEps && local > len - tout) {
                        const double u = std::max(0.0, (len - local) / tout);
                        f *= equalPower ? std::sin(u * kPi / 2) : u;
                    }
                    g = v * f * level;
                    const double pan = std::clamp(panAnimated ? evaluateKeys(panKeys->second, local) : c.pan, -1.0, 1.0);
                    pl = pan > 0 ? 1 - pan : 1;
                    pr = pan < 0 ? 1 + pan : 1;
                }
                out.channels[0][static_cast<std::size_t>(o)] += static_cast<float>(seg.channels[0][i] * g * pl);
                out.channels[1][static_cast<std::size_t>(o)] += static_cast<float>(seg.channels[1][i] * g * pr);
            }
        }
    }
    return out;
}

}  // namespace wetwaresuite::cut
