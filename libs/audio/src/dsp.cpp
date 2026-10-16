// Generators, levels, fades, editing, channels, resampling, filters and silence.
#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

#include "wetwaresuite/audio/analysis.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "internal.hpp"

namespace wetwaresuite::audio {

using detail::kPi;

namespace {

Range clampRange(const Buffer& b, Range r) {
    r.end = std::min(r.end, b.frames());
    r.begin = std::min(r.begin, r.end);
    return r;
}

double fadeShape(FadeCurve c, double x) {
    x = std::clamp(x, 0.0, 1.0);
    switch (c) {
        case FadeCurve::Linear: return x;
        case FadeCurve::Exponential: return x * x * x;
        case FadeCurve::Logarithmic: return 1 - (1 - x) * (1 - x) * (1 - x);
        case FadeCurve::SCurve: return 0.5 - 0.5 * std::cos(kPi * x);
        case FadeCurve::EqualPower: return std::sin(kPi * x / 2);
    }
    return x;
}

double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 40; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

}  // namespace

// ---- generators ----

Buffer silence(int channels, double seconds, int rate) {
    return Buffer(std::max(1, channels), static_cast<std::size_t>(std::max(0.0, std::round(seconds * rate))), rate);
}

Buffer tone(int channels, double seconds, int rate, double frequency, double amplitude, Waveform w) {
    Buffer b = silence(channels, seconds, rate);
    const std::size_t n = b.frames();
    for (std::size_t i = 0; i < n; ++i) {
        const double phase = std::fmod(frequency * static_cast<double>(i) / rate, 1.0);
        double v = 0;
        switch (w) {
            case Waveform::Sine: v = std::sin(2 * kPi * phase); break;
            case Waveform::Square: v = phase < 0.5 ? 1 : -1; break;
            case Waveform::Saw: v = 2 * phase - 1; break;
            case Waveform::Triangle: v = phase < 0.5 ? 4 * phase - 1 : 3 - 4 * phase; break;
        }
        const auto s = static_cast<float>(v * amplitude);
        for (auto& c : b.channels) c[i] = s;
    }
    return b;
}

Buffer noise(int channels, double seconds, int rate, double amplitude, NoiseColor color, unsigned seed) {
    Buffer b = silence(channels, seconds, rate);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    for (auto& c : b.channels) {
        double p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0, brown = 0;
        std::vector<double> raw(c.size());
        double peak = 1e-9;
        for (std::size_t i = 0; i < c.size(); ++i) {
            const double white = uni(rng);
            double v = white;
            if (color == NoiseColor::Pink) {  // Paul Kellet's refined filter
                p0 = 0.99886 * p0 + white * 0.0555179;
                p1 = 0.99332 * p1 + white * 0.0750759;
                p2 = 0.96900 * p2 + white * 0.1538520;
                p3 = 0.86650 * p3 + white * 0.3104856;
                p4 = 0.55000 * p4 + white * 0.5329522;
                p5 = -0.7616 * p5 - white * 0.0168980;
                v = p0 + p1 + p2 + p3 + p4 + p5 + p6 + white * 0.5362;
                p6 = white * 0.115926;
            } else if (color == NoiseColor::Brown) {
                brown = (brown + 0.02 * white) / 1.02;
                v = brown;
            }
            raw[i] = v;
            peak = std::max(peak, std::fabs(v));
        }
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = static_cast<float>(raw[i] / peak * amplitude);
    }
    return b;
}

// ---- level ----

void gain(Buffer& b, double db, Range r, const Channels& ch) {
    r = clampRange(b, r);
    const auto g = static_cast<float>(dbToGain(db));
    for (int c : resolveChannels(b, ch)) {
        auto& d = b.channels[static_cast<std::size_t>(c)];
        for (std::size_t i = r.begin; i < r.end; ++i) d[i] *= g;
    }
}

double normalizePeak(Buffer& b, double targetDb, Range r, const Channels& ch) {
    r = clampRange(b, r);
    float peak = 0;
    for (int c : resolveChannels(b, ch)) {
        const auto& d = b.channels[static_cast<std::size_t>(c)];
        for (std::size_t i = r.begin; i < r.end; ++i) peak = std::max(peak, std::fabs(d[i]));
    }
    if (peak <= 1e-9f) return 0;
    const double db = targetDb - gainToDb(peak);
    gain(b, db, r, ch);
    return db;
}

double normalizeLoudness(Buffer& b, double targetLufs, double ceilingDb, bool doLimit) {
    const double lufs = integratedLoudness(b, all(b));
    if (lufs <= -69.9) return 0;  // silence: nothing to measure
    const double db = targetLufs - lufs;
    gain(b, db, all(b));
    if (doLimit) {
        // the limiter takes a little loudness away; one extra pass gets back close to the target
        for (int pass = 0; pass < 2; ++pass) {
            float peak = 0;
            for (const auto& c : b.channels) {
                for (float v : c) peak = std::max(peak, std::fabs(v));
            }
            if (gainToDb(peak) <= ceilingDb + 0.01) break;
            limit(b, ceilingDb, 80, all(b));
            const double now = integratedLoudness(b, all(b));
            if (pass == 0 && now < targetLufs - 0.3) gain(b, std::min(targetLufs - now, 3.0), all(b));
        }
    }
    return db;
}

const std::vector<std::string>& fadeCurveNames() {
    static const std::vector<std::string> v = {"linear", "exponential", "logarithmic", "s_curve", "equal_power"};
    return v;
}

bool parseFadeCurve(const std::string& n, FadeCurve& out) {
    if (n == "linear") out = FadeCurve::Linear;
    else if (n == "exponential" || n == "exp") out = FadeCurve::Exponential;
    else if (n == "logarithmic" || n == "log") out = FadeCurve::Logarithmic;
    else if (n == "s_curve" || n == "scurve" || n == "smooth") out = FadeCurve::SCurve;
    else if (n == "equal_power" || n == "sine") out = FadeCurve::EqualPower;
    else return false;
    return true;
}

void fadeIn(Buffer& b, double seconds, Range r, FadeCurve c, const Channels& ch) {
    r = clampRange(b, r);
    const std::size_t n = std::min(r.size(), static_cast<std::size_t>(std::max(0.0, seconds * b.rate)));
    if (n == 0) return;
    for (int k : resolveChannels(b, ch)) {
        auto& d = b.channels[static_cast<std::size_t>(k)];
        for (std::size_t i = 0; i < n; ++i) d[r.begin + i] *= static_cast<float>(fadeShape(c, static_cast<double>(i) / static_cast<double>(n)));
    }
}

void fadeOut(Buffer& b, double seconds, Range r, FadeCurve c, const Channels& ch) {
    r = clampRange(b, r);
    const std::size_t n = std::min(r.size(), static_cast<std::size_t>(std::max(0.0, seconds * b.rate)));
    if (n == 0) return;
    for (int k : resolveChannels(b, ch)) {
        auto& d = b.channels[static_cast<std::size_t>(k)];
        for (std::size_t i = 0; i < n; ++i) {
            d[r.end - n + i] *= static_cast<float>(fadeShape(c, static_cast<double>(n - 1 - i) / static_cast<double>(n)));
        }
    }
}

// ---- editing ----

void trim(Buffer& b, Range keep) { b = slice(b, clampRange(b, keep)); }

Buffer cut(Buffer& b, Range r) {
    r = clampRange(b, r);
    Buffer removed = slice(b, r);
    for (auto& c : b.channels) c.erase(c.begin() + static_cast<std::ptrdiff_t>(r.begin), c.begin() + static_cast<std::ptrdiff_t>(r.end));
    return removed;
}

void insert(Buffer& b, std::size_t at, const Buffer& clip) {
    const Buffer src = conform(clip, b.rate, b.channelCount());
    at = std::min(at, b.frames());
    for (std::size_t c = 0; c < b.channels.size(); ++c) {
        b.channels[c].insert(b.channels[c].begin() + static_cast<std::ptrdiff_t>(at), src.channels[c].begin(), src.channels[c].end());
    }
}

void overwrite(Buffer& b, std::size_t at, const Buffer& clip) {
    const Buffer src = conform(clip, b.rate, b.channelCount());
    if (at + src.frames() > b.frames()) b.resize(at + src.frames());
    for (std::size_t c = 0; c < b.channels.size(); ++c) std::copy(src.channels[c].begin(), src.channels[c].end(), b.channels[c].begin() + static_cast<std::ptrdiff_t>(at));
}

void insertSilence(Buffer& b, std::size_t at, std::size_t frames) {
    at = std::min(at, b.frames());
    for (auto& c : b.channels) c.insert(c.begin() + static_cast<std::ptrdiff_t>(at), frames, 0.0f);
}

void reverse(Buffer& b, Range r, const Channels& ch) {
    r = clampRange(b, r);
    for (int c : resolveChannels(b, ch)) {
        auto& d = b.channels[static_cast<std::size_t>(c)];
        std::reverse(d.begin() + static_cast<std::ptrdiff_t>(r.begin), d.begin() + static_cast<std::ptrdiff_t>(r.end));
    }
}

void mixInto(Buffer& b, const Buffer& src, std::size_t at, double gainDb) {
    const Buffer s = conform(src, b.rate, b.channelCount());
    if (at + s.frames() > b.frames()) b.resize(at + s.frames());
    const auto g = static_cast<float>(dbToGain(gainDb));
    for (std::size_t c = 0; c < b.channels.size(); ++c) {
        for (std::size_t i = 0; i < s.frames(); ++i) b.channels[c][at + i] += s.channels[c][i] * g;
    }
}

Range nonSilent(const Buffer& b, double thresholdDb) {
    // 10 ms RMS windows (loudest channel), so a stray noise peak doesn't count as sound
    const std::size_t n = b.frames();
    const std::size_t win = std::max<std::size_t>(1, static_cast<std::size_t>(b.rate / 100));
    const double t = dbToGain(thresholdDb);
    auto loud = [&](std::size_t w) {
        const std::size_t e = std::min(n, w + win);
        for (const auto& c : b.channels) {
            double sum = 0;
            for (std::size_t i = w; i < e; ++i) sum += static_cast<double>(c[i]) * c[i];
            if (std::sqrt(sum / static_cast<double>(e - w)) > t) return true;
        }
        return false;
    };
    std::size_t first = n, last = 0;
    for (std::size_t w = 0; w < n; w += win) {
        if (loud(w)) {
            first = w;
            break;
        }
    }
    if (first == n) return {0, 0};
    for (std::size_t w = (n - 1) / win * win;; w -= win) {
        if (loud(w)) {
            last = std::min(n, w + win);
            break;
        }
        if (w < win) break;
    }
    return {first, std::max(first, last)};
}

// ---- channels and rate ----

void pan(Buffer& b, double position, Range r) {
    if (b.channelCount() == 1) setChannels(b, 2);
    if (b.channelCount() < 2) return;
    r = clampRange(b, r);
    position = std::clamp(position, -1.0, 1.0);
    const auto gl = static_cast<float>(position > 0 ? 1 - position : 1.0);
    const auto gr = static_cast<float>(position < 0 ? 1 + position : 1.0);
    for (std::size_t i = r.begin; i < r.end; ++i) {
        b.channels[0][i] *= gl;
        b.channels[1][i] *= gr;
    }
}

void setChannels(Buffer& b, int count) {
    count = std::clamp(count, 1, 32);
    const int n = b.channelCount();
    if (n == count || n == 0) {
        if (n == 0) b.channels.assign(static_cast<std::size_t>(count), {});
        return;
    }
    const std::size_t frames = b.frames();
    std::vector<std::vector<float>> out(static_cast<std::size_t>(count), std::vector<float>(frames, 0.0f));
    if (count == 1) {
        for (const auto& c : b.channels) {
            for (std::size_t i = 0; i < frames; ++i) out[0][i] += c[i] / static_cast<float>(n);
        }
    } else if (n == 6 && count == 2) {  // 5.1 (L R C LFE Ls Rs) to stereo, ITU downmix
        const float k = 0.7071f;
        for (std::size_t i = 0; i < frames; ++i) {
            out[0][i] = (b.channels[0][i] + k * b.channels[2][i] + k * b.channels[4][i]) / (1 + 2 * k);
            out[1][i] = (b.channels[1][i] + k * b.channels[2][i] + k * b.channels[5][i]) / (1 + 2 * k);
        }
    } else if (n > count) {
        for (int c = 0; c < count; ++c) out[static_cast<std::size_t>(c)] = b.channels[static_cast<std::size_t>(c)];
    } else {
        for (int c = 0; c < count; ++c) out[static_cast<std::size_t>(c)] = b.channels[static_cast<std::size_t>(c % n)];
    }
    b.channels = std::move(out);
}

void swapChannels(Buffer& b) {
    if (b.channelCount() >= 2) std::swap(b.channels[0], b.channels[1]);
}

void extractChannel(Buffer& b, int channel) {
    if (channel < 0 || channel >= b.channelCount()) throw std::runtime_error("no channel " + std::to_string(channel));
    auto keep = std::move(b.channels[static_cast<std::size_t>(channel)]);
    b.channels.clear();
    b.channels.push_back(std::move(keep));
}

Buffer resample(const Buffer& b, int newRate, int quality) {
    if (newRate <= 0) throw std::runtime_error("sample rate must be positive");
    if (newRate == b.rate || b.empty()) {
        Buffer out = b;
        out.rate = newRate;
        return out;
    }
    Buffer out = detail::resampleBy(b, static_cast<double>(newRate) / b.rate, quality);
    out.rate = newRate;
    return out;
}

Buffer detail::resampleBy(const Buffer& b, double ratio, int quality) {
    const auto outFrames = static_cast<std::size_t>(std::llround(static_cast<double>(b.frames()) * ratio));
    Buffer out(b.channelCount(), outFrames, b.rate);
    if (b.empty() || outFrames == 0) return out;
    const auto inFrames = static_cast<std::ptrdiff_t>(b.frames());
    if (quality <= 0) {  // Catmull-Rom cubic
        for (std::size_t c = 0; c < b.channels.size(); ++c) {
            const auto& x = b.channels[c];
            auto at = [&](std::ptrdiff_t i) { return x[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i, 0, inFrames - 1))]; };
            for (std::size_t n = 0; n < outFrames; ++n) {
                const double t = static_cast<double>(n) / ratio;
                const auto i = static_cast<std::ptrdiff_t>(std::floor(t));
                const auto f = static_cast<float>(t - static_cast<double>(i));
                const float p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
                out.channels[c][n] = p1 + 0.5f * f * (p2 - p0 + f * (2 * p0 - 5 * p1 + 4 * p2 - p3 + f * (3 * (p1 - p2) + p3 - p0)));
            }
        }
        return out;
    }
    // Kaiser-windowed sinc with a tabulated kernel.
    const double fc = std::min(1.0, ratio) * 0.95;  // cutoff relative to the input Nyquist
    const int halfTaps = 32;
    const double width = halfTaps / fc;  // kernel half-width in input samples
    const int res = 256;                 // table entries per input sample
    const auto tableSize = static_cast<std::size_t>(std::ceil(width * res)) + 2;
    std::vector<float> table(tableSize);
    const double beta = 8.6, i0b = besselI0(beta);
    for (std::size_t k = 0; k < tableSize; ++k) {
        const double u = static_cast<double>(k) / res;
        double v = 0;
        if (u < width) {
            const double x = fc * u;
            const double sinc = x < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double r = u / width;
            v = fc * sinc * besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0b;
        }
        table[k] = static_cast<float>(v);
    }
    auto kernel = [&](double u) {
        u = std::fabs(u) * res;
        const auto k = static_cast<std::size_t>(u);
        if (k + 1 >= tableSize) return 0.0f;
        const auto f = static_cast<float>(u - static_cast<double>(k));
        return table[k] + (table[k + 1] - table[k]) * f;
    };
    // polyphase: the fractional position is quantised to one of `phases` precomputed tap sets
    const auto span = static_cast<std::ptrdiff_t>(std::ceil(width));
    const auto taps = static_cast<std::size_t>(2 * span);
    const std::size_t phases = std::clamp<std::size_t>((std::size_t{4} << 20) / taps, 256, 2048);
    std::vector<float> bank(phases * taps);
    for (std::size_t ph = 0; ph < phases; ++ph) {
        const double frac = static_cast<double>(ph) / static_cast<double>(phases);
        for (std::size_t j = 0; j < taps; ++j) bank[ph * taps + j] = kernel(frac + static_cast<double>(span - 1) - static_cast<double>(j));
    }
    for (std::size_t c = 0; c < b.channels.size(); ++c) {
        std::vector<float> x(static_cast<std::size_t>(inFrames + 2 * span + 2), 0.0f);
        std::copy(b.channels[c].begin(), b.channels[c].end(), x.begin() + span);
        float* o = out.channels[c].data();
        for (std::size_t n = 0; n < outFrames; ++n) {
            const double t = static_cast<double>(n) / ratio;
            auto centre = static_cast<std::ptrdiff_t>(t);
            auto ph = static_cast<std::size_t>(std::lround((t - static_cast<double>(centre)) * static_cast<double>(phases)));
            if (ph >= phases) ph -= phases, ++centre;
            const float* w = bank.data() + ph * taps;
            const float* in = x.data() + (centre + 1);  // padded index of centre - span + 1
            float acc = 0;
            for (std::size_t j = 0; j < taps; ++j) acc += in[j] * w[j];
            o[n] = acc;
        }
    }
    return out;
}

