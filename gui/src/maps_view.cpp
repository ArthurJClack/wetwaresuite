// Map documents on the canvas: click tools that run maps.* commands. Every
// click asks maps.hit what is under it (and where that is), then runs the
// ordinary command (feature_add, vertex_move, feature_delete, route, measure,
// view), so it can be undone and scripts or Claude see the same steps.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "tool_panels.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

bool Shell::mapsDoc() const {
    if (!find("maps.hit")) return false;
    auto doc = session_.workspace().get(currentDoc_);
    return doc && doc->kind() == "map";
}

bool Shell::mapsEditing() const { return mapsDoc() && maps_.tool != "pan"; }

void Shell::mapsToolbar() {
    if (maps_.doc != currentDoc_) maps_ = MapsCanvas{currentDoc_};
    auto show = [&](Result r, const std::string& what) {
        resultFor_ = what;
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        canvas_.dirty = true;
    };
    struct ToolDef {
        const char* label;
        const char* tool;
        const char* tip;
    };
    const ToolDef tools[] = {{"Pan", "pan", "Drag to move around, scroll to zoom the picture"},
                             {"Identify", "identify", "Click a feature to see its properties"},
                             {"Measure", "measure", "Click points to measure a distance; with 3 or more, the area too. Right-click or Esc starts again"},
                             {"Point", "point", "Click to add a point to the layer named on the right"},
                             {"Line", "line", "Click each corner of a line; right-click or Enter finishes it"},
                             {"Area", "area", "Click each corner of an area; right-click or Enter finishes it"},
                             {"Move", "move", "Click a point or a line's corner, then where it should go"},
                             {"Delete", "delete", "Click a feature to delete it (Alt+click a corner deletes just that corner)"},
                             {"Route", "route", "Click the start, then the end: a route along the roads layer"}};
    ImGui::SameLine();
    ImGui::TextUnformatted("  Tool:");
    for (const auto& t : tools) {
        ImGui::SameLine();
        const float need = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(t.label).x;
        if (ImGui::GetContentRegionAvail().x < need) ImGui::NewLine();
        if (ImGui::RadioButton(t.label, maps_.tool == t.tool)) {
            maps_.tool = t.tool;
            maps_.points.clear();
            maps_.pointsPx.clear();
            maps_.movingLayer.clear();
        }
        ImGui::SetItemTooltip("%s", t.tip);
    }
    const float unit = ImGui::GetFontSize();
    if (maps_.tool == "point" || maps_.tool == "line" || maps_.tool == "area") {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 7);
        ImGui::InputTextWithHint("##layer", "layer", &maps_.layer);
        ImGui::SetItemTooltip("The layer new features go into (made when missing)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 8);
        ImGui::InputTextWithHint("##props", "name=...", &maps_.props);
        ImGui::SetItemTooltip("Properties for the new feature, e.g. name=Home; visits=3");
    }
    if (maps_.tool == "route") {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 5);
        if (ImGui::BeginCombo("##profile", maps_.profile.c_str())) {
            for (const char* p : {"car", "bike", "foot"}) {
                if (ImGui::Selectable(p, maps_.profile == p)) maps_.profile = p;
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("How you travel");
    }
    // the view: zoom, everything, bookmarks, search
    ImGui::NewLine();
    auto view = [&](Params p, const char* what) {
        p["doc"] = currentDoc_;
        Result r = run("maps.view", p);
        show(std::move(r), what);
        canvas_.fit = true;
    };
    if (ImGui::Button("Zoom in")) view({{"zoom", 2.0}}, "Zoom in");
    ImGui::SameLine();
    if (ImGui::Button("Zoom out")) view({{"zoom", 0.5}}, "Zoom out");
    ImGui::SameLine();
    if (ImGui::Button("Show all")) view({{"reset", true}}, "Show all");
    ImGui::SetItemTooltip("Show every layer again");
    for (const auto& [label, dx, dy] : {std::tuple<const char*, double, double>{"<", -0.4, 0}, {">", 0.4, 0}, {"^", 0, 0.4}, {"v", 0, -0.4}}) {
        ImGui::SameLine();
        if (ImGui::SmallButton(label)) view({{"pan", std::vector<double>{dx, dy}}}, "Pan");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 8);
    if (ImGui::BeginCombo("##bookmarks", "Bookmarks")) {
        Result b = query("maps.bookmarks", {{"doc", currentDoc_}});
        const std::string names = tp::text(b, "names");
        std::size_t at = 0;
        while (b.ok && at < names.size() && names != "none yet") {
            std::size_t end = names.find(", ", at);
            if (end == std::string::npos) end = names.size();
            const std::string name = names.substr(at, end - at);
            if (ImGui::Selectable(name.c_str())) view({{"bookmark", name}}, "Go to bookmark");
            at = end + 2;
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(unit * 7);
        ImGui::InputTextWithHint("##bmname", "name", &maps_.bookmark);
        ImGui::SameLine();
        if (ImGui::SmallButton("Save this view") && !maps_.bookmark.empty()) {
            show(run("maps.bookmark", {{"doc", currentDoc_}, {"name", maps_.bookmark}}), "Bookmark");
            maps_.bookmark.clear();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 9);
    const bool go = ImGui::InputTextWithHint("##search", "search places", &maps_.search, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Find") || go) && !maps_.search.empty()) {
        Result r = query("maps.search", {{"doc", currentDoc_}, {"query", maps_.search}});
        if (r.ok) {
            const double lat = tp::number(r, "lat"), lon = tp::number(r, "lon");
            Result v = run("maps.view", {{"doc", currentDoc_}, {"center", std::vector<double>{lat, lon}}, {"radius_km", 1.5}});
            if (!v.ok) r = v;
            canvas_.fit = true;
        }
        show(std::move(r), "Search");
    }
    if (maps_.tool == "measure" && maps_.points.size() >= 6) {
        ImGui::SameLine();
        if (ImGui::Button("Area")) {
            show(query("maps.measure", {{"points", maps_.points}, {"polygon", true}}), "Measure area");
        }
    }
}

void Shell::mapsFinish() {
    // a line or area being drawn becomes a feature
    const std::size_t n = maps_.points.size() / 2;
    if ((maps_.tool == "line" && n >= 2) || (maps_.tool == "area" && n >= 3)) {
        Params p{{"doc", currentDoc_}, {"points", maps_.points}, {"kind", std::string(maps_.tool == "line" ? "line" : "polygon")}};
        if (!maps_.layer.empty()) p["layer"] = maps_.layer;
        if (!maps_.props.empty()) p["props"] = maps_.props;
        Result r = run("maps.feature_add", p);
        resultFor_ = maps_.tool == "line" ? "Add line" : "Add area";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        canvas_.dirty = true;
    }
    maps_.points.clear();
    maps_.pointsPx.clear();
}

void Shell::mapsClick(ImVec2 picture) {
    auto show = [&](Result r, const std::string& what) {
        resultFor_ = what;
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        canvas_.dirty = true;
    };
    Result hit = query("maps.hit", {{"doc", currentDoc_}, {"x", static_cast<double>(picture.x)}, {"y", static_cast<double>(picture.y)}});
    if (!hit.ok) return show(hit, "Click");
    const double lat = tp::number(hit, "lat"), lon = tp::number(hit, "lon");
    const std::string kind = tp::text(hit, "kind"), layer = tp::text(hit, "layer");
    const auto index = static_cast<std::int64_t>(tp::number(hit, "index", -1));
    const auto vertex = static_cast<std::int64_t>(tp::number(hit, "vertex", -1));
    const std::string& tool = maps_.tool;
    if (tool == "identify") return show(hit, "What's here");
    if (tool == "point") {
        Params p{{"doc", currentDoc_}, {"lat", lat}, {"lon", lon}};
        if (!maps_.layer.empty()) p["layer"] = maps_.layer;
        if (!maps_.props.empty()) p["props"] = maps_.props;
        return show(run("maps.feature_add", p), "Add point");
    }
    if (tool == "line" || tool == "area" || tool == "measure" || tool == "route") {
        maps_.points.insert(maps_.points.end(), {lat, lon});
        maps_.pointsPx.push_back(picture);
        if (tool == "measure") {
            if (maps_.points.size() < 4) {
                hit.message = "measuring from " + std::to_string(lat).substr(0, 9) + "," + std::to_string(lon).substr(0, 9) + ": click the next point";
                return show(hit, "Measure");
            }
            return show(query("maps.measure", {{"points", maps_.points}}), "Measure");
        }
        if (tool == "route") {
            if (maps_.points.size() < 4) {
                hit.message = "route starts here: click where it should go";
                return show(hit, "Route");
            }
            Result r = run("maps.route", {{"doc", currentDoc_},
                                          {"from", std::vector<double>{maps_.points[0], maps_.points[1]}},
                                          {"to", std::vector<double>{lat, lon}},
                                          {"profile", maps_.profile}});
            maps_.points.clear();
            maps_.pointsPx.clear();
            return show(r, "Route by " + maps_.profile);
        }
        hit.message = std::to_string(maps_.points.size() / 2) + " points: keep clicking, right-click or Enter to finish";
        return show(hit, tool == "line" ? "Line" : "Area");
    }
    if (tool == "delete") {
        if (kind == "none") return show(hit, "Nothing to delete here");
        if (ImGui::GetIO().KeyAlt && kind == "vertex") {
            return show(run("maps.vertex_delete", {{"doc", currentDoc_}, {"layer", layer}, {"index", index}, {"vertex", vertex}}), "Delete corner");
        }
        return show(run("maps.feature_delete", {{"doc", currentDoc_}, {"layer", layer}, {"index", index}}), "Delete " + layer + " #" + std::to_string(index));
    }
    if (tool == "move") {
        if (maps_.movingLayer.empty()) {
            if (kind == "none") return show(hit, "Click a point or a corner to move");
            maps_.movingLayer = layer;
            maps_.movingIndex = index;
            maps_.movingVertex = kind == "vertex" ? vertex : -1;
            maps_.movingFrom = picture;
            hit.message = "picked up " + layer + " #" + std::to_string(index) + (kind == "vertex" ? " corner " + std::to_string(vertex) : "") + ": click where it goes";
            return show(hit, "Move");
        }
        Params p{{"doc", currentDoc_}, {"layer", maps_.movingLayer}, {"index", maps_.movingIndex}, {"to", std::vector<double>{lat, lon}}};
        std::string command = "maps.feature_move";
        if (maps_.movingVertex >= 0) {
            command = "maps.vertex_move";
            p["vertex"] = maps_.movingVertex;
        }
        maps_.movingLayer.clear();
        return show(run(command, p), "Move");
    }
}

void Shell::mapsCanvas(ImDrawList* dl, ImVec2 origin, bool hovered, bool clicked) {
    if (maps_.doc != currentDoc_) maps_ = MapsCanvas{currentDoc_};
    if (maps_.tool == "pan") return;
    ImGuiIO& io = ImGui::GetIO();
    const bool drawing = maps_.tool == "line" || maps_.tool == "area";
    if (!maps_.points.empty() && (ImGui::IsKeyPressed(ImGuiKey_Escape) || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ||
                                  (drawing && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))))) {
        if (drawing && !ImGui::IsKeyPressed(ImGuiKey_Escape)) mapsFinish();
        maps_.points.clear();
        maps_.pointsPx.clear();
    }
    if (!maps_.movingLayer.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape)) maps_.movingLayer.clear();
    if (clicked && !ImGui::IsKeyDown(ImGuiKey_Space)) mapsClick(toImage(io.MousePos));
    // what is being drawn follows the mouse
    const ImU32 col = maps_.tool == "measure" ? IM_COL32(230, 120, 20, 230) : maps_.tool == "route" ? IM_COL32(210, 50, 50, 230) : IM_COL32(40, 110, 210, 230);
    std::vector<ImVec2> pts;
    for (const auto& p : maps_.pointsPx) pts.push_back({origin.x + p.x * canvas_.zoom, origin.y + p.y * canvas_.zoom});
    if (hovered && !pts.empty()) pts.push_back(io.MousePos);
    if (pts.size() >= 2) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, maps_.tool == "area" ? ImDrawFlags_Closed : ImDrawFlags_None, 2.5f);
    for (std::size_t i = 0; i < maps_.pointsPx.size(); ++i) dl->AddCircleFilled(pts[i], 4, col);
    if (!maps_.movingLayer.empty()) {
        const ImVec2 a{origin.x + maps_.movingFrom.x * canvas_.zoom, origin.y + maps_.movingFrom.y * canvas_.zoom};
        dl->AddCircle(a, 7, col, 0, 2);
        if (hovered) dl->AddLine(a, io.MousePos, col, 1.5f);
    }
}

}  // namespace wetwaresuite::gui
