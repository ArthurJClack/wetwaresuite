// OpenDocument text (.odt), read and written natively: styles, run and paragraph
// formats, lists with their numbering, tables with merged cells, images, headers
// and footers with page fields, footnotes, comments, tracked changes (read; the
// writer saves the accepted text) and the table of contents.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
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

// "2.5cm", "10mm", "1in", "12pt", "1pc", "96px" to points; 0 when empty.
double lengthPt(const std::string& s) {
    if (s.empty()) return 0;
    const double v = std::atof(s.c_str());
    if (endsWith(s, "cm")) return v / 2.54 * 72;
    if (endsWith(s, "mm")) return v / 25.4 * 72;
    if (endsWith(s, "in") || endsWith(s, "inch")) return v * 72;
    if (endsWith(s, "pc")) return v * 12;
    if (endsWith(s, "px")) return v * 0.75;
    return v;
}

std::uint32_t odfColor(const std::string& v) {
    if (v.size() != 7 || v[0] != '#') return 0;
    for (std::size_t i = 1; i < 7; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(v[i]))) return 0;
    }
    return static_cast<std::uint32_t>(std::stoul(v.substr(1), nullptr, 16)) | kColorSet;
}

std::string familyName(const std::string& font) {
    const std::string f = lower(font);
    if (f.empty()) return "";
    if (f.find("mono") != std::string::npos || f.find("courier") != std::string::npos || f.find("consol") != std::string::npos) return "mono";
    for (const char* s : {"serif", "times", "georgia", "cambria", "garamond", "palatino"}) {
        if (f.find(s) != std::string::npos && f.find("sans") == std::string::npos) return "serif";
    }
    return "sans";
}

// Style names in ODF replace spaces with _20_ (and other characters with _xx_).
std::string displayOf(const std::string& name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '_' && i + 3 < name.size() && name[i + 3] == '_' && std::isxdigit(static_cast<unsigned char>(name[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(name[i + 2]))) {
            out += static_cast<char>(std::stoi(name.substr(i + 1, 2), nullptr, 16));
            i += 3;
        } else {
            out += name[i];
        }
    }
    return out;
}

std::string odfName(const std::string& display) {
    std::string out;
    static const char* hexd = "0123456789abcdef";
    for (unsigned char c : display) {
        if (std::isalnum(c) || c == '-' || c == '.' || c >= 0x80) out += static_cast<char>(c);
        else {
            out += '_';
            out += hexd[c >> 4];
            out += hexd[c & 15];
            out += '_';
        }
    }
    return out.empty() ? std::string("Style") : out;
}

// ---- reading ------------------------------------------------------------------------

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

struct OdtStyle {
    std::string parent, display;
    bool automatic = false;
    std::string family;
    // text properties (-1 / 0 / "" = not given)
    int bold = -1, italic = -1, underline = -1, strike = -1;
    int vertical = 0;
    std::uint32_t color = 0, highlight = 0;
    double size = 0;
    std::string font;
    // paragraph properties
    ParaFormat para;
    bool pageBreakBefore = false;
    int outline = 0;
    // table cell properties
    std::uint32_t fill = 0;
    char valign = 0;
    bool noBorder = false;
    double relWidth = 0;
};

struct OdtReader {
    std::map<std::string, std::string> files;
    std::map<std::string, OdtStyle> styles;
    struct ListLevel {
        bool ordered = false;
        std::string format;
        int start = 1;
    };
    std::map<std::string, ListLevel> listStyles;  // list style -> its first level
    std::map<std::string, std::string> fontFamily;  // font face name -> family name
    struct Change {
        bool insert = true;
        std::string author, date;
        const xml::Node* deleted = nullptr;
        int id = 0;
    };
    std::map<std::string, Change> changes;
    xml::Node contentRoot;
    TextDocument doc;
    bool ours = false;  // written by this suite
    int curComment = 0, pendingComment = 0, curChange = 0;
    std::map<std::string, int> commentByName;

    void loadFonts(const xml::Node& root) {
        if (const auto* ff = root.child("font-face-decls")) {
            for (const auto* f : ff->all("font-face")) {
                std::string fam = f->attr("svg:font-family");
                if (!fam.empty() && fam.front() == '\'') fam = fam.substr(1, fam.size() - 2);
                fontFamily[f->attr("style:name")] = fam;
            }
        }
    }

    void loadStyles(const xml::Node& container, bool automatic) {
        for (const auto* s : container.all("style")) {
            OdtStyle t;
            t.automatic = automatic;
            t.parent = s->attr("style:parent-style-name");
            t.family = s->attr("style:family");
            t.display = s->hasAttr("style:display-name") ? s->attr("style:display-name") : displayOf(s->attr("style:name"));
            if (s->hasAttr("style:default-outline-level")) t.outline = std::atoi(s->attr("style:default-outline-level").c_str());
            if (const auto* tp = s->child("text-properties")) {
                if (tp->hasAttr("fo:font-weight")) t.bold = tp->attr("fo:font-weight") == "bold" || std::atoi(tp->attr("fo:font-weight").c_str()) >= 600;
                if (tp->hasAttr("fo:font-style")) t.italic = tp->attr("fo:font-style") == "italic" || tp->attr("fo:font-style") == "oblique";
                if (tp->hasAttr("style:text-underline-style")) t.underline = tp->attr("style:text-underline-style") != "none";
                if (tp->hasAttr("style:text-line-through-style")) t.strike = tp->attr("style:text-line-through-style") != "none";
                t.color = odfColor(tp->attr("fo:color"));
                t.highlight = odfColor(tp->attr("fo:background-color"));
                const std::string sz = tp->attr("fo:font-size");
                if (!sz.empty() && sz.back() != '%') t.size = lengthPt(sz);
                const std::string pos = tp->attr("style:text-position");
                if (startsWith(pos, "super") || (!pos.empty() && std::atof(pos.c_str()) > 0)) t.vertical = 1;
                else if (startsWith(pos, "sub") || (!pos.empty() && std::atof(pos.c_str()) < 0)) t.vertical = -1;
                std::string fn = tp->attr("style:font-name");
                if (fontFamily.count(fn)) fn = fontFamily[fn];
                if (fn.empty()) fn = tp->attr("fo:font-family");
                t.font = familyName(fn);
            }
            if (const auto* pp = s->child("paragraph-properties")) {
                t.pageBreakBefore = pp->attr("fo:break-before") == "page";
                const std::string a = pp->attr("fo:text-align");
                t.para.align = a == "center" ? 'c' : (a == "end" || a == "right") ? 'r' : a == "justify" ? 'j' : (a == "start" || a == "left") ? 'l' : 0;
                t.para.indent = lengthPt(pp->attr("fo:margin-left"));
                t.para.indentRight = lengthPt(pp->attr("fo:margin-right"));
                t.para.firstLine = lengthPt(pp->attr("fo:text-indent"));
                if (pp->hasAttr("fo:margin-top")) t.para.before = lengthPt(pp->attr("fo:margin-top"));
                if (pp->hasAttr("fo:margin-bottom")) t.para.after = lengthPt(pp->attr("fo:margin-bottom"));
                const std::string lh = pp->attr("fo:line-height");
                if (!lh.empty() && lh.back() == '%') t.para.lineSpacing = std::atof(lh.c_str()) / 100.0;
                t.para.keepWithNext = pp->attr("fo:keep-with-next") == "always";
            }
            if (const auto* cp = s->child("table-cell-properties")) {
                t.fill = odfColor(cp->attr("fo:background-color"));
                const std::string va = cp->attr("style:vertical-align");
                t.valign = va == "middle" ? 'm' : va == "bottom" ? 'b' : 0;
                t.noBorder = cp->attr("fo:border") == "none";
            }
            if (const auto* cp = s->child("table-column-properties")) {
                const std::string rw = cp->attr("style:rel-column-width");
                if (!rw.empty()) t.relWidth = std::atof(rw.c_str());
            }
            styles[s->attr("style:name")] = t;
        }
        for (const auto* ls : container.all("list-style")) {
            ListLevel lv;
            for (const auto& c : ls->children) {
                if (c.is("list-level-style-number") || c.is("list-level-style-bullet") || c.is("list-level-style-image")) {
                    lv.ordered = c.is("list-level-style-number");
                    if (lv.ordered) {
                        const std::string f = c.attr("style:num-format");
                        lv.format = f == "a" ? "lowerLetter" : f == "A" ? "upperLetter" : f == "i" ? "lowerRoman" : f == "I" ? "upperRoman" : "";
                        if (f.empty()) lv.ordered = false;
                        if (c.hasAttr("text:start-value")) lv.start = std::max(0, std::atoi(c.attr("text:start-value").c_str()));
                    }
                    break;
                }
            }
            listStyles[ls->attr("style:name")] = lv;
        }
    }

    // The common (named) style a paragraph style stands on.
    std::string commonOf(const std::string& name) {
        std::string n = name;
        for (int hops = 0; hops < 8 && !n.empty(); ++hops) {
            auto it = styles.find(n);
            if (it == styles.end() || !it->second.automatic) return n;
            n = it->second.parent;
        }
        return n;
    }

