// Reading DOCX into the block document model: styles (built-in changes and your
// own), run and paragraph formats, lists with their numbering, tables with merged
// cells, images, headers and footers, footnotes, comments, tracked changes and the
// table of contents. Writing is in docx_write.cpp.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "text.hpp"
#include "xml.hpp"

namespace wetwaresuite::office {

namespace {

std::map<std::string, std::string> unzip(const std::string& bytes) {
    std::map<std::string, std::string> files;
    for (auto& e : raster::readZip(bytes)) files[e.name] = std::move(e.data);
    return files;
}

std::string entry(const std::map<std::string, std::string>& f, const std::string& name) {
    auto it = f.find(name);
    return it == f.end() ? std::string() : it->second;
}

bool onFlag(const xml::Node* n) {
    if (!n) return false;
    std::string v = n->attr("w:val");
    return v.empty() || (v != "0" && v != "false" && v != "none" && v != "off");
}

// -1 when the element is missing, else 0 or 1.
int triFlag(const xml::Node* n) { return n ? (onFlag(n) ? 1 : 0) : -1; }

double twipsAttr(const xml::Node* n, const char* key) { return n && n->hasAttr(key) ? std::atof(n->attr(key).c_str()) / 20.0 : 0; }

std::uint32_t hexColor(const std::string& v) {
    if (v.size() != 6) return 0;
    for (char c : v) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return 0;
    }
    return static_cast<std::uint32_t>(std::stoul(v, nullptr, 16)) | kColorSet;
}

std::uint32_t highlightColor(const std::string& v) {
    static const std::map<std::string, std::uint32_t> names = {
        {"yellow", 0xFFFF00},    {"green", 0x00FF00},      {"cyan", 0x00FFFF},     {"magenta", 0xFF00FF},  {"blue", 0x0000FF},
        {"red", 0xFF0000},       {"darkBlue", 0x000080},   {"darkCyan", 0x008080}, {"darkGreen", 0x008000}, {"darkMagenta", 0x800080},
        {"darkRed", 0x800000},   {"darkYellow", 0x808000}, {"darkGray", 0x808080}, {"lightGray", 0xC0C0C0}, {"black", 0x000000},
        {"white", 0xFFFFFF}};
    auto it = names.find(v);
    return it == names.end() ? 0 : it->second | kColorSet;
}

// "serif", "mono", "sans" for a font name.
std::string familyName(const std::string& font) {
    const std::string f = lower(font);
    if (f.empty()) return "";
    if (f.find("courier") != std::string::npos || f.find("consol") != std::string::npos || f.find("mono") != std::string::npos ||
        f.find("menlo") != std::string::npos || f.find("code") != std::string::npos) {
        return "mono";
    }
    for (const char* s : {"times", "georgia", "cambria", "garamond", "palatino", "book antiqua", "serif", "baskerville", "minion", "liberation serif"}) {
        if (f.find(s) != std::string::npos && f.find("sans") == std::string::npos) return "serif";
    }
    return "sans";
}

pdf::Family familyOf(const std::string& n) { return n == "serif" ? pdf::Family::Serif : n == "mono" ? pdf::Family::Mono : pdf::Family::Sans; }

char alignOf(const std::string& jc) {
    if (jc == "center") return 'c';
    if (jc == "right" || jc == "end") return 'r';
    if (jc == "both" || jc == "distribute" || jc == "justify") return 'j';
    if (jc == "left" || jc == "start") return 'l';
    return 0;
}

// Paragraph properties into a format (only what is written).
ParaFormat paraFormat(const xml::Node* pp) {
    ParaFormat f;
    if (!pp) return f;
    if (const auto* jc = pp->child("jc")) f.align = alignOf(jc->attr("w:val"));
    if (const auto* ind = pp->child("ind")) {
        f.indent = twipsAttr(ind, ind->hasAttr("w:left") ? "w:left" : "w:start");
        f.indentRight = twipsAttr(ind, ind->hasAttr("w:right") ? "w:right" : "w:end");
        if (ind->hasAttr("w:firstLine")) f.firstLine = twipsAttr(ind, "w:firstLine");
        if (ind->hasAttr("w:hanging")) f.firstLine = -twipsAttr(ind, "w:hanging");
    }
    if (const auto* sp = pp->child("spacing")) {
        if (sp->hasAttr("w:before")) f.before = twipsAttr(sp, "w:before");
        if (sp->hasAttr("w:after")) f.after = twipsAttr(sp, "w:after");
        const std::string rule = sp->attr("w:lineRule");
        if (sp->hasAttr("w:line") && (rule.empty() || rule == "auto")) f.lineSpacing = std::atof(sp->attr("w:line").c_str()) / 240.0;
    }
    f.keepWithNext = onFlag(pp->child("keepNext"));
    f.pageBreakBefore = onFlag(pp->child("pageBreakBefore"));
    return f;
}

// Header cells are drawn bold anyway: bold that covers a whole header cell is not kept as a run format.
void unboldHeader(Block& b) {
    if (!b.header || b.rows.empty()) return;
    for (auto& cell : b.rows[0]) {
        bool all = true;
        for (const auto& r : cell) all = all && (r.bold || trimCopy(r.text).empty());
        if (!all) continue;
        for (auto& r : cell) r.bold = false;
        mergeRuns(cell);
    }
}

struct ParsedStyle {
    std::string name, basedOn;
    int outline = -1;
    StyleDef def;
};

struct DocxReader {
    std::map<std::string, std::string> files;
    std::map<std::string, std::string> rels;  // id -> target (resolved under word/)
    std::map<std::string, ParsedStyle> styles;
    struct Level {
        std::string format = "bullet";
        int start = 1;
    };
    std::map<std::string, std::map<int, Level>> numLevels;  // numId -> level
    std::map<std::string, std::string> notes, endnotes;     // id -> inline Markdown
    std::map<std::string, int> commentIds;                  // file id -> our id
    std::string defaultFamily = "sans";
    double defaultSize = 11;
    TextDocument doc;
    int curComment = 0;
    bool fieldPlaceholders = false;  // headers and footers: PAGE and NUMPAGES become {page} and {pages}
    std::string lastNumId;

