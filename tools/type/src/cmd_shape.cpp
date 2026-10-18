// Outline commands: transform, oblique, embolden, remove overlaps, fix directions.
#include <algorithm>
#include <cmath>
#include <functional>

#include "cmdutil.hpp"

namespace wetwaresuite::typeface::cmd {

namespace {

Affine inverse(const Affine& m) {
    const double det = m.a * m.d - m.b * m.c;
    if (std::abs(det) < 1e-12) return {};
    const double a = m.d / det, b = -m.b / det, c = -m.c / det, d = m.a / det;
    return {a, b, c, d, -(a * m.e + c * m.f), -(b * m.e + d * m.f)};
}

// The composites in `list` whose bases are all (transitively) in `list` too: when every glyph gets the
// same change, these keep their components, since the bases change with them. The other composites in
// `list` are drawn in to plain contours now, before any base changes (else they would change twice).
// A pure translation (shiftOnly) never needs drawing in: those composites just move their components.
enum class Comp { Plain, Keep, Shift };
std::vector<Comp> prepareComposites(Font& f, const std::vector<int>& list, bool sameChangeForAll, bool shiftOnly = false) {
    std::vector<char> inList(f.glyphs.size(), 0);
    for (int i : list) inList[static_cast<std::size_t>(i)] = 1;
    std::vector<int> state(f.glyphs.size(), 0);  // 0 unknown, 1 visiting, 2 keep, 3 no
    std::function<bool(int)> covered = [&](int i) -> bool {
        auto& s = state[static_cast<std::size_t>(i)];
        if (s == 2 || s == 3) return s == 2;
        if (s == 1 || !inList[static_cast<std::size_t>(i)]) return false;
        s = 1;
        bool ok = true;
        for (const auto& c : f.glyphs[static_cast<std::size_t>(i)].components) {
            const int b = f.index(c.base);
            ok = ok && b >= 0 && covered(b);
        }
        s = ok ? 2 : 3;
        return ok;
    };
    std::vector<Comp> keep(f.glyphs.size(), Comp::Plain);
    std::vector<int> drawIn;
    for (int i : list) {
        if (f.glyphs[static_cast<std::size_t>(i)].components.empty()) continue;
        if (sameChangeForAll && covered(i)) keep[static_cast<std::size_t>(i)] = Comp::Keep;
        else if (shiftOnly) keep[static_cast<std::size_t>(i)] = Comp::Shift;
        else drawIn.push_back(i);
    }
    std::vector<std::vector<Contour>> drawn;
    for (int i : drawIn) drawn.push_back(outline(f, f.glyphs[static_cast<std::size_t>(i)]));
    for (std::size_t k = 0; k < drawIn.size(); ++k) {
        Glyph& g = f.glyphs[static_cast<std::size_t>(drawIn[k])];
        g.contours = std::move(drawn[k]);
        g.components.clear();
    }
    return keep;
}

// Applies `m` to a glyph. Components stay when `keepComponents` (their bases get `m` as well), as
// m * C * m^-1 so that the composite comes out as m applied to its old outline.
void apply(Glyph& g, const Affine& m, Comp mode) {
    transform(g.contours, m);
    if (mode == Comp::Shift) {
        for (auto& c : g.components) {
            c.dx += m.e;
            c.dy += m.f;
        }
    }
    if (mode != Comp::Keep) return;
    const Affine inv = inverse(m);
    for (auto& c : g.components) {
        const Affine n = m * componentMatrix(c) * inv;
        c.xx = n.a;
        c.xy = n.b;
        c.yx = n.c;
        c.yy = n.d;
        c.dx = n.e;
        c.dy = n.f;
    }
}

Result changedResult(const std::string& verb, const Font& f, const std::vector<int>& list) {
    Result r = Result::success(verb + " " + plural(list.size(), "glyph"));
    if (list.size() == 1) r.with("glyph", f.glyphs[static_cast<std::size_t>(list[0])].name);
    return r.with("changed", static_cast<std::int64_t>(list.size()));
}

}  // namespace

void registerShapeCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"type.transform", "Transform Glyphs",
             "Move, scale, rotate or skew glyph outlines. The scale/rotation/skew is about origin_x,origin_y (default "
             "0,0: the baseline at the glyph origin), or each glyph's centre with center=true; dx,dy move after that. "
             "Horizontal scaling scales the advance too unless keep_advance=true.",
             {docParam(),
              glyphsParam(),
              {"dx", ParamType::Number, "move right by", false, {}, 0.0},
              {"dy", ParamType::Number, "move up by", false, {}, 0.0},
              {"scale", ParamType::Number, "uniform scale", false},
              {"scale_x", ParamType::Number, "horizontal scale", false},
              {"scale_y", ParamType::Number, "vertical scale", false},
              {"rotate", ParamType::Number, "degrees counter-clockwise", false, {}, 0.0},
              {"skew", ParamType::Number, "slant in degrees (positive leans right)", false, {}, 0.0},
              {"origin_x", ParamType::Number, "transform origin x", false, {}, 0.0},
              {"origin_y", ParamType::Number, "transform origin y", false, {}, 0.0},
              {"center", ParamType::Bool, "use each glyph's bounding-box centre as the origin", false, {}, false},
              {"keep_advance", ParamType::Bool, "leave advance widths alone", false, {}, false},
              previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::vector<int> list;
                std::string error;
                if (!glyphList(f, stringParam(p, "glyphs"), list, error)) return Result::failure(error);
                const double s = numberParam(p, "scale", 1.0);
                const double sx = numberParam(p, "scale_x", s), sy = numberParam(p, "scale_y", s);
                if (sx == 0 || sy == 0) return Result::failure("scale can't be 0");
                const double rot = numberParam(p, "rotate", 0.0), skew = numberParam(p, "skew", 0.0);
                if (std::abs(skew) >= 89) return Result::failure("skew must be between -89 and 89 degrees");
                const Affine linear = Affine::rotate(rot) * Affine::skewX(skew) * Affine::scale(sx, sy);
                const bool center = boolParam(p, "center", false);
                const auto modes = prepareComposites(f, list, !center, linear.translationOnly());
                for (int i : list) {
                    Glyph& g = f.glyphs[static_cast<std::size_t>(i)];
                    double ox = numberParam(p, "origin_x", 0.0), oy = numberParam(p, "origin_y", 0.0);
                    if (center) {
                        const Box b = glyphBounds(f, g);
                        if (!b.empty) {
                            ox = (b.x0 + b.x1) / 2;
                            oy = (b.y0 + b.y1) / 2;
                        }
                    }
                    const Affine m = Affine::translate(numberParam(p, "dx", 0.0) + ox, numberParam(p, "dy", 0.0) + oy) * linear * Affine::translate(-ox, -oy);
                    apply(g, m, modes[static_cast<std::size_t>(i)]);
                    if (!boolParam(p, "keep_advance", false) && sx != 1) g.advance = std::round(g.advance * std::abs(sx));
                }
                return changedResult("transformed", f, list);
            }));

    reg.add({"type.oblique", "Oblique",
             "Slant glyphs to make an oblique (sloped roman) style: a skew around half the x-height, so letters lean "
             "without drifting sideways. Sets the font's italic angle, and the style name when style= is given.",
             {docParam(),
              {"angle", ParamType::Number, "slant in degrees (positive leans right)", false, {}, 12.0},
              glyphsParam(),
              {"style", ParamType::String, "new style name, e.g. Oblique or Bold Oblique", false},
              previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                const double angle = numberParam(p, "angle", 12.0);
                if (std::abs(angle) >= 60) return Result::failure("angle must be between -60 and 60 degrees");
                std::vector<int> list;
                std::string error;
                if (!glyphList(f, stringParam(p, "glyphs"), list, error)) return Result::failure(error);
                const double oy = f.metrics.xHeight / 2;
                const Affine m = Affine::translate(0, oy) * Affine::skewX(angle) * Affine::translate(0, -oy);
                const auto modes = prepareComposites(f, list, true);
                for (int i : list) apply(f.glyphs[static_cast<std::size_t>(i)], m, modes[static_cast<std::size_t>(i)]);
                f.metrics.italicAngle = -angle;
                if (has(p, "style")) f.info.style = stringParam(p, "style");
                Result r = changedResult("slanted " + fmt(angle) + " degrees:", f, list);
                return r.with("italic_angle", f.metrics.italicAngle);
            }));

    reg.add({"type.embolden", "Embolden",
             "Make strokes heavier (or lighter with a negative amount) by offsetting outlines outward from the ink. "
             "amount is per side in font units, so stems grow by twice it; the advance grows by 2 x amount so "
             "spacing stays the same unless keep_advance=true. Overlaps that appear can be merged with "
             "type.remove_overlap.",
             {docParam(),
              {"amount", ParamType::Number, "offset per side in font units (e.g. 20 on a 1000 em; negative thins)", true},
              {"amount_y", ParamType::Number, "vertical offset (default: amount; 0 keeps heights)", false},
              glyphsParam(),
              {"keep_advance", ParamType::Bool, "leave advance widths alone", false, {}, false},
              previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                const double dx = numberParam(p, "amount"), dy = numberParam(p, "amount_y", dx);
                std::vector<int> list;
                std::string error;
                if (!glyphList(f, stringParam(p, "glyphs"), list, error)) return Result::failure(error);
                int n = 0;
                // composites whose bases are emboldened too keep their components (the bases grow and
                // shift by dx, so the composite does as well); others are drawn in first
                prepareComposites(f, list, true);
                for (int i : list) {
                    Glyph& g = f.glyphs[static_cast<std::size_t>(i)];
                    if (g.contours.empty() && g.components.empty()) continue;
                    if (!g.contours.empty()) g.contours = embolden(g.contours, dx, dy);
                    if (!boolParam(p, "keep_advance", false)) {
                        transform(g.contours, Affine::translate(dx, 0));                        g.advance = std::max(0.0, g.advance + 2 * dx);
                    }
                    ++n;
                }
                Result r = changedResult("emboldened by " + fmt(dx) + " units:", f, list);
                return r.with("emboldened", static_cast<std::int64_t>(n));
            }));

    reg.add({"type.remove_overlap", "Remove Overlaps",
             "Merge overlapping contours into one clean outline (union, nonzero rule), e.g. after drawing a letter "
             "from overlapping strokes or after emboldening. Contours that cross are rebuilt and refitted with curves "
             "within tolerance; the rest are kept exactly. Composite glyphs keep their components (their bases "
             "are merged on their own); type.decompose a composite first to merge it as a whole.",
             {docParam(), glyphsParam(), {"tolerance", ParamType::Number, "refit error in font units (default em/1000)", false}, previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::vector<int> list;
                std::string error;
                if (!glyphList(f, stringParam(p, "glyphs"), list, error)) return Result::failure(error);
                const double tol = numberParam(p, "tolerance", f.metrics.unitsPerEm / 1000.0);
                std::size_t before = 0, after = 0;
                std::vector<int> changed;
                for (int i : list) {
                    Glyph& g = f.glyphs[static_cast<std::size_t>(i)];
                    if (g.contours.empty()) continue;
                    before += g.contours.size();
                    std::vector<Contour> merged = removeOverlaps(g.contours, tol);
                    bool same = merged.size() == g.contours.size();
                    for (std::size_t k = 0; same && k < merged.size(); ++k) same = merged[k].points.size() == g.contours[k].points.size();
                    if (!same) changed.push_back(i);
                    g.contours = std::move(merged);
                    after += g.contours.size();
                }
                Result r = changedResult("removed overlaps (" + std::to_string(before) + " contours -> " + std::to_string(after) + ") in", f, changed);
                return r.with("contours_before", static_cast<std::int64_t>(before)).with("contours_after", static_cast<std::int64_t>(after));
            }));

    reg.add({"type.fix_directions", "Fix Contour Directions",
             "Set every contour's direction by nesting (outer contours clockwise, holes counter-clockwise, the "
             "TrueType rule), so counters render as holes everywhere.",
             {docParam(), glyphsParam(), previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::vector<int> list;
                std::string error;
                if (!glyphList(f, stringParam(p, "glyphs"), list, error)) return Result::failure(error);
                int reversed = 0;
                for (int i : list) reversed += fixDirections(f.glyphs[static_cast<std::size_t>(i)].contours);
                Result r = changedResult("checked", f, list);
                r.message += ", reversed " + plural(static_cast<std::size_t>(reversed), "contour");
                return r.with("reversed", static_cast<std::int64_t>(reversed));
            }));
}

}  // namespace wetwaresuite::typeface::cmd
