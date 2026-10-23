// The formula library: results checked against what Excel gives.
#include <cmath>
#include <cstdio>
#include <string>

#include "wetwaresuite/office/sheet.hpp"
#include "check.hpp"
#include "sheet_internal.hpp"

using namespace wetwaresuite::office;

namespace {

Workbook book() {
    Workbook wb;
    wb.sheets.emplace_back();
    wb.sheets[0].name = "Sheet1";
    return wb;
}

void put(Workbook& wb, const std::string& ref, const std::string& input) {
    Range r;
    std::string err;
    if (!parseRange(wb, ref, 0, r, err)) {
        std::printf("bad ref %s: %s\n", ref.c_str(), err.c_str());
        return;
    }
    setInput(wb.sheets[static_cast<std::size_t>(r.sheet)].at(r.r1, r.c1), input);
}

CellValue val(const Workbook& wb, const std::string& ref) {
    Range r;
    std::string err;
    if (!parseRange(wb, ref, 0, r, err)) return CellValue::error("bad ref");
    const Cell* c = wb.sheets[static_cast<std::size_t>(r.sheet)].get(r.r1, r.c1);
    return c ? c->value : CellValue{};
}

std::string show(const CellValue& v) {
    switch (v.type) {
        case CellValue::Number: {
            char b[64];
            std::snprintf(b, sizeof b, "%.12g", v.number);
            return b;
        }
        case CellValue::Bool: return v.number ? "TRUE" : "FALSE";
        case CellValue::Text: return "\"" + v.text + "\"";
        case CellValue::Error: return v.text;
        default: return "(empty)";
    }
}

bool numIs(const Workbook& wb, const std::string& f, double want, double tol = 1e-9) {
    CellValue v = wb.evaluate(f, 0, 0, 0);
    const bool ok = v.type == CellValue::Number && std::abs(v.number - want) <= tol * std::max(1.0, std::abs(want));
    if (!ok) std::printf("  =%s gave %s, want %.12g\n", f.c_str(), show(v).c_str(), want);
    return ok;
}

bool textIs(const Workbook& wb, const std::string& f, const std::string& want) {
    CellValue v = wb.evaluate(f, 0, 0, 0);
    const bool ok = v.type == CellValue::Text && v.text == want;
    if (!ok) std::printf("  =%s gave %s, want \"%s\"\n", f.c_str(), show(v).c_str(), want.c_str());
    return ok;
}

bool boolIs(const Workbook& wb, const std::string& f, bool want) {
    CellValue v = wb.evaluate(f, 0, 0, 0);
    const bool ok = v.type == CellValue::Bool && (v.number != 0) == want;
    if (!ok) std::printf("  =%s gave %s, want %s\n", f.c_str(), show(v).c_str(), want ? "TRUE" : "FALSE");
    return ok;
}

bool errIs(const Workbook& wb, const std::string& f, const std::string& code) {
    CellValue v = wb.evaluate(f, 0, 0, 0);
    const bool ok = v.isError() && v.text == code;
    if (!ok) std::printf("  =%s gave %s, want %s\n", f.c_str(), show(v).c_str(), code.c_str());
    return ok;
}

// A small table: A1:C6 with a header, regions, amounts and dates.
Workbook data() {
    Workbook wb = book();
    const char* rows[][4] = {{"Region", "Amount", "Date", "Rep"},     {"North", "100", "2026-01-15", "Ann"}, {"South", "250", "2026-02-03", "Bob"},
                             {"North", "75", "2026-02-20", "Cy"},    {"East", "300", "2026-03-01", "Ann"},  {"South", "50", "2026-03-15", "Bob"}};
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 4; ++c) put(wb, cellName(r, c), rows[r][c]);
    }
    wb.recalc();
    return wb;
}

