// Shared bits of the cut tests: running commands, reading results, making
// media files without ffmpeg (PNGs through libs/raster, WAVs through libs/audio)
// and looking at rendered pixels.
#pragma once

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "check.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/cut/cut.hpp"
#include "wetwaresuite/cut/render.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace cuttest {

using namespace wetwaresuite;
using namespace wetwaresuite::cut;
namespace fs = std::filesystem;

inline bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
inline bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

inline double num(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return std::nan("");
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return std::nan("");
}

inline std::string text(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

inline Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

// A fresh folder for a test's files.
inline fs::path tempDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("wetwaresuite_cut_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

inline std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

// A PNG filled by fn(x, y) -> colour.
template <typename Fn>
std::string writePng(const fs::path& path, int w, int h, Fn fn) {
    raster::Image img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            raster::Color c = fn(x, y);
            std::uint8_t* p = img.pixel(x, y);
            p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = c.a;
        }
    }
    raster::save(img, path);
    return path.generic_string();
}

inline std::string solidPng(const fs::path& path, raster::Color c, int w = 32, int h = 18) {
    return writePng(path, w, h, [c](int, int) { return c; });
}

// A stereo sine (or silence) as a WAV.
inline std::string writeTone(const fs::path& path, double seconds, double freq, double amp, int rate = 48000) {
    audio::Buffer b = audio::tone(2, seconds, rate, freq, amp);
    audio::save(b, path);
    return path.generic_string();
}

// RGBA of one pixel.
struct Px {
    int r, g, b, a;
};
inline Px px(const raster::Image& img, int x, int y) {
    const std::uint8_t* p = img.pixel(x, y);
    return {p[0], p[1], p[2], p[3]};
}
inline bool close(Px p, int r, int g, int b, int tol = 12) { return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol; }

inline raster::Image frame(const Timeline& tl, double t, int width = 0) {
    FrameRequest rq;
    rq.t = t;
    rq.width = width;
    return renderFrame(tl, rq);
}

// Pixels brighter than `level` in every channel.
inline int brightCount(const raster::Image& img, int level = 200) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            Px p = px(img, x, y);
            if (p.r > level && p.g > level && p.b > level) ++n;
        }
    }
    return n;
}

inline double rms(const audio::Buffer& b, std::size_t from, std::size_t to, int ch = 0) {
    double s = 0;
    to = std::min(to, b.frames());
    if (to <= from) return 0;
    for (std::size_t i = from; i < to; ++i) s += b.channels[ch][i] * b.channels[ch][i];
    return std::sqrt(s / static_cast<double>(to - from));
}

std::string readTextFile(const fs::path& p);

inline std::string trackOfClip(const Timeline& tl, const std::string& id) {
    for (const auto& t : tl.tracks) {
        for (const auto& c : t.clips) {
            if (c.id == id) return t.id;
        }
    }
    return {};
}

void testModel();
void testPlan();
void testCommands();
void testRender();
void testMix();
void testEdits();
void testSequence();
void testWithFfmpeg();
void testMcp();

}  // namespace cuttest
