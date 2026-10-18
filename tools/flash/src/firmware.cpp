// Firmware file formats and the checksums the bootloaders use.
#include "wetwaresuite/flash/firmware.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace wetwaresuite::flash {

namespace {

std::uint16_t le16(const Bytes& b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}
std::uint32_t le32(const Bytes& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string machineName(std::uint16_t m) {
    switch (m) {
    case 3: return "x86";
    case 8: return "MIPS";
    case 40: return "ARM";
    case 62: return "x86-64";
    case 83: return "AVR";
    case 94: return "Xtensa";
    case 183: return "AArch64";
    case 243: return "RISC-V";
    default: return "machine " + std::to_string(m);
    }
}

}  // namespace

// ---- checksums ------------------------------------------------------------------------

std::uint8_t hexChecksum(const std::uint8_t* data, std::size_t size) {
    unsigned sum = 0;
    for (std::size_t i = 0; i < size; ++i) sum += data[i];
    return static_cast<std::uint8_t>((0x100u - (sum & 0xFFu)) & 0xFFu);
}

std::uint8_t xorBytes(const std::uint8_t* data, std::size_t size, std::uint8_t seed) {
    std::uint8_t x = seed;
    for (std::size_t i = 0; i < size; ++i) x = static_cast<std::uint8_t>(x ^ data[i]);
    return x;
}

std::uint8_t espChecksum(const std::uint8_t* data, std::size_t size, std::uint8_t seed) { return xorBytes(data, size, seed); }

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// RFC 1321.
Bytes md5(const std::uint8_t* data, std::size_t size) {
    static const std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
        0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
        0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
        0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    Bytes msg(data, data + size);
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8u;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; ++i) msg.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFFu));
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[16];
        for (int i = 0; i < 16; ++i) w[i] = le32(msg, off + static_cast<std::size_t>(i) * 4);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const std::uint32_t tmp = d;
            d = c;
            c = b;
            const std::uint32_t x = a + f + K[i] + w[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = tmp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    Bytes out;
    for (std::uint32_t v : h) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    return out;
}

std::string md5Hex(const std::uint8_t* data, std::size_t size) {
    Bytes d = md5(data, size);
    std::string s;
    char buf[3];
    for (auto b : d) {
        std::snprintf(buf, sizeof buf, "%02x", b);
        s += buf;
    }
    return s;
}

std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

std::uint32_t parseAddress(const std::string& text) {
    std::string t = lower(text);
    t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return c == '_' || std::isspace(static_cast<unsigned char>(c)); }), t.end());
    if (t.empty()) throw FirmwareError("empty address");
    std::uint64_t mult = 1;
    if (t.back() == 'k') {
        mult = 1024;
        t.pop_back();
    } else if (t.back() == 'm') {
        mult = 1024 * 1024;
        t.pop_back();
    }
    int base = 10;
    if (t.size() > 2 && t[0] == '0' && t[1] == 'x') {
        base = 16;
        t = t.substr(2);
    }
    if (t.empty()) throw FirmwareError("bad address: " + text);
    std::uint64_t v = 0;
    for (char c : t) {
        int d = hexDigit(c);
        if (d < 0 || d >= base) throw FirmwareError("bad address: " + text);
        v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
        if (v > 0xFFFFFFFFull) throw FirmwareError("address too large: " + text);
    }
    v *= mult;
    if (v > 0xFFFFFFFFull) throw FirmwareError("address too large: " + text);
    return static_cast<std::uint32_t>(v);
}

// ---- segments ----------------------------------------------------------------------------

