// The code editor widget: line numbers, syntax colours from code.highlight,
// selection, clipboard, auto-indent, comment toggling and error marks. It only
// keeps the text while you type; the code panel sends it to the document with
// code.set_text when you pause, so undo and scripts see every change.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "imgui_internal.h"
#include "tool_panels.hpp"

namespace wetwaresuite::gui {

namespace {

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

int nextCol(const std::string& s, int col) {
    const int n = static_cast<int>(s.size());
    if (col >= n) return n;
    ++col;
    while (col < n && continuation(s[static_cast<std::size_t>(col)])) ++col;
    return col;
}

int prevCol(const std::string& s, int col) {
    if (col <= 0) return 0;
    --col;
    while (col > 0 && continuation(s[static_cast<std::size_t>(col)])) --col;
    return col;
}

bool wordChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || u >= 0x80;
}

int wordLeft(const std::string& s, int col) {
    while (col > 0 && !wordChar(s[static_cast<std::size_t>(col - 1)])) col = prevCol(s, col);
    while (col > 0 && wordChar(s[static_cast<std::size_t>(col - 1)])) col = prevCol(s, col);
    return col;
}

int wordRight(const std::string& s, int col) {
    const int n = static_cast<int>(s.size());
    while (col < n && !wordChar(s[static_cast<std::size_t>(col)])) col = nextCol(s, col);
    while (col < n && wordChar(s[static_cast<std::size_t>(col)])) col = nextCol(s, col);
    return col;
}

// The line as drawn (tabs to the next multiple of 4) and where each byte lands in it.
struct Shown {
    std::string text;
    std::vector<int> at;  // byte offset in `text` for each byte of the line, plus one past the end
};

Shown shown(const std::string& line) {
    Shown d;
    d.at.resize(line.size() + 1);
    int column = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        d.at[i] = static_cast<int>(d.text.size());
        if (line[i] == '\t') {
            const int spaces = 4 - column % 4;
            d.text.append(static_cast<std::size_t>(spaces), ' ');
            column += spaces;
        } else {
            d.text += line[i];
            if (!continuation(line[i])) ++column;
        }
    }
    d.at[line.size()] = static_cast<int>(d.text.size());
    return d;
}

float xAt(const Shown& d, int col) {
    col = std::clamp(col, 0, static_cast<int>(d.at.size()) - 1);
    const int end = d.at[static_cast<std::size_t>(col)];
    return end <= 0 ? 0.0f : ImGui::CalcTextSize(d.text.data(), d.text.data() + end).x;
}

int colAt(const std::string& line, float x) {
    const Shown d = shown(line);
    int col = 0;
    float prev = 0;
    while (col < static_cast<int>(line.size())) {
        const int next = nextCol(line, col);
        const float nx = xAt(d, next);
        if (x < (prev + nx) * 0.5f) return col;
        prev = nx;
        col = next;
    }
    return static_cast<int>(line.size());
}

int columns(const std::string& line) { return static_cast<int>(shown(line).text.size()); }

TextPos clampPos(const CodeEditor& ed, TextPos p) {
    p.line = std::clamp(p.line, 0, static_cast<int>(ed.lines.size()) - 1);
    p.col = std::clamp(p.col, 0, static_cast<int>(ed.lines[static_cast<std::size_t>(p.line)].size()));
    return p;
}

std::pair<TextPos, TextPos> ordered(const CodeEditor& ed) {
    return ed.anchor < ed.cursor ? std::make_pair(ed.anchor, ed.cursor) : std::make_pair(ed.cursor, ed.anchor);
}

bool hasSelection(const CodeEditor& ed) { return ed.anchor != ed.cursor; }

void touched(CodeEditor& ed) {
    ed.dirty = true;
    ed.spansStale = true;
    ed.lastEdit = tp::seconds();
    ed.found.clear();
}

void eraseRange(CodeEditor& ed, TextPos a, TextPos b) {
    auto& L = ed.lines;
    if (a.line == b.line) {
        L[static_cast<std::size_t>(a.line)].erase(static_cast<std::size_t>(a.col), static_cast<std::size_t>(b.col - a.col));
    } else {
        L[static_cast<std::size_t>(a.line)] = L[static_cast<std::size_t>(a.line)].substr(0, static_cast<std::size_t>(a.col)) +
                                              L[static_cast<std::size_t>(b.line)].substr(static_cast<std::size_t>(b.col));
        L.erase(L.begin() + a.line + 1, L.begin() + b.line + 1);
    }
    ed.cursor = ed.anchor = a;
    touched(ed);
}

