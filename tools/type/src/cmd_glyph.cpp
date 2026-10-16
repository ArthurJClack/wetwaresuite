// Glyph commands: add, set, remove, copy, info, draw, components, decompose, SVG import.
#include <algorithm>
#include <cmath>

#include "cmdutil.hpp"

namespace achclwss::typeface::cmd {

namespace {

namespace fs = std::filesystem;

std::vector<ParamSpec> shapeParams() {
    return {{"path", ParamType::String, "SVG path data in font units (y up), e.g. \"M100 0L300 700L500 0Z\"; curves are kept", false},
            {"points", ParamType::NumberList, "a closed polygon as x1,y1,x2,y2,... in font units", false},
            {"curve", ParamType::String, "points: straight edges, or a smooth curve through them", false, {"none", "smooth"}, std::string("none")},
            {"rect", ParamType::NumberList, "a rectangle x,y,width,height", false},
            {"ellipse", ParamType::NumberList, "an ellipse cx,cy,rx,ry (a circle when rx = ry)", false},
            {"y_down", ParamType::Bool, "path/points/rect/ellipse use screen coordinates: y down from the ascender line", false, {}, false}};
}

// Contours from path= / points= / rect= / ellipse=. False with an error when none is given or one is bad.
bool shapesFrom(const Font& font, const Params& p, std::vector<Contour>& out, std::string& error, bool required = true) {
    out.clear();
    if (has(p, "path")) {
        std::string perr;
        auto cs = parsePathData(stringParam(p, "path"), &perr);
        if (!perr.empty()) {
            error = "bad path data: " + perr;
            return false;
        }
        out.insert(out.end(), cs.begin(), cs.end());
    }
    if (has(p, "points")) {
        const auto v = listParam(p, "points");
        if (v.size() < 6 || v.size() % 2) {
            error = "points needs at least 3 x,y pairs";
            return false;
        }
        Polygon poly;
        for (std::size_t i = 0; i + 1 < v.size(); i += 2) poly.push_back({v[i], v[i + 1]});
        out.push_back(stringParam(p, "curve", "none") == "smooth" ? smoothThrough(poly) : polygonContour(poly));
    }
    if (has(p, "rect")) {
        const auto v = listParam(p, "rect");
        if (v.size() != 4 || v[2] == 0 || v[3] == 0) {
            error = "rect needs x,y,width,height";
            return false;
        }
        out.push_back(rectContour(std::min(v[0], v[0] + v[2]), std::min(v[1], v[1] + v[3]), std::abs(v[2]), std::abs(v[3])));
    }
    if (has(p, "ellipse")) {
        const auto v = listParam(p, "ellipse");
        if (v.size() != 4 || v[2] <= 0 || v[3] <= 0) {
            error = "ellipse needs cx,cy,rx,ry";
            return false;
        }
        out.push_back(ellipseContour(v[0], v[1], v[2], v[3]));
    }
    if (out.empty()) {
        if (required) error = "give the shape as path=, points=, rect= or ellipse=";
        return !required;
    }
    if (boolParam(p, "y_down", false)) transform(out, Affine{1, 0, 0, -1, 0, font.metrics.ascender});
    return true;
}

Result glyphResult(const std::string& message, const Font& font, const Glyph& g) {
    Result r = Result::success(message);
    r.with("glyph", g.name).with("advance", g.advance);
    const Box b = glyphBounds(font, g);
    if (!b.empty) r.with("lsb", b.x0).with("rsb", g.advance - b.x1).with("bounds", NumberList{b.x0, b.y0, b.x1, b.y1});
    r.with("contours", static_cast<std::int64_t>(g.contours.size()));
    return r;
}

}  // namespace

void registerGlyphCommands(Session& session) {
    auto& reg = session.registry();
    {
        std::vector<ParamSpec> params = {docParam(),
                                         {"name", ParamType::String,
                                          "glyph name (A, a, zero, Aacute) or just the character (A, é, !); a character also sets the unicode",
                                          false},
                                         {"unicode", ParamType::String, "code point(s): A, U+0041, 0x41, comma-separated; none for an unencoded glyph", false},
                                         {"advance", ParamType::Number, "advance width (default: from the outline, equal side bearings; 0.5 em if empty)", false}};
        for (auto& s : shapeParams()) params.push_back(s);
        params.push_back({"replace", ParamType::Bool, "replace a glyph that already has this name", false, {}, false});
        params.push_back(previewParam());
        reg.add({"type.glyph_add", "Add Glyph",
                 "Add a glyph, optionally with an outline from SVG path data (font units, y up, baseline 0; use "
                 "y_down=true for screen-style coordinates), a polygon, a rect or an ellipse. type.draw adds more "
                 "contours later. Without a name, the unicode decides it (A -> A, ! -> exclam, é -> uni00E9).",
                 params, true},
                fontCommand(session, [](FontDoc& doc, const Params& p) {
                    Font& f = doc.font;
                    std::string name, error;
                    std::vector<std::uint32_t> unicodes;
                    if (!newGlyphIdentity(f, stringParam(p, "name"), stringParam(p, "unicode"), name, unicodes, error)) return Result::failure(error);
                    const int existing = f.index(name);
                    if (existing >= 0 && !boolParam(p, "replace", false))
                        return Result::failure("glyph " + name + " already exists (replace=true to replace it, type.draw to add to it)");
                    Glyph g;
                    g.name = name;
                    g.unicodes = unicodes;
                    if (!shapesFrom(f, p, g.contours, error, false)) return Result::failure(error);
                    fixDirections(g.contours);
                    if (has(p, "advance")) {
                        g.advance = std::max(0.0, numberParam(p, "advance"));
                    } else {
                        const Box b = bounds(g.contours);
                        g.advance = b.empty ? std::round(f.metrics.unitsPerEm * 0.5) : std::round(b.x1 + std::max(0.0, b.x0));
                    }
                    // a code point belongs to one glyph
                    for (auto& other : f.glyphs) {
                        if (other.name == name) continue;
                        for (auto u : unicodes) other.unicodes.erase(std::remove(other.unicodes.begin(), other.unicodes.end(), u), other.unicodes.end());
                    }
                    if (existing >= 0) f.glyphs[static_cast<std::size_t>(existing)] = g;
                    else f.glyphs.push_back(g);
                    return glyphResult((existing >= 0 ? "replaced " : "added ") + glyphLine(f, g), f, g);
                }));
    }

    reg.add({"type.glyph_set", "Set Glyph",
             "Change a glyph's name (components and kerning follow), unicodes, advance width or side bearings. "
             "lsb moves the outline so the left side bearing is that many units; rsb sets the advance so the right "
             "side bearing is that many units.",
             {docParam(),
              glyphParam(),
              {"name", ParamType::String, "new name", false},
              {"unicode", ParamType::String, "new code point list (replaces), e.g. A or U+0041,U+0391; none clears", false},
              {"add_unicode", ParamType::String, "code point(s) to add", false},
              {"advance", ParamType::Number, "advance width", false},
              {"lsb", ParamType::Number, "left side bearing", false},
              {"rsb", ParamType::Number, "right side bearing", false},
              previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::string error;
                const int gi = requireGlyph(f, stringParam(p, "glyph"), error);
                if (gi < 0) return Result::failure(error);
                std::vector<std::string> changed;
                auto claim = [&](const std::vector<std::uint32_t>& cps) {
                    for (std::size_t i = 0; i < f.glyphs.size(); ++i) {
                        if (static_cast<int>(i) == gi) continue;
                        auto& u = f.glyphs[i].unicodes;
                        for (auto c : cps) u.erase(std::remove(u.begin(), u.end(), c), u.end());
                    }
                };
                if (has(p, "unicode")) {
                    std::vector<std::uint32_t> cps;
                    if (!parseUnicodeList(stringParam(p, "unicode"), cps, error)) return Result::failure(error);
                    claim(cps);
                    f.glyphs[static_cast<std::size_t>(gi)].unicodes = cps;
                    changed.push_back("unicodes");
                }
                if (has(p, "add_unicode")) {
                    std::vector<std::uint32_t> cps;
                    if (!parseUnicodeList(stringParam(p, "add_unicode"), cps, error)) return Result::failure(error);
                    claim(cps);
                    auto& u = f.glyphs[static_cast<std::size_t>(gi)].unicodes;
                    for (auto c : cps) {
                        if (std::find(u.begin(), u.end(), c) == u.end()) u.push_back(c);
                    }
                    changed.push_back("unicodes");
                }
                Glyph& g = f.glyphs[static_cast<std::size_t>(gi)];
                if (has(p, "advance")) {
                    g.advance = std::max(0.0, numberParam(p, "advance"));
                    changed.push_back("advance");
                }
                if (has(p, "lsb") || has(p, "rsb")) {
                    std::optional<double> l, r;
                    if (has(p, "lsb")) l = numberParam(p, "lsb");
                    if (has(p, "rsb")) r = numberParam(p, "rsb");
                    setSidebearings(f, g, l, r);
                    changed.push_back("side bearings");
                }
                if (has(p, "name")) {
                    const std::string n = stringParam(p, "name");
                    if (!validGlyphName(n)) return Result::failure("'" + n + "' isn't a usable glyph name");
                    if (f.index(n) >= 0 && f.index(n) != gi) return Result::failure("there is already a glyph called " + n);
                    renameGlyph(f, gi, n);
                    changed.push_back("name");
                }
                if (changed.empty()) return Result::failure("nothing to change: give name=, unicode=, advance=, lsb= or rsb=");
                const Glyph& out = f.glyphs[static_cast<std::size_t>(gi)];
                return glyphResult("updated " + glyphLine(f, out), f, out);
            }));

    reg.add({"type.glyph_remove", "Remove Glyphs",
             "Remove glyphs, or with keep= remove every glyph except those (subsetting a font down to the characters a "
             "page needs). Their kerning pairs go with them; glyphs that keep using them as components keep the shape "
             "(the component is drawn in first). .notdef always stays with keep=.",
             {docParam(),
              {"glyphs", ParamType::String, "glyphs to remove: names or characters, comma-separated, ranges like a-z or U+0100-U+017F, or the words of glyphs=", false},
              {"keep", ParamType::String, "remove everything except these, e.g. ascii, latin1, or A-Z,a-z,0-9,space,period", false}},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                const bool keepMode = has(p, "keep");
                if (keepMode == has(p, "glyphs")) return Result::failure("give glyphs= (what to remove) or keep= (what to keep)");
                std::vector<int> list;
                std::string error;
                if (!glyphList(f, stringParam(p, keepMode ? "keep" : "glyphs"), list, error)) return Result::failure(error);
                if (keepMode) {
                    // keep the components the kept glyphs are built from, too
                    std::vector<char> keep(f.glyphs.size(), 0);
                    std::vector<int> todo = list;
                    if (int nd = f.index(".notdef"); nd >= 0) todo.push_back(nd);
                    while (!todo.empty()) {
                        const int i = todo.back();
                        todo.pop_back();
                        if (keep[static_cast<std::size_t>(i)]) continue;
                        keep[static_cast<std::size_t>(i)] = 1;
                        for (const auto& c : f.glyphs[static_cast<std::size_t>(i)].components) {
                            if (int b = f.index(c.base); b >= 0) todo.push_back(b);
                        }
                    }
                    list.clear();
                    for (std::size_t i = 0; i < f.glyphs.size(); ++i) {
                        if (!keep[i]) list.push_back(static_cast<int>(i));
                    }
                    if (list.empty()) return Result::success("nothing to remove: the font only has those glyphs").with("removed", std::int64_t{0});
                }
                std::string names;
                for (std::size_t k = 0; k < list.size() && k < 12; ++k) names += (k ? ", " : "") + f.glyphs[static_cast<std::size_t>(list[k])].name;
                if (list.size() > 12) names += ", ... " + std::to_string(list.size() - 12) + " more";
                removeGlyphs(f, list);
                return Result::success("removed " + plural(list.size(), "glyph") + ": " + names + " (" + plural(f.glyphs.size(), "glyph") + " left)")
                    .with("removed", static_cast<std::int64_t>(list.size()))
                    .with("glyphs", static_cast<std::int64_t>(f.glyphs.size()))
                    .with("kerning_pairs", static_cast<std::int64_t>(f.kerning.size()));
            }));

