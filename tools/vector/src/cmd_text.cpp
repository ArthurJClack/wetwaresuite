// Text, gradient, boolean and to-path commands for the vector tool.
#include <sstream>

#include "internal.hpp"

namespace wetwaresuite::vector::detail {

namespace {

// "\n" typed as two characters (as it arrives from a shell or a script) means a line break.
std::string unescapeLines(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size() && in[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else {
            out += in[i];
        }
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;  // keeps rgb(1,2,3) together
    for (char c : text) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c == ',' && depth <= 0) {
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

// Reads text layout params that are present into `s`.
bool applyText(Shape& s, const Params& p, std::string& error) {
    if (has(p, "text")) s.text = unescapeLines(stringParam(p, "text"));
    if (has(p, "x")) s.x = numberParam(p, "x");
    if (has(p, "y")) s.y = numberParam(p, "y");
    if (has(p, "size")) {
        double v = numberParam(p, "size");
        if (!(v > 0) || v > 10000) {
            error = "size must be between 0 and 10000 px";
            return false;
        }
        s.fontSize = v;
    }
    if (has(p, "font")) s.font = stringParam(p, "font");
    if (has(p, "bold")) s.bold = boolParam(p, "bold", false);
    if (has(p, "italic")) s.italic = boolParam(p, "italic", false);
    if (has(p, "anchor")) {
        std::string a = lower(stringParam(p, "anchor"));
        if (a == "left") a = "start";
        if (a == "center" || a == "centre") a = "middle";
        if (a == "right") a = "end";
        if (a != "start" && a != "middle" && a != "end") {
            error = "anchor must be start, middle or end";
            return false;
        }
        s.anchor = a;
    }
    if (has(p, "letter_spacing")) s.letterSpacing = numberParam(p, "letter_spacing");
    if (has(p, "line_height")) {
        double v = numberParam(p, "line_height");
        if (!(v > 0) || v > 20) {
            error = "line_height must be between 0 and 20 (times the size)";
            return false;
        }
        s.lineHeight = v;
    }
    if (has(p, "box_width")) s.boxWidth = std::max(0.0, numberParam(p, "box_width"));
    if (has(p, "box_height")) s.boxHeight = std::max(0.0, numberParam(p, "box_height"));
    if (has(p, "columns")) {
        const auto c = intParam(p, "columns", 1);
        if (c < 1 || c > 32) return error = "columns must be 1..32", false;
        s.columns = static_cast<int>(c);
    }
    if (has(p, "column_gap")) s.columnGap = std::max(0.0, numberParam(p, "column_gap"));
    if (has(p, "align")) {
        const std::string a = lower(stringParam(p, "align"));
        if (a != "left" && a != "center" && a != "right" && a != "justify") return error = "align must be left, center, right or justify", false;
        s.align = a;
    }
    if (has(p, "on_path")) s.onPath = stringParam(p, "on_path") == "none" ? std::string() : stringParam(p, "on_path");
    if (has(p, "path_offset")) s.pathOffset = numberParam(p, "path_offset");
    if (has(p, "case")) {
        const std::string c = lower(stringParam(p, "case"));
        if (c != "none" && c != "upper" && c != "lower" && c != "title" && c != "small_caps") {
            return error = "case must be none, upper, lower, title or small_caps", false;
        }
        s.textCase = c;
    }
    return true;
}

// The parts of text settings that need the document: the path it runs along and its text style.
bool finishText(VectorDoc& doc, Shape& s, const Params& p, std::string& error) {
    if (!s.onPath.empty()) {
        const int i = doc.find(s.onPath);
        if (i < 0) return error = "no shape '" + s.onPath + "' to run the text along", false;
        const Shape& path = doc.shapes[static_cast<std::size_t>(i)];
        if (path.kind == ShapeKind::Text || path.kind == ShapeKind::Group || path.kind == ShapeKind::Instance) {
            return error = "text runs along paths and shapes, not a " + std::string(toString(path.kind)), false;
        }
        s.onPath = path.id;
    }
    if (has(p, "style")) {
        const std::string name = stringParam(p, "style");
        if (name == "none") {
            s.textStyle.clear();
        } else {
            auto it = std::find_if(doc.textStyles.begin(), doc.textStyles.end(), [&](const TextStyle& t) { return t.name == name; });
            if (it == doc.textStyles.end()) return error = "no text style '" + name + "' (vector.text_style defines one)", false;
            applyTextStyle(s, *it);
            applyText(s, p, error);  // settings given alongside the style win
        }
    }
    return true;
}

std::vector<ParamSpec> textParams(bool adding) {
    return {
        {"text", ParamType::String, "the words; \\n starts a new line", adding},
        numParam("x", "x of the text's anchor point in px", adding),
        numParam("y", "y of the first line's baseline in px", adding),
        numParam("size", "font size in px", false, adding ? Value{48.0} : Value{}),
        {"font", ParamType::String,
         "sans (built in, same on every machine), serif, mono, a family like Arial, or a .ttf/.otf file", false, {},
         adding ? Value{std::string("sans")} : Value{}},
        {"bold", ParamType::Bool, "bold face (system fonts only)", false},
        {"italic", ParamType::Bool, "italic face (system fonts only)", false},
        {"anchor", ParamType::String, "which part of the line sits at x: start, middle or end", false, {},
         adding ? Value{std::string("start")} : Value{}},
        numParam("letter_spacing", "extra px between letters (negative tightens)", false),
        numParam("line_height", "line spacing, times the size", false),
        numParam("box_width", "area text: wrap inside a box this wide (x,y is then its top-left); 0 = point text", false),
        numParam("box_height", "area text: box height (text past it overflows, hidden)", false),
        {"columns", ParamType::Int, "area text: number of columns", false},
        numParam("column_gap", "area text: gap between columns in px", false),
        {"align", ParamType::String, "area text: left, center, right or justify", false},
        {"on_path", ParamType::String, "run the text along this shape's path (none = back to a straight line)", false},
        numParam("path_offset", "text on a path: start this many px along the path", false),
        {"case", ParamType::String, "none, upper, lower, title or small_caps", false},
        {"style", ParamType::String, "follow a text style (vector.text_style); none = stop following", false},
    };
}

bool gradientArg(const Params& p, Gradient& g, std::string& error) {
    const std::string type = lower(stringParam(p, "type", "linear"));
    if (type != "linear" && type != "radial") {
        error = "type must be linear or radial";
        return false;
    }
    g.radial = type == "radial";
    if (has(p, "stops")) {
        for (const auto& item : splitList(stringParam(p, "stops"))) {
            auto colon = item.find(':');
            if (colon == std::string::npos) {
                error = "stops are offset:colour pairs, e.g. 0:red,0.5:#ffffff,1:blue";
                return false;
            }
            GradientStop st;
            try {
                st.offset = std::clamp(std::stod(item.substr(0, colon)), 0.0, 1.0);
            } catch (...) {
                error = "'" + item.substr(0, colon) + "' isn't a stop offset (use 0..1)";
                return false;
            }
            auto c = parseColor(item.substr(colon + 1));
            if (!c) {
                error = "'" + item.substr(colon + 1) + "' isn't a colour";
                return false;
            }
            st.color = *c;
            g.stops.push_back(st);
        }
    } else {
        auto names = splitList(stringParam(p, "colors", ""));
        if (names.empty()) {
            error = "give colors (e.g. colors=red,blue) or stops (e.g. stops=0:red,1:blue)";
            return false;
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            auto c = parseColor(names[i]);
            if (!c) {
                error = "'" + names[i] + "' isn't a colour";
                return false;
            }
            double t = names.size() == 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(names.size() - 1);
            g.stops.push_back({t, *c});
        }
    }
    if (g.stops.empty()) {
        error = "a gradient needs at least one colour";
        return false;
    }
    std::stable_sort(g.stops.begin(), g.stops.end(), [](const GradientStop& a, const GradientStop& b) { return a.offset < b.offset; });
    if (g.radial) {
        g.cx = numberParam(p, "cx", 0.5);
        g.cy = numberParam(p, "cy", 0.5);
        g.r = numberParam(p, "radius", 0.5);
        if (!(g.r > 0)) {
            error = "radius must be above 0 (a fraction of the shape's size)";
            return false;
        }
    } else {
        const double a = numberParam(p, "angle", 0) * 3.14159265358979323846 / 180.0;
        const double dx = std::cos(a) / 2, dy = std::sin(a) / 2;
        g.x1 = 0.5 - dx;
        g.y1 = 0.5 - dy;
        g.x2 = 0.5 + dx;
        g.y2 = 0.5 + dy;
    }
    return true;
}

}  // namespace

void registerTextCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"vector.add_text", "Add Text",
             "Add text. The built-in font looks the same on every machine and covers Latin, Greek and Cyrillic; "
             "serif, mono, a family name or a .ttf/.otf file use that font. Text stays editable (vector.set_text) "
             "and saves as real SVG text; vector.to_path turns it into outlines.",
             join(join({docParam()}, textParams(true)), styleParams("black", "none", 1.0)), true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                Shape s;
                s.kind = ShapeKind::Text;
                s.stroke.reset();
                std::string error;
                if (!applyText(s, p, error) || !finishText(doc, s, p, error)) return Result::failure(error);
                if (s.text.empty()) return Result::failure("text can't be empty");
                if (!has(p, "fill") && (s.textStyle.empty() || !s.fill)) s.fill = Color{0, 0, 0, 255};
                return addShape(doc, std::move(s), p);
            }));

    reg.add({"vector.set_text", "Edit Text",
             "Change a text shape's words, position, font, size, alignment or spacing. Only the params you give change.",
             join({docParam(), shapeParam()}, join(textParams(false), {previewParam()})), true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                if (s.kind != ShapeKind::Text) return Result::failure(s.id + " is a " + toString(s.kind) + ", not text");
                std::string error;
                if (!applyText(s, p, error) || !finishText(doc, s, p, error)) return Result::failure(error);
                if (s.text.empty()) return Result::failure("text can't be empty");
                bool overflow = false;
                if (s.boxWidth > 0) layoutArea(s, &overflow);
                Result r = Result::success("updated " + describe(s) + (overflow ? " (the text overflows its box)" : ""), s.id);
                return r.with("shape", s.id).with("overflow", overflow);
            })));

    reg.add({"vector.gradient", "Gradient Fill",
             "Fill a shape (or its outline, target=stroke) with a linear or radial gradient: colors=red,blue "
             "spreads colours evenly, or stops=0:red,0.3:gold,1:blue places them. type=freeform blends colour "
             "points instead: points=\"0.2,0.2,red 0.8,0.3,gold 0.5,0.9,blue\" (fractions of the shape's box). "
             "Positions follow the shape when it moves or scales. remove=true goes back to the flat colour.",
             {docParam(), shapeParam(),
              {"type", ParamType::String, "linear, radial or freeform", false, {}, std::string("linear")},
              {"target", ParamType::String, "what the gradient paints", false, {"fill", "stroke"}, std::string("fill")},
              {"colors", ParamType::String, "colours spread evenly, e.g. red,blue or #ff0000,#ffd700,#0000ff", false},
              {"stops", ParamType::String, "offset:colour pairs, e.g. 0:red,0.5:white,1:blue", false},
              {"points", ParamType::String, "freeform colour points x,y,colour separated by spaces", false},
              numParam("angle", "linear direction in degrees: 0 = left to right, 90 = top to bottom", false, 0.0),
              numParam("cx", "radial centre x, fraction of the shape's width", false, 0.5),
              numParam("cy", "radial centre y, fraction of the shape's height", false, 0.5),
              numParam("radius", "radial radius, fraction of the shape's size", false, 0.5),
              {"remove", ParamType::Bool, "remove the gradient", false, {}, false},
              previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                const bool stroke = stringParam(p, "target", "fill") == "stroke";
                std::optional<Gradient>& slot = stroke ? s.strokeGradient : s.gradient;
                if (boolParam(p, "remove", false)) {
                    slot.reset();
                    return Result::success("removed the gradient from " + s.id, s.id).with("shape", s.id);
                }
                Gradient g;
                std::string error;
                if (lower(stringParam(p, "type", "linear")) == "freeform") {
                    g.freeform = true;
                    if (!parseFreeform(stringParam(p, "points", ""), g.points, error)) return Result::failure(error);
                    g.stops = {{0, g.points.front().color}};
                } else if (!gradientArg(p, g, error)) {
                    return Result::failure(error);
                }
                slot = g;
                std::optional<Color>& flat = stroke ? s.stroke : s.fill;
                if (!flat) flat = g.stops.front().color;  // fallback colour for viewers without gradients
                const std::string kind = g.freeform ? "freeform" : g.radial ? "radial" : "linear";
                return Result::success(std::string(stroke ? "stroked " : "filled ") + s.id + " with a " + kind + " gradient (" +
                                           std::to_string(g.freeform ? g.points.size() : g.stops.size()) + (g.freeform ? " points)" : " stops)"),
                                       s.id)
                    .with("shape", s.id);
            })));

    reg.add({"vector.text_style", "Text Style",
             "Define or change a named character/paragraph style (font, size, bold, italic, letter_spacing, "
             "line_height, align, case, fill), or take one from a text shape with from=. Text that follows the "
             "style (vector.set_text style=NAME, or shapes= here) changes with it. remove=true deletes it.",
             {docParam(), {"name", ParamType::String, "style name (letters, digits, _ - .)", true},
              {"from", ParamType::String, "copy the settings of this text shape", false},
              {"font", ParamType::String, "font", false}, numParam("size", "font size px", false),
              {"bold", ParamType::Bool, "bold", false}, {"italic", ParamType::Bool, "italic", false},
              numParam("letter_spacing", "extra px between letters", false), numParam("line_height", "times the size", false),
              {"align", ParamType::String, "left, center, right or justify", false},
              {"case", ParamType::String, "none, upper, lower, title or small_caps", false}, paintParam("fill", "text colour", ""),
              {"shapes", ParamType::String, "text shapes to give the style", false},
              {"remove", ParamType::Bool, "delete the style (text keeps its look)", false, {}, false}, previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (!validName(name)) return Result::failure("style names use letters, digits, _ - and . only");
                auto it = std::find_if(doc.textStyles.begin(), doc.textStyles.end(), [&](const TextStyle& t) { return t.name == name; });
                if (boolParam(p, "remove", false)) {
                    if (it == doc.textStyles.end()) return Result::failure("no text style '" + name + "'");
                    doc.textStyles.erase(it);
                    for (auto& s : doc.shapes) {
                        if (s.textStyle == name) s.textStyle.clear();
                    }
                    return Result::success("deleted text style " + name);
                }
                TextStyle t = it == doc.textStyles.end() ? TextStyle{} : *it;
                t.name = name;
                std::string error;
                if (has(p, "from")) {
                    const int i = doc.find(stringParam(p, "from"));
                    if (i < 0 || doc.shapes[static_cast<std::size_t>(i)].kind != ShapeKind::Text) return Result::failure("from= must name a text shape");
                    const Shape& s = doc.shapes[static_cast<std::size_t>(i)];
                    t.font = s.font, t.size = s.fontSize, t.bold = s.bold, t.italic = s.italic, t.letterSpacing = s.letterSpacing;
                    t.lineHeight = s.lineHeight, t.align = s.align, t.textCase = s.textCase, t.fill = s.fill;
                }
                Shape probe;  // reuse the text param checks
                probe.font = t.font, probe.fontSize = t.size, probe.bold = t.bold, probe.italic = t.italic, probe.letterSpacing = t.letterSpacing;
                probe.lineHeight = t.lineHeight, probe.align = t.align, probe.textCase = t.textCase;
                Params q = p;
                if (has(p, "size")) q["size"] = numberParam(p, "size");
                q.erase("x"), q.erase("y"), q.erase("text");
                if (!applyText(probe, q, error)) return Result::failure(error);
                t.font = probe.font, t.size = probe.fontSize, t.bold = probe.bold, t.italic = probe.italic, t.letterSpacing = probe.letterSpacing;
                t.lineHeight = probe.lineHeight, t.align = probe.align, t.textCase = probe.textCase;
                if (has(p, "fill")) {
                    std::optional<Color> c;
                    if (!paintArg(stringParam(p, "fill"), c, error)) return Result::failure(error);
                    t.fill = c;
                }
                if (it == doc.textStyles.end()) doc.textStyles.push_back(t);
                else *it = t;
                if (has(p, "shapes")) {
                    std::vector<int> idx;
                    if (!shapeList(doc, stringParam(p, "shapes"), idx, error)) return Result::failure(error);
                    for (int i : idx) {
                        if (doc.shapes[static_cast<std::size_t>(i)].kind == ShapeKind::Text) doc.shapes[static_cast<std::size_t>(i)].textStyle = name;
                    }
                }
                int count = 0;
                for (auto& s : doc.shapes) {
                    if (s.kind == ShapeKind::Text && s.textStyle == name) applyTextStyle(s, t), ++count;
                }
                Result r = Result::success("text style " + textStyleText(t) + " (" + std::to_string(count) + " text shapes follow it)", name);
                r.with("name", name).with("count", static_cast<std::int64_t>(count));
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.boolean", "Combine Shapes",
             "Combine filled shapes into one path: union merges them, subtract cuts the later shapes out of the "
             "first, intersect keeps only the overlap, exclude keeps what isn't shared. The result takes the first "
             "shape's style and place in the stacking order; the originals are removed unless keep=true.",
             {docParam(),
              {"op", ParamType::String, "union, subtract, intersect or exclude", true},
              {"shapes", ParamType::String, "two or more shape ids or names, comma separated, e.g. s1,s2", true},
              {"keep", ParamType::Bool, "keep the original shapes", false, {}, false},
              {"name", ParamType::String, "name for the result", false},
              previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const std::string opName = lower(stringParam(p, "op"));
                BoolOp op;
                if (opName == "union" || opName == "unite" || opName == "add") op = BoolOp::Union;
                else if (opName == "subtract" || opName == "difference" || opName == "minus") op = BoolOp::Subtract;
                else if (opName == "intersect" || opName == "intersection") op = BoolOp::Intersect;
                else if (opName == "exclude" || opName == "xor") op = BoolOp::Exclude;
                else return Result::failure("op must be union, subtract, intersect or exclude");

                std::vector<int> idx;
                for (const auto& name : splitList(stringParam(p, "shapes"))) {
                    int i = doc.find(name);
                    if (i < 0) return Result::failure("no shape '" + name + "' in this document (vector.info lists them)");
                    if (std::find(idx.begin(), idx.end(), i) == idx.end()) idx.push_back(i);
                }
                if (idx.size() < 2) return Result::failure("give at least two different shapes, e.g. shapes=s1,s2");

                auto nz = [&](int i) {
                    const Shape& sh = doc.shapes[static_cast<std::size_t>(i)];
                    return sh.kind == ShapeKind::Text || sh.kind == ShapeKind::Group || sh.kind == ShapeKind::Instance || sh.fillRule == "nonzero";
                };
                Outline acc = areaOf(doc, idx[0]);
                bool accNz = nz(idx[0]);
                for (std::size_t k = 1; k < idx.size(); ++k) {
                    acc = booleanOp(acc, areaOf(doc, idx[k]), op, accNz, nz(idx[k]));
                    accNz = false;
                }
                Shape result = doc.shapes[static_cast<std::size_t>(idx[0])];
                if (result.kind == ShapeKind::Group || result.kind == ShapeKind::Instance) result.fill = Color{0, 0, 0, 255};
                result.kind = ShapeKind::Path;
                result.onPath.clear();
                result.clip.clear();
                result.mask.clear();
                result.symbol.clear();
                result.layer = false;
                result.d = outlinePathData(acc);
                result.text.clear();
                result.points.clear();
                if (!result.transform.identity()) result.strokeWidth *= shapeMatrix(result).scaleFactor();
                result.transform = Transform{};
                result.fillRule = "evenodd";
                if (!result.fill) result.fill = result.stroke;  // an outline-only first shape still shows the area
                result.id.clear();
                result.name = has(p, "name") ? stringParam(p, "name") : std::string{};
                const std::string firstId = doc.shapes[static_cast<std::size_t>(idx[0])].id;

                std::string id = doc.add(std::move(result));
                Shape made = std::move(doc.shapes.back());
                doc.shapes.pop_back();
                int insertAt = idx[0];
                if (!boolParam(p, "keep", false)) {
                    std::vector<std::string> gone;
                    for (int i : idx) gone.push_back(doc.shapes[static_cast<std::size_t>(i)].id);
                    for (const auto& g : gone) {
                        const int i = doc.find(g);
                        if (i < 0) continue;
                        const int n = static_cast<int>(doc.shapes.size());
                        const int first = i;
                        eraseSubtree(doc, i);
                        if (first < insertAt) insertAt -= n - static_cast<int>(doc.shapes.size());
                    }
                    insertAt = std::max(insertAt, 0);
                } else {
                    insertAt = *std::max_element(idx.begin(), idx.end()) + 1;
                }
                insertAt = std::clamp(insertAt, 0, static_cast<int>(doc.shapes.size()));
                doc.shapes.insert(doc.shapes.begin() + insertAt, std::move(made));
                const Shape& s = doc.shapes[static_cast<std::size_t>(insertAt)];
                if (acc.empty()) {
                    Result r = Result::success(opName + " left nothing (the shapes don't overlap that way); " + id + " is empty", id);
                    r.with("shape", id);
                    return maybePreview(r, doc, p);
                }
                Result r = Result::success(opName + " of " + std::to_string(idx.size()) + " shapes -> " + describe(s) +
                                               " (style from " + firstId + ")",
                                           id);
                r.with("shape", id).with("contours", static_cast<std::int64_t>(acc.size()));
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.to_path", "Convert to Path",
             "Turn any shape into an editable path, keeping its look. Text becomes letter outlines (so it no longer "
             "needs the font); rectangles, ellipses and polygons become their outlines.",
             {docParam(), shapeParam(), previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params&) {
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                if (s.kind == ShapeKind::Path) return Result::success(s.id + " is already a path", s.id).with("shape", s.id);
                const std::string was = toString(s.kind);
                std::string d = toPathData(s);
                if (d.empty()) return Result::failure(s.id + " has nothing to outline");
                s.d = std::move(d);
                s.kind = ShapeKind::Path;
                s.text.clear();
                s.points.clear();
                return Result::success("converted " + was + " " + s.id + " to a path", s.id).with("shape", s.id);
            })));
}

}  // namespace wetwaresuite::vector::detail