    // ---- styles, numbering, notes, comments ------------------------------------------

    void loadStyles() {
        std::string s = entry(files, "word/styles.xml");
        if (s.empty()) return;
        xml::Node root = xml::parse(s);
        if (const auto* dd = root.child("docDefaults")) {
            if (const auto* rp = dd->find("rPrDefault")) {
                if (const auto* f = rp->find("rFonts")) defaultFamily = familyName(f->attr("w:ascii"));
                if (const auto* sz = rp->find("sz")) defaultSize = std::atof(sz->attr("w:val").c_str()) / 2.0;
            }
        }
        if (defaultFamily.empty()) defaultFamily = "sans";
        if (defaultSize < 4 || defaultSize > 72) defaultSize = 11;
        for (const auto* st : root.all("style")) {
            if (st->attr("w:type") != "paragraph" && st->hasAttr("w:type")) continue;
            ParsedStyle p;
            std::string id = st->attr("w:styleId");
            if (const auto* n = st->child("name")) p.name = n->attr("w:val");
            if (const auto* b = st->child("basedOn")) p.basedOn = b->attr("w:val");
            const auto* pp = st->child("pPr");
            if (pp) {
                if (const auto* o = pp->child("outlineLvl")) p.outline = std::atoi(o->attr("w:val").c_str());
                p.def.para = paraFormat(pp);
                p.def.para.pageBreakBefore = false;
            }
            if (const auto* rp = st->child("rPr")) {
                if (const auto* f = rp->child("rFonts")) p.def.font = familyName(f->attr("w:ascii"));
                if (const auto* sz = rp->child("sz")) p.def.size = std::atof(sz->attr("w:val").c_str()) / 2.0;
                if (const auto* c = rp->child("color")) p.def.color = hexColor(c->attr("w:val"));
                p.def.bold = triFlag(rp->child("b"));
                p.def.italic = triFlag(rp->child("i"));
                p.def.underline = triFlag(rp->child("u"));
            }
            p.def.name = p.name.empty() ? id : p.name;
            styles[id] = p;
        }
    }

    std::string nameOf(const std::string& id) {
        auto it = styles.find(id);
        return lower(it != styles.end() && !it->second.name.empty() ? it->second.name : id);
    }

    // The built-in style a file style stands for ("" when none).
    std::string builtinFor(const std::string& id) {
        if (id.empty()) return "Normal";
        std::string b = styleIdOf(nameOf(id));
        if (isBuiltinStyle(b)) return b;
        b = styleIdOf(id);
        return isBuiltinStyle(b) ? b : "";
    }

    // Heading level (1..6) of a paragraph style, 0 if not a heading; follows basedOn.
    int headingLevel(const std::string& styleId) {
        std::string id = styleId;
        for (int hops = 0; hops < 8 && !id.empty(); ++hops) {
            const std::string name = nameOf(id);
            if (startsWith(name, "heading")) {
                int n = std::atoi(name.c_str() + (name.size() > 7 && name[7] == ' ' ? 8 : 7));
                if (n >= 1 && n <= 9) return std::min(n, 6);
            }
            if (name == "title" || name == "subtitle" || startsWith(name, "toc")) return 0;
            auto it = styles.find(id);
            if (it == styles.end()) break;
            if (it->second.outline >= 0 && it->second.outline < 9) return std::min(it->second.outline + 1, 6);
            id = it->second.basedOn;
        }
        return 0;
    }

    std::string styleKind(const std::string& styleId) {
        std::string id = styleId;
        for (int hops = 0; hops < 8 && !id.empty(); ++hops) {
            const std::string name = nameOf(id);
            if (name.find("quote") != std::string::npos) return "quote";
            if (name.find("code") != std::string::npos || name.find("source") != std::string::npos || name.find("preformatted") != std::string::npos) {
                return "code";
            }
            auto it = styles.find(id);
            id = it == styles.end() ? "" : it->second.basedOn;
        }
        return "";
    }

    bool tocStyle(const std::string& id) {
        const std::string n = nameOf(id);
        return n.size() >= 4 && startsWith(n, "toc") && std::isdigit(static_cast<unsigned char>(n.back())) && n.find("heading") == std::string::npos;
    }

