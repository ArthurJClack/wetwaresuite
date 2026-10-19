// office.sheet_style / sheet_merge (how cells look) and the sheet layout
// commands: frozen panes, hidden rows/columns, row heights, defined names.
#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

#include "sheet_cmd.hpp"
#include "text.hpp"
#include "wetwaresuite/pdf/fonts.hpp"

namespace wetwaresuite::office::sheetcmd {

std::vector<ParamSpec> styleSpecs() {
    return {flag("bold", "bold text"),
            flag("italic", "italic text"),
            flag("underline", "underlined text"),
            flag("strike", "struck-through text"),
            str("color", "font colour: #rrggbb, a name (red, darkgreen, navy, ...) or none"),
            str("fill", "background colour: #rrggbb, a name (lightyellow, lightgreen, ...) or none"),
            number("font_size", "font size in points (0 = the default 11)"),
            str("align", "horizontal alignment", false, {}, {"general", "left", "center", "right"}),
            str("valign", "vertical alignment", false, {}, {"bottom", "middle", "top"}),
            flag("wrap", "wrap text onto several lines inside the cell"),
            integer("indent", "indent level (0-15)")};
}

bool applyStyleParams(const Params& p, CellStyle& s, std::vector<std::string>& done, std::string& error) {
    auto onOff = [&](const char* key, bool& field) {
        if (!has(p, key)) return;
        field = boolParam(p, key);
        done.push_back(std::string(field ? "" : "not ") + key);
    };
    onOff("bold", s.bold);
    onOff("italic", s.italic);
    onOff("underline", s.underline);
    onOff("strike", s.strike);
    onOff("wrap", s.wrap);
    auto colour = [&](const char* key, std::uint32_t& field, const char* label) {
        if (!has(p, key)) return true;
        if (!parseColor(stringParam(p, key), field)) {
            error = "bad colour '" + stringParam(p, key) + "' (use #rrggbb, a name like red or lightyellow, or none)";
            return false;
        }
        done.push_back(std::string(label) + " " + colorName(field));
        return true;
    };
    if (!colour("color", s.color, "colour") || !colour("fill", s.fill, "fill")) return false;
    if (has(p, "font_size")) {
        const double sz = numberParam(p, "font_size");
        if (sz < 0 || sz > 409) {
            error = "font_size goes from 1 to 409 points";
            return false;
        }
        s.size = static_cast<float>(sz == 11 ? 0 : sz);
        done.push_back("size " + textNumber(sz == 0 ? 11 : sz) + "pt");
    }
    if (has(p, "align")) {
        const std::string a = stringParam(p, "align");
        s.align = a == "left" ? 'l' : a == "center" ? 'c' : a == "right" ? 'r' : 0;
        done.push_back("align " + a);
    }
    if (has(p, "valign")) {
        const std::string a = stringParam(p, "valign");
        s.valign = a == "top" ? 't' : a == "middle" ? 'm' : 0;
        done.push_back("valign " + a);
    }
    if (has(p, "indent")) {
        s.indent = static_cast<std::uint8_t>(std::clamp<std::int64_t>(intParam(p, "indent"), 0, 15));
        done.push_back("indent " + std::to_string(s.indent));
    }
    return true;
}

std::string describeStyle(const CellStyle& s) {
    std::vector<std::string> parts;
    if (s.bold) parts.push_back("bold");
    if (s.italic) parts.push_back("italic");
    if (s.underline) parts.push_back("underline");
    if (s.strike) parts.push_back("strike");
    if (s.size > 0) parts.push_back(textNumber(s.size) + "pt");
    if (hasColor(s.color)) parts.push_back("colour " + colorName(s.color));
    if (hasColor(s.fill)) parts.push_back("fill " + colorName(s.fill));
    if (s.align) parts.push_back(std::string("align ") + (s.align == 'l' ? "left" : s.align == 'c' ? "center" : "right"));
    if (s.valign) parts.push_back(std::string("valign ") + (s.valign == 't' ? "top" : "middle"));
    if (s.wrap) parts.push_back("wrap");
    if (s.indent) parts.push_back("indent " + std::to_string(s.indent));
    static const char* sides[] = {"left", "right", "top", "bottom"};
    std::string borders;
    for (int i = 0; i < 4; ++i) {
        if (s.border[i]) borders += std::string(borders.empty() ? "" : " ") + sides[i] + ":" + borderStyles()[s.border[i]];
    }
    if (!borders.empty()) parts.push_back("borders " + borders + (hasColor(s.borderColor) ? " " + colorName(s.borderColor) : ""));
    std::string out;
    for (const auto& x : parts) out += (out.empty() ? "" : ", ") + x;
    return out.empty() ? "plain" : out;
}

namespace {

// Clips a whole-row/column range to the used area so styling C:C touches real cells only.
Range styleArea(const Workbook& wb, const Range& r) {
    Range out = r;
    if (r.r2 - r.r1 >= kMaxRows - 1 || r.c2 - r.c1 >= kMaxCols - 1) {
        Range u = usedRange(wb, r.sheet);
        out.r2 = std::max(r.r1, std::min(r.r2, u.r2));
        out.c2 = std::max(r.c1, std::min(r.c2, u.c2));
    }
    return out;
}

// Rows "3", "3:7" or columns "C", "C:E", "3:5" (numbers) as a 0-based span.
bool spanArg(const std::string& text, bool rows, int& a, int& b, std::string& error) {
    std::string t = upper(trimCopy(text));
    auto colon = t.find(':');
    std::string x = t.substr(0, colon), y = colon == std::string::npos ? x : t.substr(colon + 1);
    auto one = [&](const std::string& s, int& out) {
        if (s.empty()) return false;
        if (std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            out = std::atoi(s.c_str()) - 1;
        } else if (!rows) {
            out = colIndex(s);
        } else {
            return false;
        }
        return out >= 0 && out < (rows ? kMaxRows : kMaxCols);
    };
    if (!one(trimCopy(x), a) || !one(trimCopy(y), b)) {
        error = std::string("bad ") + (rows ? "rows '" : "columns '") + text + "' (e.g. " + (rows ? "3 or 3:7)" : "C or C:E)");
        return false;
    }
    if (b < a) std::swap(a, b);
    if (b - a > 100000) {
        error = "that span is too large";
        return false;
    }
    return true;
}

std::string spanName(int a, int b, bool rows) {
    auto n = [&](int i) { return rows ? std::to_string(i + 1) : colName(i); };
    return a == b ? (rows ? "row " : "column ") + n(a) : (rows ? "rows " : "columns ") + n(a) + ":" + n(b);
}

void registerStyle(Session& session) {
    auto& reg = session.registry();
    std::vector<ParamSpec> params = {doc(), str("range", "cells to style, e.g. A1:E1 or B:B", true)};
    for (auto& s : styleSpecs()) params.push_back(s);
    params.push_back(str("border", "borders to draw: all, outline, inside, top, bottom, left, right, horizontal, vertical or none (comma-separated)"));
    params.push_back(str("border_style", "line style for border=", false, std::string("thin"), {"thin", "medium", "thick", "double", "dashed", "dotted"}));
    params.push_back(str("border_color", "border colour (default black)"));
    params.push_back(str("like", "copy the style and number format of this cell first (format painter), e.g. A1"));
    params.push_back(flag("clear", "reset the cells to the plain style before applying the rest", false));
    params.push_back(sheetSpec());
    params.push_back(previewSpec());
    reg.add({"office.sheet_style", "Style Cells",
             "Change how cells look: bold/italic/underline/strike, font colour and size, fill colour, horizontal and vertical "
             "alignment, wrap, indent and borders (all, outline, inside, single sides). like=A1 copies another cell's look "
             "(format painter); clear=true starts from plain. Number formats are office.sheet_format.",
             params, true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, false)) return Result::failure(error);
                const Range a = styleArea(d.data, r);
                if (a.rows() * static_cast<long long>(a.cols()) > 1000000) return Result::failure("that range is too large to style (over a million cells)");
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                std::vector<std::string> done;
                std::optional<Cell> like;
                if (has(p, "like")) {
                    Range lr;
                    if (!parseRange(d.data, stringParam(p, "like"), r.sheet, lr, error)) return Result::failure(error);
                    const Cell* src = d.data.sheets[static_cast<std::size_t>(lr.sheet)].get(lr.r1, lr.c1);
                    like = src ? *src : Cell{};
                    done.push_back("look of " + cellName(lr.r1, lr.c1));
                }
                if (boolParam(p, "clear", false)) done.push_back("cleared styles");
                // borders: which sides of which cells
                int code = 0;
                std::uint32_t borderColor = 0;
                std::vector<std::string> sides;
                if (has(p, "border")) {
                    const std::string bs = stringParam(p, "border_style", "thin");
                    code = static_cast<int>(std::find(borderStyles().begin(), borderStyles().end(), bs) - borderStyles().begin());
                    if (code <= 0 || code >= static_cast<int>(borderStyles().size())) return Result::failure("bad border_style '" + bs + "'");
                    if (has(p, "border_color") && !parseColor(stringParam(p, "border_color"), borderColor)) return Result::failure("bad border_color");
                    std::string item;
                    for (char ch : stringParam(p, "border") + ",") {
                        if (ch != ',') {
                            item += ch;
                            continue;
                        }
                        const std::string s = lower(trimCopy(item));
                        item.clear();
                        if (s.empty()) continue;
                        static const std::vector<std::string> ok = {"all", "outline", "inside", "top", "bottom", "left", "right", "horizontal", "vertical", "none"};
                        if (std::find(ok.begin(), ok.end(), s) == ok.end()) return Result::failure("bad border '" + s + "' (all, outline, inside, top, bottom, left, right, horizontal, vertical, none)");
                        sides.push_back(s);
                    }
                    done.push_back("borders " + stringParam(p, "border") + (sides.size() == 1 && sides[0] == "none" ? "" : " " + borderStyles()[static_cast<std::size_t>(code)]));
                }
                auto want = [&](const char* s) { return std::find(sides.begin(), sides.end(), s) != sides.end(); };
                CellStyle probe;
                std::vector<std::string> styleDone;
                if (!applyStyleParams(p, probe, styleDone, error)) return Result::failure(error);
                done.insert(done.end(), styleDone.begin(), styleDone.end());
                if (done.empty()) return Result::failure("nothing to change: give bold=, italic=, color=, fill=, align=, border=, like=, ...");
                int n = 0;
                for (int row = a.r1; row <= a.r2; ++row) {
                    for (int col = a.c1; col <= a.c2; ++col) {
                        Cell& c = sh.at(row, col);
                        if (like) {
                            c.style = like->style;
                            c.format = like->format;
                        }
                        if (boolParam(p, "clear", false)) c.style = CellStyle{};
                        std::vector<std::string> ignore;
                        applyStyleParams(p, c.style, ignore, error);
                        if (!sides.empty()) {
                            const bool top = row == a.r1, bottom = row == a.r2, left = col == a.c1, right = col == a.c2;
                            auto set = [&](int side, bool on) {
                                if (on) {
                                    c.style.border[side] = static_cast<std::uint8_t>(code);
                                    if (hasColor(borderColor)) c.style.borderColor = borderColor;
                                }
                            };
                            if (want("none")) {
                                for (auto& b : c.style.border) b = 0;
                                c.style.borderColor = 0;
                            }
                            const bool all = want("all");
                            set(BorderTop, all || ((want("outline") || want("top")) && top) || ((want("inside") || want("horizontal")) && !top));
                            set(BorderBottom, all || ((want("outline") || want("bottom")) && bottom) || ((want("inside") || want("horizontal")) && !bottom));
                            set(BorderLeft, all || ((want("outline") || want("left")) && left) || ((want("inside") || want("vertical")) && !left));
                            set(BorderRight, all || ((want("outline") || want("right")) && right) || ((want("inside") || want("vertical")) && !right));
                        }
                        if (c.blank()) sh.erase(row, col);
                        ++n;
                    }
                }
                std::string list;
                for (const auto& x : done) list += (list.empty() ? "" : ", ") + x;
                return withPreview(Result::success("styled " + where(d.data, a) + " (" + std::to_string(n) + " cells): " + list), d, p);
            }));

    reg.add({"office.sheet_merge", "Merge Cells",
             "Merge a range into one cell (the top-left value stays; other values in the range are cleared) or unmerge it. "
             "center=true (default) also centres the text, like Merge & Center; across=true merges each row on its own.",
             {doc(), str("range", "cells to merge, e.g. A1:E1", true), flag("unmerge", "split merged cells in the range back up", false),
              flag("center", "centre the merged text", true), flag("across", "merge each row of the range separately", false), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, false)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                Range plain{0, r.r1, r.c1, r.r2, r.c2};
                if (boolParam(p, "unmerge", false)) {
                    const std::size_t before = sh.merges.size();
                    sh.merges.erase(std::remove_if(sh.merges.begin(), sh.merges.end(), [&](const Range& m) { return m.overlaps(plain); }), sh.merges.end());
                    const std::size_t n = before - sh.merges.size();
                    if (!n) return Result::failure("no merged cells in " + where(d.data, r));
                    return withPreview(Result::success("unmerged " + std::to_string(n) + " area(s) in " + where(d.data, r)), d, p);
                }
                if (r.rows() * static_cast<long long>(r.cols()) > 100000) return Result::failure("that range is too large to merge");
                std::vector<Range> areas;
                if (boolParam(p, "across", false)) {
                    for (int row = r.r1; row <= r.r2; ++row) areas.push_back(Range{0, row, r.c1, row, r.c2});
                } else {
                    areas.push_back(plain);
                }
                int cleared = 0;
                for (const Range& m : areas) {
                    if (m.rows() == 1 && m.cols() == 1) return Result::failure("merging needs at least two cells");
                    sh.merges.erase(std::remove_if(sh.merges.begin(), sh.merges.end(), [&](const Range& o) { return o.overlaps(m); }), sh.merges.end());
                    for (int row = m.r1; row <= m.r2; ++row) {
                        for (int col = m.c1; col <= m.c2; ++col) {
                            if (row == m.r1 && col == m.c1) continue;
                            Cell* c = sh.cells.count(Sheet::key(row, col)) ? &sh.at(row, col) : nullptr;
                            if (!c) continue;
                            if (c->hasContent()) ++cleared;
                            c->value = {};
                            c->formula.clear();
                            c->ast.reset();
                            if (c->blank()) sh.erase(row, col);
                        }
                    }
                    sh.merges.push_back(m);
                    if (boolParam(p, "center", true)) {
                        Cell& tl = sh.at(m.r1, m.c1);
                        tl.style.align = 'c';
                        if (m.rows() > 1) tl.style.valign = 'm';
                    }
                }
                std::string note = recalcNote(d.data);
                std::string msg = "merged " + where(d.data, r) + (areas.size() > 1 ? " row by row (" + std::to_string(areas.size()) + " areas)" : "");
                if (cleared) msg += "; cleared " + std::to_string(cleared) + " value(s) outside the top-left cell";
                return withPreview(Result::success(msg + note), d, p);
            }));
}

