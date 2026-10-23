// The vector tools on the canvas. Pick clicks a shape (vector.hit), drags it
// (vector.transform dx/dy, snapped by vector.snap) or one of the eight handles
// round its box (vector.transform scale_x/scale_y about the opposite side).
// Anchors shows the picked path's anchors and handles (vector.nodes) and drags
// them (vector.move_node); Alt+click on the path adds an anchor. Everything
// goes through commands, so it is undoable and the same as the CLI and MCP.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

std::string fieldText(const Result& r, const char* key) {
    const Value* v = r.field(key);
    if (!v) return {};
    if (auto s = std::get_if<std::string>(v)) return *s;
    return toText(*v);
}

double fieldNum(const Result& r, const char* key) {
    const Value* v = r.field(key);
    if (!v) return 0;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return 0;
}

// "sub:x,y,ix,iy,ox,oy:type;..." from vector.nodes
std::vector<float> parseNodes(const std::string& text) {
    std::vector<float> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(at, end - at);
        const std::size_t a = item.find(':'), b = item.rfind(':');
        if (a != std::string::npos && b != a) {
            float v[6];
            if (std::sscanf(item.substr(a + 1, b - a - 1).c_str(), "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                out.insert(out.end(), v, v + 6);
            }
        }
        at = end + 1;
    }
    return out;
}

// the eight scale handles: corners then edge middles, as fractions of the box
const float kHandle[8][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0.5f, 0}, {1, 0.5f}, {0.5f, 1}, {0, 0.5f}};

}  // namespace

bool Shell::vectorDoc() const {
    auto doc = session_.workspace().get(currentDoc_);
    return doc && doc->kind() == "vector" && find("vector.hit") != nullptr;
}

namespace {

// Drawing units per picture px: the canvas shows the drawing's preview, which may be smaller or larger.
float unitsPerPx(const std::string& summary, const Texture& t) {
    int w = 0, h = 0;
    if (t.srcWidth <= 0 || std::sscanf(summary.c_str(), "%dx%d", &w, &h) != 2 || w <= 0) return 1;
    return static_cast<float>(w) / static_cast<float>(t.srcWidth);
}

}  // namespace

void Shell::vectorSettings() {
    // reload the picked shape's box and anchors after any change (undo included)
    if (canvas_.vecLoaded != changes_ || canvas_.shownDoc != currentDoc_) {
        canvas_.vecLoaded = changes_;
        canvas_.vecHasBox = false;
        canvas_.vecNodes.clear();
        if (!canvas_.vecSel.empty()) {
            Result b = query("vector.bounds", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}});
            if (b.ok) {
                canvas_.vecHasBox = true;
                canvas_.vecBox[0] = static_cast<float>(fieldNum(b, "x"));
                canvas_.vecBox[1] = static_cast<float>(fieldNum(b, "y"));
                canvas_.vecBox[2] = static_cast<float>(fieldNum(b, "width"));
                canvas_.vecBox[3] = static_cast<float>(fieldNum(b, "height"));
                if (canvas_.tool == CanvasTool::Anchor) {
                    Result n = query("vector.nodes", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}});
                    if (n.ok) canvas_.vecNodes = parseNodes(fieldText(n, "nodes"));
                }
            } else {
                canvas_.vecSel.clear();
            }
        }
    }
    if (canvas_.vecSel.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(canvas_.tool == CanvasTool::Anchor ? "  Click a path to edit its anchors" : "  Click a shape to pick it");
        return;
    }
    auto edit = [this](const char* id, Params p, const char* title) {
        p["doc"] = currentDoc_;
        p["shape"] = canvas_.vecSel;
        Result r = run(id, p);
        resultFor_ = title;
        if (r.ok && std::string(id) == "vector.duplicate") canvas_.vecSel = fieldText(r, "shape");
        if (r.ok && std::string(id) == "vector.delete") canvas_.vecSel.clear();
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
    };
    ImGui::SameLine();
    ImGui::Text("  %s", canvas_.vecSel.c_str());
    const struct {
        const char* label;
        const char* key;
        const char* value;
        const char* tip;
    } aligns[] = {{"|<", "horizontal", "left", "Align left (to the canvas)"},     {"|", "horizontal", "center", "Centre across"},
                  {">|", "horizontal", "right", "Align right"},                   {"T", "vertical", "top", "Align top"},
                  {"-", "vertical", "middle", "Centre up and down"},             {"_", "vertical", "bottom", "Align bottom"}};
    for (const auto& a : aligns) {
        ImGui::SameLine();
        if (ImGui::SmallButton(a.label)) edit("vector.align", {{a.key, std::string(a.value)}}, "Align");
        ImGui::SetItemTooltip("%s", a.tip);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Front")) edit("vector.reorder", {{"to", std::string("front")}}, "Arrange");
    ImGui::SameLine();
    if (ImGui::SmallButton("Back")) edit("vector.reorder", {{"to", std::string("back")}}, "Arrange");
    ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate")) edit("vector.duplicate", {{"dx", 10.0}, {"dy", 10.0}}, "Duplicate");
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) edit("vector.delete", {}, "Delete");
}

