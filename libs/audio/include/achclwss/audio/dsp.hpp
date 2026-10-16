// Edits and effects. Functions that take a Range and a channel selection only
// touch those frames and channels (empty selection = all channels); effects
// applied to part of a buffer blend in and out over a few milliseconds so the
// edges don't click.
#pragma once

#include <string>
#include <vector>

#include "achclwss/audio/buffer.hpp"

namespace achclwss::audio {

using Channels = std::vector<int>;

// ---- generators ----
enum class Waveform { Sine, Square, Saw, Triangle };
enum class NoiseColor { White, Pink, Brown };
Buffer silence(int channels, double seconds, int rate);
Buffer tone(int channels, double seconds, int rate, double frequency, double amplitude = 0.5, Waveform w = Waveform::Sine);
Buffer noise(int channels, double seconds, int rate, double amplitude = 0.25, NoiseColor c = NoiseColor::White, unsigned seed = 1);

// ---- level ----
void gain(Buffer& b, double db, Range r, const Channels& ch = {});
// Scales so the loudest sample in the range reaches targetDb (dBFS). Returns the gain applied in dB.
double normalizePeak(Buffer& b, double targetDb, Range r, const Channels& ch = {});
// Scales to an integrated loudness (LUFS, ITU-R BS.1770), then if the result peaks over
// ceilingDb runs a limiter so it doesn't. Returns the gain applied in dB (before limiting).
double normalizeLoudness(Buffer& b, double targetLufs, double ceilingDb = -1.0, bool limit = true);

enum class FadeCurve { Linear, Exponential, Logarithmic, SCurve, EqualPower };
bool parseFadeCurve(const std::string& name, FadeCurve& out);
const std::vector<std::string>& fadeCurveNames();
// Fade in over the first `seconds` of the range, or out over its last `seconds`.
void fadeIn(Buffer& b, double seconds, Range r, FadeCurve c = FadeCurve::Linear, const Channels& ch = {});
void fadeOut(Buffer& b, double seconds, Range r, FadeCurve c = FadeCurve::Linear, const Channels& ch = {});

// ---- editing (every channel) ----
void trim(Buffer& b, Range keep);                   // keeps only the range
Buffer cut(Buffer& b, Range r);                      // removes the range and returns it
void insert(Buffer& b, std::size_t at, const Buffer& clip);  // clip is converted to b's rate and channels
void overwrite(Buffer& b, std::size_t at, const Buffer& clip);  // replaces, extending b if needed
void insertSilence(Buffer& b, std::size_t at, std::size_t frames);
void reverse(Buffer& b, Range r, const Channels& ch = {});
// Adds `src` (converted to b's rate/channels) at frame `at`, scaled by gainDb; b grows to fit.
void mixInto(Buffer& b, const Buffer& src, std::size_t at, double gainDb = 0.0);
// Leading and trailing frames quieter than thresholdDb. Returns the kept range.
Range nonSilent(const Buffer& b, double thresholdDb = -50.0);

// ---- channels and rate ----
// -1 = hard left, 0 = centre, 1 = hard right. Mono input becomes stereo (constant power);
// stereo input is balanced.
void pan(Buffer& b, double position, Range r);
// Up/down-mixes to `count` channels (mono sums, more channels repeat).
void setChannels(Buffer& b, int count);
void swapChannels(Buffer& b);
// Keeps one channel as mono.
void extractChannel(Buffer& b, int channel);
// Windowed-sinc (Kaiser, 64 taps) resampling; quality 0 = cubic (fast), 1 = sinc.
Buffer resample(const Buffer& b, int newRate, int quality = 1);
// Converts rate and channel count to match `like`.
Buffer conform(const Buffer& b, int rate, int channels);

// ---- filters ----
enum class FilterType { LowPass, HighPass, BandPass, Notch, Peaking, LowShelf, HighShelf };
bool parseFilterType(const std::string& name, FilterType& out);
const std::vector<std::string>& filterTypeNames();  // lowpass highpass bandpass notch peaking lowshelf highshelf

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    static Biquad design(FilterType t, double rate, double frequency, double q = 0.7071, double gainDb = 0.0);
};
void filter(Buffer& b, const Biquad& f, Range r, const Channels& ch = {});
// One EQ band (RBJ cookbook). q defaults to 0.7071 (Butterworth) for pass filters.
// `order` stacks the filter for steeper pass/shelf slopes (1..8, 12 dB/oct each for pass filters).
void equalize(Buffer& b, FilterType t, double frequency, double gainDb, double q, Range r, const Channels& ch = {}, int order = 1);

