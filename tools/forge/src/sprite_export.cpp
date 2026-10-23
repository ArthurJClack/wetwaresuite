// Sprite exports: sheets with Aseprite-style JSON, Godot SpriteFrames, GIFs.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "wetwaresuite/forge/sprite.hpp"
#include "wetwaresuite/forge/texture.hpp"
#include "wetwaresuite/raster/ops.hpp"

namespace wetwaresuite::forge {

namespace {

using json = nlohmann::ordered_json;

json rectJson(int x, int y, int w, int h) { return json{{"x", x}, {"y", y}, {"w", w}, {"h", h}}; }

const char* blendName(int mode) {
    static const char* names[] = {"normal",    "multiply",  "screen",     "overlay", "darken",     "lighten", "color_dodge",
                                  "color_burn", "hard_light", "soft_light", "difference", "exclusion", "hue",     "saturation",
                                  "color",     "luminosity", "addition",   "subtract", "divide"};
    return mode >= 0 && mode < 19 ? names[mode] : "normal";
}

std::string hex8(Color c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buf;
}

}  // namespace

SheetResult exportSheet(const Sprite& s, const SheetOptions& o) {
    std::vector<int> frames = o.frames;
    if (frames.empty()) {
        for (int f = 0; f < s.frames(); ++f) frames.push_back(f);
    }
    const int k = std::clamp(o.scale, 1, 16);
    struct Item {
        int frame;
        Image img;
        int offX = 0, offY = 0;
    };
    std::vector<Item> items;
    for (int f : frames) {
        Image img = s.flatten(f);
        Item it{f, img};
        if (o.trim) {
            const Rect b = opaqueBounds(img);
            if (b.empty()) {
                it.img = Image(1, 1);
            } else {
                Image t(b.w, b.h);
                for (int y = 0; y < b.h; ++y) std::copy_n(img.pixel(b.x, b.y + y), static_cast<std::size_t>(b.w) * 4, t.pixel(0, y));
                it.img = std::move(t);
                it.offX = b.x;
                it.offY = b.y;
            }
        }
        if (k > 1) it.img = raster::resize(it.img, it.img.width() * k, it.img.height() * k, raster::Filter::Nearest);
        it.offX *= k;
        it.offY *= k;
        items.push_back(std::move(it));
    }
    std::vector<PackItem> pack;
    for (const auto& it : items) pack.push_back({it.img.width(), it.img.height()});
    PackResult placed;
    const int pad = std::max(0, o.padding);
    if (o.layout == "packed") {
        placed = packShelf(pack, pad, false, 0);
    } else {
        const int n = static_cast<int>(items.size());
        const int cols = o.layout == "horizontal" ? n : o.layout == "vertical" ? 1 : (o.columns > 0 ? o.columns : static_cast<int>(std::ceil(std::sqrt(n))));
        placed = packGrid(pack, std::max(1, cols), pad, false);
    }
    SheetResult r;
    r.image = Image(std::max(1, placed.width), std::max(1, placed.height));
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Rect& at = placed.rects[i];
        blit(r.image, items[i].img, at.x, at.y);
        r.placed.push_back({items[i].frame, at.x, at.y, items[i].img.width(), items[i].img.height(), items[i].offX, items[i].offY});
    }
    // Aseprite's JSON: frames named "<title> <n>.aseprite", tags, layers
    json framesJ = o.format == "array" ? json::array() : json::object();
    for (const auto& p : r.placed) {
        const std::string name = o.title + " " + std::to_string(p.frame) + ".aseprite";
        json fj;
        if (o.format == "array") fj["filename"] = name;
        fj["frame"] = rectJson(p.x, p.y, p.w, p.h);
        fj["rotated"] = false;
        fj["trimmed"] = o.trim && (p.w != s.width * k || p.h != s.height * k);
        fj["spriteSourceSize"] = rectJson(p.offX, p.offY, p.w, p.h);
        fj["sourceSize"] = json{{"w", s.width * k}, {"h", s.height * k}};
        fj["duration"] = s.durations[static_cast<std::size_t>(p.frame)];
        if (o.format == "array") framesJ.push_back(fj);
        else framesJ[name] = fj;
    }
    json tags = json::array();
    for (const auto& t : s.tags) {
        // tag frame numbers refer to positions in this sheet when a subset was exported
        auto pos = [&](int f) {
            for (std::size_t i = 0; i < frames.size(); ++i) {
                if (frames[i] == f) return static_cast<int>(i);
            }
            return -1;
        };
        const int a = pos(t.from), b = pos(t.to);
        if (a < 0 || b < 0) continue;
        json tj = {{"name", t.name}, {"from", a}, {"to", b}, {"direction", t.direction}, {"color", hex8(t.color)}};
        if (t.repeat > 0) tj["repeat"] = std::to_string(t.repeat);
        tags.push_back(tj);
    }
    json layers = json::array();
    for (const auto& l : s.layers) {
        json lj = {{"name", l.name}, {"opacity", l.opacity}, {"blendMode", blendName(l.blend)}};
        if (l.group) lj = {{"name", l.name}};
        layers.push_back(lj);
    }
    json meta = {{"app", "wetwaresuite forge"},
                 {"version", "1.0"},
                 {"image", o.imageName},
                 {"format", "RGBA8888"},
                 {"size", {{"w", r.image.width()}, {"h", r.image.height()}}},
                 {"scale", std::to_string(k)},
                 {"frameTags", tags},
                 {"layers", layers},
                 {"slices", json::array()}};
    r.json = json{{"frames", framesJ}, {"meta", meta}}.dump(1) + "\n";
    return r;
}

