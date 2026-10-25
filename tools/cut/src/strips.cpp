// What the window needs to show a timeline like an editor does: thumbnails and
// waveforms along each clip (read once per source, in the background) and a
// player that draws frames in order for live playback.
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

#include "internal.hpp"
#include "media.hpp"
#include "render_internal.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/raster/ops.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

namespace {

constexpr int kThumbHeight = 54;
constexpr double kPeaksPerSecond = 50;

struct StripCache {
    std::mutex lock;
    std::map<std::string, std::shared_ptr<const MediaStrip>> done;  // by source path + what
    std::map<std::string, bool> reading;
};

StripCache& strips() {
    static auto* c = new StripCache;  // never destroyed: readers may still finish while the program exits
    return *c;
}

// Frames spread along the source: about one every 40th of it, at least a second apart, at most 60.
void readFrames(const Clip& c, MediaStrip& out) {
    FrameContext ctx;
    int w = c.sourceWidth, h = c.sourceHeight;
    if (c.kind == "image") {
        auto still = ctx.impl().still(c.source);
        w = still->width();
        h = still->height();
        if (w <= 0 || h <= 0) return;
        out.frameHeight = kThumbHeight;
        out.frameWidth = std::max(8, static_cast<int>(std::lround(kThumbHeight * static_cast<double>(w) / h)));
        raster::Image small = raster::resize(*still, out.frameWidth, out.frameHeight);
        out.frameTimes.push_back(0);
        out.frames.emplace_back(small.data(), small.data() + static_cast<std::size_t>(small.width()) * small.height() * 4);
        return;
    }
    if (w <= 0 || h <= 0) w = 16, h = 9;
    out.frameHeight = kThumbHeight;
    out.frameWidth = std::max(8, static_cast<int>(std::lround(kThumbHeight * static_cast<double>(w) / h)) / 2 * 2);
    const double length = c.sourceDuration > 0 ? c.sourceDuration : std::max(c.out, 1.0);
    const int count = std::clamp(static_cast<int>(length / std::max(1.0, length / 40)), 1, 60);
    for (int i = 0; i < count; ++i) {
        const double t = length * (i + 0.5) / count;
        raster::Image frame;
        try {
            if (!ctx.impl().video(c.source, t, out.frameWidth, out.frameHeight, 1, frame) || frame.empty()) continue;
        } catch (const std::exception&) {
            break;  // no ffmpeg, or the file ended early: keep what we have
        }
        out.frameTimes.push_back(t);
        out.frames.emplace_back(frame.data(), frame.data() + static_cast<std::size_t>(frame.width()) * frame.height() * 4);
    }
}

// The loudest sample in each 1/50 s, mono, scaled so the loudest part of the file fills the height.
void readPeaks(const Clip& c, MediaStrip& out) {
    audio::Buffer sound;
    const std::string ext = extOf(c.source);
    const auto& builtin = audio::builtinReadExtensions();
    if (std::find(builtin.begin(), builtin.end(), ext) != builtin.end()) {
        sound = audio::load(c.source);
    } else {
        if (!ffmpegPath()) return;
        TempDir tmp;
        const fs::path wav = tmp.path() / "peaks.wav";
        const std::string err = runFfmpeg({"ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-y", "-i", c.source, "-vn",
                                           "-ac", "1", "-ar", "4000", "-c:a", "pcm_s16le", wav.string()});
        if (!err.empty()) return;
        sound = audio::load(wav);
    }
    if (sound.empty() || sound.rate <= 0) return;
    const double per = sound.rate / kPeaksPerSecond;
    const std::size_t n = static_cast<std::size_t>(std::ceil(sound.frames() / per));
    out.peaks.assign(n, 0.0f);
    for (const auto& ch : sound.channels) {
        for (std::size_t i = 0; i < ch.size(); ++i) {
            float& p = out.peaks[std::min(n - 1, static_cast<std::size_t>(i / per))];
            p = std::max(p, std::abs(ch[i]));
        }
    }
    const float top = *std::max_element(out.peaks.begin(), out.peaks.end());
    if (top > 1e-6f) {
        for (auto& p : out.peaks) p = std::min(1.0f, p / top);
    }
    out.peaksPerSecond = kPeaksPerSecond;
}

// The strip for one source and kind of media, read in the background the first time it's asked for.
std::shared_ptr<const MediaStrip> stripFor(const Clip& c, bool picture) {
    const std::string key = (picture ? "p|" : "s|") + c.source;
    StripCache& cache = strips();
    {
        std::lock_guard<std::mutex> g(cache.lock);
        if (auto it = cache.done.find(key); it != cache.done.end()) return it->second;
        if (cache.reading[key]) return nullptr;
        cache.reading[key] = true;
    }
    std::thread([c, picture, key] {
        auto strip = std::make_shared<MediaStrip>();
        try {
            if (picture) readFrames(c, *strip);
            else readPeaks(c, *strip);
        } catch (const std::exception&) {
            // an unreadable source just shows no thumbnails
        }
        StripCache& cache = strips();
        std::lock_guard<std::mutex> g(cache.lock);
        cache.done[key] = strip;
        cache.reading.erase(key);
    }).detach();
    return nullptr;
}

class TimelinePlayer : public FramePlayer {
public:
    explicit TimelinePlayer(const Timeline& tl) : tl_(tl) {}
    bool frame(double seconds, int maxWidth, int& width, int& height, std::vector<std::uint8_t>& rgba) override {
        if (tl_.duration() <= 0) return false;
        FrameRequest rq;
        rq.t = std::clamp(seconds, 0.0, std::max(0.0, tl_.duration() - 1.0 / tl_.settings.fps));
        rq.width = std::min(tl_.settings.width, std::max(16, maxWidth));
        try {
            raster::Image img = renderFrame(tl_, rq, &ctx_);
            width = img.width();
            height = img.height();
            rgba.assign(img.data(), img.data() + static_cast<std::size_t>(width) * height * 4);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    double fps() const override { return tl_.settings.fps; }

private:
    Timeline tl_;  // a copy: the document may change while this plays
    FrameContext ctx_;
};

}  // namespace

bool Timeline::clipMedia(const std::string& clipId, MediaStrip& out) const {
    const Clip* c = findClip(clipId);
    if (!c || c->source.empty()) return false;
    const bool picture = c->kind == "video" || c->kind == "image";
    const bool sound = c->kind == "audio" || (c->kind == "video" && c->ownSound && c->hasAudio);
    if (!picture && !sound) return false;
    out = MediaStrip{};
    std::shared_ptr<const MediaStrip> frames = picture ? stripFor(*c, true) : nullptr;
    std::shared_ptr<const MediaStrip> peaks = sound ? stripFor(*c, false) : nullptr;
    out.pending = (picture && !frames) || (sound && !peaks);
    if (frames) {
        out.frameWidth = frames->frameWidth;
        out.frameHeight = frames->frameHeight;
        out.frameTimes = frames->frameTimes;
        out.frames = frames->frames;
    }
    if (peaks) {
        out.peaks = peaks->peaks;
        out.peaksPerSecond = peaks->peaksPerSecond;
    }
    return true;
}

std::unique_ptr<FramePlayer> Timeline::player() const { return std::make_unique<TimelinePlayer>(*this); }

void clearStrips() {
    StripCache& cache = strips();
    std::lock_guard<std::mutex> g(cache.lock);
    cache.done.clear();
}

}  // namespace wetwaresuite::cut
