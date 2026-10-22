// The canvas: shows the open document's picture with zoom and pan, and turns
// mouse and pen input into paint.stroke / paint.erase / paint.fill commands
// (Heal and Remove strokes go to retouch.cpp).
// Images are drawn from a mipmap pyramid: zoomed out, the texture holds a half,
// quarter, ... size copy instead of the full picture, and after an edit only the
// changed area is re-flattened and re-uploaded.
#include <algorithm>
#include <cmath>
#include <map>

#include "shell.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/pyramid.hpp"

namespace wetwaresuite::gui {

namespace {

bool hasParam(const CommandInfo* info, const std::string& name) {
    if (!info) return false;
    for (const auto& p : info->params) {
        if (p.name == name) return true;
    }
    return false;
}

// The pyramid of the image on the canvas, and which of its levels the texture holds.
struct MipCache {
    std::weak_ptr<raster::ImageDoc> doc;
    raster::Pyramid pyramid;
    int level = -1;
};

std::map<const void*, MipCache>& mipCaches() {
    static std::map<const void*, MipCache> caches;  // one per window
    return caches;
}

int maxTextureSide(SDL_Renderer* renderer) {
    const int side = static_cast<int>(
        SDL_GetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096));
    return side > 0 ? side : 4096;
}

// Puts the right pyramid level for `zoom` on the texture: a full upload when the level changes,
// else (when `changes` is set) just the area the last update touched.
bool showLevel(SDL_Renderer* renderer, Texture& tex, MipCache& mc, float zoom, bool changes) {
    if (mc.pyramid.levels() == 0) return false;
    const int level = mc.pyramid.levelFor(zoom, maxTextureSide(renderer));
    const raster::Image& img = mc.pyramid.level(level);
    if (level != mc.level || !tex.tex || tex.width != img.width() || tex.height != img.height()) {
        if (!tex.upload(renderer, img.data(), img.width(), img.height())) return false;
        mc.level = level;
    } else if (changes) {
        const raster::PixelRect r = mc.pyramid.changed(level);
        if (!r.empty()) {
            const SDL_Rect rect{r.x0, r.y0, r.width(), r.height()};
            SDL_UpdateTexture(tex.tex, &rect, img.pixel(r.x0, r.y0), img.width() * 4);
        }
    }
    // the zoom maths works in full-size picture px whatever level is shown
    tex.srcWidth = mc.pyramid.level(0).width();
    tex.srcHeight = mc.pyramid.level(0).height();
    return true;
}

}  // namespace

bool Shell::toolAvailable(CanvasTool tool) const {
    if (tool == CanvasTool::Move) return true;
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || doc->kind() != "image") return false;
    switch (tool) {
        case CanvasTool::Brush: return find("paint.stroke") != nullptr;
        case CanvasTool::Eraser: return find("paint.erase") != nullptr;
        case CanvasTool::Fill: return find("paint.fill") != nullptr;
        case CanvasTool::Heal: return find("photo.heal") != nullptr;
        case CanvasTool::Remove: return find("photo.remove_object") != nullptr;
        default: return false;
    }
}

