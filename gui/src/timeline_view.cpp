// The timeline under video edits and audio projects: tracks of clips you drag
// to move (also onto another track of the same kind), drag by an edge to trim,
// split at the cursor (S) and delete (Delete). The document describes its
// tracks (Document::timeline) and names the command for each edit
// (Document::timelineCommand), so every drag is one ordinary, undoable command.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shell.hpp"
#include "wetwaresuite/core/cli.hpp"

namespace wetwaresuite::gui {

namespace {

constexpr float kEdge = 6.0f;  // px at each end of a clip that trim instead of move

std::string clockText(double s) {
    char buf[32];
    const int m = static_cast<int>(s / 60);
    std::snprintf(buf, sizeof buf, "%d:%04.1f", m, s - m * 60);
    return buf;
}

double niceStep(double secondsPerPixel) {
    const double want = secondsPerPixel * 90;  // a label every ~90 px
    for (double s : {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0}) {
        if (s >= want) return s;
    }
    return 1200.0;
}

}  // namespace

float Shell::timelineHeight(const TimelineLayout& layout) const {
    const float row = ImGui::GetFontSize() * 2.4f;
    const float chrome = ImGui::GetFrameHeightWithSpacing() + ImGui::GetFontSize() * 1.6f + 8;
    return chrome + row * static_cast<float>(std::max<std::size_t>(1, layout.tracks.size()));
}

void Shell::runTimelineEdit(const TimelineLayout& layout, const TimelineEdit& edit) {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return;
    const std::string line = doc->timelineCommand(edit);
    if (line.empty()) return;
    std::vector<std::string> tokens = cli::split(line);
    if (tokens.empty()) return;
    const std::string id = tokens.front();
    const CommandInfo* info = find(id);
    if (!info) return;
    tokens.erase(tokens.begin());
    tokens.push_back(layout.docParam + "=" + currentDoc_);
    Result r;
    try {
        r = run(id, cli::parseArgs(*info, tokens));
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    resultFor_ = info->title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::timelineView(const TimelineLayout& layout) {
    TimelineState& st = timeline_;
    if (st.doc != currentDoc_) st = TimelineState{currentDoc_};
    ImGuiIO& io = ImGui::GetIO();

    // toolbar
    const TimelineClip* sel = nullptr;
    const TimelineTrack* selTrack = nullptr;
    double end = 1;
    for (const auto& t : layout.tracks) {
        for (const auto& c : t.clips) {
            end = std::max(end, c.start + c.length);
            if (t.id == st.selTrack && c.id == st.selClip) sel = &c, selTrack = &t;
        }
    }
    const bool canSplit = sel && st.cursor > sel->start + 0.01 && st.cursor < sel->start + sel->length - 0.01;
    ImGui::BeginDisabled(!canSplit);
    if (ImGui::Button("Split") || (canSplit && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyCtrl)) {
        runTimelineEdit(layout, {TimelineEditKind::Split, sel->id, selTrack->id, {}, 0, 0, st.cursor});
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Cut the selected clip at the cursor (S)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Delete") || (sel && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
        runTimelineEdit(layout, {TimelineEditKind::Delete, sel->id, selTrack->id});
        st.selClip.clear();
        sel = nullptr;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Remove the selected clip (Delete)");
    ImGui::SameLine();
    if (ImGui::Button("Fit")) st.zoom = 0;
    ImGui::SetItemTooltip("Show the whole timeline");
    ImGui::SameLine();
    ImGui::TextDisabled("cursor %s   drag clips to move, edges to trim, Ctrl+scroll to zoom", clockText(st.cursor).c_str());

    // geometry
    const float nameW = ImGui::GetFontSize() * 6.5f;
    const float rulerH = ImGui::GetFontSize() * 1.4f;
    const float rowH = ImGui::GetFontSize() * 2.4f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(avail.x, 64.0f), rulerH + rowH * static_cast<float>(std::max<std::size_t>(1, layout.tracks.size()))};
    ImGui::InvisibleButton("timeline", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const ImVec2 o = ImGui::GetItemRectMin();
    const float laneW = std::max(16.0f, size.x - nameW);
    const double fitPps = laneW / (end * 1.05);
    if (st.zoom <= 0) {
        st.zoom = fitPps;
        st.scroll = 0;
    }
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            const double at = st.scroll + (io.MousePos.x - o.x - nameW) / st.zoom;
            st.zoom = std::clamp(st.zoom * std::pow(1.2, static_cast<double>(io.MouseWheel)), fitPps * 0.5, 2000.0);
            st.scroll = at - (io.MousePos.x - o.x - nameW) / st.zoom;
        } else {
            st.scroll -= io.MouseWheel * 60.0 / st.zoom;
        }
    }
    st.scroll = std::clamp(st.scroll, 0.0, std::max(0.0, end * 1.05 - laneW / st.zoom));
    auto toX = [&](double t) { return o.x + nameW + static_cast<float>((t - st.scroll) * st.zoom); };
    auto toT = [&](float x) { return st.scroll + (x - o.x - nameW) / st.zoom; };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool dark = darkLook();
    const ImU32 bg = dark ? IM_COL32(32, 34, 38, 255) : IM_COL32(226, 228, 232, 255);
    const ImU32 lane = dark ? IM_COL32(40, 42, 47, 255) : IM_COL32(236, 238, 242, 255);
    const ImU32 line = dark ? IM_COL32(60, 63, 70, 255) : IM_COL32(200, 202, 208, 255);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddRectFilled(o, {o.x + size.x, o.y + size.y}, bg);
    dl->PushClipRect(o, {o.x + size.x, o.y + size.y}, true);

    // ruler
    const double step = niceStep(1.0 / st.zoom);
    for (double t = std::floor(st.scroll / step) * step; toX(t) < o.x + size.x; t += step) {
        if (t < 0) continue;
        const float x = toX(t);
        if (x < o.x + nameW) continue;
        dl->AddLine({x, o.y + rulerH * 0.55f}, {x, o.y + size.y}, line);
        dl->AddText({x + 3, o.y + 1}, muted, clockText(t).c_str());
    }

    // which row and clip the mouse is over
    const auto rowAt = [&](float y) { return static_cast<int>(std::floor((y - o.y - rulerH) / rowH)); };
    const int hoverRow = rowAt(io.MousePos.y);
    const double hoverT = toT(io.MousePos.x);

    // start a drag: on a clip body (move), an edge (trim), or the ruler / empty lane (cursor)
    if (ImGui::IsItemActivated()) {
        st.drag = TimelineState::Drag::None;
        if (io.MousePos.y < o.y + rulerH || io.MousePos.x < o.x + nameW) {
            st.drag = TimelineState::Drag::Cursor;
        } else if (hoverRow >= 0 && hoverRow < static_cast<int>(layout.tracks.size())) {
            const auto& t = layout.tracks[static_cast<std::size_t>(hoverRow)];
            st.drag = TimelineState::Drag::Cursor;
            for (const auto& c : t.clips) {
                const float x0 = toX(c.start), x1 = toX(c.start + c.length);
                if (io.MousePos.x < x0 - 1 || io.MousePos.x > x1 + 1) continue;
                st.selClip = c.id;
                st.selTrack = t.id;
                st.grabT = hoverT;
                st.origStart = c.start;
                st.origLength = c.length;
                st.headroom = c.headroom;
                st.tailroom = c.tailroom;
                const bool narrow = x1 - x0 < kEdge * 3;
                if (!narrow && io.MousePos.x < x0 + kEdge && c.trimStart) st.drag = TimelineState::Drag::TrimStart;
                else if (!narrow && io.MousePos.x > x1 - kEdge) st.drag = TimelineState::Drag::TrimEnd;
                else st.drag = TimelineState::Drag::Move;
                break;
            }
            if (st.drag == TimelineState::Drag::Cursor) st.selClip.clear();
        }
    }
    if (active && st.drag == TimelineState::Drag::Cursor) st.cursor = std::max(0.0, hoverT);

    // snapping: to 0, the cursor and other clips' edges within 8 px
    auto snap = [&](double t, const std::string& skipClip) {
        double best = t, bestPx = 8.0 / st.zoom;
        auto consider = [&](double c) {
            if (std::fabs(c - t) < bestPx) best = c, bestPx = std::fabs(c - t);
        };
        consider(0);
        consider(st.cursor);
        for (const auto& tr : layout.tracks)
            for (const auto& c : tr.clips)
                if (c.id != skipClip || tr.id != st.selTrack) consider(c.start), consider(c.start + c.length);
        return best;
    };

    // the clip being dragged, as it would end up
    double dragStart = st.origStart, dragLength = st.origLength;
    std::string dropTrack = st.selTrack;
    const bool dragging = active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) &&
                          (st.drag == TimelineState::Drag::Move || st.drag == TimelineState::Drag::TrimStart || st.drag == TimelineState::Drag::TrimEnd);
    if (dragging || (ImGui::IsItemDeactivated() && st.drag != TimelineState::Drag::None && st.drag != TimelineState::Drag::Cursor)) {
        const double delta = hoverT - st.grabT;
        if (st.drag == TimelineState::Drag::Move) {
            dragStart = std::max(0.0, st.origStart + delta);
            const double snappedStart = snap(dragStart, st.selClip), snappedEnd = snap(dragStart + st.origLength, st.selClip);
            if (snappedStart != dragStart) dragStart = snappedStart;
            else if (snappedEnd != dragStart + st.origLength) dragStart = std::max(0.0, snappedEnd - st.origLength);
            const TimelineTrack* from = nullptr;
            for (const auto& t : layout.tracks) if (t.id == st.selTrack) from = &t;
            if (from && hoverRow >= 0 && hoverRow < static_cast<int>(layout.tracks.size()) &&
                layout.tracks[static_cast<std::size_t>(hoverRow)].kind == from->kind) {
                dropTrack = layout.tracks[static_cast<std::size_t>(hoverRow)].id;
            }
        } else if (st.drag == TimelineState::Drag::TrimStart) {
            double lo = st.headroom >= 0 ? st.origStart - st.headroom : 0.0;
            lo = std::max(0.0, lo);
            dragStart = std::clamp(snap(st.origStart + delta, st.selClip), lo, st.origStart + st.origLength - 0.05);
            dragLength = st.origStart + st.origLength - dragStart;
        } else {
            const double hi = st.tailroom >= 0 ? st.origLength + st.tailroom : 1e9;
            dragLength = std::clamp(snap(st.origStart + st.origLength + delta, st.selClip) - st.origStart, 0.05, hi);
        }
    }
    if (ImGui::IsItemDeactivated() && st.drag != TimelineState::Drag::None && st.drag != TimelineState::Drag::Cursor) {
        const bool moved = std::fabs(dragStart - st.origStart) > 1e-4 || std::fabs(dragLength - st.origLength) > 1e-4 || dropTrack != st.selTrack;
        if (moved) {
            TimelineEdit e;
            e.clip = st.selClip;
            e.track = st.selTrack;
            if (st.drag == TimelineState::Drag::Move) {
                e.kind = TimelineEditKind::Move;
                e.start = dragStart;
                e.toTrack = dropTrack;
            } else if (st.drag == TimelineState::Drag::TrimStart) {
                e.kind = TimelineEditKind::TrimStart;
                e.start = dragStart;
            } else {
                e.kind = TimelineEditKind::TrimEnd;
                e.length = dragLength;
            }
            runTimelineEdit(layout, e);
            // a moved clip can change ids (audio projects number clips per track): select nothing rather than the wrong one
            if (dropTrack != st.selTrack) st.selClip.clear();
        }
        st.drag = TimelineState::Drag::None;
    }

    // lanes and clips
    const ImU32 clipCols[] = {IM_COL32(86, 140, 220, 255), IM_COL32(90, 170, 120, 255)};
    for (std::size_t ri = 0; ri < layout.tracks.size(); ++ri) {
        const auto& t = layout.tracks[ri];
        const float y0 = o.y + rulerH + rowH * static_cast<float>(ri), y1 = y0 + rowH - 2;
        dl->AddRectFilled({o.x + nameW, y0}, {o.x + size.x, y1}, lane);
        dl->AddText({o.x + 6, y0 + (rowH - ImGui::GetFontSize()) / 2}, text, t.name.c_str());
        const ImU32 col = clipCols[t.kind == "audio" ? 1 : 0];
        for (const auto& c : t.clips) {
            const bool isSel = c.id == st.selClip && t.id == st.selTrack;
            const bool isDragged = isSel && dragging;
            const float x0 = toX(c.start), x1 = std::max(x0 + 2, toX(c.start + c.length));
            dl->AddRectFilled({x0, y0 + 3}, {x1, y1 - 3}, isDragged ? (col & 0x60FFFFFF) : col, 4);
            if (isSel) dl->AddRect({x0, y0 + 3}, {x1, y1 - 3}, ImGui::ColorConvertFloat4ToU32(accent_), 4, 0, 2.5f);
            dl->PushClipRect({std::max(x0, o.x + nameW), y0}, {x1, y1}, true);
            dl->AddText({x0 + 6, y0 + (rowH - ImGui::GetFontSize()) / 2}, IM_COL32(255, 255, 255, 235), c.label.c_str());
            dl->PopClipRect();
        }
        if (dragging && t.id == dropTrack) {
            // ghost of where it will land
            const float x0 = toX(dragStart), x1 = std::max(x0 + 2, toX(dragStart + dragLength));
            dl->AddRect({x0, y0 + 2}, {x1, y1 - 2}, ImGui::ColorConvertFloat4ToU32(accent_), 4, 0, 2.0f);
            dl->AddText({x0 + 4, y1 - ImGui::GetFontSize() - 2}, text, clockText(st.drag == TimelineState::Drag::TrimEnd ? dragStart + dragLength : dragStart).c_str());
        }
    }
    if (layout.tracks.empty()) dl->AddText({o.x + nameW + 8, o.y + rulerH + 8}, muted, "No tracks yet: add a clip with an action on the right.");

    // the cursor
    const float cx = toX(st.cursor);
    if (cx >= o.x + nameW) dl->AddLine({cx, o.y}, {cx, o.y + size.y}, IM_COL32(240, 80, 70, 255), 2.0f);

    // cursor shape over edges
    if (hovered && hoverRow >= 0 && hoverRow < static_cast<int>(layout.tracks.size())) {
        for (const auto& c : layout.tracks[static_cast<std::size_t>(hoverRow)].clips) {
            const float x0 = toX(c.start), x1 = toX(c.start + c.length);
            if (x1 - x0 >= kEdge * 3 && ((std::fabs(io.MousePos.x - x0) < kEdge && c.trimStart) || std::fabs(io.MousePos.x - x1) < kEdge)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
        }
    }
    dl->PopClipRect();
}

}  // namespace wetwaresuite::gui
