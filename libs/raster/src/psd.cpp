// Layered PSD (Adobe Photoshop file format, version 1, 8-bit RGB). Layers are written raw
// (compression 0), which every reader takes; reading also handles PackBits (RLE), what
// Photoshop itself writes. Groups use 'lsct' section dividers, names 'luni', fill 'iOpa'.
#include "wetwaresuite/raster/psd.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

namespace {

const std::vector<std::pair<Blend, const char*>>& blendKeys() {
    static const std::vector<std::pair<Blend, const char*>> keys = {
        {Blend::Normal, "norm"},      {Blend::Dissolve, "diss"},    {Blend::Darken, "dark"},      {Blend::Multiply, "mul "},
        {Blend::ColorBurn, "idiv"},   {Blend::LinearBurn, "lbrn"},  {Blend::DarkerColor, "dkCl"}, {Blend::Lighten, "lite"},
        {Blend::Screen, "scrn"},      {Blend::ColorDodge, "div "},  {Blend::Add, "lddg"},         {Blend::LighterColor, "lgCl"},
        {Blend::Overlay, "over"},     {Blend::SoftLight, "sLit"},   {Blend::HardLight, "hLit"},   {Blend::VividLight, "vLit"},
        {Blend::LinearLight, "lLit"}, {Blend::PinLight, "pLit"},    {Blend::HardMix, "hMix"},     {Blend::Difference, "diff"},
        {Blend::Exclusion, "smud"},   {Blend::Subtract, "fsub"},    {Blend::Divide, "fdiv"},      {Blend::Hue, "hue "},
        {Blend::Saturation, "sat "},  {Blend::Color, "colr"},       {Blend::Luminosity, "lum "}};
    return keys;
}

const char* keyOf(Blend b) {
    for (const auto& [blend, key] : blendKeys()) {
        if (blend == b) return key;
    }
    return "norm";
}

Blend blendOf(const std::string& key) {
    for (const auto& [blend, k] : blendKeys()) {
        if (key == k) return blend;
    }
    return Blend::Normal;
}

// ---- writing ----

struct Out {
    std::string s;
    void u8(unsigned v) { s += static_cast<char>(v & 0xff); }
    void u16(unsigned v) {
        u8(v >> 8);
        u8(v);
    }
    void u32(std::uint32_t v) {
        u16(v >> 16);
        u16(v & 0xffff);
    }
    void i32(int v) { u32(static_cast<std::uint32_t>(v)); }
    void raw(const std::string& t) { s += t; }
    void pad(std::size_t multiple) {
        while (s.size() % multiple) u8(0);
    }
};

std::u16string utf16(const std::string& text) {
    std::u16string out;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        unsigned cp = c, extra = 0;
        if (c >= 0xf0) cp = c & 0x07, extra = 3;
        else if (c >= 0xe0) cp = c & 0x0f, extra = 2;
        else if (c >= 0xc0) cp = c & 0x1f, extra = 1;
        ++i;
        for (unsigned k = 0; k < extra && i < text.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3f);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xd800 + (cp >> 10));
            out += static_cast<char16_t>(0xdc00 + (cp & 0x3ff));
        } else {
            out += static_cast<char16_t>(cp);
        }
    }
    return out;
}

std::string utf8(const std::u16string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        unsigned cp = text[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < text.size()) cp = 0x10000 + ((cp - 0xd800) << 10) + (text[++i] - 0xdc00);
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) out += static_cast<char>(0xc0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3f));
        else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

// One layer record as it goes into the file.
struct Record {
    std::string name;
    int top = 0, left = 0, bottom = 0, right = 0;
    Blend blend = Blend::Normal;
    bool passThrough = false;
    double opacity = 1, fill = 1;
    bool clip = false, hidden = false, lockAlpha = false;
    int section = 0;  // lsct: 0 none, 1 open group, 2 closed group, 3 the group's bottom divider
    std::vector<std::pair<int, std::string>> channels;  // id, raw bytes (rect sized)
    // the mask, when there is one
    bool hasMask = false, maskDisabled = false;
    int mTop = 0, mLeft = 0, mBottom = 0, mRight = 0;
};

void block(Out& o, const char* key, const std::string& data) {
    o.raw("8BIM");
    o.raw(key);
    std::string d = data;
    while (d.size() % 4) d += '\0';
    o.u32(static_cast<std::uint32_t>(d.size()));
    o.raw(d);
}