    // The name a style is shown with (its display name, else its decoded name).
    std::string idOf(const std::string& name) {
        auto it = styles.find(name);
        return it != styles.end() && !it->second.display.empty() ? it->second.display : displayOf(name);
    }

    // Built-in style id for an ODF common style name ("" = a style of your own).
    std::string builtinFor(const std::string& name) {
        const std::string d = lower(idOf(name));
        if (d.empty() || d == "standard" || d == "default" || d == "text body" || d == "body text" || d == "default paragraph style") return "Normal";
        if (d == "title") return "Title";
        if (d == "subtitle") return "Subtitle";
        if (d == "quotations" || d == "quote") return "Quote";
        if (d == "preformatted text" || d == "source text" || d == "code") return "Code";
        if (d == "caption" || d == "illustration" || d == "figure" || d == "table") return "Caption";
        if (d == "list paragraph" || d == "list contents" || d == "list") return "ListParagraph";
        if (startsWith(d, "heading")) {
            const auto digit = d.find_first_of("0123456789");
            return "Heading" + std::to_string(digit == std::string::npos ? 1 : std::clamp(std::atoi(d.c_str() + digit), 1, 6));
        }
        return "";
    }

    bool helperStyle(const std::string& name) {
        const std::string d = lower(idOf(name));
        return d == "table contents" || d == "table heading" || d == "footnote" || d == "endnote" || d == "header" || d == "footer" || startsWith(d, "contents") ||
               d == "index" || d == "frame contents" || d == "header and footer" || d == "horizontal line";
    }

    StyleDef defOf(const OdtStyle& s) {
        StyleDef d;
        d.name = s.display;
        d.font = s.font;
        d.size = s.size;
        d.color = s.color;
        d.bold = s.bold;
        d.italic = s.italic;
        d.underline = s.underline;
        d.para = s.para;
        d.para.pageBreakBefore = false;
        return d;
    }

