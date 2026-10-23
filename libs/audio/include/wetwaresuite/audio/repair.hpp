// Restoration: click and crackle removal, mains hum removal and spectral
// (frequency-range) editing. Like the effects in dsp.hpp they touch only the
// given range and channels and blend in at the edges.
#pragma once

#include <vector>

#include "wetwaresuite/audio/buffer.hpp"

namespace wetwaresuite::audio {

struct ClickSettings {
    double sensitivity = 6;   // how far above the local prediction error a sample must jump (lower finds more)
    double maxClickMs = 2.5;  // longer disturbances are left alone (they're not clicks)
};
// Finds clicks with a linear-prediction residual and rebuilds each from the audio on both
// sides (forward and backward prediction, cross-faded). Returns how many were repaired.
int removeClicks(Buffer& b, const ClickSettings& s, Range r, const std::vector<int>& ch = {});

struct HumSettings {
    double frequency = 0;  // mains frequency; 0 = detect 50 or 60 Hz
    int harmonics = 8;     // notch the fundamental and this many multiples in total
    double q = 30;         // notch sharpness: higher is narrower
};
// Notches out mains hum and its harmonics. Returns the fundamental it removed.
double removeHum(Buffer& b, const HumSettings& s, Range r, const std::vector<int>& ch = {});
// 50 or 60: whichever mains frequency (with harmonics) is stronger in the range.
double detectHum(const Buffer& b, Range r);

// Turns frequencies lowHz..highHz up or down by gainDb inside the range (STFT, soft edges):
// the "select a box on the spectrogram and attenuate it" tool.
void spectralGain(Buffer& b, double lowHz, double highHz, double gainDb, Range r, const std::vector<int>& ch = {}, int fftSize = 2048);

// Level of one frequency in the range, dBFS of a sine (Goertzel; mono mix). For measuring.
double toneLevel(const Buffer& b, double frequency, Range r);

}  // namespace wetwaresuite::audio
