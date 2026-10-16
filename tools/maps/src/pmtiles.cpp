// PMTiles v3 (https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md): the
// header, varint directories with run lengths, Hilbert tile ids, gzip. Read in
// place with seeks so a 100 GB planet file opens instantly.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "wetwaresuite/maps/tiles.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "internal.hpp"

#include <nlohmann/json.hpp>

namespace wetwaresuite::maps {

namespace fs = std::filesystem;

// ---- tile ids -----------------------------------------------------------------------------

namespace {

void rotate(std::uint64_t n, std::uint64_t& x, std::uint64_t& y, std::uint64_t rx, std::uint64_t ry) {
    if (ry == 0) {
        if (rx == 1) {
            x = n - 1 - x;
            y = n - 1 - y;
        }
        std::swap(x, y);
    }
}

std::uint64_t firstIdAt(int z) { return ((std::uint64_t{1} << (2 * z)) - 1) / 3; }

}  // namespace

std::uint64_t zxyToTileId(int z, std::uint32_t x0, std::uint32_t y0) {
    if (z < 0 || z > 31) throw std::runtime_error("tile zoom must be 0..31");
    const std::uint64_t n = std::uint64_t{1} << z;
    std::uint64_t x = x0, y = y0, d = 0;
    if (x >= n || y >= n) throw std::runtime_error("tile x/y outside zoom " + std::to_string(z));
    for (std::uint64_t s = n / 2; s > 0; s /= 2) {
        const std::uint64_t rx = (x & s) > 0 ? 1 : 0, ry = (y & s) > 0 ? 1 : 0;
        d += s * s * ((3 * rx) ^ ry);
        rotate(n, x, y, rx, ry);
    }
    return firstIdAt(z) + d;
}

bool tileIdToZxy(std::uint64_t id, int& z, std::uint32_t& x, std::uint32_t& y) {
    for (z = 0; z <= 31; ++z) {
        const std::uint64_t count = std::uint64_t{1} << (2 * z);
        if (id < firstIdAt(z) + count) break;
    }
    if (z > 31) return false;
    std::uint64_t t = id - firstIdAt(z), tx = 0, ty = 0;
    const std::uint64_t n = std::uint64_t{1} << z;
    for (std::uint64_t s = 1; s < n; s *= 2) {
        const std::uint64_t rx = 1 & (t / 2), ry = 1 & (t ^ rx);
        rotate(s, tx, ty, rx, ry);
        tx += s * rx;
        ty += s * ry;
        t /= 4;
    }
    x = static_cast<std::uint32_t>(tx);
    y = static_cast<std::uint32_t>(ty);
    return true;
}

const char* toString(TileType t) {
    switch (t) {
        case TileType::Mvt: return "mvt";
        case TileType::Png: return "png";
        case TileType::Jpeg: return "jpg";
        case TileType::Webp: return "webp";
        case TileType::Avif: return "avif";
        default: return "unknown";
    }
}

const char* toString(TileCompression c) {
    switch (c) {
        case TileCompression::None: return "none";
        case TileCompression::Gzip: return "gzip";
        case TileCompression::Brotli: return "brotli";
        case TileCompression::Zstd: return "zstd";
        default: return "unknown";
    }
}

// ---- gzip -----------------------------------------------------------------------------

namespace {

std::uint32_t crc32(const std::string& s) {
    static const auto table = [] {
        std::vector<std::uint32_t> t(256);
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void putLe32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

}  // namespace

std::string gunzip(const std::string& data) {
    const auto u = [&](std::size_t i) { return static_cast<unsigned char>(data[i]); };
    if (data.size() < 18 || u(0) != 0x1f || u(1) != 0x8b) return pdf::inflate(data);  // zlib (or raw deflate)
    if (u(2) != 8) throw std::runtime_error("gzip data uses an unknown compression method");
    const unsigned flags = u(3);
    std::size_t p = 10;
    if (flags & 4) {
        if (p + 2 > data.size()) throw std::runtime_error("truncated gzip header");
        p += 2 + (u(p) | (u(p + 1) << 8));
    }
    for (unsigned bit : {8u, 16u}) {
        if (!(flags & bit)) continue;
        while (p < data.size() && data[p] != '\0') ++p;
        ++p;
    }
    if (flags & 2) p += 2;
    if (p >= data.size()) throw std::runtime_error("truncated gzip header");
    // raw deflate behind a zlib header (78 01 passes the header check; the checksum isn't verified)
    return pdf::inflate(std::string("\x78\x01", 2) + data.substr(p));
}

std::string gzipCompress(const std::string& data) {
    const std::string z = pdf::deflate(data);
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    if (z.size() >= 6) out += z.substr(2, z.size() - 6);
    putLe32(out, crc32(data));
    putLe32(out, static_cast<std::uint32_t>(data.size()));
    return out;
}

// ---- header and directories ----------------------------------------------------------------

namespace {

std::uint64_t le(const std::string& b, std::size_t at, int bytes) {
    std::uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b[at + i]);
    return v;
}

double e7(const std::string& b, std::size_t at) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(le(b, at, 4))) / 1e7; }

std::uint64_t varint(const std::string& b, std::size_t& p) {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (p >= b.size()) throw std::runtime_error("PMTiles directory is cut short");
        const unsigned char c = static_cast<unsigned char>(b[p++]);
        v |= static_cast<std::uint64_t>(c & 0x7F) << shift;
        if (!(c & 0x80)) return v;
    }
    throw std::runtime_error("PMTiles directory has a broken number");
}