void testMath() {
    Workbook wb = data();
    CHECK(numIs(wb, "SUM(B2:B6)", 775));
    CHECK(numIs(wb, "SUMPRODUCT(B2:B6,(A2:A6=\"North\")*1)", 175));
    CHECK(numIs(wb, "SUMPRODUCT((A2:A6=\"South\")*(B2:B6))", 300));
    CHECK(numIs(wb, "SUMSQ(3,4)", 25));
    CHECK(numIs(wb, "PRODUCT(2,3,4)", 24));
    CHECK(numIs(wb, "ROUND(2.675,2)", 2.68));
    CHECK(numIs(wb, "ROUND(-1.5,0)", -2));
    CHECK(numIs(wb, "ROUND(1234.5,-2)", 1200));
    CHECK(numIs(wb, "ROUNDUP(3.141,2)", 3.15));
    CHECK(numIs(wb, "ROUNDDOWN(-3.149,2)", -3.14));
    CHECK(numIs(wb, "TRUNC(8.9)", 8) && numIs(wb, "TRUNC(-8.9)", -8) && numIs(wb, "INT(-8.9)", -9));
    CHECK(numIs(wb, "MROUND(10,3)", 9) && numIs(wb, "MROUND(-10,-3)", -9));
    CHECK(errIs(wb, "MROUND(5,-2)", "#NUM!"));
    CHECK(numIs(wb, "CEILING(2.5,1)", 3) && numIs(wb, "CEILING(-2.5,-2)", -4) && numIs(wb, "CEILING(-2.5,2)", -2));
    CHECK(numIs(wb, "FLOOR(2.5,1)", 2) && numIs(wb, "FLOOR(-2.5,-2)", -2) && numIs(wb, "FLOOR(-2.5,2)", -4));
    CHECK(numIs(wb, "CEILING.MATH(-5.5)", -5) && numIs(wb, "CEILING.MATH(-5.5,2,-1)", -6) && numIs(wb, "CEILING.MATH(24.3,5)", 25));
    CHECK(numIs(wb, "FLOOR.MATH(-5.5)", -6) && numIs(wb, "FLOOR.MATH(-5.5,2,-1)", -4) && numIs(wb, "FLOOR.MATH(24.3,5)", 20));
    CHECK(numIs(wb, "MOD(-3,2)", 1) && numIs(wb, "MOD(3,-2)", -1) && numIs(wb, "QUOTIENT(-10,3)", -3));
    CHECK(numIs(wb, "SIGN(-4)", -1) && numIs(wb, "ABS(-4)", 4));
    CHECK(numIs(wb, "POWER(2,10)", 1024) && numIs(wb, "SQRT(16)", 4) && errIs(wb, "SQRT(-1)", "#NUM!"));
    CHECK(numIs(wb, "EXP(1)", 2.718281828459045) && numIs(wb, "LN(EXP(2))", 2) && numIs(wb, "LOG(8,2)", 3) && numIs(wb, "LOG10(1000)", 3));
    CHECK(numIs(wb, "LOG(100)", 2) && errIs(wb, "LN(0)", "#NUM!"));
    CHECK(numIs(wb, "FACT(5)", 120) && numIs(wb, "COMBIN(8,2)", 28) && numIs(wb, "PERMUT(5,2)", 20) && numIs(wb, "FACTDOUBLE(7)", 105));
    CHECK(numIs(wb, "GCD(24,36)", 12) && numIs(wb, "LCM(4,6)", 12));
    CHECK(numIs(wb, "EVEN(1.5)", 2) && numIs(wb, "EVEN(-1)", -2) && numIs(wb, "ODD(2)", 3) && numIs(wb, "ODD(-1.5)", -3));
    CHECK(numIs(wb, "PI()", 3.14159265358979) && numIs(wb, "DEGREES(PI())", 180) && numIs(wb, "RADIANS(90)", 1.5707963267949));
    CHECK(numIs(wb, "SIN(PI()/2)", 1) && numIs(wb, "COS(0)", 1) && numIs(wb, "TAN(PI()/4)", 1) && numIs(wb, "ATAN2(1,1)", 0.785398163397448));
    CHECK(numIs(wb, "ASIN(1)", 1.5707963267949) && errIs(wb, "ACOS(2)", "#NUM!"));
    CHECK(numIs(wb, "SQRTPI(1)", 1.77245385090552));
    CHECK(textIs(wb, "ROMAN(1994)", "MCMXCIV") && numIs(wb, "ARABIC(\"MCMXCIV\")", 1994));
    CHECK(textIs(wb, "DEC2BIN(10)", "1010") && textIs(wb, "DEC2HEX(255)", "FF") && numIs(wb, "HEX2DEC(\"FF\")", 255) && textIs(wb, "BASE(7,2,4)", "0111"));
    // RAND stays in range
    for (int i = 0; i < 5; ++i) {
        CellValue r = wb.evaluate("RAND()", 0);
        CHECK(r.type == CellValue::Number && r.number >= 0 && r.number < 1);
        CellValue b = wb.evaluate("RANDBETWEEN(3,5)", 0);
        CHECK(b.type == CellValue::Number && b.number >= 3 && b.number <= 5 && b.number == std::floor(b.number));
    }
}

void testSubtotal() {
    Workbook wb = data();
    put(wb, "B7", "=SUBTOTAL(9,B2:B6)");
    put(wb, "B8", "=SUBTOTAL(9,B2:B7)");  // ignores the other subtotal
    wb.recalc();
    CHECK(val(wb, "B7").number == 775 && val(wb, "B8").number == 775);
    wb.sheets[0].hiddenRows.insert(1);  // row 2 hidden, e.g. by a filter
    wb.recalc();
    CHECK(val(wb, "B7").number == 675);
    CHECK(numIs(wb, "SUBTOTAL(1,B2:B6)", 168.75) && numIs(wb, "SUBTOTAL(2,B2:B6)", 4) && numIs(wb, "SUBTOTAL(104,B2:B6)", 300));
    CHECK(numIs(wb, "AGGREGATE(9,5,B2:B6)", 675) && numIs(wb, "AGGREGATE(9,6,B2:B6)", 775));
    put(wb, "E1", "=1/0");
    put(wb, "E2", "5");
    wb.recalc();
    CHECK(numIs(wb, "AGGREGATE(9,6,E1:E2)", 5) && errIs(wb, "SUM(E1:E2)", "#DIV/0!"));
    CHECK(numIs(wb, "AGGREGATE(14,6,B2:B6,2)", 250));
}

