// Noise reduction by spectral gating (the approach of Audacity's and
// noisereduce's): learn each frequency's noise level, then turn down every
// time-frequency cell that isn't clearly above it.
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "wetwaresuite/audio/dsp.hpp"
#include "internal.hpp"

namespace wetwaresuite::audio {

namespace {

std::vector<float> monoOf(const Buffer& b, Range r) {
    r.end = std::min(r.end, b.frames());
    std::vector<float> m(r.size(), 0.0f);
    if (b.channelCount() == 0) return m;
    const float k = 1.0f / static_cast<float>(b.channelCount());
    for (const auto& c : b.channels) {
        for (std::size_t i = 0; i < m.size(); ++i) m[i] += c[r.begin + i] * k;
    }
    return m;
}

NoiseProfile profileOf(const detail::Spectra& frames, const std::vector<std::size_t>& use, int fftSize, int rate) {
    NoiseProfile p;
    p.fftSize = fftSize;
    p.rate = rate;
    const std::size_t bins = static_cast<std::size_t>(fftSize / 2 + 1);
    p.mean.assign(bins, 0.0f);
    p.spread.assign(bins, 0.0f);
    if (use.empty()) return p;
    for (std::size_t k = 0; k < bins; ++k) {
        double sum = 0, sq = 0;
        for (std::size_t f : use) {
            const double m = std::abs(frames[f][k]);
            sum += m;
            sq += m * m;
        }
        const double mean = sum / static_cast<double>(use.size());
        p.mean[k] = static_cast<float>(mean);
        p.spread[k] = static_cast<float>(std::sqrt(std::max(0.0, sq / static_cast<double>(use.size()) - mean * mean)));
    }
    return p;
}

int checkedSize(int fftSize) {
    int n = 256;
    while (n < fftSize && n < 16384) n <<= 1;
    return n;
}

}  // namespace

NoiseProfile learnNoise(const Buffer& b, Range r, int fftSize) {
    fftSize = checkedSize(fftSize);
    const auto mono = monoOf(b, r);
    if (mono.size() < static_cast<std::size_t>(fftSize / 2)) throw std::runtime_error("the noise sample is too short; select at least 0.1 s of noise");
    const auto frames = detail::stft(mono, fftSize);
    std::vector<std::size_t> use;
    // skip the half-empty frames at the ends of the zero-padded selection
    const std::size_t skip = frames.size() > 6 ? 2 : 0;
    for (std::size_t f = skip; f + skip < frames.size(); ++f) use.push_back(f);
    return profileOf(frames, use, fftSize, b.rate);
}

NoiseProfile estimateNoise(const Buffer& b, int fftSize) {
    fftSize = checkedSize(fftSize);
    const auto mono = monoOf(b, all(b));
    if (mono.size() < static_cast<std::size_t>(fftSize)) throw std::runtime_error("the audio is too short to estimate its noise");
    const auto frames = detail::stft(mono, fftSize);
    std::vector<std::pair<double, std::size_t>> energy;
    for (std::size_t f = 2; f + 2 < frames.size(); ++f) {
        double e = 0;
        for (const auto& c : frames[f]) e += std::norm(c);
        energy.emplace_back(e, f);
    }
    if (energy.empty()) throw std::runtime_error("the audio is too short to estimate its noise");
    std::sort(energy.begin(), energy.end());
    const std::size_t count = std::max<std::size_t>(std::min<std::size_t>(5, energy.size()), energy.size() / 10);
    std::vector<std::size_t> use;
    for (std::size_t i = 0; i < count; ++i) use.push_back(energy[i].second);
    return profileOf(frames, use, fftSize, b.rate);
}

void reduceNoise(Buffer& b, const NoiseProfile& p, double reductionDb, double sensitivity, Range r, const Channels& ch) {
    if (p.empty()) throw std::runtime_error("no noise profile");
    if (p.rate != b.rate) throw std::runtime_error("the noise profile was learned at a different sample rate; learn it again");
    const int n = p.fftSize;
    const std::size_t bins = static_cast<std::size_t>(n / 2 + 1);
    const auto floorGain = static_cast<float>(dbToGain(-std::fabs(reductionDb)));
    std::vector<float> threshold(bins);
    for (std::size_t k = 0; k < bins; ++k) threshold[k] = p.mean[k] + static_cast<float>(sensitivity) * p.spread[k];
    detail::processRegion(
        b, r, ch,
        [&](std::vector<float>& x, int) {
            auto frames = detail::stft(x, n);
            const std::size_t F = frames.size();
            std::vector<float> mask(F * bins);
            for (std::size_t f = 0; f < F; ++f) {
                for (std::size_t k = 0; k < bins; ++k) {
                    // soft knee around the threshold: 0 below it, 1 at twice it
                    const auto& z = frames[f][k];
                    const float ratio = std::sqrt(z.real() * z.real() + z.imag() * z.imag()) / std::max(threshold[k], 1e-12f);
                    mask[f * bins + k] = std::clamp(ratio - 1.0f, 0.0f, 1.0f);
                }
            }
            // smooth over +-2 frames and the neighbouring bins so isolated cells don't warble
            // ("musical noise"); the narrow frequency kernel keeps pure tones intact
            std::vector<float> tmp(F * bins);
            for (std::size_t f = 0; f < F; ++f) {
                const float* row = &mask[f * bins];
                for (std::size_t k = 0; k < bins; ++k) {
                    const float left = row[k > 0 ? k - 1 : k], right = row[k + 1 < bins ? k + 1 : k];
                    tmp[f * bins + k] = 0.25f * left + 0.5f * row[k] + 0.25f * right;
                }
            }
            for (std::size_t f = 0; f < F; ++f) {
                for (std::size_t k = 0; k < bins; ++k) {
                    float s = 0;
                    int cnt = 0;
                    for (std::size_t g = f > 2 ? f - 2 : 0; g <= std::min(F - 1, f + 2); ++g, ++cnt) s += tmp[g * bins + k];
                    const float m = s / static_cast<float>(cnt);
                    frames[f][k] *= floorGain + (1.0f - floorGain) * m;
                }
            }
            x = detail::istft(frames, n, x.size());
        },
        detail::edgeFrames(b));
}

}  // namespace wetwaresuite::audio
