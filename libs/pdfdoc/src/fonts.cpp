#include "wetwaresuite/pdfdoc/fonts.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace wetwaresuite::pdfdoc {

extern const unsigned char kBuiltinSans[];
extern const unsigned int kBuiltinSansSize;

namespace {

#include "std_metrics.inc"

// Unicode of WinAnsi codes 128..159 (0 = unused).
constexpr unsigned kHigh[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// ---- encoding ---------------------------------------------------------------

std::vector<unsigned> utf8Decode(const std::string& text) {
    std::vector<unsigned> out;
    auto byte = [&](std::size_t j) { return static_cast<unsigned>(static_cast<unsigned char>(text[j])); };
    auto cont = [&](std::size_t j) { return j < text.size() && (byte(j) & 0xC0u) == 0x80u; };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned ch = byte(i);
        if (ch < 0x80) {
            out.push_back(ch);
        } else if ((ch & 0xE0u) == 0xC0u && cont(i + 1)) {
            out.push_back(((ch & 0x1Fu) << 6) | (byte(i + 1) & 0x3Fu));
            i += 1;
        } else if ((ch & 0xF0u) == 0xE0u && cont(i + 1) && cont(i + 2)) {
            out.push_back(((ch & 0x0Fu) << 12) | ((byte(i + 1) & 0x3Fu) << 6) | (byte(i + 2) & 0x3Fu));
            i += 2;
        } else if ((ch & 0xF8u) == 0xF0u && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            out.push_back(((ch & 0x07u) << 18) | ((byte(i + 1) & 0x3Fu) << 12) | ((byte(i + 2) & 0x3Fu) << 6) | (byte(i + 3) & 0x3Fu));
            i += 3;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string utf8Encode(unsigned cp) {
    std::string s;
    auto put = [&](unsigned v) { s += static_cast<char>(static_cast<unsigned char>(v)); };
    if (cp < 0x80) {
        put(cp);
    } else if (cp < 0x800) {
        put(0xC0 | (cp >> 6));
        put(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        put(0xE0 | (cp >> 12));
        put(0x80 | ((cp >> 6) & 0x3F));
        put(0x80 | (cp & 0x3F));
    } else {
        put(0xF0 | (cp >> 18));
        put(0x80 | ((cp >> 12) & 0x3F));
        put(0x80 | ((cp >> 6) & 0x3F));
        put(0x80 | (cp & 0x3F));
    }
    return s;
}

int winAnsi(unsigned cp) {
    if (cp >= 32 && cp < 127) return static_cast<int>(cp);
    if (cp == 0xA0) return 32;  // no-break space prints as a space
    if (cp >= 0xA1 && cp <= 0xFF) return static_cast<int>(cp);
    for (int i = 0; i < 32; ++i) {
        if (kHigh[i] && kHigh[i] == cp) return 128 + i;
    }
    return -1;
}

unsigned unicodeOf(int code) {
    if (code >= 128 && code < 160) return kHigh[code - 128];
    return code >= 32 && code <= 255 && code != 127 ? static_cast<unsigned>(code) : 0;
}

std::string toWinAnsi(const std::string& utf8) {
    std::string out;
    for (unsigned cp : utf8Decode(utf8)) {
        int c = winAnsi(cp);
        out += static_cast<char>(c < 0 ? '?' : c);
    }
    return out;
}

// ---- standard fonts -----------------------------------------------------------

std::optional<std::string> standardFamily(const std::string& name) {
    std::string n = lower(name);
    n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return c == ' ' || c == '-' || c == '_'; }), n.end());
    if (n == "helvetica" || n == "sans" || n == "sansserif" || n == "arial" || n == "liberationsans" || n == "arimo") {
        return "helvetica";
    }
    if (n == "times" || n == "timesroman" || n == "serif" || n == "timesnewroman" || n == "roman" || n == "liberationserif" ||
        n == "tinos") {
        return "times";
    }
    if (n == "courier" || n == "mono" || n == "monospace" || n == "couriernew" || n == "liberationmono" || n == "cousine") {
        return "courier";
    }
    return std::nullopt;
}

Standard standardFont(const std::string& family, bool bold, bool italic) {
    const int style = (bold ? 1 : 0) + (italic ? 2 : 0);
    const int base = family == "times" ? 4 : family == "courier" ? 8 : 0;
    return static_cast<Standard>(base + style);  // regular, bold, italic/oblique, bold italic
}

const char* baseFontName(Standard f) {
    static const char* names[] = {"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
                                  "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
                                  "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"};
    return names[static_cast<int>(f)];
}

bool isBold(Standard f) { return (static_cast<int>(f) % 4) % 2 == 1; }
bool isItalic(Standard f) { return (static_cast<int>(f) % 4) >= 2; }

double standardWidth(Standard f, int code) {
    if (code < 32 || code > 255) return 0;
    const int i = code - 32;
    switch (f) {
        case Standard::Helvetica:
        case Standard::HelveticaOblique: return kHelvetica[i];
        case Standard::HelveticaBold:
        case Standard::HelveticaBoldOblique: return kHelveticaBold[i];
        case Standard::TimesRoman: return kTimesRoman[i];
        case Standard::TimesBold: return kTimesBold[i];
        case Standard::TimesItalic: return kTimesItalic[i];
        case Standard::TimesBoldItalic: return kTimesBoldItalic[i];
        default: return kHelvetica[i] == 0 ? 0 : 600;
    }
}

VMetrics standardMetrics(Standard f) {
    const int i = static_cast<int>(f);
    if (i < 4) return {718, -207, 718};
    if (i < 8) return {683, -217, f == Standard::TimesBold ? 676.0 : 662.0};
    return {629, -157, 562};
}

// ---- TrueType ---------------------------------------------------------------

struct TrueType::Impl {
    std::filesystem::path path;
    std::string bytes;
    stbtt_fontinfo info{};
    double scale = 1;  // font units -> 1/1000 em
    std::uint32_t table(const char* tag) const {
        auto* d = const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(bytes.data()));
        return stbtt__find_table(d, static_cast<stbtt_uint32>(info.fontstart), tag);
    }
    mutable std::map<unsigned, Contours> outlines;
};

TrueType::TrueType() : impl_(std::make_unique<Impl>()) {}
TrueType::~TrueType() = default;

std::shared_ptr<TrueType> TrueType::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't open font " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    auto font = fromBytes(ss.str(), path.string());
    font->impl_->path = path;
    return font;
}