Buffer conform(const Buffer& b, int rate, int channels) {
    Buffer out = b.rate == rate ? b : resample(b, rate);
    if (out.channelCount() != channels) setChannels(out, channels);
    return out;
}

// ---- filters ----

const std::vector<std::string>& filterTypeNames() {
    static const std::vector<std::string> v = {"lowpass", "highpass", "bandpass", "notch", "peaking", "lowshelf", "highshelf"};
    return v;
}

bool parseFilterType(const std::string& n, FilterType& out) {
    if (n == "lowpass") out = FilterType::LowPass;
    else if (n == "highpass") out = FilterType::HighPass;
    else if (n == "bandpass") out = FilterType::BandPass;
    else if (n == "notch") out = FilterType::Notch;
    else if (n == "peaking" || n == "bell" || n == "peak") out = FilterType::Peaking;
    else if (n == "lowshelf") out = FilterType::LowShelf;
    else if (n == "highshelf") out = FilterType::HighShelf;
    else return false;
    return true;
}

Biquad Biquad::design(FilterType t, double rate, double frequency, double q, double gainDb) {
    frequency = std::clamp(frequency, 1.0, rate * 0.499);
    q = std::max(q, 0.05);
    const double w0 = 2 * kPi * frequency / rate, cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2 * q);
    const double A = std::pow(10.0, gainDb / 40), sa = 2 * std::sqrt(A) * alpha;
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (t) {
        case FilterType::LowPass: b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha; break;
        case FilterType::HighPass: b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha; break;
        case FilterType::BandPass: b0 = alpha, b1 = 0, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha; break;
        case FilterType::Notch: b0 = 1, b1 = -2 * cw, b2 = 1, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha; break;
        case FilterType::Peaking:
            b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A;
            break;
        case FilterType::LowShelf:
            b0 = A * ((A + 1) - (A - 1) * cw + sa), b1 = 2 * A * ((A - 1) - (A + 1) * cw), b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa, a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - sa;
            break;
        case FilterType::HighShelf:
            b0 = A * ((A + 1) + (A - 1) * cw + sa), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - sa;
            break;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

void filter(Buffer& b, const Biquad& f, Range r, const Channels& ch) {
    detail::processRegion(
        b, r, ch,
        [&f](std::vector<float>& x, int) {
            double z1 = 0, z2 = 0;  // transposed direct form II
            for (float& s : x) {
                const double in = s;
                const double out = f.b0 * in + z1;
                z1 = f.b1 * in - f.a1 * out + z2;
                z2 = f.b2 * in - f.a2 * out;
                s = static_cast<float>(out);
            }
        },
        detail::edgeFrames(b));
}

void equalize(Buffer& b, FilterType t, double frequency, double gainDb, double q, Range r, const Channels& ch, int order) {
    order = std::clamp(order, 1, 8);
    const bool shaped = t == FilterType::Peaking || t == FilterType::LowShelf || t == FilterType::HighShelf;
    const Biquad f = Biquad::design(t, b.rate, frequency, q, shaped ? gainDb / order : 0.0);
    for (int i = 0; i < order; ++i) filter(b, f, r, ch);
    if (!shaped && gainDb != 0) gain(b, gainDb, r, ch);
}

// ---- silence ----

std::vector<Region> detectSilence(const Buffer& b, double thresholdDb, double minSeconds) {
    std::vector<Region> out;
    const std::size_t win = std::max<std::size_t>(1, static_cast<std::size_t>(b.rate / 100));
    const double t = dbToGain(thresholdDb);
    const std::size_t n = b.frames();
    std::size_t runStart = 0;
    bool inRun = false;
    auto close = [&](std::size_t end) {
        const double s = static_cast<double>(runStart) / b.rate, e = static_cast<double>(end) / b.rate;
        if (e - s >= minSeconds) out.push_back({s, e});
        inRun = false;
    };
    for (std::size_t w = 0; w < n; w += win) {
        const std::size_t e = std::min(n, w + win);
        double loudest = 0;
        for (const auto& c : b.channels) {
            double sum = 0;
            for (std::size_t i = w; i < e; ++i) sum += static_cast<double>(c[i]) * c[i];
            loudest = std::max(loudest, std::sqrt(sum / static_cast<double>(e - w)));
        }
        const bool quiet = loudest < t;
        if (quiet && !inRun) {
            inRun = true;
            runStart = w;
        } else if (!quiet && inRun) {
            close(w);
        }
    }
    if (inRun) close(n);
    return out;
}

std::vector<Region> splitOnSilence(const Buffer& b, double thresholdDb, double minSilence, double minSeconds, double pad) {
    const auto quiet = detectSilence(b, thresholdDb, minSilence);
    std::vector<Region> parts;
    double cursor = 0;
    const double total = b.seconds();
    for (const auto& q : quiet) {
        if (q.start > cursor) parts.push_back({cursor, q.start});
        cursor = q.end;
    }
    if (cursor < total) parts.push_back({cursor, total});
    std::vector<Region> out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].end - parts[i].start < minSeconds) continue;
        const double lo = i > 0 ? (parts[i - 1].end + parts[i].start) / 2 : 0.0;
        const double hi = i + 1 < parts.size() ? (parts[i].end + parts[i + 1].start) / 2 : total;
        out.push_back({std::max(lo, parts[i].start - pad), std::min(hi, parts[i].end + pad)});
    }
    return out;
}

}  // namespace wetwaresuite::audio
