// The sprite model: cels, flattening, palettes, pixel drawing, onion skins.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "wetwaresuite/forge/sprite.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ops.hpp"

namespace wetwaresuite::forge {

namespace {

bool parseIndex(const std::string& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    out = std::stoi(s);
    return true;
}

raster::Blend blendFor(int mode) {
    switch (mode) {
        case 1: return raster::Blend::Multiply;
        case 2: return raster::Blend::Screen;
        case 3: return raster::Blend::Overlay;
        case 4: return raster::Blend::Darken;
        case 5: return raster::Blend::Lighten;
        case 10: return raster::Blend::Difference;
        case 16: return raster::Blend::Add;
        default: return raster::Blend::Normal;
    }
}

std::vector<Color> hexList(std::initializer_list<const char*> hex) {
    std::vector<Color> out;
    for (const char* h : hex) {
        if (auto c = Color::parse(std::string("#") + h)) out.push_back(*c);
    }
    return out;
}

}  // namespace

Image& Sprite::cel(int layer, int frame) {
    Image& c = cels[static_cast<std::size_t>(layer)][static_cast<std::size_t>(frame)];
    if (c.empty()) c = Image(width, height);
    return c;
}

const Image* Sprite::celIf(int layer, int frame) const {
    if (layer < 0 || frame < 0 || layer >= static_cast<int>(cels.size()) || frame >= static_cast<int>(cels[static_cast<std::size_t>(layer)].size())) return nullptr;
    const Image& c = cels[static_cast<std::size_t>(layer)][static_cast<std::size_t>(frame)];
    return c.empty() ? nullptr : &c;
}

Image Sprite::flatten(int frame) const {
    Image out(std::max(1, width), std::max(1, height));
    std::vector<bool> shown;  // visibility of the group at each level
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const SpriteLayer& l = layers[i];
        shown.resize(static_cast<std::size_t>(l.level) + 1, true);
        const bool parentShown = l.level == 0 || shown[static_cast<std::size_t>(l.level) - 1];
        const bool visible = parentShown && l.visible;
        shown[static_cast<std::size_t>(l.level)] = visible;
        if (l.group || !visible) continue;
        if (const Image* c = celIf(static_cast<int>(i), frame)) raster::blendOnto(out, *c, 0, 0, blendFor(l.blend), l.opacity / 255.0);
    }
    return out;
}

int Sprite::layerIndex(const std::string& key) const {
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].name == key) return static_cast<int>(i);
    }
    int idx = 0;
    if (parseIndex(key, idx) && idx < static_cast<int>(layers.size())) return idx;
    return -1;
}

