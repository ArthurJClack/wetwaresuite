// Slides like PowerPoint: the slides down the left (click one to show it big, buttons to add,
// copy and delete), the slide in the middle. Click a title, the bullets or a text box on the
// slide to type into it; drag a text box, shape or picture to move it. Every change is still an
// office.deck_* command, so it undoes and Claude sees the same thing.
#include <algorithm>
#include <cmath>

#include "shell.hpp"
// after imgui.h
#include "imgui_stdlib.h"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::gui {

namespace {

double numberField(const Result& r, const char* key, double fallback = 0) {
    const Value* v = r.field(key);
    if (auto d = v ? std::get_if<double>(v) : nullptr) return *d;
    if (auto i = v ? std::get_if<std::int64_t>(v) : nullptr) return static_cast<double>(*i);
    return fallback;
}

std::string textField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    auto s = v ? std::get_if<std::string>(v) : nullptr;
    return s ? *s : std::string();
}

const char* partName(const std::string& kind) {
    if (kind == "title") return "Title";
    if (kind == "subtitle") return "Subtitle";
    if (kind == "bullets") return "Bullets";
    if (kind == "right") return "Right column";
    return "Text box";
}

}  // namespace

bool Shell::deckDoc() const {
    auto doc = session_.workspace().get(currentDoc_);
    return doc && doc->kind() == "deck" && find("office.deck_preview") && find("office.deck_hit");
}

std::string Shell::deckPicture() {
    DeckView& d = deck_;
    if (d.doc != currentDoc_) {
        d = DeckView{};
        d.doc = currentDoc_;
    }
    Result r = query("office.deck_preview", {{"doc", currentDoc_}, {"slide", static_cast<std::int64_t>(std::max(1, d.slide))}, {"width", std::int64_t{1280}}});
    if (!r.ok && d.slide > 1) {  // the slide shown was deleted: show the last one
        d.slide = 1;
        r = query("office.deck_preview", {{"doc", currentDoc_}, {"slide", std::int64_t{1}}, {"width", std::int64_t{1280}}});
    }
    if (!r.ok || r.attachments.empty()) return {};
    d.slides = static_cast<int>(numberField(r, "slides", 1));
    d.pointsWide = numberField(r, "points_wide", 960);
    return r.attachments[0].bytes;
}

void Shell::showSlide(int n) {
    n = std::clamp(n, 1, std::max(1, deck_.slides));
    if (n == deck_.slide) return;
    deck_.slide = n;
    deck_.editing = false;
    deck_.hoverKind.clear();
    canvas_.dirty = true;
}

void Shell::deckStrip() {
    DeckView& d = deck_;
    const float unit = ImGui::GetFontSize();
    // the thumbnails, drawn again after each change
    if (d.seen != changes_ || d.thumbsFor != currentDoc_) {
        d.seen = changes_;
        d.thumbsFor = currentDoc_;
        Result r = query("office.deck_preview", {{"doc", currentDoc_}, {"all", true}, {"width", std::int64_t{256}}});
        d.thumbs.resize(r.ok ? r.attachments.size() : 0);
        for (std::size_t i = 0; i < d.thumbs.size(); ++i) {
            try {
                raster::Image pic = raster::decode(r.attachments[i].bytes);
                d.thumbs[i].upload(renderer_, pic.data(), pic.width(), pic.height());
            } catch (const std::exception&) {
                d.thumbs[i].reset();
            }
        }
        if (r.ok) d.slides = static_cast<int>(numberField(r, "slides", static_cast<double>(d.thumbs.size())));
    }
    auto slideCommand = [&](const char* id, Params p, const char* what) {
        p["doc"] = currentDoc_;
        Result res = run(id, p);
        resultFor_ = what;
        res.message = res.ok ? sentence(res.message) : friendlyError(res.message);
        if (res.ok) {
            if (const Value* v = res.field("slide")) {
                if (auto n = std::get_if<std::int64_t>(v)) {
                    d.slides = std::max(d.slides, static_cast<int>(*n));
                    showSlide(static_cast<int>(*n));
                }
            }
            canvas_.dirty = true;
        }
        result_ = std::move(res);
    };
    if (ImGui::Button("+ Slide")) slideCommand("office.deck_add_slide", {{"layout", std::string("content")}, {"at", static_cast<std::int64_t>(d.slide + 1)}}, "New slide");
    ImGui::SetItemTooltip("Add a slide after this one (title and bullets)");
    ImGui::SameLine();
    if (ImGui::Button("Copy") && find("office.deck_duplicate_slide")) slideCommand("office.deck_duplicate_slide", {{"slide", static_cast<std::int64_t>(d.slide)}}, "Duplicate slide");
    ImGui::SetItemTooltip("Duplicate this slide");
    ImGui::SameLine();
    ImGui::BeginDisabled(d.slides <= 1);
    if (ImGui::Button("Delete")) {
        slideCommand("office.deck_delete_slide", {{"slide", static_cast<std::int64_t>(d.slide)}}, "Delete slide");
        d.slides = std::max(1, d.slides - 1);
        showSlide(std::min(d.slide, d.slides));
        canvas_.dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Delete this slide (Ctrl+Z brings it back)");
    ImGui::Separator();

    ImGui::BeginChild("slide_list");
    const float w = ImGui::GetContentRegionAvail().x - unit * 1.6f;
    for (std::size_t i = 0; i < d.thumbs.size(); ++i) {
        const Texture& t = d.thumbs[i];
        const int n = static_cast<int>(i) + 1;
        ImGui::PushID(n);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%d", n);
        ImGui::SameLine(unit * 1.6f);
        const float h = t.width > 0 ? w * static_cast<float>(t.height) / static_cast<float>(t.width) : w * 9 / 16;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("thumb", {w, h})) showSlide(n);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (t.tex) dl->AddImage(t.id(), at, {at.x + w, at.y + h});
        const bool on = n == d.slide;
        dl->AddRect({at.x - 2, at.y - 2}, {at.x + w + 2, at.y + h + 2},
                    on ? ImGui::ColorConvertFloat4ToU32(accent_) : ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_Border), 2,
                    0, on ? 3.0f : 1.0f);
        if (on && d.scrollToSlide) {
            ImGui::SetScrollHereY(0.5f);
            d.scrollToSlide = false;
        }
        ImGui::PopID();
        ImGui::Dummy({1, unit * 0.3f});
    }
    ImGui::EndChild();
}