void deleteSelection(CodeEditor& ed) {
    if (!hasSelection(ed)) return;
    auto [a, b] = ordered(ed);
    eraseRange(ed, a, b);
}

void insertText(CodeEditor& ed, const std::string& raw) {
    deleteSelection(ed);
    std::string t;
    t.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') continue;
            t += '\n';
        } else {
            t += raw[i];
        }
    }
    auto& L = ed.lines;
    std::string& line = L[static_cast<std::size_t>(ed.cursor.line)];
    const std::string tail = line.substr(static_cast<std::size_t>(ed.cursor.col));
    line.erase(static_cast<std::size_t>(ed.cursor.col));
    std::size_t start = 0;
    int row = ed.cursor.line;
    while (true) {
        const std::size_t nl = t.find('\n', start);
        const std::string piece = t.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        L[static_cast<std::size_t>(row)] += piece;
        ed.widest = std::max(ed.widest, columns(L[static_cast<std::size_t>(row)]) + static_cast<int>(tail.size()));
        if (nl == std::string::npos) break;
        L.insert(L.begin() + row + 1, std::string());
        ++row;
        start = nl + 1;
    }
    ed.cursor = {row, static_cast<int>(L[static_cast<std::size_t>(row)].size())};
    L[static_cast<std::size_t>(row)] += tail;
    ed.anchor = ed.cursor;
    touched(ed);
}

std::string indentOf(const std::string& line) {
    std::size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
    return line.substr(0, n);
}

std::string indentUnit(const CodeEditor& ed) { return ed.tabs ? "\t" : "    "; }

void newline(CodeEditor& ed) {
    deleteSelection(ed);
    const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
    std::string indent = indentOf(line);
    if (indent.size() > static_cast<std::size_t>(ed.cursor.col)) indent.resize(static_cast<std::size_t>(ed.cursor.col));
    // one more level after an opening bracket or a Python-style colon
    std::string before = line.substr(0, static_cast<std::size_t>(ed.cursor.col));
    while (!before.empty() && (before.back() == ' ' || before.back() == '\t')) before.pop_back();
    const char last = before.empty() ? '\0' : before.back();
    const bool opens = last == '{' || last == '[' || last == '(' || (last == ':' && ed.language == "python");
    insertText(ed, "\n" + indent + (opens ? indentUnit(ed) : ""));
    const char next = static_cast<std::size_t>(ed.cursor.col) < ed.lines[static_cast<std::size_t>(ed.cursor.line)].size()
                          ? ed.lines[static_cast<std::size_t>(ed.cursor.line)][static_cast<std::size_t>(ed.cursor.col)]
                          : '\0';
    if (opens && ((last == '{' && next == '}') || (last == '[' && next == ']') || (last == '(' && next == ')'))) {
        const TextPos keep = ed.cursor;
        insertText(ed, "\n" + indent);
        ed.cursor = ed.anchor = keep;
    }
}

void indentLines(CodeEditor& ed, bool out) {
    auto [a, b] = ordered(ed);
    int last = b.line;
    if (b.col == 0 && b.line > a.line) --last;
    const std::string unit = indentUnit(ed);
    for (int i = a.line; i <= last; ++i) {
        std::string& line = ed.lines[static_cast<std::size_t>(i)];
        int delta = 0;
        if (!out) {
            if (line.empty()) continue;
            line.insert(0, unit);
            delta = static_cast<int>(unit.size());
        } else if (!line.empty() && line[0] == '\t') {
            line.erase(0, 1);
            delta = -1;
        } else {
            int n = 0;
            while (n < 4 && n < static_cast<int>(line.size()) && line[static_cast<std::size_t>(n)] == ' ') ++n;
            line.erase(0, static_cast<std::size_t>(n));
            delta = -n;
        }
        if (ed.anchor.line == i) ed.anchor.col = std::max(0, ed.anchor.col + delta);
        if (ed.cursor.line == i) ed.cursor.col = std::max(0, ed.cursor.col + delta);
    }
    touched(ed);
}