    // Built-in styles that look different in this file become entries of doc.styles.
    void builtinChanges() {
        std::vector<std::pair<std::string, const OdtStyle*>> found;
        for (const auto& [name, s] : styles) {
            if (s.automatic || (s.family != "paragraph" && !s.family.empty())) continue;
            const std::string b = builtinFor(name);
            if (b.empty()) continue;
            const std::string d = lower(idOf(name));
            if (b == "Normal" && d != "standard") continue;  // Text body and friends
            found.emplace_back(b, &s);
        }
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first == "Normal" && b.first != "Normal"; });
        for (const auto& [id, s] : found) {
            if (doc.styles.count(id)) continue;
            const ResolvedStyle base = resolveStyle(doc, id);
            StyleDef keep;
            bool any = false;
            if (!s->font.empty() && s->font != (base.font == pdf::Family::Serif ? "serif" : base.font == pdf::Family::Mono ? "mono" : "sans")) keep.font = s->font, any = true;
            if (s->size > 0 && std::fabs(s->size - base.size) > 0.05) keep.size = s->size, any = true;
            if (s->color && rgbOf(s->color) != rgbOf(base.color)) keep.color = s->color, any = true;
            if (s->bold >= 0 && (s->bold != 0) != base.bold) keep.bold = s->bold, any = true;
            if (s->italic >= 0 && (s->italic != 0) != base.italic) keep.italic = s->italic, any = true;
            if (s->underline >= 0 && (s->underline != 0) != base.underline) keep.underline = s->underline, any = true;
            if (s->para.align && s->para.align != base.align) keep.para.align = s->para.align, any = true;
            if (s->para.indent && std::fabs(s->para.indent - base.indent) > 0.1) keep.para.indent = s->para.indent, any = true;
            if (s->para.before >= 0 && std::fabs(s->para.before - base.before) > 0.1) keep.para.before = s->para.before, any = true;
            if (s->para.after >= 0 && std::fabs(s->para.after - base.after) > 0.1) keep.para.after = s->para.after, any = true;
            if (s->para.lineSpacing > 0 && std::fabs(s->para.lineSpacing - base.lineSpacing) > 0.02) keep.para.lineSpacing = s->para.lineSpacing, any = true;
            if (any) doc.styles[id] = keep;
        }
    }

    void useStyle(const std::string& name) {
        std::string cur = name;
        for (int hops = 0; hops < 8 && !cur.empty(); ++hops) {
            const std::string id = idOf(cur);
            if (doc.styles.count(id) || !builtinFor(cur).empty()) break;
            auto it = styles.find(cur);
            if (it == styles.end()) break;
            StyleDef d = defOf(it->second);
            const std::string parent = it->second.parent;
            const std::string pb = parent.empty() ? "" : builtinFor(parent);
            d.basedOn = parent.empty() || pb == "Normal" ? "" : !pb.empty() ? pb : idOf(parent);
            doc.styles[id] = d;
            if (!pb.empty()) break;
            cur = parent;
        }
    }

    void applyText(Run& r, const OdtStyle& s) {
        if (s.bold >= 0) r.bold = s.bold;
        if (s.italic >= 0) r.italic = s.italic;
        if (s.underline >= 0) r.underline = s.underline && r.link.empty();
        if (s.strike >= 0) r.strike = s.strike;
        if (s.vertical) r.vertical = s.vertical;
        if (s.color) r.color = s.color;
        if (s.highlight) r.highlight = s.highlight;
        if (s.size > 0 && std::fabs(s.size - doc.page.fontSize) > 0.01) r.size = s.size;
        if (s.font == "mono") r.code = true;
        else if (!s.font.empty() && s.font != (doc.page.font == pdf::Family::Serif ? "serif" : "sans")) r.font = s.font;
    }

    int changeId(const std::string& key) {
        auto it = changes.find(key);
        if (it == changes.end()) return 0;
        if (!it->second.id) {
            TrackedChange ch;
            ch.id = doc.nextChangeId();
            ch.insert = it->second.insert;
            ch.author = it->second.author;
            ch.date = it->second.date;
            doc.changes.push_back(ch);
            it->second.id = ch.id;
        }
        return it->second.id;
    }

    std::string notesMarkdown(const xml::Node& body) {
        Runs rs;
        const int c = curComment, p = pendingComment, ch = curChange;
        curComment = pendingComment = curChange = 0;
        for (const auto* para : body.all("p")) {
            if (!rs.empty()) {
                Run sp;
                sp.text = " ";
                rs.push_back(sp);
            }
            spans(*para, rs, Run{});
        }
        curComment = c, pendingComment = p, curChange = ch;
        for (auto& r : rs) r.footnote.clear(), r.comment = 0, r.change = 0;
        mergeRuns(rs);
        std::string md = trimCopy(inlineMarkdown(rs));
        return md.empty() ? std::string("(note)") : md;
    }

    void push(Runs& out, Run r) {
        if (pendingComment) {
            r.comment = pendingComment;
            pendingComment = 0;
        }
        out.push_back(std::move(r));
    }

    void annotation(const xml::Node& a, Runs& out) {
        Comment cm;
        cm.id = static_cast<int>(doc.comments.size()) + 1;
        if (const auto* c = a.child("creator")) cm.author = c->allText();
        if (const auto* d = a.child("date")) cm.date = d->allText();
        std::string text;
        for (const auto* p : a.all("p")) {
            Runs rs;
            const int c = curComment, pc = pendingComment;
            curComment = pendingComment = 0;
            spans(*p, rs, Run{});
            curComment = c, pendingComment = pc;
            for (auto& r : rs) r.comment = 0, r.change = 0, r.footnote.clear();
            mergeRuns(rs);
            const std::string line = trimCopy(inlineMarkdown(rs));
            if (line.empty()) continue;
            if (!text.empty()) text += "\n";
            text += line;
        }
        cm.text = text;
        doc.comments.push_back(cm);
        const std::string name = a.attr("office:name");
        if (!name.empty()) {
            commentByName[name] = cm.id;
            curComment = cm.id;
        } else if (!out.empty()) {
            out.back().comment = cm.id;  // a comment on a point: it marks the text before it
        } else {
            pendingComment = cm.id;
        }
    }

    void spans(const xml::Node& n, Runs& out, Run base) {
        for (const auto& c : n.children) {
            Run r = base;
            if (!c.isText() && !c.is("annotation")) r.comment = curComment, r.change = curChange;
            if (c.isText()) {
                r.comment = curComment;
                r.change = curChange;
                r.text = c.text;
                push(out, r);
            } else if (c.is("s")) {
                r.text = std::string(static_cast<std::size_t>(std::max(1, std::atoi(c.attr("text:c").c_str()))), ' ');
                push(out, r);
            } else if (c.is("tab")) {
                r.text = "\t";
                push(out, r);
            } else if (c.is("line-break")) {
                r.text = "\n";
                push(out, r);
            } else if (c.is("span")) {
                auto it = styles.find(c.attr("text:style-name"));
                if (it != styles.end()) applyText(r, it->second);
                spans(c, out, r);
            } else if (c.is("a")) {
                r.link = c.attr("xlink:href");
                r.underline = false;
                spans(c, out, r);
            } else if (c.is("note")) {
                const auto* body = c.child("note-body");
                Run note = r;
                note.text.clear();
                note.footnote = body ? notesMarkdown(*body) : "(note)";
                push(out, note);
            } else if (c.is("annotation")) {
                annotation(c, out);
            } else if (c.is("annotation-end")) {
                curComment = 0;
            } else if (c.is("change-start")) {
                curChange = changeId(c.attr("text:change-id"));
            } else if (c.is("change-end")) {
                curChange = 0;
            } else if (c.is("change")) {
                auto it = changes.find(c.attr("text:change-id"));
                if (it != changes.end() && !it->second.insert && it->second.deleted) {
                    const int id = changeId(c.attr("text:change-id"));
                    const int saved = curChange;
                    curChange = id;
                    bool first = true;
                    for (const auto& p : it->second.deleted->children) {
                        if (!p.is("p") && !p.is("h")) continue;
                        if (!first) {
                            Run sp = base;
                            sp.text = " ";
                            sp.change = id;
                            push(out, sp);
                        }
                        Run db = base;
                        db.change = id;
                        spans(p, out, db);
                        first = false;
                    }
                    curChange = saved;
                }
            } else if (c.is("page-number") || c.is("page-count")) {
                r.text = c.is("page-number") ? "{page}" : "{pages}";
                push(out, r);
            } else if (c.is("bookmark") || c.is("bookmark-start") || c.is("bookmark-end") || c.is("frame") || c.is("soft-page-break") ||
                       c.is("reference-mark") || c.is("tracked-changes")) {
                continue;
            } else {
                spans(c, out, base);
            }
        }
    }

    void frames(const xml::Node& n, std::vector<Block>& out, char align) {
        std::vector<const xml::Node*> fr;
        n.descendants("frame", fr);
        for (const auto* f : fr) {
            const auto* img = f->child("image");
            if (!img) continue;
            std::string href = img->attr("xlink:href");
            std::string bytes = entry(files, href);
            if (bytes.empty()) continue;
            Block b;
            try {
                setImage(b, bytes);
            } catch (const std::exception&) {
                continue;
            }
            b.width = lengthPt(f->attr("svg:width"));
            if (const auto* d = f->child("desc")) b.alt = d->allText();
            else b.alt = f->attr("draw:name");
            b.fmt.align = align;
            out.push_back(std::move(b));
        }
    }

    Block textBlock(const xml::Node& p, bool heading) {
        Block b;
        const std::string style = p.attr("text:style-name");
        Run base;
        auto it = styles.find(style);
        if (it != styles.end() && it->second.automatic) {
            applyText(base, it->second);
            b.fmt = it->second.para;
            b.fmt.pageBreakBefore = false;
        }
        spans(p, b.runs, base);
        mergeRuns(b.runs);
        const std::string common = commonOf(style);
        const std::string builtin = builtinFor(common);
        if (heading || startsWith(builtin, "Heading")) {
            b.kind = Block::Heading;
            b.level = heading && p.hasAttr("text:outline-level") ? std::clamp(std::atoi(p.attr("text:outline-level").c_str()), 1, 6)
                                                                  : std::clamp(builtin.size() == 8 ? builtin[7] - '0' : 1, 1, 6);
            for (auto& r : b.runs) r.bold = false;
            b.fmt.keepWithNext = false;
        } else if (builtin == "Quote") {
            b.kind = Block::Quote;
        } else if (builtin == "Code") {
            b.kind = Block::Code;
        } else if (builtin == "Title" || builtin == "Subtitle" || builtin == "Caption") {
            b.style = builtin;
        } else if (builtin.empty() && !helperStyle(common) && styles.count(common)) {
            useStyle(common);
            if (doc.styles.count(idOf(common))) b.style = idOf(common);
        }
        return b;
    }

    void list(const xml::Node& l, int level, Block& block) {
        for (const auto& item : l.children) {
            if (!item.is("list-item") && !item.is("list-header")) continue;
            for (const auto& c : item.children) {
                if (c.is("p") || c.is("h")) {
                    ListItem it;
                    it.level = std::min(level, 8);
                    spans(c, it.runs, Run{});
                    mergeRuns(it.runs);
                    bool any = false;
                    for (const auto& r : it.runs) any = any || !r.footnote.empty() || !trimCopy(r.text).empty();
                    if (any) block.items.push_back(it);
                } else if (c.is("list")) {
                    list(c, level + 1, block);
                }
            }
        }
    }

    void table(const xml::Node& c, std::vector<Block>& out) {
        Block b;
        b.kind = Block::Table;
        std::vector<const xml::Node*> rows;
        bool headerRows = false;
        std::vector<double> rel;
        bool allRel = true;
        std::function<void(const xml::Node&, bool)> collect = [&](const xml::Node& x, bool head) {
            for (const auto& ch : x.children) {
                if (ch.is("table-row")) {
                    rows.push_back(&ch);
                    headerRows = headerRows || head;
                } else if (ch.is("table-header-rows")) {
                    collect(ch, true);
                } else if (ch.is("table-rows") || ch.is("table-row-group")) {
                    collect(ch, head);
                } else if (ch.is("table-column")) {
                    auto it = styles.find(ch.attr("table:style-name"));
                    const double w = it != styles.end() ? it->second.relWidth : 0;
                    const int rep = std::clamp(std::atoi(ch.attr("table:number-columns-repeated").c_str()), 1, 64);
                    for (int k = 0; k < rep; ++k) rel.push_back(w);
                    if (w <= 0) allRel = false;
                } else if (ch.is("table-columns") || ch.is("table-header-columns")) {
                    collect(ch, head);
                }
            }
        };
        collect(c, false);
        bool anyBorder = false, sawCell = false, banded = false;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            std::vector<Runs> cells;
            std::vector<CellFormat> formats;
            for (const auto& cell : rows[r]->children) {
                const bool covered = cell.is("covered-table-cell");
                if (!cell.is("table-cell") && !covered) continue;
                Runs rs;
                CellFormat f;
                bool first = true;
                for (const auto& p : cell.children) {
                    if (!p.is("p") && !p.is("h")) continue;
                    if (first) {
                        auto ps = styles.find(p.attr("text:style-name"));
                        if (ps != styles.end() && ps->second.para.align && ps->second.para.align != 'l') f.align = ps->second.para.align;
                    }
                    if (!first) {
                        Run nl;
                        nl.text = "\n";
                        rs.push_back(nl);
                    }
                    spans(p, rs, Run{});
                    first = false;
                }
                mergeRuns(rs);
                auto cs = styles.find(cell.attr("table:style-name"));
                if (!covered) {
                    sawCell = true;
                    if (cs == styles.end() || !cs->second.noBorder) anyBorder = true;
                }
                if (cs != styles.end()) {
                    f.fill = cs->second.fill;
                    f.valign = cs->second.valign;
                }
                if (f.fill && rgbOf(f.fill) == 0xF6F8FB) f.fill = 0, banded = true;
                if (f.fill && r == 0 && ours && rgbOf(f.fill) == 0xEEEEEE) f.fill = 0;
                const int col = static_cast<int>(cells.size());
                const int cspan = std::clamp(std::atoi(cell.attr("table:number-columns-spanned").c_str()), 1, 64);
                const int rspan = std::clamp(std::atoi(cell.attr("table:number-rows-spanned").c_str()), 1, 10000);
                if (!covered && (cspan > 1 || rspan > 1)) b.merges.push_back({static_cast<int>(r), col, rspan, cspan});
                const int rep = covered ? 1 : std::clamp(std::atoi(cell.attr("table:number-columns-repeated").c_str()), 1, 64);
                for (int k = 0; k < rep; ++k) {
                    cells.push_back(covered ? Runs{} : rs);
                    formats.push_back(f);
                }
            }
            b.rows.push_back(cells);
            b.cellFormats.push_back(formats);
        }
        std::size_t cols = 0;
        for (const auto& r : b.rows) cols = std::max(cols, r.size());
        for (auto& r : b.rows) r.resize(cols);
        bool anyFormat = false;
        for (auto& r : b.cellFormats) {
            r.resize(cols);
            for (const auto& f : r) anyFormat = anyFormat || f.fill || f.align || f.valign;
        }
        if (!anyFormat) b.cellFormats.clear();
        b.header = ours ? headerRows : true;
        b.borders = !sawCell || anyBorder;
        b.banded = banded;
        if (allRel && rel.size() == cols && cols > 0) b.colWidths = rel;
        unboldHeader(b);
        if (!b.rows.empty()) out.push_back(std::move(b));
    }

    void toc(const xml::Node& c, std::vector<Block>& out) {
        Block t;
        t.kind = Block::Toc;
        t.level = 3;
        if (const auto* src = c.child("table-of-content-source")) {
            if (src->hasAttr("text:outline-level")) t.level = std::clamp(std::atoi(src->attr("text:outline-level").c_str()), 1, 9);
        }
        if (const auto* body = c.child("index-body")) {
            if (const auto* title = body->child("index-title")) {
                for (const auto* p : title->all("p")) spans(*p, t.runs, Run{});
                mergeRuns(t.runs);
            }
        }
        if (trimCopy(plainText(t.runs)) == "Contents" || trimCopy(plainText(t.runs)).empty()) t.runs.clear();
        out.push_back(t);
    }

    void body(const xml::Node& n, std::vector<Block>& out) {
        for (const auto& c : n.children) {
            if (c.is("h") || c.is("p")) {
                auto st = styles.find(c.attr("text:style-name"));
                if (st != styles.end() && st->second.pageBreakBefore && !out.empty()) {
                    Block pb;
                    pb.kind = Block::PageBreak;
                    out.push_back(pb);
                }
                Block b = textBlock(c, c.is("h"));
                bool any = false;
                for (const auto& r : b.runs) any = any || !r.footnote.empty() || !trimCopy(r.text).empty();
                if (any) {
                    if (b.kind == Block::Code) {
                        Run r;
                        r.text = plainText(b.runs);
                        r.code = true;
                        b.runs = {r};
                        b.fmt = ParaFormat{};
                        if (!out.empty() && out.back().kind == Block::Code) {
                            out.back().runs[0].text += "\n" + r.text;
                        } else {
                            out.push_back(std::move(b));
                        }
                    } else {
                        out.push_back(std::move(b));
                    }
                }
                frames(c, out, any ? 0 : b.fmt.align);
            } else if (c.is("list")) {
                Block b;
                b.kind = Block::List;
                std::string ls = c.attr("text:style-name");
                auto it = listStyles.find(ls);
                if (it != listStyles.end()) {
                    b.ordered = it->second.ordered;
                    b.numbering = it->second.format;
                    b.start = it->second.start;
                }
                list(c, 0, b);
                if (!b.items.empty()) out.push_back(std::move(b));
            } else if (c.is("table")) {
                table(c, out);
            } else if (c.is("table-of-content")) {
                toc(c, out);
            } else if (c.is("section") || c.is("index-body")) {
                body(c, out);
            }
        }
    }

    // A header or footer: one line, tab stops (centre and right) as " | ".
    std::string band(const xml::Node& n) {
        std::string out;
        for (const auto& p : n.children) {
            if (!p.is("p") && !p.is("h")) continue;
            Runs rs;
            spans(p, rs, Run{});
            std::vector<Runs> parts(1);
            for (const auto& r : rs) {
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
            std::vector<std::string> md;
            for (auto& part : parts) {
                mergeRuns(part);
                std::string s = trimCopy(inlineMarkdown(part)), esc;
                for (char ch : s) {
                    if (ch == '|') esc += '\\';
                    esc += ch;
                }
                md.push_back(esc);
            }
            std::string line;
            if (md.size() == 1) line = md[0];
            else if (md.size() == 2) line = md[0].empty() ? md[1] : md[0] + " | " + md[1];
            else if (md[0].empty() && md[2].empty()) line = md[1];
            else if (md[1].empty()) line = md[0] + " | " + md[2];
            else line = md[0] + " | " + md[1] + " | " + md[2];
            if (trimCopy(line).empty()) continue;
            if (!out.empty()) out += " ";
            out += line;
        }
        return trimCopy(out);
    }

    void master(const xml::Node& stylesRoot) {
        const xml::Node* ms = stylesRoot.child("master-styles");
        const xml::Node* mp = ms ? ms->child("master-page") : nullptr;
        if (!mp) return;
        std::string layout = mp->attr("style:page-layout-name");
        if (const auto* as = stylesRoot.child("automatic-styles")) {
            for (const auto* pl : as->all("page-layout")) {
                if (pl->attr("style:name") != layout) continue;
                if (const auto* pp = pl->child("page-layout-properties")) {
                    const double w = lengthPt(pp->attr("fo:page-width")), h = lengthPt(pp->attr("fo:page-height"));
                    if (w > 100 && h > 100) doc.page.width = w, doc.page.height = h;
                    auto margin = [&](const char* key, double& v) {
                        if (pp->hasAttr(key)) v = std::clamp(lengthPt(pp->attr(key)), 0.0, 400.0);
                    };
                    margin("fo:margin-top", doc.page.top);
                    margin("fo:margin-bottom", doc.page.bottom);
                    margin("fo:margin-left", doc.page.left);
                    margin("fo:margin-right", doc.page.right);
                }
            }
        }
        HeaderFooter& hf = doc.headerFooter;
        bool footer = false;
        if (const auto* h = mp->child("header"); h && h->attr("style:display") != "false") hf.header = band(*h);
        if (const auto* f = mp->child("footer"); f && f->attr("style:display") != "false") hf.footer = band(*f), footer = true;
        const auto* hfst = mp->child("header-first");
        const auto* ffst = mp->child("footer-first");
        if (hfst || ffst) {
            hf.differentFirst = true;
            if (hfst) hf.firstHeader = band(*hfst);
            if (ffst) hf.firstFooter = band(*ffst);
        }
        if (ours) {  // the writer moves the margins in by the header and footer areas
            if (!hf.header.empty() || (hf.differentFirst && !hf.firstHeader.empty())) doc.page.top += 24;
            if (footer || (hf.differentFirst && !hf.firstFooter.empty())) doc.page.bottom += 24;
        }
        if (hf.footer == "{page} / {pages}") hf.footer.clear();
        else if (!footer) doc.page.pageNumbers = false;
    }
};

}  // namespace