int Sprite::tagIndex(const std::string& name) const {
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

std::vector<int> Sprite::tagFrames(const SpriteTag& tag) const {
    std::vector<int> fw;
    const int a = std::clamp(tag.from, 0, std::max(0, frames() - 1)), b = std::clamp(tag.to, a, std::max(0, frames() - 1));
    for (int f = a; f <= b; ++f) fw.push_back(f);
    if (tag.direction == "reverse") return {fw.rbegin(), fw.rend()};
    if (tag.direction == "pingpong" || tag.direction == "pingpong_reverse") {
        std::vector<int> out = fw;
        for (int k = static_cast<int>(fw.size()) - 2; k > 0; --k) out.push_back(fw[static_cast<std::size_t>(k)]);
        if (tag.direction == "pingpong_reverse") std::rotate(out.begin(), out.begin() + static_cast<long>(fw.size()) - 1, out.end());
        return out;
    }
    return fw;
}

Sprite newSprite(int width, int height, int frames, int duration) {
    Sprite s;
    s.width = width;
    s.height = height;
    s.layers.push_back({"Layer 1"});
    s.durations.assign(static_cast<std::size_t>(std::max(1, frames)), std::max(1, duration));
    s.cels.assign(1, std::vector<Image>(s.durations.size()));
    return s;
}

// ---- palettes --------------------------------------------------------------------

int nearestIndex(const std::vector<Color>& palette, Color c, int skip) {
    int best = -1;
    long bestD = 0;
    for (std::size_t i = 0; i < palette.size(); ++i) {
        if (static_cast<int>(i) == skip) continue;
        const Color& p = palette[i];
        const long dr = c.r - p.r, dg = c.g - p.g, db = c.b - p.b;
        // weighted towards green, like the eye
        const long d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
        if (best < 0 || d < bestD) {
            best = static_cast<int>(i);
            bestD = d;
            if (d == 0) break;
        }
    }
    return best;
}

void snapToPalette(Sprite& s) {
    if (s.palette.empty()) return;
    std::map<std::uint32_t, Color> cache;
    const int skip = s.indexed ? s.transparentIndex : -1;
    for (auto& layer : s.cels) {
        for (auto& c : layer) {
            if (c.empty()) continue;
            std::uint8_t* px = c.data();
            for (std::size_t i = 0; i < c.bytes(); i += 4) {
                if (px[i + 3] < 128) {
                    px[i] = px[i + 1] = px[i + 2] = px[i + 3] = 0;
                    continue;
                }
                const std::uint32_t key = static_cast<std::uint32_t>(px[i]) << 16 | static_cast<std::uint32_t>(px[i + 1]) << 8 | px[i + 2];
                auto it = cache.find(key);
                if (it == cache.end()) {
                    const int k = nearestIndex(s.palette, {px[i], px[i + 1], px[i + 2], 255}, skip);
                    Color p = k >= 0 ? s.palette[static_cast<std::size_t>(k)] : Color{px[i], px[i + 1], px[i + 2], 255};
                    p.a = 255;
                    it = cache.emplace(key, p).first;
                }
                px[i] = it->second.r;
                px[i + 1] = it->second.g;
                px[i + 2] = it->second.b;
                px[i + 3] = 255;
            }
        }
    }
}

std::vector<Color> spriteColors(const Sprite& s) {
    std::vector<std::uint32_t> keys;
    for (const auto& layer : s.cels) {
        for (const auto& c : layer) {
            if (c.empty()) continue;
            const std::uint8_t* px = c.data();
            for (std::size_t i = 0; i < c.bytes(); i += 4) {
                if (px[i + 3] == 0) continue;
                keys.push_back(static_cast<std::uint32_t>(px[i]) << 24 | static_cast<std::uint32_t>(px[i + 1]) << 16 |
                               static_cast<std::uint32_t>(px[i + 2]) << 8 | px[i + 3]);
            }
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<Color> out;
    for (auto k : keys) {
        out.push_back({static_cast<std::uint8_t>(k >> 24), static_cast<std::uint8_t>(k >> 16), static_cast<std::uint8_t>(k >> 8), static_cast<std::uint8_t>(k)});
    }
    return out;
}

std::vector<Color> presetPalette(const std::string& name) {
    if (name == "pico8") {
        return hexList({"000000", "1d2b53", "7e2553", "008751", "ab5236", "5f574f", "c2c3c7", "fff1e8", "ff004d", "ffa300", "ffec27", "00e436", "29adff",
                        "83769c", "ff77a8", "ffccaa"});
    }
    if (name == "db16") {
        return hexList({"140c1c", "442434", "30346d", "4e4a4e", "854c30", "346524", "d04648", "757161", "597dce", "d27d2c", "8595a1", "6daa2c", "d2aa99",
                        "6dc2ca", "dad45e", "deeed6"});
    }
    if (name == "db32") {
        return hexList({"000000", "222034", "45283c", "663931", "8f563b", "df7126", "d9a066", "eec39a", "fbf236", "99e550", "6abe30",
                        "37946e", "4b692f", "524b24", "323c39", "3f3f74", "306082", "5b6ee1", "639bff", "5fcde4", "cbdbfc", "ffffff",
                        "9badb7", "847e87", "696a6a", "595652", "76428a", "ac3232", "d95763", "d77bba", "8f974a", "8a6f30"});
    }
    if (name == "gameboy") return hexList({"0f380f", "306230", "8bac0f", "9bbc0f"});
    if (name == "grayscale4") return hexList({"000000", "555555", "aaaaaa", "ffffff"});
    return {};
}

std::vector<Color> loadPalette(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path);
    std::vector<Color> out;
    std::string line;
    bool jasc = false;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lineNo == 1 && line == "JASC-PAL") {
            jasc = true;
            continue;
        }
        if (jasc && lineNo <= 3) continue;  // version and count
        if (line.empty() || line[0] == '#' || line.rfind("GIMP", 0) == 0 || line.rfind("Name:", 0) == 0 || line.rfind("Columns:", 0) == 0) continue;
        std::istringstream ls(line);
        std::string first;
        ls >> first;
        if (first.empty()) continue;
        if (auto c = Color::parse(first[0] == '#' ? first : "#" + first); c && first.size() >= 6 && first.find_first_not_of("#0123456789abcdefABCDEF") == std::string::npos) {
            out.push_back(*c);
            continue;
        }
        int r = std::atoi(first.c_str()), g = 0, b = 0;
        if (ls >> g >> b) out.push_back({static_cast<std::uint8_t>(std::clamp(r, 0, 255)), static_cast<std::uint8_t>(std::clamp(g, 0, 255)),
                                         static_cast<std::uint8_t>(std::clamp(b, 0, 255)), 255});
    }
    if (out.empty()) throw std::runtime_error(path + " has no colours (expected GIMP .gpl, JASC .pal or one hex colour per line)");
    return out;
}

std::string paletteGpl(const std::vector<Color>& palette, const std::string& name) {
    std::ostringstream o;
    o << "GIMP Palette\nName: " << name << "\nColumns: 8\n#\n";
    char buf[64];
    for (const auto& c : palette) {
        std::snprintf(buf, sizeof buf, "%3d %3d %3d\t%s\n", c.r, c.g, c.b, c.hex().substr(1, 6).c_str());
        o << buf;
    }
    return o.str();
}