void testStats() {
    Workbook wb = data();
    CHECK(numIs(wb, "AVERAGE(B2:B6)", 155) && numIs(wb, "MEDIAN(B2:B6)", 100));
    CHECK(numIs(wb, "MIN(B2:B6)", 50) && numIs(wb, "MAX(B2:B6)", 300));
    CHECK(numIs(wb, "COUNT(A1:B6)", 5) && numIs(wb, "COUNTA(A1:A6)", 6) && numIs(wb, "COUNTBLANK(A1:A8)", 2));
    CHECK(numIs(wb, "COUNTIF(A2:A6,\"North\")", 2) && numIs(wb, "COUNTIF(B2:B6,\">=100\")", 3) && numIs(wb, "COUNTIF(A2:A6,\"*th\")", 4));
    CHECK(numIs(wb, "SUMIF(A2:A6,\"South\",B2:B6)", 300) && numIs(wb, "AVERAGEIF(A2:A6,\"North\",B2:B6)", 87.5));
    CHECK(numIs(wb, "SUMIFS(B2:B6,A2:A6,\"South\",B2:B6,\">60\")", 250));
    CHECK(numIs(wb, "COUNTIFS(A2:A6,\"North\",D2:D6,\"Ann\")", 1));
    CHECK(numIs(wb, "AVERAGEIFS(B2:B6,D2:D6,\"Bob\")", 150));
    CHECK(numIs(wb, "MAXIFS(B2:B6,A2:A6,\"North\")", 100) && numIs(wb, "MINIFS(B2:B6,A2:A6,\"South\")", 50));
    CHECK(numIs(wb, "SUMIFS(B2:B6,C2:C6,\">=2026-02-01\",C2:C6,\"<2026-03-01\")", 325));
    CHECK(numIs(wb, "SUM(COUNTIF(A2:A6,{\"North\",\"East\"}))", 3));
    // spread
    put(wb, "F1", "2");
    put(wb, "F2", "4");
    put(wb, "F3", "4");
    put(wb, "F4", "4");
    put(wb, "F5", "5");
    put(wb, "F6", "5");
    put(wb, "F7", "7");
    put(wb, "F8", "9");
    wb.recalc();
    CHECK(numIs(wb, "STDEV.P(F1:F8)", 2) && numIs(wb, "STDEVP(F1:F8)", 2) && numIs(wb, "VAR.P(F1:F8)", 4));
    CHECK(numIs(wb, "STDEV.S(F1:F8)", 2.13808993529940) && numIs(wb, "VAR(F1:F8)", 4.57142857142857));
    CHECK(numIs(wb, "MODE(F1:F8)", 4) && numIs(wb, "MODE.SNGL(F1:F8)", 4));
    CHECK(numIs(wb, "LARGE(F1:F8,2)", 7) && numIs(wb, "SMALL(F1:F8,3)", 4));
    CHECK(numIs(wb, "RANK(7,F1:F8)", 2) && numIs(wb, "RANK.EQ(4,F1:F8,1)", 2) && numIs(wb, "RANK.AVG(4,F1:F8)", 6));
    CHECK(numIs(wb, "PERCENTILE(F1:F8,0.5)", 4.5) && numIs(wb, "PERCENTILE.INC(F1:F8,0.25)", 4) && numIs(wb, "QUARTILE(F1:F8,3)", 5.5));
    CHECK(numIs(wb, "PERCENTILE.EXC(F1:F8,0.25)", 4) && numIs(wb, "QUARTILE.EXC(F1:F8,1)", 4));
    CHECK(numIs(wb, "PERCENTRANK.INC(F1:F8,5)", 0.571));
    CHECK(numIs(wb, "AVEDEV(F1:F8)", 1.5) && numIs(wb, "DEVSQ(F1:F8)", 32));
    CHECK(numIs(wb, "GEOMEAN(2,8)", 4) && numIs(wb, "HARMEAN(1,2,4)", 1.71428571428571));
    CHECK(numIs(wb, "TRIMMEAN(F1:F8,0.25)", 29.0 / 6));
    CHECK(numIs(wb, "AVERAGEA(1,TRUE)", 1) && numIs(wb, "MAXA(F1:F8)", 9));
    // regression on y = 2x + 1
    for (int i = 1; i <= 5; ++i) {
        put(wb, "H" + std::to_string(i), std::to_string(i));
        put(wb, "I" + std::to_string(i), std::to_string(2 * i + 1));
    }
    put(wb, "I3", "7.5");
    wb.recalc();
    CHECK(numIs(wb, "SLOPE(I1:I5,H1:H5)", 2) && numIs(wb, "INTERCEPT(I1:I5,H1:H5)", 1.1));
    CHECK(numIs(wb, "CORREL(H1:H5,I1:I5)", 20 / std::sqrt(402.0)) && numIs(wb, "RSQ(I1:I5,H1:H5)", 400.0 / 402));
    CHECK(numIs(wb, "FORECAST(6,I1:I5,H1:H5)", 13.1) && numIs(wb, "FORECAST.LINEAR(6,I1:I5,H1:H5)", 13.1));
    CHECK(numIs(wb, "COVARIANCE.S(H1:H5,I1:I5)", 5) && numIs(wb, "COVARIANCE.P(H1:H5,I1:I5)", 4));
    CHECK(numIs(wb, "NORM.DIST(1,0,1,TRUE)", 0.841344746068543) && numIs(wb, "NORM.S.DIST(0,FALSE)", 0.398942280401433));
    CHECK(numIs(wb, "NORM.INV(0.95,0,1)", 1.64485362695147, 1e-7) && numIs(wb, "NORM.S.INV(0.5)", 0, 1e-9));
    CHECK(numIs(wb, "STANDARDIZE(42,40,1.5)", 1.33333333333333));
    auto freq = wb.evaluateArray("FREQUENCY(F1:F8,{4,6})", 0);
    CHECK(freq.size() == 3 && freq[0][0].number == 4 && freq[1][0].number == 2 && freq[2][0].number == 2);
}

void testLogicAndInfo() {
    Workbook wb = data();
    CHECK(textIs(wb, "IF(B2>90,\"big\",\"small\")", "big"));
    CHECK(textIs(wb, "IFS(B3<100,\"low\",B3<200,\"mid\",TRUE,\"high\")", "high"));
    CHECK(textIs(wb, "SWITCH(A2,\"North\",\"N\",\"South\",\"S\",\"?\")", "N") && textIs(wb, "SWITCH(\"x\",\"a\",1,\"none\")", "none"));
    CHECK(numIs(wb, "IFERROR(1/0,7)", 7) && numIs(wb, "IFNA(NA(),8)", 8) && errIs(wb, "IFNA(1/0,8)", "#DIV/0!"));
    CHECK(boolIs(wb, "AND(TRUE,1)", true) && boolIs(wb, "OR(FALSE,0)", false) && boolIs(wb, "XOR(TRUE,TRUE,TRUE)", true) && boolIs(wb, "NOT(1)", false));
    CHECK(boolIs(wb, "TRUE()", true) && boolIs(wb, "FALSE()", false));
    CHECK(numIs(wb, "LET(x,5,y,x*2,x+y)", 15));
    CHECK(numIs(wb, "LET(total,SUM(B2:B6),total/COUNT(B2:B6))", 155));
    CHECK(boolIs(wb, "ISNUMBER(B2)", true) && boolIs(wb, "ISTEXT(A2)", true) && boolIs(wb, "ISBLANK(Z9)", true) && boolIs(wb, "ISLOGICAL(TRUE)", true));
    CHECK(boolIs(wb, "ISERR(NA())", false) && boolIs(wb, "ISNA(NA())", true) && boolIs(wb, "ISERROR(1/0)", true) && boolIs(wb, "ISNONTEXT(5)", true));
    CHECK(boolIs(wb, "ISEVEN(4)", true) && boolIs(wb, "ISODD(4)", false) && boolIs(wb, "ISREF(A1)", true) && boolIs(wb, "ISREF(1)", false));
    CHECK(numIs(wb, "N(TRUE)", 1) && textIs(wb, "T(\"a\")", "a") && textIs(wb, "T(5)", ""));
    CHECK(numIs(wb, "TYPE(\"a\")", 2) && numIs(wb, "TYPE(1)", 1) && numIs(wb, "ERROR.TYPE(1/0)", 2) && numIs(wb, "ERROR.TYPE(NA())", 7));
    put(wb, "G1", "=SUM(B2:B3)");
    wb.recalc();
    CHECK(boolIs(wb, "ISFORMULA(G1)", true) && textIs(wb, "FORMULATEXT(G1)", "=SUM(B2:B3)"));
    CHECK(numIs(wb, "SHEET()", 1) && numIs(wb, "SHEETS()", 1));
    CHECK(numIs(wb, "CHOOSE(2,\"a\",5,\"c\")", 5));
}