std::string extraData(const Record& r) {
    Out e;
    if (r.hasMask) {
        e.u32(20);
        e.i32(r.mTop), e.i32(r.mLeft), e.i32(r.mBottom), e.i32(r.mRight);
        e.u8(255);
        e.u8(r.maskDisabled ? 2 : 0);
        e.u16(0);
    } else {
        e.u32(0);
    }
    e.u32(0);  // blending ranges
    std::string ascii;
    for (char c : r.name) ascii += static_cast<unsigned char>(c) < 128 ? c : '?';
    if (ascii.size() > 255) ascii.resize(255);
    e.u8(static_cast<unsigned>(ascii.size()));
    e.raw(ascii);
    e.pad(4);
    Out u;
    const std::u16string wide = utf16(r.name);
    u.u32(static_cast<std::uint32_t>(wide.size()));
    for (char16_t c : wide) u.u16(c);
    block(e, "luni", u.s);
    if (r.section) {
        Out l;
        l.u32(static_cast<std::uint32_t>(r.section));
        l.raw("8BIM");
        l.raw(r.passThrough ? "pass" : keyOf(r.blend));
        block(e, "lsct", l.s);
    }
    if (r.fill < 1) {
        Out f;
        f.u8(static_cast<unsigned>(std::lround(std::clamp(r.fill, 0.0, 1.0) * 255)));
        block(e, "iOpa", f.s);
    }
    return e.s;
}

// The rect of pixels a layer has (alpha > 0), so empty space isn't stored.
PixelRect usedRect(const Image& img) {
    PixelRect r{img.width(), img.height(), 0, 0};
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (!img.pixel(x, y)[3]) continue;
            r.x0 = std::min(r.x0, x), r.x1 = std::max(r.x1, x + 1);
            r.y0 = std::min(r.y0, y), r.y1 = std::max(r.y1, y + 1);
        }
    }
    return r.empty() ? PixelRect{} : r;
}

Record pixelRecord(const Layer& l, const Image& pixels) {
    Record r;
    r.name = l.name;
    r.blend = l.blend;
    r.opacity = l.opacity;
    r.fill = l.fill;
    r.clip = l.clip;
    r.hidden = !l.visible;
    r.lockAlpha = l.lockAlpha;
    const PixelRect u = usedRect(pixels);
    r.top = u.y0, r.left = u.x0, r.bottom = u.y1, r.right = u.x1;
    const int w = u.width(), h = u.height();
    const int ids[4] = {-1, 0, 1, 2};
    const int src[4] = {3, 0, 1, 2};
    for (int c = 0; c < 4; ++c) {
        std::string data(static_cast<std::size_t>(std::max(0, w)) * std::max(0, h), '\0');
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) data[static_cast<std::size_t>(y) * w + x] = static_cast<char>(pixels.pixel(u.x0 + x, u.y0 + y)[src[c]]);
        }
        r.channels.emplace_back(ids[c], std::move(data));
    }
    return r;
}

void addMask(Record& r, const Layer& l) {
    if (l.mask.empty()) return;
    r.hasMask = true;
    r.maskDisabled = !l.maskEnabled;
    r.mTop = 0, r.mLeft = 0, r.mBottom = l.mask.height, r.mRight = l.mask.width;
    r.channels.emplace_back(-2, std::string(l.mask.v.begin(), l.mask.v.end()));
}

Record emptyRecord(const std::string& name) {
    Record r;
    r.name = name;
    for (int id : {-1, 0, 1, 2}) r.channels.emplace_back(id, std::string());
    return r;
}

// A pixel, text, shape or adjustment layer's record.
void collect(const Layer& l, std::vector<Record>& out, std::vector<std::string>& notes) {
    if (l.kind == LayerKind::Adjustment) {
        Record r = emptyRecord(l.name);
        r.hidden = true;
        out.push_back(std::move(r));
        const auto type = l.settings.count("type") ? l.settings.at("type") : std::string("adjustment");
        notes.push_back("adjustment layer \"" + l.name + "\" (" + type +
                        ") can't be stored in a PSD; it was saved as an empty hidden layer (keep an .ora for editing)");
        return;
    }
    Image pixels = l.image;
    if (l.style.any()) {
        Layer alone;
        alone.image = l.image;
        alone.style = l.style;
        pixels = composite({alone}, l.image.width(), l.image.height());
        notes.push_back("the style of layer \"" + l.name + "\" was drawn into its pixels");
    }
    if (l.kind == LayerKind::Text || l.kind == LayerKind::Shape) notes.push_back(toString(l.kind) + " layer \"" + l.name + "\" was saved as pixels");
    Record r = pixelRecord(l, pixels);
    addMask(r, l);
    out.push_back(std::move(r));
}