void registerLayout(Session& session) {
    auto& reg = session.registry();
    reg.add({"office.sheet_freeze", "Freeze Panes",
             "Keep rows at the top and/or columns at the left in view while scrolling. at=B2 freezes everything above and left "
             "of B2 (row 1 and column A); rows=1 freezes the header row; at=A1 or none=true unfreezes.",
             {doc(), str("at", "first cell that scrolls, e.g. B2 (A2 = freeze row 1)"), integer("rows", "rows to freeze at the top"),
              integer("cols", "columns to freeze at the left"), flag("none", "unfreeze", false), sheetSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                if (boolParam(p, "none", false)) {
                    sh.freezeRows = sh.freezeCols = 0;
                    return Result::success("unfroze " + sh.name);
                }
                if (has(p, "at")) {
                    Range at;
                    if (!parseRange(d.data, stringParam(p, "at"), s, at, error)) return Result::failure(error);
                    sh.freezeRows = at.r1;
                    sh.freezeCols = at.c1;
                } else if (has(p, "rows") || has(p, "cols")) {
                    if (has(p, "rows")) sh.freezeRows = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "rows"), 0, 1000));
                    if (has(p, "cols")) sh.freezeCols = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "cols"), 0, 1000));
                } else {
                    return Result::failure("give at=B2, rows=, cols= or none=true");
                }
                if (!sh.freezeRows && !sh.freezeCols) return Result::success("unfroze " + sh.name);
                std::string what;
                if (sh.freezeRows) what = sh.freezeRows == 1 ? "row 1" : "rows 1-" + std::to_string(sh.freezeRows);
                if (sh.freezeCols) what += (what.empty() ? "" : " and ") + std::string(sh.freezeCols == 1 ? "column A" : "columns A-" + colName(sh.freezeCols - 1));
                return Result::success("froze " + what + " of " + sh.name).with("rows", static_cast<std::int64_t>(sh.freezeRows)).with("cols", static_cast<std::int64_t>(sh.freezeCols));
            }));

    reg.add({"office.sheet_hide", "Hide Rows or Columns",
             "Hide (or with hidden=false show again) whole rows or columns. Hidden rows are skipped by SUBTOTAL/AGGREGATE "
             "and in printouts.",
             {doc(), str("rows", "rows, e.g. 4 or 4:9"), str("cols", "columns, e.g. C or C:E"), flag("hidden", "hide (false shows them)", true), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                if (!has(p, "rows") && !has(p, "cols")) return Result::failure("give rows=4:9 or cols=C:E");
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                const bool hide = boolParam(p, "hidden", true);
                std::vector<std::string> done;
                for (const bool rows : {true, false}) {
                    const char* key = rows ? "rows" : "cols";
                    if (!has(p, key)) continue;
                    int a, b;
                    if (!spanArg(stringParam(p, key), rows, a, b, error)) return Result::failure(error);
                    std::set<int>& set = rows ? sh.hiddenRows : sh.hiddenCols;
                    for (int i = a; i <= b; ++i) {
                        if (hide) set.insert(i);
                        else set.erase(i);
                    }
                    done.push_back(spanName(a, b, rows));
                }
                std::string note = recalcNote(d.data);
                std::string list;
                for (const auto& x : done) list += (list.empty() ? "" : " and ") + x;
                return withPreview(Result::success(std::string(hide ? "hid " : "showed ") + list + note), d, p);
            }));

    reg.add({"office.sheet_row_height", "Row Height",
             "Set the height of rows in points (the default is 15), or autofit=true to fit wrapped and multi-line text.",
             {doc(), str("rows", "rows, e.g. 1 or 2:20", true), number("height", "height in points"), flag("autofit", "fit the contents", false), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                int a, b;
                if (!spanArg(stringParam(p, "rows"), true, a, b, error)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                if (boolParam(p, "autofit", false)) {
                    for (int r = a; r <= b; ++r) {
                        double h = 15;
                        auto it = sh.cells.lower_bound(Sheet::key(r, 0));
                        for (; it != sh.cells.end() && Sheet::rowOf(it->first) == r; ++it) {
                            const Cell& c = it->second;
                            const double size = c.style.size > 0 ? c.style.size : 11;
                            const std::string text = formatValue(c.value, c.format);
                            double lines = 1 + static_cast<double>(std::count(text.begin(), text.end(), '\n'));
                            if (c.style.wrap) {
                                auto w = sh.colWidths.find(Sheet::colOf(it->first));
                                const double room = ((w == sh.colWidths.end() ? 8.43 : w->second) * 7 + 5) * 0.75 - 6;
                                const pdf::Font f{pdf::Family::Sans, c.style.bold, c.style.italic};
                                lines = std::max(lines, std::ceil(pdf::textWidth(f, text, size) / std::max(room, 10.0)));
                            }
                            h = std::max(h, lines * size * 1.25 + 3);
                        }
                        if (std::fabs(h - 15) < 0.01) sh.rowHeights.erase(r);
                        else sh.rowHeights[r] = std::round(h * 4) / 4;
                    }
                    return withPreview(Result::success("autofit the height of " + spanName(a, b, true)), d, p);
                }
                if (!has(p, "height")) return Result::failure("give height= in points or autofit=true");
                const double h = std::clamp(numberParam(p, "height"), 2.0, 409.0);
                for (int r = a; r <= b; ++r) {
                    if (std::fabs(h - 15) < 0.01) sh.rowHeights.erase(r);
                    else sh.rowHeights[r] = h;
                }
                return withPreview(Result::success("set the height of " + spanName(a, b, true) + " to " + textNumber(h) + "pt"), d, p);
            }));
}

