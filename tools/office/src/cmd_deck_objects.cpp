// office.deck_* commands for free objects on slides and on the master: text boxes,
// shapes, lines and pictures, placed in points like PowerPoint's own objects.
#include <algorithm>
#include <cmath>

#include "deck_cmd.hpp"
#include "text.hpp"

namespace wetwaresuite::office::deckcmd {

namespace {

std::vector<ParamSpec> join(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<ParamSpec> whereSpecs() {
    return {doc(), integer("slide", "slide number (or master=true)"), flag("master", "the master instead of a slide (its objects show on every slide but title and section slides)", false)};
}

std::vector<ParamSpec> boxSpecs() {
    return {number("x", "left edge in points from the slide's left (the slide is 960 x 540 points, 720 x 540 at 4:3)"),
            number("y", "top edge in points from the slide's top"),
            number("w", "width in points"),
            number("h", "height in points (text boxes: 0 = grow with the text)"),
            str("position", "place it on the slide instead of x/y (48 pt from the edges)", false, {},
                {"center", "top", "bottom", "left", "right", "top_left", "top_right", "bottom_left", "bottom_right"}),
            str("name", "a name to find it by later (instead of its number)")};
}

std::vector<ParamSpec> paintSpecs() {
    return {str("fill", "fill colour: #rrggbb, a name, accent, text, background or none"),
            str("line", "outline colour (none = no outline)"),
            number("line_width", "outline width in points (default 1.5)")};
}

std::vector<ParamSpec> textSpecs() {
    return {str("text", "the text (inline Markdown: **bold**, *italic*, [links](url)); one paragraph per line"),
            number("size", "text size in points (default 20 in text boxes, 18 in shapes)"),
            str("color", "text colour (default: readable on the fill)"),
            flag("bold", "bold text"),
            flag("italic", "italic text"),
            str("align", "horizontal alignment", false, {}, {"left", "center", "right"}),
            str("valign", "vertical alignment", false, {}, {"top", "middle", "bottom"}),
            flag("bullets", "lines are bullet points (indent 2 spaces for sub-bullets, 1. for numbers)")};
}

// The object list a command works on: a slide's or the master's.
std::vector<SlideObject>* objectList(Deck& d, const Params& p, std::string& where, std::string& error) {
    if (boolParam(p, "master", false)) {
        where = "the master";
        return &d.master;
    }
    if (!has(p, "slide")) {
        error = "give slide=<n> or master=true";
        return nullptr;
    }
    std::size_t i;
    if (!slideArg(d, p, "slide", i, error)) return nullptr;
    where = "slide " + std::to_string(i + 1);
    return &d.slides[i].objects;
}

// object=<n or name> to an index.
bool objectArg(const std::vector<SlideObject>& objs, const Params& p, const std::string& where, std::size_t& index, std::string& error) {
    if (!has(p, "object")) {
        error = "give object=<number or name>";
        return false;
    }
    const auto& v = p.at("object");
    const std::string key = std::holds_alternative<std::int64_t>(v) ? std::to_string(std::get<std::int64_t>(v)) : trimCopy(stringParam(p, "object"));
    if (!key.empty() && std::all_of(key.begin(), key.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        const long n = std::atol(key.c_str());
        if (n >= 1 && n <= static_cast<long>(objs.size())) {
            index = static_cast<std::size_t>(n - 1);
            return true;
        }
    }
    for (std::size_t i = 0; i < objs.size(); ++i) {
        if (!objs[i].name.empty() && lower(objs[i].name) == lower(key)) {
            index = i;
            return true;
        }
    }
    error = objs.empty() ? where + " has no objects" : "no object '" + key + "' on " + where + " (1.." + std::to_string(objs.size()) + " or a name)";
    return false;
}

const char* alignName(int a) { return a == 1 ? "center" : a == 2 ? "right" : "left"; }

std::string describeObject(const Deck& d, const SlideObject& o, std::size_t i) {
    std::string t = std::to_string(i + 1) + ". " + (o.kind == "shape" ? o.shape : o.kind == "text" ? std::string("text box") : o.kind);
    if (!o.name.empty()) t += " \"" + o.name + "\"";
    if (o.kind == "line") {
        return t + " from " + textNumber(std::round(o.x)) + "," + textNumber(std::round(o.y)) + " to " + textNumber(std::round(o.x + o.w)) + "," +
               textNumber(std::round(o.y + o.h)) + ", " + (o.line.empty() ? std::string("no colour") : o.line) + " " + textNumber(o.lineWidth) + " pt" +
               (o.arrow ? ", arrow" : "");
    }
    t += " at " + textNumber(std::round(o.x)) + "," + textNumber(std::round(o.y)) + " size " + textNumber(std::round(o.w)) + "x" +
         textNumber(std::round(objectHeight(d, o))) + (o.kind == "text" && o.h <= 0 ? " (auto height)" : "");
    if (o.kind == "image" && o.pixels) t += ", picture " + std::to_string(o.pixels->pixels.width()) + "x" + std::to_string(o.pixels->pixels.height());
    if (!o.fill.empty()) t += ", fill " + o.fill;
    if (!o.line.empty()) t += ", outline " + o.line + " " + textNumber(o.lineWidth) + " pt";
    if (!o.text.empty()) {
        std::string one = o.text;
        std::replace(one.begin(), one.end(), '\n', '/');
        if (one.size() > 60) one = one.substr(0, 57) + "...";
        t += ", text \"" + one + "\" " + textNumber(o.size) + " pt";
        if (o.bold) t += " bold";
        if (o.italic) t += " italic";
        if (!o.color.empty()) t += " " + o.color;
        if (o.align) t += std::string(" ") + alignName(o.align);
        if (o.bullets) t += " bullets";
        if (objectOverflows(d, o)) t += " - WARNING: the text doesn't fit even when shrunk; make the object bigger or the text shorter";
    }
    return t;
}

// position=center/top/... places the object on the slide, 48 pt from the edges.
void applyPosition(const Deck& d, SlideObject& o, const std::string& pos) {
    const double m = 48, h = objectHeight(d, o), w = o.w;
    if (pos.find("left") != std::string::npos) o.x = m;
    else if (pos.find("right") != std::string::npos) o.x = d.width - m - w;
    else o.x = (d.width - w) / 2;
    if (pos.find("top") != std::string::npos) o.y = m;
    else if (pos.find("bottom") != std::string::npos) o.y = d.height - m - h;
    else o.y = (d.height - h) / 2;
}

bool paintArg(const Params& p, const char* key, std::string& out, std::string& error) {
    if (!has(p, key)) return true;
    if (parsePaint(stringParam(p, key), out)) return true;
    error = std::string(key) + ": not a colour: " + stringParam(p, key) + " (#rrggbb, a name, accent, text, background or none)";
    return false;
}

// Sets every property given in p. Positions and sizes are checked by the caller.
bool applyObject(SlideObject& o, const Params& p, std::string& error) {
    if (has(p, "name")) o.name = trimCopy(stringParam(p, "name"));
    if (has(p, "x")) o.x = numberParam(p, "x");
    if (has(p, "y")) o.y = numberParam(p, "y");
    if (has(p, "w")) o.w = numberParam(p, "w");
    if (has(p, "h")) o.h = numberParam(p, "h");
    if (o.kind == "line") {
        if (has(p, "x2")) o.w = numberParam(p, "x2") - o.x;
        if (has(p, "y2")) o.h = numberParam(p, "y2") - o.y;
        if (has(p, "arrow")) o.arrow = boolParam(p, "arrow", false);
        if (!paintArg(p, "color", o.line, error)) return false;
    }
    if (has(p, "shape")) {
        const std::string sh = lower(stringParam(p, "shape"));
        if (std::find(shapeNames().begin(), shapeNames().end(), sh) == shapeNames().end()) {
            error = "unknown shape '" + sh + "'";
            return false;
        }
        o.shape = sh;
    }
    if (!paintArg(p, "fill", o.fill, error) || !paintArg(p, "line", o.line, error)) return false;
    if (has(p, "line_width")) o.lineWidth = std::clamp(numberParam(p, "line_width"), 0.25, 50.0);
    if (o.kind != "line" && o.kind != "image") {
        if (has(p, "text")) o.text = textParam(p, "text");
        if (has(p, "size")) o.size = std::clamp(numberParam(p, "size"), 4.0, 200.0);
        if (!paintArg(p, "color", o.color, error)) return false;
        if (has(p, "bold")) o.bold = boolParam(p, "bold", false);
        if (has(p, "italic")) o.italic = boolParam(p, "italic", false);
        if (has(p, "bullets")) o.bullets = boolParam(p, "bullets", false);
        if (has(p, "align")) {
            const std::string a = stringParam(p, "align");
            o.align = a == "center" ? 1 : a == "right" ? 2 : 0;
        }
        if (has(p, "valign")) {
            const std::string a = stringParam(p, "valign");
            o.anchor = a == "middle" ? 1 : a == "bottom" ? 2 : 0;
        }
    }
    if (o.kind == "image" && has(p, "path")) {
        try {
            const double oldW = o.w;
            setObjectImage(o, readBytes(stringParam(p, "path")));
            // a new picture keeps the width and takes its own proportions
            const auto& px = o.pixels->pixels;
            if (!has(p, "h") && px.width() > 0) o.h = (has(p, "w") ? o.w : oldW) * px.height() / px.width();
        } catch (const std::exception& e) {
            error = std::string("path: ") + e.what();
            return false;
        }
    }
    return true;
}

bool checkBox(const Deck& d, const SlideObject& o, std::string& error) {
    const double lim = 4 * std::max(d.width, d.height);
    if (!(std::abs(o.x) < lim && std::abs(o.y) < lim && std::abs(o.w) < lim && std::abs(o.h) < lim)) {
        error = "position or size out of range";
        return false;
    }
    if (o.kind != "line" && (o.w < 1 || (o.kind != "text" && o.h < 1) || o.h < 0)) {
        error = "w and h must be at least 1 point";
        return false;
    }
    if (o.kind == "line" && std::hypot(o.w, o.h) < 1) {
        error = "the line has no length: give x2/y2";
        return false;
    }
    return true;
}

// Shared by every "add an object" command: where it goes, its properties, the result.
Result addObject(DeckDoc& d, const Params& p, SlideObject o) {
    std::string where, error;
    auto* list = objectList(d.data, p, where, error);
    if (!list) return Result::failure(error);
    if (!applyObject(o, p, error)) return Result::failure(error);
    if (has(p, "position")) applyPosition(d.data, o, stringParam(p, "position"));
    if (!checkBox(d.data, o, error)) return Result::failure(error);
    list->push_back(std::move(o));
    const std::size_t i = list->size() - 1;
    Result r = Result::success("added to " + where + ": " + describeObject(d.data, list->back(), i), static_cast<std::int64_t>(i + 1));
    r.with("object", static_cast<std::int64_t>(i + 1));
    if (!boolParam(p, "master", false)) slidePreview(r, d.data, static_cast<std::size_t>(intParam(p, "slide") - 1), p);
    else if (!d.data.slides.empty()) {
        // show the first slide the master appears on
        for (std::size_t s = 0; s < d.data.slides.size(); ++s) {
            const auto& l = d.data.slides[s].layout;
            if (d.data.masterOnTitle || (l != "title" && l != "section")) {
                slidePreview(r, d.data, s, p);
                break;
            }
        }
    }
    return r;
}

}  // namespace

void registerDeckObjectCommands(Session& session) {
    auto& reg = session.registry();
    const std::string shapes = [] {
        std::string s;
        for (const auto& n : shapeNames()) s += (s.empty() ? "" : ", ") + n;
        return s;
    }();

    reg.add({"office.deck_text_box", "Add Text Box",
             "Put a text box anywhere on a slide (or on the master, for every slide). Without w it is 400 points wide; without h it "
             "grows with its text. Without x/y or position it goes in the middle.",
             join(join(join(whereSpecs(), boxSpecs()), join(textSpecs(), paintSpecs())), {previewSpec()}), true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                if (!hasText(p, "text")) return Result::failure("give text=");
                SlideObject o;
                o.kind = "text";
                o.w = 400;
                o.h = 0;
                o.size = 20;
                if (!has(p, "position") && !has(p, "x") && !has(p, "y")) {
                    Params q = p;
                    q["position"] = std::string("center");
                    return addObject(d, q, o);
                }
                return addObject(d, p, o);
            }));

    reg.add({"office.deck_shape", "Add Shape",
             "Draw a shape (" + shapes + ") on a slide or the master, optionally with text inside (centred). Default 200 x 120 points "
             "in the middle, filled with the theme accent.",
             join(join(join(join(whereSpecs(), {str("shape", "the shape", false, std::string("rect"), shapeNames())}), boxSpecs()),
                       join(textSpecs(), paintSpecs())),
                  {previewSpec()}),
             true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                SlideObject o;
                o.kind = "shape";
                o.w = 200;
                o.h = 120;
                o.fill = "accent";
                o.align = 1;
                o.anchor = 1;
                o.size = 18;
                if (!has(p, "position") && !has(p, "x") && !has(p, "y")) {
                    Params q = p;
                    q["position"] = std::string("center");
                    return addObject(d, q, o);
                }
                return addObject(d, p, o);
            }));

