#include "achclwss/audio/buffer.hpp"

#include <algorithm>
#include <cstdio>

namespace achclwss::audio {

Buffer::Buffer(int channelCount, std::size_t frameCount, int sampleRate) : rate(sampleRate) {
    channels.assign(static_cast<std::size_t>(std::max(channelCount, 0)), std::vector<float>(frameCount, 0.0f));
}

void Buffer::resize(std::size_t frameCount) {
    for (auto& c : channels) c.resize(frameCount, 0.0f);
}

std::size_t Buffer::toFrame(double s) const {
    if (!(s > 0)) return 0;
    const double f = std::round(s * rate);
    if (f >= static_cast<double>(frames())) return frames();
    return static_cast<std::size_t>(f);
}

Range rangeOf(const Buffer& b, double start, double end) {
    Range r{b.toFrame(start), end < 0 ? b.frames() : b.toFrame(end)};
    if (r.end < r.begin) r.end = r.begin;
    return r;
}

bool validChannels(const Buffer& b, const std::vector<int>& selection) {
    for (int c : selection) {
        if (c < 0 || c >= b.channelCount()) return false;
    }
    return true;
}

std::vector<int> resolveChannels(const Buffer& b, const std::vector<int>& selection) {
    std::vector<int> out;
    if (selection.empty()) {
        for (int c = 0; c < b.channelCount(); ++c) out.push_back(c);
        return out;
    }
    for (int c : selection) {
        if (c >= 0 && c < b.channelCount() && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    }
    return out;
}

Buffer slice(const Buffer& b, Range r) {
    Buffer out;
    out.rate = b.rate;
    r.end = std::min(r.end, b.frames());
    r.begin = std::min(r.begin, r.end);
    for (const auto& c : b.channels) {
        out.channels.emplace_back(c.begin() + static_cast<std::ptrdiff_t>(r.begin), c.begin() + static_cast<std::ptrdiff_t>(r.end));
    }
    return out;
}

std::string formatTime(double s) {
    char buf[48];
    if (s < 1.0) {
        std::snprintf(buf, sizeof buf, "%.3f s", s);
        return buf;
    }
    const auto total = static_cast<long long>(std::floor(s));
    const int ms = static_cast<int>(std::lround((s - static_cast<double>(total)) * 1000.0)) % 1000;
    const long long h = total / 3600, m = (total / 60) % 60, sec = total % 60;
    if (h > 0) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld.%03d", h, m, sec, ms);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld.%03d", m, sec, ms);
    return buf;
}

std::string describe(const Buffer& b) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%d ch, %d Hz, %s", b.channelCount(), b.rate, formatTime(b.seconds()).c_str());
    return buf;
}

}  // namespace achclwss::audio
