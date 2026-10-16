// Raster layers on disk: images with world files, and a GeoTIFF reader/writer
// (baseline TIFF strips or tiles; none, LZW, Deflate or PackBits; predictors;
// 8/16/32-bit integers and 32/64-bit floats; GeoKeys for EPSG codes).
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>

#include "achclwss/pdf/page.hpp"
#include "achclwss/raster/codec.hpp"
#include "internal.hpp"

namespace achclwss::maps {

namespace fs = std::filesystem;
using namespace detail;

// ---- world files ------------------------------------------------------------------------

std::optional<Affine> parseWorldFile(const std::string& text) {
    std::istringstream in(text);
    double v[6];
    for (double& x : v) {
        std::string tok;
        if (!(in >> tok)) return std::nullopt;
        std::replace(tok.begin(), tok.end(), ',', '.');
        char* end = nullptr;
        x = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str()) return std::nullopt;
    }
    // A D B E C F, C/F at the centre of the top-left pixel
    Affine g{v[0], v[2], v[4], v[1], v[3], v[5]};
    g.c -= (g.a + g.b) / 2;
    g.f -= (g.d + g.e) / 2;
    return g;
}

std::string worldFileText(const Affine& g) {
    const double c = g.c + (g.a + g.b) / 2, f = g.f + (g.d + g.e) / 2;
    return fmt(g.a, 12) + "\n" + fmt(g.d, 12) + "\n" + fmt(g.b, 12) + "\n" + fmt(g.e, 12) + "\n" + fmt(c, 10) + "\n" + fmt(f, 10) + "\n";
}

// ---- TIFF -------------------------------------------------------------------------------------

