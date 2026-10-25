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
// after imgui.h
#include "imgui_internal.h"
#include "imgui_stdlib.h"
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
    if (tool == CanvasTool::Pick || tool == CanvasTool::Anchor) return vectorDoc();
    if (tool == CanvasTool::VecRect) return vectorDoc() && find("vector.add_rect");
    if (tool == CanvasTool::VecEllipse) return vectorDoc() && find("vector.add_ellipse");
    if (tool == CanvasTool::VecLine) return vectorDoc() && find("vector.add_line");
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || doc->kind() != "image") return false;
    switch (tool) {
        case CanvasTool::Brush: return find("paint.stroke") != nullptr;
        case CanvasTool::Eraser: return find("paint.erase") != nullptr;
        case CanvasTool::Fill: return find("paint.fill") != nullptr;
        case CanvasTool::Heal: return find("photo.heal") != nullptr;
        case CanvasTool::Remove: return find("photo.remove_object") != nullptr;
        case CanvasTool::Select: return find("photo.select") != nullptr;
        case CanvasTool::Clone: return find("paint.clone") != nullptr;
        case CanvasTool::Dodge:
        case CanvasTool::Burn:
        case CanvasTool::Sponge: return find("paint.dodge") != nullptr;
        case CanvasTool::Smudge: return find("paint.smudge") != nullptr;
        case CanvasTool::Blur: return find("paint.blur_brush") != nullptr;
        default: return false;
    }
}

void Shell::refreshCanvas() {
    if (!canvas_.dirty && canvas_.shownDoc == currentDoc_) return;
    canvas_.dirty = false;
    canvas_.shownDoc = currentDoc_;
    ++canvas_.version;
    canvas_.summary.clear();
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) {
        mipCaches().erase(this);
        canvas_.texture.reset();
        return;
    }
    canvas_.summary = doc->summary();
    const bool newDoc = canvas_.shown3dDoc != currentDoc_;
    canvas_.shown3dDoc = currentDoc_;
    canvas_.is3d = probe3d(*doc);
    if (canvas_.is3d) {
        mipCaches().erase(this);
        if (newDoc) canvas_.orbit = OrbitView{};  // a different model starts from iso
        canvas_.orbitDirty = true;               // drawn by orbitCanvas at the view's size
        return;
    }
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
    // a level or sprite: the picture its click tools work on (game_view.cpp)
    std::string png = gamePicture();
    const bool game = !png.empty();
    // anything else: the document's own preview, else the tool's preview command
    if (png.empty() && doc->kind() == "vector" && find("vector.preview")) {  // sharper than the small built-in preview, for zooming in
        Result r = session_.run("vector.preview", {{"doc", currentDoc_}, {"size", std::int64_t{1400}}});
        if (r.ok && !r.attachments.empty()) png = r.attachments[0].bytes;
    }
    if (png.empty()) {
        if (auto att = doc->preview()) png = att->bytes;
    }
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
        if (game) {  // crisp pixels when zoomed in, and the colours for the picker
            canvas_.game.picture.assign(pic.data(), pic.data() + static_cast<std::size_t>(pic.width()) * pic.height() * 4);
            canvas_.game.pictureW = pic.width();
            canvas_.game.pictureH = pic.height();
        }
        if (!canvas_.texture.upload(renderer_, pic.data(), pic.width(), pic.height())) canvas_.texture.reset();
        else if (game) SDL_SetTextureScaleMode(canvas_.texture.tex, SDL_SCALEMODE_NEAREST);
    } catch (const std::exception&) {
        canvas_.texture.reset();
    }
}