std::vector<Segment> normalizeSegments(std::vector<Segment> segments) {
    segments.erase(std::remove_if(segments.begin(), segments.end(), [](const Segment& s) { return s.data.empty(); }), segments.end());
    std::stable_sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) { return a.address < b.address; });
    std::vector<Segment> out;
    for (auto& s : segments) {
        if (!out.empty()) {
            Segment& last = out.back();
            const std::uint64_t lastEnd = static_cast<std::uint64_t>(last.address) + last.data.size();
            if (s.address < lastEnd) throw FirmwareError("overlapping data at " + hex32(s.address));
            if (s.address == lastEnd) {
                last.data.insert(last.data.end(), s.data.begin(), s.data.end());
                continue;
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::size_t Firmware::byteCount() const {
    std::size_t n = 0;
    for (const auto& s : segments) n += s.data.size();
    return n;
}
std::uint32_t Firmware::lowAddress() const { return segments.empty() ? 0 : segments.front().address; }
std::uint32_t Firmware::highAddress() const { return segments.empty() ? 0 : segments.back().end(); }

Bytes Firmware::flatten(std::uint8_t fill) const {
    if (segments.empty()) return {};
    const std::uint32_t lo = lowAddress();
    Bytes out(highAddress() - lo, fill);
    for (const auto& s : segments) std::copy(s.data.begin(), s.data.end(), out.begin() + (s.address - lo));
    return out;
}

// ---- Intel HEX ------------------------------------------------------------------------------

Firmware parseIntelHex(const std::string& text) {
    Firmware fw;
    fw.format = "ihex";
    std::vector<Segment> segs;
    std::uint32_t base = 0;
    bool sawEof = false;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        std::size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) ++start;
        if (start == line.size()) continue;
        if (sawEof) continue;  // tools sometimes append junk after the EOF record
        if (line[start] != ':') throw FirmwareError("line " + std::to_string(lineNo) + ": doesn't start with ':'");
        const std::string hex = line.substr(start + 1);
        if (hex.size() % 2 != 0 || hex.size() < 10) throw FirmwareError("line " + std::to_string(lineNo) + ": bad record length");
        Bytes rec;
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            int hi = hexDigit(hex[i]), lo = hexDigit(hex[i + 1]);
            if (hi < 0 || lo < 0) throw FirmwareError("line " + std::to_string(lineNo) + ": not hex");
            rec.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        }
        const std::size_t count = rec[0];
        if (rec.size() != count + 5) throw FirmwareError("line " + std::to_string(lineNo) + ": byte count doesn't match");
        if (hexChecksum(rec.data(), rec.size() - 1) != rec.back())
            throw FirmwareError("line " + std::to_string(lineNo) + ": checksum mismatch (file corrupt?)");
        const std::uint32_t off = static_cast<std::uint32_t>((rec[1] << 8) | rec[2]);
        const std::uint8_t type = rec[3];
        const std::uint8_t* d = rec.data() + 4;
        switch (type) {
        case 0x00: {
            const std::uint32_t addr = base + off;
            if (!segs.empty() && segs.back().end() == addr) {
                segs.back().data.insert(segs.back().data.end(), d, d + count);
            } else {
                segs.push_back({addr, Bytes(d, d + count)});
            }
            break;
        }
        case 0x01: sawEof = true; break;
        case 0x02:
            if (count != 2) throw FirmwareError("line " + std::to_string(lineNo) + ": bad segment address record");
            base = static_cast<std::uint32_t>((d[0] << 8) | d[1]) << 4;
            break;
        case 0x03:
            if (count != 4) throw FirmwareError("line " + std::to_string(lineNo) + ": bad start segment record");
            fw.entry = (static_cast<std::uint32_t>((d[0] << 8) | d[1]) << 4) + static_cast<std::uint32_t>((d[2] << 8) | d[3]);
            break;
        case 0x04:
            if (count != 2) throw FirmwareError("line " + std::to_string(lineNo) + ": bad linear address record");
            base = static_cast<std::uint32_t>((d[0] << 8) | d[1]) << 16;
            break;
        case 0x05:
            if (count != 4) throw FirmwareError("line " + std::to_string(lineNo) + ": bad start linear record");
            fw.entry = (static_cast<std::uint32_t>(d[0]) << 24) | (static_cast<std::uint32_t>(d[1]) << 16) |
                       (static_cast<std::uint32_t>(d[2]) << 8) | d[3];
            break;
        default: throw FirmwareError("line " + std::to_string(lineNo) + ": unknown record type " + std::to_string(type));
        }
    }
    if (!sawEof && segs.empty()) throw FirmwareError("no Intel HEX records found");
    fw.segments = normalizeSegments(std::move(segs));
    return fw;
}

