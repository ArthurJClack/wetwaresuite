// The PDF editor: one page at a time with thumbnails, and a toolbar of tools
// (select and move, add or edit text, highlight, notes, drawing, shapes,
// white-out, redaction, signatures, pictures, form filling). Every tool ends in
// a pdf.* command through the Session, so undo, scripts and MCP see the same steps.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "tool_panels.hpp"
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

using Tool = PdfPanel::Tool;

struct ToolButton {
    Tool tool;
    const char* label;
    const char* tip;
};

const ToolButton kTools[] = {
    {Tool::Select, "Select", "Click an edit to pick it, drag to move it, Delete removes it. Click a form box to fill it in."},
    {Tool::Text, "Add text", "Click where the text should go"},
    {Tool::EditText, "Edit text", "Click a line of the PDF's own text to change it"},
    {Tool::Highlight, "Highlight", "Drag over text"},
    {Tool::Underline, "Underline", "Drag over text"},
    {Tool::Strikeout, "Strike out", "Drag over text"},
    {Tool::Note, "Note", "Click to pin a comment"},
    {Tool::Draw, "Draw", "Draw freehand with the mouse or a pen"},
    {Tool::Box, "Box", "Drag a rectangle"},
    {Tool::Ellipse, "Ellipse", "Drag an ellipse"},
    {Tool::Line, "Line", "Drag a line"},
    {Tool::Arrow, "Arrow", "Drag an arrow"},
    {Tool::Whiteout, "White-out", "Drag to cover something with white"},
    {Tool::Redact, "Redact", "Drag over what must go: the text under it is removed for good when you save"},
    {Tool::Sign, "Sign", "Drag (or click) where the signature goes"},
    {Tool::Picture, "Picture", "Click where a picture goes, then pick the file on the right"},
};

std::string hexOf(const float c[4]) {
    auto b = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", b(c[0]), b(c[1]), b(c[2]));
    return buf;
}

double num(const std::vector<std::string>& row, std::size_t i) {
    if (i >= row.size()) return 0;
    try {
        return std::stod(row[i]);
    } catch (...) {
        return 0;
    }
}

bool inside(double x, double y, double bx, double by, double bw, double bh, double pad = 0) {
    return x >= bx - pad && x <= bx + bw + pad && y >= by - pad && y <= by + bh + pad;
}

}  // namespace

