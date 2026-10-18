#include "wetwaresuite/gui/palette.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace wetwaresuite::gui {

namespace {

std::string lowerTrim(const std::string& text) {
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    std::string out = text.substr(b, e - b);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

Rgba rgb(std::uint32_t v) { return fromRgb24(v); }

const Rgba kBlack{0, 0, 0, 1};
const Rgba kWhite{1, 1, 1, 1};

struct NamedColour {
    const char* name;
    std::uint32_t rgb;
};
const NamedColour kNames[] = {
    {"black", 0x000000}, {"white", 0xffffff},  {"grey", 0x808080},   {"gray", 0x808080},  {"red", 0xd32f2f},
    {"orange", 0xf57c00}, {"yellow", 0xfbc02d}, {"green", 0x388e3c},  {"teal", 0x00897b}, {"blue", 0x1e66f5},
    {"navy", 0x1a237e},  {"purple", 0x7b1fa2}, {"pink", 0xd81b60},   {"brown", 0x6d4c41},
};

}  // namespace

bool operator==(const Palette& a, const Palette& b) { return hex6(a.primary) == hex6(b.primary) && hex6(a.secondary) == hex6(b.secondary) && hex6(a.tertiary) == hex6(b.tertiary); }

Palette defaultPalette(std::uint32_t accent) { return {rgb(accent), rgb(0x292b30), rgb(0x6b707a)}; }

std::optional<Rgba> parsePaletteColour(const std::string& text, const std::string& role, std::string& error) {
    std::string t = lowerTrim(text);
    for (const auto& n : kNames)
        if (t == n.name) return rgb(n.rgb);
    std::string digits = !t.empty() && t[0] == '#' ? t.substr(1) : t;
    bool hex = !digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return hexValue(c) >= 0; });
    if (hex && digits.size() == 3) {
        std::string wide;
        for (char c : digits) wide += std::string(2, c);
        digits = wide;
    }
    if (!hex || digits.size() != 6) {
        std::string shown = text.empty() ? std::string("nothing") : "\"" + text + "\"";
        error = "The " + role + " colour " + shown + " isn't a colour. Write # and six digits or letters a-f, like #3d7eff";
        if (hex && digits.size() == 8) error += " (see-through colours aren't used here)";
        error += ", or a plain name such as blue.";
        return std::nullopt;
    }
    std::uint32_t v = 0;
    for (char c : digits) v = v * 16 + static_cast<std::uint32_t>(hexValue(c));
    return rgb(v);
}

std::string hex6(const Rgba& c) {
    Rgba o = c;
    o.a = 1;
    return toHex(o);
}

const std::vector<PalettePreset>& palettePresets() {
    static const std::vector<PalettePreset> presets = {
        {"high-contrast", "High contrast", "Yellow and cyan on black: the easiest to read, for low vision or bright rooms.",
         {rgb(0xffd400), rgb(0x000000), rgb(0x00e5ff)}},
        {"light", "Light", "Dark text on pale grey panels with a blue main colour.", {rgb(0x1f5fd1), rgb(0xe4e7ec), rgb(0x7a3fc4)}},
        {"colour-blind", "Colour-blind friendly",
         "Orange and sky blue (the Okabe-Ito colours), which stay apart with every common kind of colour blindness.",
         {rgb(0xe69f00), rgb(0x1d2733), rgb(0x56b4e9)}},
        {"warm", "Warm", "Coral and gold on a dark brown, easy on the eyes at night.", {rgb(0xe8613c), rgb(0x2e2622), rgb(0xf2b134)}},
    };
    return presets;
}

const PalettePreset* findPreset(const std::string& name) {
    const std::string n = lowerTrim(name);
    for (const auto& p : palettePresets())
        if (p.name == n) return &p;
    return nullptr;
}

std::vector<std::string> presetNames() {
    std::vector<std::string> out{"default"};
    for (const auto& p : palettePresets()) out.push_back(p.name);
    return out;
}

// ---- the settings file ------------------------------------------------------------

std::optional<Palette> parsePaletteFile(const std::string& text) {
    std::optional<Rgba> found[3];
    static const char* keys[3] = {"primary", "secondary", "tertiary"};
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::string t = lowerTrim(line);  // also drops a trailing \r
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lowerTrim(t.substr(0, eq));
        for (int i = 0; i < 3; ++i) {
            if (key != keys[i]) continue;
            std::string error;
            found[i] = parsePaletteColour(t.substr(eq + 1), key, error);
        }
    }
    if (!found[0] || !found[1] || !found[2]) return std::nullopt;
    return Palette{*found[0], *found[1], *found[2]};
}

std::string formatPaletteFile(const Palette& p) {
    return "# wetwaresuite colours, shared by every app. Change them in any app's Aa menu,\n"
           "# or with app.palette_set; delete this file (or app.palette_reset) for each app's own colours.\n"
           "primary=" + hex6(p.primary) + "\nsecondary=" + hex6(p.secondary) + "\ntertiary=" + hex6(p.tertiary) + "\n";
}