void toggleComment(CodeEditor& ed) {
    if (ed.comment.empty()) return;
    auto [a, b] = ordered(ed);
    int last = b.line;
    if (b.col == 0 && b.line > a.line) --last;
    const std::string mark = ed.comment + " ";
    bool all = true;
    for (int i = a.line; i <= last; ++i) {
        const std::string& line = ed.lines[static_cast<std::size_t>(i)];
        const std::string ind = indentOf(line);
        if (ind.size() == line.size()) continue;
        if (line.compare(ind.size(), ed.comment.size(), ed.comment) != 0) all = false;
    }
    for (int i = a.line; i <= last; ++i) {
        std::string& line = ed.lines[static_cast<std::size_t>(i)];
        const std::string ind = indentOf(line);
        if (ind.size() == line.size()) continue;
        if (all) {
            const std::size_t n = line.compare(ind.size(), mark.size(), mark) == 0 ? mark.size() : ed.comment.size();
            line.erase(ind.size(), n);
        } else {
            line.insert(ind.size(), mark);
        }
    }
    ed.cursor = clampPos(ed, ed.cursor);
    ed.anchor = clampPos(ed, ed.anchor);
    touched(ed);
}

ImU32 tokenColour(int kind, bool dark) {
    static const ImU32 darkColours[] = {
        IM_COL32(222, 224, 228, 255), IM_COL32(204, 132, 230, 255), IM_COL32(94, 196, 208, 255), IM_COL32(230, 168, 110, 255),
        IM_COL32(230, 168, 110, 255), IM_COL32(158, 206, 124, 255), IM_COL32(140, 146, 158, 255), IM_COL32(232, 196, 120, 255),
        IM_COL32(104, 178, 250, 255), IM_COL32(236, 118, 126, 255), IM_COL32(230, 168, 110, 255), IM_COL32(104, 178, 250, 255),
        IM_COL32(236, 118, 126, 255), IM_COL32(94, 196, 208, 255),
    };
    static const ImU32 lightColours[] = {
        IM_COL32(30, 30, 34, 255),  IM_COL32(150, 30, 170, 255), IM_COL32(0, 110, 130, 255),  IM_COL32(170, 85, 0, 255),
        IM_COL32(170, 85, 0, 255),  IM_COL32(40, 120, 20, 255),  IM_COL32(100, 106, 116, 255), IM_COL32(140, 100, 0, 255),
        IM_COL32(20, 90, 200, 255), IM_COL32(190, 30, 40, 255),  IM_COL32(170, 85, 0, 255),   IM_COL32(20, 90, 200, 255),
        IM_COL32(190, 30, 40, 255), IM_COL32(0, 110, 130, 255),
    };
    if (kind < 0 || kind > 13) kind = 0;
    return dark ? darkColours[kind] : lightColours[kind];
}

struct Layout {
    ImVec2 origin;  // screen position of line 0, column 0 of the text
    float lineH = 0, gutterW = 0;
    ImRect inner;   // visible text area (right of the gutter)
    int visibleLines = 1;
};

TextPos hit(const CodeEditor& ed, const Layout& lay, ImVec2 mouse) {
    TextPos p;
    p.line = std::clamp(static_cast<int>(std::floor((mouse.y - lay.origin.y) / lay.lineH)), 0, static_cast<int>(ed.lines.size()) - 1);
    p.col = colAt(ed.lines[static_cast<std::size_t>(p.line)], mouse.x - lay.origin.x - lay.gutterW);
    return p;
}

void selectWord(CodeEditor& ed) {
    const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
    int a = ed.cursor.col, b = ed.cursor.col;
    while (a > 0 && wordChar(line[static_cast<std::size_t>(a - 1)])) a = prevCol(line, a);
    while (b < static_cast<int>(line.size()) && wordChar(line[static_cast<std::size_t>(b)])) b = nextCol(line, b);
    ed.anchor = {ed.cursor.line, a};
    ed.cursor = {ed.cursor.line, b};
}

