// Conditional formatting and data validation: office.sheet_cond_format,
// sheet_cond_formats, sheet_cond_clear, sheet_validate, sheet_validations,
// sheet_validation_clear.
#include <algorithm>

#include "sheet_cmd.hpp"
#include "text.hpp"

namespace wetwaresuite::office::sheetcmd {

namespace {

std::string ruleText(const CondFormat& cf) {
    const std::string& t = cf.type;
    std::string s;
    if (t == "cell") {
        s = "value " + cf.op + " " + cf.value1;
        if (cf.op == "between" || cf.op == "not_between") s = "value " + std::string(cf.op == "between" ? "between " : "not between ") + cf.value1 + " and " + cf.value2;
    } else if (t == "formula") {
        s = "formula =" + cf.value1;
    } else if (t == "text") {
        s = "text contains \"" + cf.value1 + "\"";
    } else if (t == "begins" || t == "ends") {
        s = "text " + std::string(t == "begins" ? "begins" : "ends") + " with \"" + cf.value1 + "\"";
    } else if (t == "top" || t == "bottom") {
        s = t + " " + (cf.value1.empty() ? "10" : cf.value1) + (cf.percent ? "%" : "");
    } else if (t == "color_scale") {
        s = "colour scale " + colorName(hasColor(cf.minColor) ? cf.minColor : (0xF8696B | kColorSet)) + (hasColor(cf.midColor) ? " - " + colorName(cf.midColor) : "") + " - " +
            colorName(hasColor(cf.maxColor) ? cf.maxColor : (0x63BE7B | kColorSet));
    } else if (t == "data_bar") {
        s = "data bars " + colorName(hasColor(cf.maxColor) ? cf.maxColor : (0x638EC6 | kColorSet));
    } else {
        s = t;
        std::replace(s.begin(), s.end(), '_', ' ');
    }
    if (t != "color_scale" && t != "data_bar") s += " -> " + describeStyle(cf.style);
    return s;
}

int countMatches(const Workbook& wb, int sheet, std::size_t rule, const Range& r) {
    CellPainter painter(wb, sheet);
    const Range clip = usedRange(wb, sheet);
    int n = 0;
    for (int row = r.r1; row <= std::min(r.r2, clip.r2); ++row) {
        for (int col = r.c1; col <= std::min(r.c2, clip.c2); ++col) n += painter.matches(rule, row, col) ? 1 : 0;
    }
    return n;
}

std::string validationText(const Validation& v) {
    std::string s = v.type;
    if (v.type == "list") s += " of " + v.value1;
    else if (v.type == "custom") s += " =" + v.value1;
    else if (v.op == "between" || v.op == "not_between") s += " " + std::string(v.op == "between" ? "between " : "not between ") + v.value1 + " and " + v.value2;
    else s += " " + v.op + " " + v.value1;
    if (!v.allowBlank) s += ", no blanks";
    if (v.errorStyle != "stop") s += " (" + v.errorStyle + " only)";
    if (!v.prompt.empty()) s += ", prompt \"" + v.prompt + "\"";
    if (!v.error.empty()) s += ", error \"" + v.error + "\"";
    return s;
}

// Every cell in the rule's range that breaks it (up to `limit`).
std::vector<std::string> invalidCells(const Workbook& wb, int sheet, const Validation& v, int limit, int& count) {
    std::vector<std::string> out;
    count = 0;
    const Range u = usedRange(wb, sheet);
    for (int row = v.range.r1; row <= std::min(v.range.r2, u.r2); ++row) {
        for (int col = v.range.c1; col <= std::min(v.range.c2, u.c2); ++col) {
            if (validationAt(wb, sheet, row, col) != &v) continue;
            std::string msg;
            bool reject;
            if (validateCell(wb, sheet, row, col, msg, reject)) continue;
            if (static_cast<int>(out.size()) < limit) {
                const Cell* c = wb.sheets[static_cast<std::size_t>(sheet)].get(row, col);
                out.push_back(cellName(row, col) + " (" + (c ? formatValue(c->value, c->format) : std::string()) + ")");
            }
            ++count;
        }
    }
    return out;
}

const std::vector<std::string>& ops() {
    static const std::vector<std::string> v = {">", "<", ">=", "<=", "=", "<>", "between", "not_between"};
    return v;
}

void registerCondFormats(Session& session) {
    auto& reg = session.registry();
    std::vector<ParamSpec> params = {doc(), str("range", "cells the rule covers, e.g. B2:B100", true),
                                     str("type", "kind of rule", false, std::string("cell"), condFormatTypes()),
                                     str("op", "type=cell: comparison", false, std::string(">"), ops()),
                                     str("value", "type=cell: the bound (number, date, text or formula like $B$1); type=formula: a formula that is "
                                                  "TRUE for cells to format, written for the range's top-left cell (e.g. $C2>$D2); text/begins/ends: the text; "
                                                  "top/bottom: how many (default 10)"),
                                     str("value2", "upper bound for between / not_between"), flag("percent", "top/bottom: value is a percentage", false)};
    for (auto& s : styleSpecs()) params.push_back(s);
    params.push_back(str("border", "draw a border around matching cells: thin, medium, thick or none"));
    params.push_back(str("min_color", "color_scale: colour of the lowest value (default red #f8696b)"));
    params.push_back(str("mid_color", "color_scale: colour of the middle value (optional, makes a 3-colour scale)"));
    params.push_back(str("max_color", "color_scale: colour of the highest value (default green #63be7b); data_bar: the bar colour"));
    params.push_back(sheetSpec());
    params.push_back(previewSpec());
    reg.add({"office.sheet_cond_format", "Conditional Format",
             "Highlight cells by rule, like Excel's conditional formatting: type=cell op=\">\" value=100 fill=lightgreen; "
             "type=text value=late color=red; type=top value=5; above_average, below_average, duplicate, unique, blank, "
             "not_blank, errors; type=formula value=\"$C2>$D2\" (relative refs move per cell); color_scale (2 or 3 colours) and "
             "data_bar shade by value. Later rules win where they overlap. Saved in .xlsx.",
             params, true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, false)) return Result::failure(error);
                CondFormat cf;
                cf.range = Range{0, r.r1, r.c1, r.r2, r.c2};
                cf.type = stringParam(p, "type", "cell");
                cf.op = stringParam(p, "op", ">");
                cf.value1 = stringParam(p, "value");
                cf.value2 = stringParam(p, "value2");
                cf.percent = boolParam(p, "percent", false);
                const std::string& t = cf.type;
                if ((t == "cell" || t == "formula" || t == "text" || t == "begins" || t == "ends") && cf.value1.empty()) return Result::failure("type=" + t + " needs value=");
                if (t == "cell" && (cf.op == "between" || cf.op == "not_between") && cf.value2.empty()) return Result::failure("op=" + cf.op + " needs value2=");
                if (t == "formula") {
                    if (!cf.value1.empty() && cf.value1[0] == '=') cf.value1 = trimCopy(cf.value1.substr(1));
                    const std::string fe = formulaError(cf.value1);
                    if (!fe.empty()) return Result::failure("the rule's formula doesn't parse: " + fe);
                }
                std::vector<std::string> done;
                if (!applyStyleParams(p, cf.style, done, error)) return Result::failure(error);
                if (has(p, "border")) {
                    const std::string b = stringParam(p, "border");
                    const int code = static_cast<int>(std::find(borderStyles().begin(), borderStyles().end(), b) - borderStyles().begin());
                    if (code >= static_cast<int>(borderStyles().size())) return Result::failure("bad border '" + b + "'");
                    for (auto& side : cf.style.border) side = static_cast<std::uint8_t>(code);
                }
                for (const auto& [key, field] : {std::pair<const char*, std::uint32_t*>{"min_color", &cf.minColor}, {"mid_color", &cf.midColor}, {"max_color", &cf.maxColor}}) {
                    if (has(p, key) && !parseColor(stringParam(p, key), *field)) return Result::failure(std::string("bad ") + key);
                }
                if (t != "color_scale" && t != "data_bar" && cf.style.plain()) {
                    cf.style.fill = 0xFFEB9C | kColorSet;  // nothing given: Excel's light yellow highlight
                    cf.style.color = 0x9C5700 | kColorSet;
                }
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                sh.condFormats.push_back(cf);
                const int n = t == "color_scale" || t == "data_bar" ? -1 : countMatches(d.data, r.sheet, sh.condFormats.size() - 1, r);
                std::string msg = "rule " + std::to_string(sh.condFormats.size()) + " on " + where(d.data, r) + ": " + ruleText(cf);
                if (n >= 0) msg += "; " + std::to_string(n) + " cell(s) match now";
                return withPreview(Result::success(msg, static_cast<std::int64_t>(sh.condFormats.size())).with("rule", static_cast<std::int64_t>(sh.condFormats.size())), d, p);
            }));

    reg.add({"office.sheet_cond_formats", "Conditional Formats", "List a sheet's conditional formatting rules in order (later ones win), with how many cells each highlights now.",
             {doc(), fileSpec("spreadsheet"), sheetSpec()}, false, true},
            read(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                const Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                if (sh.condFormats.empty()) return Result::success(sh.name + " has no conditional formats", std::int64_t{0}).with("count", std::int64_t{0});
                std::string t = sh.name + ": " + std::to_string(sh.condFormats.size()) + " rule(s)";
                for (std::size_t i = 0; i < sh.condFormats.size(); ++i) {
                    const auto& cf = sh.condFormats[i];
                    t += "\n  " + std::to_string(i + 1) + ". " + rangeName(cf.range) + ": " + ruleText(cf);
                    if (cf.type != "color_scale" && cf.type != "data_bar") t += " (" + std::to_string(countMatches(d.data, s, i, cf.range)) + " match)";
                }
                return Result::success(t, static_cast<std::int64_t>(sh.condFormats.size())).with("count", static_cast<std::int64_t>(sh.condFormats.size()));
            }));

    reg.add({"office.sheet_cond_clear", "Clear Conditional Formats",
             "Remove conditional formatting rules: rule=N (from office.sheet_cond_formats), every rule touching range=, or all of the sheet's.",
             {doc(), integer("rule", "rule number"), str("range", "remove the rules that overlap these cells"), sheetSpec(), previewSpec()}, true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                const std::size_t before = sh.condFormats.size();
                if (has(p, "rule")) {
                    const auto i = intParam(p, "rule");
                    if (i < 1 || i > static_cast<std::int64_t>(before)) return Result::failure("no rule " + std::to_string(i) + " (there are " + std::to_string(before) + ")");
                    sh.condFormats.erase(sh.condFormats.begin() + (i - 1));
                } else if (has(p, "range")) {
                    Range r;
                    if (!rangeArg(d.data, p, r, error, false)) return Result::failure(error);
                    const Range plain{0, r.r1, r.c1, r.r2, r.c2};
                    sh.condFormats.erase(std::remove_if(sh.condFormats.begin(), sh.condFormats.end(), [&](const CondFormat& cf) { return cf.range.overlaps(plain); }),
                                         sh.condFormats.end());
                } else {
                    sh.condFormats.clear();
                }
                const std::size_t n = before - sh.condFormats.size();
                if (!n) return Result::failure("no conditional formats removed");
                return withPreview(Result::success("removed " + std::to_string(n) + " rule(s); " + std::to_string(sh.condFormats.size()) + " left"), d, p);
            }));
}

