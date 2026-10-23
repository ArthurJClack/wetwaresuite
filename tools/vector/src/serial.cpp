// Short text forms of appearance items, effects, patterns, freeform gradient
// points and text styles. Commands take them as params, vector.info prints
// them, and SVG files keep them in data-* attributes so they open again exactly.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>

#include "internal.hpp"

namespace wetwaresuite::vector::detail {

namespace {

std::vector<std::string> tokens(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string t;
    while (in >> t) out.push_back(t);
    return out;
}

bool number(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == 0;
}

std::string lowerCopy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string colorText(const Color& c) {
    std::string hex = c.hex();
    return c.a == 255 ? hex.substr(0, 7) : hex;
}

std::vector<std::string> splitOn(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : text) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c == sep && depth <= 0) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    for (auto& s : out) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    }
    out.erase(std::remove(out.begin(), out.end(), std::string{}), out.end());
    return out;
}

bool readStrokeOption(StrokeStyle& st, const std::string& key, const std::string& value, std::string& error) {
    const std::string v = lowerCopy(value);
    double x = 0;
    if (key == "cap") {
        if (v != "butt" && v != "round" && v != "square") return error = "cap must be butt, round or square", false;
        st.cap = v;
    } else if (key == "join") {
        if (v != "miter" && v != "round" && v != "bevel") return error = "join must be miter, round or bevel", false;
        st.join = v;
    } else if (key == "miter_limit") {
        if (!number(value, x) || x < 1) return error = "miter_limit must be 1 or more", false;
        st.miterLimit = x;
    } else if (key == "dash") {
        st.dash.clear();
        if (v != "none" && !v.empty()) {
            for (const auto& part : splitOn(value, ',')) {
                if (!number(part, x) || x < 0) return error = "dash is lengths like 6,3 (on, off, ...) or none", false;
                st.dash.push_back(x);
            }
            double sum = 0;
            for (double d : st.dash) sum += d;
            if (sum <= 0) st.dash.clear();
        }
    } else if (key == "dash_offset") {
        if (!number(value, x)) return error = "dash_offset must be a number", false;
        st.dashOffset = x;
    } else if (key == "align") {
        if (v != "center" && v != "inside" && v != "outside") return error = "align must be center, inside or outside", false;
        st.align = v;
    } else if (key == "start_arrow" || key == "end_arrow") {
        if (v != "none" && v != "triangle" && v != "open" && v != "circle" && v != "square" && v != "bar") {
            return error = key + " must be none, triangle, open, circle, square or bar", false;
        }
        (key == "start_arrow" ? st.startArrow : st.endArrow) = v;
    } else if (key == "arrow_scale") {
        if (!number(value, x) || x <= 0) return error = "arrow_scale must be above 0", false;
        st.arrowScale = x;
    } else if (key == "profile") {
        if (v != "uniform" && v != "taper" && v != "taper_start" && v != "taper_end" && v != "bulge" && v != "pinch") {
            return error = "profile must be uniform, taper, taper_start, taper_end, bulge or pinch", false;
        }
        st.profile = v;
    } else {
        return error = "unknown stroke option '" + key + "'", false;
    }
    return true;
}

std::string strokeOptionsText(const StrokeStyle& st) {
    std::string out;
    const StrokeStyle def;
    if (st.cap != def.cap) out += " cap=" + st.cap;
    if (st.join != def.join) out += " join=" + st.join;
    if (st.miterLimit != def.miterLimit) out += " miter_limit=" + num(st.miterLimit);
    if (!st.dash.empty()) {
        out += " dash=";
        for (std::size_t i = 0; i < st.dash.size(); ++i) out += (i ? "," : "") + num(st.dash[i]);
    }
    if (st.dashOffset != 0) out += " dash_offset=" + num(st.dashOffset);
    if (st.align != def.align) out += " align=" + st.align;
    if (st.startArrow != "none") out += " start_arrow=" + st.startArrow;
    if (st.endArrow != "none") out += " end_arrow=" + st.endArrow;
    if (st.arrowScale != 1) out += " arrow_scale=" + num(st.arrowScale);
    if (st.profile != def.profile) out += " profile=" + st.profile;
    return out.empty() ? out : out.substr(1);
}

std::string appearanceText(const std::vector<AppearanceItem>& items) {
    std::string out;
    for (const auto& a : items) {
        if (!out.empty()) out += "; ";
        out += a.stroke ? "stroke" : "fill";
        out += " color=" + colorText(a.color);
        if (a.stroke) out += " width=" + num(a.width);
        if (a.opacity != 1) out += " opacity=" + num(a.opacity);
        if (a.blend != "normal") out += " blend=" + a.blend;
        if (a.under) out += " under";
        if (a.dx != 0) out += " dx=" + num(a.dx);
        if (a.dy != 0) out += " dy=" + num(a.dy);
        if (a.stroke) {
            const std::string opts = strokeOptionsText(a.style);
            if (!opts.empty()) out += " " + opts;
        }
    }
    return out;
}