// Mouse: click, shift+click, drag and double-click. True when the cursor moved.
bool handleMouse(CodeEditor& ed, const Layout& lay, ImGuiID id, bool hovered) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetActiveID(id, window);
        ImGui::SetFocusID(id, window);
        ImGui::FocusWindow(window);
        ed.cursor = hit(ed, lay, io.MousePos);
        if (!io.KeyShift) ed.anchor = ed.cursor;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) selectWord(ed);
        else ed.dragging = true;
        ed.wantX = -1;
        return true;
    }
    if (g.ActiveId == id && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered) {
        ImGui::ClearActiveID();
        ed.dragging = false;
    }
    if (ed.dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ed.dragging = false;
            return false;
        }
        const TextPos p = hit(ed, lay, io.MousePos);
        if (io.MousePos.y < lay.inner.Min.y) ImGui::SetScrollY(ImGui::GetScrollY() - lay.lineH);
        if (io.MousePos.y > lay.inner.Max.y) ImGui::SetScrollY(ImGui::GetScrollY() + lay.lineH);
        if (p != ed.cursor) {
            ed.cursor = p;
            return true;
        }
    }
    return false;
}

void moveTo(CodeEditor& ed, TextPos p, bool select) {
    ed.cursor = clampPos(ed, p);
    if (!select) ed.anchor = ed.cursor;
}

// Cursor keys. True when the cursor moved.
bool handleMoves(CodeEditor& ed, const Layout& lay) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool mac = io.ConfigMacOSXBehaviors;
    const bool word = mac ? io.KeyAlt : io.KeyCtrl;
    const bool jump = mac ? io.KeySuper : io.KeyCtrl;  // to the start or end of the file
    const bool shift = io.KeyShift;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
    TextPos p = ed.cursor;
    bool moved = true, keepX = false;
    if (pressed(ImGuiKey_LeftArrow)) {
        if (hasSelection(ed) && !shift) p = ordered(ed).first;
        else if (p.col > 0) p.col = word ? wordLeft(line, p.col) : prevCol(line, p.col);
        else if (p.line > 0) p = {p.line - 1, static_cast<int>(ed.lines[static_cast<std::size_t>(p.line - 1)].size())};
    } else if (pressed(ImGuiKey_RightArrow)) {
        if (hasSelection(ed) && !shift) p = ordered(ed).second;
        else if (p.col < static_cast<int>(line.size())) p.col = word ? wordRight(line, p.col) : nextCol(line, p.col);
        else if (p.line + 1 < static_cast<int>(ed.lines.size())) p = {p.line + 1, 0};
    } else if (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_PageUp) || pressed(ImGuiKey_PageDown)) {
        int step = 1;
        if (ImGui::IsKeyDown(ImGuiKey_PageUp) || ImGui::IsKeyDown(ImGuiKey_PageDown)) step = std::max(1, lay.visibleLines - 2);
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_PageUp)) step = -step;
        if (ed.wantX < 0) ed.wantX = xAt(shown(line), p.col);
        p.line = std::clamp(p.line + step, 0, static_cast<int>(ed.lines.size()) - 1);
        p.col = colAt(ed.lines[static_cast<std::size_t>(p.line)], ed.wantX);
        keepX = true;
    } else if (pressed(ImGuiKey_Home)) {
        if (jump) {
            p = {0, 0};
        } else {
            const int first = static_cast<int>(indentOf(line).size());
            p.col = p.col == first ? 0 : first;  // smart home: the first word, then the very start
        }
    } else if (pressed(ImGuiKey_End)) {
        if (jump) p = {static_cast<int>(ed.lines.size()) - 1, static_cast<int>(ed.lines.back().size())};
        else p.col = static_cast<int>(line.size());
    } else {
        moved = false;
    }
    if (!moved) return false;
    if (!keepX) ed.wantX = -1;
    moveTo(ed, p, shift);
    return true;
}

void copySelection(const CodeEditor& ed) {
    if (hasSelection(ed)) ImGui::SetClipboardText(editorSelection(ed).c_str());
    else ImGui::SetClipboardText((ed.lines[static_cast<std::size_t>(ed.cursor.line)] + "\n").c_str());
}