    reg.add({"type.glyph_copy", "Copy Glyph",
             "Copy a glyph to a new name (and unicode), from this font or another open font (from_doc). With "
             "component=true the copy is a reference to the original instead of its own outline, which is how "
             "accented letters are built (then type.component_add the accent).",
             {docParam(),
              {"from", ParamType::String, "glyph to copy (name or character)", true},
              {"to", ParamType::String, "new glyph name or character (default: the same name, for a copy between fonts)", false},
              {"unicode", ParamType::String, "code point(s) for the copy (default: from 'to' when it is a character)", false},
              {"from_doc", ParamType::String, "another open font to copy from (default: this one)", false},
              {"component", ParamType::Bool, "make the copy a component reference to the original", false, {}, false},
              {"replace", ParamType::Bool, "replace an existing glyph with that name", false, {}, false},
              previewParam()},
             true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                const std::string fromDoc = stringParam(p, "from_doc");
                std::shared_ptr<FontDoc> source = fromDoc.empty() ? nullptr : session.workspace().getAs<FontDoc>(fromDoc);
                if (!fromDoc.empty() && !source)
                    return std::make_unique<FunctionAction>([fromDoc] { return Result::failure("no open font called '" + fromDoc + "'"); });
                return fontCommand(session, [source](FontDoc& doc, const Params& q) {
                    Font& f = doc.font;
                    const Font& src = source ? source->font : f;
                    std::string error;
                    const int si = requireGlyph(src, stringParam(q, "from"), error);
                    if (si < 0) return Result::failure(error);
                    Glyph g = src.glyphs[static_cast<std::size_t>(si)];
                    const std::string to = stringParam(q, "to", source ? g.name : "");
                    if (to.empty()) return Result::failure("give to= (the new glyph's name or character)");
                    std::string name;
                    std::vector<std::uint32_t> unicodes;
                    if (!newGlyphIdentity(f, to, stringParam(q, "unicode"), name, unicodes, error)) return Result::failure(error);
                    if (source && stringParam(q, "unicode").empty() && to == g.name) unicodes = g.unicodes;
                    const int existing = f.index(name);
                    if (existing >= 0 && !boolParam(q, "replace", false)) return Result::failure("glyph " + name + " already exists (replace=true)");
                    if (boolParam(q, "component", false)) {
                        if (source) return Result::failure("component copies only work within one font");
                        g.contours.clear();
                        g.components = {Component{src.glyphs[static_cast<std::size_t>(si)].name}};
                    } else if (source) {
                        decompose(src, g);  // the other font's components may not exist here
                    }
                    g.name = name;
                    g.unicodes = unicodes;
                    for (auto& other : f.glyphs) {
                        if (other.name == name) continue;
                        for (auto u : unicodes) other.unicodes.erase(std::remove(other.unicodes.begin(), other.unicodes.end(), u), other.unicodes.end());
                    }
                    if (source && src.metrics.unitsPerEm != f.metrics.unitsPerEm) {
                        const double k = static_cast<double>(f.metrics.unitsPerEm) / src.metrics.unitsPerEm;
                        transform(g.contours, Affine::scale(k, k));
                        g.advance *= k;
                    }
                    if (existing >= 0) f.glyphs[static_cast<std::size_t>(existing)] = g;
                    else f.glyphs.push_back(g);
                    return glyphResult("copied " + src.glyphs[static_cast<std::size_t>(si)].name + " to " + glyphLine(f, g), f, g);
                })(p);
            });