void putVarint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        out += static_cast<char>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    out += static_cast<char>(v);
}

void putLe(std::string& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

std::string decompress(const std::string& data, TileCompression c, const char* what) {
    switch (c) {
        case TileCompression::None:
        case TileCompression::Unknown: return data;
        case TileCompression::Gzip: return gunzip(data);
        default:
            throw std::runtime_error(std::string("this PMTiles file uses ") + toString(c) + " compression for its " + what +
                                     ", which the maps app can't read; repack it with gzip (pmtiles convert, or the hub's map packs)");
    }
}

}  // namespace

PmtilesHeader parsePmtilesHeader(const std::string& b) {
    if (b.size() < 127 || b.compare(0, 7, "PMTiles") != 0) throw std::runtime_error("not a PMTiles file");
    if (static_cast<unsigned char>(b[7]) != 3)
        throw std::runtime_error("PMTiles version " + std::to_string(static_cast<unsigned char>(b[7])) + " isn't supported (only version 3)");
    PmtilesHeader h;
    h.rootOffset = le(b, 8, 8);
    h.rootLength = le(b, 16, 8);
    h.metadataOffset = le(b, 24, 8);
    h.metadataLength = le(b, 32, 8);
    h.leafOffset = le(b, 40, 8);
    h.leafLength = le(b, 48, 8);
    h.dataOffset = le(b, 56, 8);
    h.dataLength = le(b, 64, 8);
    h.addressedTiles = le(b, 72, 8);
    h.tileEntries = le(b, 80, 8);
    h.tileContents = le(b, 88, 8);
    h.clustered = b[96] != 0;
    h.internalCompression = static_cast<TileCompression>(static_cast<unsigned char>(b[97]));
    h.tileCompression = static_cast<TileCompression>(static_cast<unsigned char>(b[98]));
    h.tileType = static_cast<TileType>(static_cast<unsigned char>(b[99]));
    h.minZoom = static_cast<unsigned char>(b[100]);
    h.maxZoom = static_cast<unsigned char>(b[101]);
    h.minLon = e7(b, 102);
    h.minLat = e7(b, 106);
    h.maxLon = e7(b, 110);
    h.maxLat = e7(b, 114);
    h.centerZoom = static_cast<unsigned char>(b[118]);
    h.centerLon = e7(b, 119);
    h.centerLat = e7(b, 123);
    return h;
}

std::vector<PmtilesEntry> parsePmtilesDirectory(const std::string& b) {
    std::size_t p = 0;
    const std::uint64_t n = varint(b, p);
    if (n > b.size()) throw std::runtime_error("PMTiles directory is cut short");
    std::vector<PmtilesEntry> e(static_cast<std::size_t>(n));
    std::uint64_t id = 0;
    for (auto& x : e) {
        id += varint(b, p);
        x.tileId = id;
    }
    for (auto& x : e) x.runLength = static_cast<std::uint32_t>(varint(b, p));
    for (auto& x : e) x.length = static_cast<std::uint32_t>(varint(b, p));
    for (std::size_t i = 0; i < e.size(); ++i) {
        const std::uint64_t v = varint(b, p);
        e[i].offset = (v == 0 && i > 0) ? e[i - 1].offset + e[i - 1].length : v - 1;
    }
    return e;
}