std::shared_ptr<TrueType> TrueType::fromBytes(std::string bytes, const std::string& name) {
    std::shared_ptr<TrueType> font(new TrueType());
    auto& im = *font->impl_;
    im.path = name;
    im.bytes = std::move(bytes);
    const auto* data = reinterpret_cast<const unsigned char*>(im.bytes.data());
    const int offset = im.bytes.size() > 12 ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
    if (offset < 0 || !stbtt_InitFont(&im.info, data, offset)) throw std::runtime_error(name + " is not a TrueType font");
    im.scale = 1000.0 * stbtt_ScaleForMappingEmToPixels(&im.info, 1.0f);
    return font;
}

const std::filesystem::path& TrueType::path() const { return impl_->path; }
const std::string& TrueType::bytes() const { return impl_->bytes; }

std::string TrueType::postscriptName() const {
    std::string name;
    int len = 0;
    if (const char* s = stbtt_GetFontNameString(&impl_->info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                                STBTT_MS_LANG_ENGLISH, 6)) {
        for (int i = 1; i < len; i += 2) name += s[i];  // UTF-16BE, keep the low bytes
    } else if (const char* m = stbtt_GetFontNameString(&impl_->info, &len, STBTT_PLATFORM_ID_MAC, 0, 0, 6)) {
        name.assign(m, static_cast<std::size_t>(len));
    }
    std::string safe;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') safe += c;
    }
    return safe.empty() ? "EmbeddedFont" : safe;
}

bool TrueType::embeddable() const { return impl_->table("glyf") != 0; }

bool TrueType::hasGlyph(unsigned cp) const { return stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(cp)) != 0; }

double TrueType::advance(unsigned cp) const {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&impl_->info, static_cast<int>(cp), &adv, &lsb);
    return adv * impl_->scale;
}

VMetrics TrueType::metrics() const {
    VMetrics m;
    int a = 0, d = 0, gap = 0;
    if (!stbtt_GetFontVMetricsOS2(&impl_->info, &a, &d, &gap)) stbtt_GetFontVMetrics(&impl_->info, &a, &d, &gap);
    m.ascent = a * impl_->scale;
    m.descent = d * impl_->scale;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (stbtt_GetCodepointBox(&impl_->info, 'H', &x0, &y0, &x1, &y1)) m.capHeight = y1 * impl_->scale;
    else m.capHeight = m.ascent * 0.7;
    return m;
}