std::string toIntelHex(const std::vector<Segment>& segments, std::size_t perLine) {
    std::string out;
    char buf[8];
    auto record = [&](std::uint8_t type, std::uint16_t addr, const std::uint8_t* d, std::size_t n) {
        Bytes rec = {static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(addr >> 8), static_cast<std::uint8_t>(addr & 0xFF), type};
        rec.insert(rec.end(), d, d + n);
        rec.push_back(hexChecksum(rec.data(), rec.size()));
        out += ':';
        for (auto b : rec) {
            std::snprintf(buf, sizeof buf, "%02X", b);
            out += buf;
        }
        out += '\n';
    };
    std::uint32_t upper = 0;
    for (const auto& s : segments) {
        for (std::size_t i = 0; i < s.data.size();) {
            const std::uint32_t addr = s.address + static_cast<std::uint32_t>(i);
            if ((addr >> 16) != upper) {
                upper = addr >> 16;
                const std::uint8_t ext[2] = {static_cast<std::uint8_t>(upper >> 8), static_cast<std::uint8_t>(upper & 0xFF)};
                record(0x04, 0, ext, 2);
            }
            std::size_t n = std::min(perLine, s.data.size() - i);
            const std::uint32_t toBoundary = 0x10000u - (addr & 0xFFFFu);
            n = std::min<std::size_t>(n, toBoundary);
            record(0x00, static_cast<std::uint16_t>(addr & 0xFFFF), s.data.data() + i, n);
            i += n;
        }
    }
    record(0x01, 0, nullptr, 0);
    return out;
}

// ---- ELF -----------------------------------------------------------------------------------

