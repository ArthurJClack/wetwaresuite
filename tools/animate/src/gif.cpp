// A small animated GIF encoder: per-frame median-cut palettes over a 5-bit
// per channel histogram, optional ordered dithering, one transparent index
// for pixels under half alpha, LZW with the usual 12-bit table reset.
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

#include "achclwss/animate/animate.hpp"

namespace achclwss::animate {

namespace {

struct Bin {
    std::uint32_t count = 0;
    std::uint64_t r = 0, g = 0, b = 0;
};

struct Box {
    std::vector<int> bins;  // indices into the histogram (15-bit colour codes)
    std::uint64_t weight = 0;
};

int channel(int code, int axis) { return axis == 0 ? (code >> 10) & 31 : axis == 1 ? (code >> 5) & 31 : code & 31; }

const int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

int quantCode(const std::uint8_t* p, int x, int y, bool dither) {
    int r = p[0], g = p[1], b = p[2];
    if (dither) {
        const int d = (kBayer[y & 3][x & 3] - 8);  // -8..7, about one 5-bit step
        r = std::clamp(r + d, 0, 255);
        g = std::clamp(g + d, 0, 255);
        b = std::clamp(b + d, 0, 255);
    }
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

// Palette (up to maxColors) and the index of every pixel.
void quantize(const Image& img, bool dither, int maxColors, std::vector<std::array<std::uint8_t, 3>>& palette,
              std::vector<std::uint8_t>& indices, bool& hasTransparent) {
    std::vector<Bin> hist(32768);
    const int W = img.width(), H = img.height();
    std::vector<int> codes(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), -1);
    hasTransparent = false;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::uint8_t* p = img.pixel(x, y);
            const std::size_t k = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
            if (p[3] < 128) {
                hasTransparent = true;
                continue;
            }
            const int c = quantCode(p, x, y, dither);
            codes[k] = c;
            Bin& b = hist[static_cast<std::size_t>(c)];
            ++b.count;
            b.r += p[0];
            b.g += p[1];
            b.b += p[2];
        }
    }
    const int limit = std::max(1, maxColors - (hasTransparent ? 1 : 0));
    std::vector<Box> boxes(1);
    for (int c = 0; c < 32768; ++c) {
        if (hist[static_cast<std::size_t>(c)].count) {
            boxes[0].bins.push_back(c);
            boxes[0].weight += hist[static_cast<std::size_t>(c)].count;
        }
    }
    while (static_cast<int>(boxes.size()) < limit) {
        // split the box with the biggest weighted spread
        int best = -1, bestAxis = 0;
        double bestScore = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].bins.size() < 2) continue;
            for (int axis = 0; axis < 3; ++axis) {
                int lo = 31, hi = 0;
                for (int c : boxes[i].bins) {
                    lo = std::min(lo, channel(c, axis));
                    hi = std::max(hi, channel(c, axis));
                }
                const double score = static_cast<double>(hi - lo) * static_cast<double>(boxes[i].weight);
                if (score > bestScore) {
                    bestScore = score;
                    best = static_cast<int>(i);
                    bestAxis = axis;
                }
            }
        }
        if (best < 0) break;
        Box& b = boxes[static_cast<std::size_t>(best)];
        std::sort(b.bins.begin(), b.bins.end(), [&](int x, int y) { return channel(x, bestAxis) < channel(y, bestAxis); });
        std::uint64_t acc = 0;
        std::size_t cut = 1;
        for (std::size_t i = 0; i < b.bins.size(); ++i) {
            acc += hist[static_cast<std::size_t>(b.bins[i])].count;
            if (acc * 2 >= b.weight) {
                cut = std::clamp<std::size_t>(i + 1, 1, b.bins.size() - 1);
                break;
            }
        }
        Box nb;
        nb.bins.assign(b.bins.begin() + static_cast<std::ptrdiff_t>(cut), b.bins.end());
        b.bins.resize(cut);
        b.weight = 0;
        for (int c : b.bins) b.weight += hist[static_cast<std::size_t>(c)].count;
        for (int c : nb.bins) nb.weight += hist[static_cast<std::size_t>(c)].count;
        boxes.push_back(std::move(nb));
    }
    palette.clear();
    std::vector<int> lookup(32768, 0);
    for (const auto& b : boxes) {
        if (b.bins.empty()) continue;
        std::uint64_t r = 0, g = 0, bl = 0, n = 0;
        for (int c : b.bins) {
            const Bin& h = hist[static_cast<std::size_t>(c)];
            r += h.r;
            g += h.g;
            bl += h.b;
            n += h.count;
        }
        const int idx = static_cast<int>(palette.size());
        palette.push_back({static_cast<std::uint8_t>(r / n), static_cast<std::uint8_t>(g / n), static_cast<std::uint8_t>(bl / n)});
        for (int c : b.bins) lookup[static_cast<std::size_t>(c)] = idx;
    }
    if (palette.empty()) palette.push_back({0, 0, 0});
    const int transparent = static_cast<int>(palette.size());
    if (hasTransparent) palette.push_back({0, 0, 0});
    indices.resize(codes.size());
    for (std::size_t k = 0; k < codes.size(); ++k) {
        indices[k] = static_cast<std::uint8_t>(codes[k] < 0 ? transparent : lookup[static_cast<std::size_t>(codes[k])]);
    }
}

