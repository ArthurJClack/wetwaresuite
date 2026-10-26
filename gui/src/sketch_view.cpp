// Sketching a cad part with the mouse, as in Onshape or Fusion: while a sketch is open the view
// looks straight at it, and Line, Rectangle and Circle draw on a snapping grid. Points snap to
// the grid and to the sketch's own points (so lines join up), with the length or size shown
// while you draw. Each shape is one cad.sketch_* command, so it undoes and Claude sees it.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

NumberList listField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    auto l = v ? std::get_if<NumberList>(v) : nullptr;
    return l ? *l : NumberList{};
}

std::string textOf(const Result& r, const char* key) {
    const Value* v = r.field(key);
    auto s = v ? std::get_if<std::string>(v) : nullptr;
    return s ? *s : std::string();
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", std::fabs(v) < 1e-9 ? 0.0 : v);
    return b;
}

struct SketchToolDef {
    const char* name;
    SketchEdit::Tool tool;
    ImGuiKey key;
    const char* letter;
    const char* tip;
};
const SketchToolDef kSketchTools[] = {
    {"Line", SketchEdit::Tool::Line, ImGuiKey_L, "L", "Click, click, click... to draw joined lines. Click the first point again, right-click or Esc to stop"},
    {"Rectangle", SketchEdit::Tool::Rect, ImGuiKey_R, "R", "Drag from one corner to the other (or click both corners)"},
    {"Circle", SketchEdit::Tool::Circle, ImGuiKey_C, "C", "Drag from the centre out to the size (or click the centre, then the edge)"}};

}  // namespace

bool Shell::sketchActive() const { return sketch_.active && sketch_.doc == currentDoc_ && find("cad.sketch_geometry"); }

void Shell::enterSketch(const std::string& doc, const std::string& name) {
    if (!find("cad.sketch_geometry")) return;
    sketch_ = SketchEdit{};
    sketch_.doc = doc;
    sketch_.sketch = name;
    sketch_.active = true;
    sketch_.fit = true;
}

void Shell::leaveSketch() {
    sketch_.active = false;
    sketch_.drawing = false;
    canvas_.orbitDirty = true;
}

void Shell::refreshSketch() {
    SketchEdit& k = sketch_;
    if (k.seen == changes_) return;
    k.seen = changes_;
    Params p{{"doc", k.doc}};
    if (!k.sketch.empty()) p["sketch"] = k.sketch;
    Result r = query("cad.sketch_geometry", p);
    if (!r.ok) {  // the sketch was undone away
        leaveSketch();
        return;
    }
    k.sketch = textOf(r, "sketch");
    k.status = textOf(r, "status");
    k.units = textOf(r, "units");
    k.lines = listField(r, "lines");
    k.construction = listField(r, "construction");
    k.points = listField(r, "points");
    k.free = listField(r, "free");
    k.profiles = listField(r, "profiles");
}