// ---- drawing ---------------------------------------------------------------------

namespace {

void put(Image& img, int x, int y, Color c) {
    if (img.contains(x, y)) img.set(x, y, c);
}

}  // namespace

void drawPixelLine(Image& img, int x0, int y0, int x1, int y1, Color c) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 1 << 20; ++guard) {
        put(img, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void drawPixelRect(Image& img, int x0, int y0, int x1, int y1, Color c, bool filled) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (filled || x == x0 || x == x1 || y == y0 || y == y1) put(img, x, y, c);
        }
    }
}

void drawPixelEllipse(Image& img, int x0, int y0, int x1, int y1, Color c, bool filled) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0, rx = (x1 - x0 + 1) / 2.0, ry = (y1 - y0 + 1) / 2.0;
    auto inside = [&](int x, int y) {
        const double u = (x - cx) / rx, v = (y - cy) / ry;
        return u * u + v * v <= 1.0;
    };
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (!inside(x, y)) continue;
            if (filled || !inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) || !inside(x, y + 1)) put(img, x, y, c);
        }
    }
}

int bucketFill(Image& img, int x, int y, Color c) {
    if (!img.contains(x, y)) return 0;
    const Color from = img.at(x, y);
    if (from == c) return 0;
    std::vector<std::pair<int, int>> stack{{x, y}};
    int n = 0;
    while (!stack.empty()) {
        auto [px, py] = stack.back();
        stack.pop_back();
        if (!img.contains(px, py) || !(img.at(px, py) == from)) continue;
        img.set(px, py, c);
        ++n;
        stack.push_back({px + 1, py});
        stack.push_back({px - 1, py});
        stack.push_back({px, py + 1});
        stack.push_back({px, py - 1});
    }
    return n;
}

// ---- onion skin --------------------------------------------------------------------

Image onionSkin(const Sprite& s, int frame, int before, int after, int opacity) {
    Image out(std::max(1, s.width), std::max(1, s.height));
    auto ghost = [&](int f, int dist, int range, Color tint) {
        if (f < 0 || f >= s.frames()) return;
        Image g = s.flatten(f);
        const double fade = opacity / 255.0 * (1.0 - (dist - 1.0) / std::max(1, range));
        std::uint8_t* px = g.data();
        for (std::size_t i = 0; i < g.bytes(); i += 4) {
            px[i] = static_cast<std::uint8_t>((px[i] + tint.r) / 2);
            px[i + 1] = static_cast<std::uint8_t>((px[i + 1] + tint.g) / 2);
            px[i + 2] = static_cast<std::uint8_t>((px[i + 2] + tint.b) / 2);
        }
        raster::blendOnto(out, g, 0, 0, raster::Blend::Normal, std::clamp(fade, 0.0, 1.0));
    };
    for (int k = before; k >= 1; --k) ghost(frame - k, k, before, Color{255, 40, 40, 255});
    for (int k = after; k >= 1; --k) ghost(frame + k, k, after, Color{40, 80, 255, 255});
    raster::blendOnto(out, s.flatten(frame), 0, 0, raster::Blend::Normal, 1.0);
    return out;
}

// ---- documents ---------------------------------------------------------------------

std::string SpriteDoc::summary() const {
    const Sprite& s = sprite;
    std::string t = std::to_string(s.width) + "x" + std::to_string(s.height) + " sprite, " + std::to_string(s.frames()) + " frame" +
                    (s.frames() == 1 ? "" : "s") + ", " + std::to_string(s.layers.size()) + " layer" + (s.layers.size() == 1 ? "" : "s");
    if (!s.tags.empty()) {
        t += ", tags ";
        for (std::size_t i = 0; i < s.tags.size(); ++i) t += (i ? "," : "") + s.tags[i].name;
    }
    if (s.indexed) t += ", indexed " + std::to_string(s.palette.size()) + " colours";
    else if (!s.palette.empty()) t += ", palette of " + std::to_string(s.palette.size());
    if (!path.empty()) t += ", " + path;
    return t;
}

std::unique_ptr<Attachment> SpriteDoc::preview() const {
    // the first frames side by side, scaled up so small pixel art reads
    const int n = std::min(sprite.frames(), 8);
    Image strip(std::max(1, sprite.width * n + 2 * (n - 1)), std::max(1, sprite.height));
    for (int f = 0; f < n; ++f) raster::blendOnto(strip, sprite.flatten(f), f * (sprite.width + 2), 0, raster::Blend::Normal, 1.0);
    const int big = std::max(strip.width(), strip.height());
    const int k = std::clamp(512 / std::max(1, big), 1, 16);
    if (k > 1) strip = raster::resize(strip, strip.width() * k, strip.height() * k, raster::Filter::Nearest);
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(strip), "sprite.png"});
}

Result SpriteEdit::apply() {
    before_ = doc_->sprite;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void SpriteEdit::revert() { doc_->sprite = before_; }

}  // namespace wetwaresuite::forge