    reg.add({"type.glyph_info", "Glyph Info",
             "Describe one glyph: unicodes, advance, bounds, side bearings, components, its outline as SVG path data "
             "(font units, y up) and, with points=true, every point (on-curve, q = quadratic control, c = cubic control).",
             {docOrFileParam(), fontFileParam(), glyphParam(), {"points", ParamType::Bool, "list every point", false, {}, false}, previewParam()},
             false, true},
            fontCommand(
                session,
                [](FontDoc& doc, const Params& p) {
                    const Font& f = doc.font;
                    std::string error;
                    const int gi = requireGlyph(f, stringParam(p, "glyph"), error);
                    if (gi < 0) return Result::failure(error);
                    const Glyph& g = f.glyphs[static_cast<std::size_t>(gi)];
                    const std::string d = pathData(g.contours);
                    Result r = glyphResult(glyphLine(f, g) + (d.empty() ? "" : "\npath: " + d), f, g);
                    if (boolParam(p, "points", false)) {
                        for (std::size_t c = 0; c < g.contours.size(); ++c) {
                            r.message += "\ncontour " + std::to_string(c) + ":";
                            for (const auto& pt : g.contours[c].points)
                                r.message += " " + fmt(pt.x) + "," + fmt(pt.y) + (pt.type == PointType::Quad ? "q" : pt.type == PointType::Cubic ? "c" : "");
                        }
                    }
                    std::string unis;
                    for (auto u : g.unicodes) unis += (unis.empty() ? "" : ",") + unicodeLabel(u);
                    r.with("unicodes", unis).with("path", d).with("components", static_cast<std::int64_t>(g.components.size()));
                    return r;
                },
                false));

