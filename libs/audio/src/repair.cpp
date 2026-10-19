// Click removal (linear prediction), hum notches and spectral range gain.
#include "wetwaresuite/audio/repair.hpp"

#include <algorithm>
#include <cmath>

#include "wetwaresuite/audio/dsp.hpp"
#include "internal.hpp"

namespace wetwaresuite::audio {

using detail::kPi;

namespace {

constexpr int kOrder = 20;
constexpr std::size_t kBlock = 2048;

// Levinson-Durbin on the autocorrelation of a Hann-windowed block. a[k] predicts
// x[n] from x[n-1-k]. Returns false for silent blocks.
bool lpc(const float* x, std::size_t n, std::vector<double>& a) {
    std::vector<double> w(n);
    for (std::size_t i = 0; i < n; ++i) w[i] = x[i] * (0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n)));
    std::vector<double> r(kOrder + 1, 0.0);
    for (int k = 0; k <= kOrder; ++k)
        for (std::size_t i = static_cast<std::size_t>(k); i < n; ++i) r[static_cast<std::size_t>(k)] += w[i] * w[i - static_cast<std::size_t>(k)];
    if (r[0] < 1e-12) return false;
    r[0] *= 1.0 + 1e-5;  // white-noise correction keeps the predictor stable
    std::vector<double> c(kOrder + 1, 0.0), prev;
    double err = r[0];
    c[0] = 1;
    for (int i = 1; i <= kOrder; ++i) {
        double acc = r[static_cast<std::size_t>(i)];
        for (int j = 1; j < i; ++j) acc += c[static_cast<std::size_t>(j)] * r[static_cast<std::size_t>(i - j)];
        const double k = -acc / err;
        prev = c;
        for (int j = 1; j < i; ++j) c[static_cast<std::size_t>(j)] = prev[static_cast<std::size_t>(j)] + k * prev[static_cast<std::size_t>(i - j)];
        c[static_cast<std::size_t>(i)] = k;
        err *= 1 - k * k;
        if (err <= 0) return false;
    }
    a.assign(kOrder, 0.0);
    for (int k = 0; k < kOrder; ++k) a[static_cast<std::size_t>(k)] = -c[static_cast<std::size_t>(k + 1)];
    return true;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

// Rebuilds x[s, e) from kOrder clean samples each side.
void repairRun(std::vector<float>& x, std::size_t s, std::size_t e, const std::vector<double>& a) {
    const std::size_t len = e - s;
    std::vector<double> fwd(len), bwd(len);
    // forward: predict from the left, feeding predictions back in
    std::vector<double> hist(x.begin() + static_cast<std::ptrdiff_t>(s - kOrder), x.begin() + static_cast<std::ptrdiff_t>(s));
    for (std::size_t i = 0; i < len; ++i) {
        double p = 0;
        for (int k = 0; k < kOrder; ++k) p += a[static_cast<std::size_t>(k)] * hist[hist.size() - 1 - static_cast<std::size_t>(k)];
        fwd[i] = p;
        hist.push_back(p);
    }
    // backward: the same predictor run on time-reversed audio from the right
    hist.assign(x.rend() - static_cast<std::ptrdiff_t>(e + kOrder), x.rend() - static_cast<std::ptrdiff_t>(e));
    for (std::size_t i = 0; i < len; ++i) {
        double p = 0;
        for (int k = 0; k < kOrder; ++k) p += a[static_cast<std::size_t>(k)] * hist[hist.size() - 1 - static_cast<std::size_t>(k)];
        bwd[len - 1 - i] = p;
        hist.push_back(p);
    }
    for (std::size_t i = 0; i < len; ++i) {
        const double w = static_cast<double>(i + 1) / static_cast<double>(len + 1);
        x[s + i] = static_cast<float>((1 - w) * fwd[i] + w * bwd[i]);
    }
}

int declickChannel(std::vector<float>& x, int rate, const ClickSettings& st) {
    const std::size_t n = x.size();
    if (n < kBlock / 2) return 0;
    const auto maxLen = static_cast<std::size_t>(std::max(1.0, st.maxClickMs * 0.001 * rate));
    int repaired = 0;
    std::vector<double> a;
    for (std::size_t b0 = 0; b0 < n; b0 += kBlock) {
        const std::size_t b1 = std::min(n, b0 + kBlock);
        // analyse a window around the block so short blocks at the end still get a predictor
        const std::size_t w0 = b1 - b0 < kBlock && n > kBlock ? n - kBlock : b0;
        const std::size_t w1 = std::min(n, w0 + kBlock);
        if (!lpc(x.data() + w0, w1 - w0, a)) continue;
        std::vector<double> res;
        std::vector<double> absRes;
        double energy = 0;
        const std::size_t start = std::max<std::size_t>(b0, kOrder);
        for (std::size_t i = start; i < b1; ++i) {
            double p = 0;
            for (int k = 0; k < kOrder; ++k) p += a[static_cast<std::size_t>(k)] * x[i - 1 - static_cast<std::size_t>(k)];
            const double e = x[i] - p;
            res.push_back(e);
            absRes.push_back(std::fabs(e));
            energy += static_cast<double>(x[i]) * x[i];
        }
        if (res.empty()) continue;
        const double rms = std::sqrt(energy / static_cast<double>(res.size()));
        const double sigma = 1.4826 * median(absRes);
        const double thr = std::max(st.sensitivity * sigma, 0.004 * rms + 1e-4);
        // the residual's local size (about 3 ms either side, leaving out the sample's own
        // neighbourhood): loud passages predict worse than quiet ones, so a click has to
        // stand out from what's around it, not just from the block as a whole
        std::vector<double> prefix(absRes.size() + 1, 0.0);
        for (std::size_t j = 0; j < absRes.size(); ++j) prefix[j + 1] = prefix[j] + absRes[j];
        auto localSigma = [&](std::size_t j) {
            constexpr std::size_t kSpan = 128, kGap = 6;
            const std::size_t lo = j > kSpan ? j - kSpan : 0, hi = std::min(absRes.size(), j + kSpan + 1);
            const std::size_t glo = j > kGap ? j - kGap : 0, ghi = std::min(absRes.size(), j + kGap + 1);
            const double sum = (prefix[hi] - prefix[lo]) - (prefix[ghi] - prefix[glo]);
            const double count = static_cast<double>((hi - lo) - (ghi - glo));
            return count > 0 ? 1.2533 * sum / count : 0.0;
        };
        // runs of samples over the threshold, widened a little and merged when close
        std::vector<std::pair<std::size_t, std::size_t>> runs;
        for (std::size_t j = 0; j < res.size(); ++j) {
            if (std::fabs(res[j]) <= thr || std::fabs(res[j]) <= st.sensitivity * localSigma(j)) continue;
            const std::size_t at = start + j;
            const std::size_t s = at > 2 ? at - 2 : 0, e = std::min(n, at + 3);
            if (!runs.empty() && s <= runs.back().second + 8) runs.back().second = std::max(runs.back().second, e);
            else runs.emplace_back(s, e);
        }
        // a click rings on after its jump (the predictor only sees the jump), so rebuild at
        // least the first 0.75 ms after it
        const auto ring = std::min(maxLen, static_cast<std::size_t>(0.00075 * rate) + 1);
        for (auto [s, e] : runs) {
            if (e - s > maxLen + 8) continue;  // too long for a click
            const std::size_t ringEnd = std::min(n, std::max(e, s + ring));
            if (s < static_cast<std::size_t>(kOrder) || ringEnd + kOrder > n) continue;
            // a click is a jump you can hear: if rebuilding the jump hardly changes it compared
            // with the sound around it, it was only the predictor stumbling, so leave it
            const std::vector<float> before(x.begin() + static_cast<std::ptrdiff_t>(s), x.begin() + static_cast<std::ptrdiff_t>(ringEnd));
            repairRun(x, s, e, a);
            double change = 0, around = 0;
            for (std::size_t i = s; i < e; ++i) change = std::max(change, static_cast<double>(std::fabs(x[i] - before[i - s])));
            std::copy(before.begin(), before.end(), x.begin() + static_cast<std::ptrdiff_t>(s));
            const std::size_t a0 = s > 256 ? s - 256 : 0, a1 = std::min(n, e + 256);
            for (std::size_t i = a0; i < a1; ++i)
                if (i < s || i >= e) around += static_cast<double>(x[i]) * x[i];
            around = std::sqrt(around / static_cast<double>(std::max<std::size_t>(1, (a1 - a0) - (e - s))));
            if (change < std::max(0.3 * around, 2e-3)) continue;
            repairRun(x, s, ringEnd, a);
            ++repaired;
        }
    }
    return repaired;
}

double goertzel(const std::vector<float>& x, double frequency, int rate) {
    const std::size_t n = x.size();
    if (n == 0) return 0;
    const double w = 2 * kPi * frequency / rate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0, wsum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n));
        wsum += win;
        const double s = x[i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2 * std::sqrt(re * re + im * im) / std::max(1e-12, wsum);
}

