#include "internal.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace wetwaresuite::audio::detail {

namespace {

// Twiddles e^{-2 pi i k / n} for k < n/2, computed once per size.
const std::vector<std::complex<float>>& twiddles(std::size_t n) {
    thread_local std::map<std::size_t, std::vector<std::complex<float>>> cache;
    auto it = cache.find(n);
    if (it != cache.end()) return it->second;
    std::vector<std::complex<float>> t(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k) {
        const double a = -2 * kPi * static_cast<double>(k) / static_cast<double>(n);
        t[k] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
    return cache.emplace(n, std::move(t)).first->second;
}

}  // namespace

void fft(std::vector<std::complex<float>>& a, bool inverse) {
    const std::size_t n = a.size();
    if (n < 2) return;
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    const auto& tw = twiddles(n);
    // plain float arithmetic: std::complex multiplication checks for NaN/inf and is several times slower
    auto* d = reinterpret_cast<float*>(a.data());
    const auto* w = reinterpret_cast<const float*>(tw.data());
    const float sign = inverse ? -1.0f : 1.0f;
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t half = len / 2, step = n / len;
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t k = 0; k < half; ++k) {
                const float wr = w[2 * k * step], wi = sign * w[2 * k * step + 1];
                float* u = d + 2 * (i + k);
                float* v = d + 2 * (i + k + half);
                const float vr = v[0] * wr - v[1] * wi, vi = v[0] * wi + v[1] * wr;
                v[0] = u[0] - vr;
                v[1] = u[1] - vi;
                u[0] += vr;
                u[1] += vi;
            }
        }
    }
}

std::vector<float> hann(int n) {
    std::vector<float> w(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) w[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / n));
    return w;
}

void processRegionMulti(Buffer& b, Range r, const std::vector<int>& channels,
                        const std::function<void(std::vector<std::vector<float>>&)>& fn, std::size_t fadeFrames) {
    r.end = std::min(r.end, b.frames());
    if (r.size() == 0) return;
    const bool atStart = r.begin == 0, atEnd = r.end == b.frames();
    fadeFrames = std::min(fadeFrames, r.size() / 4);
    const auto chans = resolveChannels(b, channels);
    std::vector<std::vector<float>> regions;
    for (int c : chans) {
        const auto& data = b.channels[static_cast<std::size_t>(c)];
        regions.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(r.begin), data.begin() + static_cast<std::ptrdiff_t>(r.end));
    }
    fn(regions);
    const std::size_t n = r.size();
    for (std::size_t k = 0; k < chans.size(); ++k) {
        auto& data = b.channels[static_cast<std::size_t>(chans[k])];
        const auto& region = regions[k];
        for (std::size_t i = 0; i < n; ++i) {
            float mix = 1.0f;
            if (!atStart && i < fadeFrames) mix = static_cast<float>(i + 1) / static_cast<float>(fadeFrames + 1);
            if (!atEnd && n - 1 - i < fadeFrames) mix = std::min(mix, static_cast<float>(n - i) / static_cast<float>(fadeFrames + 1));
            float& d = data[r.begin + i];
            d = d + (region[i] - d) * mix;
        }
    }
}

void processRegion(Buffer& b, Range r, const std::vector<int>& channels, const std::function<void(std::vector<float>&, int)>& fn,
                   std::size_t fadeFrames) {
    const auto chans = resolveChannels(b, channels);
    processRegionMulti(
        b, r, channels,
        [&](std::vector<std::vector<float>>& regions) {
            for (std::size_t k = 0; k < regions.size(); ++k) fn(regions[k], chans[k]);
        },
        fadeFrames);
}

Spectra stft(const std::vector<float>& x, int fftSize) {
    const auto n = static_cast<std::size_t>(fftSize);
    const std::size_t hop = n / 4;
    const auto w = hann(fftSize);
    const std::size_t frames = x.size() / hop + 1;
    Spectra out(frames);
    std::vector<std::complex<float>> buf(n);
    for (std::size_t f = 0; f < frames; ++f) {
        const auto centre = static_cast<std::ptrdiff_t>(f * hop);
        for (std::size_t i = 0; i < n; ++i) {
            const std::ptrdiff_t idx = centre - static_cast<std::ptrdiff_t>(n / 2) + static_cast<std::ptrdiff_t>(i);
            const float v = idx >= 0 && idx < static_cast<std::ptrdiff_t>(x.size()) ? x[static_cast<std::size_t>(idx)] : 0.0f;
            buf[i] = {v * w[i], 0.0f};
        }
        fft(buf, false);
        out[f].assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n / 2 + 1));
    }
    return out;
}

std::vector<float> istft(const Spectra& frames, int fftSize, std::size_t length) {
    const auto n = static_cast<std::size_t>(fftSize);
    const std::size_t hop = n / 4;
    const auto w = hann(fftSize);
    std::vector<float> out(length, 0.0f), norm(length, 0.0f);
    std::vector<std::complex<float>> buf(n);
    for (std::size_t f = 0; f < frames.size(); ++f) {
        for (std::size_t k = 0; k <= n / 2; ++k) buf[k] = frames[f][k];
        for (std::size_t k = 1; k < n / 2; ++k) buf[n - k] = std::conj(frames[f][k]);
        fft(buf, true);
        const auto centre = static_cast<std::ptrdiff_t>(f * hop);
        for (std::size_t i = 0; i < n; ++i) {
            const std::ptrdiff_t idx = centre - static_cast<std::ptrdiff_t>(n / 2) + static_cast<std::ptrdiff_t>(i);
            if (idx < 0 || idx >= static_cast<std::ptrdiff_t>(length)) continue;
            out[static_cast<std::size_t>(idx)] += buf[i].real() / static_cast<float>(n) * w[i];
            norm[static_cast<std::size_t>(idx)] += w[i] * w[i];
        }
    }
    for (std::size_t i = 0; i < length; ++i) {
        if (norm[i] > 1e-6f) out[i] /= norm[i];
    }
    return out;
}

}  // namespace wetwaresuite::audio::detail