// ---- dynamics ----
struct CompressorSettings {
    double thresholdDb = -18;
    double ratio = 3;
    double attackMs = 10;
    double releaseMs = 120;
    double kneeDb = 6;
    double makeupDb = 0;
    bool autoMakeup = false;  // adds back half the gain reduction at the threshold
};
// Feed-forward compressor, stereo-linked over the selected channels. Returns max gain reduction (dB, positive).
double compress(Buffer& b, const CompressorSettings& s, Range r, const Channels& ch = {});
// Look-ahead brickwall limiter: nothing in the range goes over ceilingDb. Returns max reduction in dB.
double limit(Buffer& b, double ceilingDb, double releaseMs, Range r, const Channels& ch = {});

struct GateSettings {
    double thresholdDb = -45;
    double attackMs = 2;
    double holdMs = 60;
    double releaseMs = 150;
    double rangeDb = -80;  // how far closed is; -80 is silent, -12 just ducks
};
// Returns the fraction of the range the gate was closed.
double gate(Buffer& b, const GateSettings& s, Range r, const Channels& ch = {});

// ---- spectral noise reduction ----
// Per-frequency noise level, learned from a stretch of noise only.
struct NoiseProfile {
    int fftSize = 0;
    int rate = 0;
    std::vector<float> mean;    // magnitude per bin
    std::vector<float> spread;  // standard deviation per bin
    bool empty() const { return mean.empty(); }
};
NoiseProfile learnNoise(const Buffer& b, Range r, int fftSize = 2048);
// Without a recorded stretch of noise: uses the quietest 10% of frames.
NoiseProfile estimateNoise(const Buffer& b, int fftSize = 2048);
// Spectral gating: bins under the noise level (+ sensitivity standard deviations) are turned
// down by reductionDb, with the mask smoothed over time and frequency to avoid warbling.
void reduceNoise(Buffer& b, const NoiseProfile& profile, double reductionDb, double sensitivity, Range r, const Channels& ch = {});

// ---- time effects ----
struct ReverbSettings {
    double roomSize = 0.6;  // 0..1
    double damping = 0.5;   // 0..1
    double wet = 0.3;       // 0..1
    double dry = 1.0;       // 0..1
    double width = 1.0;     // 0..1 stereo spread
    double preDelayMs = 0;
};
// Freeverb (8 combs + 4 allpasses per channel).
void reverb(Buffer& b, const ReverbSettings& s, Range r, const Channels& ch = {});
// Feedback delay. pingPong alternates repeats between left and right on stereo audio.
void echo(Buffer& b, double delaySeconds, double feedback, double mix, Range r, const Channels& ch = {}, bool pingPong = false);

// WSOLA time stretch: factor 2 makes it twice as long at the same pitch.
Buffer stretch(const Buffer& b, double factor);
// Pitch shift in semitones at the same length (stretch + resample).
Buffer pitchShift(const Buffer& b, double semitones);
// Plays faster/slower like tape: pitch and length change together. factor 2 = twice as fast.
Buffer changeSpeed(const Buffer& b, double factor);

// ---- silence ----
struct Region {
    double start = 0;  // seconds
    double end = 0;
};
// Stretches quieter than thresholdDb (RMS over 10 ms) for at least minSeconds.
std::vector<Region> detectSilence(const Buffer& b, double thresholdDb = -40, double minSeconds = 0.5);
// The sounding parts between silences, each at least minSeconds long, padded by padSeconds.
std::vector<Region> splitOnSilence(const Buffer& b, double thresholdDb = -40, double minSilence = 0.5, double minSeconds = 0.25,
                                   double padSeconds = 0.1);

}  // namespace achclwss::audio
