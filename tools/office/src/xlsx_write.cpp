// Writing .xlsx: cells and formulas (dynamic arrays as spilled array formulas),
// styles, column widths, row heights, hidden rows/columns, merges, frozen panes,
// auto-filter, conditional formats, data validation, defined names and charts.
#include <algorithm>
#include <cmath>
#include <set>

#include "wetwaresuite/raster/ora.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"
#include "xlsx_internal.hpp"

namespace wetwaresuite::office {

namespace {

using namespace xlsx;

// Interns parts of styles.xml by their XML text, so equal pieces share one id.
struct Pool {
    std::vector<std::string> items;
    std::map<std::string, int> index;
    int add(const std::string& x) {
        auto it = index.find(x);
        if (it != index.end()) return it->second;
        const int id = static_cast<int>(items.size());
        items.push_back(x);
        index[x] = id;
        return id;
    }
};

std::string colorXml(const char* tag, std::uint32_t c) { return std::string("<") + tag + " rgb=\"" + argb(c) + "\"/>"; }

std::string fontXml(const CellStyle& s, bool full) {
    std::string x = "<font>";
    if (s.bold) x += "<b/>";
    if (s.italic) x += "<i/>";
    if (s.strike) x += "<strike/>";
    if (s.underline) x += "<u/>";
    if (full || s.size > 0) x += "<sz val=\"" + textNumber(s.size > 0 ? s.size : 11) + "\"/>";
    if (hasColor(s.color)) x += colorXml("color", s.color);
    if (full) x += "<name val=\"Calibri\"/><family val=\"2\"/><scheme val=\"minor\"/>";
    return x + "</font>";
}

std::string borderXml(const CellStyle& s) {
    std::string x = "<border>";
    static const char* sides[] = {"left", "right", "top", "bottom"};
    for (int i = 0; i < 4; ++i) {
        const int code = s.border[i];
        if (!code) {
            x += std::string("<") + sides[i] + "/>";
            continue;
        }
        x += std::string("<") + sides[i] + " style=\"" + borderName(code) + "\">";
        x += hasColor(s.borderColor) ? colorXml("color", s.borderColor) : std::string("<color indexed=\"64\"/>");
        x += std::string("</") + sides[i] + ">";
    }
    return x + "<diagonal/></border>";
}

struct Styles {
    Pool fonts, fills, borders, xfs, dxfs;
    std::map<std::string, int> customFormats;
    std::map<std::string, int> builtinIds;

    Styles() {
        fonts.add(fontXml(CellStyle{}, true));
        fills.add("<fill><patternFill patternType=\"none\"/></fill>");
        fills.add("<fill><patternFill patternType=\"gray125\"/></fill>");
        borders.add(borderXml(CellStyle{}));
        xfs.add("<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>");
        for (const auto& [id, code] : builtinFormats()) {
            if (id != 14 && id != 22 && !code.empty() && !builtinIds.count(code)) builtinIds[code] = id;
        }
    }

    int formatId(const std::string& code) {
        if (code.empty()) return 0;
        auto b = builtinIds.find(code);
        if (b != builtinIds.end()) return b->second;
        auto c = customFormats.find(code);
        if (c != customFormats.end()) return c->second;
        const int id = 164 + static_cast<int>(customFormats.size());
        customFormats[code] = id;
        return id;
    }