bool parseAppearanceItem(const std::string& text, AppearanceItem& a, std::string& error) {
    const auto t = tokens(text);
    if (t.empty() || (lowerCopy(t[0]) != "fill" && lowerCopy(t[0]) != "stroke")) {
        return error = "an appearance item starts with fill or stroke, e.g. \"stroke color=red width=6 under\"", false;
    }
    a = {};
    a.stroke = lowerCopy(t[0]) == "stroke";
    a.style.cap = a.style.join = "round";
    for (std::size_t i = 1; i < t.size(); ++i) {
        const std::string tok = t[i];
        if (lowerCopy(tok) == "under") {
            a.under = true;
            continue;
        }
        const auto eq = tok.find('=');
        if (eq == std::string::npos) return error = "'" + tok + "' should be key=value", false;
        const std::string key = lowerCopy(tok.substr(0, eq)), value = tok.substr(eq + 1);
        double x = 0;
        if (key == "color" || key == "colour") {
            auto c = parseColor(value);
            if (!c) return error = "'" + value + "' isn't a colour", false;
            a.color = *c;
        } else if (key == "width") {
            if (!number(value, x) || x < 0) return error = "width must be a number of px", false;
            a.width = x;
        } else if (key == "opacity") {
            if (!number(value, x)) return error = "opacity must be 0..1", false;
            a.opacity = std::clamp(x, 0.0, 1.0);
        } else if (key == "blend") {
            if (!raster::parseBlend(value)) return error = "'" + value + "' isn't a blend mode (multiply, screen, overlay, ...)", false;
            a.blend = raster::toString(*raster::parseBlend(value));
        } else if (key == "dx" || key == "dy") {
            if (!number(value, x)) return error = key + " must be a number", false;
            (key == "dx" ? a.dx : a.dy) = x;
        } else if (!readStrokeOption(a.style, key, value, error)) {
            return false;
        }
    }
    return true;
}

bool parseAppearance(const std::string& text, std::vector<AppearanceItem>& out, std::string& error) {
    out.clear();
    for (const auto& part : splitOn(text, ';')) {
        AppearanceItem a;
        if (!parseAppearanceItem(part, a, error)) return false;
        out.push_back(a);
    }
    return true;
}

std::string effectsText(const std::vector<Effect>& effects) {
    std::string out;
    for (const auto& e : effects) {
        if (!out.empty()) out += "; ";
        out += e.type + " color=" + colorText(e.color) + " opacity=" + num(e.opacity) + " radius=" + num(e.radius);
        if (e.type == "drop_shadow" || e.type == "inner_shadow") out += " dx=" + num(e.dx) + " dy=" + num(e.dy);
    }
    return out;
}

bool parseEffect(const std::string& text, Effect& e, std::string& error) {
    const auto t = tokens(text);
    static const char* types[] = {"drop_shadow", "inner_shadow", "outer_glow", "inner_glow", "blur"};
    if (t.empty() || std::find_if(std::begin(types), std::end(types), [&](const char* n) { return lowerCopy(t[0]) == n; }) == std::end(types)) {
        return error = "an effect is drop_shadow, inner_shadow, outer_glow, inner_glow or blur", false;
    }
    e = {};
    e.type = lowerCopy(t[0]);
    if (e.type == "outer_glow" || e.type == "inner_glow") {
        e.color = {255, 255, 190, 255};
        e.radius = 8;
        e.dx = e.dy = 0;
    }
    if (e.type == "blur") e.opacity = 1;
    for (std::size_t i = 1; i < t.size(); ++i) {
        const auto eq = t[i].find('=');
        if (eq == std::string::npos) return error = "'" + t[i] + "' should be key=value", false;
        const std::string key = lowerCopy(t[i].substr(0, eq)), value = t[i].substr(eq + 1);
        double x = 0;
        if (key == "color" || key == "colour") {
            auto c = parseColor(value);
            if (!c) return error = "'" + value + "' isn't a colour", false;
            e.color = *c;
        } else if (number(value, x)) {
            if (key == "opacity") e.opacity = std::clamp(x, 0.0, 1.0);
            else if (key == "radius" || key == "size" || key == "blur") e.radius = std::clamp(x, 0.0, 500.0);
            else if (key == "dx") e.dx = x;
            else if (key == "dy") e.dy = x;
            else return error = "unknown effect setting '" + key + "'", false;
        } else {
            return error = "'" + value + "' isn't a number", false;
        }
    }
    return true;
}