std::vector<float> monoOf(const Buffer& b, Range r) {
    r.end = std::min(r.end, b.frames());
    std::vector<float> m(r.size(), 0.0f);
    if (b.channels.empty()) return m;
    const float k = 1.0f / static_cast<float>(b.channelCount());
    for (const auto& c : b.channels)
        for (std::size_t i = 0; i < m.size(); ++i) m[i] += c[r.begin + i] * k;
    return m;
}

}  // namespace

int removeClicks(Buffer& b, const ClickSettings& s, Range r, const std::vector<int>& ch) {
    int total = 0;
    detail::processRegion(
        b, r, ch, [&](std::vector<float>& x, int) { total += declickChannel(x, b.rate, s); }, 0);
    return total;
}

double toneLevel(const Buffer& b, double frequency, Range r) { return gainToDb(goertzel(monoOf(b, r), frequency, b.rate)); }

double detectHum(const Buffer& b, Range r) {
    const auto m = monoOf(b, r);
    double fifty = 0, sixty = 0;
    for (int k = 1; k <= 4; ++k) {
        fifty += goertzel(m, 50.0 * k, b.rate);
        sixty += goertzel(m, 60.0 * k, b.rate);
    }
    return sixty > fifty ? 60.0 : 50.0;
}

double removeHum(Buffer& b, const HumSettings& s, Range r, const std::vector<int>& ch) {
    const double f0 = s.frequency > 0 ? s.frequency : detectHum(b, r);
    const int harmonics = std::clamp(s.harmonics, 1, 40);
    std::vector<Biquad> notches;
    for (int k = 1; k <= harmonics; ++k) {
        if (f0 * k >= b.rate * 0.45) break;
        notches.push_back(Biquad::design(FilterType::Notch, b.rate, f0 * k, std::max(1.0, s.q)));
    }
    detail::processRegion(
        b, r, ch,
        [&](std::vector<float>& x, int) {
            for (const auto& f : notches) {
                double z1 = 0, z2 = 0;
                for (float& v : x) {
                    const double in = v, out = f.b0 * in + z1;
                    z1 = f.b1 * in - f.a1 * out + z2;
                    z2 = f.b2 * in - f.a2 * out;
                    v = static_cast<float>(out);
                }
            }
        },
        detail::edgeFrames(b));
    return f0;
}