    {
        std::vector<ParamSpec> params = {docParam(), glyphParam("glyph to draw into (created when missing): name, character or U+XXXX")};
        for (auto& s : shapeParams()) params.push_back(s);
        params.push_back({"mode", ParamType::String, "add contours, replace the outline, or cut them out as holes", false,
                          {"add", "replace", "cut"}, std::string("add")});
        params.push_back({"advance", ParamType::Number, "set the advance width too", false});
        params.push_back(previewParam());
        reg.add({"type.draw", "Draw Contours",
                 "Draw contours into a glyph: SVG path data (M L H V C S Q T A Z; quadratic and cubic curves are kept), "
                 "a polygon from a point list (optionally a smooth curve through it), a rect or an ellipse. Units are "
                 "font units with y up from the baseline (y_down=true flips to screen-style). mode=cut punches holes "
                 "(counters). Overlapping contours fill fine (nonzero); type.remove_overlap merges them.",
                 params, true},
                fontCommand(session, [](FontDoc& doc, const Params& p) {
                    Font& f = doc.font;
                    std::string error;
                    int gi = f.resolve(stringParam(p, "glyph"));
                    bool made = false;
                    if (gi < 0) {
                        std::string name;
                        std::vector<std::uint32_t> unicodes;
                        if (!newGlyphIdentity(f, stringParam(p, "glyph"), "", name, unicodes, error)) return Result::failure(error);
                        Glyph g;
                        g.name = name;
                        g.unicodes = unicodes;
                        g.advance = 0;
                        f.glyphs.push_back(g);
                        gi = static_cast<int>(f.glyphs.size()) - 1;
                        made = true;
                    }
                    std::vector<Contour> shapes;
                    if (!shapesFrom(f, p, shapes, error)) return Result::failure(error);
                    Glyph& g = f.glyphs[static_cast<std::size_t>(gi)];
                    const std::string mode = stringParam(p, "mode", "add");
                    if (mode == "replace") g.contours.clear();
                    if (mode == "cut") {
                        // holes run against the outer direction
                        fixDirections(shapes);
                        for (auto& c : shapes) reverse(c);
                        g.contours.insert(g.contours.end(), shapes.begin(), shapes.end());
                    } else {
                        // orient the new shapes among themselves only: a stroke that overlaps an
                        // existing one must stay an outer contour so the two add up (nonzero)
                        fixDirections(shapes);
                        g.contours.insert(g.contours.end(), shapes.begin(), shapes.end());
                    }
                    if (has(p, "advance")) {
                        g.advance = std::max(0.0, numberParam(p, "advance"));
                    } else if (made || g.advance <= 0) {
                        const Box b = bounds(g.contours);
                        g.advance = b.empty ? std::round(f.metrics.unitsPerEm * 0.5) : std::round(b.x1 + std::max(0.0, b.x0));
                    }
                    return glyphResult(std::string(made ? "created " : "drew into ") + glyphLine(f, g), f, g);
                }));
    }

