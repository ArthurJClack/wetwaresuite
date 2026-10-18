// Shared command helpers and registerCommands.
#include "cmdutil.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <stdexcept>

#include "achclwss/raster/codec.hpp"

namespace achclwss::typeface {

namespace cmd {

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string plural(std::size_t n, const std::string& word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); }

std::vector<std::string> splitList(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + sep) {
        if (c == sep) {
            const auto a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
            if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
            else if (!item.empty()) out.push_back(" ");
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open font, e.g. font1", true}; }
ParamSpec docOrFileParam() { return {"doc", ParamType::String, "id of an open font, e.g. font1 (or font= a file)", false}; }
ParamSpec fontFileParam() {
    return {"font", ParamType::String, "a font file (.ttf, .otf, .woff, .typeface.json, .ufo) to read instead of doc=, for one-shot use", false};
}
ParamSpec previewParam() {
    return {"preview", ParamType::Bool, "attach a PNG: the glyph that changed (with points and metrics) or the glyph overview", false, {}, false};
}
ParamSpec glyphParam(const std::string& what) { return {"glyph", ParamType::String, what, true}; }
ParamSpec glyphsParam(const std::string& what) { return {"glyphs", ParamType::String, what, false}; }

int requireGlyph(const Font& font, const std::string& ref, std::string& error) {
    const int i = font.resolve(ref);
    if (i < 0) error = "no glyph '" + ref + "' in the font (type.info glyphs=true lists them)";
    return i;
}

bool glyphList(const Font& font, const std::string& spec, std::vector<int>& out, std::string& error) {
    out.clear();
    auto addRange = [&](std::uint32_t a, std::uint32_t b) {
        for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
            for (auto u : font.glyphs[i].unicodes) {
                if (u >= a && u <= b) {
                    out.push_back(static_cast<int>(i));
                    break;
                }
            }
        }
    };
    if (spec.empty() || spec == "all" || spec == "*") {
        for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
            if (font.glyphs[i].name != ".notdef") out.push_back(static_cast<int>(i));
        }
        return true;
    }
    for (const auto& tok : splitList(spec)) {
        if (tok == "uppercase") {
            addRange('A', 'Z');
            continue;
        }
        if (tok == "lowercase") {
            addRange('a', 'z');
            continue;
        }
        if (tok == "digits") {
            addRange('0', '9');
            continue;
        }
        if (tok == "letters") {
            addRange('A', 'Z');
            addRange('a', 'z');
            continue;
        }
        if (tok == "ascii") {
            addRange(0x20, 0x7E);
            continue;
        }
        if (tok == "latin1") {
            addRange(0x20, 0x7E);
            addRange(0xA0, 0xFF);
            continue;
        }
        if (tok == "unencoded") {
            for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
                if (font.glyphs[i].unicodes.empty() && font.glyphs[i].name != ".notdef") out.push_back(static_cast<int>(i));
            }
            continue;
        }
        if (tok == "encoded") {
            for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
                if (!font.glyphs[i].unicodes.empty()) out.push_back(static_cast<int>(i));
            }
            continue;
        }
        if (font.resolve(tok) < 0) {
            const auto cps = utf8Decode(tok);
            if (cps.size() == 3 && cps[1] == '-' && cps[0] <= cps[2]) {
                addRange(cps[0], cps[2]);
                continue;
            }
            // code point ranges: U+0100-U+017F, 0x400-0x4FF
            if (const auto dash = tok.find('-', 1); dash != std::string::npos && dash + 1 < tok.size()) {
                const auto a = parseUnicode(tok.substr(0, dash)), b = parseUnicode(tok.substr(dash + 1));
                if (a && b && *a <= *b) {
                    addRange(*a, *b);
                    continue;
                }
            }
        }
        const int i = font.resolve(tok == " " ? "space" : tok);
        if (i < 0) {
            error = "no glyph '" + tok + "' in the font";
            return false;
        }
        out.push_back(i);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) {
        error = "'" + spec + "' matches no glyphs in the font";
        return false;
    }
    return true;
}