// Layers [lo, hi) at group depth d, bottom first as PSD stores them: each group is its bottom
// divider, its members, then its header.
void records(const LayeredImage& img, int lo, int hi, int d, std::vector<Record>& out, std::vector<std::string>& notes) {
    int i = lo;
    while (i < hi) {
        int j = i;
        while (j < hi && img.layers[static_cast<std::size_t>(j)].depth > d) ++j;
        const bool group = j < hi && img.layers[static_cast<std::size_t>(j)].kind == LayerKind::Group;
        if (!group) {
            for (int k = i; k < std::min(j + 1, hi); ++k) collect(img.layers[static_cast<std::size_t>(k)], out, notes);
            i = j + 1;
            continue;
        }
        const Layer& g = img.layers[static_cast<std::size_t>(j)];
        Record divider = emptyRecord("</Layer group>");
        divider.section = 3;
        divider.hidden = true;
        out.push_back(std::move(divider));
        records(img, i, j, d + 1, out, notes);
        Record header = emptyRecord(g.name);
        header.section = g.collapsed ? 2 : 1;
        header.blend = g.blend;
        header.passThrough = g.passThrough;
        header.opacity = g.opacity;
        header.fill = g.fill;
        header.clip = g.clip;
        header.hidden = !g.visible;
        addMask(header, g);
        out.push_back(std::move(header));
        i = j + 1;
    }
}

// ---- reading ----

struct In {
    const std::string& s;
    std::size_t at = 0;
    void need(std::size_t n) const {
        if (at + n > s.size()) throw std::runtime_error("the PSD file is cut short");
    }
    unsigned u8() {
        need(1);
        return static_cast<unsigned char>(s[at++]);
    }
    unsigned u16() {
        const unsigned a = u8();
        return (a << 8) | u8();
    }
    std::uint32_t u32() {
        const std::uint32_t a = u16();
        return (a << 16) | u16();
    }
    int i32() { return static_cast<std::int32_t>(u32()); }
    int i16() { return static_cast<std::int16_t>(u16()); }
    std::string raw(std::size_t n) {
        need(n);
        std::string r = s.substr(at, n);
        at += n;
        return r;
    }
};

// One channel's bytes for a w x h rect: raw or PackBits.
std::string readChannel(In& in, std::size_t length, int w, int h) {
    const std::size_t end = in.at + length;
    if (length < 2) {
        in.at = end;
        return {};
    }
    const unsigned compression = in.u16();
    const std::size_t n = static_cast<std::size_t>(std::max(0, w)) * std::max(0, h);
    std::string out;
    if (compression == 0) {
        out = in.raw(std::min(n, length - 2));
    } else if (compression == 1) {
        std::vector<unsigned> counts(static_cast<std::size_t>(std::max(0, h)));
        for (auto& c : counts) c = in.u16();
        out.reserve(n);
        for (int row = 0; row < h; ++row) {
            const std::size_t rowEnd = in.at + counts[static_cast<std::size_t>(row)];
            std::string line;
            while (in.at < rowEnd && static_cast<int>(line.size()) < w) {
                const int c = static_cast<std::int8_t>(in.u8());
                if (c >= 0) line += in.raw(static_cast<std::size_t>(c) + 1);
                else if (c != -128) line.append(static_cast<std::size_t>(1 - c), static_cast<char>(in.u8()));
            }
            line.resize(static_cast<std::size_t>(w), '\0');
            out += line;
            in.at = rowEnd;
        }
    } else {
        throw std::runtime_error("this PSD uses ZIP-compressed layers, which aren't read layer by layer");
    }
    out.resize(n, '\0');
    in.at = end;
    return out;
}

}  // namespace