std::string godotSpriteFrames(const Sprite& s, const SheetResult& sheet, const std::string& texturePath, int scale) {
    (void)scale;
    std::map<int, const SheetResult::Placed*> at;
    for (const auto& p : sheet.placed) at[p.frame] = &p;
    struct Anim {
        std::string name;
        std::vector<int> frames;
        bool loop = true;
    };
    std::vector<Anim> anims;
    for (const auto& t : s.tags) anims.push_back({t.name, s.tagFrames(t), t.repeat == 0});
    if (anims.empty()) {
        Anim a{"default", {}, true};
        for (const auto& p : sheet.placed) a.frames.push_back(p.frame);
        anims.push_back(a);
    }
    std::ostringstream sub, res;
    int subN = 0;
    std::map<int, std::string> atlas;  // frame -> sub resource id
    auto texture = [&](int f) -> std::string {
        auto it = atlas.find(f);
        if (it != atlas.end()) return it->second;
        const auto* p = at.count(f) ? at[f] : nullptr;
        if (!p) return {};
        const std::string id = "AtlasTexture_" + std::to_string(++subN);
        sub << "[sub_resource type=\"AtlasTexture\" id=\"" << id << "\"]\natlas = ExtResource(\"1_sheet\")\nregion = Rect2(" << p->x << ", " << p->y
            << ", " << p->w << ", " << p->h << ")\n";
        if (p->offX || p->offY || p->w != sheet.placed.front().w) {
            // trimmed frames keep their place in the full frame
            const int fw = s.width * std::max(1, scale), fh = s.height * std::max(1, scale);
            sub << "margin = Rect2(" << p->offX << ", " << p->offY << ", " << (fw - p->w) << ", " << (fh - p->h) << ")\n";
        }
        sub << "\n";
        return atlas[f] = id;
    };
    res << "[resource]\nanimations = [";
    for (std::size_t a = 0; a < anims.size(); ++a) {
        const Anim& an = anims[a];
        int base = 0;
        for (int f : an.frames) base = base == 0 ? s.durations[static_cast<std::size_t>(f)] : std::min(base, s.durations[static_cast<std::size_t>(f)]);
        base = std::max(1, base);
        res << (a ? ", " : "") << "{\n\"frames\": [";
        bool first = true;
        for (int f : an.frames) {
            const std::string id = texture(f);
            if (id.empty()) continue;
            char dur[32];
            std::snprintf(dur, sizeof dur, "%g", static_cast<double>(s.durations[static_cast<std::size_t>(f)]) / base);
            res << (first ? "" : ", ") << "{\n\"duration\": " << dur << ",\n\"texture\": SubResource(\"" << id << "\")\n}";
            first = false;
        }
        char speed[32];
        std::snprintf(speed, sizeof speed, "%g", 1000.0 / base);
        res << "],\n\"loop\": " << (an.loop ? "true" : "false") << ",\n\"name\": &\"" << an.name << "\",\n\"speed\": " << speed << "\n}";
    }
    res << "]\n";
    std::ostringstream out;
    out << "[gd_resource type=\"SpriteFrames\" load_steps=" << (subN + 2) << " format=3]\n\n";
    out << "[ext_resource type=\"Texture2D\" path=\"" << texturePath << "\" id=\"1_sheet\"]\n\n";
    out << sub.str() << res.str();
    return out.str();
}

// ---- GIF ----------------------------------------------------------------------------

namespace {

class BitWriter {
public:
    std::string bytes;
    void put(int code, int bits) {
        acc_ |= static_cast<std::uint32_t>(code) << n_;
        n_ += bits;
        while (n_ >= 8) {
            bytes += static_cast<char>(acc_ & 0xFF);
            acc_ >>= 8;
            n_ -= 8;
        }
    }
    void flush() {
        if (n_ > 0) bytes += static_cast<char>(acc_ & 0xFF);
        acc_ = 0;
        n_ = 0;
    }

private:
    std::uint32_t acc_ = 0;
    int n_ = 0;
};

// LZW-compresses palette indices with `minBits` colour bits (GIF flavour).
std::string lzw(const std::vector<std::uint8_t>& idx, int minBits) {
    const int clear = 1 << minBits, end = clear + 1;
    BitWriter w;
    std::map<std::pair<int, int>, int> table;
    int next = end + 1, bits = minBits + 1;
    w.put(clear, bits);
    if (idx.empty()) {
        w.put(end, bits);
        w.flush();
        return w.bytes;
    }
    int prefix = idx[0];
    for (std::size_t i = 1; i < idx.size(); ++i) {
        const int c = idx[i];
        auto it = table.find({prefix, c});
        if (it != table.end()) {
            prefix = it->second;
            continue;
        }
        w.put(prefix, bits);
        if (next < 4096) {
            table[{prefix, c}] = next++;
            if (next > (1 << bits) && bits < 12) ++bits;
        } else {
            w.put(clear, bits);
            table.clear();
            next = end + 1;
            bits = minBits + 1;
        }
        prefix = c;
    }
    w.put(prefix, bits);
    w.put(end, bits);
    w.flush();
    return w.bytes;
}

}  // namespace

