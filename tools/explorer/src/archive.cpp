// zip, tar, tar.gz and gz: list, create and extract, with deflate from the pdf
// library. Archives are read into memory (fine for everyday sizes; ZIP64 for
// archives over 4 GB is not supported).
#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "wetwaresuite/explorer/explorer.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::explorer {

namespace {

std::uint32_t get16(const std::string& s, std::size_t at) {
    if (at + 2 > s.size()) throw std::runtime_error("archive is cut short");
    return static_cast<unsigned char>(s[at]) | (static_cast<unsigned char>(s[at + 1]) << 8);
}
std::uint32_t get32(const std::string& s, std::size_t at) {
    if (at + 4 > s.size()) throw std::runtime_error("archive is cut short");
    return get16(s, at) | (get16(s, at + 2) << 16);
}
void put16(std::string& s, std::uint32_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
}
void put32(std::string& s, std::uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

std::string lowerName(const fs::path& p) { return sysutil::lowerAscii(sysutil::nameOf(p)); }

bool endsWith(const std::string& s, const std::string& end) { return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0; }

// ---- DOS times (zip) ----
void dosTime(std::int64_t t, std::uint32_t& time, std::uint32_t& date) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    if (tm.tm_year < 80) tm = std::tm{0, 0, 0, 1, 0, 80, 0, 0, 0};
    time = static_cast<std::uint32_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    date = static_cast<std::uint32_t>(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}
std::int64_t fromDos(std::uint32_t time, std::uint32_t date) {
    std::tm tm{};
    tm.tm_year = static_cast<int>((date >> 9) + 80);
    tm.tm_mon = static_cast<int>(((date >> 5) & 15) - 1);
    tm.tm_mday = static_cast<int>(date & 31);
    tm.tm_hour = static_cast<int>(time >> 11);
    tm.tm_min = static_cast<int>((time >> 5) & 63);
    tm.tm_sec = static_cast<int>((time & 31) * 2);
    tm.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&tm));
}

// what goes into an archive: each input under its own name, folders with everything inside
struct Source {
    fs::path file;
    std::string name;
    bool dir = false;
};

std::vector<Source> gather(const std::vector<fs::path>& inputs) {
    std::vector<Source> out;
    for (const auto& in : inputs) {
        std::error_code ec;
        fs::path abs = fs::absolute(in, ec).lexically_normal();
        if (sysutil::nameOf(abs).empty()) abs = abs.parent_path();
        const fs::path base = abs.parent_path();
        if (fs::is_directory(abs, ec)) {
            out.push_back({abs, sysutil::fromPath(abs.lexically_relative(base)) + "/", true});
            std::vector<fs::path> all;
            for (fs::recursive_directory_iterator it(abs, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                if (ec) break;
                all.push_back(it->path());
            }
            std::sort(all.begin(), all.end());
            for (const auto& p : all) {
                const bool d = fs::is_directory(p, ec);
                if (!d && !fs::is_regular_file(p, ec)) continue;  // sockets, broken links
                out.push_back({p, sysutil::fromPath(p.lexically_relative(base)) + (d ? "/" : ""), d});
            }
        } else if (fs::is_regular_file(abs, ec)) {
            out.push_back({abs, sysutil::nameOf(abs), false});
        } else {
            throw std::runtime_error("not found: " + sysutil::fromPath(in));
        }
    }
    return out;
}

std::uint32_t modeOf(const fs::path& p) {
    std::error_code ec;
    auto perms = fs::status(p, ec).permissions();
    std::uint32_t m = static_cast<std::uint32_t>(perms) & 0777;
    if (ec || m == 0) m = fs::is_directory(p, ec) ? 0755 : 0644;
    return m;
}

// ---- zip ----
std::string writeZipFile(const std::vector<Source>& items, int level, std::size_t& files) {
    std::string out, central;
    std::uint32_t count = 0;
    for (const auto& it : items) {
        const std::string data = it.dir ? std::string() : sysutil::readFile(it.file);
        if (data.size() > 0xFFFFFFF0u || out.size() > 0xFFFFFFF0u) throw std::runtime_error("archives over 4 GB need ZIP64, which isn't supported; use tar.gz");
        std::string packed;
        std::uint32_t method = 0;
        if (!it.dir && level > 0 && data.size() > 32) {
            packed = pdf::deflateRaw(data, level);
            if (packed.size() < data.size()) method = 8;
            else packed.clear();
        }
        const std::string& body = method ? packed : data;
        const std::uint32_t crc = crc32(data.data(), data.size());
        std::uint32_t t = 0, d = 0;
        dosTime(modifiedTime(it.file), t, d);
        const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
        auto header = [&](std::string& s, bool centralDir) {
            put32(s, centralDir ? 0x02014b50 : 0x04034b50);
            if (centralDir) put16(s, (3 << 8) | 20);  // made on Unix-style permissions, version 2.0
            put16(s, 20);
            put16(s, 0x0800);  // names are UTF-8
            put16(s, method);
            put16(s, t);
            put16(s, d);
            put32(s, crc);
            put32(s, static_cast<std::uint32_t>(body.size()));
            put32(s, static_cast<std::uint32_t>(data.size()));
            put16(s, static_cast<std::uint32_t>(it.name.size()));
            put16(s, 0);
            if (centralDir) {
                put16(s, 0);
                put16(s, 0);
                put16(s, 0);
                put32(s, (((it.dir ? 0040000u : 0100000u) | modeOf(it.file)) << 16) | (it.dir ? 0x10u : 0u));
                put32(s, offset);
            }
            s += it.name;
        };
        header(out, false);
        out += body;
        header(central, true);
        ++count;
        if (!it.dir) ++files;
    }
    if (count > 0xFFFF) throw std::runtime_error("more than 65535 entries needs ZIP64, which isn't supported; use tar.gz");
    const std::uint32_t at = static_cast<std::uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, count);
    put16(out, count);
    put32(out, static_cast<std::uint32_t>(central.size()));
    put32(out, at);
    put16(out, 0);
    return out;
}