void Shell::vectorPress(ImVec2 screen) {
    const float u = unitsPerPx(canvas_.summary, canvas_.texture);
    const ImVec2 img = toImage(screen);
    const ImVec2 at{img.x * u, img.y * u};
    const float reach = 6 * u / std::max(canvas_.zoom, 0.01f);  // 6 screen px in drawing units
    canvas_.vecStart = canvas_.vecNow = at;
    canvas_.vecDrag = 0;
    if (canvas_.tool == CanvasTool::Anchor && !canvas_.vecNodes.empty()) {
        const auto& n = canvas_.vecNodes;
        float best = reach;
        for (std::size_t i = 0; i + 5 < n.size(); i += 6) {
            for (int part = 0; part < 3; ++part) {
                const float d = std::hypot(n[i + part * 2] - at.x, n[i + part * 2 + 1] - at.y);
                if (d < best) {
                    best = d;
                    canvas_.vecDrag = 3;
                    canvas_.vecHandle = static_cast<int>(i / 6) * 3 + part;
                }
            }
        }
        if (canvas_.vecDrag) return;
        if (ImGui::GetIO().KeyAlt) {  // Alt+click on the path: a new anchor there
            Result r = run("vector.add_node", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}, {"x", static_cast<double>(at.x)}, {"y", static_cast<double>(at.y)}});
            resultFor_ = "Add anchor";
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
            return;
        }
    }
    if (canvas_.tool == CanvasTool::Pick && canvas_.vecHasBox) {
        const float* b = canvas_.vecBox;
        for (int h = 0; h < 8; ++h) {
            if (std::hypot(b[0] + b[2] * kHandle[h][0] - at.x, b[1] + b[3] * kHandle[h][1] - at.y) < reach) {
                canvas_.vecDrag = 2;
                canvas_.vecHandle = h;
                return;
            }
        }
    }
    Result hit = query("vector.hit", {{"doc", currentDoc_}, {"x", static_cast<double>(at.x)}, {"y", static_cast<double>(at.y)},
                                      {"tolerance", static_cast<double>(reach)}});
    const std::string picked = fieldText(hit, canvas_.tool == CanvasTool::Anchor ? "shape" : "select");
    if (picked != canvas_.vecSel) {
        canvas_.vecSel = picked;
        canvas_.vecLoaded = ~0ull;  // read its box and anchors
    }
    if (!picked.empty() && canvas_.tool == CanvasTool::Pick) canvas_.vecDrag = 1;
}