    reg.add({"type.component_add", "Add Component",
             "Place another glyph inside this one as a component (a reference, so later edits to the base show up): "
             "e.g. glyph=Aacute base=A, then base=acute dx=... dy=... The glyph is created when missing, taking the "
             "first base's advance. Exported TTFs keep components as composite glyphs.",
             {docParam(),
              glyphParam("glyph to add the component to (created when missing)"),
              {"base", ParamType::String, "glyph to place (name or character)", true},
              {"dx", ParamType::Number, "horizontal offset", false, {}, 0.0},
              {"dy", ParamType::Number, "vertical offset", false, {}, 0.0},
              {"scale", ParamType::Number, "uniform scale", false},
              {"scale_x", ParamType::Number, "horizontal scale (-1 mirrors)", false},
              {"scale_y", ParamType::Number, "vertical scale (-1 flips)", false},
              {"center", ParamType::Bool, "centre the base horizontally over the glyph's current outline (for accents)", false, {}, false},
              previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::string error;
                const int bi = requireGlyph(f, stringParam(p, "base"), error);
                if (bi < 0) return Result::failure(error);
                const std::string baseName = f.glyphs[static_cast<std::size_t>(bi)].name;
                int gi = f.resolve(stringParam(p, "glyph"));
                bool made = false;
                if (gi < 0) {
                    std::string name;
                    std::vector<std::uint32_t> unicodes;
                    if (!newGlyphIdentity(f, stringParam(p, "glyph"), "", name, unicodes, error)) return Result::failure(error);
                    Glyph g;
                    g.name = name;
                    g.unicodes = unicodes;
                    g.advance = f.glyphs[static_cast<std::size_t>(bi)].advance;
                    f.glyphs.push_back(g);
                    gi = static_cast<int>(f.glyphs.size()) - 1;
                    made = true;
                }
                if (f.glyphs[static_cast<std::size_t>(gi)].name == baseName) return Result::failure("a glyph can't contain itself");
                // refuse cycles: the base must not use this glyph
                std::function<bool(const std::string&, int)> uses = [&](const std::string& n, int depth) {
                    if (depth > 16) return true;
                    const Glyph* g = f.glyph(n);
                    if (!g) return false;
                    for (const auto& c : g->components) {
                        if (c.base == f.glyphs[static_cast<std::size_t>(gi)].name || uses(c.base, depth + 1)) return true;
                    }
                    return false;
                };
                if (uses(baseName, 0)) return Result::failure(baseName + " already uses " + f.glyphs[static_cast<std::size_t>(gi)].name + " (that would be a loop)");
                Component c;
                c.base = baseName;
                const double s = numberParam(p, "scale", 1.0);
                c.xx = numberParam(p, "scale_x", s);
                c.yy = numberParam(p, "scale_y", s);
                c.dx = numberParam(p, "dx", 0.0);
                c.dy = numberParam(p, "dy", 0.0);
                if (boolParam(p, "center", false)) {
                    const Box host = glyphBounds(f, f.glyphs[static_cast<std::size_t>(gi)]);
                    Glyph probe;
                    probe.components = {Component{baseName, c.xx, 0, 0, c.yy, 0, 0}};
                    const Box b = glyphBounds(f, probe);
                    const double hostMid = host.empty ? f.glyphs[static_cast<std::size_t>(gi)].advance / 2 : (host.x0 + host.x1) / 2;
                    if (!b.empty) c.dx += hostMid - (b.x0 + b.x1) / 2;
                }
                Glyph& g = f.glyphs[static_cast<std::size_t>(gi)];
                g.components.push_back(c);
                return glyphResult(std::string(made ? "created " : "updated ") + glyphLine(f, g), f, g);
            }));

