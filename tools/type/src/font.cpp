// Font-level operations: new fonts, scaling, spacing, glyph removal and
// renaming, vertical metrics from glyph shapes, and auto-kerning.
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "internal.hpp"

namespace wetwaresuite::typeface {

Font newFont(const std::string& family, const std::string& style, int upm, double ascender, double descender, double xHeight,
             double capHeight) {
    Font f;
    f.info.family = family;
    f.info.style = style;
    f.metrics.unitsPerEm = upm;
    f.metrics.ascender = ascender;
    f.metrics.descender = descender;
    f.metrics.xHeight = xHeight;
    f.metrics.capHeight = capHeight;
    f.metrics.underlinePosition = -upm * 0.1;
    f.metrics.underlineThickness = upm * 0.05;
    Glyph notdef;
    notdef.name = ".notdef";
    notdef.advance = std::round(upm * 0.5);
    const double w = upm * 0.05, x0 = upm * 0.05, x1 = notdef.advance - upm * 0.05, top = capHeight;
    notdef.contours.push_back(rectContour(x0, 0, x1 - x0, top));
    Contour hole = rectContour(x0 + w, w, x1 - x0 - 2 * w, top - 2 * w);
    reverse(hole);
    notdef.contours.push_back(hole);
    f.glyphs.push_back(notdef);
    Glyph space;
    space.name = "space";
    space.unicodes = {0x20};
    space.advance = std::round(upm * 0.25);
    f.glyphs.push_back(space);
    return f;
}

void scaleFont(Font& font, double k) {
    auto& m = font.metrics;
    m.unitsPerEm = std::max(16, toInt(m.unitsPerEm * k));
    for (double* v : {&m.ascender, &m.descender, &m.xHeight, &m.capHeight, &m.lineGap, &m.underlinePosition, &m.underlineThickness})
        *v *= k;
    for (auto& g : font.glyphs) {
        g.advance *= k;
        transform(g.contours, Affine::scale(k, k));
        for (auto& c : g.components) {
            c.dx *= k;
            c.dy *= k;
        }
    }
    for (auto& [pair, v] : font.kerning) v *= k;
}

double leftBearing(const Font& font, const Glyph& glyph) {
    Box b = glyphBounds(font, glyph);
    return b.empty ? 0 : b.x0;
}

double rightBearing(const Font& font, const Glyph& glyph) {
    Box b = glyphBounds(font, glyph);
    return b.empty ? glyph.advance : glyph.advance - b.x1;
}

void setSidebearings(const Font& font, Glyph& glyph, std::optional<double> lsb, std::optional<double> rsb) {
    Box b = glyphBounds(font, glyph);
    if (b.empty) return;
    double shift = 0;
    if (lsb) {
        shift = std::round(*lsb - b.x0);  // whole units keep integer outlines integer
        transform(glyph.contours, Affine::translate(shift, 0));
        for (auto& c : glyph.components) c.dx += shift;
    }
    const double right = b.x1 + shift;
    if (rsb) glyph.advance = std::max(0.0, std::round(right + *rsb));
    else glyph.advance = std::max(0.0, glyph.advance + shift);
}

void decompose(const Font& font, Glyph& glyph) {
    if (glyph.components.empty()) return;
    glyph.contours = outline(font, glyph);
    glyph.components.clear();
}

void removeGlyph(Font& font, int index) {
    const std::string name = font.glyphs[static_cast<std::size_t>(index)].name;
    for (auto& g : font.glyphs) {
        bool uses = false;
        for (const auto& c : g.components) uses |= c.base == name;
        if (uses && g.name != name) decompose(font, g);
    }
    font.glyphs.erase(font.glyphs.begin() + index);
    for (auto it = font.kerning.begin(); it != font.kerning.end();) {
        if (it->first.first == name || it->first.second == name) it = font.kerning.erase(it);
        else ++it;
    }
}

void removeGlyphs(Font& font, const std::vector<int>& indexes) {
    std::vector<char> gone(font.glyphs.size(), 0);
    std::set<std::string> names;
    for (int i : indexes) {
        if (i < 0 || static_cast<std::size_t>(i) >= font.glyphs.size()) continue;
        gone[static_cast<std::size_t>(i)] = 1;
        names.insert(font.glyphs[static_cast<std::size_t>(i)].name);
    }
    if (names.empty()) return;
    // draw in first, all against the untouched font, so nested composites resolve
    std::vector<std::pair<std::size_t, std::vector<Contour>>> drawn;
    for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
        if (gone[i]) continue;
        bool uses = false;
        for (const auto& c : font.glyphs[i].components) uses |= names.count(c.base) > 0;
        if (uses) drawn.emplace_back(i, outline(font, font.glyphs[i]));
    }
    for (auto& [i, contours] : drawn) {
        font.glyphs[i].contours = std::move(contours);
        font.glyphs[i].components.clear();
    }
    std::vector<Glyph> kept;
    kept.reserve(font.glyphs.size() - names.size());
    for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
        if (!gone[i]) kept.push_back(std::move(font.glyphs[i]));
    }
    font.glyphs = std::move(kept);
    for (auto it = font.kerning.begin(); it != font.kerning.end();) {
        if (names.count(it->first.first) || names.count(it->first.second)) it = font.kerning.erase(it);
        else ++it;
    }
}

