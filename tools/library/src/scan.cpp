// Reading one file's facts: content hash, type, and the cheap measurements
// (pixel size from headers, mesh bounds, SVG canvas, ffprobe for audio and
// video, page counts for PDFs).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "achclwss/core/process.hpp"
#include "achclwss/library/library.hpp"
#include "achclwss/mesh/io.hpp"
#include "achclwss/toolkit/detect.hpp"
#include "achclwss/vector/vector.hpp"

namespace achclwss::library {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::size_t kMiB = 1024 * 1024;

std::uint64_t fnv(std::uint64_t h, const char* data, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= kFnvPrime;
    }
    return h;
}

std::string readRange(std::ifstream& in, std::uint64_t offset, std::size_t n) {
    std::string buf(n, '\0');
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(buf.data(), static_cast<std::streamsize>(n));
    buf.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return buf;
}

std::uint32_t be16(const unsigned char* p) { return (p[0] << 8) | p[1]; }
std::uint32_t le16(const unsigned char* p) { return p[0] | (p[1] << 8); }
std::uint32_t be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
std::uint32_t le32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::optional<std::pair<int, int>> sized(std::int64_t w, std::int64_t h) {
    if (w <= 0 || h <= 0 || w > 1000000 || h > 1000000) return std::nullopt;
    return std::pair<int, int>(static_cast<int>(w), static_cast<int>(h));
}

std::optional<std::pair<int, int>> jpegSize(std::ifstream& in) {
    std::uint64_t pos = 2;
    for (int guard = 0; guard < 4096; ++guard) {
        std::string m = readRange(in, pos, 9);
        if (m.size() < 4) return std::nullopt;
        auto u = reinterpret_cast<const unsigned char*>(m.data());
        if (u[0] != 0xFF) return std::nullopt;
        unsigned marker = u[1];
        if (marker == 0xFF) {  // fill byte
            ++pos;
            continue;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            pos += 2;
            continue;
        }
        std::uint32_t len = be16(u + 2);
        bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (sof) {
            if (m.size() < 9) return std::nullopt;
            return sized(be16(u + 7), be16(u + 5));
        }
        if (marker == 0xD9 || marker == 0xDA || len < 2) return std::nullopt;
        pos += 2 + len;
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>> tiffSize(std::ifstream& in, const std::string& head) {
    auto u = reinterpret_cast<const unsigned char*>(head.data());
    const bool little = head[0] == 'I';
    auto r16 = [&](const unsigned char* p) { return little ? le16(p) : be16(p); };
    auto r32 = [&](const unsigned char* p) { return little ? le32(p) : be32(p); };
    std::uint32_t ifd = r32(u + 4);
    std::string dir = readRange(in, ifd, 2);
    if (dir.size() < 2) return std::nullopt;
    std::uint32_t count = r16(reinterpret_cast<const unsigned char*>(dir.data()));
    if (count > 4096) return std::nullopt;
    std::string entries = readRange(in, ifd + 2, count * 12);
    std::int64_t w = 0, h = 0;
    for (std::uint32_t i = 0; i + 1 <= count && (i + 1) * 12 <= entries.size(); ++i) {
        auto e = reinterpret_cast<const unsigned char*>(entries.data()) + i * 12;
        std::uint32_t tag = r16(e), type = r16(e + 2);
        std::uint32_t value = type == 3 ? r16(e + 8) : r32(e + 8);
        if (tag == 256) w = value;
        if (tag == 257) h = value;
    }
    return sized(w, h);
}

}  // namespace

std::string hex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::uint64_t parseHex64(const std::string& s) {
    std::uint64_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint64_t>(c - 'A' + 10);
    }
    return v;
}

std::string fnv1a64Hex(const std::string& bytes) { return hex64(fnv(kFnvOffset, bytes.data(), bytes.size())); }

std::string contentHash(const fs::path& path, bool full) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    std::error_code ec;
    const std::uint64_t size = fs::file_size(path, ec);
    std::uint64_t h = kFnvOffset;
    std::vector<char> buf(kMiB);
    if (full || size <= 2 * kMiB) {
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            h = fnv(h, buf.data(), static_cast<std::size_t>(in.gcount()));
        }
    } else {
        std::string first = readRange(in, 0, kMiB);
        std::string last = readRange(in, size - kMiB, kMiB);
        h = fnv(h, first.data(), first.size());
        h = fnv(h, last.data(), last.size());
        char sz[8];
        for (int i = 0; i < 8; ++i) sz[i] = static_cast<char>((size >> (8 * i)) & 0xFF);
        h = fnv(h, sz, 8);
    }
    return hex64(h);
}

