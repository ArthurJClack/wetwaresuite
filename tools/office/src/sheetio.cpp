// Spreadsheet files: CSV/TSV (with import options) and the CSV writer.
// XLSX lives in xlsx_*.cpp, ODS in ods.cpp.
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

#include "wetwaresuite/office/sheet.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"

namespace wetwaresuite::office {

char sniffDelimiter(const std::string& text) {
    const char cands[] = {',', ';', '\t', '|'};
    int best = 0;
    char pick = ',';
    for (char d : cands) {
        // count per line over the first lines; a delimiter that is consistent wins
        std::vector<int> counts;
        int cur = 0;
        bool q = false;
        for (std::size_t i = 0; i < text.size() && counts.size() < 20; ++i) {
            char c = text[i];
            if (c == '"') q = !q;
            else if (!q && c == d) ++cur;
            else if (!q && c == '\n') {
                counts.push_back(cur);
                cur = 0;
            }
        }
        if (cur) counts.push_back(cur);
        if (counts.empty()) continue;
        int mn = counts[0];
        for (int c : counts) mn = std::min(mn, c);
        int score = mn * 100 + counts[0];
        if (mn > 0 && score > best) {
            best = score;
            pick = d;
        }
    }
    return pick;
}

namespace {

struct Field {
    std::string text;
    bool quoted = false;
};

std::vector<std::vector<Field>> csvRows(const std::string& text, char delim) {
    std::vector<std::vector<Field>> rows(1);
    Field field;
    bool inQuotes = false, any = false;
    auto flush = [&] {
        rows.back().push_back(std::move(field));
        field = Field{};
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        any = true;
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.text += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field.text += c;
            }
            continue;
        }
        if (c == '"' && field.text.empty()) {
            inQuotes = field.quoted = true;
        } else if (c == delim) {
            flush();
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            flush();
            rows.emplace_back();
            any = false;
        } else {
            field.text += c;
        }
    }
    if (any) flush();
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

std::string latin1ToUtf8(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

bool validUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + static_cast<std::size_t>(n) >= s.size() + (n == 0 ? 1 : 0)) return false;
        for (int k = 1; k <= n; ++k) {
            if (i + static_cast<std::size_t>(k) >= s.size() || (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) >> 6) != 2) return false;
        }
        i += static_cast<std::size_t>(n) + 1;
    }
    return true;
}

// "1.234,5" -> "1234.5" when the field is a number written with a decimal comma.
bool decimalCommaNumber(const std::string& t, std::string& out) {
    if (t.empty() || t.find(',') == std::string::npos) return false;
    std::string s;
    bool comma = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (std::isdigit(static_cast<unsigned char>(c))) s += c;
        else if (c == '.' || c == ' ' || c == '\'') continue;  // thousands separators
        else if (c == ',' && !comma) comma = true, s += '.';
        else if ((c == '-' || c == '+') && i == 0) s += c;
        else if (c == '%' && i + 1 == t.size()) s += c;
        else return false;
    }
    out = s;
    return true;
}

// "03/04/2026" with the day or month first.
bool orderedDate(const std::string& t, const std::string& order, CellValue& v, std::string& format) {
    int a, b, y;
    char s1, s2;
    int n = 0;
    if (std::sscanf(t.c_str(), "%d%c%d%c%d%n", &a, &s1, &b, &s2, &y, &n) != 5 || static_cast<std::size_t>(n) != t.size()) return false;
    if (s1 != s2 || (s1 != '/' && s1 != '.' && s1 != '-')) return false;
    if (y < 100) y += y < 50 ? 2000 : 1900;
    const int d = order == "mdy" ? b : a, m = order == "mdy" ? a : b;
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900 || y > 9999) return false;
    v = CellValue::num(dateSerial(y, m, d));
    format = order == "mdy" ? "mm/dd/yyyy" : "dd/mm/yyyy";
    return true;
}

}  // namespace

Workbook readCsv(const std::string& textIn, const CsvOptions& o) {
    std::string text = textIn;
    if (startsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
    if (o.encoding == "latin1" || o.encoding == "latin-1" || o.encoding == "iso-8859-1" || o.encoding == "cp1252" ||
        (o.encoding.empty() && !validUtf8(text))) {
        text = latin1ToUtf8(text);
    }
    const char delim = o.delimiter ? o.delimiter : sniffDelimiter(text);
    Workbook wb;
    wb.sheets.emplace_back();
    wb.sheets[0].name = "Sheet1";
    Sheet& sh = wb.sheets[0];
    auto rows = csvRows(text, delim);
    const std::size_t skip = static_cast<std::size_t>(std::max(0, o.skipRows));
    for (std::size_t r = skip; r < rows.size() && r - skip < static_cast<std::size_t>(kMaxRows); ++r) {
        const int row = static_cast<int>(r - skip);
        for (std::size_t c = 0; c < rows[r].size() && c < static_cast<std::size_t>(kMaxCols); ++c) {
            const Field& f = rows[r][c];
            const int col = static_cast<int>(c);
            if (f.text.empty() && !f.quoted) continue;
            Cell& cell = sh.at(row, col);
            const std::string t = trimCopy(f.text);
            std::string num, format;
            CellValue v;
            if (!o.detectTypes || o.textColumns.count(col)) {
                cell.value = CellValue::str(f.text);
            } else if (f.quoted && !f.text.empty() && f.text[0] == '=') {
                cell.value = CellValue::str(f.text);  // quoted fields are typed but never become formulas
            } else if (o.decimalComma && decimalCommaNumber(t, num)) {
                setInput(cell, num);
            } else if (!o.dateOrder.empty() && orderedDate(t, o.dateOrder, v, format)) {
                cell.value = v;
                cell.format = format;
            } else {
                setInput(cell, f.text);
            }
            if (cell.blank()) sh.erase(row, col);
        }
    }
    wb.recalc();
    return wb;
}

Workbook readCsv(const std::string& text, char delim) {
    CsvOptions o;
    o.delimiter = delim ? delim : ',';
    o.encoding = "utf-8";
    return readCsv(text, o);
}

std::string writeCsv(const Sheet& sh, char delim) {
    int r1, c1, r2, c2;
    if (!sh.bounds(r1, c1, r2, c2)) return {};
    std::string out;
    for (int r = 0; r <= r2; ++r) {
        for (int c = 0; c <= c2; ++c) {
            if (c) out += delim;
            const Cell* cell = sh.get(r, c);
            if (!cell) continue;
            std::string v;
            const CellValue& val = cell->value;
            if (val.type == CellValue::Number) v = isDateFormat(cell->format) ? formatValue(val, cell->format) : textNumber(val.number);
            else v = formatValue(val, "");
            bool quote = v.find_first_of(std::string("\"\r\n") + delim) != std::string::npos ||
                         (!v.empty() && (v.front() == ' ' || v.back() == ' '));
            if (quote) {
                std::string q = "\"";
                for (char ch : v) {
                    q += ch;
                    if (ch == '"') q += '"';
                }
                v = q + "\"";
            }
            out += v;
        }
        out += "\r\n";
    }
    return out;
}

}  // namespace wetwaresuite::office