void renameGlyph(Font& font, int index, const std::string& name) {
    const std::string old = font.glyphs[static_cast<std::size_t>(index)].name;
    if (old == name) return;
    font.glyphs[static_cast<std::size_t>(index)].name = name;
    for (auto& g : font.glyphs) {
        for (auto& c : g.components) {
            if (c.base == old) c.base = name;
        }
    }
    std::map<KernPair, double> kern;
    for (const auto& [pair, v] : font.kerning) {
        KernPair p = pair;
        if (p.first == old) p.first = name;
        if (p.second == old) p.second = name;
        kern[p] = v;
    }
    font.kerning = std::move(kern);
}

std::vector<std::string> detectVerticalMetrics(Font& font) {
    std::vector<std::string> changed;
    auto top = [&](std::initializer_list<const char*> names, bool high, double& out) {
        for (const char* n : names) {
            const Glyph* g = font.glyph(n);
            if (!g) continue;
            Box b = glyphBounds(font, *g);
            if (b.empty) continue;
            out = std::round(high ? b.y1 : b.y0);
            return true;
        }
        return false;
    };
    auto& m = font.metrics;
    double v = 0;
    if (top({"x", "z", "v", "w", "u"}, true, v)) {
        m.xHeight = v;
        changed.push_back("x_height=" + fmt(v));
    }
    if (top({"H", "I", "E", "T", "L"}, true, v)) {
        m.capHeight = v;
        changed.push_back("cap_height=" + fmt(v));
    }
    double asc = 0;
    if (top({"d", "l", "h", "b", "k"}, true, asc)) {
        m.ascender = std::max(asc, m.capHeight);
        changed.push_back("ascender=" + fmt(m.ascender));
    }
    if (top({"p", "q", "g", "y", "j"}, false, v) && v < 0) {
        m.descender = v;
        changed.push_back("descender=" + fmt(v));
    }
    return changed;
}

// ---- auto-kerning ----

