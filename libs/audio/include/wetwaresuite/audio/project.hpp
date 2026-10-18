// A multitrack project: tracks holding clips at time offsets, per-track gain,
// pan, mute, solo, a volume envelope and automatic ducking under another
// track, mixed down to one Buffer. Clips share their audio (copy a project
// freely, e.g. for undo snapshots).
#pragma once

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/audio/buffer.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::audio {

struct Clip {
    std::string name;
    std::shared_ptr<const Buffer> audio;  // already at the project's rate
    double at = 0;        // seconds from the project start
    double gainDb = 0;
    double fadeIn = 0;    // seconds
    double fadeOut = 0;
    // the part of `audio` that plays (trimming is non-destructive): from offset, for length seconds
    double offset = 0;
    double length = -1;  // < 0 = to the end of the audio
    double sourceSeconds() const { return audio ? audio->seconds() : 0.0; }
    double seconds() const {
        const double rest = std::max(0.0, sourceSeconds() - offset);
        return length < 0 ? rest : std::min(length, rest);
    }
    double end() const { return at + seconds(); }
};

struct EnvelopePoint {
    double time = 0;    // seconds
    double gainDb = 0;  // linear-in-dB between points, held before the first and after the last
};

struct Track {
    std::string name;
    double gainDb = 0;
    double pan = 0;  // -1..1
    bool mute = false;
    bool solo = false;
    std::vector<Clip> clips;
    std::vector<EnvelopePoint> envelope;
    // Ducking: this track drops by duckDb whenever track `duckUnder` is louder than duckThresholdDb.
    int duckUnder = -1;
    double duckDb = -12;
    double duckThresholdDb = -40;
    double duckAttack = 0.15;   // seconds to go down
    double duckRelease = 0.6;   // seconds to come back
    double end() const;
};

struct Project {
    int rate = 44100;
    int channels = 2;
    std::vector<Track> tracks;
    double masterGainDb = 0;

    double seconds() const;
};

// One track rendered (clips, fades, gain, envelope, ducking, pan) at the project's channels.
Buffer renderTrack(const Project& p, int track);
// Every audible track summed (solo wins over mute), times the master gain.
Buffer mixdown(const Project& p);

// Lanes per track with each clip's waveform, muted tracks greyed out.
raster::Image renderProject(const Project& p, int width = 1200, int laneHeight = 90);

// Saves as JSON with every clip written as a 32-bit float WAV in `<stem>_clips/` next to it.
void saveProject(const Project& p, const std::filesystem::path& path);
Project loadProject(const std::filesystem::path& path);  // throws std::runtime_error

}  // namespace wetwaresuite::audio
