// The drawing model: units, paper, layers, colours, geometry helpers,
// formatting of measured values, and the transforms edits use.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "achclwss/blueprint/blueprint.hpp"
#include "internal.hpp"

namespace achclwss::blueprint {

namespace {

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", std::clamp(decimals, 0, 8), v);
    std::string s = buf;
    if (s.rfind("-", 0) == 0 && s.find_first_not_of("-0.") == std::string::npos) s.erase(0, 1);  // no "-0"
    return s;
}

std::string trimmed(double v, int decimals) {
    std::string s = fixed(v, decimals);
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

}  // namespace

// ---- geometry -------------------------------------------------------------

double normalizeAngle(double deg) {
    double a = std::fmod(deg, 360.0);
    if (a < 0) a += 360.0;
    if (a >= 360.0 - 1e-12) a = 0;
    return a;
}

double polygonArea(const std::vector<Vec2>& pts) {
    double s = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) s += cross(pts[i], pts[(i + 1) % pts.size()]);
    return s / 2;
}

double polylineLength(const std::vector<Vec2>& pts, bool closed) {
    double s = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) s += distance(pts[i - 1], pts[i]);
    if (closed && pts.size() > 2) s += distance(pts.back(), pts.front());
    return s;
}

Vec2 polygonCentroid(const std::vector<Vec2>& pts) {
    if (pts.empty()) return {};
    double a = 0, cx = 0, cy = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        Vec2 p = pts[i], q = pts[(i + 1) % pts.size()];
        double w = cross(p, q);
        a += w;
        cx += (p.x + q.x) * w;
        cy += (p.y + q.y) * w;
    }
    if (std::abs(a) < 1e-12) {
        Vec2 s;
        for (auto p : pts) s = s + p;
        return s / static_cast<double>(pts.size());
    }
    return {cx / (3 * a), cy / (3 * a)};
}

namespace {

std::vector<Vec2> dedupe(const std::vector<Vec2>& pts, bool closed) {
    std::vector<Vec2> out;
    for (auto p : pts) {
        if (out.empty() || distance(out.back(), p) > 1e-9) out.push_back(p);
    }
    if (closed && out.size() > 2 && distance(out.front(), out.back()) <= 1e-9) out.pop_back();
    return out;
}

// Where the offset lines of the segments before and after a corner meet.
Vec2 miter(Vec2 prev, Vec2 p, Vec2 next, double d) {
    Vec2 n1 = perp(normalized(p - prev)), n2 = perp(normalized(next - p));
    Vec2 sum = n1 + n2;
    if (length(sum) < 1e-9) return p + n1 * d;  // the line doubles back on itself
    Vec2 m = normalized(sum);
    double cosHalf = std::max(dot(m, n1), 0.2);  // limit very sharp spikes
    return p + m * (d / cosHalf);
}

}  // namespace

std::vector<Vec2> offsetPolyline(const std::vector<Vec2>& input, bool closed, double d) {
    std::vector<Vec2> pts = dedupe(input, closed);
    const std::size_t n = pts.size();
    std::vector<Vec2> out;
    if (n < 2) return pts;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (closed) {
            out.push_back(miter(pts[(i + n - 1) % n], pts[i], pts[(i + 1) % n], d));
        } else if (i == 0) {
            out.push_back(pts[0] + perp(normalized(pts[1] - pts[0])) * d);
        } else if (i == n - 1) {
            out.push_back(pts[i] + perp(normalized(pts[i] - pts[i - 1])) * d);
        } else {
            out.push_back(miter(pts[i - 1], pts[i], pts[i + 1], d));
        }
    }
    return out;
}

void Box::add(Vec2 p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return;
    if (empty) {
        x0 = x1 = p.x;
        y0 = y1 = p.y;
        empty = false;
        return;
    }
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
}

void Box::add(const Box& b) {
    if (b.empty) return;
    add(Vec2{b.x0, b.y0});
    add(Vec2{b.x1, b.y1});
}

// ---- units, paper, layers -------------------------------------------------

const char* toString(Unit unit) {
    switch (unit) {
        case Unit::Mm: return "mm";
        case Unit::Cm: return "cm";
        case Unit::M: return "m";
        case Unit::In: return "in";
        case Unit::Ft: return "ft";
    }
    return "mm";
}

std::optional<Unit> parseUnit(const std::string& text) {
    const std::string t = lower(text);
    if (t == "mm" || t == "millimeters" || t == "millimetres") return Unit::Mm;
    if (t == "cm" || t == "centimeters" || t == "centimetres") return Unit::Cm;
    if (t == "m" || t == "meters" || t == "metres") return Unit::M;
    if (t == "in" || t == "inch" || t == "inches") return Unit::In;
    if (t == "ft" || t == "feet" || t == "foot") return Unit::Ft;
    return std::nullopt;
}

double unitToMm(Unit unit) {
    switch (unit) {
        case Unit::Mm: return 1;
        case Unit::Cm: return 10;
        case Unit::M: return 1000;
        case Unit::In: return 25.4;
        case Unit::Ft: return 304.8;
    }
    return 1;
}

bool metric(Unit unit) { return unit == Unit::Mm || unit == Unit::Cm || unit == Unit::M; }

const std::vector<PaperSize>& paperSizes() {
    static const std::vector<PaperSize> sizes = {
        {"A4", 210, 297},          {"A3", 297, 420},          {"A2", 420, 594},           {"A1", 594, 841}, {"A0", 841, 1189},
        {"letter", 215.9, 279.4}, {"legal", 215.9, 355.6}, {"tabloid", 279.4, 431.8},
    };
    return sizes;
}