struct ZipRecord {
    ArchiveItem item;
    std::uint32_t method = 0, crc = 0, local = 0, attrs = 0, madeBy = 0;
};

std::vector<ZipRecord> readZipDirectory(const std::string& z) {
    if (z.size() < 22) throw std::runtime_error("not a zip file");
    std::size_t eocd = std::string::npos;
    const std::size_t start = z.size() > 22 + 65535 ? z.size() - 22 - 65535 : 0;
    for (std::size_t i = z.size() - 22 + 1; i-- > start;) {
        if (get32(z, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) throw std::runtime_error("not a zip file (no directory at the end)");
    const std::uint32_t count = get16(z, eocd + 10);
    std::size_t at = get32(z, eocd + 16);
    if (count == 0xFFFF || at == 0xFFFFFFFFu) throw std::runtime_error("this is a ZIP64 archive, which isn't supported");
    std::vector<ZipRecord> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (get32(z, at) != 0x02014b50) throw std::runtime_error("corrupt zip directory");
        ZipRecord r;
        r.madeBy = get16(z, at + 4);
        r.method = get16(z, at + 10);
        r.item.modified = fromDos(get16(z, at + 12), get16(z, at + 14));
        r.crc = get32(z, at + 16);
        r.item.packed = get32(z, at + 20);
        r.item.size = get32(z, at + 24);
        const std::uint32_t nameLen = get16(z, at + 28), extraLen = get16(z, at + 30), commentLen = get16(z, at + 32);
        r.attrs = get32(z, at + 38);
        r.local = get32(z, at + 42);
        if (at + 46 + nameLen > z.size()) throw std::runtime_error("corrupt zip directory");
        r.item.name = z.substr(at + 46, nameLen);
        std::replace(r.item.name.begin(), r.item.name.end(), '\\', '/');
        r.item.dir = !r.item.name.empty() && r.item.name.back() == '/';
        at += 46 + nameLen + extraLen + commentLen;
        out.push_back(std::move(r));
    }
    return out;
}

std::string zipData(const std::string& z, const ZipRecord& r) {
    if (get32(z, r.local) != 0x04034b50) throw std::runtime_error("corrupt zip entry " + r.item.name);
    if (get16(z, r.local + 6) & 1) throw std::runtime_error(r.item.name + " is encrypted (password-protected zips aren't supported)");
    const std::size_t dataAt = r.local + 30 + get16(z, r.local + 26) + get16(z, r.local + 28);
    if (dataAt + r.item.packed > z.size()) throw std::runtime_error("truncated zip entry " + r.item.name);
    std::string body = z.substr(dataAt, static_cast<std::size_t>(r.item.packed));
    std::string data;
    if (r.method == 0) data = std::move(body);
    else if (r.method == 8) data = pdf::inflateRaw(body);
    else throw std::runtime_error(r.item.name + " uses a compression method (" + std::to_string(r.method) + ") that isn't supported; only stored and deflate");
    if (data.size() != r.item.size || crc32(data.data(), data.size()) != r.crc) throw std::runtime_error(r.item.name + " is damaged (checksum mismatch)");
    return data;
}

// ---- tar ----
void octal(char* field, std::size_t width, std::uint64_t v) {
    std::string s(width - 1, '0');
    for (std::size_t i = width - 1; i-- > 0 && v;) {
        s[i] = static_cast<char>('0' + (v & 7));
        v >>= 3;
    }
    std::memcpy(field, s.data(), width - 1);
    field[width - 1] = '\0';
}

std::uint64_t fromOctal(const char* f, std::size_t width) {
    if (static_cast<unsigned char>(f[0]) & 0x80) {  // GNU base-256 for big sizes
        std::uint64_t v = 0;
        for (std::size_t i = 1; i < width; ++i) v = (v << 8) | static_cast<unsigned char>(f[i]);
        return v;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width && f[i]; ++i) {
        if (f[i] >= '0' && f[i] <= '7') v = v * 8 + static_cast<std::uint64_t>(f[i] - '0');
    }
    return v;
}

std::string tarHeader(const std::string& name, std::uint64_t size, std::int64_t mtime, std::uint32_t mode, char type) {
    char h[512] = {};
    std::memcpy(h, name.data(), std::min<std::size_t>(name.size(), 100));
    octal(h + 100, 8, mode);
    octal(h + 108, 8, 0);
    octal(h + 116, 8, 0);
    if (size > 077777777777ull) {  // base-256
        h[124] = static_cast<char>(0x80);
        for (int i = 11; i > 0; --i, size >>= 8) h[124 + i] = static_cast<char>(size & 0xFF);
    } else {
        octal(h + 124, 12, size);
    }
    octal(h + 136, 12, static_cast<std::uint64_t>(std::max<std::int64_t>(0, mtime)));
    h[156] = type;
    std::memcpy(h + 257, "ustar", 6);
    std::memcpy(h + 263, "00", 2);
    std::memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    octal(h + 148, 7, sum);
    h[155] = ' ';
    return std::string(h, 512);
}

void tarPad(std::string& out) { out.append((512 - out.size() % 512) % 512, '\0'); }

std::string writeTar(const std::vector<Source>& items, std::size_t& files) {
    std::string out;
    for (const auto& it : items) {
        if (it.name.size() > 100) {  // GNU long name
            out += tarHeader("././@LongLink", it.name.size() + 1, 0, 0644, 'L');
            out += it.name;
            out += '\0';
            tarPad(out);
        }
        std::string data = it.dir ? std::string() : sysutil::readFile(it.file);
        out += tarHeader(it.name, data.size(), modifiedTime(it.file), modeOf(it.file), it.dir ? '5' : '0');
        out += data;
        tarPad(out);
        if (!it.dir) ++files;
    }
    out.append(1024, '\0');
    return out;
}

struct TarRecord {
    ArchiveItem item;
    std::size_t at = 0;  // where its data starts
    std::uint32_t mode = 0;
};

std::vector<TarRecord> readTar(const std::string& t) {
    std::vector<TarRecord> out;
    std::string longName;
    std::size_t at = 0;
    while (at + 512 <= t.size()) {
        const char* h = t.data() + at;
        if (std::all_of(h, h + 512, [](char c) { return c == '\0'; })) break;
        unsigned sum = 0;
        for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(h[i]);
        if (sum != fromOctal(h + 148, 8)) throw std::runtime_error("not a tar archive (bad header checksum)");
        const std::uint64_t size = fromOctal(h + 124, 12);
        const char type = h[156];
        std::string name(h, strnlen(h, 100));
        if (std::memcmp(h + 257, "ustar", 5) == 0 && h[345]) name = std::string(h + 345, strnlen(h + 345, 155)) + "/" + name;
        const std::size_t data = at + 512;
        if (data + size > t.size()) throw std::runtime_error("tar archive is cut short");
        at = data + static_cast<std::size_t>((size + 511) / 512 * 512);
        if (type == 'L') {
            longName = std::string(t.data() + data, strnlen(t.data() + data, static_cast<std::size_t>(size)));
            continue;
        }
        if (type == 'x') {  // pax: path=
            std::string pax = t.substr(data, static_cast<std::size_t>(size));
            std::size_t p = pax.find(" path=");
            if (p != std::string::npos) longName = pax.substr(p + 6, pax.find('\n', p) - p - 6);
            continue;
        }
        if (type == 'g') continue;
        if (!longName.empty()) {
            name = longName;
            longName.clear();
        }
        if (type != '0' && type != '\0' && type != '5' && type != '7') continue;  // links and devices are skipped
        TarRecord r;
        r.item.name = name;
        r.item.dir = type == '5' || (!name.empty() && name.back() == '/');
        if (r.item.dir && name.back() != '/') r.item.name += '/';
        r.item.size = r.item.packed = r.item.dir ? 0 : size;
        r.item.modified = static_cast<std::int64_t>(fromOctal(h + 136, 12));
        r.mode = static_cast<std::uint32_t>(fromOctal(h + 100, 8));
        r.at = data;
        out.push_back(std::move(r));
    }
    return out;
}

// ---- gzip ----
std::string gzip(const std::string& data, int level, const std::string& name) {
    std::string out = "\x1f\x8b\x08";
    out += static_cast<char>(name.empty() ? 0 : 8);
    put32(out, static_cast<std::uint32_t>(std::time(nullptr)));
    out += '\0';
    out += '\xff';
    if (!name.empty()) out += name + '\0';
    out += level > 0 ? pdf::deflateRaw(data, level) : pdf::deflateRaw(data, 1);
    put32(out, crc32(data.data(), data.size()));
    put32(out, static_cast<std::uint32_t>(data.size()));
    return out;
}

std::string gunzip(const std::string& z, std::string* name = nullptr) {
    if (z.size() < 18 || static_cast<unsigned char>(z[0]) != 0x1f || static_cast<unsigned char>(z[1]) != 0x8b) throw std::runtime_error("not a gzip file");
    const unsigned flags = static_cast<unsigned char>(z[3]);
    std::size_t at = 10;
    if (flags & 4) at += 2 + get16(z, at);
    if (flags & 8) {
        std::size_t end = z.find('\0', at);
        if (name) *name = z.substr(at, end - at);
        at = end + 1;
    }
    if (flags & 16) at = z.find('\0', at) + 1;
    if (flags & 2) at += 2;
    if (at >= z.size()) throw std::runtime_error("gzip file is cut short");
    std::string data = pdf::inflateRaw(z.substr(at));
    if (crc32(data.data(), data.size()) != get32(z, z.size() - 8)) throw std::runtime_error("gzip data is damaged (checksum mismatch)");
    return data;
}

// a name from an archive, made safe to put under `dest`
fs::path safeTarget(const fs::path& dest, const std::string& name) {
    std::string n = name;
    std::replace(n.begin(), n.end(), '\\', '/');
    if (n.empty() || n[0] == '/' || (n.size() > 1 && n[1] == ':')) throw std::runtime_error("refusing an absolute path in the archive: " + name);
    fs::path rel = sysutil::toPath(n).lexically_normal();
    for (const auto& part : rel) {
        if (part == "..") throw std::runtime_error("refusing a path that leaves the folder: " + name);
    }
    return dest / rel;
}

bool wanted(const std::vector<std::string>& only, const std::string& name) {
    if (only.empty()) return true;
    for (const auto& o : only) {
        std::string base = o;
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (name == o || name == base || name == base + "/" || name.rfind(base + "/", 0) == 0) return true;
    }
    return false;
}

void writeOut(const fs::path& target, const std::string& data, bool overwrite, std::vector<fs::path>& written, std::uint32_t mode) {
    std::error_code ec;
    if (fs::exists(target, ec) && !overwrite) throw std::runtime_error(sysutil::fromPath(target) + " already exists (overwrite=true replaces it)");
    fs::create_directories(target.parent_path(), ec);
    sysutil::writeFile(target, data);
#ifndef _WIN32
    if (mode & 0111) fs::permissions(target, static_cast<fs::perms>(mode & 0777), ec);
#else
    (void)mode;
#endif
    written.push_back(target);
}

void makeDir(const fs::path& target, std::vector<fs::path>& written) {
    std::error_code ec;
    if (fs::is_directory(target, ec)) return;
    fs::create_directories(target, ec);
    if (ec) throw std::runtime_error("can't make folder " + sysutil::fromPath(target));
    written.push_back(target);
}

}  // namespace

