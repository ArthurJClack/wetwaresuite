// Built-in footprints, generated from a few parameters. Millimetres, y down,
// origin at the part's centre; pad numbers match the symbol pin numbers.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "achclwss/circuit/circuit.hpp"

namespace achclwss::circuit {

namespace {

constexpr double kInch = 25.4;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void silkRect(Footprint& f, double x0, double y0, double x1, double y1) {
    f.silk.push_back({{x0, y0}, {x1, y0}});
    f.silk.push_back({{x1, y0}, {x1, y1}});
    f.silk.push_back({{x1, y1}, {x0, y1}});
    f.silk.push_back({{x0, y1}, {x0, y0}});
}

void silkCircle(Footprint& f, double cx, double cy, double r) {
    const int n = 24;
    for (int i = 0; i < n; ++i) {
        const double a0 = 2 * 3.14159265358979 * i / n, a1 = 2 * 3.14159265358979 * (i + 1) / n;
        f.silk.push_back({{cx + r * std::cos(a0), cy + r * std::sin(a0)}, {cx + r * std::cos(a1), cy + r * std::sin(a1)}});
    }
}

void finish(Footprint& f) {
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const auto& p : f.pads) {
        x0 = std::min(x0, p.pos.x - p.w / 2);
        x1 = std::max(x1, p.pos.x + p.w / 2);
        y0 = std::min(y0, p.pos.y - p.h / 2);
        y1 = std::max(y1, p.pos.y + p.h / 2);
    }
    for (const auto& [a, b] : f.silk) {
        x0 = std::min({x0, a.x, b.x});
        x1 = std::max({x1, a.x, b.x});
        y0 = std::min({y0, a.y, b.y});
        y1 = std::max({y1, a.y, b.y});
    }
    if (x0 > x1) x0 = y0 = -1, x1 = y1 = 1;
    f.x0 = x0 - 0.25, f.y0 = y0 - 0.25, f.x1 = x1 + 0.25, f.y1 = y1 + 0.25;
}

Footprint smd2(const std::string& name, const std::string& desc, double pitch, double w, double h, bool polarity) {
    Footprint f;
    f.name = name;
    f.description = desc;
    f.pads.push_back({"1", {-pitch / 2, 0}, w, h, PadShape::Rect, 0});
    f.pads.push_back({"2", {pitch / 2, 0}, w, h, PadShape::Rect, 0});
    const double sx = pitch / 2 - w / 2 - 0.2, sy = h / 2 + 0.2;
    if (sx > 0.1) {
        f.silk.push_back({{-sx, -sy}, {sx, -sy}});
        f.silk.push_back({{-sx, sy}, {sx, sy}});
    }
    if (polarity) {
        const double x = -pitch / 2 - w / 2 - 0.3;
        f.silk.push_back({{x, -sy}, {x, sy}});
    }
    finish(f);
    return f;
}

Footprint tht2(const std::string& name, const std::string& desc, double pitch, double pad, double drill, bool squareFirst) {
    Footprint f;
    f.name = name;
    f.description = desc;
    f.pads.push_back({"1", {-pitch / 2, 0}, pad, pad, squareFirst ? PadShape::Rect : PadShape::Circle, drill});
    f.pads.push_back({"2", {pitch / 2, 0}, pad, pad, PadShape::Circle, drill});
    return f;
}

Footprint axial(const std::string& name, const std::string& desc, double pitch, bool band) {
    Footprint f = tht2(name, desc, pitch, 1.6, 0.8, band);
    const double bx = pitch / 2 - 2.2, by = 1.1;
    silkRect(f, -bx, -by, bx, by);
    f.silk.push_back({{-pitch / 2 + 0.9, 0}, {-bx, 0}});
    f.silk.push_back({{bx, 0}, {pitch / 2 - 0.9, 0}});
    if (band) f.silk.push_back({{-bx + 0.5, -by}, {-bx + 0.5, by}});
    finish(f);
    return f;
}

Footprint inlinePads(const std::string& name, const std::string& desc, int n, double pitch, double w, double h, double drill,
                     PadShape shape, const std::vector<std::string>& numbers = {}) {
    Footprint f;
    f.name = name;
    f.description = desc;
    for (int i = 0; i < n; ++i) {
        const double x = (i - (n - 1) / 2.0) * pitch;
        const std::string num = i < static_cast<int>(numbers.size()) ? numbers[static_cast<std::size_t>(i)] : std::to_string(i + 1);
        f.pads.push_back({num, {x, 0}, w, h, i == 0 && shape != PadShape::Rect ? PadShape::Rect : shape, drill});
    }
    return f;
}

Footprint pinHeader(int n, bool dual) {
    Footprint f;
    const double p = 2.54;
    char name[48];
    if (dual) {
        const int rows = std::max(1, n);
        std::snprintf(name, sizeof name, "PinHeader_2x%02d", rows);
        f.name = name;
        f.description = "2x" + std::to_string(rows) + " pin header, 2.54 mm";
        for (int i = 0; i < rows; ++i) {
            const double y = (i - (rows - 1) / 2.0) * p;
            f.pads.push_back({std::to_string(2 * i + 1), {-p / 2, y}, 1.7, 1.7, i == 0 ? PadShape::Rect : PadShape::Oval, 1.0});
            f.pads.push_back({std::to_string(2 * i + 2), {p / 2, y}, 1.7, 1.7, PadShape::Oval, 1.0});
        }
        silkRect(f, -p, -rows * p / 2, p, rows * p / 2);
    } else {
        std::snprintf(name, sizeof name, "PinHeader_1x%02d", n);
        f.name = name;
        f.description = "1x" + std::to_string(n) + " pin header, 2.54 mm";
        for (int i = 0; i < n; ++i) {
            const double y = (i - (n - 1) / 2.0) * p;
            f.pads.push_back({std::to_string(i + 1), {0, y}, 1.7, 1.7, i == 0 ? PadShape::Rect : PadShape::Oval, 1.0});
        }
        silkRect(f, -p / 2, -n * p / 2, p / 2, n * p / 2);
    }
    finish(f);
    return f;
}

// Two rows: pads 1..n/2 down the left, the rest up the right (DIP) or down the right.
Footprint dualRow(const std::string& name, const std::string& desc, int n, double pitch, double rowSpacing, double w, double h,
                  double drill, PadShape shape, bool dip) {
    Footprint f;
    f.name = name;
    f.description = desc;
    const int half = n / 2;
    for (int i = 0; i < n; ++i) {
        const bool left = i < half;
        const int row = left ? i : (dip ? n - 1 - i : i - half);
        const double y = (row - (half - 1) / 2.0) * pitch;
        const double x = left ? -rowSpacing / 2 : rowSpacing / 2;
        f.pads.push_back({std::to_string(i + 1), {x, y}, w, h, i == 0 && drill > 0 ? PadShape::Rect : shape, drill});
    }
    return f;
}

Footprint dip(int n) {
    n = std::clamp(n + (n % 2), 4, 64);
    Footprint f = dualRow("DIP-" + std::to_string(n), "DIP-" + std::to_string(n) + ", 0.3 in rows", n, 2.54, 7.62, 1.6, 1.6, 0.8,
                          PadShape::Oval, true);
    const double ys = (n / 2) * 2.54 / 2;
    silkRect(f, -2.6, -ys, 2.6, ys);
    f.silk.push_back({{-0.6, -ys}, {0, -ys + 0.8}});
    f.silk.push_back({{0, -ys + 0.8}, {0.6, -ys}});
    finish(f);
    return f;
}

Footprint soic(int n) {
    n = std::clamp(n + (n % 2), 4, 64);
    Footprint f = dualRow("SOIC-" + std::to_string(n), "SOIC-" + std::to_string(n) + ", 1.27 mm pitch", n, 1.27, 5.4, 1.55, 0.6, 0,
                          PadShape::Rect, true);
    const double ys = (n / 2) * 1.27 / 2 + 0.2;
    silkRect(f, -1.95, -ys, 1.95, ys);
    f.silk.push_back({{-3.5, -ys - 0.4}, {-2.2, -ys - 0.4}});
    finish(f);
    return f;
}

Footprint sot23() {
    Footprint f;
    f.name = "SOT-23";
    f.description = "SOT-23, 3 pads";
    f.pads.push_back({"1", {-0.95, 1.05}, 0.6, 1.0, PadShape::Rect, 0});
    f.pads.push_back({"2", {0.95, 1.05}, 0.6, 1.0, PadShape::Rect, 0});
    f.pads.push_back({"3", {0, -1.05}, 0.6, 1.0, PadShape::Rect, 0});
    silkRect(f, -1.5, -0.55, 1.5, 0.55);
    finish(f);
    return f;
}

Footprint sot223() {
    Footprint f;
    f.name = "SOT-223";
    f.description = "SOT-223 (1, 2, 3 + tab = 2)";
    for (int i = 0; i < 3; ++i) f.pads.push_back({std::to_string(i + 1), {-2.3 + 2.3 * i, 3.15}, 0.95, 2.0, PadShape::Rect, 0});
    f.pads.push_back({"2", {0, -3.15}, 3.25, 2.0, PadShape::Rect, 0});
    silkRect(f, -3.4, -1.9, 3.4, 1.9);
    finish(f);
    return f;
}

Footprint to92() {
    Footprint f = inlinePads("TO-92", "TO-92, 3 pads in line, 2.54 mm", 3, 2.54, 1.3, 1.6, 0.75, PadShape::Oval);
    f.silk.push_back({{-2.6, 1.6}, {2.6, 1.6}});
    for (int i = 0; i < 12; ++i) {
        const double a0 = 3.14159265358979 * i / 12, a1 = 3.14159265358979 * (i + 1) / 12;
        f.silk.push_back({{-2.6 * std::cos(a0), 1.6 - 2.8 * std::sin(a0)}, {-2.6 * std::cos(a1), 1.6 - 2.8 * std::sin(a1)}});
    }
    finish(f);
    return f;
}

Footprint to220() {
    Footprint f = inlinePads("TO-220", "TO-220 vertical, 3 pads, 2.54 mm", 3, 2.54, 1.8, 2.4, 1.1, PadShape::Oval);
    silkRect(f, -5.2, -1.9, 5.2, 2.6);
    f.silk.push_back({{-5.2, -0.9}, {5.2, -0.9}});
    finish(f);
    return f;
}

Footprint module(const std::string& name, const std::string& desc, int perSide, double rowSpacing, bool dipNumbering, double bw, double bh) {
    Footprint f = dualRow(name, desc, perSide * 2, 2.54, rowSpacing, 1.7, 1.7, 1.0, PadShape::Circle, dipNumbering);
    silkRect(f, -bw / 2, -bh / 2, bw / 2, bh / 2);
    finish(f);
    return f;
}

Footprint unoShield() {
    Footprint f;
    f.name = "Arduino_Uno_Shield";
    f.description = "Arduino Uno R3 header positions (shield), board 68.6 x 53.3 mm";
    const double W = 2.7 * kInch, H = 2.1 * kInch;
    auto at = [&](double xIn, double yFromBottomIn) { return Pt{xIn * kInch - W / 2, (H - yFromBottomIn * kInch) - H / 2}; };
    int n = 1;
    // power header (NC, IOREF, RESET, 3V3, 5V, GND, GND, VIN), then A0..A5, along the bottom edge
    for (int i = 0; i < 8; ++i) f.pads.push_back({std::to_string(n++), at(1.1 + 0.1 * i, 0.1), 1.7, 1.7, i == 0 ? PadShape::Rect : PadShape::Circle, 1.0});
    for (int i = 0; i < 6; ++i) f.pads.push_back({std::to_string(n++), at(2.0 + 0.1 * i, 0.1), 1.7, 1.7, PadShape::Circle, 1.0});
    // D0..D7 right to left, D8..D13, GND, AREF, SDA, SCL along the top edge
    for (int i = 0; i < 8; ++i) f.pads.push_back({std::to_string(n++), at(2.6 - 0.1 * i, 2.0), 1.7, 1.7, PadShape::Circle, 1.0});
    for (int i = 0; i < 10; ++i) f.pads.push_back({std::to_string(n++), at(1.74 - 0.1 * i, 2.0), 1.7, 1.7, PadShape::Circle, 1.0});
    silkRect(f, -W / 2, -H / 2, W / 2, H / 2);
    finish(f);
    return f;
}

Footprint swPush() {
    Footprint f;
    f.name = "SW_PUSH_6mm";
    f.description = "6 mm tactile switch, 4 pads (1, 1, 2, 2)";
    f.pads.push_back({"1", {-3.25, -2.25}, 2.0, 2.0, PadShape::Circle, 1.1});
    f.pads.push_back({"1", {3.25, -2.25}, 2.0, 2.0, PadShape::Circle, 1.1});
    f.pads.push_back({"2", {-3.25, 2.25}, 2.0, 2.0, PadShape::Circle, 1.1});
    f.pads.push_back({"2", {3.25, 2.25}, 2.0, 2.0, PadShape::Circle, 1.1});
    silkRect(f, -3, -3, 3, 3);
    silkCircle(f, 0, 0, 1.75);
    finish(f);
    return f;
}

std::shared_ptr<const Footprint> build(const std::string& canon) {
    Footprint f;
    int n = 0;
    auto num = [&](const std::string& prefix) {
        if (canon.rfind(prefix, 0) != 0) return false;
        const std::string rest = canon.substr(prefix.size());
        if (rest.empty() || rest.size() > 3 || !std::all_of(rest.begin(), rest.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return false;
        n = std::stoi(rest);
        return n > 0 && n <= 80;
    };
    if (canon == "r_0603") f = smd2("R_0603", "0603 resistor", 1.6, 0.9, 0.95, false);
    else if (canon == "r_0805") f = smd2("R_0805", "0805 resistor", 1.9, 1.0, 1.3, false);
    else if (canon == "r_1206") f = smd2("R_1206", "1206 resistor", 3.0, 1.2, 1.75, false);
    else if (canon == "c_0603") f = smd2("C_0603", "0603 capacitor", 1.6, 0.9, 0.95, false);
    else if (canon == "c_0805") f = smd2("C_0805", "0805 capacitor", 1.9, 1.0, 1.3, false);
    else if (canon == "c_1206") f = smd2("C_1206", "1206 capacitor", 3.0, 1.2, 1.75, false);
    else if (canon == "l_0805") f = smd2("L_0805", "0805 inductor", 1.9, 1.0, 1.3, false);
    else if (canon == "l_1206") f = smd2("L_1206", "1206 inductor", 3.0, 1.2, 1.75, false);
    else if (canon == "led_0603") f = smd2("LED_0603", "0603 LED, pad 1 = cathode", 1.6, 0.9, 0.95, true);
    else if (canon == "led_0805") f = smd2("LED_0805", "0805 LED, pad 1 = cathode", 1.9, 1.0, 1.3, true);
    else if (canon == "led_1206") f = smd2("LED_1206", "1206 LED, pad 1 = cathode", 3.0, 1.2, 1.75, true);
    else if (canon == "d_sod-123") f = smd2("D_SOD-123", "SOD-123 diode, pad 1 = cathode", 3.3, 0.9, 1.2, true);
    else if (canon == "d_0805") f = smd2("D_0805", "0805 diode, pad 1 = cathode", 1.9, 1.0, 1.3, true);
    else if (canon == "r_tht") f = axial("R_THT", "axial resistor, 10.16 mm pitch", 10.16, false);
    else if (canon == "l_tht") f = axial("L_THT", "axial inductor, 10.16 mm pitch", 10.16, false);
    else if (canon == "d_tht") f = axial("D_THT", "DO-41 axial diode, 10.16 mm pitch, pad 1 = cathode", 10.16, true);
    else if (canon == "c_tht") {
        f = tht2("C_THT", "disc capacitor, 5.08 mm pitch", 5.08, 1.6, 0.8, false);
        silkRect(f, -3.6, -1.3, 3.6, 1.3);
        finish(f);
    } else if (canon == "cp_tht") {
        f = tht2("CP_THT", "radial electrolytic 5 mm, 2.54 mm pitch, pad 1 = +", 2.54, 1.6, 0.8, true);
        silkCircle(f, 0, 0, 2.6);
        f.silk.push_back({{-3.4, -1.6}, {-2.6, -1.6}});
        f.silk.push_back({{-3.0, -2.0}, {-3.0, -1.2}});
        finish(f);
    } else if (canon == "led_tht") {
        f = tht2("LED_THT", "5 mm LED, 2.54 mm pitch, pad 1 (square) = cathode", 2.54, 1.8, 0.9, true);
        silkCircle(f, 0, 0, 2.9);
        finish(f);
    } else if (canon == "hc49") {
        f = tht2("HC49", "HC-49 crystal, 4.88 mm pitch", 4.88, 1.5, 0.8, false);
        silkRect(f, -5.0, -2.3, 5.0, 2.3);
        finish(f);
    } else if (canon == "sot-23") f = sot23();
    else if (canon == "sot-223") f = sot223();
    else if (canon == "to-92") f = to92();
    else if (canon == "to-220") f = to220();
    else if (canon == "sw_push_6mm") f = swPush();
    else if (canon == "sw_spdt") {
        f = inlinePads("SW_SPDT", "slide switch SPDT, 3 pads, 2.54 mm (2 = common)", 3, 2.54, 1.6, 1.6, 0.9, PadShape::Circle);
        silkRect(f, -4.3, -1.8, 4.3, 1.8);
        finish(f);
    } else if (canon == "pot_tht") {
        f = inlinePads("Pot_THT", "trimmer potentiometer, 3 pads, 2.54 mm (2 = wiper)", 3, 2.54, 1.6, 1.6, 0.9, PadShape::Circle);
        silkRect(f, -4.8, -2.5, 4.8, 2.5);
        finish(f);
    } else if (canon == "arduino_nano") f = module("Arduino_Nano", "Arduino Nano, 2x15 pads, 15.24 mm rows", 15, 15.24, true, 18.0, 43.2);
    else if (canon == "esp32_devkit") f = module("ESP32_DevKit", "ESP32 DevKit V1, 2x15 pads, 25.4 mm rows", 15, 25.4, false, 28.0, 51.5);
    else if (canon == "bluepill") f = module("BluePill", "STM32 Blue Pill, 2x20 pads, 15.24 mm rows", 20, 15.24, false, 22.8, 53.0);
    else if (canon == "arduino_uno_shield") f = unoShield();
    else if (num("pinheader_1x")) f = pinHeader(n, false);
    else if (num("pinheader_2x")) f = pinHeader(n, true);
    else if (num("dip-")) f = dip(n);
    else if (num("soic-")) f = soic(n);
    else return nullptr;
    return std::make_shared<const Footprint>(std::move(f));
}

// "Resistor_SMD:R_0805_2012Metric" -> "r_0805"; "PinHeader_1x04_P2.54mm_Vertical" -> "pinheader_1x04".
std::string canonicalFootprint(const std::string& name) {
    std::string s = lower(name);
    if (auto colon = s.find(':'); colon != std::string::npos) s = s.substr(colon + 1);
    static const std::vector<std::string> fixed = {"r_0603", "r_0805", "r_1206", "c_0603", "c_0805", "c_1206", "l_0805", "l_1206",
                                                   "led_0603", "led_0805", "led_1206", "d_sod-123", "d_0805", "r_tht", "l_tht", "d_tht",
                                                   "c_tht", "cp_tht", "led_tht", "hc49", "sot-223", "sot-23", "to-92", "to-220",
                                                   "sw_push_6mm", "sw_spdt", "pot_tht", "arduino_nano", "esp32_devkit", "bluepill",
                                                   "arduino_uno_shield"};
    for (const auto& f : fixed) {
        if (s == f) return f;
    }
    for (const auto& f : fixed) {
        if (s.rfind(f, 0) == 0 && (s.size() == f.size() || !std::isdigit(static_cast<unsigned char>(s[f.size()])))) return f;
    }
    // patterns with a pin count, keeping only the digits after the prefix
    for (const std::string prefix : {"pinheader_1x", "pinheader_2x", "pinsocket_1x", "pinsocket_2x", "dip-", "soic-"}) {
        if (s.rfind(prefix, 0) != 0) continue;
        std::size_t i = prefix.size();
        std::string digits;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) digits += s[i++];
        if (digits.empty()) continue;
        std::string p = prefix;
        if (p.rfind("pinsocket", 0) == 0) p.replace(0, 9, "pinheader");
        return p + digits;
    }
    if (s.rfind("arduino_uno", 0) == 0) return "arduino_uno_shield";
    return s;
}

}  // namespace

const std::vector<FootprintEntry>& footprintCatalog() {
    static const std::vector<FootprintEntry> list = {
        {"R_0603 / R_0805 / R_1206", "SMD resistor"},
        {"C_0603 / C_0805 / C_1206", "SMD capacitor"},
        {"L_0805 / L_1206", "SMD inductor"},
        {"LED_0603 / LED_0805 / LED_1206", "SMD LED, pad 1 = cathode"},
        {"D_SOD-123 / D_0805", "SMD diode, pad 1 = cathode"},
        {"R_THT / L_THT", "axial, 10.16 mm pitch"},
        {"D_THT", "DO-41 axial diode, pad 1 = cathode"},
        {"C_THT", "disc capacitor, 5.08 mm pitch"},
        {"CP_THT", "radial electrolytic, 2.54 mm pitch"},
        {"LED_THT", "5 mm LED"},
        {"HC49", "HC-49 crystal"},
        {"SOT-23", "SOT-23 (3 pads)"},
        {"SOT-223", "SOT-223 regulator (tab = pad 2)"},
        {"TO-92", "TO-92, 2.54 mm in line"},
        {"TO-220", "TO-220 vertical"},
        {"SOIC-N", "SOIC with N pads, e.g. SOIC-8, SOIC-14"},
        {"DIP-N", "DIP with N pads, e.g. DIP-8, DIP-28"},
        {"PinHeader_1xNN", "1-row header, e.g. PinHeader_1x04"},
        {"PinHeader_2xNN", "2-row header, e.g. PinHeader_2x05 (10 pads)"},
        {"SW_PUSH_6mm", "6 mm tactile switch"},
        {"SW_SPDT", "slide switch, 3 pads"},
        {"Pot_THT", "trimmer potentiometer, 3 pads"},
        {"Arduino_Uno_Shield", "Arduino Uno R3 header positions"},
        {"Arduino_Nano", "Arduino Nano, 2x15"},
        {"ESP32_DevKit", "ESP32 DevKit V1, 2x15, 25.4 mm rows"},
        {"BluePill", "STM32 Blue Pill, 2x20"},
    };
    return list;
}

std::shared_ptr<const Footprint> findFootprint(const std::string& name) {
    if (name.empty()) return nullptr;
    const std::string canon = canonicalFootprint(name);
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const Footprint>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = cache.find(canon); it != cache.end()) return it->second;
    auto f = build(canon);
    cache[canon] = f;
    return f;
}

}  // namespace achclwss::circuit