    reg.add({"type.decompose", "Decompose",
             "Draw components into plain contours (needed before editing the shape of a composite glyph).",
             {docParam(), glyphsParam(), previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                std::vector<int> list;
                std::string error;
                if (!glyphList(doc.font, stringParam(p, "glyphs"), list, error)) return Result::failure(error);
                std::size_t n = 0;
                std::string last;
                for (int i : list) {
                    Glyph& g = doc.font.glyphs[static_cast<std::size_t>(i)];
                    if (g.components.empty()) continue;
                    decompose(doc.font, g);
                    last = g.name;
                    ++n;
                }
                Result r = Result::success("decomposed " + plural(n, "glyph"));
                if (n == 1) r.with("glyph", last);
                return r.with("changed", static_cast<std::int64_t>(n));
            }));

    reg.add({"type.import_svg", "Import SVG Glyph",
             "Make a glyph from an SVG drawing (a file, or svg= inline): filled paths, rects, circles, ellipses and "
             "polygons, with their transforms; strokes and text are skipped. fit=canvas maps the SVG's height to the "
             "space from descender to ascender (and its width to the advance), as if the SVG were the glyph's box; "
             "cap / xheight / ascender scale the ink to sit on the baseline at that height; none uses SVG units as "
             "font units.",
             {docParam(),
              glyphParam("glyph to fill (created when missing): name, character or U+XXXX"),
              {"path", ParamType::String, "SVG file", false},
              {"svg", ParamType::String, "SVG text instead of a file", false},
              {"fit", ParamType::String, "how the drawing maps to the em", false, {"canvas", "cap", "xheight", "ascender", "none"}, std::string("canvas")},
              {"lsb", ParamType::Number, "left side bearing (default: kept for canvas/none, 6% of the em otherwise)", false},
              {"rsb", ParamType::Number, "right side bearing (default like lsb)", false},
              {"advance", ParamType::Number, "advance width instead of side bearings", false},
              {"mode", ParamType::String, "replace the glyph's outline or add to it", false, {"replace", "add"}, std::string("replace")},
              previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::string text;
                if (has(p, "svg")) text = stringParam(p, "svg");
                else if (has(p, "path")) text = readFile(stringParam(p, "path"));
                else return Result::failure("give path= (an SVG file) or svg= (SVG text)");
                const std::string fit = stringParam(p, "fit", "canvas");
                double advance = 0;
                auto contours = importSvg(text, f.metrics, fit, &advance);
                std::string error;
                int gi = f.resolve(stringParam(p, "glyph"));
                if (gi < 0) {
                    std::string name;
                    std::vector<std::uint32_t> unicodes;
                    if (!newGlyphIdentity(f, stringParam(p, "glyph"), "", name, unicodes, error)) return Result::failure(error);
                    Glyph g;
                    g.name = name;
                    g.unicodes = unicodes;
                    f.glyphs.push_back(g);
                    gi = static_cast<int>(f.glyphs.size()) - 1;
                }
                Glyph& g = f.glyphs[static_cast<std::size_t>(gi)];
                if (stringParam(p, "mode", "replace") == "replace") {
                    g.contours.clear();
                    g.components.clear();
                }
                g.contours.insert(g.contours.end(), contours.begin(), contours.end());
                const double sb = std::round(f.metrics.unitsPerEm * 0.06);
                if (has(p, "advance")) {
                    g.advance = std::max(0.0, numberParam(p, "advance"));
                } else if ((fit == "canvas" || fit == "none") && !has(p, "lsb") && !has(p, "rsb")) {
                    g.advance = advance;
                } else {
                    setSidebearings(f, g, numberParam(p, "lsb", sb), numberParam(p, "rsb", sb));
                }
                return glyphResult("imported " + plural(contours.size(), "contour") + " into " + glyphLine(f, g), f, g);
            }));
}

}  // namespace achclwss::typeface::cmd
