// Reading .xlsx: values, formulas (shared and array/dynamic), styles, column
// widths, row heights, hidden rows/columns, merges, frozen panes, auto-filter,
// conditional formats, data validation, defined names and charts.
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "wetwaresuite/raster/ora.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"
#include "xlsx_internal.hpp"

namespace wetwaresuite::office {

namespace {

using namespace xlsx;

bool flagOn(const xml::Node* n) {
    if (!n) return false;
    const std::string v = n->attr("val");
    return v != "0" && v != "false" && v != "none";
}

struct XfStyle {
    std::string format;
    CellStyle style;
};

struct StyleSheet {
    std::vector<XfStyle> xfs;
    std::vector<CellStyle> dxfs;
};

CellStyle fontStyle(const xml::Node& f, std::uint32_t defaultColor, double defaultSize) {
    CellStyle s;
    s.bold = flagOn(f.child("b"));
    s.italic = flagOn(f.child("i"));
    s.strike = flagOn(f.child("strike"));
    s.underline = f.child("u") && f.child("u")->attr("val") != "none";
    if (const auto* sz = f.child("sz")) {
        const double v = std::atof(sz->attr("val").c_str());
        if (v > 0 && std::fabs(v - defaultSize) > 1e-6) s.size = static_cast<float>(v);
    }
    const std::uint32_t c = readColor(f.child("color"));
    if (c != defaultColor) s.color = c;
    return s;
}

void borderStyle(const xml::Node& b, CellStyle& s) {
    static const char* sides[] = {"left", "right", "top", "bottom"};
    for (int i = 0; i < 4; ++i) {
        const xml::Node* side = b.child(sides[i]);
        if (!side) continue;
        s.border[i] = static_cast<std::uint8_t>(borderCode(side->attr("style")));
        if (s.border[i] && !hasColor(s.borderColor)) {
            const std::uint32_t c = readColor(side->child("color"));
            if (hasColor(c) && rgbOf(c) != 0) s.borderColor = c;
        }
    }
}

std::uint32_t fillColor(const xml::Node& f, bool dxf) {
    const xml::Node* p = f.child("patternFill");
    if (!p) return 0;
    const std::string type = p->attr("patternType");
    if (type == "none" || type == "gray125") return 0;
    if (dxf) {
        // differential fills put the solid colour in bgColor (fgColor in some writers)
        std::uint32_t c = readColor(p->child("bgColor"));
        return hasColor(c) ? c : readColor(p->child("fgColor"));
    }
    if (type.empty()) return 0;
    std::uint32_t c = readColor(p->child("fgColor"));
    return hasColor(c) ? c : readColor(p->child("bgColor"));
}

StyleSheet readStyles(const std::string& text) {
    StyleSheet out;
    if (text.empty()) return out;
    xml::Node n = xml::parse(text);
    std::map<int, std::string> custom;
    if (const auto* nf = n.child("numFmts")) {
        for (const auto* f : nf->all("numFmt")) custom[std::atoi(f->attr("numFmtId").c_str())] = f->attr("formatCode");
    }
    std::vector<CellStyle> fonts;
    if (const auto* fs = n.child("fonts")) {
        auto all = fs->all("font");
        std::uint32_t defColor = 0;
        double defSize = 11;
        if (!all.empty()) {
            defColor = readColor(all[0]->child("color"));
            if (const auto* sz = all[0]->child("sz")) defSize = std::atof(sz->attr("val").c_str());
        }
        for (const auto* f : all) fonts.push_back(fontStyle(*f, defColor, defSize));
    }
    std::vector<std::uint32_t> fills;
    if (const auto* fs = n.child("fills")) {
        for (const auto* f : fs->all("fill")) fills.push_back(fillColor(*f, false));
    }
    std::vector<CellStyle> borders;
    if (const auto* bs = n.child("borders")) {
        for (const auto* b : bs->all("border")) {
            CellStyle s;
            borderStyle(*b, s);
            borders.push_back(s);
        }
    }
    if (const auto* cx = n.child("cellXfs")) {
        for (const auto* xf : cx->all("xf")) {
            XfStyle x;
            const int id = std::atoi(xf->attr("numFmtId").c_str());
            if (custom.count(id)) x.format = custom[id];
            else if (builtinFormats().count(id)) x.format = builtinFormats().at(id);
            if (lower(x.format) == "general") x.format.clear();
            CellStyle& s = x.style;
            const std::size_t font = static_cast<std::size_t>(std::atoi(xf->attr("fontId").c_str()));
            if (font < fonts.size()) s = fonts[font];
            const std::size_t fill = static_cast<std::size_t>(std::atoi(xf->attr("fillId").c_str()));
            if (fill < fills.size()) s.fill = fills[fill];
            const std::size_t border = static_cast<std::size_t>(std::atoi(xf->attr("borderId").c_str()));
            if (border < borders.size()) {
                std::copy(std::begin(borders[border].border), std::end(borders[border].border), std::begin(s.border));
                s.borderColor = borders[border].borderColor;
            }
            if (const auto* al = xf->child("alignment")) {
                const std::string h = al->attr("horizontal"), v = al->attr("vertical");
                if (h == "left") s.align = 'l';
                else if (h == "center" || h == "centerContinuous" || h == "justify" || h == "distributed") s.align = 'c';
                else if (h == "right") s.align = 'r';
                if (v == "top") s.valign = 't';
                else if (v == "center" || v == "justify" || v == "distributed") s.valign = 'm';
                s.wrap = al->attr("wrapText") == "1" || al->attr("wrapText") == "true";
                s.indent = static_cast<std::uint8_t>(std::clamp(std::atoi(al->attr("indent").c_str()), 0, 15));
                if (s.indent && s.align == 'l' && h == "left") {
                    // left + indent is what an indented general cell is written as; keep the alignment
                }
            }
            out.xfs.push_back(std::move(x));
        }
    }
    if (const auto* ds = n.child("dxfs")) {
        for (const auto* d : ds->all("dxf")) {
            CellStyle s;
            if (const auto* f = d->child("font")) s = fontStyle(*f, 0, 0);
            s.size = 0;
            if (const auto* f = d->child("fill")) s.fill = fillColor(*f, true);
            if (const auto* b = d->child("border")) borderStyle(*b, s);
            out.dxfs.push_back(s);
        }
    }
    return out;
}

struct ArrayAnchor {
    int row, col;
    Range ref;
};

Cell readCell(const xml::Node& c, int row, int col, const std::vector<std::string>& shared, const StyleSheet& styles,
              std::map<std::string, std::pair<std::string, std::pair<int, int>>>& sharedF, std::vector<ArrayAnchor>& arrays) {
    Cell cell;
    const std::string t = c.attr("t");
    std::string v;
    if (const auto* vn = c.child("v")) v = vn->allText();
    if (t == "s") {
        const std::size_t i = static_cast<std::size_t>(std::atoi(v.c_str()));
        cell.value = CellValue::str(i < shared.size() ? shared[i] : std::string());
    } else if (t == "inlineStr") {
        if (const auto* is = c.child("is")) cell.value = CellValue::str(is->allText());
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
    if (const auto* f = c.child("f")) {
        std::string text = f->allText();
        const std::string ft = f->attr("t");
        if (ft == "shared") {
            const std::string si = f->attr("si");
            if (!text.empty()) {
                sharedF[si] = {text, {row, col}};
            } else if (auto it = sharedF.find(si); it != sharedF.end()) {
                text = offsetFormula(it->second.first, row - it->second.second.first, col - it->second.second.second);
            }
        } else if (ft == "array") {
            Range r;
            if (rangeRef(f->attr("ref"), r) && (r.rows() > 1 || r.cols() > 1)) arrays.push_back({row, col, r});
        }
        if (!text.empty()) cell.formula = fileFormula(text, false);
    }
    if (const std::string sattr = c.attr("s"); !sattr.empty()) {
        const std::size_t si = static_cast<std::size_t>(std::atoi(sattr.c_str()));
        if (si < styles.xfs.size()) {
            cell.format = styles.xfs[si].format;
            cell.style = styles.xfs[si].style;
        }
    }
    return cell;
}

void readViewsAndCols(const xml::Node& ws, Sheet& sh) {
    if (const auto* views = ws.child("sheetViews")) {
        if (const auto* view = views->child("sheetView")) {
            if (const auto* pane = view->child("pane")) {
                const std::string state = pane->attr("state");
                if (state == "frozen" || state == "frozenSplit") {
                    sh.freezeCols = std::max(0, static_cast<int>(std::atof(pane->attr("xSplit").c_str())));
                    sh.freezeRows = std::max(0, static_cast<int>(std::atof(pane->attr("ySplit").c_str())));
                }
            }
        }
    }
    if (const auto* cols = ws.child("cols")) {
        for (const auto* c : cols->all("col")) {
            const int mn = std::atoi(c->attr("min").c_str()), mx = std::atoi(c->attr("max").c_str());
            const double w = std::atof(c->attr("width").c_str());
            const bool hidden = c->attr("hidden") == "1" || c->attr("hidden") == "true";
            const bool custom = c->attr("customWidth") == "1" || c->attr("customWidth") == "true" || !hidden;
            if (mn < 1 || mx < mn || mx - mn >= 256) continue;  // a "rest of the sheet" span sets defaults only
            for (int k = mn; k <= mx; ++k) {
                if (w > 0 && custom) sh.colWidths[k - 1] = w;
                if (hidden) sh.hiddenCols.insert(k - 1);
            }
        }
    }
}

void readCondFormats(const xml::Node& ws, Sheet& sh, const StyleSheet& styles) {
    struct Rule {
        int priority;
        CondFormat cf;
    };
    std::vector<Rule> rules;
    for (const auto* block : ws.all("conditionalFormatting")) {
        std::string sq = block->attr("sqref");
        for (const auto* rule : block->all("cfRule")) {
            const std::string type = rule->attr("type");
            std::vector<std::string> formulas;
            for (const auto* f : rule->all("formula")) formulas.push_back(fileFormula(f->allText(), false));
            // each space-separated area of sqref becomes a rule of its own
            std::size_t p = 0;
            while (p < sq.size()) {
                auto sp = sq.find(' ', p);
                const std::string part = sq.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
                p = sp == std::string::npos ? sq.size() : sp + 1;
                CondFormat cf;
                if (!rangeRef(part, cf.range)) continue;
                cf.range.sheet = 0;
                const std::size_t dxf = static_cast<std::size_t>(std::atoi(rule->attr("dxfId").c_str()));
                if (rule->hasAttr("dxfId") && dxf < styles.dxfs.size()) cf.style = styles.dxfs[dxf];
                if (type == "cellIs") {
                    cf.type = "cell";
                    cf.op = opFromName(rule->attr("operator"));
                    if (!formulas.empty()) cf.value1 = ruleValue(formulas[0], false);
                    if (formulas.size() > 1) cf.value2 = ruleValue(formulas[1], false);
                } else if (type == "expression") {
                    cf.type = "formula";
                    if (!formulas.empty()) cf.value1 = formulas[0];
                } else if (type == "containsText" || type == "beginsWith" || type == "endsWith") {
                    cf.type = type == "containsText" ? "text" : type == "beginsWith" ? "begins" : "ends";
                    cf.value1 = rule->attr("text");
                } else if (type == "top10") {
                    cf.type = rule->attr("bottom") == "1" ? "bottom" : "top";
                    cf.value1 = rule->hasAttr("rank") ? rule->attr("rank") : "10";
                    cf.percent = rule->attr("percent") == "1";
                } else if (type == "aboveAverage") {
                    cf.type = rule->attr("aboveAverage") == "0" ? "below_average" : "above_average";
                } else if (type == "duplicateValues" || type == "uniqueValues") {
                    cf.type = type == "duplicateValues" ? "duplicate" : "unique";
                } else if (type == "containsBlanks" || type == "notContainsBlanks" || type == "containsErrors") {
                    cf.type = type == "containsBlanks" ? "blank" : type == "notContainsBlanks" ? "not_blank" : "errors";
                } else if (type == "colorScale") {
                    cf.type = "color_scale";
                    if (const auto* cs = rule->child("colorScale")) {
                        auto colors = cs->all("color");
                        if (colors.size() >= 2) {
                            cf.minColor = readColor(colors.front());
                            cf.maxColor = readColor(colors.back());
                            if (colors.size() >= 3) cf.midColor = readColor(colors[1]);
                        }
                    }
                } else if (type == "dataBar") {
                    cf.type = "data_bar";
                    if (const auto* db = rule->child("dataBar")) cf.maxColor = readColor(db->child("color"));
                } else {
                    continue;  // icon sets and time periods are not modelled
                }
                rules.push_back({std::atoi(rule->attr("priority").c_str()), cf});
            }
        }
    }
    // Excel applies priority 1 last-wins-first; the painter lets later rules win, so lowest priority goes first
    std::stable_sort(rules.begin(), rules.end(), [](const Rule& a, const Rule& b) { return a.priority > b.priority; });
    for (auto& r : rules) sh.condFormats.push_back(std::move(r.cf));
}

void readValidations(const xml::Node& ws, Sheet& sh) {
    const auto* dv = ws.child("dataValidations");
    if (!dv) return;
    for (const auto* v : dv->all("dataValidation")) {
        Validation val;
        std::string type = v->attr("type");
        if (type.empty() || type == "none") continue;
        val.type = type == "textLength" ? "length" : type;
        val.op = v->hasAttr("operator") ? opFromName(v->attr("operator")) : "between";
        val.allowBlank = v->attr("allowBlank") == "1" || v->attr("allowBlank") == "true";
        val.dropdown = !(v->attr("showDropDown") == "1" || v->attr("showDropDown") == "true");
        val.errorStyle = v->hasAttr("errorStyle") ? v->attr("errorStyle") : "stop";
        val.errorTitle = v->attr("errorTitle");
        val.error = v->attr("error");
        val.promptTitle = v->attr("promptTitle");
        val.prompt = v->attr("prompt");
        std::string f1, f2;
        if (const auto* f = v->child("formula1")) f1 = fileFormula(f->allText(), false);
        if (const auto* f = v->child("formula2")) f2 = fileFormula(f->allText(), false);
        if (val.type == "list") {
            val.value1 = ruleValue(f1, false);
        } else if (val.type == "custom") {
            val.value1 = f1;
        } else {
            val.value1 = ruleValue(f1, val.type == "date");
            val.value2 = ruleValue(f2, val.type == "date");
        }
        std::string sq = v->attr("sqref");
        std::size_t p = 0;
        while (p < sq.size()) {
            auto sp = sq.find(' ', p);
            const std::string part = sq.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
            p = sp == std::string::npos ? sq.size() : sp + 1;
            Validation one = val;
            if (rangeRef(part, one.range)) sh.validations.push_back(one);
        }
    }
}

void readWorksheet(const xml::Node& ws, Sheet& sh, const std::vector<std::string>& shared, const StyleSheet& styles) {
    readViewsAndCols(ws, sh);
    std::map<std::string, std::pair<std::string, std::pair<int, int>>> sharedF;
    std::vector<ArrayAnchor> arrays;
    if (const auto* sd = ws.child("sheetData")) {
        int rowIdx = -1;
        for (const auto* rowNode : sd->all("row")) {
            const std::string rattr = rowNode->attr("r");
            rowIdx = rattr.empty() ? rowIdx + 1 : std::atoi(rattr.c_str()) - 1;
            if (rowIdx < 0 || rowIdx >= kMaxRows) continue;
            const std::string custom = rowNode->attr("customHeight");
            if ((custom == "1" || custom == "true") && rowNode->hasAttr("ht")) sh.rowHeights[rowIdx] = std::atof(rowNode->attr("ht").c_str());
            if (rowNode->attr("hidden") == "1" || rowNode->attr("hidden") == "true") sh.hiddenRows.insert(rowIdx);
            int colIdx = -1;
            for (const auto* c : rowNode->all("c")) {
                int row = rowIdx, col = colIdx + 1;
                if (const std::string ref = c->attr("r"); !ref.empty()) cellRef(ref, row, col);
                colIdx = col;
                if (row < 0 || row >= kMaxRows || col < 0 || col >= kMaxCols) continue;
                Cell cell = readCell(*c, row, col, shared, styles, sharedF, arrays);
                if (!cell.blank()) sh.cells[Sheet::key(row, col)] = std::move(cell);
            }
        }
    }
    // values Excel stored for the cells an array formula spills into belong to that formula
    for (const auto& a : arrays) {
        auto it = sh.cells.lower_bound(Sheet::key(a.ref.r1, a.ref.c1));
        const auto end = sh.cells.upper_bound(Sheet::key(a.ref.r2, a.ref.c2));
        for (; it != end; ++it) {
            const int r = Sheet::rowOf(it->first), col = Sheet::colOf(it->first);
            if (!a.ref.contains(r, col) || (r == a.row && col == a.col)) continue;
            if (!it->second.hasFormula()) it->second.spilled = true;
        }
    }
    if (const auto* af = ws.child("autoFilter")) {
        if (rangeRef(af->attr("ref"), sh.autoFilter)) sh.hasAutoFilter = true;
    }
    if (const auto* mc = ws.child("mergeCells")) {
        for (const auto* m : mc->all("mergeCell")) {
            Range r;
            if (rangeRef(m->attr("ref"), r) && (r.rows() > 1 || r.cols() > 1)) sh.merges.push_back(r);
        }
    }
    readCondFormats(ws, sh, styles);
    readValidations(ws, sh);
}

std::vector<std::string> readSharedStrings(const std::string& sst) {
    std::vector<std::string> shared;
    if (sst.empty()) return shared;
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
    return shared;
}

}  // namespace

Workbook readXlsx(const std::string& bytes) {
    Files files;
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
    std::string sstPath = "xl/sharedStrings.xml", stylesPath = "xl/styles.xml";
    for (const auto& [id, target] : rels) {
        if (endsWith(target, "sharedStrings.xml")) sstPath = target;
        if (endsWith(target, "styles.xml")) stylesPath = target;
    }
    const std::vector<std::string> shared = readSharedStrings(zipEntry(files, sstPath));
    const StyleSheet styles = readStyles(zipEntry(files, stylesPath));

    Workbook wb;
    const xml::Node* sheetsNode = wbNode.child("sheets");
    if (!sheetsNode) throw std::runtime_error("workbook has no sheets");
    std::vector<std::pair<int, std::string>> drawings;
    for (const auto* s : sheetsNode->all("sheet")) {
        Sheet sh;
        sh.name = s->attr("name");
        std::string rid = s->attr("r:id");
        if (rid.empty()) rid = s->attr("id");
        auto it = rels.find(rid);
        const std::string part = it == rels.end() ? std::string() : it->second;
        const std::string data = part.empty() ? std::string() : zipEntry(files, part);
        if (!data.empty()) {
            xml::Node ws = xml::parse(data);
            readWorksheet(ws, sh, shared, styles);
            if (const auto* d = ws.child("drawing")) {
                auto srels = readRels(files, part);
                auto dr = srels.find(d->attr("r:id"));
                if (dr != srels.end()) drawings.push_back({static_cast<int>(wb.sheets.size()), dr->second});
            }
        }
        wb.sheets.push_back(std::move(sh));
    }
    if (wb.sheets.empty()) throw std::runtime_error("workbook has no sheets");
    if (const auto* views = wbNode.child("bookViews")) {
        if (const auto* v = views->child("workbookView")) {
            const int a = std::atoi(v->attr("activeTab").c_str());
            if (a >= 0 && a < static_cast<int>(wb.sheets.size())) wb.active = a;
        }
    }
    if (const auto* dn = wbNode.child("definedNames")) {
        for (const auto* d : dn->all("definedName")) {
            const std::string name = d->attr("name");
            if (startsWith(name, "_xlnm.") || d->attr("hidden") == "1" || !validName(name)) continue;
            DefinedName n;
            n.name = name;
            n.formula = fileFormula(d->allText(), false);
            n.comment = d->attr("comment");
            if (!n.formula.empty() && n.formula.find("#REF!") == std::string::npos) wb.names[upper(name)] = n;
        }
    }
    for (const auto& [sheet, path] : drawings) {
        try {
            readCharts(files, path, wb, sheet);
        } catch (const std::exception&) {
            // a chart we cannot read is dropped; the cells still load
        }
    }
    return wb;
}

}  // namespace wetwaresuite::office