std::string encodeSpriteGif(const Sprite& s, const std::vector<int>& which, int scale) {
    std::vector<int> frames = which;
    if (frames.empty()) {
        for (int f = 0; f < s.frames(); ++f) frames.push_back(f);
    }
    const int k = std::clamp(scale, 1, 16);
    std::vector<Image> imgs;
    for (int f : frames) {
        Image img = s.flatten(f);
        if (k > 1) img = raster::resize(img, img.width() * k, img.height() * k, raster::Filter::Nearest);
        imgs.push_back(std::move(img));
    }
    // one global palette: index 0 is transparent, the rest the sprite's colours (or a quantised set)
    std::vector<Color> colors;
    {
        std::vector<Color> used = spriteColors(s);
        std::vector<Color> opaque;
        for (auto c : used) {
            c.a = 255;
            if (std::find(opaque.begin(), opaque.end(), c) == opaque.end()) opaque.push_back(c);
        }
        if (opaque.size() <= 255) {
            colors = opaque;
        } else {
            // too many: quantise a strip of every frame
            Image all(imgs[0].width(), imgs[0].height() * static_cast<int>(imgs.size()));
            for (std::size_t i = 0; i < imgs.size(); ++i) blit(all, imgs[i], 0, static_cast<int>(i) * imgs[0].height());
            colors = buildPalette(all, 255, QuantizeMethod::MedianCut);
        }
    }
    colors.insert(colors.begin(), Color{0, 0, 0, 0});
    int bits = 1;
    while ((1 << bits) < static_cast<int>(colors.size())) ++bits;
    const int tableSize = 1 << bits;
    const int w = imgs.empty() ? s.width * k : imgs[0].width(), h = imgs.empty() ? s.height * k : imgs[0].height();
    std::string out = "GIF89a";
    auto word = [&](int v) {
        out += static_cast<char>(v & 0xFF);
        out += static_cast<char>(v >> 8 & 0xFF);
    };
    word(w);
    word(h);
    out += static_cast<char>(0x80 | 0x70 | (bits - 1));
    out += '\0';
    out += '\0';
    for (int i = 0; i < tableSize; ++i) {
        const Color c = i < static_cast<int>(colors.size()) ? colors[static_cast<std::size_t>(i)] : Color{0, 0, 0, 255};
        out += static_cast<char>(c.r);
        out += static_cast<char>(c.g);
        out += static_cast<char>(c.b);
    }
    out += "\x21\xFF\x0BNETSCAPE2.0\x03\x01";
    word(0);
    out += '\0';
    std::map<std::uint32_t, std::uint8_t> cache;
    for (std::size_t fi = 0; fi < imgs.size(); ++fi) {
        const Image& img = imgs[fi];
        const int delay = std::max(2, (s.durations[static_cast<std::size_t>(frames[fi])] + 5) / 10);
        out += "\x21\xF9\x04";
        out += static_cast<char>(0x09);  // dispose: restore to background, transparent colour on
        word(delay);
        out += '\0';  // transparent index 0
        out += '\0';
        out += ',';
        word(0);
        word(0);
        word(w);
        word(h);
        out += '\0';
        std::vector<std::uint8_t> idx(static_cast<std::size_t>(w) * h, 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::uint8_t* p = img.pixel(x, y);
                if (p[3] < 128) continue;
                const std::uint32_t key = static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
                auto it = cache.find(key);
                if (it == cache.end()) it = cache.emplace(key, static_cast<std::uint8_t>(std::max(1, nearestIndex(colors, {p[0], p[1], p[2], 255}, 0)))).first;
                idx[static_cast<std::size_t>(y) * w + x] = it->second;
            }
        }
        const int minBits = std::max(2, bits);
        out += static_cast<char>(minBits);
        const std::string data = lzw(idx, minBits);
        for (std::size_t at = 0; at < data.size(); at += 255) {
            const std::size_t n = std::min<std::size_t>(255, data.size() - at);
            out += static_cast<char>(n);
            out += data.substr(at, n);
        }
        out += '\0';
    }
    out += ';';
    return out;
}

}  // namespace wetwaresuite::forge