void registerNames(Session& session) {
    auto& reg = session.registry();
    reg.add({"office.sheet_names", "Named Ranges", "List the workbook's defined names (named ranges and constants) with what they refer to and their current value.",
             {doc(), fileSpec("spreadsheet")}, false, true},
            read(session, [](SheetDoc& d, const Params&, const std::string&) -> Result {
                if (d.data.names.empty()) return Result::success("no defined names (office.sheet_name adds one)", std::int64_t{0}).with("count", std::int64_t{0});
                std::string t = std::to_string(d.data.names.size()) + " name(s):";
                for (const auto& [key, n] : d.data.names) {
                    auto v = d.data.evaluateArray(n.formula, 0);
                    std::string shown = v.size() == 1 && v[0].size() == 1 ? formatValue(v[0][0], "") : std::to_string(v.size()) + "x" + std::to_string(v.empty() ? 0 : v[0].size()) + " values";
                    t += "\n  " + n.name + " = " + n.formula + "  -> " + shown + (n.comment.empty() ? "" : "  (" + n.comment + ")");
                }
                return Result::success(t, static_cast<std::int64_t>(d.data.names.size())).with("count", static_cast<std::int64_t>(d.data.names.size()));
            }));

    reg.add({"office.sheet_name", "Define Name",
             "Give a range or value a name to use in formulas: name=TaxRate refers_to=B1 makes =A2*TaxRate work; refers_to may be "
             "a range (Sales!C2:C100), a constant (0.2) or any formula. Plain cell references are made absolute on the given "
             "sheet. delete=true removes the name (formulas using it then give #NAME?).",
             {doc(), str("name", "the name, e.g. TaxRate (letters, digits, _ and .; not a cell reference)", true),
              str("refers_to", "range, constant or formula"), str("comment", "a note about the name"), flag("delete", "remove the name", false), sheetSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                const std::string name = trimCopy(stringParam(p, "name"));
                const std::string key = upper(name);
                if (boolParam(p, "delete", false)) {
                    auto it = d.data.names.find(key);
                    if (it == d.data.names.end()) return Result::failure("no name '" + name + "'");
                    d.data.names.erase(it);
                    std::string note = recalcNote(d.data);
                    return Result::success("deleted the name " + name + note);
                }
                if (!validName(name)) return Result::failure("'" + name + "' can't be a name: use letters, digits, _ and ., start with a letter, and don't look like a cell (A1, R2C3)");
                if (!has(p, "refers_to")) return Result::failure("give refers_to= (a range like Sheet1!B2:B9, a constant or a formula)");
                std::string f = trimCopy(stringParam(p, "refers_to"));
                if (!f.empty() && f[0] == '=') f = trimCopy(f.substr(1));
                int sheet;
                std::string error;
                if (!sheetArg(d.data, p, sheet, error)) return Result::failure(error);
                Range r;
                const bool isRange = f.find_first_of("(+*/&^<>,\"") == std::string::npos && parseRange(d.data, f, sheet, r, error) && !d.data.name(f);
                if (isRange) {
                    // pin plain references to their sheet, absolute, the way Excel's Name Manager writes them
                    f = quoteSheet(d.data.sheets[static_cast<std::size_t>(r.sheet)].name) + "!$" + colName(r.c1) + "$" + std::to_string(r.r1 + 1);
                    if (r.r2 != r.r1 || r.c2 != r.c1) f += ":$" + colName(r.c2) + "$" + std::to_string(r.r2 + 1);
                } else {
                    const std::string fe = formulaError(f);
                    if (!fe.empty()) return Result::failure("refers_to doesn't parse: " + fe);
                }
                const bool existed = d.data.names.count(key) > 0;
                DefinedName n;
                n.name = name;
                n.formula = f;
                n.comment = stringParam(p, "comment");
                d.data.names[key] = n;
                std::string note = recalcNote(d.data);
                auto v = d.data.evaluateArray(f, sheet);
                std::string shown = v.size() == 1 && v[0].size() == 1 ? formatValue(v[0][0], "") : std::to_string(v.size()) + "x" + std::to_string(v.empty() ? 0 : v[0].size()) + " cells";
                return Result::success(std::string(existed ? "changed " : "defined ") + name + " = " + f + " (" + shown + ")" + note, f).with("name", name).with("refers_to", f);
            }));
}

}  // namespace

void registerSheetStyleCommands(Session& session) { registerStyle(session); }

void registerSheetLayoutCommands(Session& session) {
    registerLayout(session);
    registerNames(session);
}

}  // namespace wetwaresuite::office::sheetcmd