std::optional<PaperSize> findPaper(const std::string& name) {
    const std::string t = lower(name);
    for (const auto& p : paperSizes()) {
        if (lower(p.name) == t) return p;
    }
    if (t == "ledger") return findPaper("tabloid");
    return std::nullopt;
}

const char* toString(LineType type) {
    switch (type) {
        case LineType::Continuous: return "continuous";
        case LineType::Dashed: return "dashed";
        case LineType::Center: return "center";
        case LineType::Hidden: return "hidden";
    }
    return "continuous";
}

std::optional<LineType> parseLineType(const std::string& text) {
    const std::string t = lower(text);
    if (t == "continuous" || t == "solid" || t == "bylayer" || t.empty()) return LineType::Continuous;
    if (t.find("dash") != std::string::npos && t.find("dot") == std::string::npos) return LineType::Dashed;
    if (t.find("center") != std::string::npos || t.find("centre") != std::string::npos || t.find("dashdot") != std::string::npos ||
        t.find("chain") != std::string::npos)
        return LineType::Center;
    if (t.find("hidden") != std::string::npos) return LineType::Hidden;
    return std::nullopt;
}

std::vector<double> dashPattern(LineType type) {
    switch (type) {
        case LineType::Continuous: return {};
        case LineType::Dashed: return {6, 3};
        case LineType::Center: return {12, 3, 2, 3};
        case LineType::Hidden: return {3, 1.5};
    }
    return {};
}