void Shell::vectorDrag(ImVec2 screen) {
    const float u = unitsPerPx(canvas_.summary, canvas_.texture);
    const ImVec2 img = toImage(screen);
    canvas_.vecNow = {img.x * u, img.y * u};
    if (canvas_.vecDrag == 1 && canvas_.vecHasBox) {  // snap the box's corner to the grid, guides or anchors
        const float dx = canvas_.vecNow.x - canvas_.vecStart.x, dy = canvas_.vecNow.y - canvas_.vecStart.y;
        Result s = query("vector.snap", {{"doc", currentDoc_}, {"x", static_cast<double>(canvas_.vecBox[0] + dx)},
                                         {"y", static_cast<double>(canvas_.vecBox[1] + dy)}, {"radius", static_cast<double>(8 * u / canvas_.zoom)}});
        if (s.ok && fieldText(s, "snapped") != "none") {
            canvas_.vecNow.x = static_cast<float>(fieldNum(s, "x")) - canvas_.vecBox[0] + canvas_.vecStart.x;
            canvas_.vecNow.y = static_cast<float>(fieldNum(s, "y")) - canvas_.vecBox[1] + canvas_.vecStart.y;
        }
    }
}

void Shell::vectorRelease() {
    const int drag = canvas_.vecDrag;
    canvas_.vecDrag = 0;
    const float dx = canvas_.vecNow.x - canvas_.vecStart.x, dy = canvas_.vecNow.y - canvas_.vecStart.y;
    if (!drag || canvas_.vecSel.empty() || (std::abs(dx) < 1e-3f && std::abs(dy) < 1e-3f)) return;
    Params p{{"doc", currentDoc_}};
    std::string id, title;
    if (drag == 1) {
        id = "vector.transform", title = "Move";
        p["shape"] = canvas_.vecSel;
        p["dx"] = static_cast<double>(dx);
        p["dy"] = static_cast<double>(dy);
    } else if (drag == 2) {
        const float* b = canvas_.vecBox;
        const float hx = kHandle[canvas_.vecHandle][0], hy = kHandle[canvas_.vecHandle][1];
        const float ox = b[0] + b[2] * (1 - hx), oy = b[1] + b[3] * (1 - hy);  // the opposite side stays put
        double sx = hx == 0.5f || b[2] <= 0 ? 1.0 : (b[0] + b[2] * hx + dx - ox) / (b[0] + b[2] * hx - ox);
        double sy = hy == 0.5f || b[3] <= 0 ? 1.0 : (b[1] + b[3] * hy + dy - oy) / (b[1] + b[3] * hy - oy);
        if (ImGui::GetIO().KeyShift && hx != 0.5f && hy != 0.5f) sx = sy = std::max(std::abs(sx), std::abs(sy));
        if (std::abs(sx) < 1e-3 || std::abs(sy) < 1e-3) return;
        id = "vector.transform", title = "Scale";
        p["shapes"] = canvas_.vecSel;
        p["scale_x"] = sx;
        p["scale_y"] = sy;
        p["origin_x"] = static_cast<double>(ox);
        p["origin_y"] = static_cast<double>(oy);
    } else {
        const int node = canvas_.vecHandle / 3, part = canvas_.vecHandle % 3;
        id = "vector.move_node", title = "Move anchor";
        p["shape"] = canvas_.vecSel;
        p["node"] = std::to_string(node);
        p["part"] = std::string(part == 0 ? "anchor" : part == 1 ? "in" : "out");
        p["dx"] = static_cast<double>(dx);
        p["dy"] = static_cast<double>(dy);
        if (ImGui::GetIO().KeyAlt) p["corner"] = true;  // Alt breaks a smooth pair
    }
    Result r = run(id, p);
    resultFor_ = title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::vectorKeys() {
    if (canvas_.vecSel.empty() || ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        Result r = run("vector.delete", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}});
        canvas_.vecSel.clear();
        resultFor_ = "Delete";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        return;
    }
    const struct {
        ImGuiKey key;
        double dx, dy;
    } nudges[] = {{ImGuiKey_LeftArrow, -1, 0}, {ImGuiKey_RightArrow, 1, 0}, {ImGuiKey_UpArrow, 0, -1}, {ImGuiKey_DownArrow, 0, 1}};
    for (const auto& n : nudges) {
        if (!ImGui::IsKeyPressed(n.key)) continue;
        const double step = ImGui::GetIO().KeyShift ? 10 : 1;
        Result r = run("vector.transform", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}, {"dx", n.dx * step}, {"dy", n.dy * step}});
        if (!r.ok) {
            resultFor_ = "Nudge";
            r.message = friendlyError(r.message);
            result_ = std::move(r);
        }
    }
}