void Shell::pdfRun(const std::string& command, Params params, const std::string& title) {
    PdfPanel& v = panels_->pdf;
    params["doc"] = v.doc;
    Result r = run(command, params);
    resultFor_ = title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::pdfLoad(bool force) {
    PdfPanel& v = panels_->pdf;
    const bool changed = v.seen != changes_;
    if (changed || force) {
        v.seen = changes_;
        Result info = query("pdf.info", {{"doc", v.doc}});
        const Value* pages = info.field("pages");
        v.pages = pages && std::holds_alternative<std::int64_t>(*pages) ? static_cast<int>(std::get<std::int64_t>(*pages)) : 0;
        v.page = std::clamp(v.page, 0, std::max(0, v.pages - 1));
        const Params onPage{{"doc", v.doc}, {"page", static_cast<std::int64_t>(v.page + 1)}};
        v.marks = tp::rows(query("pdf.marks", onPage));
        v.lines = tp::rows(query("pdf.text", {{"doc", v.doc}, {"pages", std::to_string(v.page + 1)}}));
        Result fields = query("pdf.fields", {{"doc", v.doc}});
        v.fields = tp::rows(fields);
        v.widgets = tp::rows(fields, "widgets");
        v.shownPage = -1;
    }
    if (v.zoom <= 0) return;  // sized on the first frame of the page view
    // redraw when the page or the zoom changed (the zoom once the wheel stops)
    const bool zoomSettled = tp::seconds() - v.lastZoom > 0.2;
    if (v.shownPage == v.page && (std::abs(v.shownScale - v.zoom) < 0.01f || !zoomSettled) && v.tex.tex) return;
    const double dpi = std::clamp(72.0 * v.zoom, 10.0, 600.0);
    Result r = query("pdf.view", {{"doc", v.doc}, {"page", static_cast<std::int64_t>(v.page + 1)}, {"dpi", dpi}});
    if (!r.ok) return;
    tp::attachmentTexture(renderer_, v.tex, r);
    v.pageW = tp::number(r, "width", 595);
    v.pageH = tp::number(r, "height", 842);
    v.shownPage = v.page;
    v.shownScale = v.zoom;
}

void Shell::pdfView() {
    PdfPanel& v = panels_->pdf;
    if (v.doc != currentDoc_) {
        v = PdfPanel{};
        for (auto& [p, t] : v.thumbs) t.reset();
        v.doc = currentDoc_;
    }
    pdfLoad(false);
    pdfToolbar();
    const float unit = ImGui::GetFontSize();
    const float thumbW = unit * 6.5f;
    ImGui::BeginChild("pdfthumbs", {thumbW, 0}, ImGuiChildFlags_Borders);
    pdfThumbs(thumbW);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("pdfpage", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    pdfPage();
    ImGui::EndChild();
    pdfPopup();
}

void Shell::pdfToolbar() {
    PdfPanel& v = panels_->pdf;
    const float unit = ImGui::GetFontSize();
    // pages and saving
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left) && v.page > 0) --v.page;
    ImGui::SameLine();
    ImGui::Text("Page %d of %d", v.page + 1, v.pages);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right) && v.page + 1 < v.pages) ++v.page;
    ImGui::SameLine();
    if (ImGui::Button("-")) v.zoom = std::max(0.1f, v.zoom / 1.25f), v.lastZoom = tp::seconds();
    ImGui::SameLine();
    ImGui::Text("%d%%", static_cast<int>(std::lround(v.zoom / std::max(0.01f, dpi_) * 100)));
    ImGui::SameLine();
    if (ImGui::Button("+")) v.zoom = std::min(8.0f, v.zoom * 1.25f), v.lastZoom = tp::seconds();
    ImGui::SameLine();
    if (ImGui::Button("Fit")) v.zoom = 0;
    ImGui::SameLine();
    if (accentButton("Save")) pdfRun("pdf.save", {}, "Save");
    ImGui::SetItemTooltip("Write the PDF with every edit (over the file you opened)");
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) {
        select("pdf.save");
        canvas_.fit = false;
    }
    tp::flow("Pages...");
    if (ImGui::Button("Pages...")) ImGui::OpenPopup("pdfpages");
    if (ImGui::BeginPopup("pdfpages")) {
        const std::string here = std::to_string(v.page + 1);
        if (ImGui::MenuItem("Turn this page right")) pdfRun("pdf.page_rotate", {{"pages", here}, {"angle", std::int64_t{90}}}, "Turn page");
        if (ImGui::MenuItem("Turn this page left")) pdfRun("pdf.page_rotate", {{"pages", here}, {"angle", std::int64_t{-90}}}, "Turn page");
        if (ImGui::MenuItem("Add a blank page after this one"))
            pdfRun("pdf.page_insert", {{"after", static_cast<std::int64_t>(v.page + 1)}, {"count", std::int64_t{1}}}, "Insert page");
        if (ImGui::MenuItem("Insert pages from another PDF...")) select("pdf.page_insert");
        if (ImGui::MenuItem("Move this page up", nullptr, false, v.page > 0)) {
            pdfRun("pdf.page_move", {{"page", static_cast<std::int64_t>(v.page + 1)}, {"to", static_cast<std::int64_t>(v.page)}}, "Move page");
            --v.page;
        }
        if (ImGui::MenuItem("Move this page down", nullptr, false, v.page + 1 < v.pages)) {
            pdfRun("pdf.page_move", {{"page", static_cast<std::int64_t>(v.page + 1)}, {"to", static_cast<std::int64_t>(v.page + 2)}}, "Move page");
            ++v.page;
        }
        if (ImGui::MenuItem("Delete this page", nullptr, false, v.pages > 1)) pdfRun("pdf.page_delete", {{"pages", here}}, "Delete page");
        if (ImGui::MenuItem("Save some pages as a new PDF...")) select("pdf.save");
        ImGui::EndPopup();
    }
    tp::flow("Find text in this PDF ");
    ImGui::SetNextItemWidth(unit * 9);
    if (ImGui::InputTextWithHint("##find", "Find text", &v.findInput, ImGuiInputTextFlags_EnterReturnsTrue) && !v.findInput.empty()) {
        Result r = query("pdf.find", {{"doc", v.doc}, {"text", v.findInput}});
        v.found = tp::rows(r);
        v.foundAt = v.found.empty() ? -1 : 0;
        if (!v.found.empty()) v.page = static_cast<int>(num(v.found[0], 0)) - 1;
        resultFor_ = "Find";
        result_ = Result::success(sentence(r.message));
    }
    if (!v.found.empty()) {
        tp::flow("Next match");
        if (ImGui::Button("Next match")) {
            v.foundAt = (v.foundAt + 1) % static_cast<int>(v.found.size());
            v.page = static_cast<int>(num(v.found[static_cast<std::size_t>(v.foundAt)], 0)) - 1;
        }
        tp::flow("Highlight all");
        if (ImGui::Button("Highlight all")) pdfRun("pdf.highlight", {{"text", v.findInput}, {"page", std::int64_t{0}}}, "Highlight");
        tp::flow("Redact all");
        if (ImGui::Button("Redact all")) pdfRun("pdf.redact", {{"text", v.findInput}, {"page", std::int64_t{0}}}, "Redact");
        tp::flow("x");
        if (ImGui::Button("x##clearfind")) v.found.clear(), v.foundAt = -1;
    }

    // tools
    bool first = true;
    for (const auto& t : kTools) {
        if (t.tool == Tool::Picture && !find("pdf.image")) continue;
        tp::flow(t.label, first);
        first = false;
        const bool on = v.tool == t.tool;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, accent_), ImGui::PushStyleColor(ImGuiCol_Text, accentText_);
        if (ImGui::Button(t.label)) {
            v.tool = t.tool;
            v.selected = -1;
            // sensible colours per tool
            auto set = [&](float r, float g, float b) { v.color[0] = r, v.color[1] = g, v.color[2] = b; };
            if (t.tool == Tool::Highlight) set(1.0f, 0.9f, 0.1f);
            else if (t.tool == Tool::Text || t.tool == Tool::EditText) set(0, 0, 0);
            else if (t.tool == Tool::Sign) set(0.05f, 0.1f, 0.45f);
            else if (t.tool == Tool::Note) set(1.0f, 0.85f, 0.2f);
            else if (t.tool == Tool::Redact) set(0, 0, 0);
            else if (t.tool == Tool::Whiteout) set(1, 1, 1);
            else if (on == false && t.tool != Tool::Select && t.tool != Tool::Picture) set(0.9f, 0.1f, 0.1f);
        }
        if (on) ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", t.tip);
    }
    ImGui::SameLine();
    ImGui::ColorEdit4("##pdfcolour", v.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha);
    ImGui::SetItemTooltip("Colour");
    if (v.tool == Tool::Text || v.tool == Tool::EditText) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 5);
        ImGui::SliderFloat("##size", &v.textSize, 6, 48, "%.0f pt");
    } else if (v.tool == Tool::Draw || v.tool == Tool::Box || v.tool == Tool::Ellipse || v.tool == Tool::Line || v.tool == Tool::Arrow) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 5);
        ImGui::SliderFloat("##width", &v.width, 0.5f, 12, "%.1f pt");
    }
    if (v.selected >= 0) {
        ImGui::SameLine();
        if (ImGui::Button("Delete edit")) {
            pdfRun("pdf.mark_delete", {{"mark", std::to_string(v.selected)}}, "Delete edit");
            v.selected = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Recolour")) pdfRun("pdf.mark_edit", {{"mark", static_cast<std::int64_t>(v.selected)}, {"color", hexOf(v.color)}}, "Change edit");
    }
}

