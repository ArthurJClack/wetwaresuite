// The cell grid shown for spreadsheets: sheet tabs, a formula bar and a
// scrolling grid you click and type into. The document hands out only the
// cells on screen (Document::gridCell) and names the command for typing into a
// cell (Document::gridSetCommand), so every edit is one ordinary, undoable
// command, the same one a script or the MCP server would run.
#include <algorithm>
#include <cmath>

#include "shell.hpp"
#include "wetwaresuite/core/cli.hpp"

#include <imgui_stdlib.h>

namespace wetwaresuite::gui {

namespace {

constexpr int kMaxRows = 1048576;
constexpr int kMaxCols = 16384;

std::string columnName(int col) {
    std::string s;
    for (int c = col + 1; c > 0; c = (c - 1) / 26) s.insert(s.begin(), static_cast<char>('A' + (c - 1) % 26));
    return s;
}

std::string cellName(int row, int col) { return columnName(col) + std::to_string(row + 1); }

void appendUtf8(std::string& out, unsigned c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

int caretToEnd(ImGuiInputTextCallbackData* data) {
    auto* st = static_cast<GridState*>(data->UserData);
    if (st && st->caretToEnd) {
        data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
        st->caretToEnd = false;
    }
    return 0;
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32({x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, 1});
}

}  // namespace

bool Shell::commitCell(const CellGrid& layout, int sheet, int row, int col, const std::string& input) {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return false;
    GridCell now;
    const bool had = doc->gridCell(sheet, row, col, now);
    if ((had ? now.input : std::string()) == input) return true;  // nothing changed
    std::vector<std::string> tokens = doc->gridSetCommand(sheet, row, col, input);
    if (tokens.empty()) return false;
    const std::string id = tokens.front();
    const CommandInfo* info = find(id);
    if (!info) return false;
    tokens.erase(tokens.begin());
    tokens.push_back(layout.docParam + "=" + currentDoc_);
    Result r;
    try {
        r = run(id, cli::parseArgs(*info, tokens));
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    grid_.formulaRow = -1;  // reload the formula bar
    if (!r.ok) {
        resultFor_ = info->title;
        r.message = friendlyError(r.message);
        result_ = std::move(r);
        scrollToResult_ = true;
    }
    return r.ok;
}

void Shell::gridView(const CellGrid& layout) {
    GridState& st = grid_;
    if (st.doc != currentDoc_) st = GridState{currentDoc_};
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return;
    const int sheets = static_cast<int>(layout.sheets.size());
    const bool firstFrame = st.sheet < 0;
    if (firstFrame) st.sheet = std::clamp(layout.active, 0, sheets - 1);
    st.sheet = std::clamp(st.sheet, 0, sheets - 1);
    ImGuiIO& io = ImGui::GetIO();
    const float unit = ImGui::GetFontSize();

    // sheet tabs
    if (ImGui::BeginTabBar("##sheets", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < sheets; ++i) {
            ImGui::PushID(i);
            const ImGuiTabItemFlags flags = (firstFrame && i == st.sheet) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(layout.sheets[static_cast<std::size_t>(i)].c_str(), nullptr, flags)) {
                if (i != st.sheet && !firstFrame) {
                    st.sheet = i;
                    st.editing = false;
                    st.row = st.col = 0;
                    st.scrollToSel = true;
                }
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }

    // formula bar: the selected cell's name and what's typed in it
    if (st.formulaRow != st.row || st.formulaCol != st.col || st.formulaSheet != st.sheet) {
        GridCell c;
        st.formula = doc->gridCell(st.sheet, st.row, st.col, c) ? c.input : std::string();
        st.formulaRow = st.row, st.formulaCol = st.col, st.formulaSheet = st.sheet;
    }
    ImGui::AlignTextToFramePadding();
    const std::string here = cellName(st.row, st.col);
    ImGui::TextUnformatted(here.c_str());
    ImGui::SameLine(unit * 4.5f);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##formula", "type a value or =formula, Enter to put it in the cell", &st.formula,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        commitCell(layout, st.sheet, st.row, st.col, st.formula);
        st.row = std::min(st.row + 1, kMaxRows - 1);
        st.scrollToSel = true;
    }
    ImGui::SetItemTooltip("Enter writes it into %s. Start with = for a formula, e.g. =SUM(A1:A9)", here.c_str());

    // grid geometry
    const float rowH = ImGui::GetFrameHeight();
    const float headH = rowH;
    const float headW = ImGui::CalcTextSize("0000000").x + 8;
    const float charW = ImGui::CalcTextSize("0").x;
    const int rows = std::min(kMaxRows, std::max({layout.usedRows + 100, st.row + 50, 200}));
    const int cols = std::min(kMaxCols, std::max({layout.usedCols + 8, st.col + 8, 26}));
    std::vector<float> colX(static_cast<std::size_t>(cols) + 1, 0.0f);
    for (int c = 0; c < cols; ++c) {
        const float w = std::max(unit * 1.5f, static_cast<float>(doc->gridColumnChars(st.sheet, c)) * charW + 10);
        colX[static_cast<std::size_t>(c) + 1] = colX[static_cast<std::size_t>(c)] + w;
    }
    const float totalW = headW + colX.back(), totalH = headH + rowH * static_cast<float>(rows);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::BeginChild("##grid", {0, 0}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float scrollX = ImGui::GetScrollX(), scrollY = ImGui::GetScrollY();
    const float viewW = size.x - headW - ImGui::GetStyle().ScrollbarSize;
    const float viewH = size.y - headH - ImGui::GetStyle().ScrollbarSize;
    auto cellMin = [&](int r, int c) {
        return ImVec2{origin.x + headW + colX[static_cast<std::size_t>(c)] - scrollX, origin.y + headH + rowH * static_cast<float>(r) - scrollY};
    };

    // keep the selection on screen after keyboard moves
    if (st.scrollToSel) {
        const float x0 = colX[static_cast<std::size_t>(st.col)], x1 = colX[static_cast<std::size_t>(st.col) + 1];
        const float y0 = rowH * static_cast<float>(st.row), y1 = y0 + rowH;
        if (x0 < scrollX) ImGui::SetScrollX(x0);
        else if (x1 > scrollX + viewW) ImGui::SetScrollX(x1 - viewW);
        if (y0 < scrollY) ImGui::SetScrollY(y0);
        else if (y1 > scrollY + viewH) ImGui::SetScrollY(y1 - viewH);
        st.scrollToSel = false;
    }

    const int r0 = std::clamp(static_cast<int>(scrollY / rowH), 0, rows - 1);
    const int r1 = std::clamp(static_cast<int>((scrollY + size.y) / rowH) + 1, 0, rows);
    const int c0 = std::clamp(static_cast<int>(std::upper_bound(colX.begin(), colX.end(), scrollX) - colX.begin()) - 1, 0, cols - 1);
    int c1 = c0;
    while (c1 < cols && colX[static_cast<std::size_t>(c1)] < scrollX + size.x) ++c1;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 line = mix(bg, text, 0.15f);
    const ImU32 headBg = ImGui::GetColorU32(ImGuiCol_TableHeaderBg);
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    const ImU32 headSel = mix(headBg, accent, 0.35f);
    const ImU32 errorCol = darkLook() ? IM_COL32(255, 120, 110, 255) : IM_COL32(190, 20, 10, 255);
    const ImVec2 body{origin.x + headW, origin.y + headH};
    dl->AddRectFilled(body, {origin.x + size.x, origin.y + size.y}, bg);

    // cells: fills and data bars under the grid lines, text (and merged areas) over them
    const float pad = 4;
    auto rgb = [](std::uint32_t c, int alpha = 255) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, alpha); };
    auto luma = [](std::uint32_t c) { return 0.299f * ((c >> 16) & 255) + 0.587f * ((c >> 8) & 255) + 0.114f * (c & 255); };
    const float bgLuma = darkLook() ? 40.0f : 230.0f;
    constexpr std::uint32_t kSet = 0x1000000;
    struct Shown {
        int r, c;
        GridCell cell;
    };
    std::vector<Shown> shown;
    auto spanRect = [&](int r, int c, const GridCell& cell, ImVec2& a, ImVec2& b) {
        a = cellMin(r, c);
        const int ce = std::min(cols, c + std::max(1, cell.spanCols));
        b = {a.x + colX[static_cast<std::size_t>(ce)] - colX[static_cast<std::size_t>(c)], a.y + rowH * static_cast<float>(std::max(1, cell.spanRows))};
    };
    for (int r = r0; r < r1; ++r) {
        for (int c = c0; c < c1; ++c) {
            GridCell cell;
            if (!doc->gridCell(st.sheet, r, c, cell) || cell.covered) continue;
            ImVec2 a, b;
            spanRect(r, c, cell, a, b);
            const ImVec2 ca{std::max(a.x, body.x), std::max(a.y, body.y)};
            if (ca.x < b.x && ca.y < b.y) {
                if (cell.fill & kSet) dl->AddRectFilled(ca, b, rgb(cell.fill));
                if (cell.bar >= 0) {
                    const float bw = (b.x - a.x - 4) * std::clamp(cell.bar, 0.0f, 1.0f);
                    dl->PushClipRect(ca, b, true);
                    dl->AddRectFilled({a.x + 2, a.y + 3}, {a.x + 2 + bw, b.y - 3}, rgb(cell.barColor & kSet ? cell.barColor : 0x638EC6, 110));
                    dl->PopClipRect();
                }
            }
            if (!cell.text.empty() || cell.spanRows > 1 || cell.spanCols > 1) shown.push_back({r, c, std::move(cell)});
        }
    }
    // grid lines
    for (int c = c0; c <= c1 && c <= cols; ++c) {
        const float x = origin.x + headW + colX[static_cast<std::size_t>(c)] - scrollX;
        if (x >= body.x) dl->AddLine({x, body.y}, {x, origin.y + size.y}, line);
    }
    for (int r = r0; r <= r1; ++r) {
        const float y = origin.y + headH + rowH * static_cast<float>(r) - scrollY;
        if (y >= body.y) dl->AddLine({body.x, y}, {origin.x + size.x, y}, line);
    }
    for (const auto& [r, c, cell] : shown) {
        ImVec2 a, b;
        spanRect(r, c, cell, a, b);
        const bool merged = cell.spanRows > 1 || cell.spanCols > 1;
        const ImVec4 clip{std::max(a.x, body.x), std::max(a.y, body.y), b.x - 1, b.y};
        if (merged && clip.x < clip.z && clip.y < clip.w) {
            // cover the lines inside the merged area
            dl->AddRectFilled({std::max(a.x + 1, body.x), std::max(a.y + 1, body.y)}, {b.x, b.y}, cell.fill & kSet ? rgb(cell.fill) : bg);
        }
        if (cell.text.empty()) continue;
        ImU32 col = cell.error ? errorCol : text;
        if (!cell.error) {
            const float under = cell.fill & kSet ? luma(cell.fill) : bgLuma;
            if (cell.color & kSet) {
                if (std::abs(luma(cell.color) - under) > 60) col = rgb(cell.color);
                else if (cell.fill & kSet) col = under > 128 ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
            } else if (cell.fill & kSet) {
                col = under > 128 ? IM_COL32(20, 20, 20, 255) : IM_COL32(240, 240, 240, 255);
            }
        }
        const float tw = ImGui::CalcTextSize(cell.text.c_str()).x;
        const char align = cell.align ? cell.align : merged ? 'c' : cell.number ? 'r' : 'l';
        const float x = align == 'r' ? b.x - pad - tw : align == 'c' ? (a.x + b.x - tw) * 0.5f : a.x + pad;
        const ImVec2 p{x, merged ? (a.y + b.y - unit) * 0.5f : a.y + (rowH - unit) * 0.5f};
        dl->AddText(nullptr, 0, p, col, cell.text.c_str(), nullptr, 0, &clip);
        if (cell.bold) dl->AddText(nullptr, 0, {p.x + 1, p.y}, col, cell.text.c_str(), nullptr, 0, &clip);
        dl->PushClipRect({clip.x, clip.y}, {clip.z, clip.w}, true);
        if (cell.underline) dl->AddLine({p.x, p.y + unit}, {p.x + tw, p.y + unit}, col);
        if (cell.strike) dl->AddLine({p.x, p.y + unit * 0.55f}, {p.x + tw, p.y + unit * 0.55f}, col);
        dl->PopClipRect();
    }
    // selection
    {
        const ImVec2 a = cellMin(st.row, st.col);
        const float w = colX[static_cast<std::size_t>(st.col) + 1] - colX[static_cast<std::size_t>(st.col)];
        dl->PushClipRect(body, {origin.x + size.x, origin.y + size.y}, true);
        dl->AddRect({a.x, a.y}, {a.x + w + 1, a.y + rowH + 1}, accent, 0, 0, 2.0f);
        dl->PopClipRect();
    }
    // headers on top, so they stay put while the cells scroll under them
    dl->AddRectFilled(origin, {origin.x + size.x, origin.y + headH}, headBg);
    dl->AddRectFilled(origin, {origin.x + headW, origin.y + size.y}, headBg);
    for (int c = c0; c < c1; ++c) {
        const float x = origin.x + headW + colX[static_cast<std::size_t>(c)] - scrollX;
        const float w = colX[static_cast<std::size_t>(c) + 1] - colX[static_cast<std::size_t>(c)];
        const ImVec4 clip{std::max(x, body.x), origin.y, x + w, origin.y + headH};
        if (c == st.col) dl->AddRectFilled({clip.x, clip.y}, {clip.z, clip.w}, headSel);
        const std::string name = columnName(c);
        const float tw = ImGui::CalcTextSize(name.c_str()).x;
        dl->AddText(nullptr, 0, {x + (w - tw) * 0.5f, origin.y + (headH - unit) * 0.5f}, c == st.col ? text : muted, name.c_str(), nullptr, 0, &clip);
        if (x >= body.x) dl->AddLine({x, origin.y}, {x, body.y}, line);
    }
    for (int r = r0; r < r1; ++r) {
        const float y = origin.y + headH + rowH * static_cast<float>(r) - scrollY;
        const ImVec4 clip{origin.x, std::max(y, body.y), origin.x + headW, y + rowH};
        if (r == st.row) dl->AddRectFilled({clip.x, clip.y}, {clip.z, clip.w}, headSel);
        const std::string name = std::to_string(r + 1);
        const float tw = ImGui::CalcTextSize(name.c_str()).x;
        dl->AddText(nullptr, 0, {origin.x + headW - pad - tw, y + (rowH - unit) * 0.5f}, r == st.row ? text : muted, name.c_str(), nullptr, 0, &clip);
        if (y >= body.y) dl->AddLine({origin.x, y}, {body.x, y}, line);
    }
    dl->AddLine({origin.x, body.y}, {origin.x + size.x, body.y}, line);
    dl->AddLine({body.x, origin.y}, {body.x, origin.y + size.y}, line);

    auto move = [&](int dr, int dc) {
        st.row = std::clamp(st.row + dr, 0, kMaxRows - 1);
        st.col = std::clamp(st.col + dc, 0, kMaxCols - 1);
        st.scrollToSel = true;
    };
    auto startEdit = [&](std::string initial, bool typed) {
        st.editing = true;
        st.focusEdit = true;
        st.caretToEnd = true;
        if (typed) {
            st.buffer = std::move(initial);
        } else {
            GridCell c;
            st.buffer = doc->gridCell(st.sheet, st.row, st.col, c) ? c.input : std::string();
        }
        st.scrollToSel = true;
    };

    // mouse: click selects, double-click edits
    const bool focused = ImGui::IsWindowFocused();
    // SDL only sends typed characters while text input is on, and ImGui turns it on just for
    // its own text boxes: keep it on while the grid has focus, so typing starts an edit.
    if (focused && !st.editing && !SDL_TextInputActive(window_)) {
        SDL_StartTextInput(window_);
        st.textInput = true;
    } else if (!focused && st.textInput) {
        if (!io.WantTextInput) SDL_StopTextInput(window_);  // unless a text box has it now
        st.textInput = false;
    }
    if (ImGui::IsWindowHovered() && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        const ImVec2 m = io.MousePos;
        if (m.x > body.x && m.y > body.y && m.x < origin.x + size.x - style.ScrollbarSize && m.y < origin.y + size.y - style.ScrollbarSize) {
            const float gx = m.x - body.x + scrollX, gy = m.y - body.y + scrollY;
            const int r = std::clamp(static_cast<int>(gy / rowH), 0, rows - 1);
            const int c = std::clamp(static_cast<int>(std::upper_bound(colX.begin(), colX.end(), gx) - colX.begin()) - 1, 0, cols - 1);
            if (st.editing && (r != st.row || c != st.col)) {
                commitCell(layout, st.sheet, st.row, st.col, st.buffer);
                st.editing = false;
            }
            st.row = r, st.col = c;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) startEdit({}, false);
        }
    }