Firmware parseElf(const Bytes& b) {
    if (b.size() < 52 || b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') throw FirmwareError("not an ELF file");
    if (b[4] == 2) throw FirmwareError("64-bit ELF: microcontroller firmware is 32-bit; is this a program for your computer?");
    if (b[4] != 1) throw FirmwareError("unknown ELF class");
    if (b[5] != 1) throw FirmwareError("big-endian ELF isn't supported");
    Firmware fw;
    fw.format = "elf";
    fw.elfMachineId = le16(b, 18);
    fw.elfMachine = machineName(fw.elfMachineId);
    fw.entry = le32(b, 24);
    const std::uint32_t phoff = le32(b, 28), shoff = le32(b, 32);
    const std::uint16_t phentsize = le16(b, 42), phnum = le16(b, 44), shentsize = le16(b, 46), shnum = le16(b, 48), shstrndx = le16(b, 50);
    struct Ph {
        std::uint32_t type, offset, vaddr, paddr, filesz, memsz;
    };
    std::vector<Ph> phs;
    if (phnum && phentsize >= 32) {
        for (std::uint32_t i = 0; i < phnum; ++i) {
            const std::uint64_t at = static_cast<std::uint64_t>(phoff) + static_cast<std::uint64_t>(i) * phentsize;
            if (at + 32 > b.size()) throw FirmwareError("ELF program header out of range");
            const auto a = static_cast<std::size_t>(at);
            phs.push_back({le32(b, a), le32(b, a + 4), le32(b, a + 8), le32(b, a + 12), le32(b, a + 16), le32(b, a + 20)});
        }
    }
    // sections, for inspect and as a fallback without program headers
    if (shnum && shentsize >= 40 && static_cast<std::uint64_t>(shoff) + static_cast<std::uint64_t>(shnum) * shentsize <= b.size()) {
        auto shAt = [&](std::uint32_t i) { return static_cast<std::size_t>(shoff) + static_cast<std::size_t>(i) * shentsize; };
        std::size_t strOff = 0, strSize = 0;
        if (shstrndx < shnum) {
            strOff = le32(b, shAt(shstrndx) + 16);
            strSize = le32(b, shAt(shstrndx) + 20);
            if (strOff + strSize > b.size()) strOff = strSize = 0;
        }
        for (std::uint32_t i = 1; i < shnum; ++i) {
            const std::size_t a = shAt(i);
            ElfSection s;
            const std::uint32_t nameOff = le32(b, a);
            if (nameOff < strSize) {
                const char* p = reinterpret_cast<const char*>(b.data() + strOff + nameOff);
                std::size_t n = 0;
                while (nameOff + n < strSize && p[n]) ++n;
                s.name.assign(p, n);
            }
            s.type = le32(b, a + 4);
            s.flags = le32(b, a + 8);
            s.address = le32(b, a + 12);
            s.size = le32(b, a + 20);
            s.loadAddress = s.address;
            for (const auto& ph : phs) {
                if (ph.type == 1 && s.address >= ph.vaddr && s.address < ph.vaddr + std::max(ph.memsz, 1u)) {
                    s.loadAddress = s.address - ph.vaddr + ph.paddr;
                    break;
                }
            }
            if (s.type == 0) continue;
            fw.sections.push_back(s);
        }
    }
    std::vector<Segment> segs;
    const bool avr = fw.elfMachineId == 83;
    for (const auto& ph : phs) {
        if (ph.type != 1 || ph.filesz == 0) continue;
        if (static_cast<std::uint64_t>(ph.offset) + ph.filesz > b.size()) throw FirmwareError("ELF segment runs past the end of the file");
        if (avr && ph.paddr >= 0x800000) continue;  // AVR RAM/EEPROM/fuse address spaces, not flash
        segs.push_back({ph.paddr, Bytes(b.begin() + ph.offset, b.begin() + ph.offset + ph.filesz)});
    }
    if (phs.empty()) {
        // relocatable-looking file: take allocated sections with contents
        for (std::uint32_t i = 1; i < shnum; ++i) {
            const std::size_t a = static_cast<std::size_t>(shoff) + static_cast<std::size_t>(i) * shentsize;
            const std::uint32_t type = le32(b, a + 4), flags = le32(b, a + 8), addr = le32(b, a + 12), off = le32(b, a + 16), size = le32(b, a + 20);
            if (!(flags & 2u) || type == 8 || size == 0) continue;
            if (static_cast<std::uint64_t>(off) + size > b.size()) continue;
            if (avr && addr >= 0x800000) continue;
            segs.push_back({addr, Bytes(b.begin() + off, b.begin() + off + size)});
        }
    }
    fw.segments = normalizeSegments(std::move(segs));
    if (fw.segments.empty()) throw FirmwareError("ELF file has nothing to load into flash");
    return fw;
}

// ---- Espressif image --------------------------------------------------------------------------

std::string EspImageInfo::chipName() const {
    if (!extended) return "esp8266";
    switch (chipId) {
    case 0: return "esp32";
    case 2: return "esp32-s2";
    case 5: return "esp32-c3";
    case 9: return "esp32-s3";
    case 12: return "esp32-c2";
    case 13: return "esp32-c6";
    case 16: return "esp32-h2";
    default: return "chip id " + std::to_string(chipId);
    }
}

std::string EspImageInfo::flashModeName() const {
    static const char* names[] = {"qio", "qout", "dio", "dout"};
    return flashMode < 4 ? names[flashMode] : "mode " + std::to_string(flashMode);
}

std::string EspImageInfo::flashSizeName() const {
    const int code = flashSizeFreq >> 4;
    if (!extended) {
        static const char* names[] = {"512KB", "256KB", "1MB", "2MB", "4MB", "2MB-c1", "4MB-c1", "?", "8MB", "16MB"};
        return code < 10 ? names[code] : "size code " + std::to_string(code);
    }
    static const char* names[] = {"1MB", "2MB", "4MB", "8MB", "16MB", "32MB", "64MB", "128MB"};
    return code < 8 ? names[code] : "size code " + std::to_string(code);
}

std::string EspImageInfo::flashFreqName() const {
    switch (flashSizeFreq & 0x0F) {
    case 0x0: return "40m";
    case 0x1: return "26m";
    case 0x2: return "20m";
    case 0xF: return "80m";
    default: return "freq code " + std::to_string(flashSizeFreq & 0x0F);
    }
}

std::optional<EspImageInfo> parseEspImage(const Bytes& b) {
    if (b.size() < 24 || b[0] != 0xE9) return std::nullopt;
    EspImageInfo info;
    info.segmentCount = b[1];
    info.flashMode = b[2];
    info.flashSizeFreq = b[3];
    info.entry = le32(b, 4);
    if (info.segmentCount == 0 || info.segmentCount > 16) return std::nullopt;
    // ESP8266 images put the first segment header right after the 8-byte header, and it loads
    // into the 0x3FFxxxxx/0x40xxxxxx ranges; ESP32-family images have a 16-byte extended header.
    const std::uint32_t first = le32(b, 8);
    const bool esp8266 = first >= 0x3FF00000u && first < 0x40300000u && le32(b, 12) < b.size();
    info.extended = !esp8266;
    std::size_t pos = 8;
    if (info.extended) {
        info.chipId = le16(b, 12);
        info.hashAppended = b[23] == 1;
        pos = 24;
    }
    std::uint8_t sum = 0xEF;
    for (int i = 0; i < info.segmentCount; ++i) {
        if (pos + 8 > b.size()) return std::nullopt;
        EspSegment s{le32(b, pos), le32(b, pos + 4)};
        pos += 8;
        if (s.size > b.size() - pos) return std::nullopt;
        sum = xorBytes(b.data() + pos, s.size, sum);
        pos += s.size;
        info.segments.push_back(s);
    }
    const std::size_t at = pos + (15 - (pos % 16));
    if (at < b.size()) {
        info.checksum = b[at];
        info.checksumOk = b[at] == sum;
    }
    return info;
}

bool looksLikeMergedEsp(const Bytes& b, std::uint32_t& bootOffset) {
    if (b.empty() || b[0] != 0xE9) return false;
    for (std::uint32_t off : {0x1000u, 0x0u, 0x2000u}) {
        const std::size_t pt = 0x8000u - off;
        if (b.size() > pt + 32 && b[pt] == 0xAA && b[pt + 1] == 0x50) {
            bootOffset = off;
            return true;
        }
    }
    return false;
}

// ---- loading ---------------------------------------------------------------------------------

Firmware parseFirmware(const Bytes& bytes, const std::string& nameHint, std::uint32_t binAddress) {
    if (bytes.empty()) throw FirmwareError("the file is empty");
    Firmware fw;
    if (bytes.size() >= 4 && bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F') {
        fw = parseElf(bytes);
    } else {
        std::size_t i = 0;
        while (i < bytes.size() && std::isspace(bytes[i])) ++i;
        const std::string ext = lower(std::filesystem::path(nameHint).extension().string());
        const bool textual = i < bytes.size() && bytes[i] == ':' &&
                             std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t c) { return c == ':' || std::isxdigit(c) || std::isspace(c); });
        if (textual || ext == ".hex" || ext == ".ihex" || ext == ".ihx") {
            fw = parseIntelHex(std::string(bytes.begin(), bytes.end()));
        } else {
            fw.format = "bin";
            fw.segments.push_back({binAddress, bytes});
            if (auto esp = parseEspImage(bytes)) {
                fw.format = "esp-image";
                fw.esp = esp;
                std::uint32_t off = 0;
                if (looksLikeMergedEsp(bytes, off)) {
                    fw.espMerged = true;
                    fw.espMergedBootOffset = off;
                }
            }
        }
    }
    fw.path = nameHint;
    return fw;
}

Firmware loadFirmware(const std::filesystem::path& path, std::uint32_t binAddress) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw FirmwareError("can't open " + path.string());
    Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseFirmware(bytes, path.string(), binAddress);
}

}  // namespace wetwaresuite::flash