void Shell::pdfThumbs(float width) {
    PdfPanel& v = panels_->pdf;
    if (v.thumbsSeen != changes_) {
        for (auto& [p, t] : v.thumbs) t.reset();
        v.thumbs.clear();
        v.thumbsSeen = changes_;
    }
    const float inner = width - ImGui::GetStyle().WindowPadding.x * 2 - ImGui::GetStyle().ScrollbarSize;
    const float rowH = inner * 1.35f + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGuiListClipper clipper;
    clipper.Begin(v.pages, rowH);
    int made = 0;  // a few per frame, so a long document opens at once
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            ImGui::PushID(i);
            Texture& t = v.thumbs[i];
            if (!t.tex && made < 3) {
                ++made;
                Result r = query("pdf.view", {{"doc", v.doc}, {"page", static_cast<std::int64_t>(i + 1)}, {"dpi", std::clamp(72.0 * inner / 600.0 * 1.5, 10.0, 40.0)}});
                if (r.ok) tp::attachmentTexture(renderer_, t, r);
            }
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("thumb", {inner, inner * 1.35f})) v.page = i;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (t.tex && t.srcWidth > 0) {
                const float s = std::min(inner / static_cast<float>(t.srcWidth), inner * 1.35f / static_cast<float>(t.srcHeight));
                const float w = static_cast<float>(t.srcWidth) * s, h = static_cast<float>(t.srcHeight) * s;
                const ImVec2 p0{at.x + (inner - w) / 2, at.y + (inner * 1.35f - h) / 2};
                dl->AddImage(t.id(), p0, {p0.x + w, p0.y + h});
                dl->AddRect(p0, {p0.x + w, p0.y + h}, i == v.page ? ImGui::ColorConvertFloat4ToU32(accent_) : ImGui::GetColorU32(ImGuiCol_Border), 0, 0,
                            i == v.page ? 3.0f : 1.0f);
            } else {
                dl->AddRect(at, {at.x + inner, at.y + inner * 1.35f}, ImGui::GetColorU32(ImGuiCol_Border));
            }
            const std::string n = std::to_string(i + 1);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - ImGui::CalcTextSize(n.c_str()).x) / 2);
            ImGui::TextUnformatted(n.c_str());
            ImGui::PopID();
        }
    }
    if (made > 0) animating_ = true;  // keep drawing until every visible thumbnail is in
}

