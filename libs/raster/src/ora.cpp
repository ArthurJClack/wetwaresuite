#include "wetwaresuite/raster/ora.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
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

const std::map<std::string, Blend>& oraOps() {
    static const std::map<std::string, Blend> ops = {
        {"svg:src-over", Blend::Normal},   {"svg:multiply", Blend::Multiply}, {"svg:screen", Blend::Screen},
        {"svg:overlay", Blend::Overlay},   {"svg:plus", Blend::Add},          {"svg:darken", Blend::Darken},
        {"svg:lighten", Blend::Lighten},   {"svg:difference", Blend::Difference}};
    return ops;
}

std::string oraOp(Blend b) {
    for (const auto& [name, blend] : oraOps()) {
        if (blend == b) return name;
    }
    return "svg:src-over";
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

void saveOra(const LayeredImage& img, const std::filesystem::path& path) {
    std::vector<ZipEntry> entries;
    entries.push_back({"mimetype", "image/openraster"});  // must come first, stored
    std::string stack = "<?xml version='1.0' encoding='UTF-8'?>\n<image version=\"0.0.5\" w=\"" +
                        std::to_string(img.width) + "\" h=\"" + std::to_string(img.height) + "\">\n<stack>\n";
    std::vector<ZipEntry> layerFiles;
    for (std::size_t i = img.layers.size(); i-- > 0;) {  // ORA lists the top layer first
        const auto& layer = img.layers[i];
        std::string src = "data/layer" + std::to_string(i) + ".png";
        stack += "<layer name=\"" + xmlEscape(layer.name) + "\" src=\"" + src + "\" x=\"0\" y=\"0\" opacity=\"" +
                 std::to_string(layer.opacity) + "\" visibility=\"" + (layer.visible ? "visible" : "hidden") +
                 "\" composite-op=\"" + oraOp(layer.blend) + "\"";
        // masks and clipping aren't in the ORA spec: kept in our own attributes, other apps show the layer unmasked
        if (!layer.mask.empty()) {
            const std::string maskSrc = "data/mask" + std::to_string(i) + ".png";
            stack += " wetwaresuite-mask=\"" + maskSrc + "\"";
            if (!layer.maskEnabled) stack += " wetwaresuite-mask-enabled=\"false\"";
            Image gray(layer.mask.width, layer.mask.height);
            for (int y = 0; y < gray.height(); ++y) {
                for (int x = 0; x < gray.width(); ++x) gray.set(x, y, {layer.mask.at(x, y), layer.mask.at(x, y), layer.mask.at(x, y), 255});
            }
            layerFiles.push_back({maskSrc, encodePng(gray)});
        }
        if (layer.clip) stack += " wetwaresuite-clip=\"true\"";
        stack += "/>\n";
        layerFiles.push_back({src, encodePng(layer.image)});
    }
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
    for (auto pos = xml.find("<layer"); pos != std::string::npos; pos = xml.find("<layer", pos + 1)) {
        std::string t = xml.substr(pos, xml.find('>', pos) - pos);
        std::string src = attr(t, "src");
        auto file = files.find(src);
        if (file == files.end()) throw std::runtime_error("layer file missing: " + src);
        Layer layer;
        layer.name = attr(t, "name");
        layer.opacity = attrNumber(t, "opacity", 1.0);
        layer.visible = attr(t, "visibility") != "hidden";
        auto op = oraOps().find(attr(t, "composite-op"));
        layer.blend = op == oraOps().end() ? Blend::Normal : op->second;
        Image png = decode(file->second);
        int x = static_cast<int>(attrNumber(t, "x", 0)), y = static_cast<int>(attrNumber(t, "y", 0));
        layer.image = Image(out.width, out.height);
        blendOnto(layer.image, png, x, y, Blend::Normal, 1.0);  // place at its offset on a full-size layer
        if (std::string maskSrc = attr(t, "wetwaresuite-mask"); !maskSrc.empty()) {
            auto mf = files.find(maskSrc);
            if (mf != files.end()) {
                Image gray = decode(mf->second);
                layer.mask = Mask(out.width, out.height);
                for (int my = 0; my < std::min(out.height, gray.height()); ++my) {
                    for (int mx = 0; mx < std::min(out.width, gray.width()); ++mx) layer.mask.row(my)[mx] = gray.pixel(mx, my)[0];
                }
                layer.maskEnabled = attr(t, "wetwaresuite-mask-enabled") != "false";
            }
        }
        layer.clip = attr(t, "wetwaresuite-clip") == "true";
        topFirst.push_back(std::move(layer));
    }
    out.layers.assign(topFirst.rbegin(), topFirst.rend());
    if (out.layers.empty()) out.layers.push_back({"Background", Image(out.width, out.height)});
    return out;
}

}  // namespace wetwaresuite::raster