std::string archiveKind(const fs::path& p) {
    std::string head;
    try {
        std::ifstream in(p, std::ios::binary);
        head.resize(512);
        in.read(head.data(), 512);
        head.resize(static_cast<std::size_t>(in.gcount()));
    } catch (const std::exception&) {
        return {};
    }
    const std::string name = lowerName(p);
    if (head.size() >= 4 && head.compare(0, 2, "PK") == 0 && (head[2] == 3 || head[2] == 5)) return "zip";
    if (head.size() >= 2 && static_cast<unsigned char>(head[0]) == 0x1f && static_cast<unsigned char>(head[1]) == 0x8b)
        return endsWith(name, ".tar.gz") || endsWith(name, ".tgz") ? "tar.gz" : "gz";
    if (head.size() >= 262 && head.compare(257, 5, "ustar") == 0) return "tar";
    if (endsWith(name, ".tar") && head.size() == 512) return "tar";
    return {};
}

std::vector<ArchiveItem> archiveList(const fs::path& archive) {
    const std::string kind = archiveKind(archive);
    if (kind.empty()) throw std::runtime_error(sysutil::fromPath(archive) + " is not a zip, tar, tar.gz or gz archive");
    const std::string bytes = sysutil::readFile(archive);
    std::vector<ArchiveItem> out;
    if (kind == "zip") {
        for (auto& r : readZipDirectory(bytes)) out.push_back(r.item);
    } else if (kind == "gz") {
        std::string name;
        std::string data = gunzip(bytes, &name);
        if (name.empty()) {
            name = sysutil::nameOf(archive);
            if (endsWith(sysutil::lowerAscii(name), ".gz")) name.resize(name.size() - 3);
        }
        out.push_back({name, data.size(), bytes.size(), false, modifiedTime(archive)});
    } else {
        const std::string tar = kind == "tar" ? bytes : gunzip(bytes);
        for (auto& r : readTar(tar)) out.push_back(r.item);
        if (kind == "tar.gz") {
            for (auto& i : out) i.packed = 0;  // one stream: no per-file size
        }
    }
    return out;
}

