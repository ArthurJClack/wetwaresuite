// Shared internals: FFT, STFT framing and region helpers.
#pragma once

#include <cmath>
#include <complex>
#include <functional>
#include <vector>

#include "wetwaresuite/audio/buffer.hpp"

namespace wetwaresuite::audio::detail {

constexpr double kPi = 3.14159265358979323846;

// In-place radix-2 FFT; size must be a power of two. inverse does not scale.
void fft(std::vector<std::complex<float>>& a, bool inverse);

// Periodic Hann window.
std::vector<float> hann(int n);

// Processes frames [r.begin, r.end) of the selected channels: `fn` gets a copy of each
// channel's region and changes it in place (same length); the result is blended into the
// original over `fadeFrames` at both edges unless the region touches the buffer's ends.
void processRegion(Buffer& b, Range r, const std::vector<int>& channels, const std::function<void(std::vector<float>&, int)>& fn,
                   std::size_t fadeFrames);
// Edge blend length used by effects: 5 ms.
inline std::size_t edgeFrames(const Buffer& b) { return static_cast<std::size_t>(b.rate / 200); }

// Short-time Fourier analysis of one channel: Hann window, hop fftSize/4, frames centred
// on k*hop (the signal is zero-padded at both ends). Each frame has fftSize/2+1 bins.
using Spectra = std::vector<std::vector<std::complex<float>>>;
Spectra stft(const std::vector<float>& x, int fftSize);
// Overlap-add synthesis back to `length` samples (inverse of stft for unchanged spectra).
std::vector<float> istft(const Spectra& frames, int fftSize, std::size_t length);

// Resamples by an arbitrary ratio (output frames = input * ratio) keeping the rate label:
// what pitch shifting and speed changes need. quality 0 = cubic, 1 = windowed sinc.
Buffer resampleBy(const Buffer& b, double ratio, int quality = 1);

// Like processRegion, but `fn` gets every selected channel's region at once (for effects
// that mix channels, such as stereo reverb).
void processRegionMulti(Buffer& b, Range r, const std::vector<int>& channels,
                        const std::function<void(std::vector<std::vector<float>>&)>& fn, std::size_t fadeFrames);

}  // namespace wetwaresuite::audio::detail