namespace {

struct Tiff {
    const std::string& b;
    bool le = true;
    std::uint32_t u8(std::size_t o) const { return o < b.size() ? static_cast<unsigned char>(b[o]) : 0u; }
    std::uint32_t u16(std::size_t o) const { return le ? (u8(o) | (u8(o + 1) << 8)) : ((u8(o) << 8) | u8(o + 1)); }
    std::uint32_t u32(std::size_t o) const {
        return le ? (u8(o) | (u8(o + 1) << 8) | (u8(o + 2) << 16) | (u8(o + 3) << 24)) : ((u8(o) << 24) | (u8(o + 1) << 16) | (u8(o + 2) << 8) | u8(o + 3));
    }
    std::uint64_t u64(std::size_t o) const {
        const std::uint64_t a = u32(o), c = u32(o + 4);
        return le ? (a | (c << 32)) : ((a << 32) | c);
    }
    double f64(std::size_t o) const {
        const std::uint64_t v = u64(o);
        double d = 0;
        std::memcpy(&d, &v, 8);
        return d;
    }
};

struct Entry {
    std::uint32_t type = 0, count = 0;
    std::size_t offset = 0;  // where the values are
};

int typeSize(std::uint32_t t) {
    switch (t) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

std::vector<double> values(const Tiff& t, const Entry& e) {
    std::vector<double> out;
    const int sz = typeSize(e.type);
    for (std::uint32_t i = 0; i < e.count && sz; ++i) {
        const std::size_t o = e.offset + static_cast<std::size_t>(i) * sz;
        switch (e.type) {
            case 1: case 7: out.push_back(t.u8(o)); break;
            case 6: out.push_back(static_cast<std::int8_t>(t.u8(o))); break;
            case 3: out.push_back(t.u16(o)); break;
            case 8: out.push_back(static_cast<std::int16_t>(t.u16(o))); break;
            case 4: out.push_back(t.u32(o)); break;
            case 9: out.push_back(static_cast<std::int32_t>(t.u32(o))); break;
            case 5: out.push_back(t.u32(o + 4) ? static_cast<double>(t.u32(o)) / t.u32(o + 4) : 0); break;
            case 10: out.push_back(t.u32(o + 4) ? static_cast<double>(static_cast<std::int32_t>(t.u32(o))) / static_cast<std::int32_t>(t.u32(o + 4)) : 0); break;
            case 11: {
                const std::uint32_t u = t.u32(o);
                float f = 0;
                std::memcpy(&f, &u, 4);
                out.push_back(f);
                break;
            }
            case 12: out.push_back(t.f64(o)); break;
            default: break;
        }
    }
    return out;
}

std::string lzwDecode(const std::string& in, std::size_t expected) {
    std::string out;
    out.reserve(expected);
    std::vector<std::string> dict;
    auto reset = [&] {
        dict.clear();
        for (int i = 0; i < 256; ++i) dict.emplace_back(1, static_cast<char>(i));
        dict.emplace_back();  // 256 clear
        dict.emplace_back();  // 257 end
    };
    reset();
    std::size_t bitPos = 0;
    int width = 9;
    std::string prev;
    bool havePrev = false;
    const std::size_t totalBits = in.size() * 8;
    while (bitPos + static_cast<std::size_t>(width) <= totalBits) {
        std::uint32_t code = 0;
        for (int k = 0; k < width; ++k) {
            const std::size_t bit = bitPos + static_cast<std::size_t>(k);
            code = (code << 1) | ((static_cast<unsigned char>(in[bit / 8]) >> (7 - bit % 8)) & 1u);
        }
        bitPos += static_cast<std::size_t>(width);
        if (code == 256) {
            reset();
            width = 9;
            havePrev = false;
            continue;
        }
        if (code == 257) break;
        std::string entry;
        if (code < dict.size()) {
            entry = dict[code];
            if (havePrev) dict.push_back(prev + entry[0]);
        } else if (havePrev && code == dict.size()) {
            entry = prev + prev[0];
            dict.push_back(entry);
        } else {
            throw std::runtime_error("TIFF: corrupt LZW data");
        }
        out += entry;
        prev = entry;
        havePrev = true;
        // TIFF's "early change": widen one code before the table fills
        if (dict.size() + 1 >= (1u << width) && width < 12) ++width;
        if (out.size() >= expected) break;
    }
    return out;
}

std::string packBitsDecode(const std::string& in, std::size_t expected) {
    std::string out;
    std::size_t i = 0;
    while (i < in.size() && out.size() < expected) {
        const auto n = static_cast<std::int8_t>(in[i++]);
        if (n >= 0) {
            const std::size_t len = static_cast<std::size_t>(n) + 1;
            out += in.substr(i, len);
            i += len;
        } else if (n != -128) {
            if (i < in.size()) out.append(static_cast<std::size_t>(1 - n), in[i]);
            ++i;
        }
    }
    return out;
}

}  // namespace

std::shared_ptr<Grid> readGeoTiff(const std::string& bytes, std::vector<std::string>* warnings, bool* georeferenced) {
    if (bytes.size() < 8) throw std::runtime_error("not a TIFF file");
    Tiff t{bytes, bytes[0] == 'I'};
    if (!((bytes[0] == 'I' && bytes[1] == 'I') || (bytes[0] == 'M' && bytes[1] == 'M'))) throw std::runtime_error("not a TIFF file");
    const std::uint32_t magic = t.u16(2);
    if (magic == 43) throw std::runtime_error("BigTIFF files aren't supported (convert with gdal_translate -co BIGTIFF=NO)");
    if (magic != 42) throw std::runtime_error("not a TIFF file");
    const std::size_t ifd = t.u32(4);
    const std::uint32_t n = t.u16(ifd);
    std::map<std::uint32_t, Entry> tags;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t e = ifd + 2 + static_cast<std::size_t>(i) * 12;
        Entry en;
        const std::uint32_t tag = t.u16(e);
        en.type = t.u16(e + 2);
        en.count = t.u32(e + 4);
        const std::size_t total = static_cast<std::size_t>(typeSize(en.type)) * en.count;
        en.offset = total <= 4 ? e + 8 : t.u32(e + 8);
        if (en.offset + total > bytes.size()) continue;
        tags[tag] = en;
    }
    auto one = [&](std::uint32_t tag, double fallback) {
        auto it = tags.find(tag);
        if (it == tags.end()) return fallback;
        auto v = values(t, it->second);
        return v.empty() ? fallback : v[0];
    };
    auto list = [&](std::uint32_t tag) {
        auto it = tags.find(tag);
        return it == tags.end() ? std::vector<double>{} : values(t, it->second);
    };
    const int width = static_cast<int>(one(256, 0)), height = static_cast<int>(one(257, 0));
    if (width <= 0 || height <= 0 || static_cast<double>(width) * height > 2.0e8) throw std::runtime_error("TIFF: bad or too large image size");
    const int spp = static_cast<int>(one(277, 1));
    const auto bpsList = list(258);
    const int bps = bpsList.empty() ? 1 : static_cast<int>(bpsList[0]);
    const int compression = static_cast<int>(one(259, 1));
    const int photometric = static_cast<int>(one(262, spp >= 3 ? 2 : 1));
    const int planar = static_cast<int>(one(284, 1));
    const int predictor = static_cast<int>(one(317, 1));
    const int sampleFormat = static_cast<int>(one(339, 1));
    if (planar != 1 && spp > 1) throw std::runtime_error("TIFF: planar (band-separate) layout isn't supported");
    if (bps != 8 && bps != 16 && bps != 32 && bps != 64) throw std::runtime_error("TIFF: " + std::to_string(bps) + "-bit samples aren't supported");
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32946 && compression != 32773)
        throw std::runtime_error("TIFF: compression " + std::to_string(compression) + " isn't supported (use none, LZW or Deflate)");
    const bool tiled = tags.count(322) > 0;
    const int tw = tiled ? static_cast<int>(one(322, width)) : width;
    const int th = tiled ? static_cast<int>(one(323, height)) : static_cast<int>(std::min<double>(one(278, height), height));
    const auto offsets = list(tiled ? 324 : 273), counts = list(tiled ? 325 : 279);
    if (offsets.empty() || tw <= 0 || th <= 0) throw std::runtime_error("TIFF: no image data");
    const int bytesPer = bps / 8;
    const std::size_t pixelBytes = static_cast<std::size_t>(spp) * bytesPer;
    const int tilesAcross = (width + tw - 1) / tw;
    std::string data(static_cast<std::size_t>(width) * height * pixelBytes, '\0');
    for (std::size_t k = 0; k < offsets.size(); ++k) {
        const std::size_t off = static_cast<std::size_t>(offsets[k]);
        const std::size_t cnt = k < counts.size() ? static_cast<std::size_t>(counts[k]) : bytes.size() - off;
        if (off >= bytes.size()) continue;
        const std::string raw = bytes.substr(off, std::min(cnt, bytes.size() - off));
        const std::size_t expected = static_cast<std::size_t>(tw) * th * pixelBytes;
        std::string dec;
        if (compression == 1) dec = raw;
        else if (compression == 5) dec = lzwDecode(raw, expected);
        else if (compression == 32773) dec = packBitsDecode(raw, expected);
        else dec = pdf::inflate(raw);
        dec.resize(expected, '\0');
        const std::size_t rowBytes = static_cast<std::size_t>(tw) * pixelBytes;
        // undo predictors row by row
        for (int r = 0; r < th; ++r) {
            char* row = dec.data() + static_cast<std::size_t>(r) * rowBytes;
            if (predictor == 2) {
                for (int x = 1; x < tw; ++x) {
                    for (int s = 0; s < spp; ++s) {
                        const std::size_t cur = (static_cast<std::size_t>(x) * spp + s) * bytesPer, prev = cur - pixelBytes;
                        if (bytesPer == 1) {
                            row[cur] = static_cast<char>(static_cast<unsigned char>(row[cur]) + static_cast<unsigned char>(row[prev]));
                        } else if (bytesPer == 2) {
                            const std::uint32_t a = t.le ? (static_cast<unsigned char>(row[cur]) | (static_cast<unsigned char>(row[cur + 1]) << 8))
                                                         : ((static_cast<unsigned char>(row[cur]) << 8) | static_cast<unsigned char>(row[cur + 1]));
                            const std::uint32_t b = t.le ? (static_cast<unsigned char>(row[prev]) | (static_cast<unsigned char>(row[prev + 1]) << 8))
                                                         : ((static_cast<unsigned char>(row[prev]) << 8) | static_cast<unsigned char>(row[prev + 1]));
                            const std::uint32_t v = (a + b) & 0xFFFF;
                            row[cur + (t.le ? 0 : 1)] = static_cast<char>(v & 0xFF);
                            row[cur + (t.le ? 1 : 0)] = static_cast<char>(v >> 8);
                        } else if (bytesPer == 4) {
                            std::uint32_t a = 0, b = 0;
                            for (int q = 0; q < 4; ++q) {
                                const int sh = t.le ? 8 * q : 8 * (3 - q);
                                a |= static_cast<std::uint32_t>(static_cast<unsigned char>(row[cur + static_cast<std::size_t>(q)])) << sh;
                                b |= static_cast<std::uint32_t>(static_cast<unsigned char>(row[prev + static_cast<std::size_t>(q)])) << sh;
                            }
                            const std::uint32_t v = a + b;
                            for (int q = 0; q < 4; ++q) row[cur + static_cast<std::size_t>(q)] = static_cast<char>((v >> (t.le ? 8 * q : 8 * (3 - q))) & 0xFF);
                        }
                    }
                }
            } else if (predictor == 3) {
                // floating point: byte-wise differences, then bytes split by significance
                for (std::size_t i = static_cast<std::size_t>(spp); i < rowBytes; ++i)
                    row[i] = static_cast<char>(static_cast<unsigned char>(row[i]) + static_cast<unsigned char>(row[i - static_cast<std::size_t>(spp)]));
                std::string tmp(row, rowBytes);
                const std::size_t words = rowBytes / bytesPer;
                for (std::size_t w = 0; w < words; ++w) {
                    for (int q = 0; q < bytesPer; ++q) {
                        // tmp holds the most significant bytes first; write in the file's byte order
                        const char byte = tmp[static_cast<std::size_t>(q) * words + w];
                        const int pos = t.le ? bytesPer - 1 - q : q;
                        row[w * bytesPer + static_cast<std::size_t>(pos)] = byte;
                    }
                }
            }
        }
        // place the strip/tile
        const int tx = tiled ? static_cast<int>(k % static_cast<std::size_t>(tilesAcross)) * tw : 0;
        const int ty = tiled ? static_cast<int>(k / static_cast<std::size_t>(tilesAcross)) * th : static_cast<int>(k) * th;
        for (int r = 0; r < th && ty + r < height; ++r) {
            const int w = std::min(tw, width - tx);
            if (w <= 0) break;
            std::memcpy(data.data() + (static_cast<std::size_t>(ty + r) * width + tx) * pixelBytes, dec.data() + static_cast<std::size_t>(r) * rowBytes,
                        static_cast<std::size_t>(w) * pixelBytes);
        }
    }
    auto sampleAt = [&](std::size_t i, int s) -> double {
        const std::size_t o = i * pixelBytes + static_cast<std::size_t>(s) * bytesPer;
        Tiff d{data, t.le};
        switch (bytesPer) {
            case 1: return sampleFormat == 2 ? static_cast<double>(static_cast<std::int8_t>(d.u8(o))) : d.u8(o);
            case 2: return sampleFormat == 2 ? static_cast<double>(static_cast<std::int16_t>(d.u16(o))) : d.u16(o);
            case 4: {
                const std::uint32_t u = d.u32(o);
                if (sampleFormat == 3) {
                    float f = 0;
                    std::memcpy(&f, &u, 4);
                    return f;
                }
                return sampleFormat == 2 ? static_cast<double>(static_cast<std::int32_t>(u)) : u;
            }
            default: return sampleFormat == 3 ? d.f64(o) : static_cast<double>(d.u64(o));
        }
    };
    auto g = std::make_shared<Grid>();
    g->width = width;
    g->height = height;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    // nodata (GDAL)
    if (auto it = tags.find(42113); it != tags.end()) {
        std::string s(bytes.data() + it->second.offset, it->second.count);
        s = s.substr(0, s.find('\0'));
        if (!trim(s).empty() && lower(trim(s)) != "nan") g->nodata = std::strtod(s.c_str(), nullptr);
    }
    if (photometric == 3 && spp == 1) {
        const auto map = list(320);
        const std::size_t ncol = std::size_t{1} << bps;
        g->image = raster::Image(width, height);
        for (std::size_t i = 0; i < count; ++i) {
            const auto idx = static_cast<std::size_t>(sampleAt(i, 0));
            if (map.size() >= 3 * ncol && idx < ncol) {
                g->image.set(static_cast<int>(i % width), static_cast<int>(i / width),
                             {static_cast<std::uint8_t>(static_cast<int>(map[idx]) >> 8), static_cast<std::uint8_t>(static_cast<int>(map[ncol + idx]) >> 8),
                              static_cast<std::uint8_t>(static_cast<int>(map[2 * ncol + idx]) >> 8), 255});
            }
        }
    } else if (spp >= 3 && photometric == 2) {
        g->image = raster::Image(width, height);
        const double scale = bps == 8 ? 1.0 : bps == 16 ? 1.0 / 257 : 255.0;
        for (std::size_t i = 0; i < count; ++i) {
            auto ch = [&](int s) { return static_cast<std::uint8_t>(std::clamp(sampleAt(i, s) * scale, 0.0, 255.0)); };
            g->image.set(static_cast<int>(i % width), static_cast<int>(i / width), {ch(0), ch(1), ch(2), spp >= 4 ? ch(3) : std::uint8_t{255}});
        }
    } else {
        g->values.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            double v = sampleAt(i, 0);
            if (photometric == 0 && sampleFormat != 3) v = (bps == 8 ? 255 : 65535) - v;
            g->values[i] = static_cast<float>(v);
        }
        g->updateRange();
        if (bps == 8 && sampleFormat != 3) {
            // plain greyscale pictures show as they are
            g->image = raster::Image(width, height);
            for (std::size_t i = 0; i < count; ++i) {
                const auto v = static_cast<std::uint8_t>(g->values[i]);
                const bool nd = !std::isnan(g->nodata) && g->values[i] == static_cast<float>(g->nodata);
                g->image.set(static_cast<int>(i % width), static_cast<int>(i / width), {v, v, v, static_cast<std::uint8_t>(nd ? 0 : 255)});
            }
        }
    }
    // georeferencing
    bool placed = false;
    const auto scale = list(33550), tie = list(33922), transform = list(34264);
    const auto keys = list(34735);
    int modelType = 0, rasterType = 1, epsg = 0;
    for (std::size_t k = 4; k + 3 < keys.size(); k += 4) {
        const int id = static_cast<int>(keys[k]), loc = static_cast<int>(keys[k + 1]), val = static_cast<int>(keys[k + 3]);
        if (loc != 0) continue;
        if (id == 1024) modelType = val;
        else if (id == 1025) rasterType = val;
        else if (id == 3072 && val != 32767) epsg = val;
        else if (id == 2048 && val != 32767 && modelType != 1 && !epsg) epsg = val;
    }
    if (transform.size() >= 16) {
        g->geo = {transform[0], transform[1], transform[3], transform[4], transform[5], transform[7]};
        placed = true;
    } else if (scale.size() >= 2 && tie.size() >= 6) {
        g->geo = {scale[0], 0, tie[3] - tie[0] * scale[0], 0, -scale[1], tie[4] + tie[1] * scale[1]};
        placed = true;
    }
    if (placed && rasterType == 2) {
        g->geo.c -= (g->geo.a + g->geo.b) / 2;
        g->geo.f -= (g->geo.d + g->geo.e) / 2;
    }
    if (epsg) {
        if (auto c = Crs::parse("EPSG:" + std::to_string(epsg))) g->crs = *c;
        else if (warnings) warnings->push_back("GeoTIFF uses EPSG:" + std::to_string(epsg) + ", which maps can't reproject; read as WGS84 (pass crs=)");
    } else if (placed && modelType == 1 && warnings) {
        warnings->push_back("GeoTIFF has a projected CRS without an EPSG code; read as WGS84 (pass crs=)");
    }
    if (georeferenced) *georeferenced = placed;
    if (!placed) g->geo = {1.0 / width, 0, 0, 0, -1.0 / height, 1};
    return g;
}