void testText() {
    Workbook wb = data();
    CHECK(textIs(wb, "LEFT(\"Hello\",2)", "He") && textIs(wb, "RIGHT(\"Hello\",3)", "llo") && textIs(wb, "MID(\"Hello\",2,3)", "ell"));
    CHECK(numIs(wb, "LEN(\"héllo\")", 5) && textIs(wb, "UPPER(\"abc\")", "ABC") && textIs(wb, "PROPER(\"hello wORLD-x\")", "Hello World-X"));
    CHECK(textIs(wb, "TRIM(\"  a   b  \")", "a b") && textIs(wb, "CLEAN(\"a\"&CHAR(10)&\"b\")", "ab"));
    CHECK(numIs(wb, "FIND(\"l\",\"Hello\")", 3) && numIs(wb, "FIND(\"l\",\"Hello\",4)", 4) && errIs(wb, "FIND(\"L\",\"Hello\")", "#VALUE!"));
    CHECK(numIs(wb, "SEARCH(\"L\",\"Hello\")", 3) && numIs(wb, "SEARCH(\"l?o\",\"Hello\")", 3));
    CHECK(textIs(wb, "SUBSTITUTE(\"a-b-c\",\"-\",\"+\")", "a+b+c") && textIs(wb, "SUBSTITUTE(\"a-b-c\",\"-\",\"+\",2)", "a-b+c"));
    CHECK(textIs(wb, "REPLACE(\"abcdef\",2,3,\"X\")", "aXef") && textIs(wb, "REPT(\"ab\",3)", "ababab"));
    CHECK(boolIs(wb, "EXACT(\"a\",\"A\")", false) && boolIs(wb, "\"a\"=\"A\"", true));
    CHECK(textIs(wb, "CHAR(65)", "A") && numIs(wb, "CODE(\"A\")", 65) && textIs(wb, "UNICHAR(8364)", "\xE2\x82\xAC") && numIs(wb, "UNICODE(\"\xE2\x82\xAC\")", 8364));
    CHECK(textIs(wb, "CONCAT(A2:A3,\"!\")", "NorthSouth!") && textIs(wb, "CONCATENATE(\"a\",1,TRUE)", "a1TRUE"));
    CHECK(textIs(wb, "TEXTJOIN(\", \",TRUE,A2:A4,\"\",\"x\")", "North, South, North, x"));
    CHECK(textIs(wb, "TEXT(1234.567,\"#,##0.00\")", "1,234.57") && textIs(wb, "TEXT(0.25,\"0%\")", "25%") && textIs(wb, "TEXT(C2,\"yyyy-mm-dd\")", "2026-01-15"));
    CHECK(textIs(wb, "TEXT(C2,\"ddd d mmm\")", "Thu 15 Jan"));
    CHECK(numIs(wb, "VALUE(\"1,234.5\")", 1234.5) && numIs(wb, "VALUE(\"12:30\")", 0.520833333333333) && numIs(wb, "NUMBERVALUE(\"1.234,5\",\",\",\".\")", 1234.5));
    CHECK(textIs(wb, "FIXED(1234.567,1)", "1,234.6") && textIs(wb, "FIXED(1234.567,1,TRUE)", "1234.6") && textIs(wb, "DOLLAR(-1234.5)", "-$1,234.50"));
    CHECK(textIs(wb, "TEXTBEFORE(\"john.smith@example.com\",\"@\")", "john.smith") && textIs(wb, "TEXTAFTER(\"a-b-c\",\"-\",-1)", "c"));
    CHECK(textIs(wb, "TEXTAFTER(\"a-b-c\",\"-\",2)", "c") && errIs(wb, "TEXTBEFORE(\"abc\",\"x\")", "#N/A"));
    auto split = wb.evaluateArray("TEXTSPLIT(\"a,b;c,d\",\",\",\";\")", 0);
    CHECK(split.size() == 2 && split[0].size() == 2 && split[1][1].text == "d");
    CHECK(textIs(wb, "\"Total: \"&SUM(B2:B3)", "Total: 350"));
    CHECK(textIs(wb, "HYPERLINK(\"https://x.org\",\"site\")", "site"));
}