    // Built-in styles that look different in this file become entries of doc.styles.
    void builtinChanges() {
        doc.page.font = familyOf(defaultFamily);
        doc.page.fontSize = defaultSize;
        std::vector<std::pair<std::string, const ParsedStyle*>> found;
        for (const auto& [id, p] : styles) {
            const std::string b = builtinFor(id);
            if (!b.empty() && !doc.styles.count(b)) found.emplace_back(b, &p);
        }
        // Normal first: the others start from it
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first == "Normal" && b.first != "Normal"; });
        for (const auto& [id, p] : found) {
            if (doc.styles.count(id)) continue;
            const ResolvedStyle base = resolveStyle(doc, id);
            const StyleDef& d = p->def;
            StyleDef keep;
            bool any = false;
            if (!d.font.empty() && familyOf(d.font) != base.font) keep.font = d.font, any = true;
            if (d.size > 0 && std::fabs(d.size - base.size) > 0.3) keep.size = d.size, any = true;
            if (d.color && rgbOf(d.color) != rgbOf(base.color)) keep.color = d.color, any = true;
            if (d.bold >= 0 && (d.bold != 0) != base.bold) keep.bold = d.bold, any = true;
            if (d.italic >= 0 && (d.italic != 0) != base.italic) keep.italic = d.italic, any = true;
            if (d.underline >= 0 && (d.underline != 0) != base.underline) keep.underline = d.underline, any = true;
            const ParaFormat& f = d.para;
            if (f.align && f.align != base.align) keep.para.align = f.align, any = true;
            if (f.indent && std::fabs(f.indent - base.indent) > 0.1) keep.para.indent = f.indent, any = true;
            if (f.firstLine && std::fabs(f.firstLine - base.firstLine) > 0.1) keep.para.firstLine = f.firstLine, any = true;
            if (f.before >= 0 && std::fabs(f.before - base.before) > 0.1) keep.para.before = f.before, any = true;
            if (f.after >= 0 && std::fabs(f.after - base.after) > 0.1) keep.para.after = f.after, any = true;
            if (f.lineSpacing > 0 && std::fabs(f.lineSpacing - base.lineSpacing) > 0.02) keep.para.lineSpacing = f.lineSpacing, any = true;
            if (any) {
                StyleDef s = keep;
                s.name = "";
                doc.styles[id] = s;
            }
        }
    }

    // A style of your own: kept with the styles it is based on.
    void useStyle(const std::string& id) {
        std::string cur = id;
        for (int hops = 0; hops < 8 && !cur.empty() && !doc.styles.count(cur) && !isBuiltinStyle(cur); ++hops) {
            auto it = styles.find(cur);
            if (it == styles.end()) break;
            StyleDef d = it->second.def;
            const std::string base = it->second.basedOn;
            const std::string bb = base.empty() ? "" : builtinFor(base);
            d.basedOn = base.empty() ? "" : (!bb.empty() ? bb : base);
            if (d.basedOn == "Normal") d.basedOn = "";
            doc.styles[cur] = d;
            cur = bb.empty() ? base : "";
        }
    }

    void loadNumbering() {
        std::string s = entry(files, "word/numbering.xml");
        if (s.empty()) return;
        xml::Node root = xml::parse(s);
        std::map<std::string, std::map<int, Level>> abstract;
        for (const auto* a : root.all("abstractNum")) {
            auto& levels = abstract[a->attr("w:abstractNumId")];
            for (const auto* l : a->all("lvl")) {
                Level lv;
                if (const auto* f = l->child("numFmt")) lv.format = f->attr("w:val");
                if (const auto* st = l->child("start")) lv.start = std::max(0, std::atoi(st->attr("w:val").c_str()));
                levels[std::atoi(l->attr("w:ilvl").c_str())] = lv;
            }
        }
        for (const auto* n : root.all("num")) {
            const auto* a = n->child("abstractNumId");
            if (!a) continue;
            auto levels = abstract[a->attr("w:val")];
            for (const auto* o : n->all("lvlOverride")) {
                const int il = std::atoi(o->attr("w:ilvl").c_str());
                if (const auto* so = o->child("startOverride")) levels[il].start = std::atoi(so->attr("w:val").c_str());
            }
            numLevels[n->attr("w:numId")] = levels;
        }
    }

    std::string noteText(const xml::Node& note) {
        Para p;
        const int saved = curComment;
        for (const auto* para : note.all("p")) {
            if (!p.runs.empty()) {
                Run sp;
                sp.text = " ";
                p.runs.push_back(sp);
            }
            collectRuns(*para, p, "", 0);
        }
        curComment = saved;
        for (auto& r : p.runs) r.comment = 0, r.change = 0, r.footnote.clear();
        mergeRuns(p.runs);
        std::string md = trimCopy(inlineMarkdown(p.runs));
        return md.empty() ? std::string("(note)") : md;
    }

    void loadNotes(const std::string& file, const char* element, std::map<std::string, std::string>& out) {
        std::string s = entry(files, file);
        if (s.empty()) return;
        xml::Node root = xml::parse(s);
        for (const auto* n : root.all(element)) {
            const std::string type = n->attr("w:type");
            if (!type.empty() && type != "normal") continue;
            out[n->attr("w:id")] = noteText(*n);
        }
    }

    void loadComments() {
        std::string s = entry(files, "word/comments.xml");
        if (s.empty()) return;
        xml::Node root = xml::parse(s);
        for (const auto* c : root.all("comment")) {
            Comment cm;
            cm.id = static_cast<int>(doc.comments.size()) + 1;
            cm.author = c->attr("w:author");
            cm.date = c->attr("w:date");
            std::string text;
            for (const auto* p : c->all("p")) {
                Para para;
                collectRuns(*p, para, "", 0);
                for (auto& r : para.runs) r.comment = 0, r.change = 0, r.footnote.clear();
                mergeRuns(para.runs);
                std::string line = trimCopy(inlineMarkdown(para.runs));
                if (line.empty()) continue;
                if (!text.empty()) text += "\n";
                text += line;
            }
            cm.text = text;
            commentIds[c->attr("w:id")] = cm.id;
            doc.comments.push_back(cm);
        }
        curComment = 0;
    }

    int trackChange(const xml::Node& n, bool insert) {
        TrackedChange ch;
        ch.id = doc.nextChangeId();
        ch.insert = insert;
        ch.author = n.attr("w:author");
        ch.date = n.attr("w:date");
        doc.changes.push_back(ch);
        return ch.id;
    }

    // ---- paragraphs ----------------------------------------------------------------

    struct Para {
        Runs runs;
        std::vector<Block> images;  // images found in the paragraph
        bool pageBreakAfter = false;
        bool toc = false;
        int tocLevel = 3;
        // complex fields: <w:fldChar begin/> instr <w:fldChar separate/> result <w:fldChar end/>
        int fieldDepth = 0;
        bool fieldResult = false;
        bool skipResult = false;
        std::string instr;
    };

    Run runStyle(const xml::Node* rp, const std::string& link, int change) {
        Run base;
        base.link = link;
        base.change = change;
        base.comment = curComment;
        if (!rp) return base;
        bool hyperlinkStyle = !link.empty();
        if (const auto* rs = rp->child("rStyle")) {
            std::string sv = lower(rs->attr("w:val"));
            if (sv.find("code") != std::string::npos || sv.find("verbatim") != std::string::npos) base.code = true;
            if (sv == "strong") base.bold = true;
            if (sv == "emphasis") base.italic = true;
            if (sv.find("hyperlink") != std::string::npos) hyperlinkStyle = true;
            if (sv.find("footnotereference") != std::string::npos || sv.find("endnotereference") != std::string::npos) base.vertical = 0;
        }
        if (const auto* b = rp->child("b")) base.bold = onFlag(b);
        if (const auto* i = rp->child("i")) base.italic = onFlag(i);
        base.underline = onFlag(rp->child("u")) && !hyperlinkStyle;
        base.strike = onFlag(rp->child("strike")) || onFlag(rp->child("dstrike"));
        if (const auto* f = rp->child("rFonts")) {
            const std::string fam = familyName(f->hasAttr("w:ascii") ? f->attr("w:ascii") : f->attr("w:hAnsi"));
            if (fam == "mono") base.code = true;
            else if (!fam.empty() && fam != defaultFamily) base.font = fam;
        }
        if (const auto* c = rp->child("color")) {
            const std::uint32_t col = hexColor(c->attr("w:val"));
            if (col && !(hyperlinkStyle && rgbOf(col) == 0x0563C1)) base.color = col;
        }
        if (const auto* sz = rp->child("sz")) {
            const double v = std::atof(sz->attr("w:val").c_str()) / 2.0;
            if (v >= 1 && std::fabs(v - defaultSize) > 0.01) base.size = v;
        }
        if (const auto* h = rp->child("highlight")) base.highlight = highlightColor(h->attr("w:val"));
        if (const auto* sh = rp->child("shd")) {
            if (const std::uint32_t f = hexColor(sh->attr("w:fill"))) base.highlight = f;
        }
        if (const auto* va = rp->child("vertAlign")) {
            const std::string v = va->attr("w:val");
            base.vertical = v == "superscript" ? 1 : v == "subscript" ? -1 : 0;
        }
        return base;
    }

    void image(const xml::Node& part, Para& p) {
        std::vector<const xml::Node*> blips;
        part.descendants("blip", blips);
        std::vector<const xml::Node*> imagedata;
        part.descendants("imagedata", imagedata);
        std::string rid = !blips.empty() ? blips[0]->attr("r:embed") : !imagedata.empty() ? imagedata[0]->attr("r:id") : "";
        auto it = rels.find(rid);
        if (it == rels.end()) return;
        std::string bytes = entry(files, it->second);
        if (bytes.empty()) return;
        Block b;
        try {
            setImage(b, bytes);
            if (const auto* ext = part.find("extent")) b.width = std::atof(ext->attr("cx").c_str()) / 12700.0;
            if (const auto* dp = part.find("docPr")) b.alt = dp->attr("descr");
            p.images.push_back(std::move(b));
        } catch (const std::exception&) {
        }
    }

    void fieldChar(const xml::Node& part, Para& p) {
        const std::string t = part.attr("w:fldCharType");
        if (t == "begin") {
            if (++p.fieldDepth == 1) {
                p.instr.clear();
                p.fieldResult = false;
                p.skipResult = false;
            }
        } else if (t == "separate" && p.fieldDepth == 1) {
            p.fieldResult = true;
            fieldStart(p.instr, p);
        } else if (t == "end" && p.fieldDepth > 0) {
            if (p.fieldDepth == 1 && !p.fieldResult) fieldStart(p.instr, p);  // a field with no result
            --p.fieldDepth;
            p.skipResult = false;
        }
    }

    // Placeholders for page fields in headers and footers; tables of contents.
    void fieldStart(const std::string& instruction, Para& p) {
        const std::string in = upper(trimCopy(instruction));
        if (startsWith(in, "TOC")) {
            p.toc = true;
            const auto o = in.find("\\O");
            if (o != std::string::npos) {
                const auto dash = in.find('-', o);
                if (dash != std::string::npos) p.tocLevel = std::clamp(std::atoi(in.c_str() + dash + 1), 1, 9);
            }
            return;
        }
        if (!fieldPlaceholders) return;
        const char* ph = startsWith(in, "PAGE") ? "{page}" : startsWith(in, "NUMPAGES") || startsWith(in, "SECTIONPAGES") ? "{pages}" : nullptr;
        if (!ph || p.skipResult) return;
        Run r;
        r.text = ph;
        p.runs.push_back(r);
        p.skipResult = true;
    }

    void collectRuns(const xml::Node& n, Para& p, const std::string& link, int change) {
        for (const auto& c : n.children) {
            if (c.is("r")) {
                const Run base = runStyle(c.child("rPr"), link, change);
                for (const auto& part : c.children) {
                    Run r = base;
                    if (part.is("fldChar")) {
                        fieldChar(part, p);
                        continue;
                    }
                    if (part.is("instrText")) {
                        p.instr += part.allText();
                        continue;
                    }
                    if (p.skipResult) continue;
                    if (part.is("t") || part.is("delText")) r.text = part.allText();
                    else if (part.is("tab") || part.is("ptab")) r.text = "\t";
                    else if (part.is("br") || part.is("cr")) {
                        if (part.attr("w:type") == "page") {
                            p.pageBreakAfter = true;
                            continue;
                        }
                        r.text = "\n";
                    } else if (part.is("noBreakHyphen")) r.text = "-";
                    else if (part.is("sym")) r.text = "?";
                    else if (part.is("footnoteReference") || part.is("endnoteReference")) {
                        const auto& m = part.is("footnoteReference") ? notes : endnotes;
                        auto it = m.find(part.attr("w:id"));
                        r.footnote = it != m.end() ? it->second : "(note)";
                        r.vertical = 0;
                        r.text.clear();
                    } else if (part.is("drawing") || part.is("pict") || part.is("object")) {
                        image(part, p);
                        continue;
                    } else {
                        continue;
                    }
                    p.runs.push_back(r);
                }
            } else if (c.is("hyperlink")) {
                std::string target;
                auto it = rels.find(c.attr("r:id"));
                if (it != rels.end()) target = it->second;
                else if (c.hasAttr("w:anchor")) target = "#" + c.attr("w:anchor");
                collectRuns(c, p, target, change);
            } else if (c.is("ins") || c.is("moveTo")) {
                collectRuns(c, p, link, trackChange(c, true));
            } else if (c.is("del") || c.is("moveFrom")) {
                collectRuns(c, p, link, trackChange(c, false));
            } else if (c.is("commentRangeStart")) {
                auto it = commentIds.find(c.attr("w:id"));
                curComment = it == commentIds.end() ? 0 : it->second;
            } else if (c.is("commentRangeEnd")) {
                curComment = 0;
            } else if (c.is("fldSimple")) {
                fieldStart(c.attr("w:instr"), p);
                if (p.skipResult) {
                    p.skipResult = false;  // a simple field's result is its own children
                } else {
                    collectRuns(c, p, link, change);
                }
            } else if (c.is("smartTag") || c.is("customXml") || c.is("sdt") || c.is("sdtContent") || c.is("bdo") || c.is("dir")) {
                collectRuns(c, p, link, change);
            }
        }
    }

    // Starts a list block for a numbered or bulleted paragraph.
    void listItem(const std::string& numId, int ilvl, const Runs& runs, std::vector<Block>& out) {
        Level lv;
        if (numLevels.count(numId)) {
            auto& levels = numLevels[numId];
            if (levels.count(0)) lv = levels[0];
        }
        const bool ordered = lv.format != "bullet" && lv.format != "none";
        const std::string fmt = !ordered ? "" : lv.format == "decimal" || lv.format.empty() ? "" : lv.format;
        const bool known = fmt.empty() || fmt == "lowerLetter" || fmt == "upperLetter" || fmt == "lowerRoman" || fmt == "upperRoman";
        if (out.empty() || out.back().kind != Block::List || out.back().ordered != ordered || (ordered && numId != lastNumId)) {
            Block b;
            b.kind = Block::List;
            b.ordered = ordered;
            b.numbering = known ? fmt : "";
            b.start = ordered ? std::max(0, lv.start) : 1;
            out.push_back(b);
        }
        lastNumId = numId;
        ListItem it;
        it.level = std::clamp(ilvl, 0, 8);
        it.runs = runs;
        out.back().items.push_back(it);
    }

    void paragraph(const xml::Node& p, std::vector<Block>& out) {
        Para para;
        std::string style;
        std::string numId;
        int ilvl = 0;
        ParaFormat fmt;
        const auto* pp = p.child("pPr");
        if (pp) {
            if (const auto* ps = pp->child("pStyle")) style = ps->attr("w:val");
            if (const auto* np = pp->child("numPr")) {
                if (const auto* id = np->child("numId")) numId = id->attr("w:val");
                if (const auto* lv = np->child("ilvl")) ilvl = std::atoi(lv->attr("w:val").c_str());
            }
            fmt = paraFormat(pp);
        }
        collectRuns(p, para, "", 0);
        mergeRuns(para.runs);
        if (para.toc || tocStyle(style) || nameOf(style) == "toc heading") {  // a table of contents: one block
            if (out.empty() || out.back().kind != Block::Toc) {
                Block t;
                t.kind = Block::Toc;
                t.level = para.toc ? para.tocLevel : 3;
                if (nameOf(style) == "toc heading" && !trimCopy(plainText(para.runs)).empty()) t.runs = para.runs;
                out.push_back(t);
            } else if (para.toc) {
                out.back().level = para.tocLevel;
            }
            return;
        }
        if (fmt.pageBreakBefore && (!numId.empty() && numId != "0")) {
            Block pb;
            pb.kind = Block::PageBreak;
            out.push_back(pb);
        }
        bool hasText = false;
        for (const auto& r : para.runs) hasText = hasText || !r.footnote.empty() || !trimCopy(r.text).empty();
        if (hasText) {
            const int h = headingLevel(style);
            if (!numId.empty() && numId != "0" && !h) {
                listItem(numId, ilvl, para.runs, out);
            } else {
                Block b;
                const std::string builtin = builtinFor(style);
                if (h) {
                    b.kind = Block::Heading;
                    b.level = h;
                } else if (builtin == "Quote" || (builtin.empty() && styleKind(style) == "quote")) {
                    b.kind = Block::Quote;
                } else if (builtin == "Code" || (builtin.empty() && styleKind(style) == "code")) {
                    b.kind = Block::Code;
                } else {
                    b.kind = Block::Paragraph;
                    if (builtin == "Title" || builtin == "Subtitle" || builtin == "Caption") b.style = builtin;
                    else if (builtin.empty() && !style.empty()) {
                        useStyle(style);
                        if (doc.styles.count(style)) b.style = style;
                    }
                }
                b.runs = para.runs;
                b.fmt = fmt;
                if (b.kind == Block::Heading) b.fmt.keepWithNext = false;
                if (b.kind == Block::Code) {
                    Run r;
                    r.text = plainText(b.runs);
                    r.code = true;
                    b.runs = {r};
                    b.fmt = ParaFormat{};
                    if (!out.empty() && out.back().kind == Block::Code) {  // one paragraph per line in many files
                        out.back().runs[0].text += "\n" + r.text;
                        b.kind = Block::PageBreak;  // marker: nothing to add
                    }
                }
                if (b.kind != Block::PageBreak) out.push_back(std::move(b));
            }
        } else if (fmt.pageBreakBefore) {
            Block pb;
            pb.kind = Block::PageBreak;
            out.push_back(pb);
        }
        for (auto& img : para.images) {
            if (!hasText) img.fmt.align = fmt.align;
            out.push_back(std::move(img));
        }
        if (para.pageBreakAfter) {
            Block pb;
            pb.kind = Block::PageBreak;
            out.push_back(pb);
        }
    }

    Runs cellRuns(const xml::Node& tc, char& align) {
        Runs all;
        bool first = true;
        std::vector<const xml::Node*> ps;
        tc.descendants("p", ps);
        align = 0;
        for (const auto* p : ps) {
            Para para;
            collectRuns(*p, para, "", 0);
            if (first) {
                if (const auto* pp = p->child("pPr")) {
                    if (const auto* jc = pp->child("jc")) align = alignOf(jc->attr("w:val"));
                }
            }
            if (!first && !para.runs.empty()) {
                Run nl;
                nl.text = "\n";
                all.push_back(nl);
            }
            for (auto& r : para.runs) all.push_back(r);
            if (!para.runs.empty()) first = false;
        }
        mergeRuns(all);
        if (align == 'l') align = 0;
        return all;
    }

    void table(const xml::Node& c, std::vector<Block>& out) {
        Block b;
        b.kind = Block::Table;
        b.header = true;  // the first row reads as a header, as most tables are written
        std::string tblStyle;
        bool fixed = false, sawHeaderRow = false, firstRowOff = false, hBand = false;
        if (const auto* pr = c.child("tblPr")) {
            if (const auto* ts = pr->child("tblStyle")) tblStyle = ts->attr("w:val");
            if (const auto* lay = pr->child("tblLayout")) fixed = lay->attr("w:type") == "fixed";
            if (const auto* look = pr->child("tblLook")) {
                firstRowOff = look->attr("w:firstRow") == "0";
                hBand = look->attr("w:noHBand") == "0";
            }
        }
        std::vector<double> grid;
        if (const auto* g = c.child("tblGrid")) {
            for (const auto* gc : g->all("gridCol")) grid.push_back(std::max(0.0, twipsAttr(gc, "w:w")));
        }
        const bool ours = tblStyle == "TableGrid" || tblStyle == "TablePlain";
        b.borders = tblStyle != "TablePlain";
        b.banded = ours && hBand;
        std::vector<const xml::Node*> trs = c.all("tr");
        for (std::size_t r = 0; r < trs.size(); ++r) {
            const xml::Node* tr = trs[r];
            if (const auto* trPr = tr->child("trPr")) {
                if (r == 0 && trPr->child("tblHeader")) sawHeaderRow = true;
            }
            std::vector<Runs> row;
            std::vector<CellFormat> formats;
            auto addCell = [&](const xml::Node& cell) {
                int span = 1;
                std::string vmerge;
                bool vm = false;
                CellFormat f;
                if (const auto* pr = cell.child("tcPr")) {
                    if (const auto* gs = pr->child("gridSpan")) span = std::max(1, std::atoi(gs->attr("w:val").c_str()));
                    if (const auto* v = pr->child("vMerge")) {
                        vm = true;
                        vmerge = v->attr("w:val");
                    }
                    if (const auto* sh = pr->child("shd")) f.fill = hexColor(sh->attr("w:fill"));
                    if (const auto* va = pr->child("vAlign")) {
                        const std::string v = va->attr("w:val");
                        f.valign = v == "center" ? 'm' : v == "bottom" ? 'b' : 0;
                    }
                }
                // shading our writer adds for header rows and bands is not a cell format
                if (ours && f.fill && ((r == 0 && rgbOf(f.fill) == 0xEEEEEE) || rgbOf(f.fill) == 0xF6F8FB)) f.fill = 0;
                const int col = static_cast<int>(row.size());
                char align = 0;
                Runs content = cellRuns(cell, align);
                f.align = align;
                if (vm && vmerge != "restart") {  // continues the merge above
                    content.clear();
                    bool extended = false;
                    for (auto& m : b.merges) {
                        if (m.col == col && m.row + m.rows == static_cast<int>(r)) {
                            ++m.rows;
                            extended = true;
                            break;
                        }
                    }
                    if (!extended && r > 0) b.merges.push_back({static_cast<int>(r) - 1, col, 2, span});
                } else if ((vm && vmerge == "restart") || span > 1) {
                    b.merges.push_back({static_cast<int>(r), col, 1, span});
                }
                row.push_back(content);
                formats.push_back(f);
                for (int k = 1; k < span; ++k) {
                    row.emplace_back();
                    formats.emplace_back();
                }
            };
            for (const auto& cell : tr->children) {
                if (cell.is("tc")) addCell(cell);
                else if (cell.is("sdt")) {
                    for (const auto* sc : cell.all("sdtContent")) {
                        for (const auto* x : sc->all("tc")) addCell(*x);
                    }
                }
            }
            b.rows.push_back(row);
            b.cellFormats.push_back(formats);
        }
        // merges that never grew past one cell are nothing
        b.merges.erase(std::remove_if(b.merges.begin(), b.merges.end(), [](const CellSpan& m) { return m.rows <= 1 && m.cols <= 1; }), b.merges.end());
        std::size_t cols = 0;
        for (const auto& r : b.rows) cols = std::max(cols, r.size());
        for (auto& r : b.rows) r.resize(cols);
        bool anyFormat = false;
        for (auto& r : b.cellFormats) {
            r.resize(cols);
            for (const auto& f : r) anyFormat = anyFormat || f.fill || f.align || f.valign;
        }
        if (!anyFormat) b.cellFormats.clear();
        if (fixed && grid.size() == cols) b.colWidths = grid;
        if (!sawHeaderRow && firstRowOff) b.header = false;
        unboldHeader(b);
        if (!b.rows.empty()) out.push_back(std::move(b));
    }

    void body(const xml::Node& n, std::vector<Block>& out) {
        for (const auto& c : n.children) {
            if (c.is("p")) paragraph(c, out);
            else if (c.is("tbl")) table(c, out);
            else if (c.is("sdt")) {
                bool isToc = false;
                if (const auto* pr = c.child("sdtPr")) {
                    if (const auto* g = pr->find("docPartGallery")) isToc = g->attr("w:val") == "Table of Contents";
                }
                const auto* sc = c.child("sdtContent");
                if (!sc) continue;
                if (!isToc) {
                    body(*sc, out);
                    continue;
                }
                Block t;
                t.kind = Block::Toc;
                t.level = 3;
                std::vector<Block> inner;
                body(*sc, inner);
                for (const auto& x : inner) {
                    if (x.kind == Block::Toc) {
                        t.level = x.level;
                        if (!x.runs.empty()) t.runs = x.runs;
                    } else if (t.runs.empty() && (x.kind == Block::Paragraph || x.kind == Block::Heading)) {
                        t.runs = x.runs;  // the title, in files that give it a plain style
                    }
                }
                if (plainText(t.runs) == "Contents") t.runs.clear();
                out.push_back(t);
            } else if (c.is("customXml") || c.is("ins")) {
                body(c, out);
            }
        }
    }

    // A header or footer part as one line of text: tabs become " | ", page fields {page} and {pages}.
    std::string band(const std::string& rid) {
        auto it = rels.find(rid);
        if (it == rels.end()) return "";
        std::string x = entry(files, it->second);
        if (x.empty()) return "";
        xml::Node root = xml::parse(x);
        fieldPlaceholders = true;
        std::string out;
        std::vector<const xml::Node*> ps;
        root.descendants("p", ps);
        for (const auto* p : ps) {
            Para para;
            collectRuns(*p, para, "", 0);
            std::vector<Runs> parts(1);
            for (auto r : para.runs) {
                std::size_t from = 0;
                for (std::size_t k = 0; k <= r.text.size(); ++k) {
                    if (k == r.text.size() || r.text[k] == '\t') {
                        Run piece = r;
                        piece.text = r.text.substr(from, k - from);
                        piece.comment = piece.change = 0;
                        if (!piece.text.empty()) parts.back().push_back(piece);
                        if (k < r.text.size()) parts.emplace_back();
                        from = k + 1;
                    }
                }
            }
            std::string line;
            for (std::size_t k = 0; k < parts.size(); ++k) {
                mergeRuns(parts[k]);
                std::string md = trimCopy(inlineMarkdown(parts[k]));
                std::string esc;
                for (char ch : md) {
                    if (ch == '|') esc += '\\';
                    esc += ch;
                }
                if (k) line += " | ";
                line += esc;
            }
            if (trimCopy(line).empty() || line == " | " || line == " |  | ") continue;
            if (!out.empty()) out += " ";
            out += line;
        }
        fieldPlaceholders = false;
        return trimCopy(out);
    }
};

}  // namespace