TextDocument readOdt(const std::string& bytes) {
    OdtReader r;
    r.files = unzip(bytes);
    std::string content = entry(r.files, "content.xml");
    if (content.empty()) throw std::runtime_error("not an OpenDocument file (no content.xml)");
    if (std::string meta = entry(r.files, "meta.xml"); !meta.empty()) {
        try {
            xml::Node m = xml::parse(meta);
            if (const auto* t = m.find("title")) r.doc.title = t->allText();
            if (const auto* a = m.find("initial-creator")) r.doc.author = a->allText();
            else if (const auto* c = m.find("creator")) r.doc.author = c->allText();
            if (const auto* g = m.find("generator")) r.ours = startsWith(g->allText(), "wetwaresuite");
        } catch (const std::exception&) {
        }
    }
    xml::Node stylesRoot;
    bool haveStyles = false;
    if (std::string st = entry(r.files, "styles.xml"); !st.empty()) {
        try {
            stylesRoot = xml::parse(st);
            haveStyles = true;
            r.loadFonts(stylesRoot);
            if (const auto* os = stylesRoot.child("styles")) {
                for (const auto* ds : os->all("default-style")) {
                    if (ds->attr("style:family") != "paragraph") continue;
                    if (const auto* tp = ds->child("text-properties")) {
                        const double sz = lengthPt(tp->attr("fo:font-size"));
                        if (sz >= 4 && sz <= 72) r.doc.page.fontSize = sz;
                    }
                }
                r.loadStyles(*os, false);
            }
        } catch (const std::exception&) {
            haveStyles = false;
        }
    }
    r.contentRoot = xml::parse(content);
    const xml::Node& root = r.contentRoot;
    r.loadFonts(root);
    if (auto it = r.styles.find("Standard"); it != r.styles.end()) {
        if (it->second.size >= 4 && it->second.size <= 72) r.doc.page.fontSize = it->second.size;
        if (it->second.font == "serif") r.doc.page.font = pdf::Family::Serif;
    }
    r.builtinChanges();
    if (const auto* as = root.child("automatic-styles")) r.loadStyles(*as, true);
    const xml::Node* bodyNode = root.child("body");
    const xml::Node* text = bodyNode ? bodyNode->child("text") : nullptr;
    if (!text) throw std::runtime_error("not a text document (no office:text)");
    if (const auto* tc = text->child("tracked-changes")) {
        r.doc.trackChanges = tc->attr("text:track-changes") == "true";
        for (const auto* region : tc->all("changed-region")) {
            OdtReader::Change ch;
            const xml::Node* kind = region->child("insertion");
            if (!kind) {
                kind = region->child("deletion");
                ch.insert = false;
            }
            if (!kind) continue;
            ch.deleted = ch.insert ? nullptr : kind;
            if (const auto* info = kind->child("change-info")) {
                if (const auto* c = info->child("creator")) ch.author = c->allText();
                if (const auto* d = info->child("date")) ch.date = d->allText();
            }
            r.changes[region->attr("text:id").empty() ? region->attr("xml:id") : region->attr("text:id")] = ch;
        }
    }
    r.body(*text, r.doc.blocks);
    if (haveStyles) r.master(stylesRoot);
    return r.doc;
}

// ---- writing ------------------------------------------------------------------------

namespace {

const char* kOdtNs =
    " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
    " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
    " xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
    " xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\""
    " xmlns:loext=\"urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0\" office:version=\"1.3\"";

std::string pt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2fpt", v);
    return buf;
}

std::string hexColor(std::uint32_t c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%06x", static_cast<unsigned>(rgbOf(c)));
    return buf;
}

std::string esc(const std::string& s) { return xml::escape(s); }

const char* fontFace(pdf::Family f) { return f == pdf::Family::Serif ? "Liberation Serif" : f == pdf::Family::Mono ? "Liberation Mono" : "Liberation Sans"; }
const char* fontFace(const std::string& f) { return f == "serif" ? "Liberation Serif" : f == "mono" ? "Liberation Mono" : "Liberation Sans"; }