void testLookups() {
    Workbook wb = data();
    CHECK(numIs(wb, "VLOOKUP(\"East\",A2:B6,2,FALSE)", 300) && errIs(wb, "VLOOKUP(\"West\",A2:B6,2,FALSE)", "#N/A"));
    CHECK(textIs(wb, "HLOOKUP(\"Rep\",A1:D6,3,FALSE)", "Bob"));
    CHECK(numIs(wb, "INDEX(B2:B6,MATCH(\"East\",A2:A6,0))", 300) && numIs(wb, "INDEX(A1:D6,3,2)", 250));
    CHECK(numIs(wb, "MATCH(250,B2:B6,0)", 2) && numIs(wb, "XMATCH(\"Bob\",D2:D6)", 2) && numIs(wb, "XMATCH(\"Bob\",D2:D6,0,-1)", 5));
    CHECK(textIs(wb, "XLOOKUP(300,B2:B6,D2:D6)", "Ann") && textIs(wb, "XLOOKUP(\"West\",A2:A6,D2:D6,\"none\")", "none"));
    CHECK(numIs(wb, "XLOOKUP(\"North\",A2:A6,B2:B6,,0,-1)", 75));  // last match
    CHECK(numIs(wb, "XLOOKUP(260,B2:B6,B2:B6,,-1)", 250) && numIs(wb, "XLOOKUP(260,B2:B6,B2:B6,,1)", 300));
    CHECK(numIs(wb, "XLOOKUP(1,(A2:A6=\"South\")*(D2:D6=\"Bob\"),B2:B6)", 250));  // several conditions
    CHECK(numIs(wb, "LOOKUP(3,{1,2,3,4},{10,20,30,40})", 30));
    CHECK(numIs(wb, "SUM(OFFSET(A1,1,1,3,1))", 425) && numIs(wb, "ROWS(OFFSET(A1,0,0,4,2))", 4));
    CHECK(numIs(wb, "INDIRECT(\"B\"&3)", 250) && numIs(wb, "SUM(INDIRECT(\"B2:B6\"))", 775) && numIs(wb, "INDIRECT(\"R3C2\",FALSE)", 250));
    CHECK(textIs(wb, "ADDRESS(3,2)", "$B$3") && textIs(wb, "ADDRESS(3,2,4)", "B3") && textIs(wb, "ADDRESS(1,1,1,TRUE,\"My Sheet\")", "'My Sheet'!$A$1"));
    CHECK(numIs(wb, "ROWS(A1:D6)", 6) && numIs(wb, "COLUMNS(A1:D6)", 4) && numIs(wb, "ROW(C5)", 5) && numIs(wb, "COLUMN(C5)", 3));
    CHECK(numIs(wb, "INDEX({1,2;3,4},2,1)", 3));
    CHECK(numIs(wb, "SUM(INDEX(B2:C6,0,1))", 775));
}

void testDates() {
    Workbook wb = data();
    CHECK(numIs(wb, "DATE(2026,3,1)", 46082) && numIs(wb, "DATE(2026,14,1)", 46419) && numIs(wb, "DATE(2026,3,0)", 46081));
    CHECK(numIs(wb, "YEAR(C2)", 2026) && numIs(wb, "MONTH(C2)", 1) && numIs(wb, "DAY(C2)", 15));
    CHECK(numIs(wb, "TIME(12,30,0)", 0.520833333333333) && numIs(wb, "HOUR(0.75)", 18) && numIs(wb, "MINUTE(TIME(1,45,0))", 45) && numIs(wb, "SECOND(TIME(1,2,3))", 3));
    CHECK(numIs(wb, "WEEKDAY(C2)", 5) && numIs(wb, "WEEKDAY(C2,2)", 4) && numIs(wb, "WEEKDAY(C2,3)", 3));
    CHECK(numIs(wb, "WEEKNUM(DATE(2026,1,1))", 1) && numIs(wb, "WEEKNUM(DATE(2026,3,15))", 12) && numIs(wb, "ISOWEEKNUM(DATE(2026,1,1))", 1));
    CHECK(numIs(wb, "ISOWEEKNUM(DATE(2027,1,1))", 53));
    CHECK(numIs(wb, "EDATE(DATE(2026,1,31),1)", 46081) && numIs(wb, "EOMONTH(DATE(2026,1,15),1)", 46081) && numIs(wb, "EOMONTH(DATE(2026,1,15),0)", 46053));
    CHECK(numIs(wb, "DAYS(DATE(2026,3,1),DATE(2026,1,1))", 59) && numIs(wb, "DAYS360(DATE(2026,1,1),DATE(2026,3,1))", 60));
    CHECK(numIs(wb, "DATEDIF(DATE(2020,5,15),DATE(2026,3,1),\"Y\")", 5) && numIs(wb, "DATEDIF(DATE(2020,5,15),DATE(2026,3,1),\"M\")", 69));
    CHECK(numIs(wb, "DATEDIF(DATE(2020,5,15),DATE(2026,3,1),\"YM\")", 9) && numIs(wb, "DATEDIF(DATE(2020,5,15),DATE(2026,3,1),\"MD\")", 14));
    CHECK(numIs(wb, "DATEDIF(DATE(2026,1,1),DATE(2026,3,1),\"D\")", 59));
    CHECK(numIs(wb, "NETWORKDAYS(DATE(2026,3,2),DATE(2026,3,13))", 10) && numIs(wb, "NETWORKDAYS(DATE(2026,3,2),DATE(2026,3,13),DATE(2026,3,4))", 9));
    CHECK(numIs(wb, "NETWORKDAYS.INTL(DATE(2026,3,2),DATE(2026,3,13),11)", 11));
    CHECK(numIs(wb, "WORKDAY(DATE(2026,3,6),1)", 46090) && numIs(wb, "WORKDAY(DATE(2026,3,9),-1)", 46087));
    CHECK(numIs(wb, "WORKDAY.INTL(DATE(2026,3,6),1,\"0000011\")", 46090));
    CHECK(numIs(wb, "YEARFRAC(DATE(2026,1,1),DATE(2026,7,1))", 0.5) && numIs(wb, "YEARFRAC(DATE(2026,1,1),DATE(2026,7,1),3)", 181.0 / 365));
    CHECK(numIs(wb, "DATEVALUE(\"2026-03-01\")", 46082) && numIs(wb, "DATEVALUE(\"1 Mar 2026\")", 46082) && numIs(wb, "DATEVALUE(\"3/1/2026\")", 46082));
    CHECK(numIs(wb, "TIMEVALUE(\"6:00 PM\")", 0.75) && numIs(wb, "TIMEVALUE(\"18:00:00\")", 0.75));
    CHECK(numIs(wb, "C3-C2", 19));
    CellValue today = wb.evaluate("TODAY()", 0);
    CHECK(today.type == CellValue::Number && today.number > 46000);
}