void Shell::refreshCanvas() {
    if (!canvas_.dirty && canvas_.shownDoc == currentDoc_) return;
    canvas_.dirty = false;
    canvas_.shownDoc = currentDoc_;
    canvas_.summary.clear();
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) {
        mipCaches().erase(this);
        canvas_.texture.reset();
        return;
    }
    canvas_.summary = doc->summary();
    if (auto img = std::dynamic_pointer_cast<raster::ImageDoc>(doc); img && img->image.width > 0 && img->image.height > 0) {
        MipCache& mc = mipCaches()[this];
        const raster::ImageDoc::Changes changes = img->takeChanges();
        if (mc.doc.lock() != img || changes.all || !mc.pyramid.update(img->image, changes.rect)) {
            mc.pyramid.build(img->image);
            mc.level = -1;
        }
        mc.doc = img;
        // a pending "fit" would zoom out right away, so start on the level it will need
        float zoom = canvas_.zoom;
        if (canvas_.fit && canvas_.viewSize.x > 0 && canvas_.viewSize.y > 0) {
            zoom = std::min({canvas_.viewSize.x * 0.95f / static_cast<float>(img->image.width),
                             canvas_.viewSize.y * 0.95f / static_cast<float>(img->image.height), 8.0f});
        }
        if (showLevel(renderer_, canvas_.texture, mc, zoom, true)) return;
    }
    mipCaches().erase(this);
    // anything else: the document's own preview, else the tool's preview command
    std::string png;
    if (auto att = doc->preview()) png = att->bytes;
    if (png.empty()) {
        std::vector<std::string> owners;
        if (auto it = docTool_.find(currentDoc_); it != docTool_.end()) owners.push_back(it->second);
        for (const auto& t : tools_) owners.push_back(t);
        for (const auto& tool : owners) {
            const CommandInfo* preview = find(tool + ".preview");
            if (!hasParam(preview, "doc")) continue;
            // read-only, so it stays out of the history
            Result r = session_.run(preview->id, {{"doc", currentDoc_}});
            if (!r.ok) continue;
            for (const auto& a : r.attachments) {
                if (a.mime.rfind("image/", 0) == 0) {
                    png = a.bytes;
                    break;
                }
            }
            if (!png.empty()) break;
        }
    }
    if (png.empty()) {
        canvas_.texture.reset();
        return;
    }
    try {
        raster::Image pic = raster::decode(png);
        if (!canvas_.texture.upload(renderer_, pic.data(), pic.width(), pic.height())) canvas_.texture.reset();
    } catch (const std::exception&) {
        canvas_.texture.reset();
    }
}

