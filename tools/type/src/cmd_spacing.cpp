// Spacing and kerning commands.
#include <algorithm>
#include <cmath>
#include <sstream>

#include "cmdutil.hpp"

namespace achclwss::typeface::cmd {

void registerSpacingCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"type.metrics_auto", "Auto Metrics",
             "Space glyphs automatically: give every glyph (or glyphs=) the same left and right side bearings "
             "(spacing=, or lsb= / rsb=; default 6% of the em, round letters a little less), and with vertical=true "
             "read x-height, cap height, ascender and descender off x, H, d and p. Empty glyphs (space) keep their "
             "advance.",
             {docParam(),
              glyphsParam(),
              {"spacing", ParamType::Number, "both side bearings in font units (default 6% of the em)", false},
              {"lsb", ParamType::Number, "left side bearing (overrides spacing)", false},
              {"rsb", ParamType::Number, "right side bearing (overrides spacing)", false},
              {"round_factor", ParamType::Number, "side bearings of round shapes (o, c, e, O, C, ...) times this", false, {}, 0.8},
              {"sidebearings", ParamType::Bool, "change side bearings", false, {}, true},
              {"vertical", ParamType::Bool, "read vertical metrics from the glyph shapes", false, {}, true},
              previewParam()},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::string msg;
                int spaced = 0;
                if (boolParam(p, "sidebearings", true)) {
                    std::vector<int> list;
                    std::string error;
                    if (!glyphList(f, stringParam(p, "glyphs"), list, error)) return Result::failure(error);
                    const double sp = numberParam(p, "spacing", std::round(f.metrics.unitsPerEm * 0.06));
                    const double l = numberParam(p, "lsb", sp), r = numberParam(p, "rsb", sp);
                    const double rf = numberParam(p, "round_factor", 0.8);
                    static const std::string rounds = "oceOCGQ0369@";
                    // plain glyphs first, so composites are measured with their bases already spaced
                    std::stable_partition(list.begin(), list.end(), [&](int i) { return f.glyphs[static_cast<std::size_t>(i)].components.empty(); });
                    for (int i : list) {
                        Glyph& g = f.glyphs[static_cast<std::size_t>(i)];
                        if (glyphBounds(f, g).empty) continue;
                        bool round = false;
                        // a composite (Oacute) is as round as its first base (O)
                        const Glyph* shape = &g;
                        for (int depth = 0; depth < 4 && shape->contours.empty() && !shape->components.empty(); ++depth) {
                            const Glyph* b = f.glyph(shape->components.front().base);
                            if (!b) break;
                            shape = b;
                        }
                        for (auto u : shape->unicodes) round |= u < 0x80 && rounds.find(static_cast<char>(u)) != std::string::npos;
                        setSidebearings(f, g, std::round(round ? l * rf : l), std::round(round ? r * rf : r));
                        ++spaced;
                    }
                    msg = "spaced " + plural(static_cast<std::size_t>(spaced), "glyph") + " (lsb " + fmt(l) + ", rsb " + fmt(r) + ")";
                }
                std::string vertical;
                if (boolParam(p, "vertical", true)) {
                    for (const auto& c : detectVerticalMetrics(f)) vertical += (vertical.empty() ? "" : ", ") + c;
                    msg += std::string(msg.empty() ? "" : "; ") + (vertical.empty() ? "no x/H/d/p glyphs to read vertical metrics from" : "set " + vertical);
                }
                return Result::success(msg).with("spaced", static_cast<std::int64_t>(spaced)).with("vertical", vertical);
            }));

    reg.add({"type.kern_set", "Set Kerning",
             "Set kerning pairs in font units (negative pulls the two glyphs together; 0 removes the pair). One pair "
             "with left= right= value=, or many with pairs=\"A V -80; T o -60; L T -90\" (glyph names or characters).",
             {docParam(),
              {"left", ParamType::String, "left glyph", false},
              {"right", ParamType::String, "right glyph", false},
              {"value", ParamType::Number, "kerning value", false},
              {"pairs", ParamType::String, "several pairs: \"left right value; ...\"", false}},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::vector<std::tuple<std::string, std::string, double>> todo;
                if (has(p, "left") || has(p, "right")) {
                    if (!has(p, "left") || !has(p, "right") || !has(p, "value")) return Result::failure("give left=, right= and value=");
                    todo.emplace_back(stringParam(p, "left"), stringParam(p, "right"), numberParam(p, "value"));
                }
                if (has(p, "pairs")) {
                    for (const auto& item : splitList(stringParam(p, "pairs"), ';')) {
                        std::istringstream in(item);
                        std::string a, b, v;
                        in >> a >> b >> v;
                        if (v.empty()) return Result::failure("bad pair '" + item + "': want \"left right value\"");
                        char* end = nullptr;
                        const double value = std::strtod(v.c_str(), &end);
                        if (!end || *end) return Result::failure("bad kerning value '" + v + "'");
                        todo.emplace_back(a, b, value);
                    }
                }
                if (todo.empty()) return Result::failure("give left= right= value=, or pairs=");
                int set = 0, removed = 0;
                for (const auto& [a, b, v] : todo) {
                    std::string error;
                    const int ia = requireGlyph(f, a, error), ib = ia < 0 ? -1 : requireGlyph(f, b, error);
                    if (ia < 0 || ib < 0) return Result::failure(error);
                    const KernPair key{f.glyphs[static_cast<std::size_t>(ia)].name, f.glyphs[static_cast<std::size_t>(ib)].name};
                    if (v == 0) removed += f.kerning.erase(key) ? 1 : 0;
                    else {
                        f.kerning[key] = v;
                        ++set;
                    }
                }
                return Result::success("set " + plural(static_cast<std::size_t>(set), "kerning pair") +
                                       (removed ? ", removed " + std::to_string(removed) : ""))
                    .with("set", static_cast<std::int64_t>(set))
                    .with("removed", static_cast<std::int64_t>(removed))
                    .with("kerning_pairs", static_cast<std::int64_t>(f.kerning.size()));
            }));

    reg.add({"type.kern_auto", "Auto Kern",
             "Kern automatically (lite): compares the right profile of each left glyph with the left profile of each "
             "right glyph in horizontal bands and evens out the optical gap to the one between two H's (or target=). "
             "Default glyphs: the letters plus . , - ' \" present in the font. Existing pairs are kept unless "
             "replace=true. Check the result with type.proof or type.kern_list and fix pairs with type.kern_set.",
             {docParam(),
              {"glyphs", ParamType::String, "glyphs to kern among (default: letters and common punctuation)", false},
              {"strength", ParamType::Number, "how much of the measured difference to apply (0..1.5)", false, {}, 0.8},
              {"target", ParamType::Number, "wanted gap in font units (default: measured from HH)", false},
              {"min", ParamType::Number, "smallest kern to keep (default 1.5% of the em)", false},
              {"max", ParamType::Number, "largest kern allowed (default 30% of the em)", false},
              {"replace", ParamType::Bool, "clear existing kerning first", false, {}, false}},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::vector<int> list;
                std::string error;
                std::string spec = stringParam(p, "glyphs");
                if (spec.empty()) {
                    for (std::uint32_t c : {'A', 'a'}) {
                        for (std::uint32_t k = 0; k < 26; ++k) {
                            if (int i = f.indexForUnicode(c + k); i >= 0) list.push_back(i);
                        }
                    }
                    for (std::uint32_t c : {'.', ',', '-', '\'', '"'}) {
                        if (int i = f.indexForUnicode(c); i >= 0) list.push_back(i);
                    }
                    if (list.empty()) return Result::failure("the font has no letters to kern; give glyphs=");
                } else if (!glyphList(f, spec, list, error)) {
                    return Result::failure(error);
                }
                std::vector<std::string> names;
                for (int i : list) names.push_back(f.glyphs[static_cast<std::size_t>(i)].name);
                AutoKernOptions o;
                o.strength = std::clamp(numberParam(p, "strength", 0.8), 0.0, 1.5);
                o.target = numberParam(p, "target", 0.0);
                o.minimum = numberParam(p, "min", 0.0);
                o.maximum = numberParam(p, "max", 0.0);
                double target = 0;
                const auto pairs = autoKern(f, names, o, &target);
                if (boolParam(p, "replace", false)) f.kerning.clear();
                int added = 0;
                for (const auto& [k, v] : pairs) {
                    if (f.kerning.emplace(k, v).second) ++added;
                }
                // the strongest few, for the message
                std::vector<std::pair<double, KernPair>> top;
                for (const auto& [k, v] : pairs) top.push_back({-std::abs(v), k});
                std::sort(top.begin(), top.end());
                std::string sample;
                for (std::size_t i = 0; i < top.size() && i < 8; ++i)
                    sample += (sample.empty() ? "" : ", ") + top[i].second.first + "/" + top[i].second.second + " " + fmt(pairs.at(top[i].second));
                return Result::success("auto-kerned " + plural(names.size(), "glyph") + ": " + std::to_string(pairs.size()) + " pairs found, " +
                                       std::to_string(added) + " added (target gap " + fmt(std::round(target)) + ")" +
                                       (sample.empty() ? "" : "; strongest: " + sample))
                    .with("found", static_cast<std::int64_t>(pairs.size()))
                    .with("added", static_cast<std::int64_t>(added))
                    .with("target", std::round(target))
                    .with("kerning_pairs", static_cast<std::int64_t>(f.kerning.size()));
            }));

    reg.add({"type.kern_list", "List Kerning",
             "List kerning pairs (strongest first), optionally only those with one glyph.",
             {docOrFileParam(), fontFileParam(), {"glyph", ParamType::String, "only pairs with this glyph", false},
              {"limit", ParamType::Int, "at most this many lines", false, {}, std::int64_t{100}}},
             false, true},
            fontCommand(
                session,
                [](FontDoc& doc, const Params& p) {
                    const Font& f = doc.font;
                    std::string only;
                    if (has(p, "glyph")) {
                        std::string error;
                        const int i = requireGlyph(f, stringParam(p, "glyph"), error);
                        if (i < 0) return Result::failure(error);
                        only = f.glyphs[static_cast<std::size_t>(i)].name;
                    }
                    std::vector<std::pair<double, KernPair>> rows;
                    for (const auto& [k, v] : f.kerning) {
                        if (only.empty() || k.first == only || k.second == only) rows.push_back({-std::abs(v), k});
                    }
                    std::sort(rows.begin(), rows.end());
                    const std::size_t limit = static_cast<std::size_t>(std::max<std::int64_t>(1, intParam(p, "limit", 100)));
                    std::string s = plural(rows.size(), "kerning pair") + (only.empty() ? "" : " with " + only);
                    for (std::size_t i = 0; i < rows.size() && i < limit; ++i)
                        s += "\n  " + rows[i].second.first + " " + rows[i].second.second + " " + fmt(f.kerning.at(rows[i].second));
                    if (rows.size() > limit) s += "\n  ... " + std::to_string(rows.size() - limit) + " more";
                    return Result::success(s).with("pairs", static_cast<std::int64_t>(rows.size()));
                },
                false));

    reg.add({"type.kern_clear", "Clear Kerning",
             "Remove all kerning, or only the pairs with one glyph.",
             {docParam(), {"glyph", ParamType::String, "only pairs with this glyph", false}},
             true},
            fontCommand(session, [](FontDoc& doc, const Params& p) {
                Font& f = doc.font;
                std::size_t n = 0;
                if (has(p, "glyph")) {
                    std::string error;
                    const int i = requireGlyph(f, stringParam(p, "glyph"), error);
                    if (i < 0) return Result::failure(error);
                    const std::string name = f.glyphs[static_cast<std::size_t>(i)].name;
                    for (auto it = f.kerning.begin(); it != f.kerning.end();) {
                        if (it->first.first == name || it->first.second == name) {
                            it = f.kerning.erase(it);
                            ++n;
                        } else {
                            ++it;
                        }
                    }
                } else {
                    n = f.kerning.size();
                    f.kerning.clear();
                }
                return Result::success("removed " + plural(n, "kerning pair")).with("removed", static_cast<std::int64_t>(n));
            }));
}

}  // namespace achclwss::typeface::cmd