std::string encodePsd(const LayeredImage& img, std::vector<std::string>* notesOut) {
    std::vector<std::string> notes;
    std::vector<Record> recs;
    records(img, 0, static_cast<int>(img.layers.size()), 0, recs, notes);
    Out o;
    o.raw("8BPS");
    o.u16(1);
    o.raw(std::string(6, '\0'));
    o.u16(3);
    o.u32(static_cast<std::uint32_t>(img.height));
    o.u32(static_cast<std::uint32_t>(img.width));
    o.u16(8);
    o.u16(3);  // RGB
    o.u32(0);  // colour mode data
    o.u32(0);  // image resources

    Out info;
    info.u16(static_cast<unsigned>(recs.size()));
    for (const auto& r : recs) {
        info.i32(r.top), info.i32(r.left), info.i32(r.bottom), info.i32(r.right);
        info.u16(static_cast<unsigned>(r.channels.size()));
        for (const auto& [id, data] : r.channels) {
            info.u16(static_cast<unsigned>(id & 0xffff));
            info.u32(static_cast<std::uint32_t>(data.size() + 2));
        }
        info.raw("8BIM");
        info.raw(r.passThrough ? "pass" : keyOf(r.blend));
        info.u8(static_cast<unsigned>(std::lround(std::clamp(r.opacity, 0.0, 1.0) * 255)));
        info.u8(r.clip ? 1 : 0);
        info.u8((r.lockAlpha ? 1 : 0) | (r.hidden ? 2 : 0) | 8);
        info.u8(0);
        const std::string extra = extraData(r);
        info.u32(static_cast<std::uint32_t>(extra.size()));
        info.raw(extra);
    }
    for (const auto& r : recs) {
        for (const auto& [id, data] : r.channels) {
            info.u16(0);
            info.raw(data);
        }
    }
    info.pad(2);
    Out section;
    section.u32(static_cast<std::uint32_t>(info.s.size()));
    section.raw(info.s);
    section.u32(0);  // global layer mask info
    o.u32(static_cast<std::uint32_t>(section.s.size()));
    o.raw(section.s);

    // the flattened picture, over white, for readers that only take that
    Image merged(img.width, img.height, {255, 255, 255, 255});
    blendOnto(merged, composite(img.layers, img.width, img.height), 0, 0, Blend::Normal, 1.0);
    o.u16(0);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < img.height; ++y) {
            for (int x = 0; x < img.width; ++x) o.u8(merged.pixel(x, y)[c]);
        }
    }
    if (notesOut) *notesOut = notes;
    return o.s;
}

std::vector<std::string> savePsd(const LayeredImage& image, const std::filesystem::path& path) {
    std::vector<std::string> notes;
    const std::string bytes = encodePsd(image, &notes);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
    return notes;
}