void Shell::deckCanvas(ImDrawList* dl, ImVec2 p0, bool hovered, bool active) {
    DeckView& d = deck_;
    ImGuiIO& io = ImGui::GetIO();
    const Texture& t = canvas_.texture;
    if (!t.tex || t.srcWidth <= 0) return;
    // slide points <-> screen
    const float perPoint = static_cast<float>(t.srcWidth / d.pointsWide) * canvas_.zoom;
    auto toScreen = [&](double x, double y) { return ImVec2{p0.x + static_cast<float>(x) * perPoint, p0.y + static_cast<float>(y) * perPoint}; };
    auto toPoints = [&](ImVec2 s) { return ImVec2{(s.x - p0.x) / perPoint, (s.y - p0.y) / perPoint}; };
    auto hit = [&](ImVec2 screen) {
        const ImVec2 pt = toPoints(screen);
        return query("office.deck_hit", {{"doc", currentDoc_}, {"slide", static_cast<std::int64_t>(d.slide)}, {"x", static_cast<double>(pt.x)}, {"y", static_cast<double>(pt.y)}});
    };
    const ImVec4 c = accent_;
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(c);

    // Page Up / Page Down step through the slides
    if (hovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
            showSlide(d.slide + 1);
            d.scrollToSlide = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
            showSlide(d.slide - 1);
            d.scrollToSlide = true;
        }
    }

    // what's under the mouse gets a thin frame, so you can see what a click will pick
    if (hovered && !active && !ImGui::IsKeyDown(ImGuiKey_Space) && (io.MousePos.x != d.hoverAt.x || io.MousePos.y != d.hoverAt.y)) {
        d.hoverAt = io.MousePos;
        Result r = hit(io.MousePos);
        d.hoverKind = r.ok ? textField(r, "kind") : std::string();
        d.hoverObject = static_cast<int>(numberField(r, "object"));
        d.hover = {static_cast<float>(numberField(r, "x")), static_cast<float>(numberField(r, "y")), static_cast<float>(numberField(r, "w")),
                   static_cast<float>(numberField(r, "h"))};
        d.hoverEmpty = r.ok && textField(r, "text").empty();
    }
    if (!hovered && !active) d.hoverKind.clear();

    // press on an object: drag it; let go without moving: edit what's there
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_Space)) {
        Result r = hit(io.MousePos);
        d.pressKind = r.ok ? textField(r, "kind") : std::string();
        d.pressObject = static_cast<int>(numberField(r, "object"));
        d.press = {static_cast<float>(numberField(r, "x")), static_cast<float>(numberField(r, "y")), static_cast<float>(numberField(r, "w")),
                   static_cast<float>(numberField(r, "h"))};
        d.pressText = textField(r, "text");
        d.pressAt = io.MousePos;
        d.moving = false;
    }
    if (active && !d.pressKind.empty() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float dx = io.MousePos.x - d.pressAt.x, dy = io.MousePos.y - d.pressAt.y;
        if (d.pressKind == "object" && dx * dx + dy * dy > 16) d.moving = true;
        if (d.moving) {
            const ImVec2 a = toScreen(d.press.x, d.press.y);
            const ImVec2 b{a.x + d.press.z * perPoint, a.y + d.press.w * perPoint};
            dl->AddRect({a.x + dx, a.y + dy}, {b.x + dx, b.y + dy}, accent, 0, 0, 2);
        }
    }
    if (!d.pressKind.empty() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const std::string kind = d.pressKind;
        d.pressKind.clear();
        if (d.moving) {
            d.moving = false;
            const double nx = std::round(d.press.x + (io.MousePos.x - d.pressAt.x) / perPoint);
            const double ny = std::round(d.press.y + (io.MousePos.y - d.pressAt.y) / perPoint);
            Result r = run("office.deck_object_edit", {{"doc", currentDoc_},
                                                       {"slide", static_cast<std::int64_t>(d.slide)},
                                                       {"object", std::to_string(d.pressObject)},
                                                       {"x", nx},
                                                       {"y", ny}});
            resultFor_ = "Move";
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
            canvas_.dirty = true;
            d.hoverAt = {-1, -1};
        } else {
            d.editing = true;
            d.focus = true;
            d.kind = kind;
            d.object = d.pressObject;
            d.box = d.press;
            d.text = d.pressText;
            while (!d.text.empty() && d.text.back() == '\n') d.text.pop_back();
        }
    } else if (!d.moving && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && d.pressKind.empty() && !ImGui::IsKeyDown(ImGuiKey_Space) &&
               io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16) {
        d.editing = false;  // a click on nothing closes the editor
    }

    // frames: the part being edited, else the one under the mouse ("click to add title" when empty)
    if (!d.hoverKind.empty() && !(d.editing && d.hoverKind == d.kind && d.hoverObject == d.object) && !d.moving) {
        const ImVec2 a = toScreen(d.hover.x, d.hover.y), b{a.x + d.hover.z * perPoint, a.y + d.hover.w * perPoint};
        dl->AddRect(a, b, ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.7f}), 0, 0, 1.5f);
        if (d.hoverEmpty) {
            const std::string hint = std::string("Click to add ") + (d.hoverKind == "bullets" || d.hoverKind == "right" ? "bullets" : d.hoverKind == "object" ? "text" : d.hoverKind);
            dl->AddText({a.x + 8, a.y + 6}, IM_COL32(128, 128, 128, 255), hint.c_str());
        }
    }
    if (!d.editing) return;
    const ImVec2 a = toScreen(d.box.x, d.box.y), b{a.x + d.box.z * perPoint, a.y + d.box.w * perPoint};
    dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.08f}));
    dl->AddRect(a, b, accent, 0, 0, 2);

    // the text box floats under the part (above it near the bottom of the view)
    const float unit = ImGui::GetFontSize();
    const float boxW = std::min(unit * 30, canvas_.viewSize.x - unit * 2);
    const bool lines = d.kind == "bullets" || d.kind == "right" || d.kind == "object";
    const float boxH = unit * (lines ? 11 : 7);
    const float x = std::clamp(a.x, canvas_.viewMin.x + 8, canvas_.viewMin.x + canvas_.viewSize.x - boxW - 8);
    float y = b.y + 6;
    if (y + boxH > canvas_.viewMin.y + canvas_.viewSize.y) y = std::max(canvas_.viewMin.y + 8, a.y - boxH - 6);
    ImGui::SetNextWindowPos({x, y}, d.focus ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSize({boxW, 0}, ImGuiCond_Appearing);
    bool open = true;
    const std::string title = std::string(partName(d.kind)) + (d.kind == "object" ? " " + std::to_string(d.object) : "") + " - slide " +
                              std::to_string(d.slide) + "###deckpartedit";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        if (d.focus) {
            ImGui::SetKeyboardFocusHere();
            d.focus = false;
        }
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        bool save = false;
        if (lines) {
            ImGui::InputTextMultiline("##parttext", &d.text, {-1, unit * 6});
            save = focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter);
        } else {
            ImGui::SetNextItemWidth(-1);
            save = ImGui::InputText("##parttext", &d.text, ImGuiInputTextFlags_EnterReturnsTrue);
        }
        save = ImGui::Button("Save") || save;
        ImGui::SetItemTooltip(lines ? "Put the new text in (Ctrl+Enter)" : "Put the new text in (Enter)");
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel") || (focused && ImGui::IsKeyPressed(ImGuiKey_Escape));
        bool remove = false;
        if (d.kind == "object") {
            ImGui::SameLine();
            remove = ImGui::Button("Delete");
        }
        ImGui::SameLine();
        ImGui::TextDisabled(d.kind == "bullets" || d.kind == "right" ? "one bullet per line, 2 spaces in for a sub-bullet" : "**bold**, *italic*");
        auto done = [&](Result r, const char* what) {
            resultFor_ = what;
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            if (r.ok) d.editing = false;
            result_ = std::move(r);
            canvas_.dirty = true;
            d.hoverAt = {-1, -1};
        };
        if (save && d.kind == "object") {
            done(run("office.deck_object_edit", {{"doc", currentDoc_}, {"slide", static_cast<std::int64_t>(d.slide)}, {"object", std::to_string(d.object)}, {"text", d.text}}),
                 "Edit");
        } else if (save) {
            done(run("office.deck_edit_slide", {{"doc", currentDoc_}, {"slide", static_cast<std::int64_t>(d.slide)}, {d.kind, d.text}}), "Edit");
        } else if (remove) {
            done(run("office.deck_object_delete", {{"doc", currentDoc_}, {"slide", static_cast<std::int64_t>(d.slide)}, {"object", std::to_string(d.object)}}), "Delete");
        } else if (cancel) {
            d.editing = false;
        }
    }
    ImGui::End();
    if (!open) d.editing = false;
}

}  // namespace wetwaresuite::gui
