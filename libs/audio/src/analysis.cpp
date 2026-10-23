// Levels, BS.1770 loudness, and waveform / spectrogram pictures.
#include "wetwaresuite/audio/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "draw.hpp"
#include "internal.hpp"

namespace wetwaresuite::audio {

using detail::kPi;
using raster::Color;
using raster::Image;
using namespace detail;

namespace {

Range clampRange(const Buffer& b, Range r) {
    r.end = std::min(r.end, b.frames());
    r.begin = std::min(r.begin, r.end);
    return r;
}

struct Iir {
    double b0, b1, b2, a1, a2;
    double z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// K-weighting (BS.1770-4) for any sample rate, as libebur128 derives it.
std::pair<Iir, Iir> kWeighting(double fs) {
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = std::tan(kPi * f0 / fs);
    const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    Iir shelf{(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0, 2.0 * (K * K - 1.0) / a0,
              (1.0 - K / Q + K * K) / a0};
    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = std::tan(kPi * f0 / fs);
    a0 = 1.0 + K / Q + K * K;
    Iir high{1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    return {shelf, high};
}

double channelWeight(int index, int count) {
    if (count == 6) return index == 3 ? 0.0 : index >= 4 ? 1.41 : 1.0;
    if (count == 5) return index >= 3 ? 1.41 : 1.0;
    return 1.0;
}

double toLufs(double z) { return z > 0 ? -0.691 + 10.0 * std::log10(z) : -70.0; }

struct LoudnessBlocks {
    std::vector<double> sub;  // weighted mean square per 100 ms
    std::size_t subFrames = 0;
    std::size_t tailFrames = 0;
};

LoudnessBlocks subBlocks(const Buffer& b, Range r) {
    LoudnessBlocks out;
    out.subFrames = std::max<std::size_t>(1, static_cast<std::size_t>(b.rate / 10));
    const std::size_t n = r.size();
    const std::size_t count = (n + out.subFrames - 1) / out.subFrames;
    out.sub.assign(count, 0.0);
    for (int c = 0; c < b.channelCount(); ++c) {
        const double wgt = channelWeight(c, b.channelCount());
        if (wgt == 0) continue;
        auto [shelf, high] = kWeighting(b.rate);
        const auto& d = b.channels[static_cast<std::size_t>(c)];
        for (std::size_t i = 0; i < n; ++i) {
            const double y = high.run(shelf.run(d[r.begin + i]));
            out.sub[i / out.subFrames] += wgt * y * y;
        }
    }
    for (std::size_t s = 0; s < count; ++s) {
        const std::size_t len = std::min(out.subFrames, n - s * out.subFrames);
        out.sub[s] /= static_cast<double>(out.subFrames);
        if (s + 1 == count) out.tailFrames = len;
    }
    return out;
}

// Mean of `width` consecutive sub-blocks stepping by one (sliding windows).
std::vector<double> windows(const std::vector<double>& sub, std::size_t width) {
    std::vector<double> out;
    if (sub.empty()) return out;
    if (sub.size() < width) {
        double s = 0;
        for (double v : sub) s += v;
        out.push_back(s / static_cast<double>(sub.size()));  // shorter than one window: measure what there is
        return out;
    }
    double s = 0;
    for (std::size_t i = 0; i < sub.size(); ++i) {
        s += sub[i];
        if (i >= width) s -= sub[i - width];
        if (i + 1 >= width) out.push_back(s / static_cast<double>(width));
    }
    return out;
}

double gatedLoudness(const std::vector<double>& blocks, double relativeLu) {
    double sum = 0;
    std::size_t n = 0;
    for (double z : blocks) {
        if (toLufs(z) > -70.0) sum += z, ++n;
    }
    if (n == 0) return -70.0;
    const double rel = toLufs(sum / static_cast<double>(n)) + relativeLu;
    sum = 0;
    n = 0;
    for (double z : blocks) {
        const double l = toLufs(z);
        if (l > -70.0 && l > rel) sum += z, ++n;
    }
    return n ? toLufs(sum / static_cast<double>(n)) : -70.0;
}

// 4x oversampled peak (BS.1770 annex 2 style). Only stretches near the sample peak are
// interpolated: an inter-sample peak can't be far above the samples around it.
double truePeakOf(const Buffer& b, Range r, double samplePeak) {
    constexpr int kHalf = 8, kTaps = 2 * kHalf;
    float h[3][kTaps];
    for (int p = 1; p <= 3; ++p) {
        for (int j = 0; j < kTaps; ++j) {
            const double d = p / 4.0 + (kHalf - 1) - j;
            const double x = kPi * d, sinc = std::fabs(d) < 1e-9 ? 1.0 : std::sin(x) / x;
            const double u = d / kHalf, win = std::fabs(u) < 1 ? 0.42 + 0.5 * std::cos(kPi * u) + 0.08 * std::cos(2 * kPi * u) : 0.0;
            h[p - 1][j] = static_cast<float>(sinc * win);
        }
    }
    const auto threshold = static_cast<float>(samplePeak * 0.5);
    double tp = samplePeak;
    for (const auto& c : b.channels) {
        for (std::size_t i = r.begin; i + 1 < r.end; ++i) {
            if (std::max(std::fabs(c[i]), std::fabs(c[i + 1])) < threshold) continue;
            for (int p = 0; p < 3; ++p) {
                float acc = 0;
                for (int j = 0; j < kTaps; ++j) {
                    const auto idx = static_cast<std::ptrdiff_t>(i) - (kHalf - 1) + j;
                    if (idx >= static_cast<std::ptrdiff_t>(r.begin) && idx < static_cast<std::ptrdiff_t>(r.end)) acc += c[static_cast<std::size_t>(idx)] * h[p][j];
                }
                tp = std::max(tp, static_cast<double>(std::fabs(acc)));
            }
        }
    }
    return tp;
}

}  // namespace

double integratedLoudness(const Buffer& b, Range r) {
    r = clampRange(b, r);
    if (r.size() == 0) return -70.0;
    const auto blocks = subBlocks(b, r);
    return std::max(-70.0, gatedLoudness(windows(blocks.sub, 4), -10.0));
}

std::vector<double> loudnessCurve(const Buffer& b, Range r, double windowSeconds, double stepSeconds) {
    r = clampRange(b, r);
    std::vector<double> out;
    if (r.size() == 0) return out;
    const auto blocks = subBlocks(b, r);
    const auto width = static_cast<std::size_t>(std::max(1.0, std::round(windowSeconds * 10)));
    const auto step = static_cast<std::size_t>(std::max(1.0, std::round(stepSeconds * 10)));
    // value k covers the window ending at (k+1) steps, or as much as there is so far
    double sum = 0;
    for (std::size_t i = 0; i < blocks.sub.size(); ++i) {
        sum += blocks.sub[i];
        if (i >= width) sum -= blocks.sub[i - width];
        if ((i + 1) % step == 0 || i + 1 == blocks.sub.size()) {
            const std::size_t have = std::min(width, i + 1);
            out.push_back(std::max(-70.0, toLufs(std::max(0.0, sum) / static_cast<double>(have))));
        }
    }
    return out;
}

Levels measure(const Buffer& b, Range r, bool truePeak) {
    r = clampRange(b, r);
    Levels L;
    L.seconds = static_cast<double>(r.size()) / std::max(1, b.rate);
    if (r.size() == 0) return L;
    double peak = 0, sumSq = 0;
    for (const auto& c : b.channels) {
        double cp = 0, cs = 0, dc = 0;
        for (std::size_t i = r.begin; i < r.end; ++i) {
            const double v = c[i];
            cp = std::max(cp, std::fabs(v));
            cs += v * v;
            dc += v;
            if (std::fabs(v) >= 0.9999) ++L.clipped;
        }
        const double n = static_cast<double>(r.size());
        L.channelPeakDb.push_back(gainToDb(cp));
        L.channelRmsDb.push_back(gainToDb(std::sqrt(cs / n)));
        if (std::fabs(dc / n) > std::fabs(L.dcOffset)) L.dcOffset = dc / n;
        peak = std::max(peak, cp);
        sumSq += cs;
    }
    L.peakDb = gainToDb(peak);
    L.rmsDb = gainToDb(std::sqrt(sumSq / (static_cast<double>(r.size()) * std::max(1, b.channelCount()))));
    L.truePeakDb = L.peakDb;
    if (truePeak) L.truePeakDb = gainToDb(truePeakOf(b, r, peak));
    const auto blocks = subBlocks(b, r);
    const auto momentary = windows(blocks.sub, 4);
    const auto shortTerm = windows(blocks.sub, 30);
    L.lufs = std::max(-70.0, gatedLoudness(momentary, -10.0));
    for (double z : momentary) L.momentaryMaxLufs = std::max(L.momentaryMaxLufs, toLufs(z));
    for (double z : shortTerm) L.shortTermMaxLufs = std::max(L.shortTermMaxLufs, toLufs(z));
    // loudness range: 10th to 95th percentile of gated short-term loudness
    std::vector<double> st;
    double sum = 0;
    for (double z : shortTerm) {
        if (toLufs(z) > -70) st.push_back(z), sum += z;
    }
    if (st.size() >= 2) {
        const double rel = toLufs(sum / static_cast<double>(st.size())) - 20.0;
        std::vector<double> kept;
        for (double z : st) {
            if (toLufs(z) > rel) kept.push_back(toLufs(z));
        }
        std::sort(kept.begin(), kept.end());
        if (kept.size() >= 2) {
            auto pct = [&](double p) { return kept[static_cast<std::size_t>(std::round(p * static_cast<double>(kept.size() - 1)))]; };
            L.loudnessRange = pct(0.95) - pct(0.10);
        }
    }
    return L;
}

// ---- pictures ----

namespace {
Color magma(double t) {
    static const Color stops[] = {{0, 0, 4, 255},       {40, 11, 84, 255},  {101, 21, 110, 255}, {159, 42, 99, 255},
                                  {212, 72, 66, 255},   {245, 125, 21, 255}, {250, 193, 39, 255}, {252, 255, 164, 255}};
    t = std::clamp(t, 0.0, 1.0) * 7.0;
    const int i = std::min(6, static_cast<int>(t));
    const double f = t - i;
    auto lerp = [&](std::uint8_t a, std::uint8_t b) { return static_cast<std::uint8_t>(a + (b - a) * f); };
    return {lerp(stops[i].r, stops[i + 1].r), lerp(stops[i].g, stops[i + 1].g), lerp(stops[i].b, stops[i + 1].b), 255};
}

}  // namespace

Image waveform(const Buffer& b, Range r, const WaveformStyle& st) {
    r = clampRange(b, r);
    const int W = std::clamp(st.width, 16, 8192), H = std::clamp(st.height, 16, 8192);
    Image img(W, H, st.background);
    const int lanes = std::max(1, b.channelCount());
    const double spp = static_cast<double>(r.size()) / W;  // samples per pixel column
    for (int lane = 0; lane < lanes; ++lane) {
        const int top = lane * H / lanes, bottom = (lane + 1) * H / lanes - 1;
        const int laneH = bottom - top + 1;
        const double mid = top + (laneH - 1) / 2.0, half = (laneH - 4) / 2.0;
        auto yOf = [&](double v) {
            if (st.decibels) {
                const double db = gainToDb(std::fabs(v));
                const double m = std::clamp(1.0 + db / 60.0, 0.0, 1.0);
                v = v < 0 ? -m : m;
            }
            return static_cast<int>(std::lround(mid - std::clamp(v, -1.0, 1.0) * half));
        };
        if (st.showGrid) {
            hline(img, 0, W - 1, static_cast<int>(mid), st.grid);
            const double six = st.decibels ? 0.9 : 0.5;
            hline(img, 0, W - 1, yOf(six), st.grid, 0.7f, 4);
            hline(img, 0, W - 1, yOf(-six), st.grid, 0.7f, 4);
            if (lane > 0) hline(img, 0, W - 1, top, {110, 115, 125, 255});
        }
        if (lane >= b.channelCount() || r.size() == 0) continue;
        const auto& d = b.channels[static_cast<std::size_t>(lane)];
        int prevY = -1;
        for (int x = 0; x < W; ++x) {
            auto i0 = r.begin + static_cast<std::size_t>(std::floor(x * spp));
            auto i1 = r.begin + static_cast<std::size_t>(std::floor((x + 1) * spp));
            i1 = std::max(i1, i0 + 1);
            i0 = std::min(i0, r.end - 1);
            i1 = std::min(i1, r.end);
            float lo = d[i0], hi = d[i0];
            double sq = 0;
            bool clipped = false;
            for (std::size_t i = i0; i < i1; ++i) {
                lo = std::min(lo, d[i]);
                hi = std::max(hi, d[i]);
                sq += static_cast<double>(d[i]) * d[i];
                clipped = clipped || std::fabs(d[i]) >= 0.9999f;
            }
            int y0 = yOf(hi), y1 = yOf(lo);
            if (spp < 1.5 && prevY >= 0) {  // zoomed in: join the dots
                y0 = std::min(y0, prevY);
                y1 = std::max(y1, prevY);
            }
            prevY = yOf((lo + hi) / 2);
            vline(img, x, y0, y1, st.wave);
            const double rms = std::sqrt(sq / static_cast<double>(i1 - i0));
            if (spp >= 4) vline(img, x, yOf(rms), yOf(-rms), st.rms, 0.8f);
            if (clipped && st.showClipping) {
                vline(img, x, top, top + 3, st.clip);
                vline(img, x, bottom - 3, bottom, st.clip);
            }
        }
    }
    if (st.showGrid) timeGrid(img, static_cast<double>(r.begin) / b.rate, static_cast<double>(r.size()) / b.rate, st.grid, true);
    return img;
}

Image spectrogram(const Buffer& b, Range r, const SpectrogramStyle& st) {
    r = clampRange(b, r);
    const int W = std::clamp(st.width, 16, 8192), H = std::clamp(st.height, 16, 8192);
    Image img(W, H, {0, 0, 4, 255});
    if (r.size() == 0 || b.channelCount() == 0) return img;
    int n = 256;
    while (n < st.fftSize && n < 32768) n <<= 1;
    // signal: one channel or the mix
    std::vector<float> x(r.size(), 0.0f);
    if (st.channel >= 0 && st.channel < b.channelCount()) {
        const auto& d = b.channels[static_cast<std::size_t>(st.channel)];
        std::copy(d.begin() + static_cast<std::ptrdiff_t>(r.begin), d.begin() + static_cast<std::ptrdiff_t>(r.end), x.begin());
    } else {
        for (const auto& d : b.channels) {
            for (std::size_t i = 0; i < x.size(); ++i) x[i] += d[r.begin + i] / static_cast<float>(b.channelCount());
        }
    }
    const double nyquist = b.rate / 2.0;
    const double maxF = st.maxFrequency > 0 ? std::min(st.maxFrequency, nyquist) : nyquist;
    const double minF = st.logFrequency ? std::clamp(st.minFrequency, 1.0, maxF / 2) : 0.0;
    const double binHz = static_cast<double>(b.rate) / n;
    auto freqAt = [&](double row) {  // row 0 = top edge
        const double t = 1.0 - row / H;
        return st.logFrequency ? minF * std::pow(maxF / minF, t) : minF + (maxF - minF) * t;
    };
    const auto win = detail::hann(n);
    double winSum = 0;
    for (float v : win) winSum += v;
    std::vector<std::complex<float>> buf(static_cast<std::size_t>(n));
    std::vector<double> mag(static_cast<std::size_t>(n / 2 + 1));
    const double floorDb = std::min(-10.0, st.floorDb);
    for (int col = 0; col < W; ++col) {
        const double centre = (col + 0.5) * static_cast<double>(x.size()) / W;
        const auto start = static_cast<std::ptrdiff_t>(centre) - n / 2;
        for (int i = 0; i < n; ++i) {
            const std::ptrdiff_t idx = start + i;
            const float v = idx >= 0 && idx < static_cast<std::ptrdiff_t>(x.size()) ? x[static_cast<std::size_t>(idx)] : 0.0f;
            buf[static_cast<std::size_t>(i)] = {v * win[static_cast<std::size_t>(i)], 0.0f};
        }
        detail::fft(buf, false);
        for (std::size_t k = 0; k < mag.size(); ++k) mag[k] = std::abs(buf[k]) * 2.0 / winSum;
        for (int row = 0; row < H; ++row) {
            const double fHi = freqAt(row), fLo = freqAt(row + 1);
            const double kLo = fLo / binHz, kHi = fHi / binHz;
            double m = 0;
            if (kHi - kLo < 1.0) {  // interpolate between bins
                const double k = (kLo + kHi) / 2;
                const auto k0 = static_cast<std::size_t>(std::floor(k));
                const double f = k - static_cast<double>(k0);
                const std::size_t k1 = std::min(mag.size() - 1, k0 + 1);
                m = mag[std::min(k0, mag.size() - 1)] * (1 - f) + mag[k1] * f;
            } else {
                for (auto k = static_cast<std::size_t>(std::ceil(kLo)); k <= static_cast<std::size_t>(kHi) && k < mag.size(); ++k) m = std::max(m, mag[k]);
            }
            const double db = gainToDb(m);
            img.set(col, row, magma((db - floorDb) / -floorDb));
        }
    }
    if (st.showGrid) {
        const Color grid{255, 255, 255, 255};
        std::vector<double> lines;
        if (st.logFrequency) {
            for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) lines.push_back(f);
        } else {
            const double step = maxF > 12000 ? 2000 : maxF > 5000 ? 1000 : 500;
            for (double f = step; f < maxF; f += step) lines.push_back(f);
        }
        for (double f : lines) {
            if (f <= minF || f >= maxF) continue;
            const double t = st.logFrequency ? std::log(f / minF) / std::log(maxF / minF) : (f - minF) / (maxF - minF);
            const int y = static_cast<int>(std::lround((1 - t) * H));
            const bool major = f == 100 || f == 1000 || f == 10000;
            hline(img, 0, W - 1, y, grid, major ? 0.35f : 0.18f, major ? 0 : 3);
            char label[16];
            if (f >= 1000) std::snprintf(label, sizeof label, "%gk", f / 1000);
            else std::snprintf(label, sizeof label, "%g", f);
            if (y > 12 && y < H - 24) text(img, 3, y - 12, label, {230, 230, 235, 255});
        }
        timeGrid(img, static_cast<double>(r.begin) / b.rate, static_cast<double>(r.size()) / b.rate, {255, 255, 255, 60}, true);
    }
    return img;
}

}  // namespace wetwaresuite::audio