    int xfOf(const Cell& c) {
        const CellStyle& s = c.style;
        if (c.format.empty() && s.plain()) return 0;
        const int num = formatId(c.format);
        CellStyle fontPart;
        fontPart.bold = s.bold, fontPart.italic = s.italic, fontPart.underline = s.underline, fontPart.strike = s.strike;
        fontPart.size = s.size, fontPart.color = s.color;
        const int font = fonts.add(fontXml(fontPart, true));
        const int fill = hasColor(s.fill) ? fills.add("<fill><patternFill patternType=\"solid\">" + colorXml("fgColor", s.fill) + "<bgColor indexed=\"64\"/></patternFill></fill>") : 0;
        const int border = borders.add(borderXml(s));
        std::string x = "<xf numFmtId=\"" + std::to_string(num) + "\" fontId=\"" + std::to_string(font) + "\" fillId=\"" + std::to_string(fill) +
                        "\" borderId=\"" + std::to_string(border) + "\" xfId=\"0\"";
        if (num) x += " applyNumberFormat=\"1\"";
        if (font) x += " applyFont=\"1\"";
        if (fill) x += " applyFill=\"1\"";
        if (border) x += " applyBorder=\"1\"";
        std::string al;
        char h = s.align;
        if (!h && s.indent) h = 'l';
        if (h) al += std::string(" horizontal=\"") + (h == 'l' ? "left" : h == 'c' ? "center" : "right") + "\"";
        if (s.valign) al += std::string(" vertical=\"") + (s.valign == 't' ? "top" : "center") + "\"";
        if (s.wrap) al += " wrapText=\"1\"";
        if (s.indent) al += " indent=\"" + std::to_string(s.indent) + "\"";
        if (al.empty()) x += "/>";
        else x += " applyAlignment=\"1\"><alignment" + al + "/></xf>";
        return xfs.add(x);
    }

    int dxfOf(const CellStyle& s) {
        std::string x = "<dxf>";
        if (s.bold || s.italic || s.strike || s.underline || hasColor(s.color)) {
            CellStyle f;
            f.bold = s.bold, f.italic = s.italic, f.strike = s.strike, f.underline = s.underline, f.color = s.color;
            x += fontXml(f, false);
        }
        if (hasColor(s.fill)) x += "<fill><patternFill patternType=\"solid\">" + colorXml("bgColor", s.fill) + "</patternFill></fill>";
        if (s.border[0] || s.border[1] || s.border[2] || s.border[3]) x += borderXml(s);
        return dxfs.add(x + "</dxf>");
    }