void Shell::canvasToolbar() {
    if (ImGui::Button("Fit")) canvas_.fit = true;
    ImGui::SetItemTooltip("Show the whole picture");
    ImGui::SameLine();
    if (ImGui::Button("100%")) {
        const ImVec2 centre{canvas_.viewSize.x / 2, canvas_.viewSize.y / 2};
        const ImVec2 img{(centre.x - canvas_.pan.x) / canvas_.zoom, (centre.y - canvas_.pan.y) / canvas_.zoom};
        canvas_.zoom = 1;
        canvas_.pan = {centre.x - img.x, centre.y - img.y};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d%%", static_cast<int>(std::lround(canvas_.zoom * 100)));
    if (circuitDoc()) {
        circuitToolbar();
        return;
    }
    const CanvasTool editing[] = {CanvasTool::Brush, CanvasTool::Eraser, CanvasTool::Fill, CanvasTool::Heal, CanvasTool::Remove};
    if (std::none_of(std::begin(editing), std::end(editing), [this](CanvasTool t) { return toolAvailable(t); })) return;
    ImGui::SameLine();
    ImGui::TextUnformatted("  Tool:");
    const struct {
        const char* name;
        CanvasTool tool;
        const char* tip;
    } tools[] = {{"Move", CanvasTool::Move, "Drag to move around, scroll to zoom"},
                 {"Brush", CanvasTool::Brush, "Paint with the mouse or a pen (pressure works)"},
                 {"Eraser", CanvasTool::Eraser, "Rub out paint on the current layer"},
                 {"Fill", CanvasTool::Fill, "Click to fill an area with the colour, like a paint bucket"},
                 {"Heal", CanvasTool::Heal,
                  "Paint over a spot, pimple or scratch to make it disappear. To copy texture from a clean spot, "
                  "Alt+click it first (Option+click on a Mac)"},
                 {"Remove", CanvasTool::Remove,
                  "Paint over something you don't want (a person, a sign, a wire), then press Remove: the gap is filled "
                  "from the picture around it"}};
    for (const auto& t : tools) {
        if (!toolAvailable(t.tool)) continue;
        ImGui::SameLine();
        // wrap onto the next line instead of running off a narrow window
        const ImGuiStyle& style = ImGui::GetStyle();
        const float need = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(t.name).x;
        if (ImGui::GetContentRegionAvail().x < need) ImGui::NewLine();
        if (ImGui::RadioButton(t.name, canvas_.tool == t.tool)) canvas_.tool = t.tool;
        ImGui::SetItemTooltip("%s", t.tip);
    }
    if (canvas_.tool == CanvasTool::Move || !toolAvailable(canvas_.tool)) return;
    if (canvas_.tool == CanvasTool::Heal || canvas_.tool == CanvasTool::Remove) {
        retouchSettings();
        return;
    }
    // the tool's settings go on their own line so narrow windows still show them
    const float unit = ImGui::GetFontSize();
    bool first = true;
    auto next = [&first] {
        if (!first) ImGui::SameLine();
        first = false;
    };
    if (canvas_.tool == CanvasTool::Brush || canvas_.tool == CanvasTool::Fill) {
        next();
        ImGui::ColorEdit4("Colour", canvas_.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    }
    if (canvas_.tool == CanvasTool::Fill) return;
    if (const CommandInfo* stroke = find("paint.stroke")) {
        for (const auto& p : stroke->params) {
            if (p.name != "brush" || p.choices.empty()) continue;
            next();
            ImGui::SetNextItemWidth(unit * 6);
            if (ImGui::BeginCombo("Brush", canvas_.brush.c_str())) {
                for (const auto& c : p.choices) {
                    if (ImGui::Selectable(c.c_str(), c == canvas_.brush)) canvas_.brush = c;
                }
                ImGui::EndCombo();
            }
        }
    }
    next();
    ImGui::SetNextItemWidth(unit * 7);
    ImGui::SliderFloat("Size", &canvas_.size, 1, 300, "%.0f px", ImGuiSliderFlags_Logarithmic);
    next();
    ImGui::SetNextItemWidth(unit * 6);
    ImGui::SliderFloat("Opacity", &canvas_.opacity, 1, 100, "%.0f%%");
}

ImVec2 Shell::toImage(ImVec2 screen) const {
    // the texture may be smaller than the picture; the zoom is in picture px
    return {(screen.x - canvas_.viewMin.x - canvas_.pan.x) / canvas_.zoom,
            (screen.y - canvas_.viewMin.y - canvas_.pan.y) / canvas_.zoom};
}

void Shell::beginStroke(ImVec2 screen, float pressure, bool pen, bool eraser) {
    canvas_.stroking = true;
    canvas_.usedPen = pen;
    canvas_.penErasing = eraser;
    canvas_.points.clear();
    canvas_.pressures.clear();
    addStrokePoint(screen, pressure);
}

void Shell::addStrokePoint(ImVec2 screen, float pressure) {
    if (!canvas_.stroking) return;
    const ImVec2 p = toImage(screen);
    auto& pts = canvas_.points;
    if (pts.size() >= 2) {
        const double dx = p.x - pts[pts.size() - 2], dy = p.y - pts.back();
        if (dx * dx + dy * dy < 0.5) return;
    }
    pts.push_back(p.x);
    pts.push_back(p.y);
    canvas_.pressures.push_back(std::clamp(pressure, 0.0f, 1.0f));
}

void Shell::endStroke() {
    if (!canvas_.stroking) return;
    canvas_.stroking = false;
    if (canvas_.points.size() < 2) return;
    if (canvas_.tool == CanvasTool::Heal || canvas_.tool == CanvasTool::Remove) {
        retouchRelease();
        return;
    }
    const bool erase = canvas_.tool == CanvasTool::Eraser || canvas_.penErasing;
    const std::string id = erase ? "paint.erase" : "paint.stroke";
    const CommandInfo* info = find(id);
    if (!info) return;
    Params p{{"doc", currentDoc_}, {"points", canvas_.points}};
    if (hasParam(info, "brush")) p["brush"] = canvas_.brush;
    if (hasParam(info, "size")) p["size"] = static_cast<double>(canvas_.size);
    if (hasParam(info, "opacity")) p["opacity"] = static_cast<double>(canvas_.opacity);
    if (!erase && hasParam(info, "color")) p["color"] = toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]});
    if (canvas_.usedPen && hasParam(info, "pressures")) {
        p["pressures"] = NumberList(canvas_.pressures.begin(), canvas_.pressures.end());
    }
    Result r = run(id, p);
    resultFor_ = info->title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::onEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_PEN_PROXIMITY_IN: canvas_.usedPen = canvas_.usedPen || canvas_.stroking; break;
        case SDL_EVENT_PEN_DOWN:
            penDown_ = true;
            penEraser_ = e.ptouch.eraser;
            break;
        case SDL_EVENT_PEN_UP: penDown_ = false; break;
        case SDL_EVENT_PEN_AXIS:
            if (e.paxis.axis == SDL_PEN_AXIS_PRESSURE) penPressure_ = e.paxis.value;
            break;
        case SDL_EVENT_PEN_MOTION:
            if (canvas_.stroking && canvas_.usedPen) addStrokePoint({e.pmotion.x, e.pmotion.y}, penPressure_);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (canvas_.stroking && !canvas_.usedPen && e.motion.which != SDL_PEN_MOUSEID) addStrokePoint({e.motion.x, e.motion.y}, 1.0f);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:  // another app may have sent something or changed the colours
            refreshInbox();
            paletteStale_ = true;
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data) openFile(e.drop.data);
            break;
        default: break;
    }
}

