// Preview pictures: the arrangement (a lane per track, bars across, the
// notes and hits of each placed pattern drawn inside it) and a single
// pattern's step grid and piano roll.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/beats/song.hpp"

namespace wetwaresuite::beats {

namespace {

using raster::Color;
using raster::Image;

const Color kBg{24, 26, 31, 255};
const Color kLane{32, 35, 42, 255};
const Color kLaneAlt{28, 31, 37, 255};
const Color kBar{78, 84, 96, 255};
const Color kBeat{46, 50, 59, 255};
const Color kBlackKey{26, 28, 34, 255};

Color trackColor(int i) {
    static const Color palette[] = {{240, 120, 90, 255}, {90, 190, 255, 255}, {150, 220, 110, 255}, {230, 190, 80, 255},
                                    {190, 140, 255, 255}, {90, 220, 200, 255}, {255, 140, 190, 255}, {200, 200, 210, 255}};
    return palette[static_cast<std::size_t>(i) % 8];
}

Color dim(Color c, double f) {
    return {static_cast<std::uint8_t>(c.r * f), static_cast<std::uint8_t>(c.g * f), static_cast<std::uint8_t>(c.b * f), 255};
}

void rect(Image& im, int x0, int y0, int x1, int y1, Color c) {
    x0 = std::max(0, x0), y0 = std::max(0, y0);
    x1 = std::min(im.width(), x1), y1 = std::min(im.height(), y1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) im.set(x, y, c);
    }
}

void vline(Image& im, int x, int y0, int y1, Color c) { rect(im, x, y0, x + 1, y1, c); }
void hline(Image& im, int y, int x0, int x1, Color c) { rect(im, x0, y, x1, y + 1, c); }

bool blackKey(int midi) {
    const int pc = midi % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

}  // namespace

Image drawArrangement(const Song& s, int width) {
    width = std::clamp(width, 200, 4000);
    const int laneH = 56;
    const int tracks = std::max<int>(1, static_cast<int>(s.tracks.size()));
    const int bars = std::max(4, s.bars());
    const int top = 10;
    Image im(width, top + tracks * laneH + 10, kBg);
    const double barW = static_cast<double>(width - 20) / bars;
    const int left = 10;
    const double beatsInBar = s.beatsInBar();
    auto xOfBeat = [&](double beat) { return left + static_cast<int>(std::lround(beat / beatsInBar * barW)); };
    bool anySolo = false;
    for (const auto& t : s.tracks) anySolo = anySolo || t.solo;
    for (int i = 0; i < tracks; ++i) {
        const int y0 = top + i * laneH;
        rect(im, left, y0, width - 10, y0 + laneH - 4, i % 2 ? kLaneAlt : kLane);
    }
    for (int b = 0; b <= bars; ++b) {
        const int x = xOfBeat(b * beatsInBar);
        vline(im, x, top, top + tracks * laneH - 4, b % 4 == 0 ? kBar : kBeat);
    }
    for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
        const Track& t = s.tracks[static_cast<std::size_t>(i)];
        const bool audible = !t.mute && (!anySolo || t.solo);
        const Color c = audible ? trackColor(i) : Color{90, 92, 98, 255};
        const int y0 = top + i * laneH + 3, y1 = top + (i + 1) * laneH - 7;
        // placement blocks
        for (const auto& pl : s.arrangement) {
            if (pl.track != i) continue;
            const Pattern* p = s.findPattern(pl.pattern);
            for (int r = 0; r < std::max(1, pl.repeat); ++r) {
                const int plen = p ? p->bars : 1;
                const int xa = xOfBeat((pl.bar + r * plen) * beatsInBar) + 1, xb = xOfBeat((pl.bar + (r + 1) * plen) * beatsInBar) - 1;
                rect(im, xa, y0, xb, y1, dim(c, 0.28));
                hline(im, y0, xa, xb, dim(c, 0.7));
            }
        }
        const auto ev = trackEvents(s, i);
        if (ev.empty()) continue;
        if (t.kind == TrackKind::Drum) {
            // one row per voice used
            std::vector<std::string> voices;
            for (const auto& e : ev) {
                if (std::find(voices.begin(), voices.end(), e.voice) == voices.end()) voices.push_back(e.voice);
            }
            const double rowH = static_cast<double>(y1 - y0 - 4) / static_cast<double>(std::max<std::size_t>(1, voices.size()));
            for (const auto& e : ev) {
                const auto row = static_cast<double>(std::find(voices.begin(), voices.end(), e.voice) - voices.begin());
                const int ya = y0 + 2 + static_cast<int>(row * rowH), yb = std::max(ya + 1, y0 + 2 + static_cast<int>((row + 1) * rowH) - 1);
                const int x = xOfBeat(e.beat);
                rect(im, x, ya, x + std::max(1, static_cast<int>(barW / 32)), yb, dim(c, 0.45 + 0.55 * e.velocity / 127.0));
            }
        } else {
            int lo = 127, hi = 0;
            for (const auto& e : ev) lo = std::min(lo, e.pitch), hi = std::max(hi, e.pitch);
            lo -= 1, hi += 1;
            const double rowH = static_cast<double>(y1 - y0 - 4) / std::max(1, hi - lo + 1);
            for (const auto& e : ev) {
                const int ya = y1 - 2 - static_cast<int>((e.pitch - lo + 1) * rowH);
                const int yb = std::max(ya + 1, y1 - 2 - static_cast<int>((e.pitch - lo) * rowH));
                const int xa = xOfBeat(e.beat), xb = std::max(xa + 1, xOfBeat(e.beat + e.length) - 1);
                rect(im, xa, ya, xb, yb, dim(c, 0.45 + 0.55 * e.velocity / 127.0));
            }
        }
    }
    return im;
}