// Shortcuts and editing keys. True when the text changed.
bool handleEdits(CodeEditor& ed) {
    ImGuiIO& io = ImGui::GetIO();
    const bool mac = io.ConfigMacOSXBehaviors;
    const bool cmd = mac ? io.KeySuper : io.KeyCtrl;
    const bool word = mac ? io.KeyAlt : io.KeyCtrl;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    const std::size_t before = ed.lines.size();
    const std::string beforeLine = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
    bool changed = false;
    if (cmd && pressed(ImGuiKey_A)) {
        ed.anchor = {0, 0};
        ed.cursor = {static_cast<int>(ed.lines.size()) - 1, static_cast<int>(ed.lines.back().size())};
        return false;
    }
    if (cmd && pressed(ImGuiKey_C)) {
        copySelection(ed);
        return false;
    }
    if (cmd && (pressed(ImGuiKey_Z) && !io.KeyShift)) {
        ed.wantUndo = true;
        return false;
    }
    if (cmd && (pressed(ImGuiKey_Y) || (io.KeyShift && pressed(ImGuiKey_Z)))) {
        ed.wantRedo = true;
        return false;
    }
    if (ed.readOnly) return false;
    if (cmd && pressed(ImGuiKey_X)) {
        copySelection(ed);
        if (hasSelection(ed)) {
            deleteSelection(ed);
        } else if (ed.lines.size() > 1) {
            ed.lines.erase(ed.lines.begin() + ed.cursor.line);
            moveTo(ed, {ed.cursor.line, 0}, false);
            touched(ed);
        } else {
            ed.lines[0].clear();
            moveTo(ed, {0, 0}, false);
            touched(ed);
        }
        return true;
    }
    if (cmd && pressed(ImGuiKey_V)) {
        if (const char* clip = ImGui::GetClipboardText(); clip && *clip) insertText(ed, clip);
        return true;
    }
    if (cmd && pressed(ImGuiKey_D)) {  // duplicate the line
        const std::string copy = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
        ed.lines.insert(ed.lines.begin() + ed.cursor.line + 1, copy);
        moveTo(ed, {ed.cursor.line + 1, ed.cursor.col}, false);
        touched(ed);
        return true;
    }
    if (cmd && pressed(ImGuiKey_Slash)) {
        toggleComment(ed);
        return true;
    }
    if (pressed(ImGuiKey_Tab)) {
        if (io.KeyShift || (hasSelection(ed) && ed.anchor.line != ed.cursor.line)) indentLines(ed, io.KeyShift);
        else insertText(ed, ed.tabs ? "\t" : std::string(static_cast<std::size_t>(4 - columns(ed.lines[static_cast<std::size_t>(ed.cursor.line)].substr(0, static_cast<std::size_t>(ed.cursor.col))) % 4), ' '));
        return true;
    }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
        newline(ed);
        return true;
    }
    if (pressed(ImGuiKey_Backspace)) {
        if (hasSelection(ed)) {
            deleteSelection(ed);
        } else if (ed.cursor.col > 0) {
            const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
            int from = word ? wordLeft(line, ed.cursor.col) : prevCol(line, ed.cursor.col);
            // inside the indent, take back a whole level of spaces
            if (!word && !ed.tabs && indentOf(line).size() >= static_cast<std::size_t>(ed.cursor.col) && ed.cursor.col >= 4 && ed.cursor.col % 4 == 0 &&
                line.compare(static_cast<std::size_t>(ed.cursor.col - 4), 4, "    ") == 0) {
                from = ed.cursor.col - 4;
            }
            eraseRange(ed, {ed.cursor.line, from}, ed.cursor);
        } else if (ed.cursor.line > 0) {
            const int prev = ed.cursor.line - 1;
            eraseRange(ed, {prev, static_cast<int>(ed.lines[static_cast<std::size_t>(prev)].size())}, ed.cursor);
        }
        return true;
    }
    if (pressed(ImGuiKey_Delete)) {
        if (hasSelection(ed)) {
            deleteSelection(ed);
        } else {
            const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
            if (ed.cursor.col < static_cast<int>(line.size())) {
                eraseRange(ed, ed.cursor, {ed.cursor.line, word ? wordRight(line, ed.cursor.col) : nextCol(line, ed.cursor.col)});
            } else if (ed.cursor.line + 1 < static_cast<int>(ed.lines.size())) {
                eraseRange(ed, ed.cursor, {ed.cursor.line + 1, 0});
            }
        }
        return true;
    }
    // typed characters
    std::string typed;
    if (!(cmd && !io.KeyAlt)) {
        for (ImWchar c : io.InputQueueCharacters) {
            if (c < 32 || c == 127) continue;
            const unsigned int u = c;
            if (u < 0x80) {
                typed += static_cast<char>(u);
            } else if (u < 0x800) {
                typed += static_cast<char>(0xC0 | (u >> 6));
                typed += static_cast<char>(0x80 | (u & 0x3F));
            } else {
                typed += static_cast<char>(0xE0 | (u >> 12));
                typed += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                typed += static_cast<char>(0x80 | (u & 0x3F));
            }
        }
    }
    io.InputQueueCharacters.resize(0);
    if (!typed.empty()) {
        // a closing bracket typed over the one already there just steps past it
        const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
        if (!hasSelection(ed) && typed.size() == 1 && (typed[0] == ')' || typed[0] == ']' || typed[0] == '}') &&
            static_cast<std::size_t>(ed.cursor.col) < line.size() && line[static_cast<std::size_t>(ed.cursor.col)] == typed[0]) {
            moveTo(ed, {ed.cursor.line, ed.cursor.col + 1}, false);
            return false;
        }
        insertText(ed, typed);
        // a closing bracket on an otherwise empty line goes back one level
        std::string& now = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
        if (typed == "}" && indentOf(now).size() + 1 == now.size() && now.size() > 1) {
            const std::string unit = indentUnit(ed);
            if (now.compare(0, unit.size(), unit) == 0) {
                now.erase(0, unit.size());
                ed.cursor.col -= static_cast<int>(unit.size());
                ed.anchor = ed.cursor;
            }
        }
        changed = true;
    }
    return changed || ed.lines.size() != before || ed.lines[static_cast<std::size_t>(std::min<std::size_t>(static_cast<std::size_t>(ed.cursor.line), ed.lines.size() - 1))] != beforeLine;
}

