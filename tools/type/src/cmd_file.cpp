// type.new / open / save / export / close / info / set_info / set_metrics.
#include <algorithm>
#include <cctype>

#include "cmdutil.hpp"

namespace wetwaresuite::typeface::cmd {

namespace {

namespace fs = std::filesystem;

std::string lowerExt(const fs::path& p) {
    std::string name = p.filename().string();
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name.size() > 14 && name.compare(name.size() - 14, 14, ".typeface.json") == 0) return ".typeface.json";
    std::string ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

}  // namespace

void registerFileCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"type.new", "New Font",
             "Create an empty font and return its id (e.g. font1). It starts with .notdef (a box) and space. Then add "
             "glyphs with type.glyph_add / type.draw / type.import_svg, or trace a sheet of handwriting with "
             "type.trace_grid. Coordinates are font units, y up, baseline at 0.",
             {{"family", ParamType::String, "family name", false, {}, std::string("Untitled")},
              {"style", ParamType::String, "style name: Regular, Bold, Italic, Light, ...", false, {}, std::string("Regular")},
              {"units_per_em", ParamType::Int, "units per em (1000 for PostScript-style, 2048 for TrueType-style)", false, {}, std::int64_t{1000}},
              {"ascender", ParamType::Number, "ascender line (default 0.8 em)", false},
              {"descender", ParamType::Number, "descender line, negative (default -0.2 em)", false},
              {"x_height", ParamType::Number, "x-height (default 0.5 em)", false},
              {"cap_height", ParamType::Number, "cap height (default 0.7 em)", false},
              previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    const int upm = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "units_per_em", 1000), 16, 16384));
                    auto doc = std::make_shared<FontDoc>();
                    doc->font = newFont(stringParam(p, "family", "Untitled"), stringParam(p, "style", "Regular"), upm,
                                        numberParam(p, "ascender", upm * 0.8), numberParam(p, "descender", -upm * 0.2),
                                        numberParam(p, "x_height", upm * 0.5), numberParam(p, "cap_height", upm * 0.7));
                    if (doc->font.metrics.descender > 0) doc->font.metrics.descender = -doc->font.metrics.descender;
                    if (doc->font.info.style == "Bold") doc->font.info.weight = 700;
                    return created(session, doc, "created", p);
                });
            });

    reg.add({"type.open", "Open Font",
             "Open a font: .typeface.json (this tool's own format, keeps everything), .ttf/.otf (TrueType glyf or "
             "OpenType CFF outlines, composite glyphs as components, kern table and GPOS pair kerning), .ttc (first "
             "font), .woff (WOFF 1.0), or a .ufo folder (UFO 2/3: fontinfo, glyphs, kerning with groups). Hinting is "
             "dropped; warnings say what else was skipped.",
             {{"path", ParamType::String, "font file or .ufo folder", true}, previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        const fs::path path = fs::path(stringParam(p, "path"));
                        if (!fs::exists(path)) return Result::failure("no such file: " + path.string());
                        std::vector<std::string> warnings;
                        auto doc = std::make_shared<FontDoc>();
                        doc->font = loadFont(path, &warnings);
                        doc->path = path.string();
                        return created(session, doc, "opened " + path.string() + " as", p, warnings);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"type.save", "Save Font",
             "Save the font source: .typeface.json (or .json; everything, reopen with type.open) or a .ufo folder "
             "(UFO 3, for Glyphs, FontForge, RoboFont, fontmake). To make a usable font file use type.export.",
             {docParam(),
              {"path", ParamType::String, "output .typeface.json file or .ufo folder", true},
              {"format", ParamType::String, "override the extension", false, {"auto", "json", "ufo"}, std::string("auto")}},
             false},
            fontCommand(
                session,
                [](FontDoc& doc, const Params& p) {
                    const fs::path path = fs::path(stringParam(p, "path"));
                    std::string format = stringParam(p, "format", "auto");
                    const std::string ext = lowerExt(path);
                    if (format == "auto") {
                        if (ext == ".ufo") format = "ufo";
                        else if (ext == ".json" || ext == ".typeface.json") format = "json";
                        else if (ext == ".ttf" || ext == ".otf" || ext == ".woff" || ext == ".svg")
                            return Result::failure("use type.export for " + ext + " files; type.save writes .typeface.json or .ufo");
                        else return Result::failure("unknown extension on " + path.string() + " (use .typeface.json or .ufo)");
                    }
                    if (format == "ufo") saveUfo(doc.font, path);
                    else writeFile(path.string(), toJson(doc.font));
                    doc.path = path.string();
                    return Result::success("saved " + doc.font.familyStyle() + " to " + path.string())
                        .with("path", path.string())
                        .with("format", format)
                        .with("glyphs", static_cast<std::int64_t>(doc.font.glyphs.size()));
                },
                false));

    reg.add({"type.export", "Export Font",
             "Build a font file: .ttf (TrueType: head, hhea, maxp, OS/2, name, cmap formats 4 and 12, loca, glyf, hmtx, "
             "post, plus kerning as a kern table and a GPOS pair-adjustment lookup; cubic curves are converted to "
             "quadratics), .woff (the same, zlib-compressed for the web), or .svg (an SVG font). Install the .ttf or "
             "use the .woff in CSS @font-face.",
             {docOrFileParam(), fontFileParam(),
              {"path", ParamType::String, "output .ttf, .woff or .svg", true},
              {"format", ParamType::String, "override the extension", false, {"auto", "ttf", "woff", "svg"}, std::string("auto")},
              {"kerning", ParamType::String, "which kerning tables to write", false, {"both", "gpos", "kern", "none"}, std::string("both")},
              {"tolerance", ParamType::Number, "cubic-to-quadratic error in font units", false, {}, 0.5}},
             false},
            fontCommand(
                session,
                [](FontDoc& doc, const Params& p) {
                    const fs::path path = fs::path(stringParam(p, "path"));
                    std::string format = stringParam(p, "format", "auto");
                    if (format == "auto") {
                        const std::string ext = lowerExt(path);
                        format = ext == ".ttf" ? "ttf" : ext == ".woff" ? "woff" : ext == ".svg" ? "svg" : "";
                        if (ext == ".otf") return Result::failure(".otf with CFF outlines isn't written; export .ttf (TrueType outlines)");
                        if (format.empty()) return Result::failure("unknown extension on " + path.string() + " (use .ttf, .woff or .svg)");
                    }
                    std::vector<std::string> warnings;
                    TtfOptions o;
                    o.kerning = stringParam(p, "kerning", "both");
                    o.tolerance = std::max(0.05, numberParam(p, "tolerance", 0.5));
                    o.warnings = &warnings;
                    std::string bytes;
                    if (format == "svg") bytes = toSvgFont(doc.font);
                    else bytes = toTtf(doc.font, o);
                    if (format == "woff") bytes = toWoff(bytes);
                    writeFile(path.string(), bytes);
                    Result r = Result::success("exported " + doc.font.familyStyle() + " to " + path.string() + " (" +
                                               std::to_string(bytes.size()) + " bytes, " + plural(doc.font.glyphs.size(), "glyph") + ")");
                    for (const auto& w : warnings) r.message += "\nwarning: " + w;
                    return r.with("path", path.string())
                        .with("format", format)
                        .with("bytes", static_cast<std::int64_t>(bytes.size()))
                        .with("glyphs", static_cast<std::int64_t>(doc.font.glyphs.size()));
                },
                false));

    reg.add({"type.close", "Close Font", "Close an open font (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<CloseAction>(session, stringParam(p, "doc")); });

    reg.add({"type.info", "Font Info",
             "Describe a font: names, version, license, metrics, glyph and kerning counts, which ASCII characters are "
             "still missing, and with glyphs=true one line per glyph (unicodes, advance, side bearings, contours).",
             {docOrFileParam(), fontFileParam(), {"glyphs", ParamType::Bool, "list every glyph", false, {}, false}, previewParam()},
             false, true},
            fontCommand(
                session,
                [](FontDoc& doc, const Params& p) {
                    const Font& f = doc.font;
                    const auto& m = f.metrics;
                    std::string missing;
                    int missingCount = 0;
                    for (std::uint32_t c = 0x21; c < 0x7F; ++c) {
                        if (f.indexForUnicode(c) < 0) {
                            missing += utf8Encode(c);
                            ++missingCount;
                        }
                    }
                    std::string s = doc.summary();
                    s += "\nversion " + f.info.version + ", weight " + std::to_string(f.info.weight);
                    if (!f.info.designer.empty()) s += ", designer " + f.info.designer;
                    if (!f.info.license.empty()) s += "\nlicense: " + f.info.license;
                    if (!f.info.copyright.empty()) s += "\ncopyright: " + f.info.copyright;
                    s += "\nmetrics: ascender " + fmt(m.ascender) + ", descender " + fmt(m.descender) + ", x-height " + fmt(m.xHeight) +
                         ", cap height " + fmt(m.capHeight) + ", line gap " + fmt(m.lineGap);
                    if (m.italicAngle != 0) s += ", italic angle " + fmt(m.italicAngle);
                    s += missingCount ? "\nmissing ASCII (" + std::to_string(missingCount) + "): " + missing : "\nall printable ASCII is present";
                    if (boolParam(p, "glyphs", false)) {
                        for (const auto& g : f.glyphs) s += "\n  " + glyphLine(f, g);
                    }
                    Result r = Result::success(s);
                    r.with("family", f.info.family)
                        .with("style", f.info.style)
                        .with("version", f.info.version)
                        .with("glyphs", static_cast<std::int64_t>(f.glyphs.size()))
                        .with("kerning_pairs", static_cast<std::int64_t>(f.kerning.size()))
                        .with("units_per_em", static_cast<std::int64_t>(m.unitsPerEm))
                        .with("ascender", m.ascender)
                        .with("descender", m.descender)
                        .with("x_height", m.xHeight)
                        .with("cap_height", m.capHeight)
                        .with("missing_ascii", missing);
                    return r;
                },
                false));

    reg.add({"type.set_info", "Set Font Info",
             "Change the names and legal info written into exported fonts: family, style, version, copyright, license "
             "(e.g. SIL Open Font License 1.1), license URL, designer, manufacturer, description, vendor ID, weight class.",
             {docParam(),
              {"family", ParamType::String, "family name", false},
              {"style", ParamType::String, "style name (Regular, Bold, Italic, Bold Italic, or any other like Light)", false},
              {"version", ParamType::String, "version, e.g. 1.002", false},
              {"copyright", ParamType::String, "copyright notice", false},
              {"license", ParamType::String, "license text or name", false},
              {"license_url", ParamType::String, "license URL", false},
              {"designer", ParamType::String, "designer", false},
              {"manufacturer", ParamType::String, "manufacturer / foundry", false},
              {"description", ParamType::String, "description", false},
              {"vendor", ParamType::String, "four-letter vendor ID", false},
              {"weight", ParamType::Int, "weight class 100..900 (400 regular, 700 bold)", false}},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                auto& i = doc.font.info;
                std::vector<std::string> changed;
                auto set = [&](const char* key, std::string& field) {
                    if (has(p, key)) {
                        field = stringParam(p, key);
                        changed.push_back(key);
                    }
                };
                set("family", i.family);
                set("style", i.style);
                set("version", i.version);
                set("copyright", i.copyright);
                set("license", i.license);
                set("license_url", i.licenseUrl);
                set("designer", i.designer);
                set("manufacturer", i.manufacturer);
                set("description", i.description);
                set("vendor", i.vendor);
                if (has(p, "weight")) {
                    i.weight = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "weight"), 1, 1000));
                    changed.push_back("weight");
                }
                if (i.family.empty()) return Result::failure("the family name can't be empty");
                if (changed.empty()) return Result::failure("nothing to change: give family=, style=, license=, ...");
                std::string list;
                for (const auto& c : changed) list += (list.empty() ? "" : ", ") + c;
                return Result::success("set " + list + " on " + doc.font.familyStyle()).with("changed", list);
            }));

    reg.add({"type.set_metrics", "Set Font Metrics",
             "Change vertical metrics (ascender, descender, x-height, cap height, line gap, italic angle, underline) "
             "or units per em. Changing units_per_em scales every outline, advance and kerning value to match unless "
             "scale=false.",
             {docParam(),
              {"units_per_em", ParamType::Int, "new units per em", false},
              {"scale", ParamType::Bool, "with units_per_em: scale the outlines too", false, {}, true},
              {"ascender", ParamType::Number, "ascender", false},
              {"descender", ParamType::Number, "descender (negative)", false},
              {"x_height", ParamType::Number, "x-height", false},
              {"cap_height", ParamType::Number, "cap height", false},
              {"line_gap", ParamType::Number, "extra line spacing", false},
              {"italic_angle", ParamType::Number, "italic angle in degrees (negative leans right)", false},
              {"underline_position", ParamType::Number, "underline position (negative, below the baseline)", false},
              {"underline_thickness", ParamType::Number, "underline thickness", false},
              previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                auto& m = doc.font.metrics;
                std::vector<std::string> changed;
                if (has(p, "units_per_em")) {
                    const int upm = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "units_per_em"), 16, 16384));
                    if (boolParam(p, "scale", true)) scaleFont(doc.font, static_cast<double>(upm) / m.unitsPerEm);
                    m.unitsPerEm = upm;
                    changed.push_back("units_per_em=" + std::to_string(upm));
                }
                auto set = [&](const char* key, double& field) {
                    if (has(p, key)) {
                        field = numberParam(p, key);
                        changed.push_back(std::string(key) + "=" + fmt(field));
                    }
                };
                set("ascender", m.ascender);
                set("descender", m.descender);
                set("x_height", m.xHeight);
                set("cap_height", m.capHeight);
                set("line_gap", m.lineGap);
                set("italic_angle", m.italicAngle);
                set("underline_position", m.underlinePosition);
                set("underline_thickness", m.underlineThickness);
                if (changed.empty()) return Result::failure("nothing to change: give ascender=, x_height=, units_per_em=, ...");
                if (m.descender > 0) m.descender = -m.descender;
                std::string list;
                for (const auto& c : changed) list += (list.empty() ? "" : ", ") + c;
                return Result::success("set " + list).with("changed", list);
            }));
}

}  // namespace wetwaresuite::typeface::cmd