std::string encodeGeoTiff(const Grid& g) {
    const bool values = g.hasValues();
    const int spp = values ? 1 : 4, bps = values ? 32 : 8;
    std::string pixels;
    const std::size_t count = static_cast<std::size_t>(g.width) * g.height;
    if (values) {
        pixels.resize(count * 4);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t u = 0;
            std::memcpy(&u, &g.values[i], 4);
            for (int k = 0; k < 4; ++k) pixels[i * 4 + static_cast<std::size_t>(k)] = static_cast<char>((u >> (8 * k)) & 0xFF);
        }
    } else {
        pixels.assign(reinterpret_cast<const char*>(g.image.data()), g.image.bytes());
    }
    const std::string strip = pdf::deflate(pixels);
    struct Tag {
        std::uint16_t id, type;
        std::uint32_t count;
        std::string data;  // raw little-endian values
    };
    auto shorts = [](std::vector<std::uint16_t> v) {
        std::string s;
        for (auto x : v) {
            s += static_cast<char>(x & 0xFF);
            s += static_cast<char>(x >> 8);
        }
        return s;
    };
    auto longs = [](std::vector<std::uint32_t> v) {
        std::string s;
        for (auto x : v) {
            for (int k = 0; k < 4; ++k) s += static_cast<char>((x >> (8 * k)) & 0xFF);
        }
        return s;
    };
    auto doubles = [](std::vector<double> v) {
        std::string s;
        for (double d : v) {
            std::uint64_t u = 0;
            std::memcpy(&u, &d, 8);
            for (int k = 0; k < 8; ++k) s += static_cast<char>((u >> (8 * k)) & 0xFF);
        }
        return s;
    };
    std::vector<Tag> tags;
    tags.push_back({256, 4, 1, longs({static_cast<std::uint32_t>(g.width)})});
    tags.push_back({257, 4, 1, longs({static_cast<std::uint32_t>(g.height)})});
    tags.push_back({258, 3, static_cast<std::uint32_t>(spp), shorts(std::vector<std::uint16_t>(static_cast<std::size_t>(spp), static_cast<std::uint16_t>(bps)))});
    tags.push_back({259, 3, 1, shorts({8})});
    tags.push_back({262, 3, 1, shorts({static_cast<std::uint16_t>(values ? 1 : 2)})});
    tags.push_back({273, 4, 1, longs({0})});  // patched
    tags.push_back({277, 3, 1, shorts({static_cast<std::uint16_t>(spp)})});
    tags.push_back({278, 4, 1, longs({static_cast<std::uint32_t>(g.height)})});
    tags.push_back({279, 4, 1, longs({static_cast<std::uint32_t>(strip.size())})});
    tags.push_back({284, 3, 1, shorts({1})});
    if (!values) tags.push_back({338, 3, 1, shorts({2})});
    tags.push_back({339, 3, static_cast<std::uint32_t>(spp), shorts(std::vector<std::uint16_t>(static_cast<std::size_t>(spp), static_cast<std::uint16_t>(values ? 3 : 1)))});
    const bool rotated = g.geo.b != 0 || g.geo.d != 0;
    if (rotated) {
        tags.push_back({34264, 12, 16, doubles({g.geo.a, g.geo.b, 0, g.geo.c, g.geo.d, g.geo.e, 0, g.geo.f, 0, 0, 0, 0, 0, 0, 0, 1})});
    } else {
        tags.push_back({33550, 12, 3, doubles({g.geo.a, -g.geo.e, 0})});
        tags.push_back({33922, 12, 6, doubles({0, 0, 0, g.geo.c, g.geo.f, 0})});
    }
    const int epsg = g.crs.epsg();
    std::vector<std::uint16_t> keys = {1, 1, 0, 0};
    keys.insert(keys.end(), {1024, 0, 1, static_cast<std::uint16_t>(g.crs.geographic() ? 2 : 1)});
    keys.insert(keys.end(), {1025, 0, 1, 1});
    if (epsg) keys.insert(keys.end(), {static_cast<std::uint16_t>(g.crs.geographic() ? 2048 : 3072), 0, 1, static_cast<std::uint16_t>(epsg)});
    keys[3] = static_cast<std::uint16_t>((keys.size() - 4) / 4);
    tags.push_back({34735, 3, static_cast<std::uint32_t>(keys.size()), shorts(keys)});
    if (values && !std::isnan(g.nodata)) {
        std::string nd = fmt(g.nodata, 10);
        nd += '\0';
        tags.push_back({42113, 2, static_cast<std::uint32_t>(nd.size()), nd});
    }
    std::sort(tags.begin(), tags.end(), [](const Tag& a, const Tag& b) { return a.id < b.id; });
    // layout: header, IFD, overflow values, strip
    std::string out = "II";
    out += shorts({42});
    out += longs({8});
    const std::size_t ifdSize = 2 + tags.size() * 12 + 4;
    std::size_t extra = 8 + ifdSize;
    std::string overflow;
    std::string ifd = shorts({static_cast<std::uint16_t>(tags.size())});
    std::size_t stripOffsetPos = 0;
    for (const auto& t : tags) {
        ifd += shorts({t.id, t.type});
        ifd += longs({t.count});
        if (t.data.size() <= 4) {
            if (t.id == 273) stripOffsetPos = 8 + ifd.size();
            std::string d = t.data;
            d.resize(4, '\0');
            ifd += d;
        } else {
            if (overflow.size() % 2) overflow += '\0';
            ifd += longs({static_cast<std::uint32_t>(extra + overflow.size())});
            overflow += t.data;
        }
    }
    ifd += longs({0});
    out += ifd;
    out += overflow;
    if (out.size() % 2) out += '\0';
    const std::uint32_t stripAt = static_cast<std::uint32_t>(out.size());
    out += strip;
    for (int k = 0; k < 4; ++k) out[stripOffsetPos + static_cast<std::size_t>(k)] = static_cast<char>((stripAt >> (8 * k)) & 0xFF);
    return out;
}