std::string alignAttr(char a) { return a == 'c' ? "center" : a == 'r' ? "end" : a == 'j' ? "justify" : "start"; }

// Automatic styles, made once per distinct look.
struct Registry {
    std::string prefix, xml;
    std::map<std::string, std::string> names;
    std::string get(const std::string& key, const std::function<std::string(const std::string&)>& make) {
        auto it = names.find(key);
        if (it != names.end()) return it->second;
        const std::string name = prefix + std::to_string(names.size() + 1);
        names[key] = name;
        xml += make(name);
        return name;
    }
};

std::string textProps(const Run& r, bool forceBold) {
    std::string p;
    if (r.bold || forceBold) p += " fo:font-weight=\"bold\" style:font-weight-asian=\"bold\" style:font-weight-complex=\"bold\"";
    if (r.italic) p += " fo:font-style=\"italic\"";
    if (r.underline && r.link.empty()) p += " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"";
    if (r.strike) p += " style:text-line-through-style=\"solid\"";
    if (hasColor(r.color)) p += " fo:color=\"" + hexColor(r.color) + "\"";
    if (hasColor(r.highlight)) p += " fo:background-color=\"" + hexColor(r.highlight) + "\"";
    if (r.size > 0) p += " fo:font-size=\"" + pt(r.size) + "\"";
    if (r.vertical) p += std::string(" style:text-position=\"") + (r.vertical > 0 ? "super" : "sub") + " 58%\"";
    if (r.code) p += " style:font-name=\"Liberation Mono\"";
    else if (!r.font.empty()) p += std::string(" style:font-name=\"") + fontFace(r.font) + "\"";
    return p;
}

std::string paraProps(const ParaFormat& f, bool pageBreak) {
    std::string p;
    if (f.align) p += " fo:text-align=\"" + alignAttr(f.align) + "\"";
    if (f.indent) p += " fo:margin-left=\"" + pt(f.indent) + "\"";
    if (f.indentRight) p += " fo:margin-right=\"" + pt(f.indentRight) + "\"";
    if (f.firstLine) p += " fo:text-indent=\"" + pt(f.firstLine) + "\"";
    if (f.before >= 0) p += " fo:margin-top=\"" + pt(f.before) + "\"";
    if (f.after >= 0) p += " fo:margin-bottom=\"" + pt(f.after) + "\"";
    if (f.lineSpacing > 0) p += " fo:line-height=\"" + std::to_string(static_cast<int>(std::lround(f.lineSpacing * 100))) + "%\"";
    if (f.keepWithNext) p += " fo:keep-with-next=\"always\"";
    if (pageBreak || f.pageBreakBefore) p += " fo:break-before=\"page\"";
    return p;
}

std::string styleProps(const ResolvedStyle& r, bool withFont) {
    std::string p = "<style:paragraph-properties fo:margin-top=\"" + pt(r.before) + "\" fo:margin-bottom=\"" + pt(r.after) + "\"";
    if (r.indent) p += " fo:margin-left=\"" + pt(r.indent) + "\"";
    if (r.indentRight) p += " fo:margin-right=\"" + pt(r.indentRight) + "\"";
    if (r.firstLine) p += " fo:text-indent=\"" + pt(r.firstLine) + "\"";
    p += " fo:text-align=\"" + alignAttr(r.align) + "\"";
    p += " fo:line-height=\"" + std::to_string(static_cast<int>(std::lround(r.lineSpacing * 100))) + "%\"";
    if (r.keepWithNext) p += " fo:keep-with-next=\"always\"";
    p += "/><style:text-properties";
    if (withFont) p += std::string(" style:font-name=\"") + fontFace(r.font) + "\"";
    p += " fo:font-size=\"" + pt(r.size) + "\"";
    p += std::string(" fo:font-weight=\"") + (r.bold ? "bold" : "normal") + "\"";
    p += std::string(" fo:font-style=\"") + (r.italic ? "italic" : "normal") + "\"";
    if (hasColor(r.color)) p += " fo:color=\"" + hexColor(r.color) + "\"";
    p += std::string(" style:text-underline-style=\"") + (r.underline ? "solid" : "none") + "\"";
    return p + "/>";
}

std::string styleNameOf(const std::string& id) {
    if (id.empty() || id == "Normal") return "Standard";
    if (id == "Title" || id == "Subtitle" || id == "Caption") return id;
    if (startsWith(id, "Heading") && id.size() == 8) return std::string("Heading_20_") + id[7];
    if (id == "Quote") return "Quotations";
    if (id == "Code") return "Preformatted_20_Text";
    if (id == "ListParagraph") return "List_20_Paragraph";
    return "U_" + odfName(id);  // a style of your own
}

struct OdtWriter {
    const TextDocument& doc;
    Registry text{"T", {}, {}}, para{"P", {}, {}}, cells{"C", {}, {}}, columns{"Co", {}, {}};
    Registry masterText{"MT", {}, {}};
    std::string listStyles;
    int lists = 0, tables = 0, notes = 0, pictures = 0;
    bool bulletStyle = false;
    std::vector<raster::ZipEntry> media;
    std::vector<int> tocPages;

    explicit OdtWriter(const TextDocument& d) : doc(d) {}

    double contentWidth() const { return std::max(36.0, doc.page.width - doc.page.left - doc.page.right); }

    std::string textStyle(Registry& reg, const Run& r, bool forceBold) {
        const std::string props = textProps(r, forceBold);
        if (props.empty()) return "";
        return reg.get(props, [&](const std::string& name) {
            return "<style:style style:name=\"" + name + "\" style:family=\"text\"><style:text-properties" + props + "/></style:style>";
        });
    }

    std::string paraStyle(const std::string& parent, const ParaFormat& f, bool pageBreak) {
        const std::string props = paraProps(f, pageBreak);
        if (props.empty()) return parent;
        return para.get(parent + "|" + props, [&](const std::string& name) {
            return "<style:style style:name=\"" + name + "\" style:family=\"paragraph\" style:parent-style-name=\"" + parent +
                   "\"><style:paragraph-properties" + props + "/></style:style>";
        });
    }

