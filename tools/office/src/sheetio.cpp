// Spreadsheet files: CSV/TSV, XLSX (read and write) and ODS (read).
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

#include "achclwss/office/sheet.hpp"
#include "achclwss/raster/ora.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"
#include "xml.hpp"

namespace achclwss::office {

// ---- CSV ------------------------------------------------------------------------

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

Workbook readCsv(const std::string& textIn, char delim) {
    std::string text = textIn;
    if (startsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
    Workbook wb;
    wb.sheets.emplace_back();
    wb.sheets[0].name = "Sheet1";
    Sheet& sh = wb.sheets[0];
    int row = 0, col = 0;
    std::string field;
    bool quoted = false, inQuotes = false, any = false;
    auto flush = [&] {
        if (!field.empty() || quoted) {
            Cell& c = sh.at(row, col);
            if (quoted) {
                // quoted fields are still typed (numbers stay numbers) but never become formulas
                if (!field.empty() && field[0] == '=') c.value = CellValue::str(field);
                else setInput(c, field);
            } else {
                setInput(c, field);
            }
            if (c.blank()) sh.erase(row, col);
        }
        field.clear();
        quoted = false;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        any = true;
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == '"' && field.empty()) {
            inQuotes = quoted = true;
        } else if (c == delim) {
            flush();
            ++col;
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            flush();
            ++row;
            col = 0;
            any = false;
        } else {
            field += c;
        }
    }
    if (any) flush();
    wb.recalc();
    return wb;
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

// ---- XLSX -------------------------------------------------------------------------

namespace {

const char* kMainNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
const char* kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

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

std::string zipEntry(const std::map<std::string, std::string>& files, const std::string& name) {
    auto it = files.find(name);
    return it == files.end() ? std::string() : it->second;
}

// Resolves a relationship target against the folder of the part that owns it.
std::string resolveTarget(const std::string& base, const std::string& target) {
    if (!target.empty() && target[0] == '/') return target.substr(1);
    std::string dir = base.substr(0, base.rfind('/') + 1);
    std::string path = dir + target;
    // collapse "a/../"
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

std::map<std::string, std::string> readRels(const std::map<std::string, std::string>& files, const std::string& part) {
    std::map<std::string, std::string> out;
    std::string dir = part.substr(0, part.rfind('/') + 1);
    std::string relsPath = dir + "_rels/" + part.substr(part.rfind('/') + 1) + ".rels";
    std::string data = zipEntry(files, relsPath);
    if (data.empty()) return out;
    xml::Node root = xml::parse(data);
    for (const auto* r : root.all("Relationship")) out[r->attr("Id")] = resolveTarget(part, r->attr("Target"));
    return out;
}

// Parses "B12" into 0-based row/col.
bool cellRef(const std::string& ref, int& row, int& col) {
    std::size_t i = 0;
    std::string letters;
    while (i < ref.size() && std::isalpha(static_cast<unsigned char>(ref[i]))) letters += ref[i++];
    if (letters.empty() || i >= ref.size()) return false;
    col = colIndex(upper(letters));
    long r = std::strtol(ref.c_str() + i, nullptr, 10);
    row = static_cast<int>(r - 1);
    return col >= 0 && row >= 0 && row < kMaxRows;
}

}  // namespace

Workbook readXlsx(const std::string& bytes) {
    std::map<std::string, std::string> files;
    for (auto& e : raster::readZip(bytes)) files[e.name] = std::move(e.data);
    std::string wbPath = "xl/workbook.xml";
    if (std::string root = zipEntry(files, "_rels/.rels"); !root.empty()) {
        xml::Node n = xml::parse(root);
        for (const auto* r : n.all("Relationship")) {
            if (endsWith(r->attr("Type"), "/officeDocument")) wbPath = resolveTarget("", r->attr("Target"));
        }
    }
    std::string wbXml = zipEntry(files, wbPath);
    if (wbXml.empty()) throw std::runtime_error("not an .xlsx workbook (no " + wbPath + ")");
    xml::Node wbNode = xml::parse(wbXml);
    auto rels = readRels(files, wbPath);

    // shared strings
    std::vector<std::string> shared;
    std::string sstPath;
    for (const auto& [id, target] : rels) {
        if (endsWith(target, "sharedStrings.xml")) sstPath = target;
    }
    if (sstPath.empty()) sstPath = "xl/sharedStrings.xml";
    if (std::string sst = zipEntry(files, sstPath); !sst.empty()) {
        xml::Node n = xml::parse(sst);
        for (const auto* si : n.all("si")) {
            std::string s;
            for (const auto& ch : si->children) {
                if (ch.is("t")) s += ch.allText();
                else if (ch.is("r")) {
                    for (const auto* t : ch.all("t")) s += t->allText();
                }
            }
            shared.push_back(s);
        }
    }
    // styles: cellXfs -> (format, bold)
    std::vector<std::pair<std::string, bool>> xfs;
    std::string stylesPath = "xl/styles.xml";
    for (const auto& [id, target] : rels) {
        if (endsWith(target, "styles.xml")) stylesPath = target;
    }
    if (std::string st = zipEntry(files, stylesPath); !st.empty()) {
        xml::Node n = xml::parse(st);
        std::map<int, std::string> custom;
        if (const auto* nf = n.child("numFmts")) {
            for (const auto* f : nf->all("numFmt")) custom[std::atoi(f->attr("numFmtId").c_str())] = f->attr("formatCode");
        }
        std::vector<bool> boldFonts;
        if (const auto* fonts = n.child("fonts")) {
            for (const auto* f : fonts->all("font")) {
                const auto* b = f->child("b");
                boldFonts.push_back(b && b->attr("val") != "0" && b->attr("val") != "false");
            }
        }
        if (const auto* cx = n.child("cellXfs")) {
            for (const auto* xf : cx->all("xf")) {
                int id = std::atoi(xf->attr("numFmtId").c_str());
                std::string fmt;
                if (custom.count(id)) fmt = custom[id];
                else if (builtinFormats().count(id)) fmt = builtinFormats().at(id);
                if (lower(fmt) == "general") fmt.clear();
                int font = std::atoi(xf->attr("fontId").c_str());
                bool bold = font >= 0 && font < static_cast<int>(boldFonts.size()) && boldFonts[static_cast<std::size_t>(font)];
                xfs.emplace_back(fmt, bold);
            }
        }
    }

    Workbook wb;
    const xml::Node* sheetsNode = wbNode.child("sheets");
    if (!sheetsNode) throw std::runtime_error("workbook has no sheets");
    for (const auto* s : sheetsNode->all("sheet")) {
        Sheet sh;
        sh.name = s->attr("name");
        std::string rid = s->attr("r:id");
        if (rid.empty()) rid = s->attr("id");
        auto it = rels.find(rid);
        std::string data = it == rels.end() ? std::string() : zipEntry(files, it->second);
        if (!data.empty()) {
            xml::Node ws = xml::parse(data);
            if (const auto* cols = ws.child("cols")) {
                for (const auto* c : cols->all("col")) {
                    int mn = std::atoi(c->attr("min").c_str()), mx = std::atoi(c->attr("max").c_str());
                    double w = std::atof(c->attr("width").c_str());
                    if (mn >= 1 && mx >= mn && w > 0 && mx - mn < 256) {
                        for (int k = mn; k <= mx; ++k) sh.colWidths[k - 1] = w;
                    }
                }
            }
            struct SharedF {
                std::string formula;
                int row, col;
            };
            std::map<std::string, SharedF> sharedF;
            if (const auto* sd = ws.child("sheetData")) {
                int rowIdx = -1;
                for (const auto* rowNode : sd->all("row")) {
                    std::string rattr = rowNode->attr("r");
                    rowIdx = rattr.empty() ? rowIdx + 1 : std::atoi(rattr.c_str()) - 1;
                    int colIdx = -1;
                    for (const auto* c : rowNode->all("c")) {
                        int row = rowIdx, col = colIdx + 1;
                        if (std::string ref = c->attr("r"); !ref.empty()) cellRef(ref, row, col);
                        colIdx = col;
                        if (row < 0 || row >= kMaxRows || col < 0 || col >= kMaxCols) continue;
                        Cell cell;
                        std::string t = c->attr("t");
                        std::string v;
                        if (const auto* vn = c->child("v")) v = vn->allText();
                        if (t == "s") {
                            std::size_t i = static_cast<std::size_t>(std::atoi(v.c_str()));
                            cell.value = CellValue::str(i < shared.size() ? shared[i] : std::string());
                        } else if (t == "inlineStr") {
                            if (const auto* is = c->child("is")) cell.value = CellValue::str(is->allText());
                        } else if (t == "str") {
                            cell.value = CellValue::str(v);
                        } else if (t == "b") {
                            cell.value = CellValue::boolean(v == "1" || lower(v) == "true");
                        } else if (t == "e") {
                            cell.value = CellValue::error(v);
                        } else if (t == "d") {
                            int y, m, d;
                            if (parseIsoDate(v.substr(0, 10), y, m, d)) cell.value = CellValue::num(dateSerial(y, m, d));
                        } else if (!v.empty()) {
                            cell.value = CellValue::num(std::atof(v.c_str()));
                        }
                        if (const auto* f = c->child("f")) {
                            std::string text = f->allText();
                            std::string ft = f->attr("t");
                            if (ft == "shared") {
                                std::string si = f->attr("si");
                                if (!text.empty()) {
                                    sharedF[si] = {text, row, col};
                                } else if (sharedF.count(si)) {
                                    const auto& m = sharedF[si];
                                    text = offsetFormula(m.formula, row - m.row, col - m.col);
                                }
                            }
                            if (!text.empty()) cell.formula = fileFormula(text, false);
                        }
                        std::string sattr = c->attr("s");
                        if (!sattr.empty()) {
                            std::size_t si = static_cast<std::size_t>(std::atoi(sattr.c_str()));
                            if (si < xfs.size()) {
                                cell.format = xfs[si].first;
                                cell.bold = xfs[si].second;
                            }
                        }
                        if (!cell.blank()) sh.cells[Sheet::key(row, col)] = std::move(cell);
                    }
                }
            }
        }
        wb.sheets.push_back(std::move(sh));
    }
    if (wb.sheets.empty()) throw std::runtime_error("workbook has no sheets");
    if (const auto* views = wbNode.child("bookViews")) {
        if (const auto* v = views->child("workbookView")) {
            int a = std::atoi(v->attr("activeTab").c_str());
            if (a >= 0 && a < static_cast<int>(wb.sheets.size())) wb.active = a;
        }
    }
    return wb;
}

std::string writeXlsx(const Workbook& wb) {
    // shared strings and styles across the workbook
    std::vector<std::string> strings;
    std::map<std::string, int> stringIndex;
    std::vector<std::pair<std::string, bool>> styles = {{"", false}};
    std::map<std::pair<std::string, bool>, int> styleIndex = {{{"", false}, 0}};
    std::map<std::string, int> customFormats;
    std::map<std::string, int> builtinIds;
    for (const auto& [id, code] : builtinFormats()) {
        if (id != 14 && id != 22 && !code.empty() && !builtinIds.count(code)) builtinIds[code] = id;
    }
    auto formatId = [&](const std::string& code) {
        if (code.empty()) return 0;
        auto b = builtinIds.find(code);
        if (b != builtinIds.end()) return b->second;
        auto c = customFormats.find(code);
        if (c != customFormats.end()) return c->second;
        int id = 164 + static_cast<int>(customFormats.size());
        customFormats[code] = id;
        return id;
    };
    auto styleOf = [&](const Cell& c) {
        auto key = std::make_pair(c.format, c.bold);
        auto it = styleIndex.find(key);
        if (it != styleIndex.end()) return it->second;
        int id = static_cast<int>(styles.size());
        styles.push_back(key);
        styleIndex[key] = id;
        return id;
    };

    std::vector<raster::ZipEntry> entries;
    std::vector<std::string> sheetXml;
    for (const auto& sh : wb.sheets) {
        xml::Writer w;
        w.open("worksheet").attr("xmlns", kMainNs).attr("xmlns:r", kRelNs);
        int r1, c1, r2, c2;
        if (sh.bounds(r1, c1, r2, c2)) w.leaf("dimension", {{"ref", cellName(r1, c1) + ":" + cellName(r2, c2)}});
        if (&sh == &wb.sheets[static_cast<std::size_t>(std::clamp(wb.active, 0, static_cast<int>(wb.sheets.size()) - 1))]) {
            w.open("sheetViews").open("sheetView").attr("tabSelected", "1").attr("workbookViewId", "0").close().close();
        }
        w.leaf("sheetFormatPr", {{"defaultRowHeight", "15"}});
        if (!sh.colWidths.empty()) {
            w.open("cols");
            for (const auto& [col, width] : sh.colWidths) {
                w.leaf("col", {{"min", std::to_string(col + 1)}, {"max", std::to_string(col + 1)}, {"width", textNumber(width)}, {"customWidth", "1"}});
            }
            w.close();
        }
        w.open("sheetData");
        int currentRow = -1;
        for (const auto& [k, c] : sh.cells) {
            if (c.blank()) continue;
            const int row = Sheet::rowOf(k), col = Sheet::colOf(k);
            if (row != currentRow) {
                if (currentRow >= 0) w.close();
                w.open("row").attr("r", std::to_string(row + 1));
                currentRow = row;
            }
            w.open("c").attr("r", cellName(row, col));
            int s = styleOf(c);
            if (s) w.attr("s", std::to_string(s));
            const CellValue& v = c.value;
            if (c.hasFormula()) {
                if (v.type == CellValue::Text) w.attr("t", "str");
                else if (v.type == CellValue::Bool) w.attr("t", "b");
                else if (v.type == CellValue::Error) w.attr("t", "e");
                w.element("f", fileFormula(c.formula, true));
                if (v.type == CellValue::Text) w.element("v", v.text);
                else if (v.type == CellValue::Bool) w.element("v", v.number != 0 ? "1" : "0");
                else if (v.type == CellValue::Error) w.element("v", v.text == "#CYCLE!" ? "#N/A" : v.text);
                else if (v.type == CellValue::Number) w.element("v", textNumber(v.number));
            } else {
                switch (v.type) {
                    case CellValue::Text: {
                        auto it = stringIndex.find(v.text);
                        int idx;
                        if (it == stringIndex.end()) {
                            idx = static_cast<int>(strings.size());
                            strings.push_back(v.text);
                            stringIndex[v.text] = idx;
                        } else {
                            idx = it->second;
                        }
                        w.attr("t", "s").element("v", std::to_string(idx));
                        break;
                    }
                    case CellValue::Number: w.element("v", textNumber(v.number)); break;
                    case CellValue::Bool: w.attr("t", "b").element("v", v.number != 0 ? "1" : "0"); break;
                    case CellValue::Error: w.attr("t", "e").element("v", v.text); break;
                    case CellValue::Empty: break;
                }
            }
            w.close();
        }
        if (currentRow >= 0) w.close();
        w.close();  // sheetData
        w.leaf("pageMargins", {{"left", "0.7"}, {"right", "0.7"}, {"top", "0.75"}, {"bottom", "0.75"}, {"header", "0.3"}, {"footer", "0.3"}});
        sheetXml.push_back(w.str());
    }

    // styles.xml
    for (const auto& s : styles) formatId(s.first);
    xml::Writer st;
    st.open("styleSheet").attr("xmlns", kMainNs);
    if (!customFormats.empty()) {
        st.open("numFmts").attr("count", std::to_string(customFormats.size()));
        std::vector<std::pair<int, std::string>> ordered;
        for (const auto& [code, id] : customFormats) ordered.emplace_back(id, code);
        std::sort(ordered.begin(), ordered.end());
        for (const auto& [id, code] : ordered) st.leaf("numFmt", {{"numFmtId", std::to_string(id)}, {"formatCode", code}});
        st.close();
    }
    st.open("fonts").attr("count", "2");
    st.open("font").leaf("sz", {{"val", "11"}}).leaf("name", {{"val", "Calibri"}}).leaf("family", {{"val", "2"}}).close();
    st.open("font").leaf("b").leaf("sz", {{"val", "11"}}).leaf("name", {{"val", "Calibri"}}).leaf("family", {{"val", "2"}}).close();
    st.close();
    st.open("fills").attr("count", "2");
    st.open("fill").leaf("patternFill", {{"patternType", "none"}}).close();
    st.open("fill").leaf("patternFill", {{"patternType", "gray125"}}).close();
    st.close();
    st.open("borders").attr("count", "1").open("border").leaf("left").leaf("right").leaf("top").leaf("bottom").leaf("diagonal").close().close();
    st.open("cellStyleXfs").attr("count", "1").leaf("xf", {{"numFmtId", "0"}, {"fontId", "0"}, {"fillId", "0"}, {"borderId", "0"}}).close();
    st.open("cellXfs").attr("count", std::to_string(styles.size()));
    for (const auto& [fmt, bold] : styles) {
        int id = formatId(fmt);
        st.open("xf").attr("numFmtId", std::to_string(id)).attr("fontId", bold ? "1" : "0").attr("fillId", "0").attr("borderId", "0").attr("xfId", "0");
        if (id) st.attr("applyNumberFormat", "1");
        if (bold) st.attr("applyFont", "1");
        st.close();
    }
    st.close();
    st.open("cellStyles").attr("count", "1").leaf("cellStyle", {{"name", "Normal"}, {"xfId", "0"}, {"builtinId", "0"}}).close();
    std::string stylesXml = st.str();

    // sharedStrings.xml
    xml::Writer ss;
    ss.open("sst").attr("xmlns", kMainNs).attr("count", std::to_string(strings.size())).attr("uniqueCount", std::to_string(strings.size()));
    for (const auto& s : strings) {
        ss.open("si").open("t");
        if (!s.empty() && (s.front() == ' ' || s.back() == ' ' || s.find('\n') != std::string::npos)) ss.attr("xml:space", "preserve");
        ss.text(s).close().close();
    }
    std::string sstXml = ss.str();

    // workbook.xml and relationships
    xml::Writer wbw;
    wbw.open("workbook").attr("xmlns", kMainNs).attr("xmlns:r", kRelNs);
    wbw.open("bookViews").leaf("workbookView", {{"activeTab", std::to_string(std::max(0, wb.active))}}).close();
    wbw.open("sheets");
    for (std::size_t i = 0; i < wb.sheets.size(); ++i) {
        wbw.leaf("sheet", {{"name", wb.sheets[i].name}, {"sheetId", std::to_string(i + 1)}, {"r:id", "rId" + std::to_string(i + 1)}});
    }
    wbw.close();
    wbw.leaf("calcPr", {{"calcId", "191029"}, {"fullCalcOnLoad", "1"}});
    std::string workbookXml = wbw.str();

    xml::Writer rels;
    rels.open("Relationships").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
    for (std::size_t i = 0; i < wb.sheets.size(); ++i) {
        rels.leaf("Relationship", {{"Id", "rId" + std::to_string(i + 1)},
                                   {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet"},
                                   {"Target", "worksheets/sheet" + std::to_string(i + 1) + ".xml"}});
    }
    const std::size_t n = wb.sheets.size();
    rels.leaf("Relationship", {{"Id", "rId" + std::to_string(n + 1)},
                               {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles"},
                               {"Target", "styles.xml"}});
    rels.leaf("Relationship", {{"Id", "rId" + std::to_string(n + 2)},
                               {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings"},
                               {"Target", "sharedStrings.xml"}});
    std::string wbRels = rels.str();

    xml::Writer ct;
    ct.open("Types").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/content-types");
    ct.leaf("Default", {{"Extension", "rels"}, {"ContentType", "application/vnd.openxmlformats-package.relationships+xml"}});
    ct.leaf("Default", {{"Extension", "xml"}, {"ContentType", "application/xml"}});
    ct.leaf("Override", {{"PartName", "/xl/workbook.xml"}, {"ContentType", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"}});
    for (std::size_t i = 0; i < n; ++i) {
        ct.leaf("Override", {{"PartName", "/xl/worksheets/sheet" + std::to_string(i + 1) + ".xml"},
                             {"ContentType", "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"}});
    }
    ct.leaf("Override", {{"PartName", "/xl/styles.xml"}, {"ContentType", "application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"}});
    ct.leaf("Override", {{"PartName", "/xl/sharedStrings.xml"},
                         {"ContentType", "application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"}});
    std::string types = ct.str();

    xml::Writer root;
    root.open("Relationships").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
    root.leaf("Relationship", {{"Id", "rId1"},
                               {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"},
                               {"Target", "xl/workbook.xml"}});
    std::string rootRels = root.str();

    entries.push_back({"[Content_Types].xml", types});
    entries.push_back({"_rels/.rels", rootRels});
    entries.push_back({"xl/workbook.xml", workbookXml});
    entries.push_back({"xl/_rels/workbook.xml.rels", wbRels});
    for (std::size_t i = 0; i < n; ++i) entries.push_back({"xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", sheetXml[i]});
    entries.push_back({"xl/styles.xml", stylesXml});
    entries.push_back({"xl/sharedStrings.xml", sstXml});
    return raster::writeZip(entries);
}

// ---- ODS ----------------------------------------------------------------------------

std::string odfFormula(const std::string& in) {
    std::string f = in;
    for (const char* pre : {"of:", "oooc:", "msoxl:"}) {
        if (startsWith(f, pre)) f = f.substr(std::char_traits<char>::length(pre));
    }
    if (!f.empty() && f[0] == '=') f.erase(0, 1);
    std::string out;
    bool str = false;
    for (std::size_t i = 0; i < f.size(); ++i) {
        char c = f[i];
        if (c == '"') str = !str;
        if (str) {
            out += c;
            continue;
        }
        if (c == ';') {
            out += ',';
            continue;
        }
        if (c == '[') {
            auto e = f.find(']', i);
            if (e == std::string::npos) {
                out += c;
                continue;
            }
            std::string ref = f.substr(i + 1, e - i - 1);  // ".A1:.B2" or "Sheet2.A1" or "$'My sheet'.A1"
            std::string res;
            std::size_t p = 0;
            bool firstPart = true;
            while (p <= ref.size()) {
                auto colon = ref.find(':', p);
                std::string part = ref.substr(p, colon == std::string::npos ? std::string::npos : colon - p);
                // split sheet and cell at the last '.' outside quotes
                std::string sheet, cell = part;
                bool q = false;
                std::size_t dot = std::string::npos;
                for (std::size_t k = 0; k < part.size(); ++k) {
                    if (part[k] == '\'') q = !q;
                    if (part[k] == '.' && !q) dot = k;
                }
                if (dot != std::string::npos) {
                    sheet = part.substr(0, dot);
                    cell = part.substr(dot + 1);
                }
                if (!sheet.empty() && sheet[0] == '$') sheet.erase(0, 1);
                if (sheet.size() >= 2 && sheet.front() == '\'' && sheet.back() == '\'') sheet = sheet.substr(1, sheet.size() - 2);
                if (!res.empty()) res += ":";
                if (firstPart && !sheet.empty()) res += quoteSheet(sheet) + "!";
                res += cell;
                firstPart = false;
                if (colon == std::string::npos) break;
                p = colon + 1;
            }
            out += res;
            i = e;
            continue;
        }
        out += c;
    }
    return out;
}

namespace {

// Text of a <text:p>, honouring <text:s text:c="n"/>, tabs and line breaks.
std::string odfText(const xml::Node& n) {
    std::string out;
    for (const auto& c : n.children) {
        if (c.isText()) out += c.text;
        else if (c.is("s")) out.append(static_cast<std::size_t>(std::max(1, std::atoi(c.attr("text:c").c_str()))), ' ');
        else if (c.is("tab")) out += '\t';
        else if (c.is("line-break")) out += '\n';
        else if (c.is("note") || c.is("annotation")) continue;
        else out += odfText(c);
    }
    return out;
}

double odfLength(const std::string& v) {
    double x = std::atof(v.c_str());
    if (endsWith(v, "cm")) return x / 2.54 * 72;
    if (endsWith(v, "mm")) return x / 25.4 * 72;
    if (endsWith(v, "in")) return x * 72;
    if (endsWith(v, "pt")) return x;
    return x;
}

}  // namespace

Workbook readOds(const std::string& bytes) {
    std::map<std::string, std::string> files;
    for (auto& e : raster::readZip(bytes)) files[e.name] = std::move(e.data);
    std::string content = zipEntry(files, "content.xml");
    if (content.empty()) throw std::runtime_error("not an OpenDocument file (no content.xml)");
    xml::Node root = xml::parse(content);
    // automatic styles: bold cells, column widths
    std::set<std::string> boldStyles;
    std::map<std::string, double> colWidthPt;
    std::map<std::string, std::string> cellFormats;
    if (const auto* as = root.child("automatic-styles")) {
        for (const auto* s : as->all("style")) {
            const std::string name = s->attr("style:name");
            if (const auto* tp = s->child("text-properties"); tp && tp->attr("fo:font-weight") == "bold") boldStyles.insert(name);
            if (const auto* cp = s->child("table-column-properties")) colWidthPt[name] = odfLength(cp->attr("style:column-width"));
        }
    }
    const xml::Node* body = root.child("body");
    const xml::Node* ss = body ? body->child("spreadsheet") : nullptr;
    if (!ss) throw std::runtime_error("not a spreadsheet (no office:spreadsheet)");
    Workbook wb;
    for (const auto* table : ss->all("table")) {
        Sheet sh;
        sh.name = table->attr("table:name");
        if (sh.name.empty()) sh.name = "Sheet" + std::to_string(wb.sheets.size() + 1);
        int row = 0, colCursor = 0;
        std::function<void(const xml::Node&)> walk = [&](const xml::Node& parent) {
            for (const auto& child : parent.children) {
                if (child.is("table-column")) {
                    int rep = std::max(1, std::atoi(child.attr("table:number-columns-repeated").c_str()));
                    auto w = colWidthPt.find(child.attr("table:style-name"));
                    for (int k = 0; k < rep && colCursor < 1024; ++k, ++colCursor) {
                        if (w != colWidthPt.end() && w->second > 0) sh.colWidths[colCursor] = std::round(w->second / 5.25 * 10) / 10;
                    }
                    if (rep > 1024) colCursor += rep;
                } else if (child.is("table-columns") || child.is("table-header-columns") || child.is("table-column-group")) {
                    walk(child);
                } else if (child.is("table-row")) {
                    int rep = std::max(1, std::atoi(child.attr("table:number-rows-repeated").c_str()));
                    bool hasContent = false;
                    for (const auto& c : child.children) {
                        if (c.is("table-cell") && (c.hasAttr("office:value-type") || c.hasAttr("table:formula") || !c.children.empty())) hasContent = true;
                    }
                    if (!hasContent) {
                        row += rep;
                        continue;
                    }
                    for (int k = 0; k < rep && row < kMaxRows; ++k, ++row) {
                        int col = 0;
                        for (const auto& c : child.children) {
                            if (!c.is("table-cell") && !c.is("covered-table-cell")) continue;
                            int crep = std::max(1, std::atoi(c.attr("table:number-columns-repeated").c_str()));
                            Cell cell;
                            std::string type = c.attr("office:value-type");
                            if (type == "float" || type == "percentage" || type == "currency") {
                                cell.value = CellValue::num(std::atof(c.attr("office:value").c_str()));
                                if (type == "percentage") cell.format = "0%";
                                if (type == "currency") cell.format = "#,##0.00";
                            } else if (type == "date") {
                                std::string d = c.attr("office:date-value");
                                int y, m, dd;
                                if (parseIsoDate(d.substr(0, 10), y, m, dd)) {
                                    double s = dateSerial(y, m, dd);
                                    if (d.size() >= 19) {
                                        s += (std::atoi(d.substr(11, 2).c_str()) * 3600 + std::atoi(d.substr(14, 2).c_str()) * 60 +
                                              std::atoi(d.substr(17, 2).c_str())) / 86400.0;
                                    }
                                    cell.value = CellValue::num(s);
                                    cell.format = d.size() > 10 ? "yyyy-mm-dd hh:mm" : "yyyy-mm-dd";
                                }
                            } else if (type == "boolean") {
                                cell.value = CellValue::boolean(c.attr("office:boolean-value") == "true");
                            } else if (type == "time") {
                                // PT12H30M00S
                                std::string t = c.attr("office:time-value");
                                double h = 0, mi = 0, s = 0;
                                std::sscanf(t.c_str(), "PT%lfH%lfM%lfS", &h, &mi, &s);
                                cell.value = CellValue::num((h * 3600 + mi * 60 + s) / 86400.0);
                                cell.format = "hh:mm:ss";
                            } else {
                                std::string text;
                                for (const auto* p : c.all("p")) text += (text.empty() ? "" : "\n") + odfText(*p);
                                if (!text.empty() || type == "string") cell.value = CellValue::str(text);
                            }
                            if (c.hasAttr("table:formula")) cell.formula = odfFormula(c.attr("table:formula"));
                            if (boldStyles.count(c.attr("table:style-name"))) cell.bold = true;
                            if (!cell.blank() && (!cell.value.empty() || cell.hasFormula())) {
                                for (int m = 0; m < crep && col + m < kMaxCols; ++m) sh.cells[Sheet::key(row, col + m)] = cell;
                            }
                            col += crep;
                        }
                    }
                } else if (child.is("table-rows") || child.is("table-header-rows") || child.is("table-row-group")) {
                    walk(child);
                }
            }
        };
        walk(*table);
        wb.sheets.push_back(std::move(sh));
    }
    if (wb.sheets.empty()) throw std::runtime_error("spreadsheet has no tables");
    return wb;
}

}  // namespace achclwss::office