void testFinance() {
    Workbook wb = book();
    CHECK(numIs(wb, "PMT(0.05/12,360,200000)", -1073.64324602428, 1e-9));
    CHECK(numIs(wb, "PMT(0.08/12,10,10000,0,1)", -1030.16432717797, 1e-9));
    CHECK(numIs(wb, "FV(0.06/12,10,-200,-500,1)", 2581.40337406012, 1e-9));
    CHECK(numIs(wb, "PV(0.08/12,240,500)", -59777.1458511878, 1e-9));
    CHECK(numIs(wb, "NPER(0.12/12,-100,-1000,10000,1)", 59.6738656742946, 1e-9));
    CHECK(numIs(wb, "RATE(48,-200,8000)", 0.00770147248820165, 1e-7));
    CHECK(numIs(wb, "IPMT(0.1/12,1,36,8000)", -66.6666666666667, 1e-9) && numIs(wb, "PPMT(0.1/12,1,24,2000)", -75.6231860083188, 1e-9));
    CHECK(numIs(wb, "NPV(0.1,-10000,3000,4200,6800)", 1188.44341233522, 1e-9));
    CHECK(numIs(wb, "IRR({-70000,12000,15000,18000,21000,26000})", 0.0866309480365316, 1e-7));
    CHECK(numIs(wb, "IRR({-70000,12000,15000,18000,21000})", -0.0212448482734408, 1e-6));
    CHECK(numIs(wb, "MIRR({-120000,39000,30000,21000,37000,46000},0.1,0.12)", 0.126094130365711, 1e-9));
    CHECK(numIs(wb, "XNPV(0.09,{-10000,2750,4250,3250,2750},{39448,39508,39751,39859,39904})", 2086.6476020315, 1e-6));
    CHECK(numIs(wb, "XIRR({-10000,2750,4250,3250,2750},{39448,39508,39751,39859,39904})", 0.373362533, 1e-6));
    CHECK(numIs(wb, "EFFECT(0.0525,4)", 0.0535426673707584, 1e-9) && numIs(wb, "NOMINAL(0.053543,4)", 0.0525003431, 1e-7));
    CHECK(numIs(wb, "SLN(30000,7500,10)", 2250) && numIs(wb, "SYD(30000,7500,10,1)", 4090.90909090909) && numIs(wb, "DDB(2400,300,10,1)", 480));
    CHECK(numIs(wb, "DB(1000000,100000,6,1,7)", 186083.333333333));
    CHECK(numIs(wb, "CUMIPMT(0.09/12,360,125000,13,24,0)", -11135.2321307508, 1e-6) && numIs(wb, "CUMPRINC(0.09/12,360,125000,13,24,0)", -934.10712342, 1e-6));
    CHECK(numIs(wb, "RRI(96,10000,11000)", 0.000993255, 1e-5) && numIs(wb, "PDURATION(0.025,2000,2200)", 3.85986616262266, 1e-9));
}

