// Float32 multichannel audio: the sample engine shared by the audio editor,
// the video editor and anything else that touches sound. Channels are stored
// planar (one vector per channel), samples are nominally -1..1 and may go past
// that between steps; only saving to integer formats clips.
#pragma once

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace wetwaresuite::audio {

struct Buffer {
    int rate = 44100;
    std::vector<std::vector<float>> channels;

    Buffer() = default;
    Buffer(int channelCount, std::size_t frameCount, int sampleRate);

    int channelCount() const { return static_cast<int>(channels.size()); }
    std::size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }
    double seconds() const { return rate > 0 ? static_cast<double>(frames()) / rate : 0.0; }
    bool empty() const { return frames() == 0; }

    void resize(std::size_t frameCount);  // pads with silence
    std::size_t toFrame(double seconds) const;  // clamped to 0..frames()
};

// A span of frames [begin, end).
struct Range {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t size() const { return end > begin ? end - begin : 0; }
};

// Seconds to a frame range clamped to the buffer; end < 0 means "to the end".
Range rangeOf(const Buffer& b, double startSeconds = 0.0, double endSeconds = -1.0);
inline Range all(const Buffer& b) { return {0, b.frames()}; }

// Channel selection: empty = every channel. Returns false when an index is out of range.
bool validChannels(const Buffer& b, const std::vector<int>& selection);
std::vector<int> resolveChannels(const Buffer& b, const std::vector<int>& selection);

// Copies frames [r.begin, r.end) of every channel.
Buffer slice(const Buffer& b, Range r);

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double gain) { return gain > 1e-12 ? 20.0 * std::log10(gain) : -240.0; }

// "2 ch, 44100 Hz, 12.5 s".
std::string describe(const Buffer& b);
// "1:02.350" (or "0.350 s" under a second).
std::string formatTime(double seconds);

}  // namespace wetwaresuite::audio