// ---- files ------------------------------------------------------------------------------

namespace {

std::vector<std::string> worldExtensions(const std::string& ext) {
    if (ext == ".png") return {".pgw", ".pngw", ".wld"};
    if (ext == ".jpg" || ext == ".jpeg") return {".jgw", ".jpgw", ".jpegw", ".wld"};
    if (ext == ".gif") return {".gfw", ".gifw", ".wld"};
    if (ext == ".bmp") return {".bpw", ".bmpw", ".wld"};
    if (ext == ".tif" || ext == ".tiff") return {".tfw", ".tifw", ".wld"};
    return {".wld"};
}

}  // namespace

std::shared_ptr<Grid> readRaster(const fs::path& path, const std::optional<Crs>& crs, const std::optional<BBox>& bbox, std::vector<std::string>* warnings) {
    const std::string ext = lower(path.extension().string());
    std::shared_ptr<Grid> g;
    bool placed = false;
    if (ext == ".tif" || ext == ".tiff") {
        g = readGeoTiff(readFile(path), warnings, &placed);
    } else {
        g = std::make_shared<Grid>();
        g->image = raster::load(path);
        g->width = g->image.width();
        g->height = g->image.height();
    }
    g->source = path.string();
    if (crs) g->crs = *crs;
    if (!placed || bbox) {
        bool fromWorld = false;
        if (!bbox) {
            for (const auto& we : worldExtensions(ext)) {
                fs::path w = path;
                w.replace_extension(we);
                if (!fs::exists(w)) continue;
                auto a = parseWorldFile(readFile(w));
                if (!a) throw std::runtime_error("can't read the world file " + w.string());
                g->geo = *a;
                fromWorld = true;
                break;
            }
            if (fromWorld && !crs) {
                fs::path prj = path;
                prj.replace_extension(".prj");
                if (fs::exists(prj)) {
                    if (auto c = Crs::parse(readFile(prj))) g->crs = *c;
                    else if (warnings) warnings->push_back("the .prj next to the image isn't a CRS maps knows; read as WGS84");
                }
            }
        }
        if (!fromWorld) {
            if (!bbox)
                throw std::runtime_error(path.filename().string() + " has no georeference: add a world file (" + worldExtensions(ext)[0] +
                                         ") next to it or pass bbox=min_lon,min_lat,max_lon,max_lat");
            Coord lo = g->crs.forward({bbox->minX, bbox->minY}), hi = g->crs.forward({bbox->maxX, bbox->maxY});
            g->geo = {(hi.x - lo.x) / g->width, 0, lo.x, 0, -(hi.y - lo.y) / g->height, hi.y};
        }
    }
    if (g->hasValues()) g->updateRange();
    return g;
}