void testArraysAndSpill() {
    Workbook wb = data();
    // spilling: SORT, FILTER, UNIQUE, SEQUENCE write into the cells below and right
    put(wb, "F1", "=SORT(B2:B6,1,-1)");
    put(wb, "G1", "=UNIQUE(A2:A6)");
    put(wb, "H1", "=FILTER(D2:D6,B2:B6>=100)");
    put(wb, "I1", "=SEQUENCE(3,2,10,5)");
    put(wb, "K1", "=SUM(F1#)");
    put(wb, "K2", "=ROWS(G1#)");
    auto st = wb.recalc();
    CHECK(st.spills == 4);
    CHECK(val(wb, "F1").number == 300 && val(wb, "F2").number == 250 && val(wb, "F5").number == 50);
    CHECK(val(wb, "G1").text == "North" && val(wb, "G2").text == "South" && val(wb, "G3").text == "East" && val(wb, "G4").empty());
    CHECK(val(wb, "H1").text == "Ann" && val(wb, "H2").text == "Bob" && val(wb, "H3").text == "Ann" && val(wb, "H4").empty());
    CHECK(val(wb, "I1").number == 10 && val(wb, "J1").number == 15 && val(wb, "J3").number == 35);
    CHECK(val(wb, "K1").number == 775 && val(wb, "K2").number == 3);
    CHECK(wb.sheets[0].get(1, 5)->spilled && !wb.sheets[0].get(0, 5)->spilled && wb.sheets[0].get(0, 5)->spillRows == 5);
    // something in the way: #SPILL!, cleared again once it moves
    put(wb, "G3", "blocker");
    wb.recalc();
    CHECK(val(wb, "G1").isError() && val(wb, "G1").text == "#SPILL!" && val(wb, "G2").empty());
    wb.sheets[0].erase(2, 6);
    wb.recalc();
    CHECK(val(wb, "G1").text == "North" && val(wb, "G3").text == "East");
    // a spill shrinks when its data does
    put(wb, "B6", "500");
    wb.recalc();
    CHECK(val(wb, "F1").number == 500);
    put(wb, "H1", "=FILTER(D2:D6,B2:B6>290)");
    wb.recalc();
    CHECK(val(wb, "H1").text == "Ann" && val(wb, "H2").text == "Bob" && val(wb, "H3").empty());
    // a formula reading a spilled cell computed before the anchor still sees the final value
    put(wb, "A20", "=G2&\"!\"");
    put(wb, "G1", "=UNIQUE(D2:D6)");
    wb.recalc();
    CHECK(val(wb, "A20").text == "Bob!");
    // deleting the anchor drops its spilled values
    wb.sheets[0].erase(0, 6);
    wb.recalc();
    CHECK(val(wb, "G2").empty());
    // element-wise operators and arrays
    CHECK(numIs(wb, "SUM((B2:B6>100)*B2:B6)", 1050));
    CHECK(numIs(wb, "SUM({1,2,3}*{4,5,6})", 32) && numIs(wb, "SUM({1;2;3}*{1,2})", 18));
    CHECK(numIs(wb, "SUM(LEN(A2:A6))", 24));
    CHECK(numIs(wb, "SUM(IF(A2:A6=\"North\",B2:B6,0))", 175));
    CHECK(numIs(wb, "MAX(IFERROR(1/(B2:B6-100),0))", 1.0 / 150));
    auto sorted = wb.evaluateArray("SORTBY(D2:D6,B2:B6,-1)", 0);
    CHECK(sorted.size() == 5 && sorted[0][0].text == "Bob" && sorted[1][0].text == "Ann");
    auto two = wb.evaluateArray("SORT(A2:B6,2)", 0);
    CHECK(two.size() == 5 && two[0][0].text == "North" && two[0][1].number == 75);
    auto once = wb.evaluateArray("UNIQUE(A2:A6,FALSE,TRUE)", 0);
    CHECK(once.size() == 1 && once[0][0].text == "East");
    CHECK(errIs(wb, "FILTER(A2:A6,B2:B6>9999)", "#CALC!") && textIs(wb, "FILTER(A2:A6,B2:B6>9999,\"none\")", "none"));
    auto t = wb.evaluateArray("TRANSPOSE({1,2,3})", 0);
    CHECK(t.size() == 3 && t[2][0].number == 3);
    auto tk = wb.evaluateArray("TAKE(SEQUENCE(5),-2)", 0);
    CHECK(tk.size() == 2 && tk[0][0].number == 4);
    auto dr = wb.evaluateArray("DROP(SEQUENCE(2,3),1,1)", 0);
    CHECK(dr.size() == 1 && dr[0].size() == 2 && dr[0][0].number == 5);
    auto vs = wb.evaluateArray("VSTACK({1,2},{3,4})", 0);
    CHECK(vs.size() == 2 && vs[1][1].number == 4);
    auto hs = wb.evaluateArray("HSTACK({1;2},{3;4;5})", 0);
    CHECK(hs.size() == 3 && hs[0][1].number == 3 && hs[2][0].isError());
    auto cc = wb.evaluateArray("CHOOSECOLS(A1:D2,2,-1)", 0);
    CHECK(cc.size() == 2 && cc[0][0].text == "Amount" && cc[0][1].text == "Rep");
    auto cr = wb.evaluateArray("CHOOSEROWS(A1:D6,1,-1)", 0);
    CHECK(cr.size() == 2 && cr[1][0].text == "South");
    auto tc = wb.evaluateArray("TOCOL({1,2;3,4})", 0);
    CHECK(tc.size() == 4 && tc[2][0].number == 3);
    auto wr = wb.evaluateArray("WRAPROWS(SEQUENCE(5),2,0)", 0);
    CHECK(wr.size() == 3 && wr[2][1].number == 0);
    auto ex = wb.evaluateArray("EXPAND({1,2},2,3,\"-\")", 0);
    CHECK(ex.size() == 2 && ex[1][2].text == "-");
    auto mm = wb.evaluateArray("MMULT({1,2;3,4},{5;6})", 0);
    CHECK(mm.size() == 2 && mm[0][0].number == 17 && mm[1][0].number == 39);
    auto rows = wb.evaluateArray("ROW(A1:A3)", 0);
    CHECK(rows.size() == 3 && rows[2][0].number == 3);
    auto lg = wb.evaluateArray("LARGE(B2:B6,{1,2})", 0);
    CHECK(lg.size() == 1 && lg[0][1].number == 300);
    auto ra = wb.evaluateArray("RANDARRAY(2,2,1,6,TRUE)", 0);
    CHECK(ra.size() == 2 && ra[1][1].number >= 1 && ra[1][1].number <= 6);
    auto lifted = wb.evaluateArray("ROUND({1.25,2.75},1)", 0);
    CHECK(lifted.size() == 1 && std::abs(lifted[0][0].number - 1.3) < 1e-9 && std::abs(lifted[0][1].number - 2.8) < 1e-9);
    auto tr = wb.evaluateArray("TREND({3,5,7},{1,2,3},{4,5})", 0);
    CHECK(tr.size() == 1 && std::abs(tr[0][0].number - 9) < 1e-9 && std::abs(tr[0][1].number - 11) < 1e-9);
    // array constants
    CHECK(numIs(wb, "SUM({1,2;3,4})", 10) && numIs(wb, "COLUMNS({1,2,3})", 3));
    CHECK(boolIs(wb, "INDEX({\"a\",\"b\";\"c\",\"d\"},2,2)=\"d\"", true));
    CHECK(formulaError("SUM({1,2;3})") != "" && formulaError("{1,-2}") == "");
}

void testNamesAndRefs() {
    Workbook wb = data();
    wb.sheets.emplace_back();
    wb.sheets[1].name = "Rates";
    put(wb, "Rates!A1", "0.2");
    wb.names["TAXRATE"] = DefinedName{"TaxRate", "Rates!$A$1", "", nullptr};
    wb.names["AMOUNTS"] = DefinedName{"Amounts", "Sheet1!$B$2:$B$6", "", nullptr};
    wb.names["DOUBLED"] = DefinedName{"Doubled", "Amounts*2", "", nullptr};
    put(wb, "F1", "=SUM(Amounts)*TaxRate");
    put(wb, "F2", "=SUM(Doubled)");
    put(wb, "F3", "=Rates!A1*100");
    put(wb, "F4", "='Rates'!A1+1");
    put(wb, "F5", "=Unknown+1");
    wb.recalc();
    CHECK(std::abs(val(wb, "F1").number - 155) < 1e-9 && val(wb, "F2").number == 1550 && std::abs(val(wb, "F3").number - 20) < 1e-9);
    CHECK(std::abs(val(wb, "F4").number - 1.2) < 1e-9 && val(wb, "F5").text == "#NAME?");
    Range r;
    std::string err;
    CHECK(parseRange(wb, "Amounts", 0, r, err) && r.r1 == 1 && r.r2 == 5 && r.c1 == 1);
    // structural edits move names along with the cells
    wb.insertRows(0, 0, 2);
    CHECK(wb.names["AMOUNTS"].formula == "Sheet1!$B$4:$B$8");
    wb.recalc();
    CHECK(std::abs(val(wb, "F3").number - 155) < 1e-9);
    wb.renameSheet(1, "Tax Rates");
    CHECK(wb.names["TAXRATE"].formula == "'Tax Rates'!$A$1");
    CHECK(validName("TaxRate") && validName("_x") && !validName("A1") && !validName("AB12") && !validName("has space") && !validName("1abc"));
    // absolute and relative refs when copied
    CHECK(offsetFormula("A1+$A1+A$1+$A$1", 2, 3) == "D3+$A3+D$1+$A$1");
    CHECK(offsetFormula("SUM(Rates!B2:B4)", 1, 0) == "SUM(Rates!B3:B5)");
    CHECK(offsetFormula("A1#", 1, 1) == "B2#");
    // _xlfn prefixes in files, the spill operator as ANCHORARRAY
    std::string fileF = "_xlfn._xlws.SORT(_xlfn.UNIQUE(A1:A9))";
    CHECK(fileFormula("SORT(UNIQUE(A1:A9))", true) == fileF);
    CHECK(fileFormula(fileF, false) == "SORT(UNIQUE(A1:A9))");
    CHECK(fileFormula("SUM(A1#)+STDEV.S(B1:B3)", true) == "SUM(_xlfn.ANCHORARRAY(A1))+_xlfn.STDEV.S(B1:B3)");
    CHECK(functionNames().size() >= 250);
}