void spectralGain(Buffer& b, double lowHz, double highHz, double gainDb, Range r, const std::vector<int>& ch, int fftSize) {
    if (highHz < lowHz) std::swap(lowHz, highHz);
    int n = 256;
    while (n < fftSize && n < 16384) n <<= 1;
    const auto g = static_cast<float>(dbToGain(gainDb));
    const auto half = static_cast<float>(dbToGain(gainDb / 2));
    const double binHz = static_cast<double>(b.rate) / n;
    const auto bins = static_cast<std::size_t>(n / 2 + 1);
    std::vector<float> mask(bins, 1.0f);
    for (std::size_t k = 0; k < bins; ++k) {
        const double f = static_cast<double>(k) * binHz;
        if (f >= lowHz && f <= highHz) mask[k] = g;
        else if (f >= lowHz - binHz && f <= highHz + binHz) mask[k] = half;  // soft edges
    }
    detail::processRegion(
        b, r, ch,
        [&](std::vector<float>& x, int) {
            auto frames = detail::stft(x, n);
            for (auto& f : frames)
                for (std::size_t k = 0; k < bins; ++k) f[k] *= mask[k];
            x = detail::istft(frames, n, x.size());
        },
        detail::edgeFrames(b));
}

}  // namespace wetwaresuite::audio