std::vector<std::string> writeRaster(const Grid& g, const fs::path& path, const std::string& ramp) {
    const std::string ext = lower(path.extension().string());
    std::vector<std::string> files;
    if (ext == ".tif" || ext == ".tiff") {
        writeFile(path, encodeGeoTiff(g));
        files.push_back(path.string());
        return files;
    }
    raster::Image img = g.image;
    if (img.empty()) {
        img = raster::Image(g.width, g.height);
        const double span = g.maxValue - g.minValue;
        for (int y = 0; y < g.height; ++y) {
            for (int x = 0; x < g.width; ++x) {
                const float v = g.values[static_cast<std::size_t>(y) * g.width + x];
                if (!std::isfinite(v) || (!std::isnan(g.nodata) && v == static_cast<float>(g.nodata))) continue;
                img.set(x, y, rampColor(ramp, span > 0 ? (v - g.minValue) / span : 0.5));
            }
        }
    }
    raster::save(img, path);
    files.push_back(path.string());
    fs::path w = path;
    w.replace_extension(worldExtensions(ext)[0]);
    writeFile(w, worldFileText(g.geo));
    files.push_back(w.string());
    fs::path prj = path;
    prj.replace_extension(".prj");
    writeFile(prj, g.crs.wkt());
    files.push_back(prj.string());
    return files;
}

}  // namespace achclwss::maps