void Shell::pdfPage() {
    PdfPanel& v = panels_->pdf;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(avail.x, 32.0f), std::max(avail.y, 32.0f)};
    if (v.zoom <= 0 && v.pageW > 0) {
        v.zoom = std::clamp(static_cast<float>(size.x * 0.94f / v.pageW), 0.1f, 8.0f);
        v.pan = {(size.x - static_cast<float>(v.pageW) * v.zoom) / 2, size.x * 0.02f};
    } else if (v.zoom <= 0) {
        v.zoom = dpi_;  // the first picture tells the page size; fit after it
        pdfLoad(true);
        v.zoom = 0;
        if (v.pageW > 0) {
            v.zoom = std::clamp(static_cast<float>(size.x * 0.94f / v.pageW), 0.1f, 8.0f);
            v.pan = {(size.x - static_cast<float>(v.pageW) * v.zoom) / 2, size.x * 0.02f};
        }
    }
    pdfLoad(false);
    ImGui::InvisibleButton("pdfcanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const ImVec2 v0 = ImGui::GetItemRectMin(), v1{v0.x + size.x, v0.y + size.y};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(v0, v1, true);
    dl->AddRectFilled(v0, v1, darkLook() ? IM_COL32(40, 42, 46, 255) : IM_COL32(200, 202, 206, 255));
    if (!v.tex.tex || v.pageW <= 0) {
        dl->PopClipRect();
        return;
    }
    const float s = v.zoom;
    const float pw = static_cast<float>(v.pageW) * s, ph = static_cast<float>(v.pageH) * s;

    // wheel: scroll (past the end turns the page), Ctrl+wheel zooms at the mouse
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            const float px = (io.MousePos.x - v0.x - v.pan.x) / s, py = (io.MousePos.y - v0.y - v.pan.y) / s;
            v.zoom = std::clamp(v.zoom * std::pow(1.15f, io.MouseWheel), 0.1f, 8.0f);
            v.pan = {io.MousePos.x - v0.x - px * v.zoom, io.MousePos.y - v0.y - py * v.zoom};
            v.lastZoom = tp::seconds();
        } else {
            const float step = ImGui::GetFontSize() * 4 * io.MouseWheel;
            const float top = size.y * 0.02f, bottom = size.y - ph - size.y * 0.02f;
            if (io.MouseWheel < 0 && v.pan.y <= std::min(top, bottom) + 0.5f && v.page + 1 < v.pages) {
                ++v.page;
                v.pan.y = top;
            } else if (io.MouseWheel > 0 && v.pan.y >= top - 0.5f && v.page > 0) {
                --v.page;
                v.pan.y = std::min(top, bottom);
            } else {
                v.pan.y = std::clamp(v.pan.y + step, std::min(top, bottom), top);
            }
        }
        if (io.MouseWheelH != 0) v.pan.x += ImGui::GetFontSize() * 4 * io.MouseWheelH;
    }
    if (!io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && v.page + 1 < v.pages) ++v.page;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp) && v.page > 0) --v.page;
        if (v.selected >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
            pdfRun("pdf.mark_delete", {{"mark", std::to_string(v.selected)}}, "Delete edit");
            v.selected = -1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) v.selected = -1, v.tool = Tool::Select;
    }
    const bool panning = active && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsKeyDown(ImGuiKey_Space));
    if (panning) v.pan.x += io.MouseDelta.x, v.pan.y += io.MouseDelta.y;

    const ImVec2 p0{v0.x + v.pan.x, v0.y + v.pan.y}, p1{p0.x + pw, p0.y + ph};
    dl->AddRectFilled({p0.x + 3, p0.y + 3}, {p1.x + 3, p1.y + 3}, IM_COL32(0, 0, 0, 60));
    dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255));
    dl->AddImage(v.tex.id(), p0, p1);
    auto toScreen = [&](double x, double y) { return ImVec2{p0.x + static_cast<float>(x) * s, p0.y + static_cast<float>(y) * s}; };
    const double mx = (io.MousePos.x - p0.x) / s, my = (io.MousePos.y - p0.y) / s;
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(accent_);

    // find matches on this page
    for (std::size_t i = 0; i < v.found.size(); ++i) {
        const auto& f = v.found[i];
        if (static_cast<int>(num(f, 0)) - 1 != v.page) continue;
        dl->AddRect(toScreen(num(f, 1) - 1, num(f, 2) - 1), toScreen(num(f, 1) + num(f, 3) + 1, num(f, 2) + num(f, 4) + 1),
                    static_cast<int>(i) == v.foundAt ? accent : IM_COL32(255, 140, 0, 200), 0, 0, 2);
    }

    // hover hints
    if (hovered && !panning && !v.dragging) {
        if (v.tool == Tool::EditText) {
            for (const auto& l : v.lines) {
                if (inside(mx, my, num(l, 1), num(l, 2), num(l, 3), num(l, 4), 1)) {
                    dl->AddRect(toScreen(num(l, 1) - 1, num(l, 2) - 1), toScreen(num(l, 1) + num(l, 3) + 1, num(l, 2) + num(l, 4) + 1), accent, 0, 0, 1.5f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
                    break;
                }
            }
        } else if (v.tool == Tool::Select) {
            bool any = false;
            for (const auto& w : v.widgets) {
                if (static_cast<int>(num(w, 1)) - 1 != v.page || !inside(mx, my, num(w, 2), num(w, 3), num(w, 4), num(w, 5))) continue;
                dl->AddRect(toScreen(num(w, 2), num(w, 3)), toScreen(num(w, 2) + num(w, 4), num(w, 3) + num(w, 5)), accent, 0, 0, 1.5f);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                any = true;
                break;
            }
            for (const auto& m : v.marks) {
                if (any || !inside(mx, my, num(m, 3), num(m, 4), num(m, 5), num(m, 6), 3)) continue;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                break;
            }
        } else if (v.tool != Tool::Draw) {
            ImGui::SetMouseCursor(v.tool == Tool::Text ? ImGuiMouseCursor_TextInput : ImGuiMouseCursor_Arrow);
        }
    }
    // form boxes stand out a little
    for (const auto& w : v.widgets) {
        if (static_cast<int>(num(w, 1)) - 1 != v.page) continue;
        dl->AddRectFilled(toScreen(num(w, 2), num(w, 3)), toScreen(num(w, 2) + num(w, 4), num(w, 3) + num(w, 5)), IM_COL32(60, 120, 255, 28));
    }

    // press
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_Space)) {
        v.dragging = true;
        v.dragFrom = v.dragTo = {static_cast<float>(mx), static_cast<float>(my)};
        v.stroke = {mx, my};
        v.moving = false;
        if (v.tool == Tool::Select) {
            v.selected = -1;
            bool onWidget = false;
            for (const auto& w : v.widgets) {
                if (static_cast<int>(num(w, 1)) - 1 != v.page || !inside(mx, my, num(w, 2), num(w, 3), num(w, 4), num(w, 5))) continue;
                onWidget = true;
                v.dragging = false;
                const std::string name = w[0];
                for (const auto& f : v.fields) {
                    if (f.empty() || f[0] != name) continue;
                    const std::string type = f.size() > 1 ? f[1] : "", value = f.size() > 2 ? f[2] : "";
                    if (type == "checkbox") {
                        const bool on = !value.empty() && value != "Off";
                        pdfRun("pdf.fill", {{"name", name}, {"value", std::string(on ? "No" : "Yes")}}, "Fill form");
                    } else {
                        v.popup = "field";
                        v.field = name;
                        v.fieldType = type;
                        v.input = v.original = value;
                        v.options.clear();
                        if (f.size() > 3 && !f[3].empty()) {
                            std::string o = f[3];
                            for (std::size_t at = 0;;) {
                                const auto bar = o.find('|', at);
                                v.options.push_back(o.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
                                if (bar == std::string::npos) break;
                                at = bar + 1;
                            }
                        }
                        v.openPopup = true;
                    }
                    break;
                }
                break;
            }
            if (!onWidget) {
                // the topmost edit under the mouse (later ones are drawn on top)
                for (auto it = v.marks.rbegin(); it != v.marks.rend(); ++it) {
                    if (inside(mx, my, num(*it, 3), num(*it, 4), num(*it, 5), num(*it, 6), 3)) {
                        v.selected = static_cast<int>(num(*it, 0));
                        v.moving = true;
                        break;
                    }
                }
                if (v.selected >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    for (const auto& m : v.marks) {
                        if (static_cast<int>(num(m, 0)) != v.selected || m.size() < 8 || m[7].empty()) continue;
                        v.popup = "mark";
                        v.input = v.original = m[7];
                        v.openPopup = true;
                        v.moving = false;
                        v.dragging = false;
                    }
                }
                if (v.selected < 0) v.dragging = false;
            }
        }
    }
    if (v.dragging && active) {
        v.dragTo = {static_cast<float>(mx), static_cast<float>(my)};
        if (v.tool == Tool::Draw) {
            const double lx = v.stroke[v.stroke.size() - 2], ly = v.stroke.back();
            if (std::hypot(mx - lx, my - ly) * s >= 2) v.stroke.push_back(mx), v.stroke.push_back(my);
        }
    }
    if (v.dragging && !active) {
        v.dragging = false;
        pdfRelease();
    }

    // what is being dragged
    if (v.dragging) {
        const ImU32 col = ImGui::ColorConvertFloat4ToU32({v.color[0], v.color[1], v.color[2], 0.85f});
        const ImVec2 a = toScreen(v.dragFrom.x, v.dragFrom.y), b = toScreen(v.dragTo.x, v.dragTo.y);
        const ImVec2 lo{std::min(a.x, b.x), std::min(a.y, b.y)}, hi{std::max(a.x, b.x), std::max(a.y, b.y)};
        switch (v.tool) {
            case Tool::Draw: {
                std::vector<ImVec2> pts;
                for (std::size_t i = 0; i + 1 < v.stroke.size(); i += 2) pts.push_back(toScreen(v.stroke[i], v.stroke[i + 1]));
                dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, std::max(1.0f, v.width * s));
                break;
            }
            case Tool::Line:
            case Tool::Arrow: dl->AddLine(a, b, col, std::max(1.0f, v.width * s)); break;
            case Tool::Ellipse:
                dl->AddEllipse({(lo.x + hi.x) / 2, (lo.y + hi.y) / 2}, {(hi.x - lo.x) / 2, (hi.y - lo.y) / 2}, col, 0, 0, std::max(1.0f, v.width * s));
                break;
            case Tool::Highlight:
            case Tool::Whiteout:
            case Tool::Redact: dl->AddRectFilled(lo, hi, ImGui::ColorConvertFloat4ToU32({v.color[0], v.color[1], v.color[2], 0.45f})); break;
            case Tool::Select:
                if (v.moving) {
                    for (const auto& m : v.marks) {
                        if (static_cast<int>(num(m, 0)) != v.selected) continue;
                        const double dx = v.dragTo.x - v.dragFrom.x, dy = v.dragTo.y - v.dragFrom.y;
                        dl->AddRect(toScreen(num(m, 3) + dx, num(m, 4) + dy), toScreen(num(m, 3) + num(m, 5) + dx, num(m, 4) + num(m, 6) + dy), accent, 0, 0, 2);
                    }
                }
                break;
            default: dl->AddRect(lo, hi, col, 0, 0, std::max(1.0f, v.width * s)); break;
        }
    }
    // the picked edit
    if (v.selected >= 0 && !(v.dragging && v.moving)) {
        for (const auto& m : v.marks) {
            if (static_cast<int>(num(m, 0)) != v.selected) continue;
            dl->AddRect(toScreen(num(m, 3) - 2, num(m, 4) - 2), toScreen(num(m, 3) + num(m, 5) + 2, num(m, 4) + num(m, 6) + 2), accent, 0, 0, 2);
        }
    }
    dl->PopClipRect();
}