void Shell::canvasView() {
    if (canvas_.pending && canvas_.pending->shown) runPendingRetouch();  // the busy sign is on screen: run it now
    refreshCanvas();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(avail.x, 32.0f), std::max(avail.y, 32.0f)};
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    canvas_.viewMin = ImGui::GetItemRectMin();
    canvas_.viewSize = size;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 v0 = canvas_.viewMin, v1{v0.x + size.x, v0.y + size.y};
    dl->PushClipRect(v0, v1, true);
    dl->AddRectFilled(v0, v1, darkLook() ? IM_COL32(40, 42, 46, 255) : IM_COL32(200, 202, 206, 255));

    const Texture& t = canvas_.texture;
    if (!t.tex) {
        const std::string text = canvas_.summary.empty() ? "No picture for this file." : canvas_.summary;
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, size.x * 0.8f);
        dl->AddText(nullptr, 0, {v0.x + (size.x - ts.x) / 2, v0.y + (size.y - ts.y) / 2}, ImGui::GetColorU32(ImGuiCol_Text),
                    text.c_str(), nullptr, size.x * 0.8f);
        dl->PopClipRect();
        return;
    }
    const float w = static_cast<float>(t.srcWidth), h = static_cast<float>(t.srcHeight);
    if (canvas_.fit) {
        canvas_.zoom = std::min({size.x * 0.95f / w, size.y * 0.95f / h, 8.0f});
        canvas_.pan = {(size.x - w * canvas_.zoom) / 2, (size.y - h * canvas_.zoom) / 2};
        canvas_.fit = false;
    }
    if (hovered && io.MouseWheel != 0 && !canvas_.stroking) {
        const ImVec2 img = toImage(io.MousePos);
        canvas_.zoom = std::clamp(canvas_.zoom * std::pow(1.15f, io.MouseWheel), 0.02f, 64.0f);
        canvas_.pan = {io.MousePos.x - v0.x - img.x * canvas_.zoom, io.MousePos.y - v0.y - img.y * canvas_.zoom};
    }
    const bool painting = toolAvailable(canvas_.tool) && canvas_.tool != CanvasTool::Move && !ImGui::IsKeyDown(ImGuiKey_Space) &&
                          !canvas_.pending;
    const bool dragPan = active && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                                    (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !painting && !canvas_.stroking && !circuitEditing()));
    if (dragPan) {
        canvas_.pan.x += io.MouseDelta.x;
        canvas_.pan.y += io.MouseDelta.y;
    }
    if (auto it = mipCaches().find(this); it != mipCaches().end() && !it->second.doc.expired()) {
        showLevel(renderer_, canvas_.texture, it->second, canvas_.zoom, false);  // zoom changed: maybe another level
    }
    const ImVec2 p0{v0.x + canvas_.pan.x, v0.y + canvas_.pan.y}, p1{p0.x + w * canvas_.zoom, p0.y + h * canvas_.zoom};
    dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255));
    dl->AddImage(t.id(), p0, p1);
    dl->AddRect({p0.x - 1, p0.y - 1}, {p1.x + 1, p1.y + 1}, ImGui::GetColorU32(ImGuiCol_Border));

    // painting
    const bool retouch = canvas_.tool == CanvasTool::Heal || canvas_.tool == CanvasTool::Remove;
    const bool stroke = canvas_.tool == CanvasTool::Brush || canvas_.tool == CanvasTool::Eraser || retouch;
    if (painting && stroke && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !(retouch && retouchPress(io.MousePos))) {
        const bool pen = io.MouseSource == ImGuiMouseSource_Pen || penDown_;
        beginStroke(io.MousePos, pen ? penPressure_ : 1.0f, pen, pen && penEraser_);
    }
    if (canvas_.stroking && active) addStrokePoint(io.MousePos, canvas_.usedPen ? penPressure_ : 1.0f);
    if (canvas_.stroking && !active) endStroke();
    if (painting && canvas_.tool == CanvasTool::Fill && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const ImVec2 img = toImage(io.MousePos);
        if (img.x >= 0 && img.y >= 0 && img.x < w && img.y < h && find("paint.fill")) {
            Result r = run("paint.fill", {{"doc", currentDoc_},
                                          {"x", static_cast<std::int64_t>(img.x)},
                                          {"y", static_cast<std::int64_t>(img.y)},
                                          {"color", toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]})}});
            resultFor_ = "Bucket fill";
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
        }
    }

    // the stroke being drawn, before the command runs
    if (canvas_.stroking && canvas_.points.size() >= 2 && !retouch) {
        const bool erasing = canvas_.tool == CanvasTool::Eraser || canvas_.penErasing;
        const ImU32 col = erasing ? IM_COL32(255, 255, 255, 160)
                                  : ImGui::ColorConvertFloat4ToU32({canvas_.color[0], canvas_.color[1], canvas_.color[2],
                                                                    canvas_.color[3] * canvas_.opacity / 100.0f});
        std::vector<ImVec2> pts;
        for (std::size_t i = 0; i + 1 < canvas_.points.size(); i += 2) {
            pts.push_back({p0.x + static_cast<float>(canvas_.points[i]) * canvas_.zoom,
                           p0.y + static_cast<float>(canvas_.points[i + 1]) * canvas_.zoom});
        }
        const float thick = std::max(1.0f, canvas_.size * canvas_.zoom);
        if (pts.size() == 1) dl->AddCircleFilled(pts[0], thick / 2, col);
        else dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, thick);
    }
    if (hovered && painting && stroke) {
        dl->AddCircle(io.MousePos, std::max(2.0f, canvas_.size * canvas_.zoom / 2), IM_COL32(128, 128, 128, 255), 0, 1.5f);
    }
    drawRetouch(dl, p0, hovered);
    if (circuitDoc()) circuitCanvas(dl, p0, hovered, ImGui::IsItemClicked(ImGuiMouseButton_Left));
    dl->PopClipRect();
}

}  // namespace wetwaresuite::gui