    // Text with ODF's whitespace rules: runs of spaces, tabs and line breaks as elements.
    std::string textXml(const std::string& s, bool& prevSpace) {
        std::string out, cur;
        auto flush = [&] {
            out += esc(cur);
            cur.clear();
        };
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == ' ') {
                if (prevSpace) {
                    std::size_t n = 1;
                    while (i + 1 < s.size() && s[i + 1] == ' ') ++n, ++i;
                    flush();
                    out += n > 1 ? "<text:s text:c=\"" + std::to_string(n) + "\"/>" : "<text:s/>";
                } else {
                    cur += ' ';
                }
                prevSpace = true;
            } else if (c == '\t') {
                flush();
                out += "<text:tab/>";
                prevSpace = false;
            } else if (c == '\n') {
                flush();
                out += "<text:line-break/>";
                prevSpace = true;
            } else {
                cur += c;
                prevSpace = false;
            }
        }
        flush();
        return out;
    }

    std::string runsXml(const Runs& rs, bool forceBold, Registry& reg, bool allowNotes = true) {
        std::string out;
        bool prevSpace = true;
        int openComment = 0;
        auto closeComment = [&] {
            if (!openComment) return;
            out += "<office:annotation-end office:name=\"c" + std::to_string(openComment) + "\"/>";
            openComment = 0;
        };
        for (std::size_t i = 0; i < rs.size();) {
            const Run& r = rs[i];
            if (r.comment != openComment) {
                closeComment();
                if (const Comment* cm = r.comment ? doc.comment(r.comment) : nullptr) {
                    openComment = cm->id;
                    out += "<office:annotation office:name=\"c" + std::to_string(cm->id) + "\">";
                    if (!cm->author.empty()) out += "<dc:creator>" + esc(cm->author) + "</dc:creator>";
                    if (!cm->date.empty()) out += "<dc:date>" + esc(cm->date) + "</dc:date>";
                    for (const auto& line : splitLines(cm->text.empty() ? std::string(" ") : cm->text)) {
                        bool ps = true;
                        out += "<text:p>";
                        for (const auto& lr : parseInline(line)) out += textXml(lr.text, ps);
                        out += "</text:p>";
                    }
                    out += "</office:annotation>";
                }
            }
            std::size_t j = i;
            while (j < rs.size() && rs[j].link == r.link && rs[j].comment == r.comment) ++j;
            if (!r.link.empty()) out += "<text:a xlink:type=\"simple\" xlink:href=\"" + esc(r.link) + "\" text:style-name=\"Internet_20_link\">";
            for (std::size_t k = i; k < j; ++k) {
                const Run& x = rs[k];
                if (!x.footnote.empty()) {
                    if (!allowNotes) continue;
                    ++notes;
                    out += "<text:note text:id=\"ftn" + std::to_string(notes) + "\" text:note-class=\"footnote\"><text:note-citation>" + std::to_string(notes) +
                           "</text:note-citation><text:note-body><text:p text:style-name=\"Footnote\">" + runsXml(parseInline(x.footnote), false, reg, false) +
                           "</text:p></text:note-body></text:note>";
                    prevSpace = false;
                    continue;
                }
                if (x.text.empty()) continue;
                const std::string ts = textStyle(reg, x, forceBold);
                const std::string body = textXml(x.text, prevSpace);
                out += ts.empty() ? body : "<text:span text:style-name=\"" + ts + "\">" + body + "</text:span>";
            }
            if (!r.link.empty()) out += "</text:a>";
            i = j;
        }
        closeComment();
        return out;
    }

    std::string paragraph(const char* tag, const std::string& style, const Runs& rs, const std::string& extra = "") {
        return std::string("<text:") + tag + " text:style-name=\"" + style + "\"" + extra + ">" + runsXml(rs, false, text) + "</text:" + tag + ">";
    }

    std::string listStyle(const Block& b) {
        const std::string fmt = !b.numbering.empty() && b.numbering != "bullet" ? b.numbering : b.ordered ? "decimal" : "bullet";
        if (fmt == "bullet" && bulletStyle) return "LB";
        const std::string name = fmt == "bullet" ? "LB" : "L" + std::to_string(++lists);
        if (fmt == "bullet") bulletStyle = true;
        std::string x = "<text:list-style style:name=\"" + name + "\">";
        static const char* bullets[3] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"};
        for (int l = 1; l <= 10; ++l) {
            const std::string pos = pt(18.0 * l), indent = pt(-14);
            const std::string props = "<style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\"><style:list-level-label-alignment "
                                      "text:label-followed-by=\"listtab\" text:list-tab-stop-position=\"" + pos + "\" fo:text-indent=\"" + indent +
                                      "\" fo:margin-left=\"" + pos + "\"/></style:list-level-properties>";
            if (fmt == "bullet") {
                x += "<text:list-level-style-bullet text:level=\"" + std::to_string(l) + "\" text:bullet-char=\"" + bullets[(l - 1) % 3] + "\">" + props +
                     "</text:list-level-style-bullet>";
            } else {
                const std::string f = l == 1 ? fmt : (l - 1) % 3 == 1 ? "lowerLetter" : (l - 1) % 3 == 2 ? "lowerRoman" : "decimal";
                const char* nf = f == "lowerLetter" ? "a" : f == "upperLetter" ? "A" : f == "lowerRoman" ? "i" : f == "upperRoman" ? "I" : "1";
                x += "<text:list-level-style-number text:level=\"" + std::to_string(l) + "\" style:num-suffix=\"" + (f == "lowerLetter" && l == 1 ? ")" : ".") +
                     "\" style:num-format=\"" + nf + "\"" + (l == 1 && b.start != 1 ? " text:start-value=\"" + std::to_string(b.start) + "\"" : "") + ">" + props +
                     "</text:list-level-style-number>";
            }
        }
        listStyles += x + "</text:list-style>";
        return name;
    }

    // Items from `i` on at `level` and deeper, as nested lists.
    std::string listItems(const Block& b, std::size_t& i, int level, const std::string& style, const std::string& pstyle) {
        std::string x = level == 0 ? "<text:list text:style-name=\"" + style + "\">" : "<text:list>";
        while (i < b.items.size() && b.items[i].level >= level) {
            if (b.items[i].level == level) {
                x += "<text:list-item>" + paragraph("p", pstyle, b.items[i].runs);
                ++i;
                if (i < b.items.size() && b.items[i].level > level) x += listItems(b, i, level + 1, style, pstyle);
                x += "</text:list-item>";
            } else {
                x += "<text:list-item>" + listItems(b, i, level + 1, style, pstyle) + "</text:list-item>";
            }
        }
        return x + "</text:list>";
    }

    std::string image(const Block& b) {
        double pw = b.pixels ? b.pixels->pixels.width() : 100, ph = b.pixels ? b.pixels->pixels.height() : 100;
        if (pw <= 0) pw = 1;
        double w = b.width > 0 ? b.width : pw * 0.75;
        w = std::min(w, contentWidth());
        const double h = w * ph / pw;
        ++pictures;
        const bool jpeg = b.image.size() > 2 && static_cast<unsigned char>(b.image[0]) == 0xFF && static_cast<unsigned char>(b.image[1]) == 0xD8;
        const std::string file = "Pictures/image" + std::to_string(pictures) + (jpeg ? ".jpeg" : ".png");
        media.push_back({file, b.image});
        ParaFormat f = b.fmt;
        if (!f.align) f.align = 'c';
        std::string x = "<text:p text:style-name=\"" + paraStyle(styleNameOf(b.style), f, false) + "\">";
        x += "<draw:frame draw:style-name=\"fr1\" draw:name=\"Image" + std::to_string(pictures) + "\" text:anchor-type=\"as-char\" svg:width=\"" + pt(w) +
             "\" svg:height=\"" + pt(h) + "\" draw:z-index=\"0\"><draw:image xlink:href=\"" + file +
             "\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/>";
        if (!b.alt.empty()) x += "<svg:desc>" + esc(b.alt) + "</svg:desc>";
        return x + "</draw:frame></text:p>";
    }

    std::string table(const Block& b) {
        std::size_t cols = 0;
        for (const auto& r : b.rows) cols = std::max(cols, r.size());
        if (!cols) return "";
        const std::size_t rows = b.rows.size();
        ++tables;
        const double W = contentWidth();
        std::vector<double> rel(cols, 1.0);
        double given = 0;
        for (std::size_t c = 0; c < cols && c < b.colWidths.size(); ++c) given += std::max(0.0, b.colWidths[c]);
        const bool fixed = b.colWidths.size() >= cols && given > 0;
        if (fixed) {
            for (std::size_t c = 0; c < cols; ++c) rel[c] = std::max(0.0, b.colWidths[c]);
        } else {
            given = static_cast<double>(cols);
        }
        std::string tstyle = "Tb" + std::to_string(tables);
        listStyles += "<style:style style:name=\"" + tstyle + "\" style:family=\"table\"><style:table-properties style:width=\"" + pt(W) +
                      "\" table:align=\"margins\" table:border-model=\"collapsing\" fo:margin-bottom=\"6pt\"/></style:style>";
        std::string x = "<table:table table:name=\"Table" + std::to_string(tables) + "\" table:style-name=\"" + tstyle + "\">";
        for (std::size_t c = 0; c < cols; ++c) {
            const double w = W * rel[c] / given;
            const std::string relAttr = fixed ? " style:rel-column-width=\"" + std::to_string(static_cast<long>(std::lround(rel[c] * 1000))) + "*\"" : "";
            const std::string cs = columns.get(pt(w) + relAttr, [&](const std::string& name) {
                return "<style:style style:name=\"" + name + "\" style:family=\"table-column\"><style:table-column-properties style:column-width=\"" + pt(w) + "\"" +
                       relAttr + "/></style:style>";
            });
            x += "<table:table-column table:style-name=\"" + cs + "\"/>";
        }
        std::vector<std::vector<const CellSpan*>> cover(rows, std::vector<const CellSpan*>(cols, nullptr));
        for (const auto& m : b.merges) {
            for (int r = m.row; r < m.row + m.rows && r < static_cast<int>(rows); ++r) {
                for (int c = m.col; c < m.col + m.cols && c < static_cast<int>(cols); ++c) {
                    if (r >= 0 && c >= 0) cover[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = &m;
                }
            }
        }
        for (std::size_t r = 0; r < rows; ++r) {
            const bool head = r == 0 && b.header;
            if (head) x += "<table:table-header-rows>";
            x += "<table:table-row>";
            for (std::size_t c = 0; c < cols; ++c) {
                const CellSpan* m = cover[r][c];
                if (m && (m->row != static_cast<int>(r) || m->col != static_cast<int>(c))) {
                    x += "<table:covered-table-cell/>";
                    continue;
                }
                CellFormat f;
                if (r < b.cellFormats.size() && c < b.cellFormats[r].size()) f = b.cellFormats[r][c];
                std::uint32_t fill = f.fill;
                if (!fill && head) fill = 0xEEEEEE | kColorSet;
                if (!fill && b.banded && r % 2 == (b.header ? 0u : 1u)) fill = 0xF6F8FB | kColorSet;
                std::string props = " fo:padding=\"2.5pt\"";
                props += b.borders ? " fo:border=\"0.5pt solid #999999\"" : " fo:border=\"none\"";
                if (hasColor(fill)) props += " fo:background-color=\"" + hexColor(fill) + "\"";
                if (f.valign) props += std::string(" style:vertical-align=\"") + (f.valign == 'm' ? "middle" : "bottom") + "\"";
                const std::string cs = cells.get(props, [&](const std::string& name) {
                    return "<style:style style:name=\"" + name + "\" style:family=\"table-cell\"><style:table-cell-properties" + props + "/></style:style>";
                });
                x += "<table:table-cell table:style-name=\"" + cs + "\" office:value-type=\"string\"";
                if (m && m->cols > 1) x += " table:number-columns-spanned=\"" + std::to_string(std::min<std::size_t>(m->cols, cols - c)) + "\"";
                if (m && m->rows > 1) x += " table:number-rows-spanned=\"" + std::to_string(std::min<std::size_t>(m->rows, rows - r)) + "\"";
                x += ">";
                ParaFormat pf;
                pf.align = f.align;
                const std::string ps = paraStyle("Table_20_Contents", pf, false);
                Runs line;
                bool any = false;
                auto flush = [&] {
                    x += "<text:p text:style-name=\"" + ps + "\">" + runsXml(line, head, text) + "</text:p>";
                    line.clear();
                    any = true;
                };
                if (c < b.rows[r].size()) {
                    for (const auto& run : b.rows[r][c]) {
                        std::size_t from = 0;
                        for (std::size_t k = 0; k <= run.text.size(); ++k) {
                            if (k == run.text.size() || run.text[k] == '\n') {
                                Run part = run;
                                part.text = run.text.substr(from, k - from);
                                if (!part.text.empty() || !part.footnote.empty()) line.push_back(part);
                                if (k < run.text.size()) flush();
                                from = k + 1;
                            }
                        }
                    }
                }
                if (!line.empty() || !any) flush();
                x += "</table:table-cell>";
            }
            x += "</table:table-row>";
            if (head) x += "</table:table-header-rows>";
        }
        return x + "</table:table>";
    }

    std::string toc(const Block& b) {
        const int maxLevel = std::clamp(b.level, 1, 9);
        const Runs title = b.runs.empty() ? parseInline("Contents") : b.runs;
        std::string x = "<text:table-of-content text:protected=\"true\" text:name=\"Table of Contents1\"><text:table-of-content-source text:outline-level=\"" +
                        std::to_string(maxLevel) + "\"><text:index-title-template text:style-name=\"Contents_20_Heading\">" + esc(plainText(title)) +
                        "</text:index-title-template>";
        for (int l = 1; l <= maxLevel; ++l) {
            x += "<text:table-of-content-entry-template text:outline-level=\"" + std::to_string(l) + "\" text:style-name=\"Contents_20_" + std::to_string(l) +
                 "\"><text:index-entry-link-start text:style-name=\"Index_20_Link\"/><text:index-entry-chapter/><text:index-entry-text/>"
                 "<text:index-entry-tab-stop style:type=\"right\" style:leader-char=\".\"/><text:index-entry-page-number/><text:index-entry-link-end/>"
                 "</text:table-of-content-entry-template>";
        }
        x += "</text:table-of-content-source><text:index-body><text:index-title text:name=\"Table of Contents1_Head\">";
        x += paragraph("p", "Contents_20_Heading", title) + "</text:index-title>";
        const auto entries = outline(doc, maxLevel);
        std::vector<int> index;
        int k = 0;
        for (const auto& h : doc.blocks) {
            if (h.kind != Block::Heading) continue;
            if (h.level <= maxLevel) index.push_back(k);
            ++k;
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const int hi = i < index.size() ? index[i] : -1;
            const int pg = hi >= 0 && static_cast<std::size_t>(hi) < tocPages.size() ? tocPages[static_cast<std::size_t>(hi)] : 1;
            x += "<text:p text:style-name=\"Contents_20_" + std::to_string(std::clamp(entries[i].first, 1, 9)) + "\">" + esc(entries[i].second) + "<text:tab/>" +
                 std::to_string(std::max(pg, 1)) + "</text:p>";
        }
        return x + "</text:index-body></text:table-of-content>";
    }

    std::string bodyXml() {
        std::string x;
        bool breakNext = false;
        for (std::size_t i = 0; i < doc.blocks.size(); ++i) {
            const Block& b = doc.blocks[i];
            const bool pb = breakNext;
            breakNext = false;
            switch (b.kind) {
                case Block::Heading: {
                    const int lv = std::clamp(b.level, 1, 6);
                    x += paragraph("h", paraStyle("Heading_20_" + std::to_string(lv), b.fmt, pb), b.runs, " text:outline-level=\"" + std::to_string(lv) + "\"");
                    break;
                }
                case Block::Paragraph: x += paragraph("p", paraStyle(styleNameOf(b.style), b.fmt, pb), b.runs); break;
                case Block::Quote: x += paragraph("p", paraStyle(styleNameOf(b.style.empty() ? "Quote" : b.style), b.fmt, pb), b.runs); break;
                case Block::Code: x += paragraph("p", paraStyle("Preformatted_20_Text", ParaFormat{}, pb), b.runs); break;
                case Block::PageBreak:
                    if (i + 1 < doc.blocks.size() && doc.blocks[i + 1].kind != Block::List && doc.blocks[i + 1].kind != Block::Table &&
                        doc.blocks[i + 1].kind != Block::Toc && doc.blocks[i + 1].kind != Block::PageBreak) {
                        breakNext = true;
                    } else {
                        x += "<text:p text:style-name=\"" + paraStyle("Standard", ParaFormat{}, true) + "\"/>";
                    }
                    break;
                case Block::Toc:
                    if (pb) x += "<text:p text:style-name=\"" + paraStyle("Standard", ParaFormat{}, true) + "\"/>";
                    x += toc(b);
                    break;
                case Block::List: {
                    const std::string style = listStyle(b);
                    std::size_t k = 0;
                    x += listItems(b, k, 0, style, paraStyle("List_20_Paragraph", b.fmt, false));
                    break;
                }
                case Block::Table: x += table(b); break;
                case Block::Image: {
                    if (pb) x += "<text:p text:style-name=\"" + paraStyle("Standard", ParaFormat{}, true) + "\"/>";
                    x += image(b);
                    break;
                }
            }
        }
        return x;
    }

    // Header or footer text as paragraphs of the master page.
    std::string bandXml(const std::string& textIn, const char* style) {
        std::vector<std::string> parts;
        std::string cur;
        for (std::size_t i = 0; i < textIn.size(); ++i) {
            if (textIn[i] == '\\' && i + 1 < textIn.size() && textIn[i + 1] == '|') {
                cur += '|';
                ++i;
            } else if (textIn[i] == '|') {
                parts.push_back(trimCopy(cur));
                cur.clear();
            } else {
                cur += textIn[i];
            }
        }
        parts.push_back(trimCopy(cur));
        if (parts.size() > 3) parts.resize(3);
        auto partXml = [&](const std::string& t) {
            std::string out;
            std::size_t from = 0;
            auto textPart = [&](const std::string& s) {
                if (!s.empty()) out += runsXml(parseInline(fillFields(s, 1, 1, doc)), false, masterText, false);
            };
            for (std::size_t i = 0; i < t.size(); ++i) {
                if (t[i] != '{') continue;
                const std::string rest = lower(t.substr(i, 8));
                const bool page = startsWith(rest, "{page}"), pages = startsWith(rest, "{pages}");
                if (!page && !pages) continue;
                textPart(t.substr(from, i - from));
                out += page ? "<text:page-number text:select-page=\"current\">1</text:page-number>" : "<text:page-count>1</text:page-count>";
                i += page ? 5 : 6;
                from = i + 1;
            }
            textPart(t.substr(from));
            return out;
        };
        std::string x = std::string("<text:p text:style-name=\"") + style + "\">";
        if (parts.size() == 1) x += "<text:tab/>" + partXml(parts[0]);
        else if (parts.size() == 2) x += partXml(parts[0]) + "<text:tab/><text:tab/>" + partXml(parts[1]);
        else x += partXml(parts[0]) + "<text:tab/>" + partXml(parts[1]) + "<text:tab/>" + partXml(parts[2]);
        return x + "</text:p>";
    }

    std::string stylesXml() {
        const double W = contentWidth();
        std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles" + std::string(kOdtNs) + ">";
        s += "<office:font-face-decls>";
        for (const char* f : {"Liberation Sans", "Liberation Serif", "Liberation Mono"}) {
            s += std::string("<style:font-face style:name=\"") + f + "\" svg:font-family=\"'" + f + "'\" style:font-family-generic=\"" +
                 (std::string(f) == "Liberation Mono" ? "modern\" style:font-pitch=\"fixed" : std::string(f) == "Liberation Serif" ? "roman\" style:font-pitch=\"variable" : "swiss\" style:font-pitch=\"variable") +
                 "\"/>";
        }
        s += "</office:font-face-decls><office:styles>";
        s += std::string("<style:default-style style:family=\"paragraph\"><style:paragraph-properties style:tab-stop-distance=\"36pt\"/><style:text-properties style:font-name=\"") +
             fontFace(doc.page.font) + "\" fo:font-size=\"" + pt(doc.page.fontSize) + "\" fo:language=\"en\" fo:country=\"US\"/></style:default-style>";
        auto common = [&](const std::string& name, const std::string& display, const std::string& id, const std::string& parent, const std::string& extra) {
            const ResolvedStyle r = resolveStyle(doc, id);
            s += "<style:style style:name=\"" + name + "\" style:display-name=\"" + esc(display) + "\" style:family=\"paragraph\"";
            if (!parent.empty()) s += " style:parent-style-name=\"" + parent + "\"";
            s += extra + ">" + styleProps(r, true) + "</style:style>";
        };
        common("Standard", "Standard", "Normal", "", " style:class=\"text\"");
        common("Title", "Title", "Title", "Standard", " style:class=\"chapter\"");
        common("Subtitle", "Subtitle", "Subtitle", "Standard", " style:class=\"chapter\"");
        for (int i = 1; i <= 6; ++i) {
            common("Heading_20_" + std::to_string(i), "Heading " + std::to_string(i), "Heading" + std::to_string(i), "Standard",
                   " style:default-outline-level=\"" + std::to_string(i) + "\" style:class=\"text\"");
        }
        common("Quotations", "Quotations", "Quote", "Standard", " style:class=\"html\"");
        common("Preformatted_20_Text", "Preformatted Text", "Code", "Standard", " style:class=\"html\"");
        common("Caption", "Caption", "Caption", "Standard", " style:class=\"extra\"");
        common("List_20_Paragraph", "List Paragraph", "ListParagraph", "Standard", " style:class=\"list\"");
        for (const auto& [id, def] : doc.styles) {
            if (isBuiltinStyle(id)) continue;
            const std::string parent = def.basedOn.empty() ? "Standard" : styleNameOf(def.basedOn);
            common(styleNameOf(id), id, id, parent, "");
        }
        s += "<style:style style:name=\"Table_20_Contents\" style:display-name=\"Table Contents\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" "
             "style:class=\"extra\"><style:paragraph-properties fo:margin-top=\"0pt\" fo:margin-bottom=\"0pt\" fo:line-height=\"100%\"/></style:style>";
        s += "<style:style style:name=\"Footnote\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" style:class=\"extra\"><style:paragraph-properties "
             "fo:margin-top=\"0pt\" fo:margin-bottom=\"0pt\" fo:margin-left=\"12pt\" fo:text-indent=\"-12pt\"/><style:text-properties fo:font-size=\"" +
             pt(doc.page.fontSize * 0.8) + "\"/></style:style>";
        const std::string tabs = "<style:tab-stops><style:tab-stop style:position=\"" + pt(W / 2) + "\" style:type=\"center\"/><style:tab-stop style:position=\"" + pt(W) +
                                 "\" style:type=\"right\"/></style:tab-stops>";
        for (const char* hf : {"Header", "Footer"}) {
            s += std::string("<style:style style:name=\"") + hf + "\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" style:class=\"extra\">"
                 "<style:paragraph-properties fo:margin-top=\"0pt\" fo:margin-bottom=\"0pt\">" + tabs + "</style:paragraph-properties><style:text-properties fo:color=\"#666666\" "
                 "fo:font-size=\"9pt\"/></style:style>";
        }
        s += "<style:style style:name=\"Contents_20_Heading\" style:display-name=\"Contents Heading\" style:family=\"paragraph\" style:parent-style-name=\"Heading_20_1\" "
             "style:class=\"index\"><style:paragraph-properties text:number-lines=\"false\"/></style:style>";
        for (int i = 1; i <= 9; ++i) {
            s += "<style:style style:name=\"Contents_20_" + std::to_string(i) + "\" style:display-name=\"Contents " + std::to_string(i) +
                 "\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" style:class=\"index\"><style:paragraph-properties fo:margin-left=\"" +
                 pt((i - 1) * 14.0) + "\" fo:margin-top=\"0pt\" fo:margin-bottom=\"3pt\"><style:tab-stops><style:tab-stop style:position=\"" + pt(W - (i - 1) * 14.0) +
                 "\" style:type=\"right\" style:leader-style=\"dotted\" style:leader-text=\".\"/></style:tab-stops></style:paragraph-properties>" +
                 (i == 1 ? "<style:text-properties fo:font-weight=\"bold\"/>" : "") + "</style:style>";
        }
        s += "<style:style style:name=\"Internet_20_link\" style:display-name=\"Internet link\" style:family=\"text\"><style:text-properties fo:color=\"#0563c1\" "
             "style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"/></style:style>";
        s += "<style:style style:name=\"Index_20_Link\" style:display-name=\"Index Link\" style:family=\"text\"/>";
        s += "<text:notes-configuration text:note-class=\"footnote\" style:num-format=\"1\" text:start-value=\"0\" text:footnotes-position=\"page\" "
             "text:start-numbering-at=\"document\"/>";
        s += "</office:styles>";
        // the page and its header and footer
        const HeaderFooter& hf = doc.headerFooter;
        const std::string footer = !hf.footer.empty() || !doc.page.pageNumbers ? hf.footer : std::string("{page} / {pages}");
        std::string masterXml;
        if (!hf.header.empty()) masterXml += "<style:header>" + bandXml(hf.header, "Header") + "</style:header>";
        if (hf.differentFirst) masterXml += "<style:header-first>" + (hf.firstHeader.empty() ? std::string("<text:p text:style-name=\"Header\"/>") : bandXml(hf.firstHeader, "Header")) + "</style:header-first>";
        if (!footer.empty()) masterXml += "<style:footer>" + bandXml(footer, "Footer") + "</style:footer>";
        if (hf.differentFirst) masterXml += "<style:footer-first>" + (hf.firstFooter.empty() ? std::string("<text:p text:style-name=\"Footer\"/>") : bandXml(hf.firstFooter, "Footer")) + "</style:footer-first>";
        const bool hasHeader = !hf.header.empty() || (hf.differentFirst && !hf.firstHeader.empty());
        const bool hasFooter = !footer.empty() || (hf.differentFirst && !hf.firstFooter.empty());
        s += "<office:automatic-styles><style:page-layout style:name=\"pm1\"><style:page-layout-properties fo:page-width=\"" + pt(doc.page.width) + "\" fo:page-height=\"" +
             pt(doc.page.height) + "\" style:print-orientation=\"" + (doc.page.width > doc.page.height ? "landscape" : "portrait") + "\" fo:margin-top=\"" +
             pt(hasHeader ? std::max(12.0, doc.page.top - 24) : doc.page.top) + "\" fo:margin-bottom=\"" + pt(hasFooter ? std::max(12.0, doc.page.bottom - 24) : doc.page.bottom) +
             "\" fo:margin-left=\"" + pt(doc.page.left) + "\" fo:margin-right=\"" + pt(doc.page.right) + "\" style:writing-mode=\"lr-tb\"/>";
        s += "<style:header-style><style:header-footer-properties fo:min-height=\"12pt\" fo:margin-bottom=\"12pt\"/></style:header-style>";
        s += "<style:footer-style><style:header-footer-properties fo:min-height=\"12pt\" fo:margin-top=\"12pt\"/></style:footer-style></style:page-layout>";
        s += masterText.xml + "</office:automatic-styles>";
        s += "<office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\">" + masterXml + "</style:master-page></office:master-styles>";
        return s + "</office:document-styles>";
    }
};

}  // namespace

