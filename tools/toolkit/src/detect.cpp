#include "achclwss/toolkit/detect.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace achclwss::toolkit {

namespace {

constexpr std::size_t kHeadSize = 4096;

// Extension table: the fallback when content alone doesn't say, and the way
// container formats (ZIP, OLE2, TIFF) are told apart.
const std::map<std::string, FileType>& byExtension() {
    static const std::map<std::string, FileType> table = {
        // images
        {"png", {"PNG image", "image", "photo", "image/png"}},
        {"jpg", {"JPEG image", "image", "photo", "image/jpeg"}},
        {"jpeg", {"JPEG image", "image", "photo", "image/jpeg"}},
        {"gif", {"GIF image", "image", "photo", "image/gif"}},
        {"webp", {"WebP image", "image", "photo", "image/webp"}},
        {"avif", {"AVIF image", "image", "photo", "image/avif"}},
        {"heic", {"HEIC image", "image", "photo", "image/heic"}},
        {"tif", {"TIFF image", "image", "photo", "image/tiff"}},
        {"tiff", {"TIFF image", "image", "photo", "image/tiff"}},
        {"bmp", {"BMP image", "image", "photo", "image/bmp"}},
        {"tga", {"TGA image", "texture", "asset forge", "image/x-tga"}},
        {"exr", {"OpenEXR image", "image", "photo", "image/x-exr"}},
        {"hdr", {"Radiance HDR image", "image", "photo", "image/vnd.radiance"}},
        {"psd", {"Photoshop document", "image", "photo", "image/vnd.adobe.photoshop"}},
        {"kra", {"Krita document", "image", "paint", "application/x-krita"}},
        {"ora", {"OpenRaster image", "image", "paint", "image/openraster"}},
        {"xcf", {"GIMP image", "image", "photo", "image/x-xcf"}},
        // camera raw
        {"cr2", {"Canon RAW", "raw", "photo", "image/x-canon-cr2"}},
        {"cr3", {"Canon RAW", "raw", "photo", "image/x-canon-cr3"}},
        {"nef", {"Nikon RAW", "raw", "photo", "image/x-nikon-nef"}},
        {"arw", {"Sony RAW", "raw", "photo", "image/x-sony-arw"}},
        {"dng", {"Digital Negative RAW", "raw", "photo", "image/x-adobe-dng"}},
        {"raf", {"Fujifilm RAW", "raw", "photo", "image/x-fuji-raf"}},
        {"orf", {"Olympus RAW", "raw", "photo", "image/x-olympus-orf"}},
        {"rw2", {"Panasonic RAW", "raw", "photo", "image/x-panasonic-rw2"}},
        // vector
        {"svg", {"SVG drawing", "vector", "vector", "image/svg+xml"}},
        {"eps", {"EPS drawing", "vector", "vector", "application/postscript"}},
        {"ai", {"Illustrator drawing", "vector", "vector", "application/illustrator"}},
        {"emf", {"EMF drawing", "vector", "vector", "image/emf"}},
        {"wmf", {"WMF drawing", "vector", "vector", "image/wmf"}},
        // cad
        {"dxf", {"DXF drawing", "cad", "cad", "image/vnd.dxf"}},
        {"dwg", {"DWG drawing", "cad", "cad", "image/vnd.dwg"}},
        {"step", {"STEP model", "cad", "cad", "model/step"}},
        {"stp", {"STEP model", "cad", "cad", "model/step"}},
        {"iges", {"IGES model", "cad", "cad", "model/iges"}},
        {"igs", {"IGES model", "cad", "cad", "model/iges"}},
        {"brep", {"OpenCASCADE BREP", "cad", "cad", "model/x-brep"}},
        {"stl", {"STL mesh", "cad", "cad", "model/stl"}},
        {"3mf", {"3MF print model", "cad", "cad", "model/3mf"}},
        // 3d / game
        {"obj", {"Wavefront OBJ model", "3d", "asset forge", "model/obj"}},
        {"gltf", {"glTF model", "3d", "asset forge", "model/gltf+json"}},
        {"glb", {"glTF binary model", "3d", "asset forge", "model/gltf-binary"}},
        {"fbx", {"FBX model", "3d", "asset forge", "application/vnd.autodesk.fbx"}},
        {"dae", {"COLLADA model", "3d", "asset forge", "model/vnd.collada+xml"}},
        {"ply", {"PLY model", "3d", "asset forge", "model/x-ply"}},
        {"usd", {"USD scene", "3d", "asset forge", "model/vnd.usd"}},
        {"usdz", {"USDZ scene", "3d", "asset forge", "model/vnd.usdz+zip"}},
        {"blend", {"Blender file", "3d", "asset forge", "application/x-blender"}},
        {"dds", {"DDS texture", "texture", "asset forge", "image/vnd.ms-dds"}},
        {"ktx2", {"KTX2 texture", "texture", "asset forge", "image/ktx2"}},
        {"basis", {"Basis Universal texture", "texture", "asset forge", "image/x-basis"}},
        // audio
        {"wav", {"WAV audio", "audio", "audio", "audio/wav"}},
        {"mp3", {"MP3 audio", "audio", "audio", "audio/mpeg"}},
        {"flac", {"FLAC audio", "audio", "audio", "audio/flac"}},
        {"ogg", {"Ogg audio", "audio", "audio", "audio/ogg"}},
        {"opus", {"Opus audio", "audio", "audio", "audio/opus"}},
        {"aac", {"AAC audio", "audio", "audio", "audio/aac"}},
        {"m4a", {"M4A audio", "audio", "audio", "audio/mp4"}},
        {"aiff", {"AIFF audio", "audio", "audio", "audio/aiff"}},
        {"aif", {"AIFF audio", "audio", "audio", "audio/aiff"}},
        {"mid", {"MIDI sequence", "audio", "audio", "audio/midi"}},
        {"midi", {"MIDI sequence", "audio", "audio", "audio/midi"}},
        // video
        {"mp4", {"MP4 video", "video", "cut", "video/mp4"}},
        {"mkv", {"Matroska video", "video", "cut", "video/x-matroska"}},
        {"mov", {"QuickTime video", "video", "cut", "video/quicktime"}},
        {"webm", {"WebM video", "video", "cut", "video/webm"}},
        {"avi", {"AVI video", "video", "cut", "video/x-msvideo"}},
        // documents
        {"pdf", {"PDF document", "document", "office", "application/pdf"}},
        {"docx", {"Word document", "document", "office",
                  "application/vnd.openxmlformats-officedocument.wordprocessingml.document"}},
        {"doc", {"Word 97-2003 document", "document", "office", "application/msword"}},
        {"odt", {"OpenDocument text", "document", "office", "application/vnd.oasis.opendocument.text"}},
        {"rtf", {"Rich Text document", "document", "office", "application/rtf"}},
        {"epub", {"EPUB ebook", "document", "office", "application/epub+zip"}},
        {"md", {"Markdown text", "document", "toolkit", "text/markdown"}},
        {"txt", {"Plain text", "document", "toolkit", "text/plain"}},
        {"xlsx", {"Excel spreadsheet", "spreadsheet", "office",
                  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"}},
        {"xls", {"Excel 97-2003 spreadsheet", "spreadsheet", "office", "application/vnd.ms-excel"}},
        {"ods", {"OpenDocument spreadsheet", "spreadsheet", "office",
                 "application/vnd.oasis.opendocument.spreadsheet"}},
        {"csv", {"CSV table", "spreadsheet", "office", "text/csv"}},
        {"tsv", {"TSV table", "spreadsheet", "office", "text/tab-separated-values"}},
        {"pptx", {"PowerPoint presentation", "slides", "office",
                  "application/vnd.openxmlformats-officedocument.presentationml.presentation"}},
        {"ppt", {"PowerPoint 97-2003 presentation", "slides", "office", "application/vnd.ms-powerpoint"}},
        {"odp", {"OpenDocument presentation", "slides", "office",
                 "application/vnd.oasis.opendocument.presentation"}},
        // archives
        {"zip", {"ZIP archive", "archive", "toolkit", "application/zip"}},
        {"7z", {"7-Zip archive", "archive", "toolkit", "application/x-7z-compressed"}},
        {"rar", {"RAR archive", "archive", "toolkit", "application/vnd.rar"}},
        {"tar", {"TAR archive", "archive", "toolkit", "application/x-tar"}},
        {"gz", {"Gzip archive", "archive", "toolkit", "application/gzip"}},
        {"xz", {"XZ archive", "archive", "toolkit", "application/x-xz"}},
        {"zst", {"Zstandard archive", "archive", "toolkit", "application/zstd"}},
        {"bz2", {"Bzip2 archive", "archive", "toolkit", "application/x-bzip2"}},
        {"iso", {"Disc image", "archive", "toolkit", "application/x-iso9660-image"}},
        // fonts
        {"ttf", {"TrueType font", "font", "toolkit", "font/ttf"}},
        {"otf", {"OpenType font", "font", "toolkit", "font/otf"}},
        {"woff", {"WOFF font", "font", "toolkit", "font/woff"}},
        {"woff2", {"WOFF2 font", "font", "toolkit", "font/woff2"}},
        // data and code
        {"json", {"JSON data", "data", "toolkit", "application/json"}},
        {"yaml", {"YAML data", "data", "toolkit", "application/yaml"}},
        {"yml", {"YAML data", "data", "toolkit", "application/yaml"}},
        {"xml", {"XML data", "data", "toolkit", "application/xml"}},
        {"toml", {"TOML data", "data", "toolkit", "application/toml"}},
        {"sqlite", {"SQLite database", "data", "toolkit", "application/vnd.sqlite3"}},
        {"db", {"SQLite database", "data", "toolkit", "application/vnd.sqlite3"}},
        {"c", {"C source", "code", "toolkit", "text/x-c"}},
        {"h", {"C header", "code", "toolkit", "text/x-c"}},
        {"cpp", {"C++ source", "code", "toolkit", "text/x-c++"}},
        {"hpp", {"C++ header", "code", "toolkit", "text/x-c++"}},
        {"rs", {"Rust source", "code", "toolkit", "text/x-rust"}},
        {"py", {"Python source", "code", "toolkit", "text/x-python"}},
        {"js", {"JavaScript source", "code", "toolkit", "text/javascript"}},
        {"ts", {"TypeScript source", "code", "toolkit", "text/x-typescript"}},
        {"html", {"HTML page", "code", "toolkit", "text/html"}},
        {"css", {"CSS stylesheet", "code", "toolkit", "text/css"}},
        {"sh", {"Shell script", "code", "toolkit", "text/x-shellscript"}},
        {"exe", {"Windows executable", "executable", "toolkit", "application/vnd.microsoft.portable-executable"}},
        {"dll", {"Windows library", "executable", "toolkit", "application/vnd.microsoft.portable-executable"}},
    };
    return table;
}

std::string extensionOf(const std::filesystem::path& name) {
    std::string ext = name.extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool startsWith(std::string_view head, std::string_view magic, std::size_t offset = 0) {
    return head.size() >= offset + magic.size() && head.substr(offset, magic.size()) == magic;
}

std::optional<FileType> fromExtension(const std::string& ext) {
    auto it = byExtension().find(ext);
    if (it == byExtension().end()) return std::nullopt;
    return it->second;
}

// Formats that are containers for several file types: trust the extension when it names one of them.
FileType refine(const FileType& generic, const std::string& ext, std::initializer_list<const char*> family) {
    for (const char* member : family) {
        if (ext == member) return *fromExtension(ext);
    }
    return generic;
}

bool looksLikeText(std::string_view head) {
    if (head.empty()) return true;
    std::size_t odd = 0;
    for (unsigned char c : head) {
        if (c == 0) return false;
        if (c < 0x09 || (c > 0x0d && c < 0x20)) ++odd;
    }
    return odd * 100 < head.size();  // under 1% control characters
}

std::optional<FileType> fromContent(std::string_view h, const std::string& ext) {
    using namespace std::string_view_literals;

    if (startsWith(h, "\x89PNG\r\n\x1a\n"sv)) return fromExtension("png");
    if (startsWith(h, "\xff\xd8\xff"sv)) return fromExtension("jpg");
    if (startsWith(h, "GIF87a") || startsWith(h, "GIF89a")) return fromExtension("gif");
    if (startsWith(h, "8BPS")) return fromExtension("psd");
    if (startsWith(h, "\x76\x2f\x31\x01"sv)) return fromExtension("exr");
    if (startsWith(h, "#?RADIANCE") || startsWith(h, "#?RGBE")) return fromExtension("hdr");
    if (startsWith(h, "gimp xcf")) return fromExtension("xcf");
    if (startsWith(h, "BM") && h.size() >= 26 && !looksLikeText(h)) return fromExtension("bmp");
    if (startsWith(h, "II*\0"sv) || startsWith(h, "MM\0*"sv)) {
        return refine(*fromExtension("tiff"), ext, {"cr2", "nef", "arw", "dng", "orf", "rw2"});
    }
    if (startsWith(h, "FUJIFILMCCD-RAW")) return fromExtension("raf");
    if (startsWith(h, "RIFF") && h.size() >= 12) {
        auto kind = h.substr(8, 4);
        if (kind == "WEBP") return fromExtension("webp");
        if (kind == "WAVE") return fromExtension("wav");
        if (kind == "AVI ") return fromExtension("avi");
    }
    if (startsWith(h, "FORM") && h.size() >= 12 && (h.substr(8, 4) == "AIFF" || h.substr(8, 4) == "AIFC")) {
        return fromExtension("aiff");
    }
    if (startsWith(h, "ftyp", 4) && h.size() >= 12) {
        auto brand = h.substr(8, 4);
        if (brand == "heic" || brand == "heix" || brand == "mif1" || brand == "msf1") return fromExtension("heic");
        if (brand == "avif" || brand == "avis") return fromExtension("avif");
        if (brand == "crx ") return fromExtension("cr3");
        if (brand == "qt  ") return fromExtension("mov");
        if (brand == "M4A " || brand == "M4B ") return fromExtension("m4a");
        return fromExtension("mp4");
    }
    if (startsWith(h, "\x1a\x45\xdf\xa3"sv)) {
        return h.find("webm") != std::string_view::npos ? fromExtension("webm") : fromExtension("mkv");
    }
    if (startsWith(h, "fLaC")) return fromExtension("flac");
    if (startsWith(h, "OggS")) return h.find("OpusHead") != std::string_view::npos ? fromExtension("opus") : fromExtension("ogg");
    if (startsWith(h, "ID3")) return fromExtension("mp3");
    if (startsWith(h, "MThd")) return fromExtension("mid");
    if (h.size() >= 2 && static_cast<unsigned char>(h[0]) == 0xff && (static_cast<unsigned char>(h[1]) & 0xe0) == 0xe0) {
        return (static_cast<unsigned char>(h[1]) & 0x06) == 0 ? fromExtension("aac") : fromExtension("mp3");
    }

    if (startsWith(h, "%PDF-")) return refine(*fromExtension("pdf"), ext, {"ai"});
    if (startsWith(h, "%!PS")) return fromExtension("eps");
    if (startsWith(h, "{\\rtf")) return fromExtension("rtf");
    if (startsWith(h, "PK\x03\x04"sv) || startsWith(h, "PK\x05\x06"sv)) {
        return refine(*fromExtension("zip"), ext,
                      {"docx", "xlsx", "pptx", "odt", "ods", "odp", "epub", "3mf", "kra", "ora", "usdz"});
    }
    if (startsWith(h, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1"sv)) {
        return refine({"OLE2 compound file", "binary", "toolkit", "application/x-ole-storage"}, ext, {"doc", "xls", "ppt"});
    }
    if (startsWith(h, "7z\xbc\xaf\x27\x1c"sv)) return fromExtension("7z");
    if (startsWith(h, "Rar!\x1a\x07"sv)) return fromExtension("rar");
    if (startsWith(h, "\x1f\x8b"sv)) return fromExtension("gz");
    if (startsWith(h, "\xfd" "7zXZ\0"sv)) return fromExtension("xz");
    if (startsWith(h, "\x28\xb5\x2f\xfd"sv)) return fromExtension("zst");
    if (startsWith(h, "BZh")) return fromExtension("bz2");
    if (startsWith(h, "ustar", 257)) return fromExtension("tar");

    if (startsWith(h, "SQLite format 3\0"sv)) return fromExtension("sqlite");
    if (startsWith(h, "glTF")) return fromExtension("glb");
    if (startsWith(h, "Kaydara FBX Binary")) return fromExtension("fbx");
    if (startsWith(h, "BLENDER")) return fromExtension("blend");
    if (startsWith(h, "ISO-10303-21")) return fromExtension("step");
    if (startsWith(h, "AC10")) return fromExtension("dwg");
    if (startsWith(h, "DDS ")) return fromExtension("dds");
    if (startsWith(h, "\xabKTX 20\xbb"sv)) return fromExtension("ktx2");
    if (startsWith(h, "ply\n") || startsWith(h, "ply\r\n")) return fromExtension("ply");

    if (startsWith(h, "\0\x01\0\0"sv) && (ext == "ttf" || ext.empty())) return fromExtension("ttf");
    if (startsWith(h, "OTTO")) return fromExtension("otf");
    if (startsWith(h, "wOFF")) return fromExtension("woff");
    if (startsWith(h, "wOF2")) return fromExtension("woff2");

    if (startsWith(h, "\x7f" "ELF"sv)) return FileType{"ELF executable", "executable", "toolkit", "application/x-elf"};
    if (startsWith(h, "MZ")) return refine(*fromExtension("exe"), ext, {"dll"});
    if (startsWith(h, "\xcf\xfa\xed\xfe"sv) || startsWith(h, "\xfe\xed\xfa\xcf"sv)) {
        return FileType{"Mach-O executable", "executable", "toolkit", "application/x-mach-binary"};
    }

    // text formats that announce themselves
    if (looksLikeText(h)) {
        if (startsWith(h, "solid ") && (ext == "stl" || ext.empty())) return fromExtension("stl");
        if (h.find("<svg") != std::string_view::npos) return fromExtension("svg");
        if (startsWith(h, "<?xml") && h.find("COLLADA") != std::string_view::npos) return fromExtension("dae");
    }
    return std::nullopt;
}

}  // namespace

FileType detect(std::string_view head, const std::filesystem::path& name) {
    const std::string ext = extensionOf(name);
    if (auto t = fromContent(head, ext)) return *t;
    if (auto t = fromExtension(ext)) return *t;
    if (looksLikeText(head)) return {"Plain text", "document", "toolkit", "text/plain"};
    return {"Unknown binary", "binary", "toolkit", "application/octet-stream"};
}

FileType detectFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::string head(kHeadSize, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    return detect(head, path.filename());
}

std::string hexDump(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t length) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    in.seekg(static_cast<std::streamoff>(offset));
    std::vector<char> data(static_cast<std::size_t>(length));
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(in.gcount()));

    std::string out;
    char buf[16];
    for (std::size_t row = 0; row < data.size(); row += 16) {
        std::snprintf(buf, sizeof buf, "%08llx ", static_cast<unsigned long long>(offset + row));
        out += buf;
        std::string ascii;
        for (std::size_t i = 0; i < 16; ++i) {
            if (i == 8) out += ' ';
            if (row + i < data.size()) {
                auto c = static_cast<unsigned char>(data[row + i]);
                std::snprintf(buf, sizeof buf, " %02x", c);
                out += buf;
                ascii += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
            } else {
                out += "   ";
            }
        }
        out += "  |" + ascii + "|\n";
    }
    return out;
}

namespace {

class InspectAction : public Action {
public:
    explicit InspectAction(std::string path) : path_(std::move(path)) {}
    Result apply() override {
        std::error_code ec;
        auto size = std::filesystem::file_size(path_, ec);
        if (ec) return Result::failure("cannot read " + path_ + ": " + ec.message());
        try {
            FileType t = detectFile(path_);
            return Result::success("format: " + t.format + "\ncategory: " + t.category + "\nopens in: " + t.tool +
                                   "\nmime: " + t.mime + "\nsize: " + std::to_string(size) + " bytes");
        } catch (const std::exception& e) {
            return Result::failure(e.what());
        }
    }

private:
    std::string path_;
};

class HexAction : public Action {
public:
    HexAction(std::string path, std::int64_t offset, std::int64_t length)
        : path_(std::move(path)), offset_(offset), length_(length) {}
    Result apply() override {
        if (offset_ < 0 || length_ <= 0 || length_ > 65536) {
            return Result::failure("offset must be >= 0 and length between 1 and 65536");
        }
        try {
            return Result::success(hexDump(path_, static_cast<std::uint64_t>(offset_), static_cast<std::uint64_t>(length_)));
        } catch (const std::exception& e) {
            return Result::failure(e.what());
        }
    }

private:
    std::string path_;
    std::int64_t offset_;
    std::int64_t length_;
};

std::int64_t intOr(const Params& p, const std::string& key, std::int64_t fallback) {
    auto it = p.find(key);
    if (it == p.end()) return fallback;
    if (auto v = std::get_if<std::int64_t>(&it->second)) return *v;
    return fallback;
}

}  // namespace

void registerCommands(CommandRegistry& registry) {
    registry.add({"toolkit.inspect",
                  "Inspect file",
                  "Identify a file's format from its contents and say which suite tool opens it",
                  {{"path", ParamType::String, "path to the file", true}},
                  false},
                 [](const Params& p) { return std::make_unique<InspectAction>(std::get<std::string>(p.at("path"))); });

    registry.add({"toolkit.hex",
                  "Hex dump",
                  "Show raw bytes of a file as hex and ASCII",
                  {{"path", ParamType::String, "path to the file", true},
                   {"offset", ParamType::Int, "byte offset to start at (default 0)", false},
                   {"length", ParamType::Int, "number of bytes, up to 65536 (default 256)", false}},
                  false},
                 [](const Params& p) {
                     return std::make_unique<HexAction>(std::get<std::string>(p.at("path")), intOr(p, "offset", 0),
                                                        intOr(p, "length", 256));
                 });
}

}  // namespace achclwss::toolkit