void testFormatting() {
    // cell styles compare and the colour parser
    std::uint32_t c;
    CHECK(parseColor("#ff0000", c) && c == (0xFF0000 | kColorSet) && colorName(c) == "#ff0000");
    CHECK(parseColor("black", c) && hasColor(c) && rgbOf(c) == 0);
    CHECK(parseColor("none", c) && c == 0 && !parseColor("nope", c));
    CellStyle a, b;
    CHECK(a == b && a.plain());
    b.border[BorderTop] = 1;
    CHECK(!(a == b) && !b.plain());
    CHECK(formatColor(CellValue::num(-5), "0;[Red]-0") == (0xFF0000 | kColorSet) && formatColor(CellValue::num(5), "0;[Red]-0") == 0);
    // conditional formats
    Workbook wb = data();
    CondFormat big;
    big.range = Range{0, 1, 1, 5, 1};
    big.type = "cell";
    big.op = ">";
    big.value1 = "200";
    big.style.fill = 0xFFC7CE | kColorSet;
    wb.sheets[0].condFormats.push_back(big);
    CondFormat north;
    north.range = Range{0, 1, 0, 5, 0};
    north.type = "formula";
    north.value1 = "$A2=\"North\"";
    north.style.bold = true;
    wb.sheets[0].condFormats.push_back(north);
    CondFormat top;
    top.range = Range{0, 1, 1, 5, 1};
    top.type = "top";
    top.value1 = "1";
    top.style.color = 0x0000FF | kColorSet;
    wb.sheets[0].condFormats.push_back(top);
    CondFormat scale;
    scale.range = Range{0, 1, 1, 5, 1};
    scale.type = "data_bar";
    wb.sheets[0].condFormats.push_back(scale);
    CellPainter p(wb, 0);
    double bar;
    CellStyle s = p.style(2, 1, &bar);  // B3 = 250
    CHECK(s.fill == (0xFFC7CE | kColorSet) && !hasColor(s.color) && bar > 0.7 && bar < 0.9);
    s = p.style(4, 1, &bar);  // B5 = 300, the top one
    CHECK(s.fill == (0xFFC7CE | kColorSet) && s.color == (0x0000FF | kColorSet) && bar == 1);
    s = p.style(1, 1, &bar);  // B2 = 100
    CHECK(!hasColor(s.fill) && bar < 0.3);
    CHECK(p.style(1, 0).bold && p.style(3, 0).bold && !p.style(2, 0).bold);  // A2 and A4 are North
    CHECK(p.matches(0, 2, 1) && !p.matches(0, 1, 1));
    // validation
    Validation list;
    list.range = Range{0, 1, 0, 20, 0};
    list.type = "list";
    list.value1 = "North,South,East,West";
    wb.sheets[0].validations.push_back(list);
    Validation amount;
    amount.range = Range{0, 1, 1, 20, 1};
    amount.type = "whole";
    amount.op = "between";
    amount.value1 = "0";
    amount.value2 = "1000";
    amount.errorStyle = "warning";
    wb.sheets[0].validations.push_back(amount);
    std::string msg;
    bool reject;
    CHECK(validateCell(wb, 0, 1, 0, msg, reject));
    put(wb, "A7", "Central");
    CHECK(!validateCell(wb, 0, 6, 0, msg, reject) && reject && msg.find("North, South") != std::string::npos);
    put(wb, "B7", "12.5");
    CHECK(!validateCell(wb, 0, 6, 1, msg, reject) && !reject);
    put(wb, "B7", "1200");
    CHECK(!validateCell(wb, 0, 6, 1, msg, reject) && msg.find("between 0 and 1000") != std::string::npos);
    put(wb, "B7", "999");
    CHECK(validateCell(wb, 0, 6, 1, msg, reject));
    CHECK(validateCell(wb, 0, 7, 0, msg, reject));  // blank is fine
    // a list from cells
    Validation fromCells;
    fromCells.range = Range{0, 1, 3, 20, 3};
    fromCells.value1 = "$D$2:$D$6";
    wb.sheets[0].validations.push_back(fromCells);
    auto items = validationList(wb, 0, fromCells);
    CHECK(items.size() == 5 && items[1] == "Bob");
    put(wb, "D7", "Cy");
    CHECK(validateCell(wb, 0, 6, 3, msg, reject));
    // inserting rows moves the rules
    wb.insertRows(0, 0, 1);
    CHECK(wb.sheets[0].condFormats[1].range.r1 == 2 && wb.sheets[0].condFormats[1].value1 == "$A3=\"North\"");
    CHECK(wb.sheets[0].validations[2].value1 == "$D$3:$D$7");
}

}  // namespace

int main() {
    testMath();
    testSubtotal();
    testStats();
    testLogicAndInfo();
    testText();
    testLookups();
    testDates();
    testFinance();
    testArraysAndSpill();
    testNamesAndRefs();
    testFormatting();
    return check::summary("office calc");
}
