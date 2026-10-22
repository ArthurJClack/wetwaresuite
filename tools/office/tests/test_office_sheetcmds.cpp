// The office.sheet_* commands added for Excel-style work: styles, merges, panes,
// names, conditional formats, validation, charts, pivots, copy/move, autofilter,
// import and fill series. Headless; needs no LibreOffice.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

#include "wetwaresuite/office/office.hpp"
#include "wetwaresuite/office/sheet.hpp"
#include "check.hpp"
#include "commands.hpp"
#include "sheet_internal.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::office;
namespace fs = std::filesystem;

namespace {

fs::path tmpDir() {
    static const fs::path dir = fs::temp_directory_path() / ("wetwaresuite_sheetcmd_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    return dir;
}

std::string txt(const Result& r) { return toText(r.value); }
Params P(std::initializer_list<std::pair<const std::string, Value>> l) { return Params(l); }
Value S(const char* s) { return std::string(s); }

Workbook& wb(Session& s, const std::string& id = "sheet1") { return s.workspace().getAs<SheetDoc>(id)->data; }

const Cell* cell(Session& s, const std::string& ref) {
    Workbook& w = wb(s);
    Range r;
    std::string err;
    if (!parseRange(w, ref, w.active, r, err)) return nullptr;
    return w.sheets[static_cast<std::size_t>(r.sheet)].get(r.r1, r.c1);
}

std::string get(Session& s, const std::string& ref) { return txt(s.run("office.sheet_get", P({{"range", ref}}))); }

Result run(Session& s, const std::string& id, Params p) {
    Result r = s.run(id, p);
    if (!r.ok) std::printf("  %s failed: %s\n", id.c_str(), r.message.c_str());
    return r;
}

void testStyleCommands() {
    Session s;
    registerCommands(s);
    run(s, "office.sheet_new", P({{"csv", S("Region,Q1,Q2\nNorth,100,120\nSouth,50,40\nEast,75,90")}}));
    Result r = run(s, "office.sheet_style", P({{"range", S("A1:C1")}, {"bold", true}, {"fill", S("lightblue")}, {"color", S("#112233")}, {"align", S("center")}, {"border", S("bottom")}, {"border_style", S("double")}}));
    CHECK(r.ok);
    const Cell* a1 = cell(s, "A1");
    CHECK(a1 && a1->style.bold && a1->style.align == 'c' && (a1->style.color & 0xFFFFFF) == 0x112233 && a1->style.border[3] == 4);
    r = run(s, "office.sheet_style", P({{"range", S("A2:A4")}, {"like", S("A1")}}));
    CHECK(r.ok && cell(s, "A3")->style.bold);
    r = run(s, "office.sheet_style", P({{"range", S("A2:A4")}, {"clear", true}, {"italic", true}}));
    CHECK(r.ok && !cell(s, "A3")->style.bold && cell(s, "A3")->style.italic);
    r = s.run("office.sheet_style", P({{"range", S("A1")}, {"fill", S("notacolour")}}));
    CHECK(!r.ok);
    r = run(s, "office.sheet_get", P({{"range", S("A1")}}));
    CHECK(r.message.find("bold") != std::string::npos);
    CHECK(s.history().undo() && !cell(s, "A3")->style.italic && cell(s, "A3")->style.bold);

    r = run(s, "office.sheet_merge", P({{"range", S("E1:G1")}}));
    CHECK(r.ok && wb(s).sheets[0].merges.size() == 1);
    r = run(s, "office.sheet_get", P({{"range", S("E1")}}));
    CHECK(r.message.find("merged E1:G1") != std::string::npos);
    r = run(s, "office.sheet_merge", P({{"range", S("E1:G1")}, {"unmerge", true}}));
    CHECK(r.ok && wb(s).sheets[0].merges.empty());
    r = run(s, "office.sheet_merge", P({{"range", S("E2:F3")}, {"across", true}}));
    CHECK(r.ok && wb(s).sheets[0].merges.size() == 2);

    // what the window's grid sees: styles, merges and conditional formats
    {
        auto d = s.workspace().getAs<SheetDoc>("sheet1");
        CellGrid layout;
        CHECK(d->grid(layout));
        GridCell g;
        CHECK(d->gridCell(0, 0, 0, g) && g.bold && g.align == 'c' && (g.color & 0xFFFFFF) == 0x112233 && (g.fill & 0x1000000));
        GridCell top, inside;
        CHECK(d->gridCell(0, 1, 4, top) && top.spanCols == 2 && top.spanRows == 1);
        CHECK(d->gridCell(0, 1, 5, inside) && inside.covered);
        run(s, "office.sheet_cond_format", P({{"range", S("B2:B4")}, {"op", S(">")}, {"value", S("60")}, {"fill", S("#00ff00")}}));
        CHECK(d->grid(layout));
        GridCell hit, miss;
        CHECK(d->gridCell(0, 1, 1, hit) && (hit.fill & 0xFFFFFF) == 0x00FF00);
        CHECK(d->gridCell(0, 2, 1, miss) && !(miss.fill & 0x1000000));
        CHECK(s.history().undo());
    }

    r = run(s, "office.sheet_freeze", P({{"at", S("B2")}}));
    CHECK(r.ok && wb(s).sheets[0].freezeRows == 1 && wb(s).sheets[0].freezeCols == 1);
    r = run(s, "office.sheet_freeze", P({{"none", true}}));
    CHECK(r.ok && wb(s).sheets[0].freezeRows == 0);

    r = run(s, "office.sheet_hide", P({{"rows", S("3:4")}}));
    CHECK(r.ok && wb(s).sheets[0].hiddenRows.count(2) && wb(s).sheets[0].hiddenRows.count(3));
    r = run(s, "office.sheet_hide", P({{"cols", S("C")}}));
    CHECK(r.ok && wb(s).sheets[0].hiddenCols.count(2));
    r = run(s, "office.sheet_hide", P({{"rows", S("3:4")}, {"hidden", false}}));
    CHECK(r.ok && wb(s).sheets[0].hiddenRows.empty());

    r = run(s, "office.sheet_row_height", P({{"rows", S("1")}, {"height", 30.0}}));
    CHECK(r.ok && std::abs(wb(s).sheets[0].rowHeights[0] - 30.0) < 1e-9);

    r = run(s, "office.sheet_name", P({{"name", S("TaxRate")}, {"refers_to", S("0.2")}}));
    CHECK(r.ok);
    r = run(s, "office.sheet_name", P({{"name", S("Sales")}, {"refers_to", S("B2:B4")}}));
    CHECK(r.ok);
    r = run(s, "office.sheet_set", P({{"range", S("E5")}, {"value", S("=SUM(Sales)*TaxRate")}}));
    CHECK(r.ok && txt(r) == "45");
    r = run(s, "office.sheet_names", {});
    CHECK(r.ok && r.message.find("TaxRate") != std::string::npos && r.message.find("Sales") != std::string::npos);
    r = s.run("office.sheet_name", P({{"name", S("A1")}, {"refers_to", S("1")}}));
    CHECK(!r.ok);  // looks like a cell reference
    r = run(s, "office.sheet_name", P({{"name", S("TaxRate")}, {"delete", true}}));
    CHECK(r.ok && get(s, "E5") == "#NAME?");
}

void testRuleCommands() {
    Session s;
    registerCommands(s);
    run(s, "office.sheet_new", P({{"csv", S("Item,Qty\nA,5\nB,150\nC,20\nD,300")}}));
    Result r = run(s, "office.sheet_cond_format", P({{"range", S("B2:B5")}, {"op", S(">")}, {"value", S("100")}, {"fill", S("lightgreen")}}));
    CHECK(r.ok && r.message.find("2 cell(s) match") != std::string::npos);
    r = run(s, "office.sheet_cond_format", P({{"range", S("B2:B5")}, {"type", S("color_scale")}}));
    CHECK(r.ok);
    r = run(s, "office.sheet_cond_format", P({{"range", S("A2:A5")}, {"type", S("formula")}, {"value", S("$B2<10")}, {"bold", true}}));
    CHECK(r.ok && r.message.find("1 cell(s) match") != std::string::npos);
    r = s.run("office.sheet_cond_format", P({{"range", S("A2:A5")}, {"type", S("formula")}, {"value", S("SUM(")}}));
    CHECK(!r.ok);
    CHECK(wb(s).sheets[0].condFormats.size() == 3);
    r = run(s, "office.sheet_cond_formats", {});
    CHECK(r.ok && r.message.find("B2:B5") != std::string::npos);
    r = run(s, "office.sheet_cond_clear", P({{"rule", std::int64_t{2}}}));
    CHECK(r.ok && wb(s).sheets[0].condFormats.size() == 2);
    r = run(s, "office.sheet_cond_clear", P({{"range", S("A3")}}));
    CHECK(r.ok && wb(s).sheets[0].condFormats.size() == 1);

    r = run(s, "office.sheet_validate", P({{"range", S("B2:B10")}, {"type", S("whole")}, {"op", S("between")}, {"value", S("0")}, {"value2", S("200")}}));
    CHECK(r.ok && r.message.find("D5") == std::string::npos && r.message.find("B5") != std::string::npos);  // 300 already breaks it
    r = s.run("office.sheet_set", P({{"range", S("B6")}, {"value", S("500")}}));
    CHECK(!r.ok && cell(s, "B6") == nullptr);  // stop rule rejects, nothing written
    r = run(s, "office.sheet_set", P({{"range", S("B6")}, {"value", S("50")}}));
    CHECK(r.ok);
    r = run(s, "office.sheet_validate", P({{"range", S("C2:C10")}, {"type", S("list")}, {"value", S("yes,no")}, {"error_style", S("warning")}}));
    CHECK(r.ok);
    r = run(s, "office.sheet_set", P({{"range", S("C2")}, {"value", S("maybe")}}));
    CHECK(r.ok && get(s, "C2") == "maybe");  // warning: accepted with a note
    r = run(s, "office.sheet_validations", {});
    CHECK(r.ok && r.message.find("C2:C10") != std::string::npos);
    r = run(s, "office.sheet_get", P({{"range", S("C2")}}));
    CHECK(r.message.find("validation list") != std::string::npos && r.message.find("invalid") != std::string::npos);
    r = run(s, "office.sheet_validation_clear", P({{"range", S("B2")}}));
    CHECK(r.ok && wb(s).sheets[0].validations.size() == 1);
    r = run(s, "office.sheet_validation_clear", {});
    CHECK(r.ok && wb(s).sheets[0].validations.empty());
}

void testObjectCommands() {
    Session s;
    registerCommands(s);
    run(s, "office.sheet_new", P({{"csv", S("Region,Product,Sales\nNorth,Pens,10\nSouth,Pens,20\nNorth,Ink,5\nNorth,Pens,7\nSouth,Ink,3")}}));
    Result r = run(s, "office.sheet_chart_add", P({{"range", S("B1:C6")}, {"type", S("pie")}, {"title", S("Share")}}));
    CHECK(r.ok && wb(s).sheets[0].charts.size() == 1 && wb(s).sheets[0].charts[0].col == 4);
    r = run(s, "office.sheet_chart_edit", P({{"type", S("line")}, {"at", S("H2")}}));
    CHECK(r.ok && wb(s).sheets[0].charts[0].type == "line" && wb(s).sheets[0].charts[0].row == 1);
    r = run(s, "office.sheet_charts", P({{"chart", std::int64_t{1}}}));
    CHECK(r.ok && r.message.find("line") != std::string::npos);
    r = s.run("office.sheet_chart_edit", P({{"chart", std::int64_t{5}}, {"type", S("bar")}}));
    CHECK(!r.ok);
    r = run(s, "office.sheet_chart_delete", {});
    CHECK(r.ok && wb(s).sheets[0].charts.empty());

    r = run(s, "office.sheet_pivot", P({{"range", S("A1:C6")}, {"rows", S("Region")}, {"columns", S("Product")}, {"values", S("Sales")}}));
    CHECK(r.ok && wb(s).sheets.size() == 2);
    const Sheet& pv = wb(s).sheets[1];
    // labels sorted: rows North, South; columns Ink, Pens, then Total
    auto at = [&](int row, int col) -> std::string {
        const Cell* c = pv.get(row, col);
        return c ? (c->value.type == CellValue::Number ? generalNumber(c->value.number) : c->value.text) : std::string();
    };
    CHECK(at(1, 0) == "North" && at(2, 0) == "South");
    CHECK(at(0, 1) == "Ink" && at(0, 2) == "Pens");
    CHECK(at(1, 1) == "5" && at(1, 2) == "17" && at(2, 1) == "3" && at(2, 2) == "20");
    CHECK(at(3, 3) == "45");  // grand total
    CHECK(pv.get(1, 2) && pv.get(1, 2)->hasFormula());  // live formulas by default
    // editing the source updates the pivot
    run(s, "office.sheet_set", P({{"range", S("Sheet1!C2")}, {"value", S("100")}}));
    CHECK(at(1, 2) == "107");
    r = run(s, "office.sheet_pivot", P({{"range", S("Sheet1!A1:C6")}, {"rows", S("Product")}, {"function", S("count")}, {"to", S("Sheet1!F1")}, {"static", true}}));
    CHECK(r.ok);
    CHECK(get(s, "Sheet1!F2") == "Ink" && get(s, "Sheet1!G2") == "2" && get(s, "Sheet1!G3") == "3");
    CHECK(!cell(s, "Sheet1!G2")->hasFormula());
}

void testDataCommands() {
    Session s;
    registerCommands(s);
    run(s, "office.sheet_new", P({{"csv", S("A,B\n1,2\n3,4")}}));
    run(s, "office.sheet_set", P({{"range", S("C2:C3")}, {"value", S("=A2+B2")}}));
    run(s, "office.sheet_style", P({{"range", S("A2")}, {"bold", true}}));
    Result r = run(s, "office.sheet_copy", P({{"from", S("A1:C3")}, {"to", S("E1")}}));
    CHECK(r.ok && get(s, "G3") == "7" && cell(s, "G3")->formula == "E3+F3" && cell(s, "E2")->style.bold);
    r = run(s, "office.sheet_copy", P({{"from", S("C2:C3")}, {"to", S("I1")}, {"what", S("values")}}));
    CHECK(r.ok && get(s, "I1") == "3" && !cell(s, "I1")->hasFormula());
    r = run(s, "office.sheet_copy", P({{"from", S("A1:B2")}, {"to", S("K1")}, {"transpose", true}}));
    CHECK(r.ok && get(s, "K1") == "A" && get(s, "L1") == "1" && get(s, "K2") == "B" && get(s, "L2") == "2");
    r = run(s, "office.sheet_copy", P({{"from", S("A2")}, {"to", S("M5")}, {"what", S("formats")}}));
    CHECK(r.ok && cell(s, "M5")->style.bold && !cell(s, "M5")->hasContent());

    // move keeps references pointing at the moved cells
    run(s, "office.sheet_set", P({{"range", S("A10")}, {"value", S("=SUM(A2:B3)")}}));
    r = run(s, "office.sheet_move", P({{"from", S("A1:C3")}, {"to", S("A20")}}));
    CHECK(r.ok && get(s, "C22") == "7" && cell(s, "C22")->formula == "A22+B22");
    CHECK(cell(s, "A10")->formula == "SUM(A21:B22)" && get(s, "A10") == "10");
    CHECK(cell(s, "A2") == nullptr);
    CHECK(s.history().undo());
    CHECK(get(s, "C3") == "7" && cell(s, "A10")->formula == "SUM(A2:B3)");

    run(s, "office.sheet_add", P({{"name", S("T")}, {"csv", S("Name,Score\nAnn,90\nBob,40\nCid,75\nDee,20")}}));
    r = run(s, "office.sheet_autofilter", P({{"column", S("Score")}, {"criteria", S(">=50")}}));
    CHECK(r.ok && txt(r) == "2");
    const Sheet& t = wb(s).sheets[static_cast<std::size_t>(wb(s).sheetIndex("T"))];
    CHECK(t.hasAutoFilter && t.hiddenRows.count(2) && t.hiddenRows.count(4) && !t.hiddenRows.count(1));
    r = run(s, "office.sheet_autofilter", P({{"clear", true}}));
    CHECK(r.ok && t.hiddenRows.empty() && !t.hasAutoFilter);

    r = run(s, "office.sheet_import", P({{"csv", S("title line\nid;amount\n007;1.234,5\n010;2,25")}, {"name", S("Imp")}, {"skip_rows", std::int64_t{1}},
                                         {"decimal_comma", true}, {"text_columns", S("A")}}));
    CHECK(r.ok);
    CHECK(get(s, "Imp!A2") == "007" && get(s, "Imp!B2") == "1234.5" && get(s, "Imp!B3") == "2.25");
    const fs::path f = tmpDir() / "people.csv";
    {
        std::ofstream out(f, std::ios::binary);
        out << "who,when\nZoe,03/04/2026\n";
    }
    r = run(s, "office.sheet_import", P({{"path", f.string()}, {"at", S("T!D1")}, {"date_order", S("dmy")}}));
    CHECK(r.ok && get(s, "T!D2") == "Zoe");
    const Cell* when = cell(s, "T!E2");
    CHECK(when && when->value.type == CellValue::Number && std::abs(when->value.number - dateSerial(2026, 4, 3)) < 1e-9);
    r = run(s, "office.sheet_open", P({{"path", f.string()}, {"date_order", S("mdy")}}));
    CHECK(r.ok);
    fs::remove_all(tmpDir());
}

std::string fillValue(const std::vector<Cell>& out, std::size_t k) {
    if (k >= out.size()) return "?";
    const CellValue& v = out[k].value;
    return v.type == CellValue::Number ? generalNumber(v.number) : v.text;
}

std::vector<Cell> cells(std::initializer_list<const char*> inputs, const std::string& format = "") {
    std::vector<Cell> v;
    for (const char* in : inputs) {
        Cell c;
        setInput(c, in);
        if (!format.empty()) c.format = format;
        v.push_back(c);
    }
    return v;
}

void testFillSeries() {
    std::vector<Cell> out;
    std::vector<int> from;
    std::string err;
    FillSpec auto_;
    CHECK(fillSeries(cells({"1", "3"}), 3, auto_, out, from, err) && fillValue(out, 0) == "5" && fillValue(out, 2) == "9");
    CHECK(fillSeries(cells({"1", "2", "4"}), 1, auto_, out, from, err) && fillValue(out, 0) == "5.333333333");  // least-squares trend
    CHECK(fillSeries(cells({"7"}), 2, auto_, out, from, err) && fillValue(out, 1) == "7" && from[1] == 0);  // one number copies
    FillSpec lin;
    lin.series = "linear";
    CHECK(fillSeries(cells({"7"}), 2, lin, out, from, err) && fillValue(out, 1) == "9");
    FillSpec grow;
    grow.series = "growth";
    CHECK(fillSeries(cells({"2", "6"}), 2, grow, out, from, err) && fillValue(out, 0) == "18" && fillValue(out, 1) == "54");
    CHECK(fillSeries(cells({"Item 9"}), 2, auto_, out, from, err) && fillValue(out, 0) == "Item 10" && fillValue(out, 1) == "Item 11");
    CHECK(fillSeries(cells({"file-008.txt", "file-010.txt"}), 1, auto_, out, from, err) && fillValue(out, 0) == "file-012.txt");
    CHECK(fillSeries(cells({"Nov"}), 3, auto_, out, from, err) && fillValue(out, 0) == "Dec" && fillValue(out, 1) == "Jan" && fillValue(out, 2) == "Feb");
    CHECK(fillSeries(cells({"MONDAY", "WEDNESDAY"}), 2, auto_, out, from, err) && fillValue(out, 0) == "FRIDAY" && fillValue(out, 1) == "SUNDAY");
    CHECK(fillSeries(cells({"x", "y"}), 3, auto_, out, from, err) && fillValue(out, 2) == "x" && from[2] == 0);
    // dates: one date counts days; month-apart dates keep stepping by months (clamped to month end)
    auto dates = cells({"2026-01-31"});
    CHECK(fillSeries(dates, 1, auto_, out, from, err) && std::abs(out[0].value.number - dateSerial(2026, 2, 1)) < 1e-9);
    dates = cells({"2026-01-31", "2026-02-28"});
    CHECK(fillSeries(dates, 2, auto_, out, from, err) && std::abs(out[0].value.number - dateSerial(2026, 3, 31)) < 1e-9 &&
          std::abs(out[1].value.number - dateSerial(2026, 4, 30)) < 1e-9);
    FillSpec wd;
    wd.unit = "weekday";
    dates = cells({"2026-10-16"});  // a Friday
    CHECK(fillSeries(dates, 2, wd, out, from, err) && std::abs(out[0].value.number - dateSerial(2026, 10, 19)) < 1e-9 &&
          std::abs(out[1].value.number - dateSerial(2026, 10, 20)) < 1e-9);
    FillSpec yr;
    yr.unit = "year";
    CHECK(fillSeries(cells({"2024-02-29"}), 1, yr, out, from, err) && std::abs(out[0].value.number - dateSerial(2025, 2, 28)) < 1e-9);
    FillSpec stepped;
    stepped.hasStep = true;
    stepped.step = 2;
    CHECK(!fillSeries(cells({"=A1"}), 2, stepped, out, from, err));

    Session s;
    registerCommands(s);
    run(s, "office.sheet_new", {});
    run(s, "office.sheet_set", P({{"range", S("A1")}, {"value", S("Q1")}}));
    run(s, "office.sheet_set", P({{"range", S("B1")}, {"value", S("=A1&\"!\"")}}));
    Result r = run(s, "office.sheet_fill", P({{"range", S("A1:B4")}, {"direction", S("down")}}));
    CHECK(r.ok && get(s, "A4") == "Q4" && get(s, "B4") == "Q4!");
    run(s, "office.sheet_set", P({{"range", S("D1")}, {"value", S("10")}}));
    run(s, "office.sheet_set", P({{"range", S("D2")}, {"value", S("20")}}));
    r = run(s, "office.sheet_fill", P({{"range", S("D1:D5")}, {"source", std::int64_t{2}}}));
    CHECK(r.ok && get(s, "D5") == "50");
    r = run(s, "office.sheet_fill", P({{"range", S("D1:H1")}, {"series", S("copy")}}));
    CHECK(r.ok && get(s, "H1") == "10");
}

}  // namespace

int main() {
    testStyleCommands();
    testRuleCommands();
    testObjectCommands();
    testDataCommands();
    testFillSeries();
    return check::summary("office sheet commands");
}