double TrueType::italicAngle() const {
    const std::uint32_t post = impl_->table("post");
    if (!post || post + 8 > impl_->bytes.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(impl_->bytes.data()) + post + 4;
    const auto fixed = static_cast<std::int32_t>((std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3]);
    return fixed / 65536.0;
}

bool TrueType::fixedPitch() const {
    const std::uint32_t post = impl_->table("post");
    if (!post || post + 16 > impl_->bytes.size()) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(impl_->bytes.data()) + post + 12;
    return (p[0] | p[1] | p[2] | p[3]) != 0;
}

void TrueType::bbox(double out[4]) const {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetFontBoundingBox(&impl_->info, &x0, &y0, &x1, &y1);
    out[0] = x0 * impl_->scale;
    out[1] = y0 * impl_->scale;
    out[2] = x1 * impl_->scale;
    out[3] = y1 * impl_->scale;
}

const Contours& TrueType::outline(unsigned cp) const {
    auto it = impl_->outlines.find(cp);
    if (it != impl_->outlines.end()) return it->second;
    Contours& out = impl_->outlines[cp];
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetCodepointShape(&impl_->info, static_cast<int>(cp), &v);
    const double s = impl_->scale;
    Pt cur;
    for (int i = 0; i < n; ++i) {
        const Pt p{v[i].x * s, v[i].y * s};
        if (v[i].type == STBTT_vmove) {
            out.push_back({p});
        } else if (!out.empty()) {
            auto& c = out.back();
            if (v[i].type == STBTT_vline) {
                c.push_back(p);
            } else {
                const Pt c1{v[i].cx * s, v[i].cy * s};
                const Pt c2{v[i].cx1 * s, v[i].cy1 * s};
                const bool cubic = v[i].type == STBTT_vcubic;
                const double len = std::hypot(c1.x - cur.x, c1.y - cur.y) + std::hypot(p.x - c1.x, p.y - c1.y);
                const int steps = std::clamp(static_cast<int>(len / 20), 3, 24);
                for (int k = 1; k <= steps; ++k) {
                    const double t = static_cast<double>(k) / steps, u = 1 - t;
                    if (cubic) {
                        c.push_back({u * u * u * cur.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * p.x,
                                     u * u * u * cur.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * p.y});
                    } else {
                        c.push_back({u * u * cur.x + 2 * u * t * c1.x + t * t * p.x, u * u * cur.y + 2 * u * t * c1.y + t * t * p.y});
                    }
                }
            }
        }
        cur = p;
    }
    if (v) stbtt_FreeShape(&impl_->info, v);
    out.erase(std::remove_if(out.begin(), out.end(), [](const auto& c) { return c.size() < 3; }), out.end());
    return out;
}

std::string TrueType::svgPath(unsigned cp, double x, double y, double size) const {
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetCodepointShape(&impl_->info, static_cast<int>(cp), &v);
    const double k = impl_->scale * size / 1000.0;
    std::string d;
    char buf[160];
    auto X = [&](double fx) { return x + fx * k; };
    auto Y = [&](double fy) { return y - fy * k; };
    bool open = false;
    for (int i = 0; i < n; ++i) {
        switch (v[i].type) {
            case STBTT_vmove:
                if (open) d += "Z";
                std::snprintf(buf, sizeof buf, "M%.2f %.2f", X(v[i].x), Y(v[i].y));
                open = true;
                break;
            case STBTT_vline: std::snprintf(buf, sizeof buf, "L%.2f %.2f", X(v[i].x), Y(v[i].y)); break;
            case STBTT_vcurve:
                std::snprintf(buf, sizeof buf, "Q%.2f %.2f %.2f %.2f", X(v[i].cx), Y(v[i].cy), X(v[i].x), Y(v[i].y));
                break;
            default:
                std::snprintf(buf, sizeof buf, "C%.2f %.2f %.2f %.2f %.2f %.2f", X(v[i].cx), Y(v[i].cy), X(v[i].cx1), Y(v[i].cy1),
                              X(v[i].x), Y(v[i].y));
                break;
        }
        d += buf;
    }
    if (open) d += "Z";
    if (v) stbtt_FreeShape(&impl_->info, v);
    return d;
}

double TrueType::kern(unsigned left, unsigned right) const {
    return stbtt_GetCodepointKernAdvance(&impl_->info, static_cast<int>(left), static_cast<int>(right)) * impl_->scale;
}

std::shared_ptr<TrueType> builtinSans() {
    static std::shared_ptr<TrueType> font = [] {
        return TrueType::fromBytes(std::string(reinterpret_cast<const char*>(kBuiltinSans), kBuiltinSansSize), "Roboto Medium (built in)");
    }();
    return font;
}

// ---- system fonts -------------------------------------------------------------

namespace {

// An environment variable, or empty (MSVC wants _dupenv_s over getenv).
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

std::vector<std::filesystem::path> fontDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const auto d = env("WETWARESUITE_FONT_DIR"); !d.empty()) dirs.emplace_back(d);
#ifdef _WIN32
    if (const auto w = env("WINDIR"); !w.empty()) dirs.push_back(std::filesystem::path(w) / "Fonts");
    else dirs.emplace_back("C:\\Windows\\Fonts");
    if (const auto l = env("LOCALAPPDATA"); !l.empty()) dirs.push_back(std::filesystem::path(l) / "Microsoft" / "Windows" / "Fonts");
#else
    const std::string homeDir = env("HOME");
    const char* home = homeDir.empty() ? nullptr : homeDir.c_str();
#ifdef __APPLE__
    dirs.emplace_back("/System/Library/Fonts/Supplemental");
    dirs.emplace_back("/System/Library/Fonts");
    dirs.emplace_back("/Library/Fonts");
    if (home) dirs.push_back(std::filesystem::path(home) / "Library" / "Fonts");
#else
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
    if (home) {
        dirs.push_back(std::filesystem::path(home) / ".fonts");
        dirs.push_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
    }
#endif
#endif
    return dirs;
}