std::string writeOdt(const TextDocument& source) {
    const TextDocument doc = acceptedView(source);
    OdtWriter w(doc);
    if (std::any_of(doc.blocks.begin(), doc.blocks.end(), [](const Block& b) { return b.kind == Block::Toc; })) w.tocPages = headingPages(doc);
    const std::string body = w.bodyXml();
    std::string content = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content" + std::string(kOdtNs) + ">";
    content += "<office:font-face-decls><style:font-face style:name=\"Liberation Sans\" svg:font-family=\"'Liberation Sans'\"/><style:font-face style:name=\"Liberation Serif\" "
               "svg:font-family=\"'Liberation Serif'\"/><style:font-face style:name=\"Liberation Mono\" svg:font-family=\"'Liberation Mono'\" style:font-pitch=\"fixed\"/></office:font-face-decls>";
    content += "<office:automatic-styles>" + w.para.xml + w.text.xml + w.columns.xml + w.cells.xml + w.listStyles +
               "<style:style style:name=\"fr1\" style:family=\"graphic\"><style:graphic-properties style:vertical-pos=\"top\" style:vertical-rel=\"baseline\"/></style:style>"
               "</office:automatic-styles>";
    content += "<office:body><office:text>" + body + "</office:text></office:body></office:document-content>";
    const std::string styles = w.stylesXml();  // after the body: header runs add their styles
    std::string meta = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta" + std::string(kOdtNs) + "><office:meta><meta:generator>wetwaresuite</meta:generator>";
    if (!doc.title.empty()) meta += "<dc:title>" + esc(doc.title) + "</dc:title>";
    if (!doc.author.empty()) meta += "<meta:initial-creator>" + esc(doc.author) + "</meta:initial-creator><dc:creator>" + esc(doc.author) + "</dc:creator>";
    meta += "</office:meta></office:document-meta>";
    std::string manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>";
    for (const auto& m : w.media) {
        manifest += "<manifest:file-entry manifest:full-path=\"" + m.name + "\" manifest:media-type=\"" + (endsWith(m.name, ".png") ? "image/png" : "image/jpeg") + "\"/>";
    }
    manifest += "</manifest:manifest>";
    std::vector<raster::ZipEntry> entries;
    entries.push_back({"mimetype", "application/vnd.oasis.opendocument.text"});
    entries.push_back({"content.xml", content});
    entries.push_back({"styles.xml", styles});
    entries.push_back({"meta.xml", meta});
    for (auto& m : w.media) entries.push_back(std::move(m));
    entries.push_back({"META-INF/manifest.xml", manifest});
    return raster::writeZip(entries);
}

}  // namespace wetwaresuite::office
