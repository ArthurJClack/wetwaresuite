// Compressor, limiter and noise gate. They run offline over the whole range,
// so the limiter and gate can look ahead without adding delay.
#include <algorithm>
#include <cmath>
#include <deque>

#include "achclwss/audio/dsp.hpp"
#include "internal.hpp"

namespace achclwss::audio {

namespace {

Range clampRange(const Buffer& b, Range r) {
    r.end = std::min(r.end, b.frames());
    r.begin = std::min(r.begin, r.end);
    return r;
}

// Loudest selected channel at each frame of the range.
std::vector<float> linkedLevel(const Buffer& b, Range r, const std::vector<int>& chans) {
    std::vector<float> level(r.size(), 0.0f);
    for (int c : chans) {
        const auto& d = b.channels[static_cast<std::size_t>(c)];
        for (std::size_t i = 0; i < r.size(); ++i) level[i] = std::max(level[i], std::fabs(d[r.begin + i]));
    }
    return level;
}

void applyGain(Buffer& b, Range r, const Channels& ch, const std::vector<float>& g) {
    detail::processRegion(
        b, r, ch,
        [&g](std::vector<float>& x, int) {
            for (std::size_t i = 0; i < x.size(); ++i) x[i] *= g[i];
        },
        detail::edgeFrames(b));
}

double coefficient(double ms, int rate) { return ms <= 0 ? 0.0 : std::exp(-1.0 / (ms * 0.001 * rate)); }

}  // namespace

double compress(Buffer& b, const CompressorSettings& s, Range r, const Channels& ch) {
    r = clampRange(b, r);
    if (r.size() == 0) return 0;
    const auto chans = resolveChannels(b, ch);
    const auto level = linkedLevel(b, r, chans);
    const double T = s.thresholdDb, R = std::max(1.0, s.ratio), W = std::max(0.0, s.kneeDb);
    double makeup = s.makeupDb;
    if (s.autoMakeup) makeup += -T * (1 - 1 / R) / 2;
    const double att = coefficient(s.attackMs, b.rate), rel = coefficient(s.releaseMs, b.rate);
    std::vector<float> g(r.size());
    double smooth = 0, maxReduction = 0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const double x = gainToDb(level[i]);
        double y = x;
        if (2 * (x - T) > W) y = T + (x - T) / R;
        else if (W > 0 && 2 * std::fabs(x - T) <= W) y = x + (1 / R - 1) * (x - T + W / 2) * (x - T + W / 2) / (2 * W);
        const double reduction = x - y;
        const double c = reduction > smooth ? att : rel;
        smooth = c * smooth + (1 - c) * reduction;
        maxReduction = std::max(maxReduction, smooth);
        g[i] = static_cast<float>(dbToGain(makeup - smooth));
    }
    applyGain(b, r, ch, g);
    return maxReduction;
}

double limit(Buffer& b, double ceilingDb, double releaseMs, Range r, const Channels& ch) {
    r = clampRange(b, r);
    if (r.size() == 0) return 0;
    const auto chans = resolveChannels(b, ch);
    const auto level = linkedLevel(b, r, chans);
    const auto ceiling = static_cast<float>(dbToGain(ceilingDb));
    const std::size_t n = r.size();
    std::vector<float> need(n);
    for (std::size_t i = 0; i < n; ++i) need[i] = level[i] > ceiling ? ceiling / level[i] : 1.0f;
    // minimum over the next `look` frames (sliding window with a monotonic deque)
    const std::size_t look = std::max<std::size_t>(1, static_cast<std::size_t>(b.rate * 0.005));
    std::vector<float> ahead(n);
    std::deque<std::size_t> q;
    for (std::size_t j = n; j-- > 0;) {
        while (!q.empty() && need[q.back()] >= need[j]) q.pop_back();
        q.push_back(j);
        while (q.front() > j + look) q.pop_front();
        ahead[j] = need[q.front()];
    }
    // average over the previous `look` frames: a smooth attack that still never overshoots
    std::vector<float> g(n);
    double sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += ahead[i];
        if (i >= look + 1) sum -= ahead[i - look - 1];
        const double count = static_cast<double>(std::min(i + 1, look + 1));
        g[i] = static_cast<float>(sum / count);
        // the first frames average fewer values; never above what's needed
        g[i] = std::min(g[i], ahead[i]);
    }
    const double rel = coefficient(releaseMs, b.rate);
    double cur = 1, worst = 1;
    for (std::size_t i = 0; i < n; ++i) {
        if (g[i] < cur) cur = g[i];
        else cur = rel * cur + (1 - rel) * g[i];
        g[i] = static_cast<float>(cur);
        worst = std::min(worst, cur);
    }
    applyGain(b, r, ch, g);
    for (int c : chans) {  // catch float rounding
        auto& d = b.channels[static_cast<std::size_t>(c)];
        for (std::size_t i = r.begin; i < r.end; ++i) d[i] = std::clamp(d[i], -ceiling, ceiling);
    }
    return -gainToDb(worst);
}

double gate(Buffer& b, const GateSettings& s, Range r, const Channels& ch) {
    r = clampRange(b, r);
    if (r.size() == 0) return 0;
    const auto chans = resolveChannels(b, ch);
    const auto level = linkedLevel(b, r, chans);
    const std::size_t n = r.size();
    const double openAt = dbToGain(s.thresholdDb), closeAt = dbToGain(s.thresholdDb - 4);
    const double decay = coefficient(10, b.rate);
    const auto hold = static_cast<std::size_t>(s.holdMs * 0.001 * b.rate);
    std::vector<char> open(n, 0);
    double env = 0;
    bool isOpen = false;
    std::size_t holdLeft = 0;
    for (std::size_t i = 0; i < n; ++i) {
        env = std::max(static_cast<double>(level[i]), env * decay);
        if (env >= openAt) {
            isOpen = true;
            holdLeft = hold;
        } else if (isOpen && env < closeAt) {
            if (holdLeft > 0) --holdLeft;
            else isOpen = false;
        }
        open[i] = isOpen ? 1 : 0;
    }
    // look ahead: open a little before the sound starts so attacks aren't chopped
    const auto lead = static_cast<std::size_t>(std::max(1.0, s.attackMs) * 0.001 * b.rate);
    std::vector<char> early = open;
    for (std::size_t i = 0; i < n; ++i) {
        if (open[i] && (i == 0 || !open[i - 1])) {
            for (std::size_t k = i > lead ? i - lead : 0; k < i; ++k) early[k] = 1;
        }
    }
    const double closed = dbToGain(std::min(0.0, s.rangeDb));
    const double att = coefficient(s.attackMs, b.rate), rel = coefficient(s.releaseMs, b.rate);
    std::vector<float> g(n);
    double cur = early[0] ? 1.0 : closed;
    std::size_t closedCount = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double target = early[i] ? 1.0 : closed;
        const double c = target > cur ? att : rel;
        cur = c * cur + (1 - c) * target;
        g[i] = static_cast<float>(cur);
        if (!early[i]) ++closedCount;
    }
    applyGain(b, r, ch, g);
    return static_cast<double>(closedCount) / static_cast<double>(n);
}

}  // namespace achclwss::audio