TextDocument readDocx(const std::string& bytes) {
    DocxReader r;
    r.files = unzip(bytes);
    std::string docPath = "word/document.xml";
    if (std::string root = entry(r.files, "_rels/.rels"); !root.empty()) {
        xml::Node n = xml::parse(root);
        for (const auto* rel : n.all("Relationship")) {
            if (endsWith(rel->attr("Type"), "/officeDocument")) {
                std::string t = rel->attr("Target");
                docPath = t.empty() ? docPath : (t[0] == '/' ? t.substr(1) : t);
            }
        }
    }
    std::string docXml = entry(r.files, docPath);
    if (docXml.empty()) throw std::runtime_error("not a .docx document (no " + docPath + ")");
    std::string dir = docPath.substr(0, docPath.rfind('/') + 1);
    std::string relPath = dir + "_rels/" + docPath.substr(docPath.rfind('/') + 1) + ".rels";
    if (std::string rx = entry(r.files, relPath); !rx.empty()) {
        xml::Node n = xml::parse(rx);
        for (const auto* rel : n.all("Relationship")) {
            std::string t = rel->attr("Target");
            if (rel->attr("TargetMode") == "External") r.rels[rel->attr("Id")] = t;
            else r.rels[rel->attr("Id")] = t.size() && t[0] == '/' ? t.substr(1) : dir + t;
        }
    }
    try {
        r.loadStyles();
    } catch (const std::exception&) {
    }
    r.builtinChanges();
    try {
        r.loadNumbering();
    } catch (const std::exception&) {
    }
    try {
        r.loadNotes(dir + "footnotes.xml", "footnote", r.notes);
        r.loadNotes(dir + "endnotes.xml", "endnote", r.endnotes);
        r.loadComments();
    } catch (const std::exception&) {
    }
    r.doc.changes.clear();  // changes inside notes and comments are not kept
    xml::Node root = xml::parse(docXml);
    const xml::Node* body = root.child("body");
    if (!body) throw std::runtime_error("document has no body");
    r.body(*body, r.doc.blocks);
    if (const auto* sect = body->child("sectPr")) {
        if (const auto* sz = sect->child("pgSz")) {
            double w = std::atof(sz->attr("w:w").c_str()) / 20, h = std::atof(sz->attr("w:h").c_str()) / 20;
            if (w > 100 && h > 100) {
                r.doc.page.width = w;
                r.doc.page.height = h;
            }
        }
        if (const auto* m = sect->child("pgMar")) {
            auto margin = [&](const char* key, double& out) {
                double v = std::fabs(std::atof(m->attr(key).c_str())) / 20;
                if (m->hasAttr(key) && v < 400) out = v;
            };
            margin("w:top", r.doc.page.top);
            margin("w:bottom", r.doc.page.bottom);
            margin("w:left", r.doc.page.left);
            margin("w:right", r.doc.page.right);
        }
        HeaderFooter& hf = r.doc.headerFooter;
        hf.differentFirst = sect->child("titlePg") && onFlag(sect->child("titlePg"));
        bool anyFooter = false;
        for (const auto& c : sect->children) {
            const bool head = c.is("headerReference"), foot = c.is("footerReference");
            if (!head && !foot) continue;
            const std::string type = c.attr("w:type");
            std::string text;
            try {
                text = r.band(c.attr("r:id"));
            } catch (const std::exception&) {
                continue;
            }
            if (type == "first") (head ? hf.firstHeader : hf.firstFooter) = text;
            else if (type.empty() || type == "default") (head ? hf.header : hf.footer) = text;
            if (foot && (type.empty() || type == "default")) anyFooter = true;
        }
        if (!hf.differentFirst) hf.firstHeader.clear(), hf.firstFooter.clear();
        if (hf.footer == "{page} / {pages}") hf.footer.clear();  // the default page numbers
        else if (!anyFooter) r.doc.page.pageNumbers = false;
    } else {
        r.doc.page.pageNumbers = false;
    }
    if (std::string settings = entry(r.files, dir + "settings.xml"); !settings.empty()) {
        try {
            xml::Node s = xml::parse(settings);
            r.doc.trackChanges = onFlag(s.child("trackRevisions"));
        } catch (const std::exception&) {
        }
    }
    if (std::string core = entry(r.files, "docProps/core.xml"); !core.empty()) {
        try {
            xml::Node c = xml::parse(core);
            if (const auto* t = c.child("title")) r.doc.title = t->allText();
            if (const auto* a = c.child("creator")) r.doc.author = a->allText();
        } catch (const std::exception&) {
        }
    }
    return r.doc;
}

}  // namespace wetwaresuite::office