std::string encodePmtilesDirectory(const std::vector<PmtilesEntry>& e) {
    std::string out;
    putVarint(out, e.size());
    std::uint64_t last = 0;
    for (const auto& x : e) {
        putVarint(out, x.tileId - last);
        last = x.tileId;
    }
    for (const auto& x : e) putVarint(out, x.runLength);
    for (const auto& x : e) putVarint(out, x.length);
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (i > 0 && e[i].offset == e[i - 1].offset + e[i - 1].length) putVarint(out, 0);
        else putVarint(out, e[i].offset + 1);
    }
    return out;
}

// ---- the reader -----------------------------------------------------------------------------

PmtilesReader::PmtilesReader(const fs::path& path) : path_(path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw std::runtime_error("no map file at " + path.string());
    header_ = parsePmtilesHeader(read(0, 127));
    root_ = directory(header_.rootOffset, header_.rootLength);
}

std::string PmtilesReader::read(std::uint64_t offset, std::uint64_t length) const {
    if (length > (std::uint64_t{1} << 31)) throw std::runtime_error("PMTiles entry is too large");
    std::ifstream in(path_, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + path_.string());
    in.seekg(static_cast<std::streamoff>(offset));
    std::string out(static_cast<std::size_t>(length), '\0');
    in.read(out.data(), static_cast<std::streamsize>(length));
    if (static_cast<std::uint64_t>(in.gcount()) != length) throw std::runtime_error(path_.string() + " is cut short (an unfinished copy?)");
    return out;
}

std::vector<PmtilesEntry> PmtilesReader::directory(std::uint64_t offset, std::uint64_t length) const {
    return parsePmtilesDirectory(decompress(read(offset, length), header_.internalCompression, "directories"));
}

std::string PmtilesReader::metadata() const {
    if (header_.metadataLength == 0) return "{}";
    return decompress(read(header_.metadataOffset, header_.metadataLength), header_.internalCompression, "metadata");
}

std::optional<std::string> PmtilesReader::tile(int z, std::uint32_t x, std::uint32_t y) const {
    if (z < header_.minZoom || z > header_.maxZoom || z > 31) return std::nullopt;
    if (x >= (std::uint64_t{1} << z) || y >= (std::uint64_t{1} << z)) return std::nullopt;
    const std::uint64_t id = zxyToTileId(z, x, y);
    std::vector<PmtilesEntry> leafCopy;
    const std::vector<PmtilesEntry>* dir = &root_;
    for (int depth = 0; depth < 4; ++depth) {
        // the last entry whose id is <= ours
        auto it = std::upper_bound(dir->begin(), dir->end(), id, [](std::uint64_t v, const PmtilesEntry& e) { return v < e.tileId; });
        if (it == dir->begin()) return std::nullopt;
        const PmtilesEntry e = *(it - 1);
        if (e.runLength > 0) {
            if (id >= e.tileId + e.runLength) return std::nullopt;
            return decompress(read(header_.dataOffset + e.offset, e.length), header_.tileCompression, "tiles");
        }
        const std::uint64_t at = header_.leafOffset + e.offset;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = leaves_.find(at);
            if (found != leaves_.end()) {
                leafCopy = found->second;
            } else {
                leafCopy = directory(at, e.length);
                if (leaves_.size() > 256) leaves_.clear();
                leaves_[at] = leafCopy;
            }
        }
        dir = &leafCopy;
    }
    return std::nullopt;
}

std::shared_ptr<const PmtilesReader> openPmtiles(const fs::path& path) {
    static std::mutex m;
    struct Cached {
        std::shared_ptr<const PmtilesReader> reader;
        fs::file_time_type time;
        std::uintmax_t size = 0;
    };
    static std::map<std::string, Cached> cache;
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    const auto size = fs::file_size(path, ec);
    std::lock_guard<std::mutex> lock(m);
    const std::string key = path.string();
    auto it = cache.find(key);
    if (it != cache.end() && it->second.time == time && it->second.size == size) return it->second.reader;
    auto reader = std::make_shared<const PmtilesReader>(path);
    if (cache.size() > 16) cache.clear();
    cache[key] = {reader, time, size};
    return reader;
}

// ---- metadata ---------------------------------------------------------------------------