std::optional<std::pair<int, int>> imageHeaderSize(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string head = readRange(in, 0, 64);
    if (head.size() < 16) return std::nullopt;
    auto u = reinterpret_cast<const unsigned char*>(head.data());
    const std::string ext = extOf(path);
    if (head.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 && head.size() >= 24) return sized(be32(u + 16), be32(u + 20));
    if (u[0] == 0xFF && u[1] == 0xD8) return jpegSize(in);
    if (head.compare(0, 4, "GIF8") == 0) return sized(le16(u + 6), le16(u + 8));
    if (head.compare(0, 2, "BM") == 0 && head.size() >= 26) {
        auto h = static_cast<std::int32_t>(le32(u + 22));
        return sized(static_cast<std::int32_t>(le32(u + 18)), h < 0 ? -static_cast<std::int64_t>(h) : h);
    }
    if (head.compare(0, 4, "RIFF") == 0 && head.compare(8, 4, "WEBP") == 0 && head.size() >= 30) {
        if (head.compare(12, 4, "VP8 ") == 0) return sized(le16(u + 26) & 0x3FFF, le16(u + 28) & 0x3FFF);
        if (head.compare(12, 4, "VP8L") == 0) {
            std::uint32_t b = le32(u + 21);
            return sized((b & 0x3FFF) + 1, ((b >> 14) & 0x3FFF) + 1);
        }
        if (head.compare(12, 4, "VP8X") == 0) {
            return sized(1 + (u[24] | (u[25] << 8) | (u[26] << 16)), 1 + (u[27] | (u[28] << 8) | (u[29] << 16)));
        }
    }
    if (head.compare(0, 4, "8BPS") == 0 && head.size() >= 26) return sized(be32(u + 18), be32(u + 14));
    if (head.compare(0, 4, "qoif") == 0) return sized(be32(u + 4), be32(u + 8));
    if (head.compare(0, 4, "DDS ") == 0 && head.size() >= 20) return sized(le32(u + 16), le32(u + 12));
    if ((head.compare(0, 4, "II*\0", 4) == 0) || (head.compare(0, 4, "MM\0*", 4) == 0)) return tiffSize(in, head);
    if (head[0] == 'P' && head[1] >= '1' && head[1] <= '7') {
        // P6\n# comment\n640 480\n255
        std::string text = readRange(in, 0, 1024);
        std::size_t i = 2;
        std::int64_t nums[2] = {0, 0};
        for (int n = 0; n < 2 && i < text.size();) {
            char c = text[i];
            if (c == '#') {
                while (i < text.size() && text[i] != '\n') ++i;
            } else if (c >= '0' && c <= '9') {
                std::int64_t v = 0;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9') v = v * 10 + (text[i++] - '0');
                nums[n++] = v;
                continue;
            }
            ++i;
        }
        return sized(nums[0], nums[1]);
    }
    if (ext == "tga" && head.size() >= 18) return sized(le16(u + 12), le16(u + 14));
    return std::nullopt;
}

namespace {

// ffprobe: "duration=12.5" and "width=1920" lines.
void ffprobeInto(const fs::path& path, Item& item) {
    static const auto exe = findProgram("ffprobe");
    if (!exe) return;
    auto r = runProgram({exe->string(), "-v", "error", "-show_entries", "format=duration:stream=codec_type,width,height",
                         "-of", "default=noprint_wrappers=1", path.string()});
    if (r.exitCode != 0) return;
    std::size_t start = 0;
    bool video = false;
    while (start < r.output.size()) {
        auto end = r.output.find('\n', start);
        std::string line = r.output.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? r.output.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        try {
            if (key == "codec_type") video = value == "video";
            else if (key == "width" && video && item.width == 0) item.width = std::stoi(value);
            else if (key == "height" && video && item.height == 0) item.height = std::stoi(value);
            else if (key == "duration" && value != "N/A") item.duration = std::stod(value);
        } catch (const std::exception&) {
        }
    }
    // a still image probed by ffmpeg reports a tiny bogus duration
    if (item.type == "image" || item.type == "raw" || item.type == "texture") item.duration = 0;
}

int pdfPages(const fs::path& path, std::uint64_t size) {
    static const auto exe = findProgram("pdfinfo");
    if (exe) {
        auto r = runProgram({exe->string(), path.string()});
        auto pos = r.output.find("Pages:");
        if (r.exitCode == 0 && pos != std::string::npos) {
            try {
                return std::stoi(r.output.substr(pos + 6));
            } catch (const std::exception&) {
            }
        }
    }
    if (size > 64 * kMiB) return 0;
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // count page objects: "/Type /Page" not followed by "s"
    int pages = 0;
    for (std::size_t pos = text.find("/Type"); pos != std::string::npos; pos = text.find("/Type", pos + 5)) {
        std::size_t i = pos + 5;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\r' || text[i] == '\n')) ++i;
        if (text.compare(i, 5, "/Page") == 0 && (i + 5 >= text.size() || text[i + 5] != 's')) ++pages;
    }
    return pages;
}

}  // namespace

Item readItem(const fs::path& path, bool probe, bool fullHash) {
    Item item;
    item.path = normPath(path);
    std::error_code ec;
    item.size = fs::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot read " + path.string() + ": " + ec.message());
    auto t = fs::last_write_time(path, ec);
    item.mtick = static_cast<std::int64_t>(t.time_since_epoch().count());
    item.mtime = unixSeconds(t);
    item.ext = extOf(path);
    item.hash = contentHash(path, fullHash);
    try {
        auto type = toolkit::detectFile(path);
        item.type = type.category;
        item.format = type.format;
    } catch (const std::exception&) {
        item.type = "binary";
    }
    item.indexed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    if (item.type == "image" || item.type == "raw" || item.type == "texture") {
        if (auto wh = imageHeaderSize(path)) {
            item.width = wh->first;
            item.height = wh->second;
        } else if (probe) {
            ffprobeInto(path, item);
        }
    } else if (item.type == "vector" && item.ext == "svg" && item.size <= 16 * kMiB) {
        try {
            auto doc = vector::openSvg(path);
            item.width = doc->width;
            item.height = doc->height;
        } catch (const std::exception&) {
        }
    } else if ((item.type == "audio" || item.type == "video") && probe) {
        ffprobeInto(path, item);
    } else if (item.ext == "pdf" && probe) {
        item.pages = pdfPages(path, item.size);
    }
    if (probe && mesh::canRead(path) && item.size <= 256 * kMiB) {
        try {
            auto m = mesh::load(path);
            auto b = mesh::bounds(m);
            item.triangles = static_cast<std::int64_t>(m.triangleCount());
            auto s = b.size();
            item.bounds = {s.x, s.y, s.z};
        } catch (const std::exception&) {
        }
    }
    return item;
}

}  // namespace achclwss::library