bool parseEffects(const std::string& text, std::vector<Effect>& out, std::string& error) {
    out.clear();
    for (const auto& part : splitOn(text, ';')) {
        Effect e;
        if (!parseEffect(part, e, error)) return false;
        out.push_back(e);
    }
    return true;
}

std::string patternText(const Pattern& p) {
    return p.type + " color=" + colorText(p.color) + " background=" + (p.background ? colorText(*p.background) : std::string("none")) +
           " size=" + num(p.size) + " angle=" + num(p.angle) + " weight=" + num(p.weight);
}

bool parsePattern(const std::string& text, Pattern& p, std::string& error) {
    const auto t = tokens(text);
    if (t.empty()) return error = "empty pattern", false;
    p = {};
    p.type = lowerCopy(t[0]);
    if (p.type != "stripes" && p.type != "dots" && p.type != "grid" && p.type != "checker" && p.type != "hatch") {
        return error = "pattern type must be stripes, dots, grid, checker or hatch", false;
    }
    for (std::size_t i = 1; i < t.size(); ++i) {
        const auto eq = t[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lowerCopy(t[i].substr(0, eq)), value = t[i].substr(eq + 1);
        double x = 0;
        if (key == "color") {
            if (auto c = parseColor(value)) p.color = *c;
        } else if (key == "background") {
            if (lowerCopy(value) == "none") p.background.reset();
            else if (auto c = parseColor(value)) p.background = *c;
        } else if (number(value, x)) {
            if (key == "size") p.size = std::max(0.5, x);
            else if (key == "angle") p.angle = x;
            else if (key == "weight") p.weight = std::clamp(x, 0.02, 0.98);
        }
    }
    return true;
}

std::string freeformText(const std::vector<ColorPoint>& pts) {
    std::string out;
    for (const auto& p : pts) out += (out.empty() ? "" : " ") + num(p.x) + "," + num(p.y) + "," + colorText(p.color);
    return out;
}

bool parseFreeform(const std::string& text, std::vector<ColorPoint>& out, std::string& error) {
    out.clear();
    for (const auto& tok : tokens(text)) {
        const auto parts = splitOn(tok, ',');
        double x = 0, y = 0;
        if (parts.size() != 3 || !number(parts[0], x) || !number(parts[1], y)) {
            return error = "freeform points are x,y,colour (fractions of the shape's box), e.g. 0.2,0.3,red 0.8,0.7,blue", false;
        }
        auto c = parseColor(parts[2]);
        if (!c) return error = "'" + parts[2] + "' isn't a colour", false;
        out.push_back({x, y, *c});
    }
    if (out.empty()) return error = "give at least one point", false;
    return true;
}

std::string textStyleText(const TextStyle& t) {
    std::string out = "name=" + t.name + " font=" + t.font + " size=" + num(t.size);
    if (t.bold) out += " bold=1";
    if (t.italic) out += " italic=1";
    if (t.letterSpacing != 0) out += " letter_spacing=" + num(t.letterSpacing);
    if (t.lineHeight != 1.2) out += " line_height=" + num(t.lineHeight);
    if (t.align != "left") out += " align=" + t.align;
    if (t.textCase != "none") out += " case=" + t.textCase;
    if (t.fill) out += " fill=" + colorText(*t.fill);
    return out;
}

bool parseTextStyle(const std::string& text, TextStyle& t) {
    t = {};
    for (const auto& tok : tokens(text)) {
        const auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = tok.substr(0, eq), value = tok.substr(eq + 1);
        double x = 0;
        if (key == "name") t.name = value;
        else if (key == "font") t.font = value;
        else if (key == "size" && number(value, x)) t.size = x;
        else if (key == "bold") t.bold = value == "1" || value == "true";
        else if (key == "italic") t.italic = value == "1" || value == "true";
        else if (key == "letter_spacing" && number(value, x)) t.letterSpacing = x;
        else if (key == "line_height" && number(value, x)) t.lineHeight = x;
        else if (key == "align") t.align = value;
        else if (key == "case") t.textCase = value;
        else if (key == "fill") t.fill = parseColor(value);
    }
    return !t.name.empty();
}

bool validName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

void applyTextStyle(Shape& s, const TextStyle& t) {
    s.font = t.font;
    s.fontSize = t.size;
    s.bold = t.bold;
    s.italic = t.italic;
    s.letterSpacing = t.letterSpacing;
    s.lineHeight = t.lineHeight;
    s.align = t.align;
    s.textCase = t.textCase;
    if (t.fill) s.fill = t.fill;
    s.textStyle = t.name;
}

}  // namespace wetwaresuite::vector::detail