// Lowercase file name -> path of every font file in the font folders.
const std::map<std::string, std::filesystem::path>& fontFiles() {
    static const std::map<std::string, std::filesystem::path> files = [] {
        std::map<std::string, std::filesystem::path> out;
        for (const auto& dir : fontDirs()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                if (it.depth() > 4) {
                    it.disable_recursion_pending();
                    continue;
                }
                std::error_code fe;
                if (!it->is_regular_file(fe)) continue;
                const std::string name = lower(it->path().filename().string());
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ttf") == 0) out.emplace(name, it->path());
            }
        }
        return out;
    }();
    return files;
}

std::vector<std::string> candidates(const std::string& family, int style) {
    // style: 0 regular, 1 bold, 2 italic, 3 bold italic
    static const char* sfx[4] = {"regular", "bold", "italic", "bolditalic"};
    std::vector<std::string> c;
    const std::string s = sfx[style];
    if (family == "helvetica") {
        c = {"liberationsans-" + s + ".ttf", "arimo-" + s + ".ttf"};
        static const char* win[4] = {"arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"};
        static const char* mac[4] = {"arial.ttf", "arial bold.ttf", "arial italic.ttf", "arial bold italic.ttf"};
        static const char* fr[4] = {"freesans.ttf", "freesansbold.ttf", "freesansoblique.ttf", "freesansboldoblique.ttf"};
        static const char* dj[4] = {"dejavusans.ttf", "dejavusans-bold.ttf", "dejavusans-oblique.ttf", "dejavusans-boldoblique.ttf"};
        c.insert(c.end(), {win[style], mac[style], fr[style], dj[style]});
    } else if (family == "times") {
        c = {"liberationserif-" + s + ".ttf", "tinos-" + s + ".ttf"};
        static const char* win[4] = {"times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"};
        static const char* mac[4] = {"times new roman.ttf", "times new roman bold.ttf", "times new roman italic.ttf",
                                     "times new roman bold italic.ttf"};
        static const char* fr[4] = {"freeserif.ttf", "freeserifbold.ttf", "freeserifitalic.ttf", "freeserifbolditalic.ttf"};
        static const char* dj[4] = {"dejavuserif.ttf", "dejavuserif-bold.ttf", "dejavuserif-italic.ttf", "dejavuserif-bolditalic.ttf"};
        c.insert(c.end(), {win[style], mac[style], fr[style], dj[style]});
    } else {
        c = {"liberationmono-" + s + ".ttf", "cousine-" + s + ".ttf"};
        static const char* win[4] = {"cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf"};
        static const char* mac[4] = {"courier new.ttf", "courier new bold.ttf", "courier new italic.ttf", "courier new bold italic.ttf"};
        static const char* fr[4] = {"freemono.ttf", "freemonobold.ttf", "freemonooblique.ttf", "freemonoboldoblique.ttf"};
        static const char* dj[4] = {"dejavusansmono.ttf", "dejavusansmono-bold.ttf", "dejavusansmono-oblique.ttf",
                                    "dejavusansmono-boldoblique.ttf"};
        c.insert(c.end(), {win[style], mac[style], fr[style], dj[style]});
    }
    return c;
}

}  // namespace

std::shared_ptr<TrueType> systemFont(const std::string& family, bool bold, bool italic) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<TrueType>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    const int style = (bold ? 1 : 0) + (italic ? 2 : 0);
    const std::string key = family + std::to_string(style);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::shared_ptr<TrueType> found;
    const auto& files = fontFiles();
    for (int s : {style, 0}) {
        for (const auto& name : candidates(family, s)) {
            auto f = files.find(name);
            if (f == files.end()) continue;
            try {
                found = TrueType::load(f->second);
                break;
            } catch (const std::exception&) {
            }
        }
        if (found) break;
    }
    cache[key] = found;
    return found;
}

}  // namespace wetwaresuite::pdfdoc