LayeredImage decodePsd(const std::string& bytes) {
    In in{bytes};
    if (in.raw(4) != "8BPS") throw std::runtime_error("not a PSD file");
    if (in.u16() != 1) throw std::runtime_error("PSB (large document) files aren't read layer by layer");
    in.raw(6);
    const unsigned channels = in.u16();
    LayeredImage out;
    out.height = static_cast<int>(in.u32());
    out.width = static_cast<int>(in.u32());
    const unsigned depth = in.u16(), mode = in.u16();
    if (depth != 8 || mode != 3) throw std::runtime_error("only 8-bit RGB PSD files are read layer by layer");
    if (out.width <= 0 || out.height <= 0 || out.width > 30000 || out.height > 30000) throw std::runtime_error("bad PSD size");
    (void)channels;
    in.at += in.u32();  // colour mode data
    in.need(4);
    in.at += in.u32();  // image resources
    const std::uint32_t sectionLength = in.u32();
    if (sectionLength == 0) throw std::runtime_error("this PSD has no layers");
    const std::uint32_t infoLength = in.u32();
    if (infoLength == 0) throw std::runtime_error("this PSD has no layers");
    const int count = std::abs(in.i16());
    struct Rec {
        int top, left, bottom, right;
        std::vector<std::pair<int, std::uint32_t>> channels;
        std::string blend;
        unsigned opacity, clip, flags;
        std::string name;
        int section = 0;
        std::string sectionBlend;
        double fill = 1;
        bool hasMask = false;
        int mTop = 0, mLeft = 0, mBottom = 0, mRight = 0;
        unsigned mDefault = 0, mFlags = 0;
    };
    std::vector<Rec> recs(static_cast<std::size_t>(count));
    for (auto& r : recs) {
        r.top = in.i32(), r.left = in.i32(), r.bottom = in.i32(), r.right = in.i32();
        const unsigned n = in.u16();
        for (unsigned c = 0; c < n; ++c) {
            const int id = in.i16();
            r.channels.emplace_back(id, in.u32());
        }
        if (in.raw(4) != "8BIM") throw std::runtime_error("corrupt PSD layer record");
        r.blend = in.raw(4);
        r.opacity = in.u8(), r.clip = in.u8(), r.flags = in.u8();
        in.u8();
        const std::uint32_t extra = in.u32();
        const std::size_t extraEnd = in.at + extra;
        const std::uint32_t maskLen = in.u32();
        const std::size_t maskEnd = in.at + maskLen;
        if (maskLen >= 18) {
            r.hasMask = true;
            r.mTop = in.i32(), r.mLeft = in.i32(), r.mBottom = in.i32(), r.mRight = in.i32();
            r.mDefault = in.u8(), r.mFlags = in.u8();
        }
        in.at = maskEnd;
        in.need(4);
        in.at += in.u32();  // blending ranges
        const unsigned nameLen = in.u8();
        r.name = in.raw(nameLen);
        in.at += (4 - (nameLen + 1) % 4) % 4;
        while (in.at + 12 <= extraEnd) {
            const std::string sig = in.raw(4);
            if (sig != "8BIM" && sig != "8B64") break;
            const std::string key = in.raw(4);
            const std::uint32_t len = in.u32();
            const std::size_t end = in.at + len;
            if (key == "luni" && len >= 4) {
                const std::uint32_t chars = in.u32();
                std::u16string wide;
                for (std::uint32_t k = 0; k < chars && in.at + 2 <= end; ++k) wide += static_cast<char16_t>(in.u16());
                r.name = utf8(wide);
            } else if ((key == "lsct" || key == "lsdk") && len >= 4) {
                r.section = static_cast<int>(in.u32());
                if (len >= 12) {
                    in.raw(4);
                    r.sectionBlend = in.raw(4);
                }
            } else if (key == "iOpa" && len >= 1) {
                r.fill = in.u8() / 255.0;
            }
            in.at = end;
        }
        in.at = extraEnd;
    }
    // channel data, in record order
    std::vector<Layer> layers;
    int depthNow = 0;
    for (auto& r : recs) {
        const int w = r.right - r.left, h = r.bottom - r.top;
        std::map<int, std::string> data;
        for (const auto& [id, len] : r.channels) {
            const int cw = id == -2 ? r.mRight - r.mLeft : w, ch = id == -2 ? r.mBottom - r.mTop : h;
            data[id] = readChannel(in, len, cw, ch);
        }
        Layer l;
        l.name = r.name;
        l.opacity = r.opacity / 255.0;
        l.blend = blendOf(r.blend);
        l.clip = r.clip != 0;
        l.visible = !(r.flags & 2);
        l.lockAlpha = (r.flags & 1) != 0;
        l.fill = r.fill;
        if (r.section == 3) {  // the bottom of a group
            ++depthNow;
            continue;
        }
        if (r.section == 1 || r.section == 2) {
            depthNow = std::max(0, depthNow - 1);
            l.kind = LayerKind::Group;
            l.depth = depthNow;
            l.collapsed = r.section == 2;
            l.passThrough = r.blend == "pass" || r.sectionBlend == "pass";
            if (!l.passThrough && !r.sectionBlend.empty()) l.blend = blendOf(r.sectionBlend);
        } else {
            l.depth = depthNow;
            l.image = Image(out.width, out.height);
            const std::string& R = data[0];
            const std::string& G = data[1];
            const std::string& B = data[2];
            const std::string* A = data.count(-1) ? &data[-1] : nullptr;
            for (int y = 0; y < h; ++y) {
                const int cy = r.top + y;
                if (cy < 0 || cy >= out.height) continue;
                for (int x = 0; x < w; ++x) {
                    const int cx = r.left + x;
                    if (cx < 0 || cx >= out.width) continue;
                    const std::size_t k = static_cast<std::size_t>(y) * w + x;
                    std::uint8_t* p = l.image.pixel(cx, cy);
                    p[0] = k < R.size() ? static_cast<std::uint8_t>(R[k]) : 0;
                    p[1] = k < G.size() ? static_cast<std::uint8_t>(G[k]) : 0;
                    p[2] = k < B.size() ? static_cast<std::uint8_t>(B[k]) : 0;
                    p[3] = A ? (k < A->size() ? static_cast<std::uint8_t>((*A)[k]) : 0) : 255;
                }
            }
        }
        if (r.hasMask && data.count(-2)) {
            const std::string& m = data[-2];
            l.mask = Mask(out.width, out.height, static_cast<std::uint8_t>(r.mDefault));
            const int mw = r.mRight - r.mLeft;
            for (int y = r.mTop; y < r.mBottom; ++y) {
                if (y < 0 || y >= out.height) continue;
                for (int x = r.mLeft; x < r.mRight; ++x) {
                    if (x < 0 || x >= out.width) continue;
                    const std::size_t k = static_cast<std::size_t>(y - r.mTop) * mw + (x - r.mLeft);
                    if (k < m.size()) l.mask.row(y)[x] = static_cast<std::uint8_t>(m[k]);
                }
            }
            l.maskEnabled = !(r.mFlags & 2);
        }
        layers.push_back(std::move(l));
    }
    out.layers = std::move(layers);
    normalizeDepths(out.layers);
    if (out.layers.empty()) throw std::runtime_error("this PSD has no layers");
    return out;
}

LayeredImage loadPsd(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    return decodePsd(bytes);
}

}  // namespace wetwaresuite::raster
