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
                if (!applyText(s, p, error)) return Result::failure(error);
                if (s.text.empty()) return Result::failure("text can't be empty");
                if (!has(p, "fill")) s.fill = Color{0, 0, 0, 255};
                return addShape(doc, std::move(s), p);
            }));

    reg.add({"vector.set_text", "Edit Text",
             "Change a text shape's words, position, font, size, alignment or spacing. Only the params you give change.",
             join({docParam(), shapeParam()}, join(textParams(false), {previewParam()})), true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                if (s.kind != ShapeKind::Text) return Result::failure(s.id + " is a " + toString(s.kind) + ", not text");
                std::string error;
                if (!applyText(s, p, error)) return Result::failure(error);
                if (s.text.empty()) return Result::failure("text can't be empty");
                return Result::success("updated " + describe(s), s.id).with("shape", s.id);
            })));

    reg.add({"vector.gradient", "Gradient Fill",
             "Fill a shape with a linear or radial gradient: colors=red,blue spreads colours evenly, or "
             "stops=0:red,0.3:gold,1:blue places them. Positions are fractions of the shape's box, so the gradient "
             "follows the shape when it moves or scales. remove=true goes back to the flat fill.",
             {docParam(), shapeParam(),
              {"type", ParamType::String, "linear or radial", false, {}, std::string("linear")},
              {"colors", ParamType::String, "colours spread evenly, e.g. red,blue or #ff0000,#ffd700,#0000ff", false},
              {"stops", ParamType::String, "offset:colour pairs, e.g. 0:red,0.5:white,1:blue", false},
              numParam("angle", "linear direction in degrees: 0 = left to right, 90 = top to bottom", false, 0.0),
              numParam("cx", "radial centre x, fraction of the shape's width", false, 0.5),
              numParam("cy", "radial centre y, fraction of the shape's height", false, 0.5),
              numParam("radius", "radial radius, fraction of the shape's size", false, 0.5),
              {"remove", ParamType::Bool, "remove the gradient", false, {}, false},
              previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                Shape& s = doc.shapes[static_cast<std::size_t>(idx)];
                if (boolParam(p, "remove", false)) {
                    s.gradient.reset();
                    return Result::success("removed the gradient from " + s.id, s.id).with("shape", s.id);
                }
                Gradient g;
                std::string error;
                if (!gradientArg(p, g, error)) return Result::failure(error);
                s.gradient = g;
                if (!s.fill) s.fill = g.stops.front().color;  // fallback colour for viewers without gradients
                return Result::success("filled " + s.id + " with a " + (g.radial ? "radial" : "linear") + " gradient (" +
                                           std::to_string(g.stops.size()) + " stops)",
                                       s.id)
                    .with("shape", s.id);
            })));

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

                Outline acc = outline(doc.shapes[static_cast<std::size_t>(idx[0])], 4.0);
                for (std::size_t k = 1; k < idx.size(); ++k) {
                    Outline next = outline(doc.shapes[static_cast<std::size_t>(idx[k])], 4.0);
                    for (auto& c : next) c.closed = true;
                    for (auto& c : acc) c.closed = true;
                    acc = booleanOp(acc, next, op);
                }

                Shape result = doc.shapes[static_cast<std::size_t>(idx[0])];
                result.kind = ShapeKind::Path;
                result.d = outlinePathData(acc);
                result.text.clear();
                result.points.clear();
                result.transform = Transform{};
                if (!result.fill) result.fill = result.stroke;  // an outline-only first shape still shows the area
                result.id.clear();
                result.name = has(p, "name") ? stringParam(p, "name") : std::string{};
                const std::string firstId = doc.shapes[static_cast<std::size_t>(idx[0])].id;

                std::string id = doc.add(std::move(result));
                Shape made = std::move(doc.shapes.back());
                doc.shapes.pop_back();
                int insertAt = idx[0];
                if (!boolParam(p, "keep", false)) {
                    std::vector<int> sorted = idx;
                    std::sort(sorted.rbegin(), sorted.rend());
                    for (int i : sorted) {
                        doc.shapes.erase(doc.shapes.begin() + i);
                        if (i < insertAt) --insertAt;
                    }
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