void Shell::drawVector(ImDrawList* dl, ImVec2 origin) {
    if (canvas_.vecSel.empty() || !canvas_.vecHasBox) return;
    const float u = unitsPerPx(canvas_.summary, canvas_.texture);
    const float k = canvas_.zoom / u;  // screen px per drawing unit
    auto screen = [&](float x, float y) { return ImVec2{origin.x + x * k, origin.y + y * k}; };
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(accent_);
    float b[4] = {canvas_.vecBox[0], canvas_.vecBox[1], canvas_.vecBox[2], canvas_.vecBox[3]};
    const float dx = canvas_.vecNow.x - canvas_.vecStart.x, dy = canvas_.vecNow.y - canvas_.vecStart.y;
    if (canvas_.vecDrag == 1) b[0] += dx, b[1] += dy;
    if (canvas_.vecDrag == 2) {  // the box as it will be
        const float hx = kHandle[canvas_.vecHandle][0], hy = kHandle[canvas_.vecHandle][1];
        if (hx == 0) b[0] += dx, b[2] -= dx;
        if (hx == 1) b[2] += dx;
        if (hy == 0) b[1] += dy, b[3] -= dy;
        if (hy == 1) b[3] += dy;
    }
    dl->AddRect(screen(b[0], b[1]), screen(b[0] + b[2], b[1] + b[3]), accent, 0, 0, 1.5f);
    if (canvas_.tool == CanvasTool::Pick) {
        for (const auto& h : kHandle) {
            const ImVec2 c = screen(b[0] + b[2] * h[0], b[1] + b[3] * h[1]);
            dl->AddRectFilled({c.x - 4, c.y - 4}, {c.x + 4, c.y + 4}, IM_COL32(255, 255, 255, 255));
            dl->AddRect({c.x - 4, c.y - 4}, {c.x + 4, c.y + 4}, accent);
        }
        return;
    }
    const auto& n = canvas_.vecNodes;
    for (std::size_t i = 0; i + 5 < n.size(); i += 6) {
        float v[6];
        std::copy(n.begin() + static_cast<std::ptrdiff_t>(i), n.begin() + static_cast<std::ptrdiff_t>(i) + 6, v);
        if (canvas_.vecDrag == 3 && static_cast<int>(i / 6) == canvas_.vecHandle / 3) {
            const int part = canvas_.vecHandle % 3;
            if (part == 0) {
                for (int q = 0; q < 3; ++q) v[q * 2] += dx, v[q * 2 + 1] += dy;
            } else {
                v[part * 2] += dx, v[part * 2 + 1] += dy;
            }
        }
        const ImVec2 a = screen(v[0], v[1]);
        for (int part = 1; part < 3; ++part) {
            if (std::abs(v[part * 2] - v[0]) < 1e-4f && std::abs(v[part * 2 + 1] - v[1]) < 1e-4f) continue;
            const ImVec2 hnd = screen(v[part * 2], v[part * 2 + 1]);
            dl->AddLine(a, hnd, accent, 1.0f);
            dl->AddCircleFilled(hnd, 3.5f, accent);
        }
        dl->AddRectFilled({a.x - 3.5f, a.y - 3.5f}, {a.x + 3.5f, a.y + 3.5f}, IM_COL32(255, 255, 255, 255));
        dl->AddRect({a.x - 3.5f, a.y - 3.5f}, {a.x + 3.5f, a.y + 3.5f}, accent, 0, 0, 1.5f);
    }
}

}  // namespace wetwaresuite::gui