void keepCursorVisible(const CodeEditor& ed, const Layout& lay) {
    const float y = static_cast<float>(ed.cursor.line) * lay.lineH;
    const float top = ImGui::GetScrollY(), height = lay.inner.GetHeight();
    if (y < top) ImGui::SetScrollY(y);
    else if (y + lay.lineH > top + height) ImGui::SetScrollY(y + lay.lineH - height);
    const float x = xAt(shown(ed.lines[static_cast<std::size_t>(ed.cursor.line)]), ed.cursor.col);
    const float left = ImGui::GetScrollX(), width = lay.inner.GetWidth() - lay.lineH;
    if (x < left) ImGui::SetScrollX(std::max(0.0f, x - lay.lineH * 2));
    else if (x > left + width) ImGui::SetScrollX(x - width + lay.lineH * 2);
}

void drawLine(const CodeEditor& ed, const Layout& lay, int i, bool dark, ImDrawList* dl) {
    const std::string& line = ed.lines[static_cast<std::size_t>(i)];
    const Shown d = shown(line);
    const float y = lay.origin.y + static_cast<float>(i) * lay.lineH;
    const float x0 = lay.origin.x + lay.gutterW;
    const float pad = (lay.lineH - ImGui::GetFontSize()) * 0.5f;
    if (ed.active && i == ed.cursor.line && !hasSelection(ed)) {
        dl->AddRectFilled({lay.inner.Min.x, y}, {lay.inner.Max.x, y + lay.lineH}, dark ? IM_COL32(255, 255, 255, 14) : IM_COL32(0, 0, 0, 12));
    }
    for (const auto& [mline, error] : ed.marks) {
        if (mline == i) {
            dl->AddRectFilled({lay.inner.Min.x, y}, {lay.inner.Max.x, y + lay.lineH}, error ? IM_COL32(230, 60, 60, 40) : IM_COL32(230, 180, 40, 36));
        }
    }
    for (const TextPos& f : ed.found) {
        if (f.line != i) continue;
        const float fx = x0 + xAt(d, f.col), fx2 = x0 + xAt(d, std::min(f.col + ed.foundLen, static_cast<int>(line.size())));
        dl->AddRectFilled({fx, y}, {fx2, y + lay.lineH}, IM_COL32(250, 200, 40, 90));
    }
    if (hasSelection(ed)) {
        auto [a, b] = ordered(ed);
        if (i >= a.line && i <= b.line) {
            const float sx = x0 + (i == a.line ? xAt(d, a.col) : 0.0f);
            float ex = x0 + (i == b.line ? xAt(d, b.col) : xAt(d, static_cast<int>(line.size())) + ImGui::GetFontSize() * 0.4f);
            if (ex <= sx) ex = sx + 2;
            dl->AddRectFilled({sx, y}, {ex, y + lay.lineH}, ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
        }
    }
    // coloured runs
    int done = 0;
    const ImU32 plain = tokenColour(0, dark);
    auto run = [&](int from, int to, ImU32 colour) {
        if (to <= from) return;
        const int a = d.at[static_cast<std::size_t>(from)], b = d.at[static_cast<std::size_t>(to)];
        dl->AddText({x0 + xAt(d, from), y + pad}, colour, d.text.data() + a, d.text.data() + b);
    };
    const int n = static_cast<int>(line.size());
    if (static_cast<std::size_t>(i) < ed.spanStart.size()) {
        for (std::size_t s = static_cast<std::size_t>(ed.spanStart[static_cast<std::size_t>(i)]); s + 3 < ed.spans.size() && ed.spans[s] == i; s += 4) {
            const int col = std::clamp(ed.spans[s + 1], 0, n), len = ed.spans[s + 2];
            const int end = std::clamp(col + len, 0, n);
            if (col < done) continue;
            run(done, col, plain);
            run(col, end, tokenColour(ed.spans[s + 3], dark));
            done = end;
        }
    }
    run(done, n, plain);
    if (ed.active && i == ed.cursor.line) {
        const float cx = x0 + xAt(d, ed.cursor.col);
        dl->AddLine({cx, y + 1}, {cx, y + lay.lineH - 1}, ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
    }
}

void drawGutter(const CodeEditor& ed, const Layout& lay, int first, int last, ImDrawList* dl) {
    const float left = lay.inner.Min.x - lay.gutterW;
    dl->AddRectFilled({left, lay.inner.Min.y}, {lay.inner.Min.x, lay.inner.Max.y}, ImGui::GetColorU32(ImGuiCol_FrameBg));
    const float pad = (lay.lineH - ImGui::GetFontSize()) * 0.5f;
    for (int i = first; i < last; ++i) {
        const float y = lay.origin.y + static_cast<float>(i) * lay.lineH;
        const std::string num = std::to_string(i + 1);
        const float w = ImGui::CalcTextSize(num.c_str()).x;
        ImU32 colour = ImGui::GetColorU32(i == ed.cursor.line ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        for (const auto& [mline, error] : ed.marks) {
            if (mline == i) colour = error ? IM_COL32(240, 80, 80, 255) : IM_COL32(230, 170, 30, 255);
        }
        dl->AddText({lay.inner.Min.x - w - ImGui::GetFontSize() * 0.6f, y + pad}, colour, num.c_str());
    }
}

}  // namespace

std::string editorText(const CodeEditor& ed) {
    std::string out;
    std::size_t total = 0;
    for (const auto& l : ed.lines) total += l.size() + 1;
    out.reserve(total);
    for (std::size_t i = 0; i < ed.lines.size(); ++i) {
        if (i) out += '\n';
        out += ed.lines[i];
    }
    return out;
}

void setEditorText(CodeEditor& ed, const std::string& text) {
    ed.lines.clear();
    std::size_t start = 0;
    int tabbed = 0, spaced = 0;
    ed.widest = 0;
    while (true) {
        std::size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '\t') ++tabbed;
        if (line.rfind("  ", 0) == 0) ++spaced;
        ed.widest = std::max(ed.widest, static_cast<int>(line.size()));
        ed.lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    ed.tabs = tabbed > spaced;
    ed.cursor = clampPos(ed, ed.cursor);
    ed.anchor = clampPos(ed, ed.anchor);
    ed.dirty = false;
    ed.spansStale = true;
}

void editorSelect(CodeEditor& ed, TextPos from, TextPos to) {
    ed.anchor = clampPos(ed, from);
    ed.cursor = clampPos(ed, to);
    ed.scrollTo = ed.cursor.line;
}

std::string editorSelection(const CodeEditor& ed) {
    auto [a, b] = ordered(ed);
    if (a == b) return {};
    if (a.line == b.line) return ed.lines[static_cast<std::size_t>(a.line)].substr(static_cast<std::size_t>(a.col), static_cast<std::size_t>(b.col - a.col));
    std::string out = ed.lines[static_cast<std::size_t>(a.line)].substr(static_cast<std::size_t>(a.col));
    for (int i = a.line + 1; i < b.line; ++i) out += "\n" + ed.lines[static_cast<std::size_t>(i)];
    out += "\n" + ed.lines[static_cast<std::size_t>(b.line)].substr(0, static_cast<std::size_t>(b.col));
    return out;
}

bool drawEditor(CodeEditor& ed, ImFont* mono, bool dark, ImVec2 size) {
    if (ed.lines.empty()) ed.lines.emplace_back();
    ImGui::PushFont(mono, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::BeginChild("##editor", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID id = window->GetID("##text");

    Layout lay;
    lay.lineH = std::round(ImGui::GetFontSize() * 1.35f);
    const float charW = ImGui::CalcTextSize("0").x;
    const int digits = std::max(3, static_cast<int>(std::to_string(ed.lines.size()).size()));
    lay.gutterW = charW * static_cast<float>(digits + 2);
    lay.origin = ImGui::GetCursorScreenPos();
    lay.inner = window->InnerRect;
    lay.inner.Min.x += lay.gutterW;
    lay.visibleLines = std::max(1, static_cast<int>(lay.inner.GetHeight() / lay.lineH));
    // scroll room: every line, and the widest one
    ImGui::Dummy({lay.gutterW + charW * static_cast<float>(ed.widest + 8),
                  lay.lineH * static_cast<float>(ed.lines.size()) + lay.inner.GetHeight() * 0.5f});
    ImRect bb(window->InnerRect.Min, window->InnerRect.Max);
    ImGui::ItemAdd(bb, id);
    const bool hovered = ImGui::ItemHoverable(bb, id, ImGuiItemFlags_None);
    if (ed.focusNext) {
        ImGui::SetActiveID(id, window);
        ImGui::SetFocusID(id, window);
        ImGui::FocusWindow(window);
        ed.focusNext = false;
    }
    bool moved = handleMouse(ed, lay, id, hovered);
    ed.active = g.ActiveId == id;
    bool changed = false;
    if (ed.active) {
        for (ImGuiKey k : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_PageUp, ImGuiKey_PageDown,
                           ImGuiKey_Home, ImGuiKey_End, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Tab, ImGuiKey_Backspace,
                           ImGuiKey_Delete, ImGuiKey_Escape}) {
            ImGui::SetKeyOwner(k, id);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ed.anchor = ed.cursor;
        moved = handleMoves(ed, lay) || moved;
        changed = handleEdits(ed);
        moved = moved || changed;
    }
    if (moved && !ed.dragging) keepCursorVisible(ed, lay);
    if (ed.scrollTo >= 0) {
        ImGui::SetScrollY(std::max(0.0f, (static_cast<float>(ed.scrollTo) - static_cast<float>(lay.visibleLines) / 3) * lay.lineH));
        ImGui::SetScrollX(0);
        ed.scrollTo = -1;
    }

    // the visible lines
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int first = std::clamp(static_cast<int>((lay.inner.Min.y - lay.origin.y) / lay.lineH), 0, static_cast<int>(ed.lines.size()));
    const int last = std::min(static_cast<int>(ed.lines.size()), first + lay.visibleLines + 2);
    dl->PushClipRect(lay.inner.Min, lay.inner.Max, true);
    for (int i = first; i < last; ++i) drawLine(ed, lay, i, dark, dl);
    dl->PopClipRect();
    drawGutter(ed, lay, first, last, dl);

    if (ed.active && !ed.readOnly) {
        // ask the platform for typed text (SDL3 sends none otherwise) and place the IME box
        const float cy = lay.origin.y + static_cast<float>(ed.cursor.line) * lay.lineH;
        const float cx = lay.origin.x + lay.gutterW + xAt(shown(ed.lines[static_cast<std::size_t>(ed.cursor.line)]), ed.cursor.col);
        g.PlatformImeData.WantVisible = true;
        g.PlatformImeData.WantTextInput = true;
        g.PlatformImeData.InputPos = {cx, cy};
        g.PlatformImeData.InputLineHeight = lay.lineH;
        g.PlatformImeData.ViewportId = window->Viewport->ID;
    }
    ImGui::EndChild();
    ImGui::PopFont();
    return changed;
}

}  // namespace wetwaresuite::gui
