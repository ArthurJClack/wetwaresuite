// Font from handwriting: trace a scanned grid, trace one image, print a template.
#include <algorithm>
#include <cmath>

#include "achclwss/raster/codec.hpp"
#include "cmdutil.hpp"

namespace achclwss::typeface::cmd {

namespace {

namespace fs = std::filesystem;

std::vector<std::uint32_t> charsFrom(const std::string& spec) {
    std::vector<std::uint32_t> out;
    auto range = [&](std::uint32_t a, std::uint32_t b) {
        for (std::uint32_t c = a; c <= b; ++c) out.push_back(c);
    };
    if (spec == "uppercase") range('A', 'Z');
    else if (spec == "lowercase") range('a', 'z');
    else if (spec == "digits") range('0', '9');
    else if (spec == "letters") {
        range('A', 'Z');
        range('a', 'z');
    } else if (spec == "ascii") {
        range('!', '~');
    } else if (spec == "basic") {
        range('A', 'Z');
        range('a', 'z');
        range('0', '9');
        for (char c : std::string(".,!?'\"-:;()&")) out.push_back(static_cast<unsigned char>(c));
    } else {
        for (auto cp : utf8Decode(spec)) out.push_back(cp == ' ' ? 0 : cp);
    }
    return out;
}

ParamSpec charsParam(bool required) {
    return {"chars", ParamType::String,
            "the characters in the cells, in reading order (a space skips a cell), or uppercase, lowercase, digits, letters, "
            "basic (letters, digits, punctuation) or ascii",
            required};
}

std::vector<ParamSpec> traceParams() {
    return {{"threshold", ParamType::Number, "darkness (0..1) that counts as ink", false, {}, 0.5},
            {"invert", ParamType::Bool, "light ink on a dark background", false, {}, false},
            {"tolerance", ParamType::Number, "curve fitting error in scan pixels (bigger = smoother, fewer points)", false, {}, 1.0},
            {"min_area", ParamType::Number, "ignore specks smaller than this many square pixels", false, {}, 12.0},
            {"corner", ParamType::Number, "turns sharper than this many degrees stay corners", false, {}, 70.0}};
}

TraceOptions traceOptions(const Params& p) {
    TraceOptions o;
    o.threshold = std::clamp(numberParam(p, "threshold", 0.5), 0.01, 0.99);
    o.invert = boolParam(p, "invert", false);
    o.tolerance = std::clamp(numberParam(p, "tolerance", 1.0), 0.05, 50.0);
    o.minArea = std::max(0.0, numberParam(p, "min_area", 12.0));
    o.corner = std::clamp(numberParam(p, "corner", 70.0), 10.0, 179.0);
    return o;
}

// Puts traced contours into the glyph for `cp` (created when missing) and spaces it.
Glyph& placeGlyph(Font& f, std::uint32_t cp, const std::string& ref, std::vector<Contour> contours, bool replace, double lsb, double rsb) {
    int gi = ref.empty() ? f.indexForUnicode(cp) : f.resolve(ref);
    if (gi < 0) {
        Glyph g;
        std::string error;
        std::vector<std::uint32_t> unicodes;
        if (ref.empty() || !newGlyphIdentity(f, ref, "", g.name, unicodes, error)) {
            g.name = glyphNameFor(cp);
            unicodes = {cp};
        }
        g.unicodes = unicodes;
        f.glyphs.push_back(g);
        gi = static_cast<int>(f.glyphs.size()) - 1;
    }
    Glyph& g = f.glyphs[static_cast<std::size_t>(gi)];
    if (replace) {
        g.contours.clear();
        g.components.clear();
    }
    for (auto& c : contours) {
        for (auto& pt : c.points) {
            pt.x = std::round(pt.x);  // whole font units, like every exported font
            pt.y = std::round(pt.y);
        }
    }
    g.contours.insert(g.contours.end(), contours.begin(), contours.end());
    setSidebearings(f, g, lsb, rsb);
    return g;
}

}  // namespace

void registerTraceCommands(Session& session) {
    auto& reg = session.registry();
    {
        std::vector<ParamSpec> params = {docParam(),
                                         {"image", ParamType::String, "scan or photo of the sheet (PNG, JPEG, ...)", true},
                                         charsParam(true),
                                         {"cols", ParamType::Int, "cells per row", false, {}, std::int64_t{10}},
                                         {"rows", ParamType::Int, "rows of cells (default: enough for chars)", false},
                                         {"margin", ParamType::Number, "px around the whole grid (both directions)", false, {}, 0.0},
                                         {"margin_x", ParamType::Number, "px left and right of the grid", false},
                                         {"margin_y", ParamType::Number, "px above and below the grid", false},
                                         {"inset", ParamType::Number, "fraction of each cell ignored at its edges (grid lines, labels)", false, {}, 0.06},
                                         {"baseline", ParamType::Number, "where the baseline sits in each cell, as a fraction from the top (default: from the metrics, like type.grid_template)", false}};
        for (auto& t : traceParams()) params.push_back(t);
        params.push_back({"spacing", ParamType::Number, "side bearings for the traced glyphs (default 6% of the em)", false});
        params.push_back({"mode", ParamType::String, "replace existing glyph outlines or add to them", false, {"replace", "add"}, std::string("replace")});
        params.push_back(previewParam());
        reg.add({"type.trace_grid", "Trace Handwriting Grid",
                 "Build glyphs from a scanned sheet of handwritten (or painted) letters in a grid: the sheet is sliced "
                 "into rows x cols cells, each cell's ink is traced (marching squares) and fitted with Bezier curves, "
                 "and each glyph gets even side bearings. Cell top = ascender, cell bottom = descender, so draw letters "
                 "on the cell's baseline. type.grid_template prints a matching sheet with guides.",
                 params, true},
                fontCommand(session, [](FontDoc& doc, const Params& p) {
                    Font& f = doc.font;
                    const Image sheet = raster::load(fs::path(stringParam(p, "image")));
                    const auto chars = charsFrom(stringParam(p, "chars"));
                    if (chars.empty()) return Result::failure("chars is empty");
                    GridLayout g;
                    g.cols = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "cols", 10), 1, 200));
                    g.rows = has(p, "rows") ? static_cast<int>(std::clamp<std::int64_t>(intParam(p, "rows"), 1, 200))
                                            : static_cast<int>((chars.size() + static_cast<std::size_t>(g.cols) - 1) / static_cast<std::size_t>(g.cols));
                    const double margin = numberParam(p, "margin", 0.0);
                    g.marginX = numberParam(p, "margin_x", margin);
                    g.marginY = numberParam(p, "margin_y", margin);
                    g.inset = numberParam(p, "inset", 0.06);
                    g.baseline = has(p, "baseline") ? numberParam(p, "baseline") : -1;
                    const auto cells = traceGrid(sheet, chars, g, f.metrics, traceOptions(p));
                    const double sp = numberParam(p, "spacing", std::round(f.metrics.unitsPerEm * 0.06));
                    const bool replace = stringParam(p, "mode", "replace") == "replace";
                    std::string empty, made;
                    int count = 0;
                    std::string lastName;
                    for (const auto& c : cells) {
                        if (c.contours.empty()) {
                            empty += utf8Encode(c.codepoint);
                            continue;
                        }
                        Glyph& glyph = placeGlyph(f, c.codepoint, "", c.contours, replace, sp, sp);
                        made += utf8Encode(c.codepoint);
                        lastName = glyph.name;
                        ++count;
                    }
                    Result r = Result::success("traced " + plural(static_cast<std::size_t>(count), "glyph") + " from a " + std::to_string(g.rows) + "x" +
                                               std::to_string(g.cols) + " grid: " + made + (empty.empty() ? "" : "; empty cells: " + empty));
                    if (count == 1) r.with("glyph", lastName);
                    return r.with("traced", static_cast<std::int64_t>(count)).with("glyph_chars", made).with("empty", empty);
                }));
    }
    {
        std::vector<ParamSpec> params = {docParam(),
                                         {"image", ParamType::String, "image of one letter", true},
                                         glyphParam("glyph to fill (created when missing): name, character or U+XXXX"),
                                         {"fit", ParamType::String,
                                          "cell: the image height spans descender to ascender (like one grid cell); cap / xheight / "
                                          "ascender: the ink is scaled to that height on the baseline",
                                          false, {"cell", "cap", "xheight", "ascender"}, std::string("cell")},
                                         {"baseline", ParamType::Number, "fit=cell: baseline position as a fraction from the top", false}};
        for (auto& t : traceParams()) params.push_back(t);
        params.push_back({"spacing", ParamType::Number, "side bearings (default 6% of the em)", false});
        params.push_back({"mode", ParamType::String, "replace the glyph's outline or add to it", false, {"replace", "add"}, std::string("replace")});
        params.push_back(previewParam());
        reg.add({"type.trace_image", "Trace Image Glyph",
                 "Trace one image (a letter drawn on paper, a logo mark, a brush stroke) into a glyph: ink outlines by "
                 "marching squares, Bezier curve fitting, even side bearings.",
                 params, true},
                fontCommand(session, [](FontDoc& doc, const Params& p) {
                    Font& f = doc.font;
                    const Image img = raster::load(fs::path(stringParam(p, "image")));
                    const std::string fit = stringParam(p, "fit", "cell");
                    const auto& m = f.metrics;
                    std::vector<Contour> contours;
                    if (fit == "cell") {
                        GridLayout g;
                        g.inset = 0;
                        g.baseline = has(p, "baseline") ? numberParam(p, "baseline") : -1;
                        auto cells = traceGrid(img, {1}, g, m, traceOptions(p));
                        if (!cells.empty()) contours = cells[0].contours;
                    } else {
                        contours = traceImage(img, traceOptions(p), Affine{1, 0, 0, -1, 0, 0});
                        const Box b = bounds(contours);
                        if (!b.empty) {
                            const double target = fit == "cap" ? m.capHeight : fit == "xheight" ? m.xHeight : m.ascender;
                            const double k = target / std::max(1e-9, b.height());
                            transform(contours, Affine{k, 0, 0, k, -b.x0 * k, -b.y0 * k});
                        }
                    }
                    if (contours.empty()) return Result::failure("found no ink in the image (try a different threshold=, or invert=true)");
                    const double sp = numberParam(p, "spacing", std::round(m.unitsPerEm * 0.06));
                    const std::string ref = stringParam(p, "glyph");
                    auto cp = parseUnicode(ref);
                    Glyph& g = placeGlyph(f, cp ? *cp : 0, ref, contours, stringParam(p, "mode", "replace") == "replace", sp, sp);
                    return Result::success("traced " + plural(contours.size(), "contour") + " into " + glyphLine(f, g))
                        .with("glyph", g.name)
                        .with("contours", static_cast<std::int64_t>(contours.size()));
                }));
    }

    reg.add({"type.grid_template", "Handwriting Template",
             "Draw a sheet to print and write letters on for type.trace_grid: one cell per character with baseline, "
             "x-height and cap-height guides (light enough to be ignored by the tracer) and a faint label. Use the "
             "same chars and cols when tracing the scan.",
             {{"doc", ParamType::String, "font whose metrics place the guides (default: a 1000-unit em)", false},
              {"path", ParamType::String, "output PNG", true},
              charsParam(false),
              {"cols", ParamType::Int, "cells per row", false, {}, std::int64_t{10}},
              {"cell", ParamType::Int, "cell size in px (240 px is about 2 cm at 300 dpi)", false, {}, std::int64_t{240}},
              previewParam()},
             false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        Font fallback = newFont("Template", "Regular", 1000, 800, -200, 500, 700);
                        const Font* font = &fallback;
                        if (has(p, "doc")) {
                            auto doc = session.workspace().getAs<FontDoc>(stringParam(p, "doc"));
                            if (!doc) return Result::failure("no open font called '" + stringParam(p, "doc") + "'");
                            font = &doc->font;
                        }
                        const auto chars = charsFrom(stringParam(p, "chars", "basic"));
                        const int cols = static_cast<int>(intParam(p, "cols", 10));
                        const Image img = gridTemplate(*font, chars, cols, static_cast<int>(intParam(p, "cell", 240)));
                        const std::string path = stringParam(p, "path");
                        raster::save(img, fs::path(path));
                        Result r = Result::success("drew a template with " + plural(chars.size(), "cell") + " (" + std::to_string(img.width()) + "x" +
                                                   std::to_string(img.height()) + ") to " + path + "; trace it with cols=" + std::to_string(std::clamp(cols, 1, 64)));
                        if (boolParam(p, "preview", false)) r.attach(Attachment{"image/png", raster::previewPng(img, 1024), "template.png"});
                        return r.with("path", path)
                            .with("cells", static_cast<std::int64_t>(chars.size()))
                            .with("cols", static_cast<std::int64_t>(std::clamp(cols, 1, 64)))
                            .with("width", static_cast<std::int64_t>(img.width()))
                            .with("height", static_cast<std::int64_t>(img.height()));
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });
}

}  // namespace achclwss::typeface::cmd