namespace {

constexpr double kNone = std::numeric_limits<double>::quiet_NaN();

struct Profile {
    std::vector<double> left, right;  // ink extent per band, NaN where the band is empty
    double advance = 0;
};

Profile profile(const Font& font, const Glyph& g, double y0, double y1, int bands) {
    Profile p;
    p.advance = g.advance;
    p.left.assign(static_cast<std::size_t>(bands), kNone);
    p.right.assign(static_cast<std::size_t>(bands), kNone);
    const auto polys = flatten(outline(font, g), font.metrics.unitsPerEm / 500.0);
    const int sub = 3;  // scanlines per band
    for (int b = 0; b < bands; ++b) {
        for (int s = 0; s < sub; ++s) {
            const double y = y0 + (y1 - y0) * (b + (s + 0.5) / sub) / bands;
            for (const auto& poly : polys) {
                for (std::size_t i = 0, n = poly.size(); i < n; ++i) {
                    const Vec a = poly[i], c = poly[(i + 1) % n];
                    if ((a.y > y) == (c.y > y)) continue;
                    const double x = a.x + (y - a.y) / (c.y - a.y) * (c.x - a.x);
                    auto& l = p.left[static_cast<std::size_t>(b)];
                    auto& r = p.right[static_cast<std::size_t>(b)];
                    l = std::isnan(l) ? x : std::min(l, x);
                    r = std::isnan(r) ? x : std::max(r, x);
                }
            }
        }
    }
    return p;
}

// How far apart the two shapes look: half the closest approach, half the average gap
// (gaps capped so open shapes don't count for too much). NaN when they share no band.
double gap(const Profile& a, const Profile& b, double cap, double* closest) {
    double minGap = std::numeric_limits<double>::infinity(), sum = 0;
    int n = 0;
    for (std::size_t i = 0; i < a.right.size(); ++i) {
        if (std::isnan(a.right[i]) || std::isnan(b.left[i])) continue;
        const double g = (a.advance - a.right[i]) + b.left[i];
        minGap = std::min(minGap, g);
        sum += std::min(g, cap);
        ++n;
    }
    if (n == 0) return kNone;
    if (closest) *closest = minGap;
    return 0.5 * std::min(minGap, cap) + 0.5 * (sum / n);
}

}  // namespace

std::map<KernPair, double> autoKern(const Font& font, const std::vector<std::string>& names, const AutoKernOptions& o,
                                    double* measuredTarget) {
    const double em = font.metrics.unitsPerEm;
    const int bands = std::clamp(o.bands, 4, 200);
    const double y0 = std::min(font.metrics.descender, 0.0), y1 = std::max(font.metrics.ascender, font.metrics.capHeight);
    std::map<std::string, Profile> profiles;
    for (const auto& n : names) {
        if (const Glyph* g = font.glyph(n)) profiles[g->name] = profile(font, *g, y0, y1, bands);
    }
    const double cap = em * 0.6;
    double target = o.target;
    if (target <= 0) {
        for (const char* ref : {"H", "n", "I", "o"}) {
            const Glyph* g = font.glyph(ref);
            if (!g) continue;
            const Profile p = profile(font, *g, y0, y1, bands);
            const double v = gap(p, p, cap, nullptr);
            if (!std::isnan(v)) {
                target = v;
                break;
            }
        }
    }
    if (target <= 0) {
        // average sidebearings of the glyphs being kerned
        double sum = 0;
        int n = 0;
        for (const auto& [name, p] : profiles) {
            const Glyph* g = font.glyph(name);
            Box b = glyphBounds(font, *g);
            if (b.empty) continue;
            sum += b.x0 + (g->advance - b.x1);
            ++n;
        }
        target = n ? sum / n : em * 0.1;
    }
    if (measuredTarget) *measuredTarget = target;
    const double minimum = o.minimum > 0 ? o.minimum : em * 0.015;
    const double maximum = o.maximum > 0 ? o.maximum : em * 0.3;
    std::map<KernPair, double> out;
    for (const auto& [ln, lp] : profiles) {
        for (const auto& [rn, rp] : profiles) {
            double closest = 0;
            const double g = gap(lp, rp, cap, &closest);
            if (std::isnan(g)) continue;
            double k = (target - g) * o.strength;
            // never let the shapes get closer than a third of the target
            k = std::max(k, target / 3 - closest);
            k = std::clamp(k, -maximum, maximum);
            k = std::round(k);
            if (std::abs(k) >= minimum) out[{ln, rn}] = k;
        }
    }
    return out;
}

}  // namespace wetwaresuite::typeface