    std::string xmlText() {
        std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<styleSheet xmlns=\"" + std::string(kMainNs) + "\">";
        if (!customFormats.empty()) {
            std::vector<std::pair<int, std::string>> ordered;
            for (const auto& [code, id] : customFormats) ordered.emplace_back(id, code);
            std::sort(ordered.begin(), ordered.end());
            x += "<numFmts count=\"" + std::to_string(ordered.size()) + "\">";
            for (const auto& [id, code] : ordered) x += "<numFmt numFmtId=\"" + std::to_string(id) + "\" formatCode=\"" + xml::escape(code) + "\"/>";
            x += "</numFmts>";
        }
        auto list = [&](const char* tag, const Pool& p) {
            x += std::string("<") + tag + " count=\"" + std::to_string(p.items.size()) + "\">";
            for (const auto& i : p.items) x += i;
            x += std::string("</") + tag + ">";
        };
        list("fonts", fonts);
        list("fills", fills);
        list("borders", borders);
        x += "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>";
        list("cellXfs", xfs);
        x += "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>";
        list("dxfs", dxfs);
        return x + "</styleSheet>";
    }
};

struct Strings {
    std::vector<std::string> list;
    std::map<std::string, int> index;
    int add(const std::string& s) {
        auto it = index.find(s);
        if (it != index.end()) return it->second;
        const int id = static_cast<int>(list.size());
        list.push_back(s);
        index[s] = id;
        return id;
    }
};

std::string errorText(const CellValue& v) { return v.text == "#CYCLE!" ? "#N/A" : v.text; }

void writeViewsAndCols(xml::Writer& w, const Sheet& sh, bool active) {
    w.open("sheetViews").open("sheetView").attr("workbookViewId", "0");
    if (active) w.attr("tabSelected", "1");
    if (sh.freezeRows > 0 || sh.freezeCols > 0) {
        const char* pane = sh.freezeRows > 0 && sh.freezeCols > 0 ? "bottomRight" : sh.freezeRows > 0 ? "bottomLeft" : "topRight";
        w.open("pane");
        if (sh.freezeCols > 0) w.attr("xSplit", std::to_string(sh.freezeCols));
        if (sh.freezeRows > 0) w.attr("ySplit", std::to_string(sh.freezeRows));
        w.attr("topLeftCell", cellName(sh.freezeRows, sh.freezeCols)).attr("activePane", pane).attr("state", "frozen").close();
        w.leaf("selection", {{"pane", pane}});
    }
    w.close().close();
    w.leaf("sheetFormatPr", {{"defaultRowHeight", "15"}});
    std::set<int> cols;
    for (const auto& [c, width] : sh.colWidths) cols.insert(c);
    for (int c : sh.hiddenCols) cols.insert(c);
    if (cols.empty()) return;
    w.open("cols");
    for (int c : cols) {
        auto it = sh.colWidths.find(c);
        w.open("col").attr("min", std::to_string(c + 1)).attr("max", std::to_string(c + 1));
        w.attr("width", textNumber(it == sh.colWidths.end() ? 8.43 : it->second));
        if (sh.hiddenCols.count(c)) w.attr("hidden", "1");
        if (it != sh.colWidths.end()) w.attr("customWidth", "1");
        w.close();
    }
    w.close();
}

void writeCell(xml::Writer& w, int row, int col, const Cell& c, Styles& styles, Strings& strings, bool& dynamicArrays) {
    w.open("c").attr("r", cellName(row, col));
    if (int s = styles.xfOf(c)) w.attr("s", std::to_string(s));
    const CellValue& v = c.value;
    if (c.hasFormula() && !c.spilled) {
        const bool array = c.spillRows > 0 && c.spillCols > 0 && !v.isError();
        if (array) {
            w.attr("cm", "1");
            dynamicArrays = true;
        }
        if (v.type == CellValue::Text) w.attr("t", "str");
        else if (v.type == CellValue::Bool) w.attr("t", "b");
        else if (v.type == CellValue::Error) w.attr("t", "e");
        w.open("f");
        if (array) w.attr("t", "array").attr("ref", rangeName(Range{0, row, col, row + c.spillRows - 1, col + c.spillCols - 1}));
        w.text(fileFormula(c.formula, true)).close();
        if (v.type == CellValue::Text) w.element("v", v.text);
        else if (v.type == CellValue::Bool) w.element("v", v.number != 0 ? "1" : "0");
        else if (v.type == CellValue::Error) w.element("v", errorText(v));
        else if (v.type == CellValue::Number) w.element("v", textNumber(v.number));
    } else {
        switch (v.type) {
            case CellValue::Text: w.attr("t", "s").element("v", std::to_string(strings.add(v.text))); break;
            case CellValue::Number: w.element("v", textNumber(v.number)); break;
            case CellValue::Bool: w.attr("t", "b").element("v", v.number != 0 ? "1" : "0"); break;
            case CellValue::Error: w.attr("t", "e").element("v", errorText(v)); break;
            case CellValue::Empty: break;
        }
    }
    w.close();
}

void writeSheetData(xml::Writer& w, const Sheet& sh, Styles& styles, Strings& strings, bool& dynamicArrays) {
    std::set<int> rows;
    for (const auto& [r, h] : sh.rowHeights) rows.insert(r);
    for (int r : sh.hiddenRows) rows.insert(r);
    w.open("sheetData");
    auto it = sh.cells.begin();
    auto extra = rows.begin();
    while (it != sh.cells.end() || extra != rows.end()) {
        const int cellRow = it != sh.cells.end() ? Sheet::rowOf(it->first) : kMaxRows;
        const int row = std::min(cellRow, extra != rows.end() ? *extra : kMaxRows);
        if (extra != rows.end() && *extra == row) ++extra;
        w.open("row").attr("r", std::to_string(row + 1));
        if (auto h = sh.rowHeights.find(row); h != sh.rowHeights.end()) w.attr("ht", textNumber(h->second)).attr("customHeight", "1");
        if (sh.hiddenRows.count(row)) w.attr("hidden", "1");
        for (; it != sh.cells.end() && Sheet::rowOf(it->first) == row; ++it) {
            if (it->second.blank()) continue;
            writeCell(w, row, Sheet::colOf(it->first), it->second, styles, strings, dynamicArrays);
        }
        w.close();
    }
    w.close();
}

std::string sqref(const Range& r) { return rangeName(Range{0, r.r1, r.c1, r.r2, r.c2}); }

void writeCondFormats(xml::Writer& w, const Sheet& sh, Styles& styles) {
    int priority = static_cast<int>(sh.condFormats.size());
    for (const auto& cf : sh.condFormats) {
        const std::string tl = cellName(cf.range.r1, cf.range.c1);
        auto quoted = [](const std::string& s) {
            std::string q = "\"";
            for (char ch : s) q += ch == '"' ? std::string("\"\"") : std::string(1, ch);
            return q + "\"";
        };
        w.open("conditionalFormatting").attr("sqref", sqref(cf.range));
        w.open("cfRule");
        const std::string& t = cf.type;
        const bool styled = t != "color_scale" && t != "data_bar";
        if (t == "cell") w.attr("type", "cellIs");
        else if (t == "formula") w.attr("type", "expression");
        else if (t == "text") w.attr("type", "containsText").attr("operator", "containsText").attr("text", cf.value1);
        else if (t == "begins") w.attr("type", "beginsWith").attr("operator", "beginsWith").attr("text", cf.value1);
        else if (t == "ends") w.attr("type", "endsWith").attr("operator", "endsWith").attr("text", cf.value1);
        else if (t == "top" || t == "bottom") w.attr("type", "top10");
        else if (t == "above_average" || t == "below_average") w.attr("type", "aboveAverage");
        else if (t == "duplicate") w.attr("type", "duplicateValues");
        else if (t == "unique") w.attr("type", "uniqueValues");
        else if (t == "blank") w.attr("type", "containsBlanks");
        else if (t == "not_blank") w.attr("type", "notContainsBlanks");
        else if (t == "errors") w.attr("type", "containsErrors");
        else if (t == "color_scale") w.attr("type", "colorScale");
        else w.attr("type", "dataBar");
        if (styled) w.attr("dxfId", std::to_string(styles.dxfOf(cf.style)));
        w.attr("priority", std::to_string(priority--));
        if (t == "cell") w.attr("operator", opName(cf.op));
        if (t == "top" || t == "bottom") {
            w.attr("rank", cf.value1.empty() ? "10" : cf.value1);
            if (cf.percent) w.attr("percent", "1");
            if (t == "bottom") w.attr("bottom", "1");
        }
        if (t == "below_average") w.attr("aboveAverage", "0");
        if (t == "cell") {
            w.element("formula", ruleFormula(cf.value1));
            if (cf.op == "between" || cf.op == "not_between") w.element("formula", ruleFormula(cf.value2));
        } else if (t == "formula") {
            w.element("formula", trimCopy(cf.value1[0] == '=' ? cf.value1.substr(1) : cf.value1));
        } else if (t == "text") {
            w.element("formula", "NOT(ISERROR(SEARCH(" + quoted(cf.value1) + "," + tl + ")))");
        } else if (t == "begins") {
            w.element("formula", "LEFT(" + tl + ",LEN(" + quoted(cf.value1) + "))=" + quoted(cf.value1));
        } else if (t == "ends") {
            w.element("formula", "RIGHT(" + tl + ",LEN(" + quoted(cf.value1) + "))=" + quoted(cf.value1));
        } else if (t == "blank") {
            w.element("formula", "LEN(TRIM(" + tl + "))=0");
        } else if (t == "not_blank") {
            w.element("formula", "LEN(TRIM(" + tl + "))>0");
        } else if (t == "errors") {
            w.element("formula", "ISERROR(" + tl + ")");
        } else if (t == "color_scale") {
            w.open("colorScale").leaf("cfvo", {{"type", "min"}});
            if (hasColor(cf.midColor)) w.leaf("cfvo", {{"type", "percentile"}, {"val", "50"}});
            w.leaf("cfvo", {{"type", "max"}});
            w.leaf("color", {{"rgb", argb(hasColor(cf.minColor) ? cf.minColor : (0xF8696B | kColorSet))}});
            if (hasColor(cf.midColor)) w.leaf("color", {{"rgb", argb(cf.midColor)}});
            w.leaf("color", {{"rgb", argb(hasColor(cf.maxColor) ? cf.maxColor : (0x63BE7B | kColorSet))}});
            w.close();
        } else if (t == "data_bar") {
            w.open("dataBar").leaf("cfvo", {{"type", "min"}}).leaf("cfvo", {{"type", "max"}});
            w.leaf("color", {{"rgb", argb(hasColor(cf.maxColor) ? cf.maxColor : (0x638EC6 | kColorSet))}}).close();
        }
        w.close().close();
    }
}

void writeValidations(xml::Writer& w, const Sheet& sh) {
    if (sh.validations.empty()) return;
    w.open("dataValidations").attr("count", std::to_string(sh.validations.size()));
    for (const auto& v : sh.validations) {
        w.open("dataValidation");
        w.attr("type", v.type == "length" ? "textLength" : v.type);
        if (v.type != "list" && v.type != "custom" && v.op != "between") w.attr("operator", opName(v.op));
        w.attr("errorStyle", v.errorStyle == "warning" || v.errorStyle == "information" ? v.errorStyle : "stop");
        if (v.allowBlank) w.attr("allowBlank", "1");
        if (v.type == "list" && !v.dropdown) w.attr("showDropDown", "1");  // sic: the flag hides the arrow
        w.attr("showInputMessage", "1").attr("showErrorMessage", "1");
        if (!v.errorTitle.empty()) w.attr("errorTitle", v.errorTitle);
        if (!v.error.empty()) w.attr("error", v.error);
        if (!v.promptTitle.empty()) w.attr("promptTitle", v.promptTitle);
        if (!v.prompt.empty()) w.attr("prompt", v.prompt);
        w.attr("sqref", sqref(v.range));
        std::string f1 = v.value1, f2 = v.value2;
        if (v.type == "list") {
            Range r;
            const std::string t = trimCopy(f1[0] == '=' ? f1.substr(1) : f1);
            const bool isRef = rangeRef(t, r) || t.find('!') != std::string::npos;
            f1 = isRef ? t : "\"" + t + "\"";
        } else if (v.type == "custom") {
            f1 = trimCopy(!f1.empty() && f1[0] == '=' ? f1.substr(1) : f1);
        } else {
            f1 = ruleFormula(f1);
            f2 = ruleFormula(f2);
        }
        w.element("formula1", f1);
        if ((v.op == "between" || v.op == "not_between") && v.type != "list" && v.type != "custom") w.element("formula2", f2);
        w.close();
    }
    w.close();
}

std::string worksheetXml(const Workbook& wb, std::size_t index, Styles& styles, Strings& strings, bool hasDrawing, bool& dynamicArrays) {
    const Sheet& sh = wb.sheets[index];
    xml::Writer w;
    w.open("worksheet").attr("xmlns", kMainNs).attr("xmlns:r", kRelNs);
    int r1, c1, r2, c2;
    if (sh.bounds(r1, c1, r2, c2)) w.leaf("dimension", {{"ref", cellName(r1, c1) + ":" + cellName(r2, c2)}});
    const int active = std::clamp(wb.active, 0, static_cast<int>(wb.sheets.size()) - 1);
    writeViewsAndCols(w, sh, static_cast<int>(index) == active);
    writeSheetData(w, sh, styles, strings, dynamicArrays);
    if (sh.hasAutoFilter) w.leaf("autoFilter", {{"ref", sqref(sh.autoFilter)}});
    if (!sh.merges.empty()) {
        w.open("mergeCells").attr("count", std::to_string(sh.merges.size()));
        for (const auto& m : sh.merges) w.leaf("mergeCell", {{"ref", sqref(m)}});
        w.close();
    }
    writeCondFormats(w, sh, styles);
    writeValidations(w, sh);
    w.leaf("pageMargins", {{"left", "0.7"}, {"right", "0.7"}, {"top", "0.75"}, {"bottom", "0.75"}, {"header", "0.3"}, {"footer", "0.3"}});
    if (hasDrawing) w.leaf("drawing", {{"r:id", "rIdDrawing"}});
    return w.str();
}

const char* kMetadata =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<metadata xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
    "xmlns:xda=\"http://schemas.microsoft.com/office/spreadsheetml/2017/dynamicarray\">"
    "<metadataTypes count=\"1\"><metadataType name=\"XLDAPR\" minSupportedVersion=\"120000\" copy=\"1\" pasteAll=\"1\" "
    "pasteValues=\"1\" merge=\"1\" splitFirst=\"1\" rowColShift=\"1\" clearFormats=\"1\" clearComments=\"1\" assign=\"1\" "
    "coerce=\"1\" cellMeta=\"1\"/></metadataTypes>"
    "<futureMetadata name=\"XLDAPR\" count=\"1\"><bk><extLst><ext uri=\"{bdbb8cdc-fa1e-496e-a857-3c3f30c029c3}\">"
    "<xda:dynamicArrayProperties fDynamic=\"1\" fCollapsed=\"0\"/></ext></extLst></bk></futureMetadata>"
    "<cellMetadata count=\"1\"><bk><rc t=\"1\" v=\"0\"/></bk></cellMetadata></metadata>";

std::string workbookXml(const Workbook& wb) {
    xml::Writer w;
    w.open("workbook").attr("xmlns", kMainNs).attr("xmlns:r", kRelNs);
    w.open("bookViews").leaf("workbookView", {{"activeTab", std::to_string(std::max(0, wb.active))}}).close();
    w.open("sheets");
    for (std::size_t i = 0; i < wb.sheets.size(); ++i) {
        w.leaf("sheet", {{"name", wb.sheets[i].name}, {"sheetId", std::to_string(i + 1)}, {"r:id", "rId" + std::to_string(i + 1)}});
    }
    w.close();
    bool any = !wb.names.empty();
    for (const auto& sh : wb.sheets) any = any || sh.hasAutoFilter;
    if (any) {
        w.open("definedNames");
        for (std::size_t i = 0; i < wb.sheets.size(); ++i) {
            const Sheet& sh = wb.sheets[i];
            if (!sh.hasAutoFilter) continue;
            const Range& r = sh.autoFilter;
            w.open("definedName").attr("name", "_xlnm._FilterDatabase").attr("localSheetId", std::to_string(i)).attr("hidden", "1");
            w.text(quoteSheet(sh.name) + "!$" + colName(r.c1) + "$" + std::to_string(r.r1 + 1) + ":$" + colName(r.c2) + "$" + std::to_string(r.r2 + 1));
            w.close();
        }
        for (const auto& [key, n] : wb.names) {
            w.open("definedName").attr("name", n.name);
            if (!n.comment.empty()) w.attr("comment", n.comment);
            w.text(fileFormula(n.formula, true)).close();
        }
        w.close();
    }
    w.leaf("calcPr", {{"calcId", "191029"}, {"fullCalcOnLoad", "1"}});
    return w.str();
}

}  // namespace

std::string writeXlsx(const Workbook& wb) {
    Styles styles;
    Strings strings;
    bool dynamicArrays = false;
    const std::size_t n = wb.sheets.size();
    std::vector<std::string> sheetXml(n);
    std::vector<ChartParts> charts(n);
    int chartNumber = 1;
    for (std::size_t i = 0; i < n; ++i) {
        charts[i] = writeCharts(wb, static_cast<int>(i), chartNumber);
        chartNumber += static_cast<int>(charts[i].charts.size());
        sheetXml[i] = worksheetXml(wb, i, styles, strings, !charts[i].drawing.empty(), dynamicArrays);
    }

    xml::Writer ss;
    ss.open("sst").attr("xmlns", kMainNs).attr("count", std::to_string(strings.list.size())).attr("uniqueCount", std::to_string(strings.list.size()));
    for (const auto& s : strings.list) {
        ss.open("si").open("t");
        if (!s.empty() && (s.front() == ' ' || s.back() == ' ' || s.find('\n') != std::string::npos)) ss.attr("xml:space", "preserve");
        ss.text(s).close().close();
    }

    xml::Writer rels;
    rels.open("Relationships").attr("xmlns", kPkgRelNs);
    for (std::size_t i = 0; i < n; ++i) {
        rels.leaf("Relationship", {{"Id", "rId" + std::to_string(i + 1)}, {"Type", std::string(kRelType) + "worksheet"},
                                   {"Target", "worksheets/sheet" + std::to_string(i + 1) + ".xml"}});
    }
    rels.leaf("Relationship", {{"Id", "rId" + std::to_string(n + 1)}, {"Type", std::string(kRelType) + "styles"}, {"Target", "styles.xml"}});
    rels.leaf("Relationship", {{"Id", "rId" + std::to_string(n + 2)}, {"Type", std::string(kRelType) + "sharedStrings"}, {"Target", "sharedStrings.xml"}});
    if (dynamicArrays) {
        rels.leaf("Relationship", {{"Id", "rId" + std::to_string(n + 3)}, {"Type", std::string(kRelType) + "sheetMetadata"}, {"Target", "metadata.xml"}});
    }

    xml::Writer ct;
    ct.open("Types").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/content-types");
    ct.leaf("Default", {{"Extension", "rels"}, {"ContentType", "application/vnd.openxmlformats-package.relationships+xml"}});
    ct.leaf("Default", {{"Extension", "xml"}, {"ContentType", "application/xml"}});
    const std::string sml = "application/vnd.openxmlformats-officedocument.spreadsheetml.";
    ct.leaf("Override", {{"PartName", "/xl/workbook.xml"}, {"ContentType", sml + "sheet.main+xml"}});
    for (std::size_t i = 0; i < n; ++i) {
        ct.leaf("Override", {{"PartName", "/xl/worksheets/sheet" + std::to_string(i + 1) + ".xml"}, {"ContentType", sml + "worksheet+xml"}});
    }
    ct.leaf("Override", {{"PartName", "/xl/styles.xml"}, {"ContentType", sml + "styles+xml"}});
    ct.leaf("Override", {{"PartName", "/xl/sharedStrings.xml"}, {"ContentType", sml + "sharedStrings+xml"}});
    if (dynamicArrays) ct.leaf("Override", {{"PartName", "/xl/metadata.xml"}, {"ContentType", sml + "sheetMetadata+xml"}});
    for (int k = 1; k < chartNumber; ++k) {
        ct.leaf("Override", {{"PartName", "/xl/charts/chart" + std::to_string(k) + ".xml"},
                             {"ContentType", "application/vnd.openxmlformats-officedocument.drawingml.chart+xml"}});
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (charts[i].drawing.empty()) continue;
        ct.leaf("Override", {{"PartName", "/xl/drawings/drawing" + std::to_string(i + 1) + ".xml"},
                             {"ContentType", "application/vnd.openxmlformats-officedocument.drawing+xml"}});
    }

    xml::Writer root;
    root.open("Relationships").attr("xmlns", kPkgRelNs);
    root.leaf("Relationship", {{"Id", "rId1"}, {"Type", std::string(kRelType) + "officeDocument"}, {"Target", "xl/workbook.xml"}});

    std::vector<raster::ZipEntry> entries;
    entries.push_back({"[Content_Types].xml", ct.str()});
    entries.push_back({"_rels/.rels", root.str()});
    entries.push_back({"xl/workbook.xml", workbookXml(wb)});
    entries.push_back({"xl/_rels/workbook.xml.rels", rels.str()});
    chartNumber = 1;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string name = "sheet" + std::to_string(i + 1);
        entries.push_back({"xl/worksheets/" + name + ".xml", sheetXml[i]});
        if (charts[i].drawing.empty()) continue;
        xml::Writer sr;
        sr.open("Relationships").attr("xmlns", kPkgRelNs);
        sr.leaf("Relationship", {{"Id", "rIdDrawing"}, {"Type", std::string(kRelType) + "drawing"}, {"Target", "../drawings/drawing" + std::to_string(i + 1) + ".xml"}});
        entries.push_back({"xl/worksheets/_rels/" + name + ".xml.rels", sr.str()});
        entries.push_back({"xl/drawings/drawing" + std::to_string(i + 1) + ".xml", charts[i].drawing});
        entries.push_back({"xl/drawings/_rels/drawing" + std::to_string(i + 1) + ".xml.rels", charts[i].drawingRels});
        for (const auto& c : charts[i].charts) entries.push_back({"xl/charts/chart" + std::to_string(chartNumber++) + ".xml", c});
    }
    entries.push_back({"xl/styles.xml", styles.xmlText()});
    entries.push_back({"xl/sharedStrings.xml", ss.str()});
    if (dynamicArrays) entries.push_back({"xl/metadata.xml", kMetadata});
    return raster::writeZip(entries);
}

}  // namespace wetwaresuite::office
