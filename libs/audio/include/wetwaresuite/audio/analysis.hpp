// Measuring and drawing audio: levels, loudness (ITU-R BS.1770 / EBU R128),
// clipping, and waveform and spectrogram pictures through libs/raster.
#pragma once

#include <vector>

#include "wetwaresuite/audio/buffer.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::audio {

struct Levels {
    double seconds = 0;
    double peakDb = -240;      // sample peak, dBFS
    double truePeakDb = -240;  // 4x oversampled peak, dBTP
    double rmsDb = -240;
    double lufs = -70;         // integrated loudness; -70 when everything is gated out (silence)
    double momentaryMaxLufs = -70;  // loudest 400 ms
    double shortTermMaxLufs = -70;  // loudest 3 s
    double loudnessRange = 0;  // LRA in LU (EBU Tech 3342)
    double dcOffset = 0;       // mean sample value, worst channel
    std::size_t clipped = 0;   // samples at or over full scale (|x| >= 0.9999)
    std::vector<double> channelPeakDb;
    std::vector<double> channelRmsDb;
};

// truePeak oversamples 4x (slower on long files).
Levels measure(const Buffer& b, Range r, bool truePeak = true);
// Integrated loudness only (LUFS).
double integratedLoudness(const Buffer& b, Range r);

// ---- pictures ----
struct WaveformStyle {
    int width = 1200;
    int height = 300;          // total, split into one lane per channel
    raster::Color background{24, 26, 31, 255};
    raster::Color wave{90, 190, 255, 255};
    raster::Color rms{170, 225, 255, 255};
    raster::Color grid{60, 64, 72, 255};
    raster::Color clip{255, 70, 70, 255};
    bool showClipping = true;
    bool showGrid = true;  // centre line, -6 dB lines and a tick per second (or per 10 s / minute)
    bool decibels = false; // vertical scale in dB instead of linear
};
raster::Image waveform(const Buffer& b, Range r, const WaveformStyle& style = {});

struct SpectrogramStyle {
    int width = 1200;
    int height = 400;
    int fftSize = 2048;
    bool logFrequency = true;  // octaves equally spaced (how we hear); false = linear Hz
    double minFrequency = 20;  // bottom of a log scale
    double maxFrequency = 0;   // 0 = half the sample rate
    double floorDb = -100;     // quieter than this is black
    bool showGrid = true;      // lines at 100 Hz, 1 kHz, 10 kHz (log) or every 2 kHz (linear)
    int channel = -1;          // -1 = mix of all channels
};
raster::Image spectrogram(const Buffer& b, Range r, const SpectrogramStyle& style = {});

}  // namespace wetwaresuite::audio
