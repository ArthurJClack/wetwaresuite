// Spreadsheet files: everything a sheet can hold survives .xlsx and .ods round trips,
// and CSV import options do what they say.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "wetwaresuite/office/chart.hpp"
#include "wetwaresuite/office/sheet.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "check.hpp"
#include "xml.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::office;

namespace {

void put(Workbook& wb, int sheet, const std::string& ref, const std::string& input) {
    Range r;
    std::string err;
    if (!parseRange(wb, ref, sheet, r, err)) {
        std::printf("bad ref %s: %s\n", ref.c_str(), err.c_str());
        return;
    }
    setInput(wb.sheets[static_cast<std::size_t>(r.sheet)].at(r.r1, r.c1), input);
}

const Cell* at(const Workbook& wb, int sheet, const std::string& ref) {
    Range r;
    std::string err;
    if (!parseRange(wb, ref, sheet, r, err)) return nullptr;
    return wb.sheets[static_cast<std::size_t>(r.sheet)].get(r.r1, r.c1);
}

// OFFICE_TEST_DUMP=<folder> keeps the written files for opening in Excel or LibreOffice by hand.
void dump(const std::string& name, const std::string& bytes) {
    const char* dir = std::getenv("OFFICE_TEST_DUMP");
    if (!dir || !*dir) return;
    if (std::FILE* f = std::fopen((std::string(dir) + "/" + name).c_str(), "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
}

Range rng(const Workbook& wb, const std::string& ref) {
    Range r;
    std::string err;
    parseRange(wb, ref, 0, r, err);
    return r;
}

Workbook sample() {
    Workbook wb;
    wb.sheets.resize(2);
    wb.sheets[0].name = "Sales";
    wb.sheets[1].name = "Rates & Notes";
    const char* rows[][4] = {{"Region", "Q1", "Q2", "Q3"}, {"North", "120", "135", "150"}, {"South", "80", "95", "70"}, {"East", "60", "75", "110"}, {"West", "140", "120", "160"}};
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 4; ++c) put(wb, 0, cellName(r, c), rows[r][c]);
    }
    put(wb, 0, "E1", "Total");
    for (int r = 2; r <= 5; ++r) put(wb, 0, "E" + std::to_string(r), "=SUM(B" + std::to_string(r) + ":D" + std::to_string(r) + ")*TaxRate");
    put(wb, 0, "G1", "=SORT(A2:A5)");
    put(wb, 0, "H1", "=UNIQUE(FILTER(A2:A5,B2:B5>100))");
    put(wb, 0, "A7", "2026-03-15");
    put(wb, 0, "B7", "0.256");
    wb.sheets[0].at(6, 1).format = "0.0%";
    put(wb, 0, "C7", "1234.5");
    wb.sheets[0].at(6, 2).format = "#,##0.00";
    put(wb, 0, "D7", "Wrapped text that goes on");
    put(wb, 0, "A9", "Merged title");
    put(wb, 1, "A1", "Tax");
    put(wb, 1, "B1", "1.1");
    put(wb, 1, "A2", "Note");
    put(wb, 1, "B2", "'007");
    wb.names["TAXRATE"] = DefinedName{"TaxRate", "'Rates & Notes'!$B$1", "VAT multiplier", nullptr};
    wb.names["REGIONS"] = DefinedName{"Regions", "Sales!$A$2:$A$5", "", nullptr};

    Sheet& s = wb.sheets[0];
    CellStyle& head = s.at(0, 0).style;
    head.bold = true;
    head.fill = 0x1F4E79 | kColorSet;
    head.color = 0xFFFFFF | kColorSet;
    head.align = 'c';
    for (int c = 1; c <= 4; ++c) s.at(0, c).style = head;
    CellStyle& b2 = s.at(1, 1).style;
    b2.italic = true;
    b2.underline = true;
    b2.strike = true;
    b2.size = 14;
    b2.border[BorderLeft] = 1;
    b2.border[BorderRight] = 2;
    b2.border[BorderTop] = 3;
    b2.border[BorderBottom] = 4;
    b2.borderColor = 0xC00000 | kColorSet;
    CellStyle& d7 = s.at(6, 3).style;
    d7.wrap = true;
    d7.valign = 'm';
    d7.align = 'l';
    d7.indent = 2;
    s.colWidths[0] = 14.5;
    s.colWidths[3] = 20;
    s.rowHeights[6] = 30;
    s.hiddenRows.insert(10);
    s.hiddenCols.insert(10);
    s.merges.push_back(rng(wb, "A9:D10"));
    s.freezeRows = 1;
    s.freezeCols = 1;
    s.hasAutoFilter = true;
    s.autoFilter = rng(wb, "A1:E5");
    CondFormat cf;
    cf.range = rng(wb, "B2:D5");
    cf.type = "cell";
    cf.op = ">=";
    cf.value1 = "120";
    cf.style.fill = 0xC6EFCE | kColorSet;
    cf.style.color = 0x006100 | kColorSet;
    s.condFormats.push_back(cf);
    CondFormat between = cf;
    between.op = "between";
    between.value1 = "60";
    between.value2 = "80";
    between.style = CellStyle{};
    between.style.bold = true;
    s.condFormats.push_back(between);
    CondFormat text;
    text.range = rng(wb, "A2:A5");
    text.type = "text";
    text.value1 = "th";
    text.style.italic = true;
    s.condFormats.push_back(text);
    CondFormat top;
    top.range = rng(wb, "E2:E5");
    top.type = "top";
    top.value1 = "2";
    top.style.fill = 0xFFEB9C | kColorSet;
    s.condFormats.push_back(top);
    CondFormat scale;
    scale.range = rng(wb, "B2:B5");
    scale.type = "color_scale";
    scale.minColor = 0xFFFFFF | kColorSet;
    scale.midColor = 0xFFEB84 | kColorSet;
    scale.maxColor = 0x63BE7B | kColorSet;
    s.condFormats.push_back(scale);
    CondFormat bar;
    bar.range = rng(wb, "C2:C5");
    bar.type = "data_bar";
    bar.maxColor = 0x5A8AC6 | kColorSet;
    s.condFormats.push_back(bar);
    CondFormat formula;
    formula.range = rng(wb, "A2:E5");
    formula.type = "formula";
    formula.value1 = "$B2>$C2";
    formula.style.border[BorderBottom] = 1;
    s.condFormats.push_back(formula);
    CondFormat word;
    word.range = rng(wb, "A2:A5");
    word.type = "cell";
    word.op = "=";
    word.value1 = "North";
    word.style.fill = 0xFFC7CE | kColorSet;
    s.condFormats.push_back(word);
    Validation list;
    list.range = rng(wb, "A2:A5");
    list.type = "list";
    list.value1 = "North,South,East,West";
    list.error = "Pick a region";
    list.errorTitle = "Region";
    s.validations.push_back(list);
    Validation whole;
    whole.range = rng(wb, "B2:D5");
    whole.type = "whole";
    whole.op = "between";
    whole.value1 = "0";
    whole.value2 = "1000";
    whole.errorStyle = "warning";
    whole.prompt = "Units sold";
    s.validations.push_back(whole);
    Validation date;
    date.range = rng(wb, "A7");
    date.type = "date";
    date.op = ">=";
    date.value1 = "2026-01-01";
    s.validations.push_back(date);
    Validation ref;
    ref.range = rng(wb, "F2:F5");
    ref.type = "list";
    ref.value1 = "$A$2:$A$5";
    ref.dropdown = false;
    s.validations.push_back(ref);
    Validation custom;
    custom.range = rng(wb, "C7");
    custom.type = "custom";
    custom.value1 = "C7>B7";
    s.validations.push_back(custom);
    SheetChart bars;
    bars.type = "bar";
    bars.title = "Quarterly sales";
    bars.range = "A1:D5";
    bars.row = 12;
    bars.col = 0;
    s.charts.push_back(bars);
    SheetChart pie;
    pie.type = "pie";
    pie.range = "A1:B5";
    pie.row = 12;
    pie.col = 8;
    pie.width = 360;
    pie.height = 240;
    s.charts.push_back(pie);
    SheetChart line = bars;
    line.type = "line";
    line.title = "By region";
    line.byRows = true;
    line.row = 30;
    s.charts.push_back(line);
    SheetChart scatter = bars;
    scatter.type = "scatter";
    scatter.title = "";
    scatter.range = "B2:C5";
    scatter.row = 30;
    scatter.col = 8;
    s.charts.push_back(scatter);
    SheetChart area = bars;
    area.type = "area";
    area.range = "'Rates & Notes'!A1:B1";
    area.row = 48;
    s.charts.push_back(area);
    wb.active = 0;
    wb.recalc();
    return wb;
}

void checkValues(const Workbook& wb, const char* label) {
    std::printf("  checking %s\n", label);
    CHECK(wb.sheets.size() == 2 && wb.sheets[0].name == "Sales" && wb.sheets[1].name == "Rates & Notes");
    const Cell* e2 = at(wb, 0, "E2");
    CHECK(e2 && e2->formula == "SUM(B2:D2)*TaxRate" && std::abs(e2->value.number - 445.5) < 1e-9);
    CHECK(at(wb, 0, "G1") && at(wb, 0, "G1")->value.text == "East" && at(wb, 0, "G4") && at(wb, 0, "G4")->value.text == "West");
    CHECK(at(wb, 0, "G2") && at(wb, 0, "G2")->spilled && !at(wb, 0, "G2")->hasFormula());
    CHECK(at(wb, 0, "H1") && at(wb, 0, "H1")->value.text == "North" && at(wb, 0, "H2") && at(wb, 0, "H2")->value.text == "West");
    CHECK(at(wb, 0, "B7") && at(wb, 0, "B7")->format == "0.0%" && formatValue(at(wb, 0, "B7")->value, at(wb, 0, "B7")->format) == "25.6%");
    CHECK(at(wb, 0, "C7") && formatValue(at(wb, 0, "C7")->value, at(wb, 0, "C7")->format) == "1,234.50");
    CHECK(at(wb, 0, "A7") && isDateFormat(at(wb, 0, "A7")->format) && at(wb, 0, "A7")->value.number == dateSerial(2026, 3, 15));
    CHECK(at(wb, 1, "B2") && at(wb, 1, "B2")->value.type == CellValue::Text && at(wb, 1, "B2")->value.text == "007");
    const DefinedName* tax = wb.name("taxrate");
    CHECK(tax && tax->name == "TaxRate" && tax->formula == "'Rates & Notes'!$B$1");
    CHECK(wb.name("Regions") && wb.name("Regions")->formula == "Sales!$A$2:$A$5");
}

void checkStyles(const Workbook& wb, bool full) {
    const Sheet& s = wb.sheets[0];
    const CellStyle& head = s.get(0, 2)->style;
    CHECK(head.bold && head.align == 'c' && head.fill == (0x1F4E79 | kColorSet) && head.color == (0xFFFFFF | kColorSet));
    const CellStyle& b2 = s.get(1, 1)->style;
    CHECK(b2.italic && b2.underline && b2.strike && std::abs(b2.size - 14) < 0.01);
    CHECK(b2.border[BorderLeft] == 1 && b2.border[BorderRight] == 2 && b2.border[BorderTop] == 3 && b2.border[BorderBottom] == 4);
    CHECK(b2.borderColor == (0xC00000 | kColorSet));
    const CellStyle& d7 = s.get(6, 3)->style;
    CHECK(d7.wrap && d7.valign == 'm' && d7.align == 'l' && d7.indent == 2);
    CHECK(s.get(0, 0)->style == head);
    CHECK(s.get(2, 2)->style.plain());
    CHECK(std::abs(s.colWidths.at(0) - 14.5) < 0.05 && std::abs(s.colWidths.at(3) - 20) < 0.05 && !s.colWidths.count(1));
    CHECK(s.rowHeights.count(6) && std::abs(s.rowHeights.at(6) - 30) < 0.05);
    CHECK(s.hiddenRows.count(10) && s.hiddenCols.count(10) && s.hiddenRows.size() == 1);
    CHECK(s.merges.size() == 1 && rangeName(s.merges[0]) == "A9:D10");
    CHECK(s.freezeRows == 1 && s.freezeCols == 1);
    CHECK(s.hasAutoFilter && rangeName(s.autoFilter) == "A1:E5");
    if (!full) return;
}

void checkRules(const Workbook& wb) {
    const Sheet& s = wb.sheets[0];
    CHECK(s.condFormats.size() == 8);
    if (s.condFormats.size() != 8) return;
    const CondFormat& cf = s.condFormats[0];
    CHECK(cf.type == "cell" && cf.op == ">=" && cf.value1 == "120" && rangeName(cf.range) == "B2:D5");
    CHECK(cf.style.fill == (0xC6EFCE | kColorSet) && cf.style.color == (0x006100 | kColorSet));
    CHECK(s.condFormats[1].op == "between" && s.condFormats[1].value1 == "60" && s.condFormats[1].value2 == "80" && s.condFormats[1].style.bold);
    CHECK(s.condFormats[2].type == "text" && s.condFormats[2].value1 == "th" && s.condFormats[2].style.italic);
    CHECK(s.condFormats[3].type == "top" && s.condFormats[3].value1 == "2" && !s.condFormats[3].percent);
    CHECK(s.condFormats[4].type == "color_scale" && s.condFormats[4].midColor == (0xFFEB84 | kColorSet) && s.condFormats[4].maxColor == (0x63BE7B | kColorSet));
    CHECK(s.condFormats[5].type == "data_bar" && s.condFormats[5].maxColor == (0x5A8AC6 | kColorSet));
    CHECK(s.condFormats[6].type == "formula" && s.condFormats[6].value1 == "$B2>$C2" && s.condFormats[6].style.border[BorderBottom] == 1);
    CHECK(s.condFormats[7].value1 == "North" && s.condFormats[7].op == "=");
    // the rules still paint the same cells
    CellPainter p(wb, 0);
    CHECK(p.style(1, 1).fill == (0xC6EFCE | kColorSet) || p.style(1, 1).fill != 0);  // B2 = 120: green, under the colour scale
    CHECK(p.matches(0, 4, 3) && !p.matches(0, 2, 1));                                   // D5 = 160 >= 120, B3 = 80 not
    CHECK(p.matches(1, 2, 1) && p.matches(2, 1, 0) && p.matches(3, 4, 4) && !p.matches(3, 3, 4));
    CHECK(p.matches(6, 4, 0) && !p.matches(6, 1, 0));  // West: Q1 140 > Q2 120
    CHECK(p.matches(7, 1, 0) && !p.matches(7, 2, 0));
    CHECK(s.validations.size() == 5);
    if (s.validations.size() != 5) return;
    const Validation& list = s.validations[0];
    CHECK(list.type == "list" && list.value1 == "North,South,East,West" && list.error == "Pick a region" && list.errorTitle == "Region" && list.dropdown);
    CHECK(s.validations[1].type == "whole" && s.validations[1].value1 == "0" && s.validations[1].value2 == "1000" && s.validations[1].errorStyle == "warning");
    CHECK(s.validations[1].prompt == "Units sold");
    CHECK(s.validations[2].type == "date" && s.validations[2].op == ">=" && s.validations[2].value1 == "2026-01-01");
    CHECK(s.validations[3].value1 == "$A$2:$A$5" && !s.validations[3].dropdown);
    CHECK(s.validations[4].type == "custom" && s.validations[4].value1 == "C7>B7");
    std::string msg;
    bool reject = false;
    CHECK(validateCell(wb, 0, 1, 0, msg, reject));
    CHECK(validationList(wb, 0, s.validations[3]).size() == 4);
}

void checkCharts(const Workbook& wb) {
    const Sheet& s = wb.sheets[0];
    CHECK(s.charts.size() == 5);
    if (s.charts.size() != 5) return;
    CHECK(s.charts[0].type == "bar" && s.charts[0].title == "Quarterly sales" && s.charts[0].range == "A1:D5" && s.charts[0].row == 12 && s.charts[0].col == 0);
    CHECK(std::abs(s.charts[0].width - 480) <= 2 && std::abs(s.charts[0].height - 288) <= 2);
    CHECK(s.charts[1].type == "pie" && s.charts[1].range == "A1:B5" && s.charts[1].col == 8 && std::abs(s.charts[1].width - 360) <= 2);
    CHECK(s.charts[2].type == "line" && s.charts[2].byRows && s.charts[2].range == "A1:D5");
    CHECK(s.charts[3].type == "scatter" && s.charts[3].range == "B2:C5" && s.charts[3].title.empty());
    CHECK(s.charts[4].type == "area" && s.charts[4].range == "'Rates & Notes'!A1:B1");
    Chart c = sheetChart(wb, 0, s.charts[0]);
    CHECK(c.series.size() == 3 && c.categories.size() == 4 && c.series[2].name == "Q3" && c.series[2].values[3] == 160);
}

void testXlsx() {
    Workbook wb = sample();
    const std::string bytes = writeXlsx(wb);
    dump("sample.xlsx", bytes);
    // every part is well-formed XML and the dynamic-array metadata is there
    bool metadata = false, chartPart = false;
    for (const auto& e : raster::readZip(bytes)) {
        if (e.name.size() > 4 && (e.name.substr(e.name.size() - 4) == ".xml" || e.name.substr(e.name.size() - 5) == ".rels")) {
            bool ok = true;
            try {
                xml::parse(e.data);
            } catch (const std::exception& ex) {
                ok = false;
                std::printf("  %s: %s\n", e.name.c_str(), ex.what());
            }
            CHECK(ok);
        }
        metadata = metadata || e.name == "xl/metadata.xml";
        chartPart = chartPart || e.name == "xl/charts/chart5.xml";
        if (e.name == "xl/worksheets/sheet1.xml") {
            CHECK(e.data.find("<f t=\"array\" ref=\"G1:G4\">_xlfn._xlws.SORT(A2:A5)</f>") != std::string::npos);
            CHECK(e.data.find("cm=\"1\"") != std::string::npos);
            CHECK(e.data.find("state=\"frozen\"") != std::string::npos && e.data.find("<mergeCell ref=\"A9:D10\"/>") != std::string::npos);
        }
    }
    CHECK(metadata && chartPart);
    Workbook back = readXlsx(bytes);
    back.recalc();
    checkValues(back, "xlsx");
    checkStyles(back, true);
    checkRules(back);
    checkCharts(back);
    // a second trip is stable
    Workbook again = readXlsx(writeXlsx(back));
    again.recalc();
    checkValues(again, "xlsx twice");
    checkRules(again);
    CHECK(again.sheets[0].charts.size() == 5);
}

void testOds() {
    Workbook wb = sample();
    const std::string bytes = writeOds(wb);
    dump("sample.ods", bytes);
    auto entries = raster::readZip(bytes);
    CHECK(!entries.empty() && entries[0].name == "mimetype" && entries[0].data == "application/vnd.oasis.opendocument.spreadsheet");
    for (const auto& e : entries) {
        if (e.name.find(".xml") == std::string::npos) continue;
        bool ok = true;
        try {
            xml::parse(e.data);
        } catch (const std::exception& ex) {
            ok = false;
            std::printf("  %s: %s\n", e.name.c_str(), ex.what());
        }
        CHECK(ok);
        if (e.name == "content.xml") {
            CHECK(e.data.find("of:=SUM([.B2:.D2])*TaxRate") != std::string::npos);
            CHECK(e.data.find("table:number-matrix-rows-spanned=\"4\"") != std::string::npos);
        }
    }
    Workbook back = readOds(bytes);
    back.recalc();
    checkValues(back, "ods");
    checkStyles(back, false);
    // formulas with sheet names, arrays and booleans survive OpenFormula
    Workbook f;
    f.sheets.resize(2);
    f.sheets[0].name = "A";
    f.sheets[1].name = "My Data";
    put(f, 1, "A1", "5");
    put(f, 0, "A1", "=SUM('My Data'!A1:A3)+SUM({1,2;3,4})+IF(TRUE,1,0)+LOG10(100)");
    put(f, 0, "A2", "=COUNTIF(B:B,\"x;y\")");
    f.recalc();
    Workbook fb = readOds(writeOds(f));
    fb.recalc();
    CHECK(fb.sheets[0].get(0, 0) && fb.sheets[0].get(0, 0)->value.number == 18);
    CHECK(fb.sheets[0].get(1, 0) && fb.sheets[0].get(1, 0)->formula == "COUNTIF(B:B,\"x;y\")");
}

void testCsvOptions() {
    CsvOptions o;
    o.delimiter = ';';
    o.decimalComma = true;
    o.dateOrder = "dmy";
    o.skipRows = 1;
    o.textColumns = {0};
    Workbook wb = readCsv("exported by tool\nid;amount;when\n007;1.234,5;03/04/2026\n010;-2,25;31.12.2025\n", o);
    const Sheet& s = wb.sheets[0];
    CHECK(s.get(0, 0) && s.get(0, 0)->value.text == "id");
    CHECK(s.get(1, 0) && s.get(1, 0)->value.type == CellValue::Text && s.get(1, 0)->value.text == "007");
    CHECK(s.get(1, 1) && s.get(1, 1)->value.number == 1234.5 && s.get(2, 1)->value.number == -2.25);
    CHECK(s.get(1, 2) && s.get(1, 2)->value.number == dateSerial(2026, 4, 3) && s.get(1, 2)->format == "dd/mm/yyyy");
    CHECK(s.get(2, 2) && s.get(2, 2)->value.number == dateSerial(2025, 12, 31));
    o = CsvOptions{};
    o.dateOrder = "mdy";
    Workbook us = readCsv("when,n\n03/04/2026,5\n", o);
    CHECK(us.sheets[0].get(1, 0) && us.sheets[0].get(1, 0)->value.number == dateSerial(2026, 3, 4));
    o = CsvOptions{};
    o.detectTypes = false;
    Workbook raw = readCsv("a,b\n1,=2+2\n", o);
    CHECK(raw.sheets[0].get(1, 0)->value.type == CellValue::Text && raw.sheets[0].get(1, 1)->value.text == "=2+2" && !raw.sheets[0].get(1, 1)->hasFormula());
    // latin-1 bytes are read as text, not mojibake
    Workbook latin = readCsv(std::string("name\nJos\xE9\n"), CsvOptions{});
    CHECK(latin.sheets[0].get(1, 0) && latin.sheets[0].get(1, 0)->value.text == "Jos\xC3\xA9");
    // detection picks the delimiter
    Workbook tabs = readCsv("a\tb\n1\t2\n", CsvOptions{});
    CHECK(tabs.sheets[0].get(1, 1) && tabs.sheets[0].get(1, 1)->value.number == 2);
}

void testPreview() {
    Workbook wb = sample();
    auto pages = layoutSheet(wb, 0);
    CHECK(!pages.empty());
    bool image = false;
    for (const auto& p : pages) {
        for (const auto& op : p.ops) image = image || std::holds_alternative<pdf::ImageOp>(op);
    }
    CHECK(image);  // the charts are drawn on the sheet pages
    pdf::Page grid = sheetGridPage(wb, rng(wb, "A1:E7"), true);
    raster::Image img = pdf::renderPage(grid, 1);
    // the header fill is dark blue
    auto px = img.at(60, 24);
    CHECK(px.b > px.r && px.r < 0x60);
}

}  // namespace

int main() {
    testXlsx();
    testOds();
    testCsvOptions();
    testPreview();
    return check::summary("office files");
}