namespace {

// The picture and drawing tools in the rail, with their Photoshop letters.
struct CanvasToolDef {
    const char* name;
    CanvasTool tool;
    const char* key;
    const char* tip;
};
const CanvasToolDef kCanvasTools[] = {
    {"Move", CanvasTool::Move, "V", "Drag to move around, scroll to zoom"},
    {"Pick", CanvasTool::Pick, "Q",
     "Click a shape to pick it, drag to move it, drag a corner or edge handle to resize (Shift keeps the "
     "proportions). Arrow keys nudge, Delete removes"},
    {"Anchors", CanvasTool::Anchor, "A",
     "Click a path to show its anchors and handles, drag them to reshape it (Alt breaks a smooth pair). "
     "Alt+click on the path adds an anchor"},
    {"Rectangle", CanvasTool::VecRect, "M", "Drag to draw a rectangle (Shift makes a square)"},
    {"Ellipse", CanvasTool::VecEllipse, "L", "Drag to draw an ellipse (Shift makes a circle)"},
    {"Line", CanvasTool::VecLine, "", "Drag to draw a straight line"},
    {"Select", CanvasTool::Select, "M",
     "Choose the part of the picture to change: drag a box, oval or lasso, or click with the wand. Shift adds, "
     "Alt takes away. Edits then only change inside it"},
    {"Brush", CanvasTool::Brush, "B", "Paint with the mouse or a pen (pressure works)"},
    {"Eraser", CanvasTool::Eraser, "E", "Rub out paint on the current layer"},
    {"Fill", CanvasTool::Fill, "G", "Click to fill an area with the colour, like a paint bucket"},
    {"Heal", CanvasTool::Heal, "J",
     "Paint over a spot, pimple or scratch to make it disappear. To copy texture from a clean spot, "
     "Alt+click it first (Option+click on a Mac)"},
    {"Remove", CanvasTool::Remove, "R",
     "Paint over something you don't want (a person, a sign, a wire), then press Remove: the gap is filled "
     "from the picture around it"},
    {"Clone", CanvasTool::Clone, "S",
     "Paint with a copy of another part of the picture: Alt+click (Option+click on a Mac) the spot to copy "
     "from, then paint"},
    {"Dodge", CanvasTool::Dodge, "O", "Paint to lighten (like dodging a print)"},
    {"Burn", CanvasTool::Burn, "", "Paint to darken"},
    {"Sponge", CanvasTool::Sponge, "", "Paint to wash the colour out"},
    {"Smudge", CanvasTool::Smudge, "W", "Drag colours along like a finger in wet paint"},
    {"Blur", CanvasTool::Blur, "U", "Paint to soften (or sharpen) just there"}};

}  // namespace

void Shell::canvasTools() {
    const CanvasTool editing[] = {CanvasTool::Pick, CanvasTool::Anchor, CanvasTool::Select, CanvasTool::Brush, CanvasTool::Eraser, CanvasTool::Fill, CanvasTool::Heal,
                                  CanvasTool::Remove, CanvasTool::Clone, CanvasTool::Dodge, CanvasTool::Smudge, CanvasTool::Blur};
    if (std::none_of(std::begin(editing), std::end(editing), [this](CanvasTool t) { return toolAvailable(t); })) return;
    for (const auto& t : kCanvasTools) {
        if (!toolAvailable(t.tool)) continue;
        if (railItem(t.name, t.key, t.tip, canvas_.tool == t.tool)) canvas_.tool = t.tool;
    }
}

void Shell::railTools() {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || canvas_.is3d) return;
    if (doc->kind() == "pdf" && find("pdf.view")) return;  // the PDF editor has its own tools
    if (circuitDoc()) return circuitTools();
    if (mapsDoc()) return mapsTools();
    if (gameActive()) return gameTools();
    canvasTools();
}

