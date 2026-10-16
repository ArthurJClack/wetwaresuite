// Reverb, echo, time stretch, pitch shift and speed.
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "achclwss/audio/dsp.hpp"
#include "internal.hpp"

namespace achclwss::audio {

namespace {

// ---- Freeverb (Jezar at Dreampoint, public domain) ----
struct Comb {
    std::vector<float> buf;
    std::size_t pos = 0;
    float store = 0;
    float process(float in, float feedback, float damp) {
        const float out = buf[pos];
        store = out * (1 - damp) + store * damp;
        buf[pos] = in + store * feedback;
        if (++pos >= buf.size()) pos = 0;
        return out;
    }
};

struct Allpass {
    std::vector<float> buf;
    std::size_t pos = 0;
    float process(float in) {
        const float b = buf[pos];
        buf[pos] = in + b * 0.5f;
        if (++pos >= buf.size()) pos = 0;
        return b - in;
    }
};

struct ReverbBank {
    std::vector<Comb> combs;
    std::vector<Allpass> allpasses;
    ReverbBank(int rate, int spread) {
        const int combTuning[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        const int allpassTuning[] = {556, 441, 341, 225};
        const double scale = rate / 44100.0;
        for (int t : combTuning) combs.push_back({std::vector<float>(static_cast<std::size_t>(std::max(1.0, (t + spread) * scale)), 0.0f)});
        for (int t : allpassTuning) allpasses.push_back({std::vector<float>(static_cast<std::size_t>(std::max(1.0, (t + spread) * scale)), 0.0f)});
    }
    float process(float in, float feedback, float damp) {
        float out = 0;
        for (auto& c : combs) out += c.process(in, feedback, damp);
        for (auto& a : allpasses) out = a.process(out);
        return out;
    }
};

}  // namespace

void reverb(Buffer& b, const ReverbSettings& s, Range r, const Channels& ch) {
    const auto feedback = static_cast<float>(std::clamp(s.roomSize, 0.0, 1.0) * 0.28 + 0.7);
    const auto damp = static_cast<float>(std::clamp(s.damping, 0.0, 1.0) * 0.4);
    const double wet = std::clamp(s.wet, 0.0, 1.0) * 3.0, width = std::clamp(s.width, 0.0, 1.0);
    const auto dry = static_cast<float>(std::clamp(s.dry, 0.0, 1.0));
    const auto wet1 = static_cast<float>(wet * (width / 2 + 0.5)), wet2 = static_cast<float>(wet * ((1 - width) / 2));
    const auto pre = static_cast<std::size_t>(std::max(0.0, s.preDelayMs) * 0.001 * b.rate);
    const int rate = b.rate;
    detail::processRegionMulti(
        b, r, ch,
        [&](std::vector<std::vector<float>>& x) {
            if (x.empty()) return;
            const std::size_t n = x[0].size();
            const float inGain = 0.015f / static_cast<float>(x.size());
            std::vector<float> mono(n, 0.0f);
            for (const auto& c : x) {
                for (std::size_t i = 0; i < n; ++i) mono[i] += c[i] * inGain;
            }
            if (pre > 0) {
                mono.insert(mono.begin(), std::min(pre, n), 0.0f);
                mono.resize(n);
            }
            if (x.size() == 2) {
                ReverbBank left(rate, 0), right(rate, 23);
                for (std::size_t i = 0; i < n; ++i) {
                    const float l = left.process(mono[i], feedback, damp), rr = right.process(mono[i], feedback, damp);
                    x[0][i] = x[0][i] * dry + l * wet1 + rr * wet2;
                    x[1][i] = x[1][i] * dry + rr * wet1 + l * wet2;
                }
            } else {
                for (std::size_t c = 0; c < x.size(); ++c) {
                    ReverbBank bank(rate, static_cast<int>(c) * 23);
                    for (std::size_t i = 0; i < n; ++i) x[c][i] = x[c][i] * dry + bank.process(mono[i], feedback, damp) * (wet1 + wet2);
                }
            }
        },
        detail::edgeFrames(b));
}

void echo(Buffer& b, double delaySeconds, double feedback, double mix, Range r, const Channels& ch, bool pingPong) {
    const auto d = static_cast<std::size_t>(std::max(1.0, delaySeconds * b.rate));
    const auto fb = static_cast<float>(std::clamp(feedback, 0.0, 0.98));
    const auto m = static_cast<float>(std::clamp(mix, 0.0, 1.0));
    detail::processRegionMulti(
        b, r, ch,
        [&](std::vector<std::vector<float>>& x) {
            if (x.empty()) return;
            const std::size_t n = x[0].size();
            if (pingPong && x.size() == 2) {
                std::vector<float> el(n, 0.0f), er(n, 0.0f);
                for (std::size_t i = d; i < n; ++i) {
                    el[i] = 0.5f * (x[0][i - d] + x[1][i - d]) + fb * er[i - d];
                    er[i] = fb * el[i - d];
                }
                for (std::size_t i = 0; i < n; ++i) {
                    x[0][i] += m * el[i];
                    x[1][i] += m * er[i];
                }
                return;
            }
            for (auto& c : x) {
                std::vector<float> e(n, 0.0f);
                for (std::size_t i = d; i < n; ++i) e[i] = c[i - d] + fb * e[i - d];
                for (std::size_t i = 0; i < n; ++i) c[i] += m * e[i];
            }
        },
        detail::edgeFrames(b));
}

Buffer stretch(const Buffer& b, double factor) {
    if (!(factor > 0.05 && factor < 20)) throw std::runtime_error("stretch factor must be between 0.05 and 20");
    if (b.empty() || std::fabs(factor - 1.0) < 1e-6) return b;
    const auto N = static_cast<std::ptrdiff_t>(std::max(256, (b.rate / 25) & ~1));  // 40 ms frames
    const std::ptrdiff_t Hs = N / 2;                                              // output hop
    const double Ha = static_cast<double>(Hs) / factor;                           // input hop
    const auto tol = static_cast<std::ptrdiff_t>(b.rate / 100);                  // +-10 ms search
    const auto inFrames = static_cast<std::ptrdiff_t>(b.frames());
    const auto outFrames = static_cast<std::ptrdiff_t>(std::llround(static_cast<double>(inFrames) * factor));
    const std::ptrdiff_t pad = N + tol + 2;

    // padded mono mix for matching, padded channels for output
    std::vector<float> mono(static_cast<std::size_t>(inFrames + 2 * pad), 0.0f);
    std::vector<std::vector<float>> in(b.channels.size(), std::vector<float>(mono.size(), 0.0f));
    for (std::size_t c = 0; c < b.channels.size(); ++c) {
        std::copy(b.channels[c].begin(), b.channels[c].end(), in[c].begin() + pad);
        for (std::ptrdiff_t i = 0; i < inFrames; ++i) mono[static_cast<std::size_t>(i + pad)] += b.channels[c][static_cast<std::size_t>(i)];
    }
    const auto w = detail::hann(static_cast<int>(N));
    std::vector<std::vector<float>> out(b.channels.size(), std::vector<float>(static_cast<std::size_t>(outFrames + N), 0.0f));
    std::vector<float> norm(static_cast<std::size_t>(outFrames + N), 0.0f);

    auto corr = [&](std::ptrdiff_t a, std::ptrdiff_t c, std::ptrdiff_t step) {
        double s = 0, ea = 0, ec = 0;
        for (std::ptrdiff_t i = 0; i < N; i += step) {
            const double x = mono[static_cast<std::size_t>(a + i)], y = mono[static_cast<std::size_t>(c + i)];
            s += x * y;
            ea += x * x;
            ec += y * y;
        }
        return s / std::sqrt(ea * ec + 1e-12);
    };

    std::ptrdiff_t prev = pad;  // where the previous frame was taken from (padded index)
    for (std::ptrdiff_t k = 0;; ++k) {
        const std::ptrdiff_t outPos = k * Hs;
        if (outPos >= outFrames) break;
        const std::ptrdiff_t ideal = pad + static_cast<std::ptrdiff_t>(std::llround(static_cast<double>(k) * Ha)) - N / 2;
        std::ptrdiff_t best = std::clamp<std::ptrdiff_t>(ideal, 0, static_cast<std::ptrdiff_t>(mono.size()) - N);
        if (k > 0) {
            const std::ptrdiff_t target = prev + Hs;  // natural continuation of the last frame
            if (target + N <= static_cast<std::ptrdiff_t>(mono.size())) {
                double bestScore = -2;
                const std::ptrdiff_t lo = std::max<std::ptrdiff_t>(0, ideal - tol);
                const std::ptrdiff_t hi = std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(mono.size()) - N, ideal + tol);
                for (std::ptrdiff_t c = lo; c <= hi; c += 4) {
                    const double sc = corr(target, c, 4);
                    if (sc > bestScore) bestScore = sc, best = c;
                }
                const std::ptrdiff_t coarse = best;
                for (std::ptrdiff_t c = std::max(lo, coarse - 3); c <= std::min(hi, coarse + 3); ++c) {
                    const double sc = corr(target, c, 2);
                    if (sc > bestScore) bestScore = sc, best = c;
                }
            }
        }
        prev = best;
        for (std::ptrdiff_t i = 0; i < N; ++i) {
            const std::ptrdiff_t o = outPos - N / 2 + i;
            if (o < 0 || o >= outFrames) continue;
            const float wi = w[static_cast<std::size_t>(i)];
            for (std::size_t c = 0; c < in.size(); ++c) out[c][static_cast<std::size_t>(o)] += in[c][static_cast<std::size_t>(best + i)] * wi;
            norm[static_cast<std::size_t>(o)] += wi;
        }
    }
    Buffer result(b.channelCount(), static_cast<std::size_t>(outFrames), b.rate);
    for (std::size_t c = 0; c < in.size(); ++c) {
        for (std::ptrdiff_t i = 0; i < outFrames; ++i) {
            const float nv = norm[static_cast<std::size_t>(i)];
            result.channels[c][static_cast<std::size_t>(i)] = nv > 1e-3f ? out[c][static_cast<std::size_t>(i)] / nv : 0.0f;
        }
    }
    return result;
}

Buffer pitchShift(const Buffer& b, double semitones) {
    if (std::fabs(semitones) > 36) throw std::runtime_error("pitch shift is limited to 36 semitones either way");
    if (b.empty() || std::fabs(semitones) < 1e-6) return b;
    const double ratio = std::pow(2.0, semitones / 12.0);
    Buffer longer = stretch(b, ratio);
    Buffer out = detail::resampleBy(longer, 1.0 / ratio);
    out.resize(b.frames());
    return out;
}

Buffer changeSpeed(const Buffer& b, double factor) {
    if (!(factor > 0.05 && factor < 20)) throw std::runtime_error("speed factor must be between 0.05 and 20");
    return detail::resampleBy(b, 1.0 / factor);
}

}  // namespace achclwss::audio