void Shell::pdfRelease() {
    PdfPanel& v = panels_->pdf;
    const double x0 = std::min(v.dragFrom.x, v.dragTo.x), y0 = std::min(v.dragFrom.y, v.dragTo.y);
    const double w = std::abs(v.dragTo.x - v.dragFrom.x), h = std::abs(v.dragTo.y - v.dragFrom.y);
    const bool click = w * v.zoom < 4 && h * v.zoom < 4;
    const auto page = static_cast<std::int64_t>(v.page + 1);
    const std::string colour = hexOf(v.color);
    auto popupAt = [&](const char* kind) {
        v.popup = kind;
        v.px = v.dragFrom.x;
        v.py = v.dragFrom.y;
        v.pw = w;
        v.ph = h;
        v.input.clear();
        v.original.clear();
        v.openPopup = true;
    };
    switch (v.tool) {
        case Tool::Select:
            if (v.moving && v.selected >= 0 && !click) {
                pdfRun("pdf.mark_move",
                       {{"mark", static_cast<std::int64_t>(v.selected)}, {"dx", static_cast<double>(v.dragTo.x - v.dragFrom.x)}, {"dy", static_cast<double>(v.dragTo.y - v.dragFrom.y)}},
                       "Move edit");
            }
            v.moving = false;
            break;
        case Tool::Text: popupAt("text"); break;
        case Tool::Note: popupAt("note"); break;
        case Tool::Sign: popupAt("sign"); break;
        case Tool::EditText:
            for (const auto& l : v.lines) {
                if (!inside(v.dragFrom.x, v.dragFrom.y, num(l, 1), num(l, 2), num(l, 3), num(l, 4), 1) || l.size() < 6) continue;
                popupAt("edit");
                v.input = v.original = l[5];
                break;
            }
            break;
        case Tool::Picture:
            select("pdf.image");
            if (form_) {
                for (auto& f : form_->fields) {
                    auto put = [&](double n) { f.number = n, f.set = true; };
                    if (f.spec.name == "page") put(static_cast<double>(page));
                    else if (f.spec.name == "x") put(x0);
                    else if (f.spec.name == "y") put(y0);
                    else if (f.spec.name == "w" && !click) put(w);
                    else if (f.spec.name == "h" && !click) put(h);
                }
            }
            break;
        case Tool::Draw:
            if (v.stroke.size() >= 4) {
                pdfRun("pdf.draw", {{"page", page}, {"points", NumberList(v.stroke.begin(), v.stroke.end())}, {"color", colour}, {"width", static_cast<double>(v.width)}},
                       "Draw");
            }
            break;
        default: {
            if (click) break;
            const Params area{{"page", page}, {"x", x0}, {"y", y0}, {"w", w}, {"h", h}};
            Params p = area;
            if (v.tool == Tool::Highlight || v.tool == Tool::Underline || v.tool == Tool::Strikeout) {
                p["kind"] = std::string(v.tool == Tool::Highlight ? "highlight" : v.tool == Tool::Underline ? "underline" : "strikeout");
                p["color"] = colour;
                pdfRun("pdf.highlight", p, "Highlight");
            } else if (v.tool == Tool::Whiteout) {
                p["color"] = colour;
                pdfRun("pdf.whiteout", p, "White-out");
            } else if (v.tool == Tool::Redact) {
                pdfRun("pdf.redact", p, "Redact");
            } else {
                const char* kind = v.tool == Tool::Ellipse ? "ellipse" : v.tool == Tool::Line ? "line" : v.tool == Tool::Arrow ? "arrow" : "rect";
                p["kind"] = std::string(kind);
                p["color"] = colour;
                p["width"] = static_cast<double>(v.width);
                if (v.tool == Tool::Line || v.tool == Tool::Arrow) {
                    p["x"] = static_cast<double>(v.dragFrom.x);
                    p["y"] = static_cast<double>(v.dragFrom.y);
                    p["w"] = static_cast<double>(v.dragTo.x - v.dragFrom.x);
                    p["h"] = static_cast<double>(v.dragTo.y - v.dragFrom.y);
                }
                pdfRun("pdf.shape", p, "Shape");
            }
        }
    }
}