namespace {

// "<a href=...>Protomaps</a> &copy; OpenStreetMap" -> "Protomaps (c) OpenStreetMap"
std::string plainText(const std::string& html) {
    std::string out;
    bool tag = false;
    for (char c : html) {
        if (c == '<') tag = true;
        else if (c == '>') tag = false;
        else if (!tag) out += c;
    }
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"&copy;", "\xC2\xA9"}, {"&amp;", "&"}, {"&nbsp;", " "}, {"&lt;", "<"}, {"&gt;", ">"}}) {
        for (auto at = out.find(from); at != std::string::npos; at = out.find(from, at + to.size())) out.replace(at, from.size(), to);
    }
    return detail::trim(out);
}

}  // namespace

TilesInfo describePmtiles(const PmtilesReader& r) {
    TilesInfo info;
    info.header = r.header();
    try {
        const auto j = nlohmann::json::parse(r.metadata());
        if (j.is_object()) {
            if (j.contains("name") && j["name"].is_string()) info.name = j["name"].get<std::string>();
            if (j.contains("attribution") && j["attribution"].is_string()) info.attribution = plainText(j["attribution"].get<std::string>());
            if (j.contains("description") && j["description"].is_string()) info.description = j["description"].get<std::string>();
            if (j.contains("vector_layers") && j["vector_layers"].is_array()) {
                for (const auto& l : j["vector_layers"]) {
                    if (l.is_object() && l.contains("id") && l["id"].is_string()) info.layers.push_back(l["id"].get<std::string>());
                }
            }
        }
    } catch (const std::exception&) {
        // unreadable metadata: the header still says enough
    }
    if (info.layers.empty() && info.header.tileType == TileType::Mvt) {
        // no metadata layer list: look inside the tile at the centre of the lowest zoom
        const int z = info.header.minZoom;
        double tx = 0, ty = 0;
        lonLatToTile(info.header.centerLon, info.header.centerLat, z, tx, ty);
        try {
            if (auto t = r.tile(z, static_cast<std::uint32_t>(tx), static_cast<std::uint32_t>(ty))) {
                for (const auto& l : decodeMvt(*t)) info.layers.push_back(l.name);
            }
        } catch (const std::exception&) {
        }
    }
    return info;
}

// ---- the writer -----------------------------------------------------------------------------