void registerValidation(Session& session) {
    auto& reg = session.registry();
    reg.add({"office.sheet_validate", "Data Validation",
             "Limit what may be typed into cells: type=list (value=\"North,South\" or a range like $F$1:$F$9; shows a dropdown), "
             "whole or decimal numbers (op=between value=0 value2=100, or op=\">=\" value=0), date, time, length (of the text) "
             "or custom (value=a formula TRUE for good input, e.g. COUNTIF($A:$A,A2)=1). error_style=stop rejects bad input in "
             "office.sheet_set; warning/information accept it with a note. Saved in .xlsx.",
             {doc(), str("range", "cells the rule covers, e.g. B2:B100", true), str("type", "what is allowed", false, std::string("list"), validationTypes()),
              str("op", "comparison for numbers, dates, times and lengths", false, std::string("between"), ops()),
              str("value", "list items, a list range, the (lower) bound or the custom formula", true), str("value2", "upper bound for between"),
              flag("allow_blank", "empty cells are fine", true), flag("dropdown", "type=list: show the in-cell dropdown", true),
              str("error_style", "what happens to bad input", false, std::string("stop"), {"stop", "warning", "information"}),
              str("error_title", "title of the error message"), str("error", "error message (default: says what is allowed)"),
              str("prompt_title", "title of the input hint"), str("prompt", "input hint shown when the cell is selected"), sheetSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, false)) return Result::failure(error);
                Validation v;
                v.range = Range{0, r.r1, r.c1, r.r2, r.c2};
                v.type = stringParam(p, "type", "list");
                v.op = stringParam(p, "op", "between");
                v.value1 = trimCopy(stringParam(p, "value"));
                v.value2 = trimCopy(stringParam(p, "value2"));
                if (v.type == "custom" && !v.value1.empty() && v.value1[0] == '=') v.value1 = trimCopy(v.value1.substr(1));
                if (v.type == "list" && !v.value1.empty() && v.value1[0] == '=') v.value1 = trimCopy(v.value1.substr(1));
                if (v.type != "list" && v.type != "custom" && (v.op == "between" || v.op == "not_between") && v.value2.empty())
                    return Result::failure("op=" + v.op + " needs value2= (or use op=\">=\" with one bound)");
                if (v.type == "custom") {
                    const std::string fe = formulaError(v.value1);
                    if (!fe.empty()) return Result::failure("the custom formula doesn't parse: " + fe);
                }
                v.allowBlank = boolParam(p, "allow_blank", true);
                v.dropdown = boolParam(p, "dropdown", true);
                v.errorStyle = stringParam(p, "error_style", "stop");
                v.errorTitle = stringParam(p, "error_title");
                v.error = stringParam(p, "error");
                v.promptTitle = stringParam(p, "prompt_title");
                v.prompt = stringParam(p, "prompt");
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                // a new rule replaces older ones on the same cells, as in Excel
                const Range plain = v.range;
                for (auto it = sh.validations.begin(); it != sh.validations.end();) {
                    if (plain.contains(it->range.r1, it->range.c1) && plain.contains(it->range.r2, it->range.c2)) it = sh.validations.erase(it);
                    else ++it;
                }
                sh.validations.push_back(v);
                if (v.type == "list" && validationList(d.data, r.sheet, sh.validations.back()).empty()) return Result::failure("the list is empty");
                int bad = 0;
                auto cells = invalidCells(d.data, r.sheet, sh.validations.back(), 5, bad);
                std::string msg = "validation on " + where(d.data, r) + ": " + validationText(v);
                if (bad) {
                    std::string list;
                    for (const auto& c : cells) list += (list.empty() ? "" : ", ") + c;
                    msg += "; " + std::to_string(bad) + " existing cell(s) break it: " + list + (bad > 5 ? ", ..." : "");
                }
                return Result::success(msg).with("invalid", static_cast<std::int64_t>(bad));
            }));

    reg.add({"office.sheet_validations", "Validation Rules",
             "List a sheet's data validation rules and the cells that currently break them (like Excel's Circle Invalid Data).",
             {doc(), fileSpec("spreadsheet"), sheetSpec()}, false, true},
            read(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                const Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                if (sh.validations.empty()) return Result::success(sh.name + " has no validation rules", std::int64_t{0}).with("invalid", std::int64_t{0});
                std::string t = sh.name + ": " + std::to_string(sh.validations.size()) + " rule(s)";
                int total = 0;
                for (std::size_t i = 0; i < sh.validations.size(); ++i) {
                    const auto& v = sh.validations[i];
                    int bad = 0;
                    auto cells = invalidCells(d.data, s, v, 10, bad);
                    total += bad;
                    t += "\n  " + std::to_string(i + 1) + ". " + rangeName(v.range) + ": " + validationText(v);
                    if (bad) {
                        std::string list;
                        for (const auto& c : cells) list += (list.empty() ? "" : ", ") + c;
                        t += "\n     " + std::to_string(bad) + " invalid: " + list + (bad > 10 ? ", ..." : "");
                    }
                }
                return Result::success(t, static_cast<std::int64_t>(total)).with("invalid", static_cast<std::int64_t>(total));
            }));

    reg.add({"office.sheet_validation_clear", "Clear Validation", "Remove data validation from range= (rules overlapping it) or from the whole sheet.",
             {doc(), str("range", "cells to free (default: every rule on the sheet)"), sheetSpec()}, true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                const std::size_t before = sh.validations.size();
                if (has(p, "range")) {
                    Range r;
                    if (!rangeArg(d.data, p, r, error, false)) return Result::failure(error);
                    const Range plain{0, r.r1, r.c1, r.r2, r.c2};
                    sh.validations.erase(std::remove_if(sh.validations.begin(), sh.validations.end(), [&](const Validation& v) { return v.range.overlaps(plain); }),
                                         sh.validations.end());
                } else {
                    sh.validations.clear();
                }
                const std::size_t n = before - sh.validations.size();
                if (!n) return Result::failure("no validation rules removed");
                return Result::success("removed " + std::to_string(n) + " validation rule(s)");
            }));
}

}  // namespace

void registerSheetRuleCommands(Session& session) {
    registerCondFormats(session);
    registerValidation(session);
}

}  // namespace wetwaresuite::office::sheetcmd