void Shell::pdfPopup() {
    PdfPanel& v = panels_->pdf;
    if (v.openPopup) {
        ImGui::OpenPopup("##pdfinput");
        v.openPopup = false;
    }
    if (v.popup.empty()) return;
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 22, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##pdfinput", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        v.popup.clear();
        return;
    }
    const std::string kind = v.popup;
    const char* head = kind == "text" ? "Add text" : kind == "edit" ? "Change this text" : kind == "note" ? "Note" : kind == "sign" ? "Type your signature"
                     : kind == "mark" ? "Change the text" : "Fill in";
    tp::title(kind == "field" ? v.field : head, 1.1f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool ok = false;
    const float unit = ImGui::GetFontSize();
    if (kind == "field" && !v.options.empty()) {
        for (const auto& o : v.options) {
            if (ImGui::Selectable(o.c_str(), o == v.input)) v.input = o, ok = true;
        }
    } else if (kind == "sign") {
        ok = ImGui::InputText("##in", &v.input, ImGuiInputTextFlags_EnterReturnsTrue);
    } else {
        ImGui::InputTextMultiline("##in", &v.input, {unit * 21, unit * 5});
        tp::muted("Ctrl+Enter to finish");
        ok = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter);
    }
    if (kind == "sign") tp::muted("Or use Draw to sign by hand.");
    ok = accentButton("OK") || ok;
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (ok) {
        const auto page = static_cast<std::int64_t>(v.page + 1);
        const std::string colour = hexOf(v.color);
        if (kind == "text" && !v.input.empty()) {
            pdfRun("pdf.add_text", {{"page", page}, {"x", v.px}, {"y", v.py}, {"text", v.input}, {"size", static_cast<double>(v.textSize)}, {"color", colour}}, "Add text");
        } else if (kind == "edit" && v.input != v.original) {
            pdfRun("pdf.replace_text", {{"page", page}, {"find", v.original}, {"replace", v.input}, {"all", false}, {"match_case", true}}, "Edit text");
        } else if (kind == "note" && !v.input.empty()) {
            pdfRun("pdf.note", {{"page", page}, {"x", v.px}, {"y", v.py}, {"text", v.input}, {"color", colour}}, "Note");
        } else if (kind == "sign" && !v.input.empty()) {
            Params p{{"page", page}, {"x", v.px}, {"y", v.py}, {"name", v.input}, {"color", colour}};
            if (v.pw * v.zoom >= 4 && v.ph * v.zoom >= 4) p["w"] = v.pw, p["h"] = v.ph;
            pdfRun("pdf.sign", p, "Sign");
        } else if (kind == "mark" && v.input != v.original) {
            pdfRun("pdf.mark_edit", {{"mark", static_cast<std::int64_t>(v.selected)}, {"text", v.input}}, "Change edit");
        } else if (kind == "field" && v.input != v.original) {
            pdfRun("pdf.fill", {{"name", v.field}, {"value", v.input}}, "Fill form");
        }
    }
    if (ok || cancel) {
        v.popup.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace wetwaresuite::gui