class BitWriter {
public:
    explicit BitWriter(std::string& out) : out_(out) {}
    void write(int code, int bits) {
        acc_ |= static_cast<std::uint32_t>(code) << n_;
        n_ += bits;
        while (n_ >= 8) {
            byte(static_cast<std::uint8_t>(acc_ & 0xff));
            acc_ >>= 8;
            n_ -= 8;
        }
    }
    void finish() {
        if (n_ > 0) byte(static_cast<std::uint8_t>(acc_ & 0xff));
        acc_ = 0;
        n_ = 0;
        flushBlock();
        out_ += '\0';
    }

private:
    void byte(std::uint8_t b) {
        block_ += static_cast<char>(b);
        if (block_.size() == 255) flushBlock();
    }
    void flushBlock() {
        if (block_.empty()) return;
        out_ += static_cast<char>(block_.size());
        out_ += block_;
        block_.clear();
    }
    std::string& out_;
    std::string block_;
    std::uint32_t acc_ = 0;
    int n_ = 0;
};

void lzw(std::string& out, const std::vector<std::uint8_t>& px, int minCodeSize) {
    out += static_cast<char>(minCodeSize);
    BitWriter w(out);
    const int clear = 1 << minCodeSize, eoi = clear + 1;
    std::vector<std::int16_t> table(4096 * 256, -1);  // (prefix code, byte) -> code
    int next = eoi + 1, size = minCodeSize + 1;
    auto reset = [&] {
        std::fill(table.begin(), table.end(), static_cast<std::int16_t>(-1));
        next = eoi + 1;
        size = minCodeSize + 1;
    };
    w.write(clear, size);
    if (px.empty()) {
        w.write(eoi, size);
        w.finish();
        return;
    }
    int prefix = px[0];
    for (std::size_t i = 1; i < px.size(); ++i) {
        const int c = px[i];
        const std::size_t slot = static_cast<std::size_t>(prefix) * 256 + static_cast<std::size_t>(c);
        if (table[slot] >= 0) {
            prefix = table[slot];
            continue;
        }
        w.write(prefix, size);
        if (next < 4095) {
            table[slot] = static_cast<std::int16_t>(next);
            if (next == (1 << size) && size < 12) ++size;
            ++next;
        } else {
            w.write(clear, size);
            reset();
        }
        prefix = c;
    }
    w.write(prefix, size);
    w.write(eoi, size);
    w.finish();
}

void u16(std::string& s, int v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>((v >> 8) & 0xff);
}

}  // namespace

std::string encodeGif(const std::vector<Image>& frames, const std::vector<int>& delays, int loop, bool dither) {
    if (frames.empty()) throw std::runtime_error("a GIF needs at least one frame");
    const int W = frames[0].width(), H = frames[0].height();
    if (W < 1 || H < 1 || W > 65535 || H > 65535) throw std::runtime_error("GIF frames must be 1..65535 px");
    std::string out = "GIF89a";
    u16(out, W);
    u16(out, H);
    out += static_cast<char>(0x70);  // no global table, 8-bit colour resolution
    out += '\0';
    out += '\0';
    if (frames.size() > 1) {
        out += "\x21\xff\x0bNETSCAPE2.0\x03\x01";
        u16(out, std::clamp(loop, 0, 65535));
        out += '\0';
    }
    std::vector<std::array<std::uint8_t, 3>> palette;
    std::vector<std::uint8_t> indices;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const Image& f = frames[i];
        if (f.width() != W || f.height() != H) throw std::runtime_error("every GIF frame must be the same size");
        bool transparent = false;
        quantize(f, dither, 256, palette, indices, transparent);
        // graphic control: restore to background between frames when there is transparency
        out += "\x21\xf9\x04";
        out += static_cast<char>((transparent ? (2 << 2) | 1 : (1 << 2)));
        u16(out, std::clamp(i < delays.size() ? delays[i] : 4, 0, 65535));
        out += static_cast<char>(transparent ? palette.size() - 1 : 0);
        out += '\0';
        // image descriptor with a local colour table
        out += '\x2c';
        u16(out, 0);
        u16(out, 0);
        u16(out, W);
        u16(out, H);
        int bits = 1;
        while ((1 << bits) < static_cast<int>(palette.size())) ++bits;
        out += static_cast<char>(0x80 | (bits - 1));
        for (int k = 0; k < (1 << bits); ++k) {
            const auto& c = k < static_cast<int>(palette.size()) ? palette[static_cast<std::size_t>(k)] : std::array<std::uint8_t, 3>{0, 0, 0};
            out += static_cast<char>(c[0]);
            out += static_cast<char>(c[1]);
            out += static_cast<char>(c[2]);
        }
        lzw(out, indices, std::max(2, bits));
    }
    out += '\x3b';
    return out;
}

}  // namespace achclwss::animate