void Shell::sketchToolbar() {
    SketchEdit& k = sketch_;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(("Sketching " + k.sketch + ":").c_str());
    for (const auto& t : kSketchTools) {
        ImGui::SameLine();
        const bool on = k.tool == t.tool;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, accent_);
            ImGui::PushStyleColor(ImGuiCol_Text, accentText_);
        }
        if (ImGui::Button((std::string(t.name) + " (" + t.letter + ")").c_str())) {
            k.tool = t.tool;
            k.drawing = false;
        }
        if (on) ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", t.tip);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit")) k.fit = true;
    ImGui::SetItemTooltip("Show the whole sketch (or double-click)");
    ImGui::SameLine();
    ImGui::BeginDisabled(k.profiles.empty() || !find("cad.sketch_extrude"));
    if (accentButton("Extrude...")) {
        const std::string name = k.sketch;
        leaveSketch();
        select("cad.sketch_extrude");
        if (form_) {
            for (auto& f : form_->fields)
                if (f.spec.name == "sketch") setFieldText(f, name);
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(k.profiles.empty() ? "Draw a closed shape first (a rectangle, a circle, or lines that end where they started)"
                                             : "Pull the closed shapes into a solid");
    ImGui::SameLine();
    if (ImGui::Button("Finish sketch")) leaveSketch();
    ImGui::SetItemTooltip("Back to the 3D view");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", k.status.c_str());
}

void Shell::sketchCanvas(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered, bool active) {
    SketchEdit& k = sketch_;
    refreshSketch();
    if (!k.active) return;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mid{v0.x + size.x / 2, v0.y + size.y / 2};

    if (k.fit) {  // the whole sketch, or 50 units around the origin when it's empty
        double x0 = -25, y0 = -25, x1 = 25, y1 = 25;
        bool any = false;
        auto grow = [&](const NumberList& l) {
            for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
                if (!any) x0 = x1 = l[i], y0 = y1 = l[i + 1];
                any = true;
                x0 = std::min(x0, l[i]), x1 = std::max(x1, l[i]), y0 = std::min(y0, l[i + 1]), y1 = std::max(y1, l[i + 1]);
            }
        };
        grow(k.lines);
        grow(k.construction);
        if (any) {
            const double pad = std::max({x1 - x0, y1 - y0, 10.0}) * 0.15;
            x0 -= pad, x1 += pad, y0 -= pad, y1 += pad;
        }
        k.cx = (x0 + x1) / 2;
        k.cy = (y0 + y1) / 2;
        k.ppm = std::min(size.x / (x1 - x0), size.y / (y1 - y0));
        k.fit = false;
    }
    auto toScreen = [&](double x, double y) {
        return ImVec2{mid.x + static_cast<float>((x - k.cx) * k.ppm), mid.y - static_cast<float>((y - k.cy) * k.ppm)};
    };
    auto toSketch = [&](ImVec2 s, double& x, double& y) {
        x = k.cx + (s.x - mid.x) / k.ppm;
        y = k.cy - (s.y - mid.y) / k.ppm;
    };

    // pan with the right or middle button, zoom with the wheel around the mouse
    if (active && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))) {
        k.cx -= io.MouseDelta.x / k.ppm;
        k.cy += io.MouseDelta.y / k.ppm;
    }
    if (hovered && io.MouseWheel != 0) {
        double bx, by;
        toSketch(io.MousePos, bx, by);
        k.ppm = std::clamp(k.ppm * std::pow(1.15, static_cast<double>(io.MouseWheel)), 1e-3, 1e5);
        double ax, ay;
        toSketch(io.MousePos, ax, ay);
        k.cx += bx - ax;
        k.cy += by - ay;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !k.drawing) k.fit = true;

    // grid: the smallest of 1, 2, 5 x 10^n that keeps lines 12 px apart
    double step = 1e-3;
    for (int e = -3; e < 7 && step * k.ppm < 12; ++e) {
        for (double m : {1.0, 2.0, 5.0}) {
            step = m * std::pow(10.0, e);
            if (step * k.ppm >= 12) break;
        }
    }
    const bool dark = darkLook();
    dl->AddRectFilled(v0, {v0.x + size.x, v0.y + size.y}, dark ? IM_COL32(32, 34, 38, 255) : IM_COL32(250, 250, 252, 255));
    {
        double x0, y0, x1, y1;
        toSketch(v0, x0, y1);
        toSketch({v0.x + size.x, v0.y + size.y}, x1, y0);
        const ImU32 minor = dark ? IM_COL32(48, 51, 56, 255) : IM_COL32(232, 234, 238, 255);
        const ImU32 major = dark ? IM_COL32(64, 68, 75, 255) : IM_COL32(210, 213, 220, 255);
        for (double x = std::floor(x0 / step) * step; x <= x1; x += step) {
            const bool big = std::fmod(std::fabs(std::round(x / step)), 5.0) < 0.5;
            const float sx = toScreen(x, 0).x;
            dl->AddLine({sx, v0.y}, {sx, v0.y + size.y}, big ? major : minor);
        }
        for (double y = std::floor(y0 / step) * step; y <= y1; y += step) {
            const bool big = std::fmod(std::fabs(std::round(y / step)), 5.0) < 0.5;
            const float sy = toScreen(0, y).y;
            dl->AddLine({v0.x, sy}, {v0.x + size.x, sy}, big ? major : minor);
        }
        const ImVec2 o = toScreen(0, 0);
        dl->AddLine({v0.x, o.y}, {v0.x + size.x, o.y}, IM_COL32(220, 70, 70, 200), 1.5f);  // x axis
        dl->AddLine({o.x, v0.y}, {o.x, v0.y + size.y}, IM_COL32(60, 170, 80, 200), 1.5f);  // y axis
    }

    // closed profiles shaded (what an extrude will use), then the lines and points
    const ImVec4 c = accent_;
    for (std::size_t i = 0; i < k.profiles.size();) {
        const std::size_t n = static_cast<std::size_t>(k.profiles[i++]);
        std::vector<ImVec2> pts;
        for (std::size_t j = 0; j < n && i + 1 < k.profiles.size(); ++j, i += 2) pts.push_back(toScreen(k.profiles[i], k.profiles[i + 1]));
        if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.16f}));
    }
    const ImU32 ink = dark ? IM_COL32(235, 236, 240, 255) : IM_COL32(25, 27, 32, 255);
    for (std::size_t i = 0; i + 3 < k.lines.size(); i += 4) dl->AddLine(toScreen(k.lines[i], k.lines[i + 1]), toScreen(k.lines[i + 2], k.lines[i + 3]), ink, 2.0f);
    for (std::size_t i = 0; i + 3 < k.construction.size(); i += 4)
        dl->AddLine(toScreen(k.construction[i], k.construction[i + 1]), toScreen(k.construction[i + 2], k.construction[i + 3]), IM_COL32(60, 120, 230, 200), 1.0f);
    for (std::size_t i = 0; i + 1 < k.points.size(); i += 2) dl->AddCircleFilled(toScreen(k.points[i], k.points[i + 1]), 3.0f, ink);
    for (std::size_t i = 0; i + 1 < k.free.size(); i += 2) dl->AddCircleFilled(toScreen(k.free[i], k.free[i + 1]), 3.0f, IM_COL32(60, 120, 230, 255));

    // tools' letters
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
        for (const auto& t : kSketchTools) {
            if (ImGui::IsKeyPressed(t.key, false)) {
                k.tool = t.tool;
                k.drawing = false;
            }
        }
    }
    if (k.drawing && (ImGui::IsKeyPressed(ImGuiKey_Escape) || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)))) k.drawing = false;

    // where the mouse is, snapped: to a point of the sketch within 10 px, else to the grid (Alt: free)
    double mx, my;
    toSketch(io.MousePos, mx, my);
    bool onPoint = false;
    auto snapTo = [&](const NumberList& l) {
        for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
            const ImVec2 s = toScreen(l[i], l[i + 1]);
            const float dx = s.x - io.MousePos.x, dy = s.y - io.MousePos.y;
            if (dx * dx + dy * dy < 100) {
                mx = l[i], my = l[i + 1];
                onPoint = true;
                return;
            }
        }
    };
    snapTo(k.points);
    if (!onPoint) snapTo(k.free);
    if (!onPoint && k.drawing && k.tool == SketchEdit::Tool::Line) snapTo(NumberList{k.chainX, k.chainY});
    if (!onPoint && !io.KeyAlt) {
        mx = std::round(mx / step) * step;
        my = std::round(my / step) * step;
    }
    if (hovered) {
        const ImVec2 s = toScreen(mx, my);
        dl->AddCircle(s, onPoint ? 6.0f : 4.0f, ImGui::ColorConvertFloat4ToU32(c), 0, 2.0f);
    }

    auto report = [&](Result r, const char* what) {
        resultFor_ = what;
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
    };
    auto base = [&]() { return Params{{"doc", k.doc}, {"sketch", k.sketch}}; };
    auto finishShape = [&](double bx, double by) {
        if (k.tool == SketchEdit::Tool::Line) {
            if (std::fabs(bx - k.ax) < 1e-9 && std::fabs(by - k.ay) < 1e-9) return;
            Params p = base();
            p["x1"] = num(k.ax), p["y1"] = num(k.ay), p["x2"] = num(bx), p["y2"] = num(by);
            Result r = run("cad.sketch_line", p);
            const bool ok = r.ok;
            report(std::move(r), "Line");
            // keep going from the end, unless it closed the shape
            const bool closed = std::fabs(bx - k.chainX) < 1e-9 && std::fabs(by - k.chainY) < 1e-9;
            k.drawing = ok && !closed;
            k.ax = bx, k.ay = by;
            return;
        }
        k.drawing = false;
        if (k.tool == SketchEdit::Tool::Rect) {
            const double w = std::fabs(bx - k.ax), h = std::fabs(by - k.ay);
            if (w < 1e-9 || h < 1e-9) return;
            Params p = base();
            p["x"] = num(std::min(k.ax, bx)), p["y"] = num(std::min(k.ay, by)), p["width"] = num(w), p["height"] = num(h);
            report(run("cad.sketch_rect", p), "Rectangle");
        } else {
            const double r = std::hypot(bx - k.ax, by - k.ay);
            if (r < 1e-9) return;
            Params p = base();
            p["cx"] = num(k.ax), p["cy"] = num(k.ay), p["diameter"] = num(2 * r);
            report(run("cad.sketch_circle", p), "Circle");
        }
    };

    const bool press = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (press) {
        if (!k.drawing) {
            k.drawing = true;
            k.ax = k.chainX = mx;
            k.ay = k.chainY = my;
        } else {
            finishShape(mx, my);
        }
        k.pressAt = io.MousePos;
    }
    // a drag (rather than two clicks) makes a shape in one go; a dragged line keeps going from its end
    if (k.drawing && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const float dx = io.MousePos.x - k.pressAt.x, dy = io.MousePos.y - k.pressAt.y;
        if (dx * dx + dy * dy > 64) finishShape(mx, my);
    }

    // the shape being drawn, with its size
    if (k.drawing) {
        const ImU32 draft = ImGui::ColorConvertFloat4ToU32(c);
        const ImVec2 a = toScreen(k.ax, k.ay), b = toScreen(mx, my);
        std::string label;
        if (k.tool == SketchEdit::Tool::Line) {
            dl->AddLine(a, b, draft, 2.0f);
            label = num(std::round(std::hypot(mx - k.ax, my - k.ay) * 1000) / 1000) + " " + k.units;
        } else if (k.tool == SketchEdit::Tool::Rect) {
            dl->AddRect({std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}, draft, 0, 0, 2.0f);
            label = num(std::round(std::fabs(mx - k.ax) * 1000) / 1000) + " x " + num(std::round(std::fabs(my - k.ay) * 1000) / 1000) + " " + k.units;
        } else {
            const double r = std::hypot(mx - k.ax, my - k.ay);
            dl->AddCircle(a, static_cast<float>(r * k.ppm), draft, 96, 2.0f);
            dl->AddLine(a, b, draft, 1.0f);
            label = "diameter " + num(std::round(2 * r * 1000) / 1000) + " " + k.units;
        }
        dl->AddText({b.x + 12, b.y + 10}, ink, label.c_str());
    } else if (hovered) {
        const std::string at = num(std::round(mx * 1000) / 1000) + ", " + num(std::round(my * 1000) / 1000);
        dl->AddText({io.MousePos.x + 12, io.MousePos.y + 10}, IM_COL32(128, 128, 128, 255), at.c_str());
    }
    const char* hint = k.tool == SketchEdit::Tool::Line   ? "Line: click to start, click for each corner; click the first point, right-click or Esc to stop"
                       : k.tool == SketchEdit::Tool::Rect ? "Rectangle: drag corner to corner"
                                                         : "Circle: drag from the centre";
    dl->AddText({v0.x + 10, v0.y + size.y - ImGui::GetFontSize() - 8}, IM_COL32(128, 128, 128, 255), hint);
}

}  // namespace wetwaresuite::gui