    // editing in the cell
    if (st.editing) {
        const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        const bool tab = ImGui::IsKeyPressed(ImGuiKey_Tab, false);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            st.editing = false;
        } else if ((enter || tab) && !st.focusEdit) {
            commitCell(layout, st.sheet, st.row, st.col, st.buffer);
            st.editing = false;
            if (enter) move(io.KeyShift ? -1 : 1, 0);
            else move(0, io.KeyShift ? -1 : 1);
        } else {
            const ImVec2 a = cellMin(st.row, st.col);
            const float w = colX[static_cast<std::size_t>(st.col) + 1] - colX[static_cast<std::size_t>(st.col)];
            ImGui::SetCursorScreenPos({std::max(a.x, body.x), std::max(a.y, body.y)});
            ImGui::SetNextItemWidth(std::max(w, unit * 10));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {pad, (rowH - unit) * 0.5f});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
            if (st.focusEdit) {
                // the editor takes focus a frame or two late: keep what's typed meanwhile
                for (ImWchar ch : io.InputQueueCharacters) {
                    if (ch >= 32 && ch != 127) appendUtf8(st.buffer, ch);
                }
                io.InputQueueCharacters.resize(0);
                st.caretToEnd = true;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::InputText("##cell", &st.buffer, ImGuiInputTextFlags_CallbackAlways, caretToEnd, &st);
            if (ImGui::IsItemActive()) st.focusEdit = false;
            if (ImGui::IsItemDeactivated() && !st.focusEdit) {  // clicked away: keep what was typed
                commitCell(layout, st.sheet, st.row, st.col, st.buffer);
                st.editing = false;
            }
            ImGui::PopStyleVar(2);
        }
    } else if (focused && !io.KeyCtrl && !io.KeyAlt) {
        // keyboard on the grid
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move(-1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move(1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move(0, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move(0, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) move(-static_cast<int>(viewH / rowH), 0);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) move(static_cast<int>(viewH / rowH), 0);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) move(0, -st.col);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) move(io.KeyShift ? -1 : 1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) move(0, io.KeyShift ? -1 : 1);
        if (ImGui::IsKeyPressed(ImGuiKey_F2)) startEdit({}, false);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            commitCell(layout, st.sheet, st.row, st.col, "");
        }
        // typing starts an edit that replaces the cell, like every spreadsheet
        std::string typed;
        for (ImWchar ch : io.InputQueueCharacters) {
            if (ch >= 32 && ch != 127) appendUtf8(typed, ch);
        }
        if (!typed.empty()) startEdit(typed, true);
    } else if (focused && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            GridCell c;
            ImGui::SetClipboardText(doc->gridCell(st.sheet, st.row, st.col, c) ? c.input.c_str() : "");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            if (const char* clip = ImGui::GetClipboardText()) {
                std::string s = clip;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (!s.empty()) commitCell(layout, st.sheet, st.row, st.col, s);  // rows/tabs paste as a block
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            GridCell c;
            if (doc->gridCell(st.sheet, st.row, st.col, c)) {
                ImGui::SetClipboardText(c.input.c_str());
                commitCell(layout, st.sheet, st.row, st.col, "");
            }
        }
    }

    // the scroll area: the whole grid's size
    ImGui::SetCursorPos({totalW, totalH});
    ImGui::Dummy({1, 1});
    ImGui::EndChild();
}

}  // namespace wetwaresuite::gui
