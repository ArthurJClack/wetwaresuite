// .xlsx helpers shared by reader and writer, and charts as DrawingML parts
// (bar, column, line, area, pie and scatter charts anchored over cells).
#include <cmath>
#include <stdexcept>

#include "wetwaresuite/office/chart.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"
#include "xlsx_internal.hpp"

namespace wetwaresuite::office::xlsx {

const std::map<int, std::string>& builtinFormats() {
    static const std::map<int, std::string> m = {
        {0, ""},          {1, "0"},           {2, "0.00"},        {3, "#,##0"},       {4, "#,##0.00"},     {9, "0%"},
        {10, "0.00%"},    {11, "0.00E+00"},   {12, "# ?/?"},      {13, "# ?\?/?\?"},    {14, "yyyy-mm-dd"},  {15, "d-mmm-yy"},
        {16, "d-mmm"},    {17, "mmm-yy"},     {18, "h:mm AM/PM"}, {19, "h:mm:ss AM/PM"}, {20, "h:mm"}, {21, "h:mm:ss"},
        {22, "yyyy-mm-dd h:mm"}, {37, "#,##0 ;(#,##0)"}, {38, "#,##0 ;[Red](#,##0)"}, {39, "#,##0.00;(#,##0.00)"},
        {40, "#,##0.00;[Red](#,##0.00)"}, {45, "mm:ss"}, {46, "[h]:mm:ss"}, {47, "mmss.0"}, {48, "##0.0E+0"}, {49, "@"},
    };
    return m;
}

std::string zipEntry(const Files& files, const std::string& name) {
    auto it = files.find(name);
    return it == files.end() ? std::string() : it->second;
}

std::string resolveTarget(const std::string& base, const std::string& target) {
    if (!target.empty() && target[0] == '/') return target.substr(1);
    std::string dir = base.substr(0, base.rfind('/') + 1);
    std::string path = dir + target;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path + "/") {
        if (c == '/') {
            if (cur == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!cur.empty() && cur != ".") {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

std::map<std::string, std::string> readRels(const Files& files, const std::string& part) {
    std::map<std::string, std::string> out;
    std::string dir = part.substr(0, part.rfind('/') + 1);
    std::string relsPath = dir + "_rels/" + part.substr(part.rfind('/') + 1) + ".rels";
    std::string data = zipEntry(files, relsPath);
    if (data.empty()) return out;
    xml::Node root = xml::parse(data);
    for (const auto* r : root.all("Relationship")) out[r->attr("Id")] = resolveTarget(part, r->attr("Target"));
    return out;
}

bool cellRef(const std::string& refIn, int& row, int& col) {
    std::string ref;
    for (char c : refIn) {
        if (c != '$') ref += c;
    }
    std::size_t i = 0;
    std::string letters;
    while (i < ref.size() && std::isalpha(static_cast<unsigned char>(ref[i]))) letters += ref[i++];
    if (letters.empty() || i >= ref.size()) return false;
    col = colIndex(upper(letters));
    long r = std::strtol(ref.c_str() + i, nullptr, 10);
    row = static_cast<int>(r - 1);
    return col >= 0 && row >= 0 && row < kMaxRows;
}

bool rangeRef(const std::string& ref, Range& out) {
    auto colon = ref.find(':');
    if (!cellRef(ref.substr(0, colon), out.r1, out.c1)) return false;
    out.r2 = out.r1, out.c2 = out.c1;
    if (colon != std::string::npos && !cellRef(ref.substr(colon + 1), out.r2, out.c2)) return false;
    if (out.r2 < out.r1) std::swap(out.r1, out.r2);
    if (out.c2 < out.c1) std::swap(out.c1, out.c2);
    return true;
}

std::string argb(std::uint32_t color) {
    char b[12];
    std::snprintf(b, sizeof b, "FF%06X", static_cast<unsigned>(rgbOf(color)));
    return b;
}

namespace {

std::uint32_t tinted(std::uint32_t rgb, double tint) {
    if (tint == 0) return rgb;
    auto ch = [&](int shift) {
        double v = (rgb >> shift) & 0xFF;
        v = tint < 0 ? v * (1 + tint) : v + (255 - v) * tint;
        return static_cast<std::uint32_t>(std::clamp(std::lround(v), 0L, 255L));
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

}  // namespace

std::uint32_t readColor(const xml::Node* n) {
    if (!n) return 0;
    std::uint32_t rgb = 0;
    bool have = false;
    if (n->hasAttr("rgb")) {
        std::string v = n->attr("rgb");
        if (v.size() == 8) v = v.substr(2);
        rgb = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 16)) & 0xFFFFFF;
        have = true;
    } else if (n->hasAttr("theme")) {
        // the default Office theme: lt1, dk1, lt2, dk2, accent1..6
        static const std::uint32_t theme[] = {0xFFFFFF, 0x000000, 0xE7E6E6, 0x44546A, 0x4472C4, 0xED7D31,
                                              0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47, 0x0563C1, 0x954F72};
        const int t = std::atoi(n->attr("theme").c_str());
        if (t >= 0 && t < 12) rgb = theme[t], have = true;
    } else if (n->hasAttr("indexed")) {
        static const std::uint32_t indexed[] = {0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
                                                0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
                                                0x800000, 0x008000, 0x000080, 0x808000, 0x800080, 0x008080, 0xC0C0C0, 0x808080};
        const int i = std::atoi(n->attr("indexed").c_str());
        if (i >= 0 && i < 24) rgb = indexed[i], have = true;  // 64 and up: system colours, i.e. automatic
    }
    if (!have) return 0;
    if (n->hasAttr("tint")) rgb = tinted(rgb, std::atof(n->attr("tint").c_str()));
    return rgb | kColorSet;
}

const char* borderName(int code) {
    static const char* names[] = {"", "thin", "medium", "thick", "double", "dashed", "dotted"};
    return code >= 0 && code <= 6 ? names[code] : "";
}

int borderCode(const std::string& name) {
    if (name.empty() || name == "none") return 0;
    if (name == "medium" || name == "mediumDashDot" || name == "mediumDashDotDot" || name == "slantDashDot") return 2;
    if (name == "thick") return 3;
    if (name == "double") return 4;
    if (name == "dashed" || name == "mediumDashed" || name == "dashDot" || name == "dashDotDot") return 5;
    if (name == "dotted" || name == "hair") return 6;
    return 1;
}

std::string opName(const std::string& op) {
    if (op == ">") return "greaterThan";
    if (op == "<") return "lessThan";
    if (op == ">=") return "greaterThanOrEqual";
    if (op == "<=") return "lessThanOrEqual";
    if (op == "=") return "equal";
    if (op == "<>") return "notEqual";
    if (op == "not_between") return "notBetween";
    return "between";
}

std::string opFromName(const std::string& n) {
    if (n == "greaterThan") return ">";
    if (n == "lessThan") return "<";
    if (n == "greaterThanOrEqual") return ">=";
    if (n == "lessThanOrEqual") return "<=";
    if (n == "equal") return "=";
    if (n == "notEqual") return "<>";
    if (n == "notBetween") return "not_between";
    return "between";
}

std::string ruleFormula(const std::string& value) {
    std::string t = trimCopy(value);
    if (!t.empty() && t[0] == '=') t = trimCopy(t.substr(1));
    if (t.empty()) return t;
    char* end = nullptr;
    std::strtod(t.c_str(), &end);
    if (end && *end == 0) return t;
    int y, m, d;
    if (parseIsoDate(t, y, m, d)) return textNumber(dateSerial(y, m, d));
    int h, mi;
    char colon;
    if (std::sscanf(t.c_str(), "%d%c%d", &h, &colon, &mi) == 3 && colon == ':' && t.find_first_not_of("0123456789:") == std::string::npos) {
        return textNumber((h * 60 + mi) / 1440.0);
    }
    bool word = formulaError(t) != "";
    if (!word) {
        // a bare word that is not a cell reference or TRUE/FALSE reads as text, the way the rule engine takes it
        bool plain = t.find_first_of("()\"!:+-*/&^<>=,{}") == std::string::npos;
        int r, c;
        const std::string up = upper(t);
        if (plain && !cellRef(t, r, c) && up != "TRUE" && up != "FALSE") word = true;
    }
    if (word) {
        std::string q = "\"";
        for (char ch : t) q += ch == '"' ? std::string("\"\"") : std::string(1, ch);
        return q + "\"";
    }
    return t;
}

std::string ruleValue(const std::string& formula, bool date) {
    std::string f = trimCopy(formula);
    if (f.size() >= 2 && f.front() == '"' && f.back() == '"') {
        std::string out;
        for (std::size_t i = 1; i + 1 < f.size(); ++i) {
            out += f[i];
            if (f[i] == '"' && f[i + 1] == '"') ++i;
        }
        return out;
    }
    if (date) {
        char* end = nullptr;
        const double v = std::strtod(f.c_str(), &end);
        if (end && *end == 0 && !f.empty() && v >= 1) {
            int y, m, d;
            serialDate(v, y, m, d);
            char b[16];
            std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
            return b;
        }
    }
    return f;
}

double colPixels(const Sheet& sh, int col) {
    if (sh.hiddenCols.count(col)) return 0;
    auto it = sh.colWidths.find(col);
    const double w = it == sh.colWidths.end() ? 8.43 : it->second;
    return std::floor(w * 7 + 5);
}

double rowPixels(const Sheet& sh, int row) {
    if (sh.hiddenRows.count(row)) return 0;
    auto it = sh.rowHeights.find(row);
    return (it == sh.rowHeights.end() ? 15.0 : it->second) / 0.75;
}

// ---- charts ---------------------------------------------------------------------------

namespace {

constexpr long kEmuPerPx = 9525;

std::string seriesHex(std::size_t i) {
    static const char* palette[] = {"2B579A", "E97B26", "3A9B5C", "C93C3C", "7E57C2", "1F9EA8", "D8A91E", "8C564B", "E377C2", "7F7F7F"};
    return palette[i % 10];
}

std::string absRange(const std::string& sheet, int r1, int c1, int r2, int c2) {
    std::string s = quoteSheet(sheet) + "!$" + colName(c1) + "$" + std::to_string(r1 + 1);
    if (r2 != r1 || c2 != c1) s += ":$" + colName(c2) + "$" + std::to_string(r2 + 1);
    return s;
}

struct SeriesRefs {
    std::string name, cat, val;
};

void strCache(xml::Writer& w, const std::vector<std::string>& values) {
    w.open("c:strCache").leaf("c:ptCount", {{"val", std::to_string(values.size())}});
    for (std::size_t i = 0; i < values.size(); ++i) w.open("c:pt").attr("idx", std::to_string(i)).element("c:v", values[i]).close();
    w.close();
}

void numCache(xml::Writer& w, const std::vector<double>& values) {
    w.open("c:numCache").element("c:formatCode", "General").leaf("c:ptCount", {{"val", std::to_string(values.size())}});
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (std::isfinite(values[i])) w.open("c:pt").attr("idx", std::to_string(i)).element("c:v", textNumber(values[i])).close();
    }
    w.close();
}

void numRef(xml::Writer& w, const char* tag, const std::string& ref, const std::vector<double>& values) {
    w.open(tag).open("c:numRef").element("c:f", ref);
    numCache(w, values);
    w.close().close();
}

void catRef(xml::Writer& w, const char* tag, const std::string& ref, const std::vector<std::string>& values) {
    w.open(tag).open("c:strRef").element("c:f", ref);
    strCache(w, values);
    w.close().close();
}

void axes(xml::Writer& w, bool horizontal, bool scatter) {
    w.open(scatter ? "c:valAx" : "c:catAx").leaf("c:axId", {{"val", "500000001"}});
    w.open("c:scaling").leaf("c:orientation", {{"val", "minMax"}}).close();
    w.leaf("c:delete", {{"val", "0"}}).leaf("c:axPos", {{"val", horizontal ? "l" : "b"}});
    w.leaf("c:majorTickMark", {{"val", "out"}}).leaf("c:minorTickMark", {{"val", "none"}}).leaf("c:tickLblPos", {{"val", "nextTo"}});
    w.leaf("c:crossAx", {{"val", "500000002"}}).leaf("c:crosses", {{"val", "autoZero"}});
    if (!scatter) w.leaf("c:auto", {{"val", "1"}}).leaf("c:lblAlgn", {{"val", "ctr"}}).leaf("c:lblOffset", {{"val", "100"}});
    else w.leaf("c:crossBetween", {{"val", "midCat"}});
    w.close();
    w.open("c:valAx").leaf("c:axId", {{"val", "500000002"}});
    w.open("c:scaling").leaf("c:orientation", {{"val", "minMax"}}).close();
    w.leaf("c:delete", {{"val", "0"}}).leaf("c:axPos", {{"val", horizontal ? "b" : "l"}}).leaf("c:majorGridlines");
    w.leaf("c:majorTickMark", {{"val", "out"}}).leaf("c:minorTickMark", {{"val", "none"}}).leaf("c:tickLblPos", {{"val", "nextTo"}});
    w.leaf("c:crossAx", {{"val", "500000001"}}).leaf("c:crosses", {{"val", "autoZero"}});
    w.leaf("c:crossBetween", {{"val", scatter ? "midCat" : "between"}});
    w.close();
}

std::string chartXml(const Workbook& wb, int sheet, const SheetChart& sc) {
    Range range;
    std::string err;
    if (!parseRange(wb, sc.range, sheet, range, err)) throw std::runtime_error(err);
    const ChartLayout L = chartLayout(wb, range, sc.byRows);
    const Chart chart = chartFromRange(wb, range, sc.byRows);
    const std::string sheetName = wb.sheets[static_cast<std::size_t>(range.sheet)].name;
    const Range& r = L.range;
    const int N = L.byRows ? r.cols() : r.rows();
    const int i0 = L.header ? 1 : 0;
    const int iEnd = std::min(N - 1, i0 + 999);
    auto at = [&](int i, int j, int& row, int& col) {
        row = L.byRows ? r.r1 + j : r.r1 + i;
        col = L.byRows ? r.c1 + i : r.c1 + j;
    };
    auto refOf = [&](int ia, int ib, int j) {
        int ra, ca, rb, cb;
        at(ia, j, ra, ca);
        at(ib, j, rb, cb);
        return absRange(sheetName, ra, ca, rb, cb);
    };
    const std::string type = sc.type;
    const bool scatter = type == "scatter", pie = type == "pie";

    xml::Writer w;
    w.open("c:chartSpace").attr("xmlns:c", kChartNs).attr("xmlns:a", kDrawingNs).attr("xmlns:r", kRelNs);
    w.leaf("c:roundedCorners", {{"val", "0"}});
    w.open("c:chart");
    if (!sc.title.empty()) {
        w.open("c:title").open("c:tx").open("c:rich").leaf("a:bodyPr").open("a:p").open("a:r").element("a:t", sc.title).close().close().close().close();
        w.leaf("c:overlay", {{"val", "0"}}).close();
        w.leaf("c:autoTitleDeleted", {{"val", "0"}});
    } else {
        w.leaf("c:autoTitleDeleted", {{"val", "1"}});
    }
    w.open("c:plotArea").leaf("c:layout");
    if (type == "bar" || type == "hbar") {
        w.open("c:barChart").leaf("c:barDir", {{"val", type == "hbar" ? "bar" : "col"}}).leaf("c:grouping", {{"val", "clustered"}});
    } else if (type == "line") {
        w.open("c:lineChart").leaf("c:grouping", {{"val", "standard"}});
    } else if (type == "area") {
        w.open("c:areaChart").leaf("c:grouping", {{"val", "standard"}});
    } else if (pie) {
        w.open("c:pieChart");
    } else {
        w.open("c:scatterChart").leaf("c:scatterStyle", {{"val", "lineMarker"}});
    }
    w.leaf("c:varyColors", {{"val", pie ? "1" : "0"}});
    std::size_t first = scatter && chart.series.size() >= 2 ? 1 : 0;
    const std::size_t count = pie ? std::min<std::size_t>(1, chart.series.size()) : chart.series.size();
    for (std::size_t si = first, idx = 0; si < count; ++si, ++idx) {
        const ChartSeries& s = chart.series[si];
        w.open("c:ser").leaf("c:idx", {{"val", std::to_string(idx)}}).leaf("c:order", {{"val", std::to_string(idx)}});
        if (L.header) {
            int hr, hc;
            at(0, s.field, hr, hc);
            w.open("c:tx").open("c:strRef").element("c:f", absRange(sheetName, hr, hc, hr, hc));
            strCache(w, {s.name});
            w.close().close();
        }
        // explicit colours (the same palette as previews), so every reader draws the series alike
        const std::string colour = seriesHex(idx);
        auto solid = [&](const std::string& hex) { w.open("a:solidFill").leaf("a:srgbClr", {{"val", hex}}).close(); };
        auto marker = [&] {
            w.open("c:marker").leaf("c:symbol", {{"val", "circle"}}).leaf("c:size", {{"val", "5"}});
            w.open("c:spPr");
            solid(colour);
            w.open("a:ln");
            solid(colour);
            w.close().close().close();
        };
        if (scatter) {
            w.open("c:spPr").open("a:ln").attr("w", "19050").leaf("a:noFill").close().close();
            marker();
        } else if (type == "line") {
            w.open("c:spPr").open("a:ln").attr("w", "28575").attr("cap", "rnd");
            solid(colour);
            w.leaf("a:round").close().close();
            marker();
        } else if (pie) {
            for (std::size_t p = 0; p < s.values.size(); ++p) {
                w.open("c:dPt").leaf("c:idx", {{"val", std::to_string(p)}}).leaf("c:bubble3D", {{"val", "0"}}).open("c:spPr");
                solid(seriesHex(p));
                w.open("a:ln");
                solid("FFFFFF");
                w.close();
                w.close().close();
            }
        } else {
            w.open("c:spPr");
            solid(colour);
            w.close();
            if (type == "bar" || type == "hbar") w.leaf("c:invertIfNegative", {{"val", "0"}});
        }
        if (scatter) {
            if (first == 1) numRef(w, "c:xVal", refOf(i0, iEnd, chart.series[0].field), chart.series[0].values);
            numRef(w, "c:yVal", refOf(i0, iEnd, s.field), s.values);
            w.leaf("c:smooth", {{"val", "0"}});
        } else {
            if (L.labels) catRef(w, "c:cat", refOf(i0, iEnd, 0), chart.categories);
            numRef(w, "c:val", refOf(i0, iEnd, s.field), s.values);
            if (type == "line") w.leaf("c:smooth", {{"val", "0"}});
        }
        w.close();
    }
    if (type == "bar" || type == "hbar") w.leaf("c:gapWidth", {{"val", "150"}});
    if (type == "line") w.leaf("c:marker", {{"val", "1"}});
    if (pie) {
        w.leaf("c:firstSliceAng", {{"val", "0"}});
    } else {
        w.leaf("c:axId", {{"val", "500000001"}}).leaf("c:axId", {{"val", "500000002"}});
    }
    w.close();  // the chart type
    if (!pie) axes(w, type == "hbar", scatter);
    w.close();  // plotArea
    if (pie || chart.series.size() > first + 1) w.open("c:legend").leaf("c:legendPos", {{"val", pie ? "r" : "b"}}).leaf("c:overlay", {{"val", "0"}}).close();
    w.leaf("c:plotVisOnly", {{"val", "1"}}).leaf("c:dispBlanksAs", {{"val", "gap"}});
    w.close();  // chart
    return w.str();
}

void anchorCell(const Sheet& sh, int start, double px, bool cols, int& cell, long& offset) {
    cell = start;
    double left = px;
    for (int guard = 0; guard < 2000; ++guard) {
        const double size = cols ? colPixels(sh, cell) : rowPixels(sh, cell);
        if (left < size || (cols ? cell >= kMaxCols - 1 : cell >= kMaxRows - 1)) break;
        left -= size;
        ++cell;
    }
    offset = std::lround(left * kEmuPerPx);
}

}  // namespace

ChartParts writeCharts(const Workbook& wb, int sheet, int firstChartNumber) {
    ChartParts parts;
    const Sheet& sh = wb.sheets[static_cast<std::size_t>(sheet)];
    xml::Writer d;
    d.open("xdr:wsDr").attr("xmlns:xdr", kSheetDrawingNs).attr("xmlns:a", kDrawingNs);
    xml::Writer rels;
    rels.open("Relationships").attr("xmlns", kPkgRelNs);
    int id = 0;
    for (const auto& sc : sh.charts) {
        std::string xmlText;
        try {
            xmlText = chartXml(wb, sheet, sc);
        } catch (const std::exception&) {
            continue;  // the data is gone; Excel would show an empty frame
        }
        ++id;
        const std::string rid = "rId" + std::to_string(id);
        parts.charts.push_back(std::move(xmlText));
        rels.leaf("Relationship", {{"Id", rid}, {"Type", std::string(kRelType) + "chart"},
                                   {"Target", "../charts/chart" + std::to_string(firstChartNumber + id - 1) + ".xml"}});
        int toCol, toRow;
        long colOff, rowOff;
        anchorCell(sh, sc.col, sc.width, true, toCol, colOff);
        anchorCell(sh, sc.row, sc.height, false, toRow, rowOff);
        d.open("xdr:twoCellAnchor").attr("editAs", "oneCell");
        d.open("xdr:from").element("xdr:col", std::to_string(sc.col)).element("xdr:colOff", "0").element("xdr:row", std::to_string(sc.row)).element("xdr:rowOff", "0").close();
        d.open("xdr:to").element("xdr:col", std::to_string(toCol)).element("xdr:colOff", std::to_string(colOff)).element("xdr:row", std::to_string(toRow)).element("xdr:rowOff", std::to_string(rowOff)).close();
        d.open("xdr:graphicFrame").attr("macro", "");
        d.open("xdr:nvGraphicFramePr").leaf("xdr:cNvPr", {{"id", std::to_string(id + 1)}, {"name", "Chart " + std::to_string(id)}}).leaf("xdr:cNvGraphicFramePr").close();
        d.open("xdr:xfrm").leaf("a:off", {{"x", "0"}, {"y", "0"}}).leaf("a:ext", {{"cx", "0"}, {"cy", "0"}}).close();
        d.open("a:graphic").open("a:graphicData").attr("uri", kChartNs);
        d.leaf("c:chart", {{"xmlns:c", kChartNs}, {"xmlns:r", kRelNs}, {"r:id", rid}});
        d.close().close();  // graphicData, graphic
        d.close();          // graphicFrame
        d.leaf("xdr:clientData");
        d.close();  // anchor
    }
    if (parts.charts.empty()) return parts;
    parts.drawing = d.str();
    parts.drawingRels = rels.str();
    return parts;
}

namespace {

std::string chartTypeOf(const xml::Node& plot) {
    for (const auto& ch : plot.children) {
        if (ch.isText()) continue;
        const std::string_view n = ch.local();
        if (n == "barChart" || n == "bar3DChart") {
            const auto* dir = ch.child("barDir");
            return dir && dir->attr("val") == "bar" ? "hbar" : "bar";
        }
        if (n == "lineChart" || n == "line3DChart" || n == "stockChart") return "line";
        if (n == "areaChart" || n == "area3DChart") return "area";
        if (n == "pieChart" || n == "pie3DChart" || n == "doughnutChart" || n == "ofPieChart") return "pie";
        if (n == "scatterChart" || n == "bubbleChart") return "scatter";
        if (n == "radarChart") return "line";
    }
    return "bar";
}

}  // namespace

void readCharts(const Files& files, const std::string& drawingPath, Workbook& wb, int sheet) {
    std::string data = zipEntry(files, drawingPath);
    if (data.empty()) return;
    xml::Node root = xml::parse(data);
    auto rels = readRels(files, drawingPath);
    Sheet& sh = wb.sheets[static_cast<std::size_t>(sheet)];
    for (const auto& anchor : root.children) {
        if (anchor.isText()) continue;
        const std::string_view kind = anchor.local();
        if (kind != "twoCellAnchor" && kind != "oneCellAnchor" && kind != "absoluteAnchor") continue;
        const xml::Node* cref = anchor.find("chart");
        if (!cref) continue;
        auto rel = rels.find(cref->attr("r:id"));
        if (rel == rels.end()) continue;
        std::string cx = zipEntry(files, rel->second);
        if (cx.empty()) continue;
        SheetChart sc;
        auto num = [](const xml::Node* n, const char* name) {
            const xml::Node* c = n ? n->child(name) : nullptr;
            return c ? std::atol(c->allText().c_str()) : 0L;
        };
        const xml::Node* from = anchor.child("from");
        sc.col = static_cast<int>(num(from, "col"));
        sc.row = static_cast<int>(num(from, "row"));
        const double fromX = num(from, "colOff") / static_cast<double>(kEmuPerPx), fromY = num(from, "rowOff") / static_cast<double>(kEmuPerPx);
        if (const xml::Node* ext = anchor.child("ext")) {
            sc.width = static_cast<int>(std::atol(ext->attr("cx").c_str()) / kEmuPerPx);
            sc.height = static_cast<int>(std::atol(ext->attr("cy").c_str()) / kEmuPerPx);
        } else if (const xml::Node* to = anchor.child("to")) {
            double w = -fromX, h = -fromY;
            for (int c = sc.col; c < num(to, "col") && c < sc.col + 500; ++c) w += colPixels(sh, c);
            for (int r = sc.row; r < num(to, "row") && r < sc.row + 2000; ++r) h += rowPixels(sh, r);
            sc.width = static_cast<int>(std::lround(w + num(to, "colOff") / static_cast<double>(kEmuPerPx)));
            sc.height = static_cast<int>(std::lround(h + num(to, "rowOff") / static_cast<double>(kEmuPerPx)));
        }
        sc.width = std::clamp(sc.width, 120, 4000);
        sc.height = std::clamp(sc.height, 90, 4000);
        xml::Node chart = xml::parse(cx);
        const xml::Node* ch = chart.child("chart");
        const xml::Node* plot = ch ? ch->child("plotArea") : nullptr;
        if (!plot) continue;
        sc.type = chartTypeOf(*plot);
        if (const xml::Node* title = ch->child("title")) {
            std::vector<const xml::Node*> ts;
            title->descendants("t", ts);
            for (const auto* t : ts) sc.title += t->allText();
        }
        // the data range: every reference the series use, boxed together
        std::vector<const xml::Node*> fs;
        plot->descendants("f", fs);
        bool any = false, rowsSeries = false;
        Range box;
        for (const auto* f : fs) {
            Range r;
            std::string err;
            if (!parseRange(wb, f->allText(), sheet, r, err)) continue;
            if (!any) {
                box = r;
                any = true;
            } else if (r.sheet == box.sheet) {
                box.r1 = std::min(box.r1, r.r1), box.c1 = std::min(box.c1, r.c1);
                box.r2 = std::max(box.r2, r.r2), box.c2 = std::max(box.c2, r.c2);
            }
        }
        std::vector<const xml::Node*> vals;
        plot->descendants("val", vals);
        plot->descendants("yVal", vals);
        if (!vals.empty()) {
            if (const xml::Node* f = vals[0]->find("f")) {
                Range r;
                std::string err;
                if (parseRange(wb, f->allText(), sheet, r, err)) rowsSeries = r.r1 == r.r2 && r.c2 > r.c1;
            }
        }
        if (!any) continue;
        sc.byRows = rowsSeries;
        sc.range = (box.sheet == sheet ? std::string() : quoteSheet(wb.sheets[static_cast<std::size_t>(box.sheet)].name) + "!") + rangeName(box);
        sh.charts.push_back(sc);
    }
}

}  // namespace wetwaresuite::office::xlsx