    reg.add({"office.deck_line", "Add Line",
             "Draw a straight line or an arrow from (x, y) to (x2, y2), in points, on a slide or the master.",
             join(whereSpecs(), {number("x", "start, from the left", true), number("y", "start, from the top", true), number("x2", "end, from the left", true),
                                 number("y2", "end, from the top", true), str("color", "line colour (default: the theme's text colour)"),
                                 number("line_width", "width in points (default 2)"), flag("arrow", "an arrowhead at the end", false),
                                 str("name", "a name to find it by later"), previewSpec()}),
             true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                SlideObject o;
                o.kind = "line";
                o.line = "text";
                o.lineWidth = 2;
                return addObject(d, p, o);
            }));

    reg.add({"office.deck_image", "Add Picture",
             "Place a picture (png, jpg, ...) anywhere on a slide, or on the master for a logo on every slide. Without w it keeps "
             "its natural size up to 480 points wide; h follows the picture's proportions unless given.",
             join(join(whereSpecs(), {str("path", "picture file", true)}),
                  join(boxSpecs(), {str("line", "border colour (default none)"), number("line_width", "border width in points"), previewSpec()})),
             true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                SlideObject o;
                o.kind = "image";
                try {
                    setObjectImage(o, readBytes(stringParam(p, "path")));
                } catch (const std::exception& e) {
                    return Result::failure(std::string("path: ") + e.what());
                }
                const double pw = o.pixels->pixels.width(), ph = o.pixels->pixels.height();
                o.w = has(p, "w") ? numberParam(p, "w") : std::min(pw * 0.75, 480.0);
                o.h = has(p, "h") ? numberParam(p, "h") : o.w * ph / pw;
                Params q = p;
                q.erase("path");  // already loaded
                q.erase("w");
                q.erase("h");
                if (!has(p, "position") && !has(p, "x") && !has(p, "y")) q["position"] = std::string("center");
                return addObject(d, q, o);
            }));

    reg.add({"office.deck_object_edit", "Edit Object",
             "Change a text box, shape, line or picture on a slide or the master: move it (x/y or position), resize it, change its "
             "text, colours or shape, swap the picture (path=), rename it, or bring it forward/back. Fields left out stay.",
             join(join(join(whereSpecs(), {str("object", "object number (office.deck_objects lists them) or name", true),
                                           str("shape", "change the shape", false, {}, shapeNames())}),
                       join(boxSpecs(), join(textSpecs(), paintSpecs()))),
                  {number("x2", "line: end, from the left"), number("y2", "line: end, from the top"), flag("arrow", "line: arrowhead at the end"),
                   str("path", "picture: a new picture file"),
                   str("order", "move it in front of or behind the others", false, {}, {"front", "back", "forward", "backward"}), previewSpec()}),
             true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                std::string where, error;
                auto* list = objectList(d.data, p, where, error);
                std::size_t i;
                if (!list || !objectArg(*list, p, where, i, error)) return Result::failure(error);
                SlideObject& o = (*list)[i];
                if (!applyObject(o, p, error)) return Result::failure(error);
                if (has(p, "position")) applyPosition(d.data, o, stringParam(p, "position"));
                if (!checkBox(d.data, o, error)) return Result::failure(error);
                if (has(p, "order")) {
                    const std::string ord = stringParam(p, "order");
                    SlideObject moved = o;
                    list->erase(list->begin() + static_cast<std::ptrdiff_t>(i));
                    std::size_t to = ord == "front" ? list->size() : ord == "back" ? 0 : ord == "forward" ? std::min(i + 1, list->size()) : (i == 0 ? 0 : i - 1);
                    list->insert(list->begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
                    i = to;
                }
                Result r = Result::success("changed on " + where + ": " + describeObject(d.data, (*list)[i], i));
                r.with("object", static_cast<std::int64_t>(i + 1));
                if (!boolParam(p, "master", false)) slidePreview(r, d.data, static_cast<std::size_t>(intParam(p, "slide") - 1), p);
                return r;
            }));

    reg.add({"office.deck_object_delete", "Delete Object", "Delete an object (or all=true: every object) from a slide or the master.",
             join(whereSpecs(), {str("object", "object number or name"), flag("all", "delete every object there", false)}), true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                std::string where, error;
                auto* list = objectList(d.data, p, where, error);
                if (!list) return Result::failure(error);
                if (boolParam(p, "all", false)) {
                    const std::size_t n = list->size();
                    list->clear();
                    return Result::success("deleted " + std::to_string(n) + " object(s) from " + where);
                }
                std::size_t i;
                if (!objectArg(*list, p, where, i, error)) return Result::failure(error);
                const std::string what = describeObject(d.data, (*list)[i], i);
                list->erase(list->begin() + static_cast<std::ptrdiff_t>(i));
                return Result::success("deleted from " + where + ": " + what).with("objects", static_cast<std::int64_t>(list->size()));
            }));

    reg.add({"office.deck_objects", "List Objects",
             "List the text boxes, shapes, lines and pictures on a slide (or on the master) with their numbers, names, positions, "
             "sizes, colours and text.",
             join(whereSpecs(), {fileSpec("slide deck")}), false, true},
            read(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                std::string where, error;
                auto* list = objectList(d.data, p, where, error);
                if (!list) return Result::failure(error);
                std::string t = where + ": " + std::to_string(list->size()) + " object(s)";
                for (std::size_t i = 0; i < list->size(); ++i) t += "\n  " + describeObject(d.data, (*list)[i], i);
                return Result::success(t).with("count", static_cast<std::int64_t>(list->size()));
            }));
}

}  // namespace wetwaresuite::office::deckcmd