void Shell::canvasToolbar() {
    if (canvas_.is3d) {
        orbitToolbar();
        return;
    }
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
    if (const std::string opener = docOpener(currentDoc_); opener == "office.doc_new" || opener == "office.doc_open") {
        writeBar();
        return;
    }
    if (mapsDoc()) {
        mapsToolbar();
        return;
    }
    if (gameActive()) {
        gameToolbar();
        return;
    }
    if (canvas_.tool == CanvasTool::Move || !toolAvailable(canvas_.tool)) return;
    for (const auto& t : kCanvasTools) {
        if (t.tool != canvas_.tool) continue;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted((std::string("   ") + t.name + ":").c_str());  // the options bar names its tool, like Photoshop's
        ImGui::SameLine();
    }
    if (canvas_.tool == CanvasTool::Pick || canvas_.tool == CanvasTool::Anchor) {
        vectorSettings();
        return;
    }
    if (vectorDrawTool(canvas_.tool)) {
        barLabel(canvas_.tool == CanvasTool::VecLine ? "Colour" : "Fill");
        ImGui::ColorEdit4("##shapecolour", canvas_.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        return;
    }
    if (canvas_.tool == CanvasTool::Heal || canvas_.tool == CanvasTool::Remove) {
        retouchSettings();
        return;
    }
    if (canvas_.tool == CanvasTool::Select) {
        selectSettings();
        return;
    }
    if (brushTool(canvas_.tool)) {
        brushToolSettings();
        return;
    }
    // the tool's settings follow its name on the options bar
    const float unit = ImGui::GetFontSize();
    bool first = false;
    auto next = [&first] {
        if (!first) ImGui::SameLine();
        first = false;
    };
    if (canvas_.tool == CanvasTool::Brush || canvas_.tool == CanvasTool::Fill) {
        next();
        barLabel("Colour");
        ImGui::ColorEdit4("##colour", canvas_.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    }
    if (canvas_.tool == CanvasTool::Fill) return;
    if (const CommandInfo* stroke = find("paint.stroke")) {
        for (const auto& p : stroke->params) {
            if (p.name != "brush" || p.choices.empty()) continue;
            next();
            barLabel("Tip");
            ImGui::SetNextItemWidth(unit * 6);
            if (ImGui::BeginCombo("##tip", canvas_.brush.c_str())) {
                for (const auto& c : p.choices) {
                    if (ImGui::Selectable(c.c_str(), c == canvas_.brush)) canvas_.brush = c;
                }
                ImGui::EndCombo();
            }
        }
    }
    next();
    barLabel("Size");
    ImGui::SetNextItemWidth(unit * 5.5f);
    ImGui::SliderFloat("##size", &canvas_.size, 1, 300, "%.0f px", ImGuiSliderFlags_Logarithmic);
    next();
    barLabel("Opacity");
    ImGui::SetNextItemWidth(unit * 4.5f);
    ImGui::SliderFloat("##opacity", &canvas_.opacity, 1, 100, "%.0f%%");
}

// A text document: type a paragraph under the toolbar and Enter adds it to the end, so writing
// doesn't need the Add form every time.
void Shell::writeBar() {
    const float unit = ImGui::GetFontSize();
    static const char* kinds[] = {"paragraph", "heading", "list", "numbered", "quote"};
    ImGui::SameLine(0, unit);
    ImGui::SetNextItemWidth(unit * 6.5f);
    if (ImGui::BeginCombo("##kind", writeKind_.c_str())) {
        for (const char* k : kinds) {
            if (ImGui::Selectable(k, writeKind_ == k)) writeKind_ = k;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("What the next line adds");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    // the box stays active after Enter, so the next line can be typed straight away
    ImGuiIO& io = ImGui::GetIO();
    const bool keep = io.ConfigInputTextEnterKeepActive;
    io.ConfigInputTextEnterKeepActive = true;
    const bool entered = ImGui::InputTextWithHint("##write", "Type here, Enter adds it to the end (**bold**, *italic*)", &writeText_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    io.ConfigInputTextEnterKeepActive = keep;
    if (entered && !writeText_.empty()) {
        Result r = run("office.doc_add", {{"doc", currentDoc_}, {"kind", writeKind_}, {"text", writeText_}});
        resultFor_ = "Add";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        if (r.ok) {
            writeText_.clear();
            if (ImGuiInputTextState* box = ImGui::GetInputTextState(ImGui::GetItemID())) box->ReloadUserBufAndMoveToEnd();
        }
        result_ = std::move(r);
        canvas_.dirty = true;
    }
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
    if (brushTool(canvas_.tool)) {
        runBrushTool();
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
            installedStale_ = true;
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data) openFile(e.drop.data);
            break;
        default: break;
    }
}

void Shell::canvasView() {
    if (!gameActive()) {
        canvasArea();
        return;
    }
    // a level or sprite: the canvas, with its layers, tiles, frames and colours beside it
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::min(ImGui::GetFontSize() * 16, avail.x * 0.4f);
    const ImGuiWindowFlags still = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::BeginChild("game_canvas", {std::max(32.0f, avail.x - side - ImGui::GetStyle().ItemSpacing.x), avail.y}, ImGuiChildFlags_None, still);
    canvasArea();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("game_side", {0, avail.y}, ImGuiChildFlags_Borders);
    gameSide();
    ImGui::EndChild();
}

void Shell::canvasArea() {
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

    if (canvas_.is3d) {
        orbitCanvas(dl, v0, size, hovered, active);
        dl->PopClipRect();
        return;
    }
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
        // pixel art is tiny: let fit zoom further in on levels and sprites
        canvas_.zoom = std::min({size.x * 0.95f / w, size.y * 0.95f / h, gameActive() ? 48.0f : 8.0f});
        canvas_.pan = {(size.x - w * canvas_.zoom) / 2, (size.y - h * canvas_.zoom) / 2};
        canvas_.fit = false;
    }
    if (hovered && io.MouseWheel != 0 && !canvas_.stroking) {
        const ImVec2 img = toImage(io.MousePos);
        canvas_.zoom = std::clamp(canvas_.zoom * std::pow(1.15f, io.MouseWheel), 0.02f, 64.0f);
        canvas_.pan = {io.MousePos.x - v0.x - img.x * canvas_.zoom, io.MousePos.y - v0.y - img.y * canvas_.zoom};
    }
    const bool game = gameActive();
    const bool painting = game ? gamePainting() && !ImGui::IsKeyDown(ImGuiKey_Space)
                               : toolAvailable(canvas_.tool) && canvas_.tool != CanvasTool::Move && !ImGui::IsKeyDown(ImGuiKey_Space) &&
                                     !canvas_.pending;
    auto shownDoc = session_.workspace().get(currentDoc_);
    const bool waveform = shownDoc && shownDoc->kind() == "audio";  // left-drag picks a time range instead
    const bool dragPan = active && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                                    (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !painting && !canvas_.stroking &&
                                     ((!circuitEditing() && !mapsEditing() && !waveform) || ImGui::IsKeyDown(ImGuiKey_Space))));
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
    if (game) {
        gameInput(dl, p0, hovered && painting, active);
        dl->PopClipRect();
        return;
    }

    // painting
    const bool retouch = canvas_.tool == CanvasTool::Heal || canvas_.tool == CanvasTool::Remove;
    const bool retouchBrush = brushTool(canvas_.tool);
    const bool stroke = canvas_.tool == CanvasTool::Brush || canvas_.tool == CanvasTool::Eraser || retouch || retouchBrush;
    if (painting && stroke && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !(retouch && retouchPress(io.MousePos)) && !(retouchBrush && brushToolPress(io.MousePos))) {
        const bool pen = io.MouseSource == ImGuiMouseSource_Pen || penDown_;
        beginStroke(io.MousePos, pen ? penPressure_ : 1.0f, pen, pen && penEraser_);
    }
    if (canvas_.stroking) addStrokePoint(io.MousePos, canvas_.usedPen ? penPressure_ : 1.0f);
    if (canvas_.stroking && !active) endStroke();
    if (painting && canvas_.tool == CanvasTool::Select) {
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) selectPress(io.MousePos);
        if (canvas_.selecting && active) selectDrag(io.MousePos);
        if (canvas_.selecting && !active) selectRelease(io.MousePos);
    }
    const bool vectorTool = canvas_.tool == CanvasTool::Pick || canvas_.tool == CanvasTool::Anchor || vectorDrawTool(canvas_.tool);
    if (painting && vectorTool) {
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) vectorPress(io.MousePos);
        if (canvas_.vecDrag) vectorDrag(io.MousePos);
        if (canvas_.vecDrag && !active) vectorRelease();
        if (hovered) vectorKeys();
    }
    drawVectorDraft(dl, p0);
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
        const ImU32 col = erasing || retouchBrush ? IM_COL32(255, 255, 255, 110)
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
    drawBrushTool(dl, p0, hovered);

    if (waveform) audioCanvas(dl, p0, p1, hovered, active);
    // the playhead over a waveform; a click (not a drag) moves it there
    if (playheadFraction_ >= 0) {
        const float x = p0.x + (p1.x - p0.x) * std::clamp(playheadFraction_, 0.0f, 1.0f);
        dl->AddLine({x, p0.y}, {x, p1.y}, ImGui::ColorConvertFloat4ToU32(accent_), 2.0f);
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0 &&
            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y == 0 && io.MousePos.x >= p0.x && io.MousePos.x <= p1.x && transport_.length > 0) {
            seekTo(transport_.length * (io.MousePos.x - p0.x) / (p1.x - p0.x));
        }
    }
    drawSelection(dl, p0, hovered);
    if (vectorTool && toolAvailable(canvas_.tool)) drawVector(dl, p0);
    if (circuitDoc()) circuitCanvas(dl, p0, hovered, ImGui::IsItemClicked(ImGuiMouseButton_Left));
    if (mapsDoc()) mapsCanvas(dl, p0, hovered, ImGui::IsItemClicked(ImGuiMouseButton_Left));
    dl->PopClipRect();
}

