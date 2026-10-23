// Aseprite (.aseprite/.ase) files: header, frames, layer/cel/tag/palette
// chunks. Cels are written zlib-compressed (cel type 2), as Aseprite does.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "wetwaresuite/forge/sprite.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Defined (and exported) by stb_image_write inside libs/raster.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace wetwaresuite::forge {

namespace {

const char* kDirections[] = {"forward", "reverse", "pingpong", "pingpong_reverse"};

class Reader {
public:
    Reader(const std::string& d, std::size_t at = 0, std::size_t end = std::string::npos)
        : d_(d), at_(at), end_(std::min(end, d.size())) {}
    std::size_t pos() const { return at_; }
    void seek(std::size_t p) { at_ = p; }
    bool left(std::size_t n) const { return at_ + n <= end_; }
    std::uint8_t byte() {
        need(1);
        return static_cast<std::uint8_t>(d_[at_++]);
    }
    std::uint16_t word() {
        need(2);
        const auto v = static_cast<std::uint16_t>(static_cast<std::uint8_t>(d_[at_]) | static_cast<std::uint8_t>(d_[at_ + 1]) << 8);
        at_ += 2;
        return v;
    }
    std::int16_t shortv() { return static_cast<std::int16_t>(word()); }
    std::uint32_t dword() {
        const std::uint32_t lo = word();
        return lo | static_cast<std::uint32_t>(word()) << 16;
    }
    std::string str() {
        const std::uint16_t n = word();
        need(n);
        std::string s = d_.substr(at_, n);
        at_ += n;
        return s;
    }
    std::string bytes(std::size_t n) {
        need(n);
        std::string s = d_.substr(at_, n);
        at_ += n;
        return s;
    }
    void skip(std::size_t n) {
        need(n);
        at_ += n;
    }

private:
    void need(std::size_t n) const {
        if (at_ + n > end_) throw std::runtime_error("the Aseprite file is cut short or corrupt");
    }
    const std::string& d_;
    std::size_t at_, end_;
};

class Writer {
public:
    std::string out;
    void byte(int v) { out += static_cast<char>(v & 0xFF); }
    void word(int v) {
        byte(v);
        byte(v >> 8);
    }
    void dword(std::uint32_t v) {
        word(static_cast<int>(v & 0xFFFF));
        word(static_cast<int>(v >> 16));
    }
    void str(const std::string& s) {
        word(static_cast<int>(s.size()));
        out += s;
    }
    void zeros(int n) { out.append(static_cast<std::size_t>(n), '\0'); }
    void patch(std::size_t at, std::uint32_t v) {
        for (int b = 0; b < 4; ++b) out[at + static_cast<std::size_t>(b)] = static_cast<char>(v >> (8 * b) & 0xFF);
    }
};

std::string inflateZlib(const std::string& data, std::size_t expected) {
    int len = 0;
    char* raw = stbi_zlib_decode_malloc_guesssize_headerflag(data.data(), static_cast<int>(data.size()), static_cast<int>(expected + 64), &len, 1);
    if (!raw) throw std::runtime_error("corrupt compressed cel in the Aseprite file");
    std::string out(raw, static_cast<std::size_t>(len));
    std::free(raw);
    return out;
}

std::string deflateZlib(const std::string& data) {
    int len = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())), static_cast<int>(data.size()), &len, 8);
    if (!z) throw std::runtime_error("zlib compression failed");
    std::string out(reinterpret_cast<char*>(z), static_cast<std::size_t>(len));
    std::free(z);
    return out;
}

}  // namespace