Image drawPattern(const Song& s, const Pattern& p, TrackKind kind, int width) {
    width = std::clamp(width, 200, 4000);
    const double beats = p.bars * s.beatsInBar();
    const int steps = std::max(1, static_cast<int>(std::lround(beats * p.stepsPerBeat)));
    const int left = 10;
    const double stepW = static_cast<double>(width - 20) / steps;
    auto xOfBeat = [&](double beat) { return left + static_cast<int>(std::lround(beat * p.stepsPerBeat * stepW)); };
    const int laneH = 18;
    // step lanes (drums, or synth lanes named by note)
    const int nLanes = static_cast<int>(p.lanes.size());
    int lo = 127, hi = 0;
    for (const auto& n : p.notes) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
    const bool roll = !p.notes.empty();
    if (roll) lo = std::max(0, lo - 2), hi = std::min(127, hi + 2);
    const int rows = roll ? hi - lo + 1 : 0;
    const int rowH = roll ? std::clamp(360 / std::max(1, rows), 6, 14) : 0;
    const int top = 10;
    const int gridH = nLanes * laneH;
    const int rollTop = top + gridH + (nLanes ? 10 : 0);
    const int height = rollTop + rows * rowH + 10;
    Image im(width, std::max(60, height), kBg);
    const Color c = kind == TrackKind::Drum ? trackColor(0) : trackColor(1);
    // grid lanes
    for (int l = 0; l < nLanes; ++l) {
        const auto& steps_ = p.lanes[static_cast<std::size_t>(l)].second;
        const int y0 = top + l * laneH;
        for (int i = 0; i < steps; ++i) {
            const int xa = left + static_cast<int>(i * stepW) + 1, xb = left + static_cast<int>((i + 1) * stepW) - 1;
            const bool beatStart = i % p.stepsPerBeat == 0;
            Color cell = (i / p.stepsPerBeat) % 2 ? kLaneAlt : kLane;
            if (beatStart) cell = dim(Color{60, 64, 74, 255}, 1.0);
            const char ch = steps_.empty() ? '.' : steps_[static_cast<std::size_t>(i) % steps_.size()];
            const int vel = stepVelocity(ch);
            if (vel) cell = dim(c, 0.4 + 0.6 * vel / 127.0);
            else if (ch == '-' && kind != TrackKind::Drum) cell = dim(c, 0.3);
            rect(im, xa, y0 + 1, xb, y0 + laneH - 2, cell);
        }
    }
    if (roll) {
        for (int r = 0; r < rows; ++r) {
            const int midi = hi - r;
            const int y0 = rollTop + r * rowH;
            rect(im, left, y0, width - 10, y0 + rowH, blackKey(midi) ? kBlackKey : kLane);
            if (midi % 12 == 0) hline(im, y0 + rowH - 1, left, width - 10, kBar);  // under each C
        }
        for (int i = 0; i <= steps; ++i) {
            const int x = left + static_cast<int>(i * stepW);
            const bool bar = i % std::max(1, static_cast<int>(std::lround(s.beatsInBar() * p.stepsPerBeat))) == 0;
            if (bar || i % p.stepsPerBeat == 0) vline(im, x, rollTop, rollTop + rows * rowH, bar ? kBar : kBeat);
        }
        for (const auto& n : p.notes) {
            if (n.pitch < lo || n.pitch > hi) continue;
            const int y0 = rollTop + (hi - n.pitch) * rowH;
            const int xa = xOfBeat(n.start) + 1, xb = std::max(xa + 2, xOfBeat(std::min(beats, n.start + n.length)) - 1);
            rect(im, xa, y0 + 1, xb, y0 + rowH - 1, dim(c, 0.45 + 0.55 * n.velocity / 127.0));
            rect(im, xa, y0 + 1, xa + 2, y0 + rowH - 1, Color{255, 255, 255, 255});
        }
    }
    return im;
}

}  // namespace wetwaresuite::beats
