// Previews, rendered text, proof sheets and measuring.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "achclwss/raster/codec.hpp"
#include "cmdutil.hpp"

namespace achclwss::typeface::cmd {

namespace {

namespace fs = std::filesystem;

TextStyle styleFrom(const Params& p, double defaultSize) {
    TextStyle st;
    st.size = std::clamp(numberParam(p, "size", defaultSize), 2.0, 2000.0);
    st.kerning = boolParam(p, "kerning", true);
    st.tracking = numberParam(p, "tracking", 0.0);
    return st;
}

bool colorParam(const Params& p, const std::string& key, Color fallback, Color& out, std::string& error) {
    out = fallback;
    if (!has(p, key)) return true;
    auto c = Color::parse(stringParam(p, key));
    if (!c) {
        error = key + " isn't a colour: " + stringParam(p, key);
        return false;
    }
    out = *c;
    return true;
}

std::string missingChars(const Font& f, const std::string& text) {
    std::string missing;
    for (auto cp : utf8Decode(text)) {
        if (cp == ' ' || cp == '\n') continue;
        if (f.indexForUnicode(cp) < 0 && missing.find(utf8Encode(cp)) == std::string::npos) missing += utf8Encode(cp);
    }
    return missing;
}

}  // namespace

void registerOutputCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"type.preview", "Preview Font",
             "Look at the font (PNG attachment, nothing is written): with glyph= one glyph large with its points, "
             "handles and metric lines; with text= a line of text set in the font (kerned); otherwise a grid of every "
             "glyph with its name.",
             {docOrFileParam(), fontFileParam(),
              {"glyph", ParamType::String, "show this glyph (name or character)", false},
              {"text", ParamType::String, "show this text", false},
              {"size", ParamType::Number, "text size in px, or glyph view size", false},
              {"points", ParamType::Bool, "glyph view: show points and handles", false, {}, true},
              {"metrics", ParamType::Bool, "glyph view: show metric lines", false, {}, true},
              {"cell", ParamType::Int, "grid: cell size in px", false, {}, std::int64_t{96}},
              {"columns", ParamType::Int, "grid: glyphs per row", false, {}, std::int64_t{10}},
              {"max_glyphs", ParamType::Int, "grid: at most this many glyphs", false, {}, std::int64_t{160}}},
             false, true},
            fontCommand(
                session,
                [](FontDoc& doc, const Params& p) {
                    const Font& f = doc.font;
                    Image img;
                    std::string what;
                    if (has(p, "glyph")) {
                        std::string error;
                        const int i = requireGlyph(f, stringParam(p, "glyph"), error);
                        if (i < 0) return Result::failure(error);
                        GlyphViewOptions o;
                        o.size = static_cast<int>(numberParam(p, "size", 512));
                        o.points = boolParam(p, "points", true);
                        o.metrics = boolParam(p, "metrics", true);
                        img = glyphView(f, f.glyphs[static_cast<std::size_t>(i)], o);
                        what = "glyph " + f.glyphs[static_cast<std::size_t>(i)].name;
                    } else if (has(p, "text")) {
                        img = renderText(f, stringParam(p, "text"), styleFrom(p, 72), {255, 255, 255, 255}, 16, 2400);
                        what = "text";
                        const std::string missing = missingChars(f, stringParam(p, "text"));
                        if (!missing.empty()) what += " (missing: " + missing + ")";
                    } else {
                        img = overview(f, static_cast<int>(intParam(p, "cell", 96)), static_cast<int>(intParam(p, "columns", 10)),
                                       static_cast<int>(intParam(p, "max_glyphs", 160)));
                        what = "overview";
                    }
                    Result r = Result::success("preview of " + what + ": " + std::to_string(img.width()) + "x" + std::to_string(img.height()));
                    r.attach(png(img, "preview.png"));
                    return r.with("width", static_cast<std::int64_t>(img.width())).with("height", static_cast<std::int64_t>(img.height()));
                },
                false));

    reg.add({"type.render", "Render Text",
             "Set text in the font and write it as an image (PNG, JPEG, BMP, TGA): one line or several (\\n), "
             "optionally wrapped at max_width px, with kerning and tracking. glyph= renders one glyph's proof view "
             "instead (points and metrics).",
             {docOrFileParam(), fontFileParam(),
              {"path", ParamType::String, "output image file", true},
              {"text", ParamType::String, "text to set", false},
              {"glyph", ParamType::String, "render this glyph's view instead of text", false},
              {"size", ParamType::Number, "text size in px per em (glyph view: image size)", false, {}, 72.0},
              {"color", ParamType::String, "text colour", false, {}, std::string("#000000")},
              {"background", ParamType::String, "background colour (transparent works for PNG)", false, {}, std::string("#ffffff")},
              {"max_width", ParamType::Number, "wrap lines at this width in px", false},
              {"padding", ParamType::Int, "margin around the text in px", false, {}, std::int64_t{16}},
              {"kerning", ParamType::Bool, "apply kerning", false, {}, true},
              {"tracking", ParamType::Number, "extra space between letters in font units", false, {}, 0.0},
              {"points", ParamType::Bool, "glyph view: show points", false, {}, true},
              previewParam()},
             false},
            fontCommand(
                session,
                [](FontDoc& doc, const Params& p) {
                    const Font& f = doc.font;
                    const std::string path = stringParam(p, "path");
                    Image img;
                    std::string missing;
                    if (has(p, "glyph")) {
                        std::string error;
                        const int i = requireGlyph(f, stringParam(p, "glyph"), error);
                        if (i < 0) return Result::failure(error);
                        GlyphViewOptions o;
                        o.size = static_cast<int>(numberParam(p, "size", 512) < 64 ? 512 : numberParam(p, "size", 512));
                        o.points = boolParam(p, "points", true);
                        img = glyphView(f, f.glyphs[static_cast<std::size_t>(i)], o);
                    } else {
                        if (!has(p, "text")) return Result::failure("give text= (or glyph=)");
                        TextStyle st = styleFrom(p, 72);
                        std::string error;
                        Color bg;
                        if (!colorParam(p, "color", {0, 0, 0, 255}, st.color, error) || !colorParam(p, "background", {255, 255, 255, 255}, bg, error))
                            return Result::failure(error);
                        std::string text = stringParam(p, "text");
                        for (std::size_t k; (k = text.find("\\n")) != std::string::npos;) text.replace(k, 2, "\n");
                        img = renderText(f, text, st, bg, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "padding", 16), 0, 1000)),
                                         numberParam(p, "max_width", 0.0));
                        missing = missingChars(f, text);
                    }
                    raster::save(img, fs::path(path));
                    Result r = Result::success("rendered " + std::to_string(img.width()) + "x" + std::to_string(img.height()) + " to " + path +
                                               (missing.empty() ? "" : " (missing glyphs drawn as .notdef: " + missing + ")"));
                    if (boolParam(p, "preview", false)) r.attach(png(img, "render.png"));
                    return r.with("path", path)
                        .with("width", static_cast<std::int64_t>(img.width()))
                        .with("height", static_cast<std::int64_t>(img.height()))
                        .with("missing", missing);
                },
                false));

    reg.add({"type.proof", "Proof Sheet",
             "Write a specimen / proof sheet set in the font itself: title and info, the family name large, the "
             "character set, a waterfall of sizes, paragraphs, the strongest kerning pairs with and without kerning, "
             "and a grid of every glyph with names and code points. .png gives one long image, .pdf A4 pages with "
             "vector outlines.",
             {docOrFileParam(), fontFileParam(),
              {"path", ParamType::String, "output .png or .pdf", true},
              {"title", ParamType::String, "heading (default: family and style)", false},
              {"sample", ParamType::String, "waterfall line", false, {}, std::string("The quick brown fox jumps over the lazy dog.")},
              {"paragraph", ParamType::String, "paragraph text (default: pangrams)", false},
              {"sizes", ParamType::NumberList, "waterfall sizes in px/pt", false},
              {"charset", ParamType::Bool, "show the character set", false, {}, true},
              {"grid", ParamType::Bool, "show the glyph grid", false, {}, true},
              {"kerning", ParamType::Bool, "show kerning pairs", false, {}, true},
              {"width", ParamType::Number, "PNG width in px", false, {}, 1200.0},
              previewParam()},
             false},
            fontCommand(
                session,
                [](FontDoc& doc, const Params& p) {
                    const std::string path = stringParam(p, "path");
                    ProofOptions o;
                    o.title = stringParam(p, "title");
                    o.sample = stringParam(p, "sample", o.sample);
                    o.paragraph = stringParam(p, "paragraph");
                    if (has(p, "sizes")) o.sizes = listParam(p, "sizes");
                    o.charset = boolParam(p, "charset", true);
                    o.grid = boolParam(p, "grid", true);
                    o.kerning = boolParam(p, "kerning", true);
                    o.width = numberParam(p, "width", 1200.0);
                    std::string ext = fs::path(path).extension().string();
                    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    Result r;
                    if (ext == ".pdf") {
                        const std::string pdf = proofPdf(doc.font, o);
                        writeFile(path, pdf);
                        r = Result::success("wrote a PDF proof of " + doc.font.familyStyle() + " to " + path + " (" + std::to_string(pdf.size()) + " bytes)");
                        r.with("bytes", static_cast<std::int64_t>(pdf.size()));
                        if (boolParam(p, "preview", false)) {
                            ProofOptions small = o;
                            small.width = 900;
                            r.attach(Attachment{"image/png", raster::previewPng(proofImage(doc.font, small), 1024), "proof.png"});
                        }
                    } else {
                        const Image img = proofImage(doc.font, o);
                        raster::save(img, fs::path(path));
                        r = Result::success("wrote a " + std::to_string(img.width()) + "x" + std::to_string(img.height()) + " proof of " +
                                            doc.font.familyStyle() + " to " + path);
                        r.with("width", static_cast<std::int64_t>(img.width())).with("height", static_cast<std::int64_t>(img.height()));
                        if (boolParam(p, "preview", false)) r.attach(Attachment{"image/png", raster::previewPng(img, 1400), "proof.png"});
                    }
                    return r.with("path", path);
                },
                false));

    reg.add({"type.measure", "Measure Text",
             "Measure a line of text set in the font: advance in font units and in px at size=, glyph count and "
             "which characters the font is missing.",
             {docOrFileParam(), fontFileParam(),
              {"text", ParamType::String, "text to measure", true},
              {"size", ParamType::Number, "size in px per em", false, {}, 16.0},
              {"kerning", ParamType::Bool, "apply kerning", false, {}, true},
              {"tracking", ParamType::Number, "extra units between letters", false, {}, 0.0}},
             false, true},
            fontCommand(
                session,
                [](FontDoc& doc, const Params& p) {
                    const Font& f = doc.font;
                    const std::string text = stringParam(p, "text");
                    const TextStyle st = styleFrom(p, 16);
                    const double units = textAdvance(f, text, st);
                    const double px = units * st.size / f.metrics.unitsPerEm;
                    const std::string missing = missingChars(f, text);
                    return Result::success("\"" + text + "\" is " + fmt(units) + " units wide (" + fmt(std::round(px * 100) / 100) + " px at " +
                                           fmt(st.size) + " px)" + (missing.empty() ? "" : "; missing: " + missing))
                        .with("units", units)
                        .with("px", px)
                        .with("glyphs", static_cast<std::int64_t>(glyphRun(f, text).size()))
                        .with("missing", missing);
                },
                false));
}

}  // namespace achclwss::typeface::cmd