Sprite readAseprite(const std::string& d) {
    Reader h(d);
    if (d.size() < 128) throw std::runtime_error("not an Aseprite file (too short)");
    h.dword();
    if (h.word() != 0xA5E0) throw std::runtime_error("not an Aseprite file (bad magic number)");
    const int frames = h.word();
    Sprite s;
    s.width = h.word();
    s.height = h.word();
    const int depth = h.word();
    if (depth != 32 && depth != 16 && depth != 8) throw std::runtime_error("unknown Aseprite colour depth " + std::to_string(depth));
    if (s.width <= 0 || s.height <= 0 || static_cast<long long>(s.width) * s.height > 64LL * 1024 * 1024) throw std::runtime_error("bad sprite size");
    const std::uint32_t flags = h.dword();
    const int speed = h.word();
    h.skip(8);
    s.transparentIndex = h.byte();
    s.indexed = depth == 8;
    h.seek(128);
    std::vector<Color> oldPalette;
    bool newPalette = false;
    const int bpp = depth / 8;
    auto pixel = [&](const std::uint8_t* p) -> Color {
        if (depth == 32) return {p[0], p[1], p[2], p[3]};
        if (depth == 16) return {p[0], p[0], p[0], p[1]};
        const int i = p[0];
        if (i == s.transparentIndex) return {0, 0, 0, 0};
        if (i < static_cast<int>(s.palette.size())) {
            Color c = s.palette[static_cast<std::size_t>(i)];
            return c;
        }
        return {0, 0, 0, 0};
    };
    // cels are decoded after all chunks of the file are read (the palette can come after the first cels)
    struct RawCel {
        int layer, frame, x, y, opacity, w, h;
        std::string pixels;
        int link = -1;
    };
    std::vector<RawCel> raw;
    for (int f = 0; f < frames; ++f) {
        const std::size_t start = h.pos();
        const std::uint32_t frameBytes = h.dword();
        if (h.word() != 0xF1FA) throw std::runtime_error("corrupt Aseprite frame " + std::to_string(f));
        const int oldChunks = h.word();
        const int duration = h.word();
        h.skip(2);
        const std::uint32_t newChunks = h.dword();
        s.durations.push_back(duration > 0 ? duration : (speed > 0 ? speed : 100));
        const std::uint32_t chunks = newChunks ? newChunks : static_cast<std::uint32_t>(oldChunks);
        for (std::uint32_t c = 0; c < chunks; ++c) {
            const std::size_t cstart = h.pos();
            const std::uint32_t size = h.dword();
            const int type = h.word();
            if (size < 6) throw std::runtime_error("corrupt Aseprite chunk");
            Reader r(d, h.pos(), cstart + size);
            if (type == 0x2004) {  // layer
                SpriteLayer l;
                const int lflags = r.word();
                const int ltype = r.word();
                l.level = r.word();
                r.skip(4);
                l.blend = r.word();
                l.opacity = (flags & 1) ? r.byte() : (r.skip(1), 255);
                r.skip(3);
                l.name = r.str();
                l.visible = (lflags & 1) != 0;
                l.group = ltype == 1;
                s.layers.push_back(l);
            } else if (type == 0x2005) {  // cel
                RawCel rc;
                rc.layer = r.word();
                rc.x = r.shortv();
                rc.y = r.shortv();
                rc.opacity = r.byte();
                const int ctype = r.word();
                r.skip(7);
                rc.frame = f;
                rc.w = rc.h = 0;
                if (ctype == 0 || ctype == 2) {
                    rc.w = r.word();
                    rc.h = r.word();
                    const std::size_t need = static_cast<std::size_t>(rc.w) * rc.h * static_cast<std::size_t>(bpp);
                    rc.pixels = ctype == 0 ? r.bytes(need) : inflateZlib(r.bytes(cstart + size - r.pos()), need);
                    if (rc.pixels.size() < need) throw std::runtime_error("cel data too short in the Aseprite file");
                } else if (ctype == 1) {
                    rc.link = r.word();
                } else {
                    continue;  // tilemap cels: not supported
                }
                raw.push_back(std::move(rc));
            } else if (type == 0x2018) {  // tags
                const int n = r.word();
                r.skip(8);
                for (int t = 0; t < n; ++t) {
                    SpriteTag tag;
                    tag.from = r.word();
                    tag.to = r.word();
                    const int dir = r.byte();
                    tag.direction = dir >= 0 && dir < 4 ? kDirections[dir] : "forward";
                    tag.repeat = r.word();
                    r.skip(6);
                    tag.color.r = r.byte();
                    tag.color.g = r.byte();
                    tag.color.b = r.byte();
                    r.skip(1);
                    tag.name = r.str();
                    s.tags.push_back(tag);
                }
            } else if (type == 0x2019) {  // palette
                newPalette = true;
                const std::uint32_t total = r.dword(), first = r.dword(), last = r.dword();
                r.skip(8);
                if (total > 65536 || last < first || last >= total) throw std::runtime_error("bad palette in the Aseprite file");
                s.palette.resize(total, Color{0, 0, 0, 255});
                for (std::uint32_t i = first; i <= last; ++i) {
                    const int pf = r.word();
                    Color col;
                    col.r = r.byte();
                    col.g = r.byte();
                    col.b = r.byte();
                    col.a = r.byte();
                    if (pf & 1) r.str();
                    s.palette[i] = col;
                }
            } else if (type == 0x0004 || type == 0x0011) {  // old palettes
                const int packets = r.word();
                int index = 0;
                for (int k = 0; k < packets; ++k) {
                    index += r.byte();
                    int n = r.byte();
                    if (n == 0) n = 256;
                    for (int i = 0; i < n; ++i, ++index) {
                        Color col{r.byte(), r.byte(), r.byte(), 255};
                        if (type == 0x0011) col = {static_cast<std::uint8_t>(col.r * 255 / 63), static_cast<std::uint8_t>(col.g * 255 / 63),
                                                   static_cast<std::uint8_t>(col.b * 255 / 63), 255};
                        if (static_cast<int>(oldPalette.size()) <= index) oldPalette.resize(static_cast<std::size_t>(index) + 1);
                        oldPalette[static_cast<std::size_t>(index)] = col;
                    }
                }
            }
            h.seek(cstart + size);
        }
        h.seek(start + frameBytes);
    }
    if (!newPalette) s.palette = oldPalette;
    if (s.layers.empty()) s.layers.push_back({"Layer 1"});
    s.cels.assign(s.layers.size(), std::vector<Image>(s.durations.size()));
    // plain cels first, then linked ones (they copy the cel of another frame)
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& rc : raw) {
            if (rc.layer < 0 || rc.layer >= static_cast<int>(s.layers.size())) continue;
            if ((rc.link >= 0) != (pass == 1)) continue;
            Image& dst = s.cels[static_cast<std::size_t>(rc.layer)][static_cast<std::size_t>(rc.frame)];
            if (rc.link >= 0) {
                if (rc.link < s.frames()) dst = s.cels[static_cast<std::size_t>(rc.layer)][static_cast<std::size_t>(rc.link)];
                continue;
            }
            dst = Image(s.width, s.height);
            const auto* px = reinterpret_cast<const std::uint8_t*>(rc.pixels.data());
            for (int y = 0; y < rc.h; ++y) {
                for (int x = 0; x < rc.w; ++x) {
                    const int dx = rc.x + x, dy = rc.y + y;
                    if (!dst.contains(dx, dy)) continue;
                    Color c = pixel(px + (static_cast<std::size_t>(y) * rc.w + x) * static_cast<std::size_t>(bpp));
                    if (rc.opacity < 255) c.a = static_cast<std::uint8_t>(c.a * rc.opacity / 255);
                    dst.set(dx, dy, c);
                }
            }
        }
    }
    return s;
}