std::string writePmtiles(std::vector<PmtilesTile> tiles, TileType type, const std::string& metadataJson, bool gzip, std::size_t leafSize) {
    struct T {
        std::uint64_t id;
        const std::string* bytes;
    };
    std::vector<T> order;
    PmtilesHeader h;
    h.minZoom = 99;
    h.maxZoom = 0;
    BBox ll;
    for (const auto& t : tiles) {
        order.push_back({zxyToTileId(t.z, t.x, t.y), &t.bytes});
        h.minZoom = std::min(h.minZoom, t.z);
        h.maxZoom = std::max(h.maxZoom, t.z);
        double lon0 = 0, lat0 = 0, lon1 = 0, lat1 = 0;
        tileToLonLat(t.x, t.y, t.z, lon0, lat1);
        tileToLonLat(t.x + 1.0, t.y + 1.0, t.z, lon1, lat0);
        ll.add(lon0, lat0);
        ll.add(lon1, lat1);
    }
    if (tiles.empty()) h.minZoom = 0;
    std::sort(order.begin(), order.end(), [](const T& a, const T& b) { return a.id < b.id; });
    std::string data;
    std::vector<PmtilesEntry> entries;
    std::map<std::string, std::uint64_t> seen;  // identical tiles (open ocean) are stored once
    for (const auto& t : order) {
        const std::string body = gzip ? gzipCompress(*t.bytes) : *t.bytes;
        auto known = seen.find(body);
        std::uint64_t off = 0;
        if (known != seen.end()) {
            off = known->second;
        } else {
            off = data.size();
            seen[body] = off;
            data += body;
        }
        if (!entries.empty() && entries.back().offset == off && entries.back().tileId + entries.back().runLength == t.id) {
            ++entries.back().runLength;
        } else {
            entries.push_back({t.id, off, static_cast<std::uint32_t>(body.size()), 1});
        }
    }
    const auto z = [&](const std::string& s) { return gzip ? gzipCompress(s) : s; };
    std::string root = z(encodePmtilesDirectory(entries)), leaves;
    if (leafSize > 0 || root.size() > 16000) {
        const std::size_t per = leafSize > 0 ? leafSize : 4096;
        std::vector<PmtilesEntry> rootEntries;
        for (std::size_t i = 0; i < entries.size(); i += per) {
            const std::vector<PmtilesEntry> chunk(entries.begin() + static_cast<std::ptrdiff_t>(i),
                                                  entries.begin() + static_cast<std::ptrdiff_t>(std::min(entries.size(), i + per)));
            const std::string leaf = z(encodePmtilesDirectory(chunk));
            rootEntries.push_back({chunk.front().tileId, leaves.size(), static_cast<std::uint32_t>(leaf.size()), 0});
            leaves += leaf;
        }
        root = z(encodePmtilesDirectory(rootEntries));
    }
    const std::string meta = z(metadataJson.empty() ? "{}" : metadataJson);
    h.rootOffset = 127;
    h.rootLength = root.size();
    h.metadataOffset = h.rootOffset + h.rootLength;
    h.metadataLength = meta.size();
    h.leafOffset = h.metadataOffset + h.metadataLength;
    h.leafLength = leaves.size();
    h.dataOffset = h.leafOffset + h.leafLength;
    h.dataLength = data.size();
    std::string out = "PMTiles";
    out += '\x03';
    for (std::uint64_t v : {h.rootOffset, h.rootLength, h.metadataOffset, h.metadataLength, h.leafOffset, h.leafLength, h.dataOffset, h.dataLength,
                            static_cast<std::uint64_t>(order.size()), static_cast<std::uint64_t>(entries.size()), static_cast<std::uint64_t>(seen.size())})
        putLe(out, v, 8);
    out += '\x01';  // clustered
    const char comp = gzip ? '\x02' : '\x01';
    out += comp;
    out += comp;
    out += static_cast<char>(type);
    out += static_cast<char>(h.minZoom);
    out += static_cast<char>(h.maxZoom);
    const auto putE7 = [&](double v) { putLe(out, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(v * 1e7))), 4); };
    if (!ll.valid()) ll = {-180, -85, 180, 85};
    putE7(ll.minX);
    putE7(ll.minY);
    putE7(ll.maxX);
    putE7(ll.maxY);
    out += static_cast<char>(h.minZoom);
    putE7((ll.minX + ll.maxX) / 2);
    putE7((ll.minY + ll.maxY) / 2);
    return out + root + meta + leaves + data;
}

// ---- installed map packs ------------------------------------------------------------------

namespace {

std::string env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t len = 0;
    std::string out;
    if (_dupenv_s(&value, &len, name) == 0 && value) out = value;
    std::free(value);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

}  // namespace

fs::path packsDir() {
    fs::path root;
    const std::string home = env("WETWARESUITE_HOME");
    if (!home.empty()) {
        root = home;
    } else {
#ifdef _WIN32
        const std::string appdata = env("APPDATA");
        root = appdata.empty() ? fs::path(env("USERPROFILE")) / "AppData" / "Roaming" / "wetwaresuite" : fs::path(appdata) / "wetwaresuite";
#else
        const fs::path user = env("HOME").empty() ? fs::path(".") : fs::path(env("HOME"));
#ifdef __APPLE__
        root = user / "Library" / "Application Support" / "wetwaresuite";
#else
        const std::string xdg = env("XDG_DATA_HOME");
        root = xdg.empty() ? user / ".local" / "share" / "wetwaresuite" : fs::path(xdg) / "wetwaresuite";
#endif
#endif
    }
    return root / "packs";
}

std::vector<fs::path> findMapPacks() {
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path dir = packsDir();
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& pack : fs::directory_iterator(dir, ec)) {
        if (!pack.is_directory(ec)) continue;
        for (const auto& f : fs::directory_iterator(pack.path(), ec)) {
            const std::string ext = detail::lower(f.path().extension().string());
            if (ext == ".pmtiles" && f.is_regular_file(ec)) out.push_back(f.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<fs::path> bestMapPack() {
    std::optional<fs::path> best;
    int bestZoom = -1;
    for (const auto& p : findMapPacks()) {
        try {
            const auto r = openPmtiles(p);
            if (r->header().maxZoom > bestZoom) {
                bestZoom = r->header().maxZoom;
                best = p;
            }
        } catch (const std::exception&) {
            // a half-copied pack: skip it
        }
    }
    return best;
}

}  // namespace wetwaresuite::maps
