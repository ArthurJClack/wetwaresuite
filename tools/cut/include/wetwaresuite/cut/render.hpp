// The picture compositor and the sound mixer behind previews, playback and
// export. Both are plain C++ over libs/raster and libs/audio: stills, colour
// mattes, titles and WAV/MP3/FLAC/Ogg sound need no ffmpeg; video files are
// decoded through ffmpeg (or a hook, so tests can feed frames).
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "wetwaresuite/audio/buffer.hpp"
#include "wetwaresuite/cut/cut.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::cut {

// Supplies a video file's picture at a source time, scaled to width x height. Returns false
// when it can't. The default (an empty function) decodes with ffmpeg.
using VideoFrameFn = std::function<bool(const std::string& path, double sourceTime, int width, int height, raster::Image& out)>;
void setVideoFrameSource(VideoFrameFn fn);

// Decoders and caches that live across frames (open video readers, decoded stills, drawn
// titles). Export keeps one for the whole run; previews use a shared one.
class FrameContext {
public:
    FrameContext();
    ~FrameContext();
    FrameContext(const FrameContext&) = delete;
    FrameContext& operator=(const FrameContext&) = delete;
    struct Impl;
    Impl& impl() { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

struct FrameRequest {
    double t = 0;                 // timeline seconds
    int width = 0, height = 0;    // output size (0 = the timeline's; one of them 0 keeps the aspect)
    std::string reframe = "fit";  // when the output's shape differs from the timeline's: fit or fill
    bool transparent = false;     // no background (nested sequences over what's below)
};

// The composited picture at one moment. Throws std::runtime_error when a source can't be read.
raster::Image renderFrame(const Timeline& timeline, const FrameRequest& request, FrameContext* context = nullptr);

// Everything audible from `from` to `to` timeline seconds, mixed to stereo at `rate`:
// clip volume (and its keyframes), pan, fades, crossfades, track volume, mute/solo and the
// master volume. Speed changes keep the pitch. Throws std::runtime_error.
audio::Buffer mixAudio(const Timeline& timeline, double from, double to, int rate = 48000);
// Whether anything on the timeline makes sound.
bool hasSound(const Timeline& timeline);

// Drops every cached decoded still, title and sound (after files changed on disk).
void clearCaches();

// ---- the colour and effect building blocks (exposed for tests) ----

// A 3D LUT read from a .cube file. Throws std::runtime_error.
struct Lut3d {
    int size = 0;
    std::vector<float> table;  // size^3 * 3, red fastest
};
Lut3d loadCube(const std::string& path);
Lut3d parseCube(const std::string& text);
// Monotone curve through x,y pairs in 0..1, sampled at 256 points.
std::vector<float> curveTable(const std::vector<double>& points);

}  // namespace wetwaresuite::cut