std::size_t archiveCreate(const fs::path& output, const std::vector<fs::path>& inputs, const std::string& kind, int level) {
    if (inputs.empty()) throw std::runtime_error("nothing to pack");
    auto items = gather(inputs);
    level = std::clamp(level, 0, 9);
    std::size_t files = 0;
    std::string bytes;
    if (kind == "zip") bytes = writeZipFile(items, level, files);
    else if (kind == "tar") bytes = writeTar(items, files);
    else if (kind == "tar.gz") bytes = gzip(writeTar(items, files), level, {});
    else throw std::runtime_error("unknown archive format '" + kind + "' (zip, tar or tar.gz)");
    std::error_code ec;
    if (output.has_parent_path()) fs::create_directories(output.parent_path(), ec);
    sysutil::writeFile(output, bytes);
    return files;
}

std::vector<fs::path> archiveExtract(const fs::path& archive, const fs::path& dest, const std::vector<std::string>& only, bool overwrite) {
    const std::string kind = archiveKind(archive);
    if (kind.empty()) throw std::runtime_error(sysutil::fromPath(archive) + " is not a zip, tar, tar.gz or gz archive");
    const std::string bytes = sysutil::readFile(archive);
    std::vector<fs::path> written;
    std::error_code ec;
    if (!fs::is_directory(dest, ec)) makeDir(dest, written);
    if (kind == "zip") {
        for (const auto& r : readZipDirectory(bytes)) {
            if (!wanted(only, r.item.name)) continue;
            fs::path target = safeTarget(dest, r.item.name);
            if (r.item.dir) makeDir(target, written);
            else writeOut(target, zipData(bytes, r), overwrite, written, (r.madeBy >> 8) == 3 ? (r.attrs >> 16) : 0);
        }
    } else if (kind == "gz") {
        auto items = archiveList(archive);
        if (!wanted(only, items[0].name)) return written;
        writeOut(safeTarget(dest, sysutil::nameOf(sysutil::toPath(items[0].name))), gunzip(bytes), overwrite, written, 0);
    } else {
        const std::string tar = kind == "tar" ? bytes : gunzip(bytes);
        for (const auto& r : readTar(tar)) {
            if (!wanted(only, r.item.name)) continue;
            fs::path target = safeTarget(dest, r.item.name);
            if (r.item.dir) makeDir(target, written);
            else writeOut(target, tar.substr(r.at, static_cast<std::size_t>(r.item.size)), overwrite, written, r.mode);
        }
    }
    return written;
}

}  // namespace wetwaresuite::explorer