std::optional<Palette> loadPalette(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream buf;
    buf << in.rdbuf();
    return parsePaletteFile(buf.str());
}

bool savePalette(const fs::path& file, const Palette& palette, std::string& error) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << formatPaletteFile(palette);
        if (!out) {
            error = "couldn't write " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        error = "couldn't save " + file.string();
        return false;
    }
    return true;
}

// ---- contrast and shades ----------------------------------------------------------

Rgba mixColour(const Rgba& a, const Rgba& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

float relativeLuminance(const Rgba& c) {
    auto lin = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

float contrastRatio(const Rgba& a, const Rgba& b) {
    float la = relativeLuminance(a), lb = relativeLuminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05f) / (lb + 0.05f);
}

Rgba readableOn(const Rgba& background) {
    return contrastRatio(kWhite, background) >= contrastRatio(kBlack, background) ? kWhite : kBlack;
}

Rgba ensureContrast(const Rgba& c, const Rgba& against, float minRatio) {
    if (contrastRatio(c, against) >= minRatio) return c;
    const Rgba target = readableOn(against);
    for (int step = 1; step <= 20; ++step) {
        Rgba m = mixColour(c, target, static_cast<float>(step) / 20.0f);
        if (contrastRatio(m, against) >= minRatio) return m;
    }
    return target;
}

Rgba mixReadable(const Rgba& base, const Rgba& toward, float t, const Rgba& text, float minRatio) {
    for (; t > 0.001f; t -= 0.05f) {
        Rgba m = mixColour(base, toward, t);
        if (contrastRatio(m, text) >= minRatio) return m;
    }
    return base;
}

Theme deriveTheme(const Palette& palette) {
    Theme th;
    const Rgba primary{palette.primary.r, palette.primary.g, palette.primary.b, 1};
    const Rgba secondary{palette.secondary.r, palette.secondary.g, palette.secondary.b, 1};
    const Rgba tertiary{palette.tertiary.r, palette.tertiary.g, palette.tertiary.b, 1};
    th.text = readableOn(secondary);
    th.dark = th.text.r > 0.5f;
    const Rgba away = th.dark ? kBlack : kWhite;  // the direction surfaces go to stay behind the text
    // surfaces: the secondary colour is the boxes; the background sits a step further from the text
    th.frame = ensureContrast(secondary, th.text, 7.0f);
    th.background = mixColour(th.frame, away, th.dark ? 0.45f : 0.65f);
    th.popup = mixColour(th.frame, away, th.dark ? 0.2f : 0.85f);
    // muted text: halfway to the surfaces, but still readable on every one of them
    Rgba muted = mixColour(th.text, th.frame, 0.3f);
    for (const Rgba* s : {&th.frame, &th.background, &th.popup}) muted = ensureContrast(muted, *s, kTextContrast);
    th.textMuted = muted;
    // hover and pressed shades lean toward the main colour, never so far that text gets hard to read
    th.frameHover = mixReadable(th.frame, primary, 0.25f, th.text, kTextContrast);
    th.frameActive = mixReadable(th.frame, primary, 0.40f, th.text, kTextContrast);
    th.button = mixReadable(th.frame, primary, th.dark ? 0.30f : 0.18f, th.text, kTextContrast);
    th.buttonHover = mixReadable(th.frame, primary, 0.55f, th.text, kTextContrast);
    th.buttonActive = mixReadable(th.frame, primary, 0.80f, th.text, kTextContrast);
    th.header = mixReadable(th.background, primary, 0.35f, th.text, kTextContrast);
    th.headerHover = mixReadable(th.background, primary, 0.50f, th.text, kTextContrast);
    th.headerActive = mixReadable(th.background, primary, 0.70f, th.text, kTextContrast);
    // filled main buttons: the primary itself with black or white text
    th.accent = primary;
    th.accentText = readableOn(primary);
    const Rgba shade = th.accentText.r > 0.5f ? kBlack : kWhite;  // darker under white text, lighter under black
    th.accentHover = mixReadable(primary, readableOn(shade), 0.2f, th.accentText, kTextContrast);
    th.accentActive = mixReadable(primary, shade, 0.2f, th.accentText, kTextContrast);
    th.title = ensureContrast(primary, th.background, kTextContrast);
    th.highlight = ensureContrast(primary, th.frame, kUiContrast);
    // the details
    th.border = ensureContrast(tertiary, th.background, kUiContrast);
    th.focus = ensureContrast(tertiary, th.background, kUiContrast);
    th.link = ensureContrast(ensureContrast(tertiary, th.background, kTextContrast), th.frame, kTextContrast);
    th.selection = {tertiary.r, tertiary.g, tertiary.b, 0.40f};
    return th;
}

}  // namespace wetwaresuite::gui