// Audacity's way: drag over the waveform to pick a time range (shaded), then any action
// with start/end gets it; Delete cuts it, a click clears it and moves the playhead.
void Shell::audioCanvas(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool hovered, bool active) {
    AudioSelection& sel = audioSel_;
    if (sel.doc != currentDoc_) sel = {currentDoc_};
    if (sel.seen != changes_ || sel.lengthDoc != currentDoc_) {
        sel.seen = changes_;
        sel.lengthDoc = currentDoc_;
        Result info = query("audio.info", {{"doc", currentDoc_}});
        sel.length = 0;
        if (const Value* v = info.ok ? info.field("seconds") : nullptr) {
            if (auto d = std::get_if<double>(v)) sel.length = *d;
            if (auto i = std::get_if<std::int64_t>(v)) sel.length = static_cast<double>(*i);
        }
        if (sel.b > sel.length) sel.b = sel.length;
        if (sel.a >= sel.b) sel.a = sel.b = -1;
    }
    if (sel.length <= 0 || p1.x - p0.x < 4) return;
    ImGuiIO& io = ImGui::GetIO();
    auto timeAt = [&](float x) { return std::clamp((x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f) * sel.length; };
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_Space)) {
        sel.dragging = true;
        sel.a = sel.b = timeAt(io.MousePos.x);
    }
    static double anchor = 0;
    if (sel.dragging) {
        if (ImGui::IsItemActivated()) anchor = sel.a;
        const double t = timeAt(io.MousePos.x);
        sel.a = std::min(anchor, t);
        sel.b = std::max(anchor, t);
        if (!active) {
            sel.dragging = false;
            const double minSpan = sel.length * 3.0 / (p1.x - p0.x);  // under 3 px is a click
            if (sel.b - sel.a < minSpan) {
                seekTo(anchor);
                sel.a = sel.b = -1;
            } else {
                fillAudioRange(true);
                char text[96];
                std::snprintf(text, sizeof text, "Selected %.2f s to %.2f s (%.2f s). Pick an action, or Delete to cut it.", sel.a, sel.b,
                              sel.b - sel.a);
                result_ = Result::success(text);
                resultFor_ = "Selection";
            }
        }
    }
    if (sel.any() && hovered && !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        Result r = run("audio.cut", {{"doc", currentDoc_}, {"start", sel.a}, {"end", sel.b}});
        resultFor_ = "Cut";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        sel.a = sel.b = -1;
    }
    if (sel.any() || sel.dragging) {
        const float x0 = p0.x + static_cast<float>(sel.a / sel.length) * (p1.x - p0.x);
        const float x1 = p0.x + static_cast<float>(sel.b / sel.length) * (p1.x - p0.x);
        ImVec4 c = accent_;
        dl->AddRectFilled({x0, p0.y}, {x1, p1.y}, ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.28f}));
        dl->AddLine({x0, p0.y}, {x0, p1.y}, ImGui::ColorConvertFloat4ToU32(c), 1.5f);
        dl->AddLine({x1, p0.y}, {x1, p1.y}, ImGui::ColorConvertFloat4ToU32(c), 1.5f);
    }
}

}  // namespace wetwaresuite::gui
