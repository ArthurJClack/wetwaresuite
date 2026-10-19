#include "wetwaresuite/raster/ora.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

#include "wetwaresuite/raster/codec.hpp"
#include "stb_image.h"

namespace wetwaresuite::raster {

namespace {

std::uint32_t crc32(const std::string& data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(std::string& s, std::uint32_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
}
void put32(std::string& s, std::uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}
std::uint32_t get16(const std::string& s, std::size_t at) {
    if (at + 2 > s.size()) throw std::runtime_error("corrupt zip");
    return static_cast<unsigned char>(s[at]) | (static_cast<unsigned char>(s[at + 1]) << 8);
}
std::uint32_t get32(const std::string& s, std::size_t at) { return get16(s, at) | (get16(s, at + 2) << 16); }

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string xmlUnescape(const std::string& s) {
    std::string out = s;
    const std::pair<const char*, const char*> table[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [from, to] : table) {
        for (auto pos = out.find(from); pos != std::string::npos; pos = out.find(from, pos + 1)) {
            out.replace(pos, std::char_traits<char>::length(from), to);
        }
    }
    return out;
}

// Attribute value from a tag's text, e.g. attr("<layer name=\"a\"", "name") == "a".
std::string attr(const std::string& tag, const std::string& name) {
    for (std::size_t pos = tag.find(name + "="); pos != std::string::npos; pos = tag.find(name + "=", pos + 1)) {
        if (pos > 0 && !std::isspace(static_cast<unsigned char>(tag[pos - 1]))) continue;
        std::size_t q = pos + name.size() + 1;
        if (q >= tag.size()) return {};
        char quote = tag[q];
        std::size_t end = tag.find(quote, q + 1);
        if (end == std::string::npos) return {};
        return xmlUnescape(tag.substr(q + 1, end - q - 1));
    }
    return {};
}

double attrNumber(const std::string& tag, const std::string& name, double fallback) {
    std::string v = attr(tag, name);
    if (v.empty()) return fallback;
    try {
        return std::stod(v);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

std::string writeZip(const std::vector<ZipEntry>& entries) {
    std::string out, central;
    for (const auto& e : entries) {
        const std::uint32_t crc = crc32(e.data);
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto size = static_cast<std::uint32_t>(e.data.size());
        put32(out, 0x04034b50);
        put16(out, 10);  // version needed
        put16(out, 0);   // flags
        put16(out, 0);   // stored
        put16(out, 0);   // time
        put16(out, 0x21);  // date: 1980-01-01
        put32(out, crc);
        put32(out, size);
        put32(out, size);
        put16(out, static_cast<std::uint32_t>(e.name.size()));
        put16(out, 0);
        out += e.name;
        out += e.data;

        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 10);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crc);
        put32(central, size);
        put32(central, size);
        put16(central, static_cast<std::uint32_t>(e.name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central += e.name;
    }
    const auto centralOffset = static_cast<std::uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, static_cast<std::uint32_t>(central.size()));
    put32(out, centralOffset);
    put16(out, 0);
    return out;
}

std::vector<ZipEntry> readZip(const std::string& z) {
    if (z.size() < 22) throw std::runtime_error("not a zip file");
    std::size_t eocd = std::string::npos;
    const std::size_t start = z.size() > 22 + 65535 ? z.size() - 22 - 65535 : 0;  // EOCD comment is at most 64 KiB
    for (std::size_t i = z.size() - 22 + 1; i-- > start;) {
        if (get32(z, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) throw std::runtime_error("not a zip file");
    const std::uint32_t count = get16(z, eocd + 10);
    std::size_t at = get32(z, eocd + 16);
    std::vector<ZipEntry> entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (get32(z, at) != 0x02014b50) throw std::runtime_error("corrupt zip directory");
        const std::uint32_t method = get16(z, at + 10);
        const std::uint32_t compSize = get32(z, at + 20);
        const std::uint32_t size = get32(z, at + 24);
        const std::uint32_t nameLen = get16(z, at + 28), extraLen = get16(z, at + 30), commentLen = get16(z, at + 32);
        const std::uint32_t local = get32(z, at + 42);
        if (at + 46 + nameLen > z.size()) throw std::runtime_error("corrupt zip directory");
        ZipEntry e;
        e.name = z.substr(at + 46, nameLen);
        at += 46 + nameLen + extraLen + commentLen;

        if (get32(z, local) != 0x04034b50) throw std::runtime_error("corrupt zip entry " + e.name);
        const std::size_t dataAt = local + 30 + get16(z, local + 26) + get16(z, local + 28);
        if (dataAt + compSize > z.size()) throw std::runtime_error("truncated zip entry " + e.name);
        if (method == 0) {
            e.data = z.substr(dataAt, compSize);
        } else if (method == 8) {
            int outLen = 0;
            char* raw = stbi_zlib_decode_noheader_malloc(z.data() + dataAt, static_cast<int>(compSize), &outLen);
            if (!raw) throw std::runtime_error("cannot inflate zip entry " + e.name);
            e.data.assign(raw, static_cast<std::size_t>(outLen));
            std::free(raw);
            if (e.data.size() != size) throw std::runtime_error("zip entry " + e.name + " has the wrong size");
        } else {
            throw std::runtime_error("unsupported zip compression in " + e.name);
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

namespace {

// ORA has the SVG compositing ops; our other modes go in wetwaresuite-blend (other apps show them as normal).
const std::map<Blend, std::string>& svgOps() {
    static const std::map<Blend, std::string> ops = {
        {Blend::Normal, "svg:src-over"},       {Blend::Multiply, "svg:multiply"},     {Blend::Screen, "svg:screen"},
        {Blend::Overlay, "svg:overlay"},       {Blend::Add, "svg:plus"},              {Blend::Darken, "svg:darken"},
        {Blend::Lighten, "svg:lighten"},       {Blend::Difference, "svg:difference"}, {Blend::ColorBurn, "svg:color-burn"},
        {Blend::ColorDodge, "svg:color-dodge"}, {Blend::SoftLight, "svg:soft-light"}, {Blend::HardLight, "svg:hard-light"},
        {Blend::Exclusion, "svg:exclusion"},   {Blend::Hue, "svg:hue"},               {Blend::Saturation, "svg:saturation"},
        {Blend::Color, "svg:color"},           {Blend::Luminosity, "svg:luminosity"}};
    return ops;
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::string escapeAttr(const std::string& s) {
    std::string out;
    for (char c : xmlEscape(s)) {
        if (c == '\n') out += "&#10;";
        else if (c == '\t') out += "&#9;";
        else out += c;
    }
    return out;
}

std::string unescapeAttr(std::string s) {
    for (auto [from, to] : {std::pair<const char*, const char*>{"&#10;", "\n"}, {"&#9;", "\t"}}) {
        for (auto pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + 1)) s.replace(pos, std::strlen(from), to);
    }
    return s;
}

Image maskPicture(const Mask& m) {
    Image gray(m.width, m.height);
    for (int y = 0; y < gray.height(); ++y) {
        for (int x = 0; x < gray.width(); ++x) gray.set(x, y, {m.at(x, y), m.at(x, y), m.at(x, y), 255});
    }
    return gray;
}

// The attributes every layer and group shares: name, opacity, visibility, blend, plus ours.
std::string commonAttrs(const Layer& l, std::size_t i, std::vector<ZipEntry>& files) {
    auto op = svgOps().find(l.blend);
    std::string a = " name=\"" + escapeAttr(l.name) + "\" opacity=\"" + num(l.opacity) + "\" visibility=\"" + (l.visible ? "visible" : "hidden") +
                    "\" composite-op=\"" + (op == svgOps().end() ? "svg:src-over" : op->second) + "\"";
    if (op == svgOps().end()) a += " wetwaresuite-blend=\"" + toString(l.blend) + "\"";
    // masks, clipping, styles and the rest aren't in the ORA spec: our own attributes, other apps skip them
    if (!l.mask.empty()) {
        const std::string maskSrc = "data/mask" + std::to_string(i) + ".png";
        a += " wetwaresuite-mask=\"" + maskSrc + "\"";
        if (!l.maskEnabled) a += " wetwaresuite-mask-enabled=\"false\"";
        files.push_back({maskSrc, encodePng(maskPicture(l.mask))});
    }
    if (l.clip) a += " wetwaresuite-clip=\"true\"";
    if (l.fill != 1.0) a += " wetwaresuite-fill=\"" + num(l.fill) + "\"";
    if (l.lockAlpha) a += " wetwaresuite-lock-alpha=\"true\"";
    if (l.kind != LayerKind::Pixel && l.kind != LayerKind::Group) a += " wetwaresuite-kind=\"" + toString(l.kind) + "\"";
    for (const auto& [k, v] : l.settings) a += " wetwaresuite-set-" + k + "=\"" + escapeAttr(v) + "\"";
    if (l.style.any()) a += " wetwaresuite-style=\"" + escapeAttr(styleToText(l.style)) + "\"";
    return a;
}

// Writes layers [lo, hi) top first; groups become nested <stack>s, as in the ORA spec.
void writeStack(const LayeredImage& img, int lo, int hi, std::string& xml, std::vector<ZipEntry>& files) {
    for (int i = hi - 1; i >= lo; --i) {
        const Layer& l = img.layers[static_cast<std::size_t>(i)];
        const auto idx = static_cast<std::size_t>(i);
        if (l.kind == LayerKind::Group) {
            xml += "<stack" + commonAttrs(l, idx, files) + " isolation=\"" + (l.passThrough ? "auto" : "isolate") + "\"" +
                   (l.collapsed ? " wetwaresuite-collapsed=\"true\"" : "") + ">\n";
            const int start = groupStart(img.layers, i);
            writeStack(img, start, i, xml, files);
            xml += "</stack>\n";
            i = start;
            continue;
        }
        const std::string src = "data/layer" + std::to_string(i) + ".png";
        xml += "<layer" + commonAttrs(l, idx, files) + " src=\"" + src + "\" x=\"0\" y=\"0\"/>\n";
        // adjustment layers have no pixels: a 1x1 clear picture keeps other apps happy
        files.push_back({src, encodePng(l.image.empty() ? Image(1, 1) : l.image)});
    }
}

void readCommon(const std::string& t, Layer& layer, const std::map<std::string, std::string>& files, int width, int height) {
    layer.name = unescapeAttr(attr(t, "name"));
    layer.opacity = attrNumber(t, "opacity", 1.0);
    layer.visible = attr(t, "visibility") != "hidden";
    layer.blend = Blend::Normal;
    for (const auto& [b, name] : svgOps()) {
        if (name == attr(t, "composite-op")) layer.blend = b;
    }
    if (auto b = parseBlend(attr(t, "wetwaresuite-blend"))) layer.blend = *b;
    if (std::string maskSrc = attr(t, "wetwaresuite-mask"); !maskSrc.empty()) {
        auto mf = files.find(maskSrc);
        if (mf != files.end()) {
            Image gray = decode(mf->second);
            layer.mask = Mask(width, height);
            for (int my = 0; my < std::min(height, gray.height()); ++my) {
                for (int mx = 0; mx < std::min(width, gray.width()); ++mx) layer.mask.row(my)[mx] = gray.pixel(mx, my)[0];
            }
            layer.maskEnabled = attr(t, "wetwaresuite-mask-enabled") != "false";
        }
    }
    layer.clip = attr(t, "wetwaresuite-clip") == "true";
    layer.fill = attrNumber(t, "wetwaresuite-fill", 1.0);
    layer.lockAlpha = attr(t, "wetwaresuite-lock-alpha") == "true";
    if (auto k = parseLayerKind(attr(t, "wetwaresuite-kind"))) layer.kind = *k;
    for (std::size_t pos = t.find(" wetwaresuite-set-"); pos != std::string::npos; pos = t.find(" wetwaresuite-set-", pos + 1)) {
        const std::size_t keyAt = pos + 18, eq = t.find('=', keyAt);
        if (eq == std::string::npos) break;
        const std::string key = t.substr(keyAt, eq - keyAt);
        layer.settings[key] = unescapeAttr(attr(t, "wetwaresuite-set-" + key));
    }
    if (std::string st = attr(t, "wetwaresuite-style"); !st.empty()) {
        std::string error;
        if (!styleFromText(unescapeAttr(st), layer.style, error)) layer.style = {};
    }
}

}  // namespace

void saveOra(const LayeredImage& img, const std::filesystem::path& path) {
    std::vector<ZipEntry> entries;
    entries.push_back({"mimetype", "image/openraster"});  // must come first, stored
    std::string stack = "<?xml version='1.0' encoding='UTF-8'?>\n<image version=\"0.0.5\" w=\"" + std::to_string(img.width) + "\" h=\"" +
                        std::to_string(img.height) + "\">\n<stack>\n";
    std::vector<ZipEntry> layerFiles;
    writeStack(img, 0, static_cast<int>(img.layers.size()), stack, layerFiles);
    stack += "</stack>\n</image>\n";
    entries.push_back({"stack.xml", stack});
    for (auto& f : layerFiles) entries.push_back(std::move(f));
    Image merged = composite(img.layers, img.width, img.height);
    entries.push_back({"mergedimage.png", encodePng(merged)});
    entries.push_back({"Thumbnails/thumbnail.png", previewPng(merged, 256)});
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    std::string zip = writeZip(entries);
    out.write(zip.data(), static_cast<std::streamsize>(zip.size()));
}

LayeredImage loadOra(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    std::map<std::string, std::string> files;
    for (auto& e : readZip(bytes)) files[e.name] = std::move(e.data);
    auto stackIt = files.find("stack.xml");
    if (stackIt == files.end()) throw std::runtime_error(path.string() + " has no stack.xml, so it isn't an OpenRaster file");
    const std::string& xml = stackIt->second;
    LayeredImage out;
    auto imageTag = xml.find("<image");
    if (imageTag == std::string::npos) throw std::runtime_error("stack.xml has no <image>");
    std::string tag = xml.substr(imageTag, xml.find('>', imageTag) - imageTag);
    out.width = static_cast<int>(attrNumber(tag, "w", 0));
    out.height = static_cast<int>(attrNumber(tag, "h", 0));
    if (out.width <= 0 || out.height <= 0) throw std::runtime_error("stack.xml has no image size");
    std::vector<Layer> topFirst;
    int open = 0;  // stacks open; the first is the image's root stack
    for (std::size_t pos = xml.find('<', imageTag + 1); pos != std::string::npos; pos = xml.find('<', pos + 1)) {
        const std::size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        const std::string t = xml.substr(pos, end - pos);
        if (t.rfind("</stack", 0) == 0) {
            open = std::max(0, open - 1);
            continue;
        }
        const bool isStack = t.rfind("<stack", 0) == 0 && (t.size() == 6 || std::isspace(static_cast<unsigned char>(t[6])));
        const bool isLayer = t.rfind("<layer", 0) == 0 && (t.size() == 6 || std::isspace(static_cast<unsigned char>(t[6])));
        const bool selfClosing = !t.empty() && t.back() == '/';
        if (isStack) {
            if (open++ == 0) continue;  // the root
            Layer group;
            readCommon(t, group, files, out.width, out.height);
            group.kind = LayerKind::Group;
            group.depth = open - 2;
            group.passThrough = attr(t, "isolation") != "isolate";
            group.collapsed = attr(t, "wetwaresuite-collapsed") == "true";
            topFirst.push_back(std::move(group));
            if (selfClosing) --open;
            continue;
        }
        if (!isLayer) continue;
        std::string src = attr(t, "src");
        auto file = files.find(src);
        if (file == files.end()) throw std::runtime_error("layer file missing: " + src);
        Layer layer;
        readCommon(t, layer, files, out.width, out.height);
        layer.depth = std::max(0, open - 1);
        if (layer.kind != LayerKind::Adjustment) {
            Image png = decode(file->second);
            int x = static_cast<int>(attrNumber(t, "x", 0)), y = static_cast<int>(attrNumber(t, "y", 0));
            layer.image = Image(out.width, out.height);
            blendOnto(layer.image, png, x, y, Blend::Normal, 1.0);  // place at its offset on a full-size layer
        }
        topFirst.push_back(std::move(layer));
    }
    out.layers.assign(std::make_move_iterator(topFirst.rbegin()), std::make_move_iterator(topFirst.rend()));
    normalizeDepths(out.layers);
    if (out.layers.empty()) out.layers.push_back({"Background", Image(out.width, out.height)});
    return out;
}

}  // namespace wetwaresuite::raster