std::optional<Color> parseColor(const std::string& text) {
    std::string t = lower(text);
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    static const std::map<std::string, Color> extra = {
        {"darkgray", {64, 64, 64, 255}},  {"darkgrey", {64, 64, 64, 255}},   {"lightgray", {192, 192, 192, 255}},
        {"lightgrey", {192, 192, 192, 255}}, {"silver", {192, 192, 192, 255}}, {"navy", {0, 0, 128, 255}},
        {"teal", {0, 128, 128, 255}},     {"maroon", {128, 0, 0, 255}},      {"olive", {128, 128, 0, 255}},
        {"darkblue", {0, 0, 139, 255}},   {"darkred", {139, 0, 0, 255}},     {"darkgreen", {0, 100, 0, 255}},
    };
    if (auto it = extra.find(t); it != extra.end()) return it->second;
    if (!t.empty() && std::all_of(t.begin(), t.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
        int aci = std::atoi(t.c_str());
        if (aci >= 1 && aci <= 255) return aciColor(aci);
        return std::nullopt;
    }
    if (t.rfind("aci", 0) == 0) return parseColor(t.substr(t.find_first_of("0123456789") == std::string::npos ? t.size() : t.find_first_of("0123456789")));
    return Color::parse(text);
}

Color aciColor(int index) {
    static const Color basic[10] = {{0, 0, 0, 255},     {255, 0, 0, 255},     {255, 255, 0, 255}, {0, 255, 0, 255},     {0, 255, 255, 255},
                                    {0, 0, 255, 255},   {255, 0, 255, 255},   {0, 0, 0, 255},     {128, 128, 128, 255}, {192, 192, 192, 255}};
    if (index >= 0 && index <= 9) return basic[index];
    if (index >= 250 && index <= 255) {
        static const int grays[6] = {51, 80, 105, 130, 190, 255};
        auto g = static_cast<std::uint8_t>(grays[index - 250]);
        return {g, g, g, 255};
    }
    if (index < 10 || index > 249) return basic[7];
    const int hueStep = (index - 10) / 10, shade = index % 10;
    const double hue = hueStep * 15.0;
    auto bump = [](double h) {  // 1 near the channel's own hue, fading out 60..120 degrees away
        h = std::fmod(h + 720.0, 360.0);
        if (h <= 60 || h >= 300) return 1.0;
        if (h < 120) return (120 - h) / 60;
        if (h > 240) return (h - 240) / 60;
        return 0.0;
    };
    double r = bump(hue), g = bump(hue - 120), b = bump(hue - 240);
    if (shade % 2 == 1) {  // odd entries are the pale version
        r = (r + 1) / 2;
        g = (g + 1) / 2;
        b = (b + 1) / 2;
    }
    static const double value[5] = {1.0, 0.8, 0.6, 0.5, 0.3};
    const double v = value[shade / 2];
    auto c8 = [&](double x) { return static_cast<std::uint8_t>(std::lround(std::clamp(x * v, 0.0, 1.0) * 255)); };
    return {c8(r), c8(g), c8(b), 255};
}

int nearestAci(Color c) {
    if ((c.r == 0 && c.g == 0 && c.b == 0) || (c.r == 255 && c.g == 255 && c.b == 255)) return 7;
    int best = 7;
    long bestD = -1;
    for (int i = 1; i <= 255; ++i) {
        if (i == 7) continue;
        Color a = aciColor(i);
        long dr = a.r - c.r, dg = a.g - c.g, db = a.b - c.b;
        long d = dr * dr + dg * dg + db * db;
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// ---- entity names ---------------------------------------------------------

const char* toString(EntityType type) {
    switch (type) {
        case EntityType::Line: return "line";
        case EntityType::Polyline: return "polyline";
        case EntityType::Circle: return "circle";
        case EntityType::Arc: return "arc";
        case EntityType::Text: return "text";
        case EntityType::Dimension: return "dimension";
        case EntityType::Hatch: return "hatch";
        case EntityType::Point: return "point";
        case EntityType::Wall: return "wall";
        case EntityType::Door: return "door";
        case EntityType::Window: return "window";
        case EntityType::Room: return "room";
    }
    return "line";
}

std::optional<EntityType> parseEntityType(const std::string& text) {
    const std::string t = lower(text);
    for (int i = 0; i <= static_cast<int>(EntityType::Room); ++i) {
        auto type = static_cast<EntityType>(i);
        if (t == toString(type) || t == std::string(toString(type)) + "s") return type;
    }
    if (t == "rect" || t == "rectangle" || t == "lwpolyline") return EntityType::Polyline;
    if (t == "dim" || t == "dims") return EntityType::Dimension;
    return std::nullopt;
}

const char* toString(DimKind kind) {
    switch (kind) {
        case DimKind::Aligned: return "aligned";
        case DimKind::Horizontal: return "horizontal";
        case DimKind::Vertical: return "vertical";
        case DimKind::Radius: return "radius";
        case DimKind::Diameter: return "diameter";
        case DimKind::Angular: return "angular";
    }
    return "aligned";
}

std::optional<DimKind> parseDimKind(const std::string& text) {
    const std::string t = lower(text);
    for (int i = 0; i <= static_cast<int>(DimKind::Angular); ++i) {
        auto kind = static_cast<DimKind>(i);
        if (t == toString(kind)) return kind;
    }
    if (t == "linear") return DimKind::Aligned;
    return std::nullopt;
}

// ---- document -------------------------------------------------------------

double Drawing::paperWidth() const {
    auto p = findPaper(paper);
    double w = p ? p->width : 297, h = p ? p->height : 420;
    return landscape ? std::max(w, h) : std::min(w, h);
}

double Drawing::paperHeight() const {
    auto p = findPaper(paper);
    double w = p ? p->width : 297, h = p ? p->height : 420;
    return landscape ? std::min(w, h) : std::max(w, h);
}

double Drawing::k() const { return unitToMm(units) * scaleNum / scaleDen; }

std::string Drawing::scaleText() const { return trimmed(scaleNum, 4) + ":" + trimmed(scaleDen, 4); }

int Drawing::find(const std::string& id) const {
    for (std::size_t i = 0; i < entities.size(); ++i) {
        if (entities[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

Layer* Drawing::layer(const std::string& name) {
    for (auto& l : layers) {
        if (l.name == name) return &l;
    }
    for (auto& l : layers) {
        if (lower(l.name) == lower(name)) return &l;
    }
    return nullptr;
}

const Layer* Drawing::layer(const std::string& name) const { return const_cast<Drawing*>(this)->layer(name); }

Layer& Drawing::ensureLayer(const std::string& name) {
    if (Layer* l = layer(name)) return *l;
    layers.push_back(Layer{name});
    return layers.back();
}

std::string Drawing::add(Entity e) {
    e.id = "e" + std::to_string(nextId++);
    if (const Layer* l = layer(e.layer)) e.layer = l->name;
    else ensureLayer(e.layer);
    entities.push_back(std::move(e));
    return entities.back().id;
}

Vec2 Drawing::snap(Vec2 p) const {
    if (!(grid.snap > 0)) return p;
    return {std::round(p.x / grid.snap) * grid.snap, std::round(p.y / grid.snap) * grid.snap};
}

std::string Drawing::summary() const {
    std::string s = paper + (landscape ? " landscape " : " portrait ") + scaleText() + ", " + toString(units) + ", " +
                    std::to_string(entities.size()) + (entities.size() == 1 ? " entity, " : " entities, ") +
                    std::to_string(layers.size()) + (layers.size() == 1 ? " layer" : " layers");
    if (!path.empty()) s += ", " + path;
    return s;
}

bool parseScale(const std::string& text, double& num, double& den) {
    std::string t;
    for (char ch : text) {
        if (!std::isspace(static_cast<unsigned char>(ch))) t += ch;
    }
    auto number = [](const std::string& s, double& out) {
        if (s.empty()) return false;
        char* end = nullptr;
        out = std::strtod(s.c_str(), &end);
        return end && *end == '\0' && out > 0 && std::isfinite(out);
    };
    auto sep = t.find_first_of(":/");
    if (sep == std::string::npos) {
        num = 1;
        return number(t, den);
    }
    return number(t.substr(0, sep), num) && number(t.substr(sep + 1), den);
}

// ---- measuring and formatting ---------------------------------------------

double dimensionValue(const Entity& d) {
    switch (d.dim) {
        case DimKind::Aligned:
            return d.pts.size() >= 2 ? distance(d.pts[0], d.pts[1]) : 0;
        case DimKind::Horizontal:
            return d.pts.size() >= 2 ? std::abs(d.pts[1].x - d.pts[0].x) : 0;
        case DimKind::Vertical:
            return d.pts.size() >= 2 ? std::abs(d.pts[1].y - d.pts[0].y) : 0;
        case DimKind::Radius: return d.r;
        case DimKind::Diameter: return 2 * d.r;
        case DimKind::Angular: {
            if (d.pts.size() < 4) return 0;
            double a1 = angleOf(d.pts[1] - d.pts[0]), a2 = angleOf(d.pts[2] - d.pts[0]), a3 = angleOf(d.pts[3] - d.pts[0]);
            double sweep = normalizeAngle(a2 - a1);
            double rel = normalizeAngle(a3 - a1);
            return rel <= sweep ? sweep : 360 - sweep;
        }
    }
    return 0;
}

namespace {

std::string feetInches(double feet) {
    const bool negative = feet < 0;
    long n16 = std::lround(std::abs(feet) * 12 * 16);
    long ft = n16 / (12 * 16);
    long rem = n16 - ft * 12 * 16;
    long in = rem / 16, frac = rem % 16, den = 16;
    while (frac > 0 && frac % 2 == 0) {
        frac /= 2;
        den /= 2;
    }
    std::string s = (negative ? "-" : "") + std::to_string(ft) + "'-" + std::to_string(in);
    if (frac > 0) s += " " + std::to_string(frac) + "/" + std::to_string(den);
    return s + "\"";
}

}  // namespace

std::string formatLength(const Drawing& doc, double value) {
    int precision = doc.dims.precision;
    if (precision < 0) {
        switch (doc.units) {
            case Unit::Mm: precision = 0; break;
            case Unit::Cm: precision = 1; break;
            case Unit::M: precision = 2; break;
            case Unit::In: precision = 2; break;
            case Unit::Ft: return feetInches(value);
        }
    }
    std::string s = fixed(value, precision);
    if (doc.dims.suffix) {
        switch (doc.units) {
            case Unit::Mm: s += " mm"; break;
            case Unit::Cm: s += " cm"; break;
            case Unit::M: s += " m"; break;
            case Unit::In: s += "\""; break;
            case Unit::Ft: s += "'"; break;
        }
    }
    return s;
}

std::string formatArea(const Drawing& doc, double area) {
    // rooms in m² / ft²; small parts in mm² / in² so they don't round to nothing
    const double mm2 = std::abs(area) * unitToMm(doc.units) * unitToMm(doc.units);
    if (metric(doc.units)) return mm2 >= 1e5 ? fixed(mm2 / 1e6, 2) + " m²" : fixed(mm2, 0) + " mm²";
    const double ft2 = mm2 / (304.8 * 304.8);
    return ft2 >= 1 ? fixed(ft2, 2) + " ft²" : fixed(mm2 / (25.4 * 25.4), 2) + " in²";
}

std::string formatAngle(double deg) { return trimmed(deg, 2) + "°"; }

std::string dimensionText(const Drawing& doc, const Entity& d) {
    const double v = dimensionValue(d);
    std::string value;
    if (d.dim == DimKind::Angular) value = formatAngle(v);
    else value = (d.dim == DimKind::Radius ? "R" : d.dim == DimKind::Diameter ? "Ø" : "") + formatLength(doc, v);
    if (d.text.empty()) return value;
    std::string out = d.text;
    if (auto pos = out.find("<>"); pos != std::string::npos) out.replace(pos, 2, value);
    return out;
}

// ---- walls and openings ---------------------------------------------------

namespace {

struct WallPath {
    std::vector<Vec2> pts;
    std::vector<double> cum;  // distance at the start of each segment, plus the total
    std::size_t segments = 0;
};

WallPath wallPath(const Entity& wall) {
    WallPath w;
    w.pts = dedupe(wall.pts, wall.closed);
    const std::size_t n = w.pts.size();
    if (n < 2) return w;
    w.segments = wall.closed && n > 2 ? n : n - 1;
    w.cum.push_back(0);
    for (std::size_t i = 0; i < w.segments; ++i) w.cum.push_back(w.cum.back() + distance(w.pts[i], w.pts[(i + 1) % n]));
    return w;
}

}  // namespace

std::vector<Vec2> wallFace(const Entity& wall, int side) {
    return offsetPolyline(wall.pts, wall.closed && wall.pts.size() > 2, side * wall.thickness / 2);
}

int openingSegment(const Entity& wall, double at, double width, double* segStart) {
    WallPath w = wallPath(wall);
    const double h = wall.thickness / 2;
    for (std::size_t i = 0; i < w.segments; ++i) {
        const bool openStart = !wall.closed && i == 0, openEnd = !wall.closed && i + 1 == w.segments;
        const double lo = w.cum[i] + (openStart ? 0 : h), hi = w.cum[i + 1] - (openEnd ? 0 : h);
        if (at - width / 2 >= lo - 1e-9 && at + width / 2 <= hi + 1e-9) {
            if (segStart) *segStart = w.cum[i];
            return static_cast<int>(i);
        }
    }
    return -1;
}

double wallDistanceAt(const Entity& wall, Vec2 p) {
    WallPath w = wallPath(wall);
    double best = -1, bestAt = 0;
    for (std::size_t i = 0; i < w.segments; ++i) {
        Vec2 a = w.pts[i], b = w.pts[(i + 1) % w.pts.size()];
        double len = distance(a, b);
        if (len <= 0) continue;
        double t = std::clamp(dot(p - a, b - a) / (len * len), 0.0, 1.0);
        double d = distance(p, a + (b - a) * t);
        if (best < 0 || d < best) {
            best = d;
            bestAt = w.cum[i] + t * len;
        }
    }
    return bestAt;
}

// ---- explode --------------------------------------------------------------

namespace {

Entity basic(EntityType type, const Entity& from) {
    Entity e;
    e.type = type;
    e.layer = from.layer;
    e.color = from.color;
    return e;
}

Entity lineOf(const Entity& from, Vec2 a, Vec2 b) {
    Entity e = basic(EntityType::Line, from);
    e.pts = {a, b};
    return e;
}

Entity textOf(const Entity& from, Vec2 at, const std::string& text, double height, double rotation, HAlign h, VAlign v) {
    Entity e = basic(EntityType::Text, from);
    e.c = at;
    e.text = text;
    e.height = height;
    e.rotation = rotation;
    e.halign = h;
    e.valign = v;
    return e;
}

Entity solidOf(const Entity& from, std::vector<Vec2> pts) {
    Entity e = basic(EntityType::Hatch, from);
    e.pts = std::move(pts);
    e.closed = true;
    e.pattern = "solid";
    return e;
}

// Keeps text reading left to right or bottom to top.
double readable(double deg) {
    double a = normalizeAngle(deg);
    if (a > 90.0 + 1e-9 && a <= 270.0 + 1e-9) a -= 180.0;
    else if (a > 270.0 + 1e-9) a -= 360.0;
    return a;
}

// An arrowhead (or tick, or dot) with its tip at `tip`, pointing along `w`.
void arrowhead(std::vector<Piece>& out, const Drawing& doc, const Entity& from, Vec2 tip, Vec2 w, double size) {
    w = normalized(w);
    Vec2 n = perp(w);
    if (doc.dims.arrows == "tick") {
        Vec2 t = normalized(w + n) * (size / 2);
        out.push_back({lineOf(from, tip - t, tip + t), false});
    } else if (doc.dims.arrows == "dot") {
        std::vector<Vec2> pts;
        for (int i = 0; i < 16; ++i) pts.push_back(tip + direction(i * 22.5) * (size / 4));
        out.push_back({solidOf(from, pts), true});
    } else {
        out.push_back({solidOf(from, {tip, tip - w * size + n * (size / 6), tip - w * size - n * (size / 6)}), true});
    }
}

void explodeDimension(std::vector<Piece>& out, const Drawing& doc, const Entity& d) {
    const double s = 1.0 / doc.k();
    const double th = doc.dims.textHeight * s, asz = doc.dims.arrowSize * s, exo = doc.dims.extOffset * s,
                 exe = doc.dims.extBeyond * s, gap = doc.dims.textGap * s;
    const std::string label = dimensionText(doc, d);
    auto line = [&](Vec2 a, Vec2 b) { out.push_back({lineOf(d, a, b), true}); };
    auto text = [&](Vec2 at, double rot, HAlign h, VAlign v) { out.push_back({textOf(d, at, label, th, rot, h, v), true}); };

    switch (d.dim) {
        case DimKind::Aligned:
        case DimKind::Horizontal:
        case DimKind::Vertical: {
            if (d.pts.size() < 3) return;
            Vec2 p1 = d.pts[0], p2 = d.pts[1], loc = d.pts[2];
            Vec2 u = d.dim == DimKind::Horizontal ? Vec2{1, 0} : d.dim == DimKind::Vertical ? Vec2{0, 1} : normalized(p2 - p1);
            Vec2 n = perp(u);
            Vec2 d1 = p1 + n * dot(loc - p1, n), d2 = p2 + n * dot(loc - p2, n);
            for (auto [p, q] : {std::pair{p1, d1}, std::pair{p2, d2}}) {
                Vec2 v = q - p;
                double len = length(v);
                if (len <= exo) continue;
                Vec2 dir = v / len;
                line(p + dir * exo, q + dir * exe);
            }
            Vec2 w = normalized(d1 - d2);
            if (distance(d1, d2) < 1e-12) w = -u;
            if (doc.dims.arrows == "tick") line(d1 + w * exe, d2 - w * exe);
            else line(d1, d2);
            arrowhead(out, doc, d, d1, w, asz);
            arrowhead(out, doc, d, d2, -w, asz);
            double rot = readable(angleOf(u));
            text((d1 + d2) / 2 + perp(direction(rot)) * gap, rot, HAlign::Center, VAlign::Baseline);
            break;
        }
        case DimKind::Radius: {
            Vec2 dir = direction(d.rotation);
            Vec2 on = d.c + dir * d.r;
            Vec2 land = on + dir * (asz * 1.5);
            line(d.c, land);
            arrowhead(out, doc, d, on, dir, asz);
            text(land + Vec2{dir.x >= 0 ? gap : -gap, 0}, 0, dir.x >= 0 ? HAlign::Left : HAlign::Right, VAlign::Middle);
            break;
        }
        case DimKind::Diameter: {
            Vec2 dir = direction(d.rotation);
            Vec2 a = d.c - dir * d.r, b = d.c + dir * d.r;
            Vec2 land = b + dir * (asz * 1.5);  // the label sits outside, so concentric circles don't collide
            line(a, land);
            arrowhead(out, doc, d, a, -dir, asz);
            arrowhead(out, doc, d, b, dir, asz);
            text(land + Vec2{dir.x >= 0 ? gap : -gap, 0}, 0, dir.x >= 0 ? HAlign::Left : HAlign::Right, VAlign::Middle);
            break;
        }
        case DimKind::Angular: {
            if (d.pts.size() < 4) return;
            Vec2 v = d.pts[0];
            double a1 = angleOf(d.pts[1] - v), a2 = angleOf(d.pts[2] - v), a3 = angleOf(d.pts[3] - v);
            double R = distance(v, d.pts[3]);
            double sweep = normalizeAngle(a2 - a1);
            double start = a1, end = a2;
            if (normalizeAngle(a3 - a1) > sweep) {
                start = a2;
                end = a1;
                sweep = 360 - sweep;
            }
            Entity arc = basic(EntityType::Arc, d);
            arc.c = v;
            arc.r = R;
            arc.a0 = normalizeAngle(start);
            arc.a1 = normalizeAngle(end);
            out.push_back({arc, true});
            for (int i = 1; i <= 2; ++i) {
                Vec2 p = d.pts[static_cast<std::size_t>(i)];
                Vec2 dir = normalized(p - v);
                double len = distance(v, p);
                if (len + exo < R) line(p + dir * exo, v + dir * (R + exe));
            }
            Vec2 ps = v + direction(start) * R, pe = v + direction(end) * R;
            arrowhead(out, doc, d, ps, -perp(direction(start)), asz);
            arrowhead(out, doc, d, pe, perp(direction(end)), asz);
            double mid = start + sweep / 2;
            double rot = readable(mid - 90);
            text(v + direction(mid) * (R + gap), rot, HAlign::Center, VAlign::Baseline);
            break;
        }
    }
}

// Does an open wall end at `end` (the wall running away from it along `dirIn`) butt into
// `target`? If so: which segment, which face (+1 left, -1 right) it meets, and how far the
// end moves along dirIn to sit on that face.
bool buttInto(const Entity& target, Vec2 end, Vec2 dirIn, std::size_t& seg, int& side, double& shift) {
    WallPath tw = wallPath(target);
    const double th = target.thickness / 2;
    const double tol = 1e-6 * std::max(1.0, th);
    if (!target.closed && !tw.pts.empty() &&
        (distance(end, tw.pts.front()) < tol || distance(end, tw.pts.back()) < tol)) {
        return false;  // end to end: a corner, not a T
    }
    for (std::size_t i = 0; i < tw.segments; ++i) {
        Vec2 a = tw.pts[i], b = tw.pts[(i + 1) % tw.pts.size()];
        const double len = distance(a, b);
        if (len <= 0) continue;
        Vec2 u = (b - a) / len, n = perp(u);
        const double t = dot(end - a, u), d = dot(end - a, n);
        if (t < -tol || t > len + tol || std::abs(d) > th + tol) continue;
        const double along = dot(dirIn, n);
        if (std::abs(along) < 0.05) continue;  // running alongside, not into it
        side = along > 0 ? 1 : -1;
        shift = (side * th - d) / along;
        seg = i;
        return true;
    }
    return false;
}

struct Butt {
    bool found = false;
    double shift = 0;
};

Butt buttAt(const Drawing& doc, const Entity& wall, Vec2 end, Vec2 dirIn) {
    for (const auto& o : doc.entities) {
        if (o.type != EntityType::Wall || o.id == wall.id) continue;
        std::size_t seg = 0;
        int side = 0;
        double shift = 0;
        if (buttInto(o, end, dirIn, seg, side, shift)) return {true, shift};
    }
    return {};
}

using Intervals = std::vector<std::pair<double, double>>;

// [lo, hi] minus the gaps.
Intervals subtract(double lo, double hi, Intervals gaps) {
    std::sort(gaps.begin(), gaps.end());
    Intervals out;
    double pos = lo;
    for (auto [g0, g1] : gaps) {
        if (g1 <= pos) continue;
        if (g0 > pos) out.push_back({pos, std::min(g0, hi)});
        pos = std::max(pos, g1);
        if (pos >= hi) break;
    }
    if (pos < hi) out.push_back({pos, hi});
    Intervals clean;
    for (auto iv : out) {
        if (iv.second - iv.first > 1e-9) clean.push_back(iv);
    }
    return clean;
}

void explodeWall(std::vector<Piece>& out, const Drawing& doc, const Entity& wall) {
    WallPath w = wallPath(wall);
    if (w.segments == 0) return;
    const bool closed = wall.closed && w.pts.size() > 2;
    const std::size_t n = w.pts.size();
    const double h = wall.thickness / 2;

    // open ends that run into another wall stop at its face and lose their end cap
    Butt startButt, endButt;
    std::vector<Vec2> adjusted = w.pts;
    if (!closed) {
        Vec2 u0 = normalized(w.pts[1] - w.pts[0]), u1 = normalized(w.pts[n - 1] - w.pts[n - 2]);
        startButt = buttAt(doc, wall, w.pts[0], u0);
        endButt = buttAt(doc, wall, w.pts[n - 1], -u1);
        if (startButt.found) adjusted[0] = w.pts[0] + u0 * startButt.shift;
        if (endButt.found) adjusted[n - 1] = w.pts[n - 1] - u1 * endButt.shift;
    }
    std::vector<Vec2> L = offsetPolyline(adjusted, closed, h), R = offsetPolyline(adjusted, closed, -h);
    if (L.size() != n || R.size() != n) return;

    // openings cut both faces; other walls butting in cut one face
    std::vector<Intervals> openings(w.segments), leftGaps(w.segments), rightGaps(w.segments);
    for (const auto& o : doc.entities) {
        if ((o.type == EntityType::Door || o.type == EntityType::Window) && o.host == wall.id) {
            double segStart = 0;
            int seg = openingSegment(wall, o.at, o.width, &segStart);
            if (seg >= 0) openings[static_cast<std::size_t>(seg)].push_back({o.at - o.width / 2 - segStart, o.at + o.width / 2 - segStart});
            continue;
        }
        if (o.type != EntityType::Wall || o.id == wall.id || o.closed || o.pts.size() < 2) continue;
        const std::size_t m = o.pts.size();
        const std::pair<Vec2, Vec2> ends[2] = {{o.pts[0], normalized(o.pts[1] - o.pts[0])}, {o.pts[m - 1], normalized(o.pts[m - 2] - o.pts[m - 1])}};
        for (auto [end, dirIn] : ends) {
            std::size_t seg = 0;
            int side = 0;
            double shift = 0;
            if (!buttInto(wall, end, dirIn, seg, side, shift)) continue;
            Vec2 a = w.pts[seg], b = w.pts[(seg + 1) % n];
            Vec2 u = normalized(b - a);
            const double t = dot(end + dirIn * shift - a, u);
            const double half = (o.thickness / 2) / std::max(0.05, std::abs(cross(u, dirIn)));
            (side > 0 ? leftGaps : rightGaps)[seg].push_back({t - half, t + half});
        }
    }

    for (std::size_t i = 0; i < w.segments; ++i) {
        Vec2 a = w.pts[i], b = w.pts[(i + 1) % n];
        const double len = distance(a, b);
        if (len <= 0) continue;
        Vec2 u = (b - a) / len, nrm = perp(u);
        const double lo = !closed && i == 0 ? startButt.shift : 0;
        const double hi = !closed && i + 1 == w.segments ? len - endButt.shift : len;
        if (hi - lo <= 1e-9) continue;
        auto face = [&](int side, double t) {
            const auto& F = side > 0 ? L : R;
            if (t <= lo + 1e-9) return F[i];
            if (t >= hi - 1e-9) return F[(i + 1) % n];
            return a + u * t + nrm * (side * h);
        };
        const Intervals solid = subtract(lo, hi, openings[i]);
        for (auto [t0, t1] : solid) {
            if (wall.fill == "solid" || wall.fill == "lines") {
                Entity f = solidOf(wall, {face(1, t0), face(1, t1), face(-1, t1), face(-1, t0)});
                f.pattern = wall.fill;
                f.angle = 45;
                f.spacing = 1.5;
                out.push_back({f, true});
            }
            for (int side : {1, -1}) {
                for (auto [f0, f1] : subtract(t0, t1, side > 0 ? leftGaps[i] : rightGaps[i])) {
                    out.push_back({lineOf(wall, face(side, f0), face(side, f1)), false});
                }
            }
            const bool atStart = t0 <= lo + 1e-9, atEnd = t1 >= hi - 1e-9;
            const bool startCap = !closed && i == 0 && atStart && !startButt.found;
            const bool endCap = !closed && i + 1 == w.segments && atEnd && !endButt.found;
            if (!atStart || startCap) out.push_back({lineOf(wall, face(1, t0), face(-1, t0)), false});
            if (!atEnd || endCap) out.push_back({lineOf(wall, face(1, t1), face(-1, t1)), false});
        }
    }
}

void explodeOpening(std::vector<Piece>& out, const Drawing& doc, const Entity& o) {
    int wi = doc.find(o.host);
    if (wi < 0) return;
    const Entity& wall = doc.entities[static_cast<std::size_t>(wi)];
    WallPath w = wallPath(wall);
    double segStart = 0;
    int seg = openingSegment(wall, o.at, o.width, &segStart);
    if (seg < 0) return;
    Vec2 a = w.pts[static_cast<std::size_t>(seg)], b = w.pts[(static_cast<std::size_t>(seg) + 1) % w.pts.size()];
    Vec2 u = normalized(b - a), nrm = perp(u);
    const double h = wall.thickness / 2, t = o.at - segStart;
    Vec2 c0 = a + u * (t - o.width / 2), c1 = a + u * (t + o.width / 2);
    if (o.type == EntityType::Window) {
        out.push_back({lineOf(o, c0 + nrm * h, c1 + nrm * h), true});
        out.push_back({lineOf(o, c0 - nrm * h, c1 - nrm * h), true});
        out.push_back({lineOf(o, c0 + nrm * (h / 4), c1 + nrm * (h / 4)), false});
        out.push_back({lineOf(o, c0 - nrm * (h / 4), c1 - nrm * (h / 4)), false});
        return;
    }
    const double side = o.swingRight ? -1 : 1;
    Vec2 hingeC = o.hingeEnd ? c1 : c0, otherC = o.hingeEnd ? c0 : c1;
    Vec2 hinge = hingeC + nrm * (side * h), other = otherC + nrm * (side * h);
    Vec2 leafDir = nrm * side;
    Vec2 tip = hinge + leafDir * o.width;
    out.push_back({lineOf(o, hinge, tip), false});
    Entity arc = basic(EntityType::Arc, o);
    arc.c = hinge;
    arc.r = o.width;
    double aLeaf = angleOf(leafDir), aClosed = angleOf(other - hinge);
    if (std::abs(normalizeAngle(aClosed - aLeaf) - 90) < 1) {
        arc.a0 = normalizeAngle(aLeaf);
        arc.a1 = normalizeAngle(aClosed);
    } else {
        arc.a0 = normalizeAngle(aClosed);
        arc.a1 = normalizeAngle(aLeaf);
    }
    out.push_back({arc, true});
}

void explodeRoom(std::vector<Piece>& out, const Drawing& doc, const Entity& room) {
    if (room.outline && room.pts.size() >= 3) {
        Entity p = basic(EntityType::Polyline, room);
        p.pts = room.pts;
        p.closed = true;
        out.push_back({p, true});
    }
    const double s = 1.0 / doc.k();
    const double big = doc.dims.textHeight * 1.4 * s, small = doc.dims.textHeight * s;
    const std::string area = formatArea(doc, polygonArea(room.pts));
    if (!room.name.empty() && room.showArea) {
        out.push_back({textOf(room, room.c + Vec2{0, big * 0.4}, room.name, big, 0, HAlign::Center, VAlign::Baseline), false});
        out.push_back({textOf(room, room.c - Vec2{0, big * 0.4}, area, small, 0, HAlign::Center, VAlign::Top), false});
    } else if (!room.name.empty()) {
        out.push_back({textOf(room, room.c, room.name, big, 0, HAlign::Center, VAlign::Middle), false});
    } else if (room.showArea) {
        out.push_back({textOf(room, room.c, area, small, 0, HAlign::Center, VAlign::Middle), false});
    }
}

}  // namespace

std::vector<Piece> explode(const Drawing& doc, const Entity& e) {
    std::vector<Piece> out;
    switch (e.type) {
        case EntityType::Dimension: explodeDimension(out, doc, e); break;
        case EntityType::Wall: explodeWall(out, doc, e); break;
        case EntityType::Door:
        case EntityType::Window: explodeOpening(out, doc, e); break;
        case EntityType::Room: explodeRoom(out, doc, e); break;
        default: out.push_back({e, false}); break;
    }
    return out;
}

namespace {

void addArcBounds(Box& box, Vec2 c, double r, double a0, double a1) {
    box.add(c + direction(a0) * r);
    box.add(c + direction(a1) * r);
    const double sweep = normalizeAngle(a1 - a0);
    for (int q = 0; q < 4; ++q) {
        if (normalizeAngle(q * 90.0 - a0) <= sweep + 1e-9) box.add(c + direction(q * 90.0) * r);
    }
}

}  // namespace

Box extents(const Drawing& doc) {
    Box box;
    for (const auto& ent : doc.entities) {
        const Layer* l = doc.layer(ent.layer);
        if (l && !l->visible) continue;
        for (const auto& piece : explode(doc, ent)) {
            const Entity& e = piece.e;
            switch (e.type) {
                case EntityType::Circle:
                    box.add(e.c - Vec2{e.r, e.r});
                    box.add(e.c + Vec2{e.r, e.r});
                    break;
                case EntityType::Arc: addArcBounds(box, e.c, e.r, e.a0, e.a1); break;
                case EntityType::Point: box.add(e.c); break;
                case EntityType::Text: {
                    const double w = textWidth(e.text) * e.height;
                    const double x0 = e.halign == HAlign::Left ? 0 : e.halign == HAlign::Center ? -w / 2 : -w;
                    const double y0 = e.valign == VAlign::Baseline ? -0.25 * e.height : e.valign == VAlign::Middle ? -0.5 * e.height : -e.height;
                    Vec2 ux = direction(e.rotation), uy = perp(ux);
                    for (auto [lx, ly] : {std::pair{x0, y0}, std::pair{x0 + w, y0}, std::pair{x0, y0 + e.height * 1.25},
                                          std::pair{x0 + w, y0 + e.height * 1.25}}) {
                        box.add(e.c + ux * lx + uy * ly);
                    }
                    break;
                }
                default:
                    for (auto p : e.pts) box.add(p);
                    break;
            }
        }
    }
    return box;
}

// ---- transforms -----------------------------------------------------------

Xform Xform::rotate(Vec2 center, double deg) {
    const double cs = std::cos(radians(deg)), sn = std::sin(radians(deg));
    return {cs, sn, -sn, cs, center.x - cs * center.x + sn * center.y, center.y - sn * center.x - cs * center.y};
}

Xform Xform::scaleAbout(Vec2 center, double factor) {
    return {factor, 0, 0, factor, center.x * (1 - factor), center.y * (1 - factor)};
}

Xform Xform::mirror(Vec2 p1, Vec2 p2) {
    Vec2 u = normalized(p2 - p1);
    Xform m{u.x * u.x - u.y * u.y, 2 * u.x * u.y, 2 * u.x * u.y, u.y * u.y - u.x * u.x, 0, 0};
    Vec2 q = m.applyDir(p1);
    m.e = p1.x - q.x;
    m.f = p1.y - q.y;
    return m;
}

void transformEntity(Entity& e, const Xform& xf) {
    auto dirAngle = [&](double deg) { return angleOf(xf.applyDir(direction(deg))); };
    for (auto& p : e.pts) p = xf.apply(p);
    e.c = xf.apply(e.c);
    const double s = xf.scale();
    e.r *= s;
    e.height *= s;
    e.thickness *= s;
    switch (e.type) {
        case EntityType::Arc: {
            double n0 = normalizeAngle(dirAngle(e.a0)), n1 = normalizeAngle(dirAngle(e.a1));
            e.a0 = xf.mirrors() ? n1 : n0;
            e.a1 = xf.mirrors() ? n0 : n1;
            break;
        }
        case EntityType::Text: {
            double rot = dirAngle(e.rotation);
            e.rotation = xf.mirrors() ? readable(rot) : normalizeAngle(rot);
            if (e.rotation > 180) e.rotation -= 360;
            break;
        }
        case EntityType::Dimension: {
            if (e.dim == DimKind::Horizontal || e.dim == DimKind::Vertical) {
                Vec2 u = normalized(xf.applyDir(e.dim == DimKind::Horizontal ? Vec2{1, 0} : Vec2{0, 1}));
                if (std::abs(u.y) < 1e-9) e.dim = DimKind::Horizontal;
                else if (std::abs(u.x) < 1e-9) e.dim = DimKind::Vertical;
                else e.dim = DimKind::Aligned;
            }
            if (e.dim == DimKind::Radius || e.dim == DimKind::Diameter) e.rotation = normalizeAngle(dirAngle(e.rotation));
            break;
        }
        case EntityType::Hatch: e.angle = std::fmod(normalizeAngle(dirAngle(e.angle)), 180.0); break;
        default: break;
    }
}

// ---- text metrics ---------------------------------------------------------

namespace {

// Helvetica advance widths (1/1000 em) for ASCII 32..126.
constexpr short kHelvetica[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
    556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556,
    556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};

}  // namespace

double textWidth(const std::string& text) {
    double em = 0;
    for (unsigned cp : detail::utf8Decode(text)) em += detail::glyphWidth(cp);
    return em / 1000.0 / 0.718;  // Helvetica's cap height is 0.718 em
}

namespace detail {

double glyphWidth(unsigned cp) {
    if (cp >= 32 && cp <= 126) return kHelvetica[cp - 32];
    switch (cp) {
        case 0xB0: return 400;  // degree
        case 0xB1: return 584;  // plus-minus
        case 0xB2: return 333;  // superscript two
        case 0xD7: return 584;  // multiplication
        case 0xD8: return 778;  // O with stroke (diameter)
        default: return 556;
    }
}

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
            out.push_back(ch);  // stray byte: read it as Latin-1
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
        put(0xC0u | (cp >> 6));
        put(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000) {
        put(0xE0u | (cp >> 12));
        put(0x80u | ((cp >> 6) & 0x3Fu));
        put(0x80u | (cp & 0x3Fu));
    } else {
        put(0xF0u | (cp >> 18));
        put(0x80u | ((cp >> 12) & 0x3Fu));
        put(0x80u | ((cp >> 6) & 0x3Fu));
        put(0x80u | (cp & 0x3Fu));
    }
    return s;
}

std::string num(double v, int decimals) {
    if (std::abs(v) < 0.5 * std::pow(10.0, -decimals)) return "0";
    return trimmed(v, decimals);
}

}  // namespace detail

}  // namespace achclwss::blueprint