bool parseUnicodeList(const std::string& text, std::vector<std::uint32_t>& out, std::string& error) {
    out.clear();
    if (text == "none" || text.empty()) return true;
    const auto items = text == "," ? std::vector<std::string>{","} : splitList(text);
    for (const auto& item : items) {
        auto cp = parseUnicode(item);
        if (!cp) {
            // a bare hex number such as 41 or 00E9
            bool hex = item.size() >= 2 && item.size() <= 6 && std::all_of(item.begin(), item.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
            if (hex) cp = static_cast<std::uint32_t>(std::stoul(item, nullptr, 16));
        }
        if (!cp || *cp > 0x10FFFF) {
            error = "'" + item + "' isn't a character or code point (use A, U+0041 or 0x41)";
            return false;
        }
        out.push_back(*cp);
    }
    return true;
}

bool newGlyphIdentity(const Font& font, const std::string& nameOrChar, const std::string& unicode, std::string& name,
                      std::vector<std::uint32_t>& unicodes, std::string& error) {
    unicodes.clear();
    name = nameOrChar;
    if (!unicode.empty() && !parseUnicodeList(unicode, unicodes, error)) return false;
    const auto cps = utf8Decode(nameOrChar);
    if (cps.size() == 1) {
        // a character: its standard name, and the code point unless one was given
        name = glyphNameFor(cps[0]);
        if (unicode.empty()) unicodes = {cps[0]};
    } else if (nameOrChar.rfind("U+", 0) == 0 || nameOrChar.rfind("u+", 0) == 0) {
        auto cp = parseUnicode(nameOrChar);
        if (!cp) {
            error = "'" + nameOrChar + "' isn't a code point";
            return false;
        }
        name = glyphNameFor(*cp);
        if (unicode.empty()) unicodes = {*cp};
    } else if (name.empty()) {
        if (unicodes.empty()) {
            error = "give the glyph a name= or unicode=";
            return false;
        }
        name = glyphNameFor(unicodes[0]);
    } else if (unicode.empty()) {
        if (auto cp = unicodeForName(name)) unicodes = {*cp};
    }
    if (!validGlyphName(name)) {
        error = "'" + name + "' isn't a usable glyph name (ASCII letters, digits, . _ - , up to 63 characters)";
        return false;
    }
    (void)font;
    return true;
}

Attachment png(const Image& img, const std::string& name) { return Attachment{"image/png", raster::encodePng(img), name}; }

Result& maybePreview(Result& r, const FontDoc& doc, const Params& p) {
    if (!r.ok || !boolParam(p, "preview", false) || !r.attachments.empty()) return r;
    if (const Value* v = r.field("glyph")) {
        if (auto s = std::get_if<std::string>(v)) {
            if (const Glyph* g = doc.font.glyph(*s)) {
                r.attach(png(glyphView(doc.font, *g), "glyph.png"));
                return r;
            }
        }
    }
    r.attach(*doc.preview());
    return r;
}

ActionFactory fontCommand(Session& session, Edit edit, bool undoable) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<FontDoc>(id);
        if (!doc && id.empty() && has(p, "font")) {
            // one-shot use: read the file into a font that lives only for this command
            const std::string path = stringParam(p, "font");
            std::vector<std::string> warnings;
            try {
                if (!std::filesystem::exists(std::filesystem::path(path))) throw std::runtime_error("no such file: " + path);
                doc = std::make_shared<FontDoc>();
                doc->font = loadFont(std::filesystem::path(path), &warnings);
                doc->path = path;
            } catch (const std::exception& e) {
                const std::string message = e.what();
                return std::make_unique<FunctionAction>([message] { return Result::failure(message); });
            }
        }
        if (!doc) {
            if (id.empty())
                return std::make_unique<FunctionAction>([] { return Result::failure("give doc= (an open font) or font= (a font file)"); });
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open font called '" + id + "' (use type.new or type.open first)"); });
        }
        auto run = [edit](FontDoc& d, const Params& q) {
            Result r = edit(d, q);
            if (r.ok && !r.field("doc") && has(q, "doc")) r.data.insert(r.data.begin(), {"doc", stringParam(q, "doc")});
            return maybePreview(r, d, q);
        };
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, run, p] {
                try {
                    return run(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<FontEdit>(doc, [run, p](FontDoc& d) { return run(d, p); });
    };
}

Result created(Session& session, std::shared_ptr<FontDoc> doc, const std::string& verb, const Params& p,
               const std::vector<std::string>& warnings) {
    const std::string id = session.workspace().add(doc, "font");
    Result r = Result::success(verb + " " + id + ": " + doc->summary(), id);
    for (const auto& w : warnings) r.message += "\nwarning: " + w;
    const auto& m = doc->font.metrics;
    r.with("doc", id)
        .with("family", doc->font.info.family)
        .with("style", doc->font.info.style)
        .with("glyphs", static_cast<std::int64_t>(doc->font.glyphs.size()))
        .with("kerning_pairs", static_cast<std::int64_t>(doc->font.kerning.size()))
        .with("units_per_em", static_cast<std::int64_t>(m.unitsPerEm))
        .with("ascender", m.ascender)
        .with("descender", m.descender)
        .with("x_height", m.xHeight)
        .with("cap_height", m.capHeight);
    if (!warnings.empty()) r.with("warnings", static_cast<std::int64_t>(warnings.size()));
    return maybePreview(r, *doc, p);
}

std::string glyphLine(const Font& font, const Glyph& g) {
    std::string s = g.name;
    for (auto u : g.unicodes) s += " " + unicodeLabel(u);
    s += " advance " + fmt(g.advance);
    const Box b = glyphBounds(font, g);
    // bearings to a tenth of a unit: curve extremes rarely fall on whole units
    auto tenth = [](double v) { return fmt(std::round(v * 10) / 10); };
    if (!b.empty) s += ", lsb " + tenth(b.x0) + ", rsb " + tenth(g.advance - b.x1);
    std::size_t pts = 0;
    for (const auto& c : g.contours) pts += c.points.size();
    s += ", " + plural(g.contours.size(), "contour") + " (" + plural(pts, "point") + ")";
    if (!g.components.empty()) {
        s += ", components:";
        for (const auto& c : g.components) s += " " + c.base + "@" + fmt(c.dx) + "," + fmt(c.dy);
    }
    return s;
}

}  // namespace cmd

void registerCommands(Session& session) {
    cmd::registerFileCommands(session);
    cmd::registerGlyphCommands(session);
    cmd::registerShapeCommands(session);
    cmd::registerSpacingCommands(session);
    cmd::registerOutputCommands(session);
    cmd::registerTraceCommands(session);
}

}  // namespace achclwss::typeface