std::string writeAseprite(const Sprite& s) {
    if (s.width <= 0 || s.height <= 0 || s.width > 65535 || s.height > 65535) throw std::runtime_error("bad sprite size");
    if (s.frames() > 65535 || s.layers.size() > 65535) throw std::runtime_error("too many frames or layers for an Aseprite file");
    const bool indexed = s.indexed && !s.palette.empty() && s.palette.size() <= 256;
    Writer w;
    w.dword(0);  // file size, patched at the end
    w.word(0xA5E0);
    w.word(s.frames());
    w.word(s.width);
    w.word(s.height);
    w.word(indexed ? 8 : 32);
    w.dword(1);  // layer opacity is valid
    w.word(s.durations.empty() ? 100 : s.durations[0]);
    w.dword(0);
    w.dword(0);
    w.byte(indexed ? s.transparentIndex : 0);
    w.zeros(3);
    w.word(static_cast<int>(s.palette.size() >= 256 ? 0 : s.palette.size()));
    w.byte(1);  // pixel width
    w.byte(1);  // pixel height
    w.word(0);
    w.word(0);
    w.word(16);
    w.word(16);
    w.zeros(84);
    // exact palette lookups for indexed pixels
    auto indexOf = [&](const std::uint8_t* p) {
        if (p[3] < 128) return s.transparentIndex;
        for (std::size_t i = 0; i < s.palette.size(); ++i) {
            const Color& c = s.palette[i];
            if (static_cast<int>(i) != s.transparentIndex && c.r == p[0] && c.g == p[1] && c.b == p[2]) return static_cast<int>(i);
        }
        return std::max(0, nearestIndex(s.palette, {p[0], p[1], p[2], 255}, s.transparentIndex));
    };
    for (int f = 0; f < s.frames(); ++f) {
        const std::size_t start = w.out.size();
        w.dword(0);
        w.word(0xF1FA);
        const std::size_t chunkCountAt = w.out.size();
        w.word(0);
        w.word(s.durations[static_cast<std::size_t>(f)]);
        w.zeros(2);
        w.dword(0);
        int chunks = 0;
        auto chunk = [&](int type, const std::string& body) {
            w.dword(static_cast<std::uint32_t>(body.size() + 6));
            w.word(type);
            w.out += body;
            ++chunks;
        };
        if (f == 0) {
            if (!s.palette.empty()) {
                Writer b;
                b.dword(static_cast<std::uint32_t>(s.palette.size()));
                b.dword(0);
                b.dword(static_cast<std::uint32_t>(s.palette.size() - 1));
                b.zeros(8);
                for (const auto& c : s.palette) {
                    b.word(0);
                    b.byte(c.r);
                    b.byte(c.g);
                    b.byte(c.b);
                    b.byte(c.a);
                }
                chunk(0x2019, b.out);
            }
            for (const auto& l : s.layers) {
                Writer b;
                b.word((l.visible ? 1 : 0) | 2);
                b.word(l.group ? 1 : 0);
                b.word(l.level);
                b.word(0);
                b.word(0);
                b.word(l.blend);
                b.byte(l.opacity);
                b.zeros(3);
                b.str(l.name);
                chunk(0x2004, b.out);
            }
            if (!s.tags.empty()) {
                Writer b;
                b.word(static_cast<int>(s.tags.size()));
                b.zeros(8);
                for (const auto& t : s.tags) {
                    b.word(t.from);
                    b.word(t.to);
                    int dir = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (t.direction == kDirections[k]) dir = k;
                    }
                    b.byte(dir);
                    b.word(t.repeat);
                    b.zeros(6);
                    b.byte(t.color.r);
                    b.byte(t.color.g);
                    b.byte(t.color.b);
                    b.byte(0);
                    b.str(t.name);
                }
                chunk(0x2018, b.out);
            }
        }
        for (std::size_t li = 0; li < s.layers.size(); ++li) {
            if (s.layers[li].group) continue;
            const Image* c = s.celIf(static_cast<int>(li), f);
            if (!c) continue;
            // only the cel's visible bounds are stored
            int x0 = c->width(), y0 = c->height(), x1 = -1, y1 = -1;
            for (int y = 0; y < c->height(); ++y) {
                for (int x = 0; x < c->width(); ++x) {
                    if (c->pixel(x, y)[3] == 0) continue;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
            if (x1 < 0) continue;
            std::string px;
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const std::uint8_t* p = c->pixel(x, y);
                    if (indexed) px += static_cast<char>(indexOf(p));
                    else px.append(reinterpret_cast<const char*>(p), 4);
                }
            }
            Writer b;
            b.word(static_cast<int>(li));
            b.word(x0);
            b.word(y0);
            b.byte(255);
            b.word(2);
            b.word(0);  // z-index
            b.zeros(5);
            b.word(x1 - x0 + 1);
            b.word(y1 - y0 + 1);
            b.out += deflateZlib(px);
            chunk(0x2005, b.out);
        }
        w.out[chunkCountAt] = static_cast<char>(std::min(chunks, 0xFFFF) & 0xFF);
        w.out[chunkCountAt + 1] = static_cast<char>(std::min(chunks, 0xFFFF) >> 8 & 0xFF);
        w.patch(chunkCountAt + 6, static_cast<std::uint32_t>(chunks));
        w.patch(start, static_cast<std::uint32_t>(w.out.size() - start));
    }
    w.patch(0, static_cast<std::uint32_t>(w.out.size()));
    return w.out;
}

}  // namespace wetwaresuite::forge
