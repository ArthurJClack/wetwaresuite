// Writing DOCX: styles, paragraph and run formats, lists with their numbering,
// tables with merged cells, images, headers and footers with page fields,
// footnotes, comments, tracked changes and a table of contents.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <map>

#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "text.hpp"
#include "xml.hpp"

namespace wetwaresuite::office {

namespace {

const char* kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
const char* kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char* kRelBase = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
const char* kCtBase = "application/vnd.openxmlformats-officedocument.wordprocessingml.";

std::string twips(double pt) { return std::to_string(static_cast<long>(std::lround(pt * 20))); }
std::string emu(double pt) { return std::to_string(static_cast<long long>(std::llround(pt * 12700))); }
std::string halfPoints(double pt) { return std::to_string(static_cast<long>(std::lround(pt * 2))); }
std::string hex(std::uint32_t c) {
    static const char* d = "0123456789ABCDEF";
    std::string s(6, '0');
    for (int i = 5; i >= 0; --i, c >>= 4) s[static_cast<std::size_t>(i)] = d[c & 15];
    return s;
}

bool isJpeg(const std::string& b) { return b.size() > 2 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8; }

std::string fontName(const std::string& f, pdf::Family fallback) {
    pdf::Family fam = fallback;
    if (f == "serif") fam = pdf::Family::Serif;
    else if (f == "mono") fam = pdf::Family::Mono;
    else if (f == "sans") fam = pdf::Family::Sans;
    return fam == pdf::Family::Serif ? "Times New Roman" : fam == pdf::Family::Mono ? "Courier New" : "Calibri";
}

std::string isoNow() {
    const std::time_t now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    return buf;
}

// Natural size of an image block in points, fitted to `maxWidth`.
void imageSize(const Block& b, double maxWidth, double& w, double& h) {
    double pw = b.pixels ? b.pixels->pixels.width() : 100, ph = b.pixels ? b.pixels->pixels.height() : 100;
    if (pw <= 0) pw = 1;
    w = b.width > 0 ? b.width : pw * 0.75;
    if (w > maxWidth) w = maxWidth;
    h = w * ph / pw;
}

void pPrFormat(xml::Writer& w, const ParaFormat& f) {
    if (f.keepWithNext) w.leaf("w:keepNext");
    if (f.pageBreakBefore) w.leaf("w:pageBreakBefore");
    if (f.before >= 0 || f.after >= 0 || f.lineSpacing > 0) {
        std::vector<std::pair<std::string, std::string>> a;
        if (f.before >= 0) a.emplace_back("w:before", twips(f.before));
        if (f.after >= 0) a.emplace_back("w:after", twips(f.after));
        if (f.lineSpacing > 0) {
            a.emplace_back("w:line", std::to_string(static_cast<long>(std::lround(f.lineSpacing * 240))));
            a.emplace_back("w:lineRule", "auto");
        }
        w.leaf("w:spacing", a);
    }
    if (f.indent || f.indentRight || f.firstLine) {
        std::vector<std::pair<std::string, std::string>> a;
        if (f.indent) a.emplace_back("w:left", twips(f.indent));
        if (f.indentRight) a.emplace_back("w:right", twips(f.indentRight));
        if (f.firstLine > 0) a.emplace_back("w:firstLine", twips(f.firstLine));
        if (f.firstLine < 0) a.emplace_back("w:hanging", twips(-f.firstLine));
        w.leaf("w:ind", a);
    }
    if (f.align) w.leaf("w:jc", {{"w:val", f.align == 'c' ? "center" : f.align == 'r' ? "right" : f.align == 'j' ? "both" : "left"}});
}

struct Rel {
    std::string id, type, target;
    bool external = false;
};

struct DocxWriter {
    const TextDocument& doc;
    xml::Writer w;
    std::vector<Rel> rels;
    std::vector<raster::ZipEntry> media;
    std::map<std::string, std::string> linkIds;
    std::vector<std::string> footnotes;  // inline Markdown of each note, id = index + 1
    // numbering: one num per numbered list (restarting at its start), bullets share num 1
    struct Num {
        std::string format;
        int start = 1;
    };
    std::vector<Num> nums;  // numId = index + 2
    int pictures = 0;
    std::vector<int> tocPages;
    int bookmark = 0;

    explicit DocxWriter(const TextDocument& d) : doc(d) {}

    std::string addRel(const std::string& type, const std::string& target, bool external) {
        std::string id = "rId" + std::to_string(rels.size() + 10);
        rels.push_back({id, kRelBase + type, target, external});
        return id;
    }

    void rPr(const Run& r, bool forceBold) {
        const bool any = r.bold || forceBold || r.italic || r.underline || r.code || r.strike || r.vertical || r.color || r.highlight || r.size > 0 ||
                         !r.font.empty() || !r.link.empty();
        if (!any) return;
        w.open("w:rPr");
        if (!r.link.empty()) w.leaf("w:rStyle", {{"w:val", "Hyperlink"}});
        if (r.code || !r.font.empty()) {
            const std::string f = r.code ? "Courier New" : fontName(r.font, doc.page.font);
            w.leaf("w:rFonts", {{"w:ascii", f}, {"w:hAnsi", f}, {"w:cs", f}});
        }
        if (r.bold || forceBold) w.leaf("w:b");
        if (r.italic) w.leaf("w:i");
        if (r.strike) w.leaf("w:strike");
        if (hasColor(r.color)) w.leaf("w:color", {{"w:val", hex(rgbOf(r.color))}});
        if (r.size > 0) w.leaf("w:sz", {{"w:val", halfPoints(r.size)}}).leaf("w:szCs", {{"w:val", halfPoints(r.size)}});
        if (r.underline && r.link.empty()) w.leaf("w:u", {{"w:val", "single"}});
        if (hasColor(r.highlight)) w.leaf("w:shd", {{"w:val", "clear"}, {"w:color", "auto"}, {"w:fill", hex(rgbOf(r.highlight))}});
        if (r.vertical) w.leaf("w:vertAlign", {{"w:val", r.vertical > 0 ? "superscript" : "subscript"}});
        w.close();
    }

    void textRun(const Run& r, bool forceBold, bool deleted) {
        if (!r.footnote.empty()) {
            footnotes.push_back(r.footnote);
            w.open("w:r").open("w:rPr").leaf("w:rStyle", {{"w:val", "FootnoteReference"}}).close();
            w.leaf("w:footnoteReference", {{"w:id", std::to_string(footnotes.size())}});
            w.close();
            return;
        }
        w.open("w:r");
        rPr(r, forceBold);
        std::string cur;
        auto flushText = [&] {
            if (cur.empty()) return;
            w.open(deleted ? "w:delText" : "w:t").attr("xml:space", "preserve").text(cur).close();
            cur.clear();
        };
        for (char c : r.text) {
            if (c == '\n') {
                flushText();
                w.leaf("w:br");
            } else if (c == '\t') {
                flushText();
                w.leaf("w:tab");
            } else {
                cur += c;
            }
        }
        flushText();
        w.close();
    }

    // Runs with their links, tracked changes and comment ranges.
    void runs(const Runs& rs, bool forceBold = false) {
        int openComment = 0;
        auto closeComment = [&] {
            if (!openComment) return;
            w.leaf("w:commentRangeEnd", {{"w:id", std::to_string(openComment)}});
            w.open("w:r").open("w:rPr").leaf("w:rStyle", {{"w:val", "CommentReference"}}).close();
            w.leaf("w:commentReference", {{"w:id", std::to_string(openComment)}}).close();
            openComment = 0;
        };
        for (std::size_t i = 0; i < rs.size();) {
            const Run& r = rs[i];
            if (r.comment != openComment) {
                closeComment();
                if (r.comment && doc.comment(r.comment)) {
                    openComment = r.comment;
                    w.leaf("w:commentRangeStart", {{"w:id", std::to_string(openComment)}});
                }
            }
            // a group of runs sharing link, change and comment
            std::size_t j = i;
            while (j < rs.size() && rs[j].link == r.link && rs[j].change == r.change && rs[j].comment == r.comment) ++j;
            const TrackedChange* ch = r.change ? doc.change(r.change) : nullptr;
            if (ch) {
                w.open(ch->insert ? "w:ins" : "w:del").attr("w:id", std::to_string(1000 + ch->id)).attr("w:author", ch->author.empty() ? "wetwaresuite" : ch->author);
                if (!ch->date.empty()) w.attr("w:date", ch->date);
            }
            std::string linkId;
            if (!r.link.empty()) {
                auto it = linkIds.find(r.link);
                if (r.link[0] == '#') {
                    w.open("w:hyperlink").attr("w:anchor", r.link.substr(1)).attr("w:history", "1");
                } else {
                    linkId = it != linkIds.end() ? it->second : (linkIds[r.link] = addRel("hyperlink", r.link, true));
                    w.open("w:hyperlink").attr("r:id", linkId).attr("w:history", "1");
                }
            }
            for (std::size_t k = i; k < j; ++k) textRun(rs[k], forceBold, ch && !ch->insert);
            if (!r.link.empty()) w.close();
            if (ch) w.close();
            i = j;
        }
        closeComment();
    }

    void paragraph(const std::string& style, const Runs& rs, const ParaFormat& f, int numId = 0, int ilvl = 0, bool forceBold = false) {
        w.open("w:p");
        if (!style.empty() || numId || f.any()) {
            w.open("w:pPr");
            if (!style.empty()) w.leaf("w:pStyle", {{"w:val", style}});
            if (numId) w.open("w:numPr").leaf("w:ilvl", {{"w:val", std::to_string(ilvl)}}).leaf("w:numId", {{"w:val", std::to_string(numId)}}).close();
            pPrFormat(w, f);
            w.close();
        }
        runs(rs, forceBold);
        w.close();
    }

    void image(const Block& b) {
        const double contentW = doc.page.width - doc.page.left - doc.page.right;
        double wpt, hpt;
        imageSize(b, contentW, wpt, hpt);
        ++pictures;
        std::string name = "image" + std::to_string(pictures) + (isJpeg(b.image) ? ".jpeg" : ".png");
        media.push_back({"word/media/" + name, b.image});
        std::string id = addRel("image", "media/" + name, false);
        const std::string cx = emu(wpt), cy = emu(hpt);
        w.open("w:p");
        ParaFormat f = b.fmt;
        if (!f.align) f.align = 'c';
        w.open("w:pPr");
        if (!b.style.empty()) w.leaf("w:pStyle", {{"w:val", b.style}});
        pPrFormat(w, f);
        w.close();
        w.open("w:r").open("w:drawing");
        w.open("wp:inline").attr("distT", "0").attr("distB", "0").attr("distL", "0").attr("distR", "0");
        w.leaf("wp:extent", {{"cx", cx}, {"cy", cy}});
        w.leaf("wp:effectExtent", {{"l", "0"}, {"t", "0"}, {"r", "0"}, {"b", "0"}});
        w.leaf("wp:docPr", {{"id", std::to_string(pictures)}, {"name", "Picture " + std::to_string(pictures)}, {"descr", b.alt}});
        w.open("wp:cNvGraphicFramePr").leaf("a:graphicFrameLocks", {{"xmlns:a", "http://schemas.openxmlformats.org/drawingml/2006/main"}, {"noChangeAspect", "1"}}).close();
        w.open("a:graphic").attr("xmlns:a", "http://schemas.openxmlformats.org/drawingml/2006/main");
        w.open("a:graphicData").attr("uri", "http://schemas.openxmlformats.org/drawingml/2006/picture");
        w.open("pic:pic").attr("xmlns:pic", "http://schemas.openxmlformats.org/drawingml/2006/picture");
        w.open("pic:nvPicPr").leaf("pic:cNvPr", {{"id", "0"}, {"name", name}}).leaf("pic:cNvPicPr").close();
        w.open("pic:blipFill").leaf("a:blip", {{"r:embed", id}}).open("a:stretch").leaf("a:fillRect").close().close();
        w.open("pic:spPr");
        w.open("a:xfrm").leaf("a:off", {{"x", "0"}, {"y", "0"}}).leaf("a:ext", {{"cx", cx}, {"cy", cy}}).close();
        w.open("a:prstGeom").attr("prst", "rect").leaf("a:avLst").close();
        w.close();                          // spPr
        w.close().close().close();          // pic, graphicData, graphic
        w.close().close().close().close();  // inline, drawing, r, p
    }

    void table(const Block& b) {
        std::size_t cols = 0;
        for (const auto& r : b.rows) cols = std::max(cols, r.size());
        if (!cols) return;
        const std::size_t rows = b.rows.size();
        const double contentW = doc.page.width - doc.page.left - doc.page.right;
        std::vector<double> widths(cols, contentW / static_cast<double>(cols));
        double given = 0;
        for (std::size_t c = 0; c < cols && c < b.colWidths.size(); ++c) given += std::max(0.0, b.colWidths[c]);
        const bool fixed = b.colWidths.size() >= cols && given > 0;
        if (fixed) {
            for (std::size_t c = 0; c < cols; ++c) widths[c] = contentW * std::max(0.0, b.colWidths[c]) / given;
        }
        std::vector<std::vector<const CellSpan*>> cover(rows, std::vector<const CellSpan*>(cols, nullptr));
        for (const auto& m : b.merges) {
            for (int r = m.row; r < m.row + m.rows && r < static_cast<int>(rows); ++r) {
                for (int c = m.col; c < m.col + m.cols && c < static_cast<int>(cols); ++c) {
                    if (r >= 0 && c >= 0) cover[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = &m;
                }
            }
        }
        w.open("w:tbl");
        w.open("w:tblPr").leaf("w:tblStyle", {{"w:val", b.borders ? "TableGrid" : "TablePlain"}});
        if (fixed) w.leaf("w:tblW", {{"w:w", twips(contentW)}, {"w:type", "dxa"}}).leaf("w:tblLayout", {{"w:type", "fixed"}});
        else w.leaf("w:tblW", {{"w:w", "0"}, {"w:type", "auto"}});
        w.leaf("w:tblLook", {{"w:val", "04A0"}, {"w:firstRow", b.header ? "1" : "0"}, {"w:lastRow", "0"}, {"w:firstColumn", "0"}, {"w:lastColumn", "0"}, {"w:noHBand", b.banded ? "0" : "1"}, {"w:noVBand", "1"}});
        w.close();
        w.open("w:tblGrid");
        for (std::size_t c = 0; c < cols; ++c) w.leaf("w:gridCol", {{"w:w", twips(widths[c])}});
        w.close();
        for (std::size_t r = 0; r < rows; ++r) {
            const bool head = r == 0 && b.header;
            w.open("w:tr");
            if (head) w.open("w:trPr").leaf("w:tblHeader").close();
            for (std::size_t c = 0; c < cols;) {
                const CellSpan* m = cover[r][c];
                const std::size_t span = m ? static_cast<std::size_t>(std::max(1, m->cols)) : 1;
                if (m && m->col != static_cast<int>(c)) {  // inside a horizontal merge: written by its first column
                    ++c;
                    continue;
                }
                double cw = 0;
                for (std::size_t k = c; k < std::min(cols, c + span); ++k) cw += widths[k];
                CellFormat f;
                if (r < b.cellFormats.size() && c < b.cellFormats[r].size()) f = b.cellFormats[r][c];
                std::uint32_t fill = f.fill;
                if (!fill && head) fill = 0xEEEEEE | kColorSet;
                if (!fill && b.banded && r % 2 == (b.header ? 0u : 1u)) fill = 0xF6F8FB | kColorSet;
                w.open("w:tc").open("w:tcPr").leaf("w:tcW", {{"w:w", twips(cw)}, {"w:type", "dxa"}});
                if (span > 1) w.leaf("w:gridSpan", {{"w:val", std::to_string(span)}});
                if (m && m->rows > 1) {
                    if (m->row == static_cast<int>(r)) w.leaf("w:vMerge", {{"w:val", "restart"}});
                    else w.leaf("w:vMerge");
                }
                if (hasColor(fill)) w.leaf("w:shd", {{"w:val", "clear"}, {"w:color", "auto"}, {"w:fill", hex(rgbOf(fill))}});
                if (f.valign) w.leaf("w:vAlign", {{"w:val", f.valign == 'm' ? "center" : "bottom"}});
                w.close();
                const bool content = !m || m->row == static_cast<int>(r);
                ParaFormat pf;
                pf.align = f.align;
                pf.before = pf.after = 0;
                if (content && c < b.rows[r].size()) {
                    // a line break in a cell starts a new paragraph, as Word writes it
                    Runs line;
                    auto flushLine = [&] { paragraph("", line, pf, 0, 0, head); line.clear(); };
                    for (const auto& run : b.rows[r][c]) {
                        std::size_t from = 0;
                        for (std::size_t k = 0; k <= run.text.size(); ++k) {
                            if (k == run.text.size() || run.text[k] == '\n') {
                                Run part = run;
                                part.text = run.text.substr(from, k - from);
                                if (!part.text.empty() || !part.footnote.empty()) line.push_back(part);
                                if (k < run.text.size()) flushLine();
                                from = k + 1;
                            }
                        }
                    }
                    flushLine();
                } else {
                    paragraph("", {}, pf);
                }
                w.close();
                c += span;
            }
            w.close();
        }
        w.close();
    }

    void toc(const Block& b) {
        const int maxLevel = std::clamp(b.level, 1, 9);
        w.open("w:sdt").open("w:sdtPr").open("w:docPartObj").leaf("w:docPartGallery", {{"w:val", "Table of Contents"}}).leaf("w:docPartUnique").close().close();
        w.open("w:sdtContent");
        paragraph("TOCHeading", b.runs.empty() ? parseInline("Contents") : b.runs, ParaFormat{});
        const auto entries = outline(doc, maxLevel);
        std::vector<int> index;
        int k = 0;
        for (const auto& x : doc.blocks) {
            if (x.kind != Block::Heading) continue;
            if (x.level <= maxLevel) index.push_back(k);
            ++k;
        }
        const std::string instr = " TOC \\o \"1-" + std::to_string(maxLevel) + "\" \\h \\z \\u ";
        for (std::size_t i = 0; i <= entries.size(); ++i) {
            w.open("w:p");
            if (i < entries.size()) w.open("w:pPr").leaf("w:pStyle", {{"w:val", "TOC" + std::to_string(std::clamp(entries[i].first, 1, 9))}}).close();
            if (i == 0) {
                w.open("w:r").leaf("w:fldChar", {{"w:fldCharType", "begin"}}).close();
                w.open("w:r").open("w:instrText").attr("xml:space", "preserve").text(instr).close().close();
                w.open("w:r").leaf("w:fldChar", {{"w:fldCharType", "separate"}}).close();
            }
            if (i < entries.size()) {
                const int hi = i < index.size() ? index[i] : -1;
                const int pg = hi >= 0 && static_cast<std::size_t>(hi) < tocPages.size() ? tocPages[static_cast<std::size_t>(hi)] : 0;
                w.open("w:hyperlink").attr("w:anchor", "_Toc" + std::to_string(hi + 1)).attr("w:history", "1");
                w.open("w:r").open("w:t").attr("xml:space", "preserve").text(entries[i].second).close().close();
                w.open("w:r").leaf("w:tab").close();
                w.open("w:r").open("w:t").text(std::to_string(std::max(pg, 1))).close().close();
                w.close();
            } else {
                w.open("w:r").leaf("w:fldChar", {{"w:fldCharType", "end"}}).close();
            }
            w.close();
        }
        w.close().close();
    }

    std::string documentXml() {
        w.open("w:document").attr("xmlns:w", kW).attr("xmlns:r", kR).attr("xmlns:wp", "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing");
        w.open("w:body");
        std::map<std::string, int> abstractFor;
        for (std::size_t i = 0; i < doc.blocks.size(); ++i) {
            const Block& b = doc.blocks[i];
            switch (b.kind) {
                case Block::Heading: {
                    // a bookmark per heading, for the table of contents' links
                    ++bookmark;
                    w.open("w:p");
                    w.open("w:pPr").leaf("w:pStyle", {{"w:val", "Heading" + std::to_string(std::clamp(b.level, 1, 6))}});
                    pPrFormat(w, b.fmt);
                    w.close();
                    w.leaf("w:bookmarkStart", {{"w:id", std::to_string(bookmark)}, {"w:name", "_Toc" + std::to_string(bookmark)}});
                    runs(b.runs);
                    w.leaf("w:bookmarkEnd", {{"w:id", std::to_string(bookmark)}});
                    w.close();
                    break;
                }
                case Block::Paragraph: paragraph(b.style.empty() || b.style == "Normal" ? "" : b.style, b.runs, b.fmt); break;
                case Block::Quote: paragraph(b.style.empty() ? "Quote" : b.style, b.runs, b.fmt); break;
                case Block::Code: paragraph("Code", b.runs, b.fmt); break;
                case Block::PageBreak: w.open("w:p").open("w:r").leaf("w:br", {{"w:type", "page"}}).close().close(); break;
                case Block::Toc: toc(b); break;
                case Block::List: {
                    int numId = 1;
                    const std::string fmt = !b.numbering.empty() ? b.numbering : b.ordered ? "decimal" : "bullet";
                    if (fmt != "bullet") {
                        nums.push_back({fmt, b.start});
                        numId = static_cast<int>(nums.size()) + 1;
                    }
                    for (const auto& it : b.items) paragraph("ListParagraph", it.runs, b.fmt, numId, std::clamp(it.level, 0, 8));
                    break;
                }
                case Block::Table:
                    table(b);
                    if (i + 1 == doc.blocks.size() || doc.blocks[i + 1].kind == Block::Table) w.leaf("w:p");
                    break;
                case Block::Image: image(b); break;
            }
        }
        const HeaderFooter& hf = doc.headerFooter;
        std::vector<std::pair<std::string, std::string>> refs;  // (element, rId) for sectPr
        auto part = [&](const std::string& kind, const std::string& which, const std::string& text) {
            const int n = static_cast<int>(headerParts.size()) + 1;
            const std::string file = kind + std::to_string(n) + ".xml";
            headerParts.push_back({file, kind, bandXml(kind, text)});
            refs.emplace_back(std::string("w:") + kind + "Reference" + "|" + which, addRel(kind, file, false));
        };
        const std::string footer = !hf.footer.empty() || !doc.page.pageNumbers ? hf.footer : std::string("{page} / {pages}");
        if (!hf.header.empty()) part("header", "default", hf.header);
        if (!footer.empty()) part("footer", "default", footer);
        if (hf.differentFirst) {
            part("header", "first", hf.firstHeader);
            part("footer", "first", hf.firstFooter);
        }
        w.open("w:sectPr");
        for (const auto& [el, rid] : refs) {
            const auto bar = el.find('|');
            w.leaf(el.substr(0, bar), {{"w:type", el.substr(bar + 1)}, {"r:id", rid}});
        }
        std::vector<std::pair<std::string, std::string>> size = {{"w:w", twips(doc.page.width)}, {"w:h", twips(doc.page.height)}};
        if (doc.page.width > doc.page.height) size.emplace_back("w:orient", "landscape");
        w.leaf("w:pgSz", size);
        w.leaf("w:pgMar", {{"w:top", twips(doc.page.top)},
                           {"w:right", twips(doc.page.right)},
                           {"w:bottom", twips(doc.page.bottom)},
                           {"w:left", twips(doc.page.left)},
                           {"w:header", twips(doc.page.top / 2 - 6)},
                           {"w:footer", twips(doc.page.bottom / 2 - 6)},
                           {"w:gutter", "0"}});
        if (hf.differentFirst) w.leaf("w:titlePg");
        w.close();
        w.close().close();
        return w.str();
    }

    struct Part {
        std::string file, kind, xml;
    };
    std::vector<Part> headerParts;

    // A header or footer: "left | centre | right" with fields as Word fields.
    std::string bandXml(const std::string& kind, const std::string& text) {
        xml::Writer h;
        h.open(kind == "header" ? "w:hdr" : "w:ftr").attr("xmlns:w", kW).attr("xmlns:r", kR);
        std::vector<std::string> parts;
        std::string cur;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '|') {
                cur += '|';
                ++i;
            } else if (text[i] == '|') {
                parts.push_back(trimCopy(cur));
                cur.clear();
            } else {
                cur += text[i];
            }
        }
        parts.push_back(trimCopy(cur));
        const double contentW = doc.page.width - doc.page.left - doc.page.right;
        h.open("w:p").open("w:pPr").leaf("w:pStyle", {{"w:val", kind == "header" ? "Header" : "Footer"}});
        if (parts.size() == 1) {
            h.leaf("w:jc", {{"w:val", "center"}});
        } else {
            h.open("w:tabs");
            if (parts.size() == 3) h.leaf("w:tab", {{"w:val", "center"}, {"w:pos", twips(contentW / 2)}});
            h.leaf("w:tab", {{"w:val", "right"}, {"w:pos", twips(contentW)}});
            h.close();
        }
        h.close();
        for (std::size_t k = 0; k < parts.size() && k < 3; ++k) {
            if (k) h.open("w:r").leaf("w:tab").close();
            const std::string& t = parts[k];
            std::size_t from = 0;
            auto textPart = [&](const std::string& s) {
                if (s.empty()) return;
                for (const auto& r : parseInline(fillFields(s, 1, 1, doc))) {
                    h.open("w:r");
                    if (r.bold || r.italic) {
                        h.open("w:rPr");
                        if (r.bold) h.leaf("w:b");
                        if (r.italic) h.leaf("w:i");
                        h.close();
                    }
                    h.open("w:t").attr("xml:space", "preserve").text(r.text).close().close();
                }
            };
            for (std::size_t i = 0; i < t.size(); ++i) {
                if (t[i] != '{') continue;
                const std::string rest = lower(t.substr(i, 8));
                const char* field = startsWith(rest, "{page}") ? "PAGE" : startsWith(rest, "{pages}") ? "NUMPAGES" : nullptr;
                if (!field) continue;
                textPart(t.substr(from, i - from));
                h.open("w:fldSimple").attr("w:instr", std::string(" ") + field + " ").open("w:r").open("w:t").text("1").close().close().close();
                i += std::string(field) == "PAGE" ? 5 : 6;
                from = i + 1;
            }
            textPart(t.substr(from));
        }
        h.close();
        return h.str();
    }
};

void runPr(xml::Writer& s, const ResolvedStyle& r) {
    s.open("w:rPr");
    {
        const std::string f = r.font == pdf::Family::Serif ? "Times New Roman" : r.font == pdf::Family::Mono ? "Courier New" : "Calibri";
        s.leaf("w:rFonts", {{"w:ascii", f}, {"w:hAnsi", f}, {"w:cs", f}});
    }
    s.leaf("w:b", {{"w:val", r.bold ? "1" : "0"}}).leaf("w:bCs", {{"w:val", r.bold ? "1" : "0"}});
    s.leaf("w:i", {{"w:val", r.italic ? "1" : "0"}});
    if (hasColor(r.color)) s.leaf("w:color", {{"w:val", hex(rgbOf(r.color))}});
    s.leaf("w:sz", {{"w:val", halfPoints(r.size)}}).leaf("w:szCs", {{"w:val", halfPoints(r.size)}});
    if (r.underline) s.leaf("w:u", {{"w:val", "single"}});
    s.close();
}

void paraPr(xml::Writer& s, const ResolvedStyle& r, int outline) {
    s.open("w:pPr");
    if (r.keepWithNext) s.leaf("w:keepNext").leaf("w:keepLines");
    s.leaf("w:spacing", {{"w:before", twips(r.before)}, {"w:after", twips(r.after)}, {"w:line", std::to_string(static_cast<long>(std::lround(r.lineSpacing * 240)))}, {"w:lineRule", "auto"}});
    if (r.indent || r.indentRight || r.firstLine) {
        std::vector<std::pair<std::string, std::string>> a;
        if (r.indent) a.emplace_back("w:left", twips(r.indent));
        if (r.indentRight) a.emplace_back("w:right", twips(r.indentRight));
        if (r.firstLine > 0) a.emplace_back("w:firstLine", twips(r.firstLine));
        if (r.firstLine < 0) a.emplace_back("w:hanging", twips(-r.firstLine));
        s.leaf("w:ind", a);
    }
    if (r.align != 'l') s.leaf("w:jc", {{"w:val", r.align == 'c' ? "center" : r.align == 'r' ? "right" : "both"}});
    if (outline > 0) s.leaf("w:outlineLvl", {{"w:val", std::to_string(outline - 1)}});
    s.close();
}

std::string stylesXml(const TextDocument& doc) {
    xml::Writer s;
    const std::string font = fontName("", doc.page.font);
    const std::string size = halfPoints(doc.page.fontSize);
    s.open("w:styles").attr("xmlns:w", kW);
    s.open("w:docDefaults");
    s.open("w:rPrDefault").open("w:rPr").leaf("w:rFonts", {{"w:ascii", font}, {"w:hAnsi", font}, {"w:eastAsia", font}, {"w:cs", font}});
    s.leaf("w:sz", {{"w:val", size}}).leaf("w:szCs", {{"w:val", size}}).leaf("w:lang", {{"w:val", "en-US"}}).close().close();
    s.open("w:pPrDefault").open("w:pPr").leaf("w:spacing", {{"w:after", "160"}, {"w:line", "259"}, {"w:lineRule", "auto"}}).close().close();
    s.close();
    auto paragraphStyle = [&](const std::string& id, const std::string& name, const std::string& basedOn, const std::string& next, int outline, bool isDefault, bool quick) {
        const ResolvedStyle r = resolveStyle(doc, id);
        s.open("w:style").attr("w:type", "paragraph");
        if (isDefault) s.attr("w:default", "1");
        s.attr("w:styleId", id).leaf("w:name", {{"w:val", name}});
        if (!basedOn.empty()) s.leaf("w:basedOn", {{"w:val", basedOn}});
        if (!next.empty()) s.leaf("w:next", {{"w:val", next}});
        if (outline) s.leaf("w:uiPriority", {{"w:val", "9"}});
        if (quick) s.leaf("w:qFormat");
        paraPr(s, r, outline);
        runPr(s, r);
        s.close();
    };
    paragraphStyle("Normal", "Normal", "", "", 0, true, true);
    paragraphStyle("Title", "Title", "Normal", "Normal", 0, false, true);
    paragraphStyle("Subtitle", "Subtitle", "Normal", "Normal", 0, false, true);
    for (int i = 1; i <= 6; ++i) paragraphStyle("Heading" + std::to_string(i), "heading " + std::to_string(i), "Normal", "Normal", i, false, true);
    paragraphStyle("Quote", "Quote", "Normal", "Normal", 0, false, true);
    paragraphStyle("Code", "Code", "Normal", "", 0, false, true);
    paragraphStyle("Caption", "caption", "Normal", "Normal", 0, false, true);
    paragraphStyle("ListParagraph", "List Paragraph", "Normal", "", 0, false, true);
    // your own styles
    for (const auto& [id, def] : doc.styles) {
        if (isBuiltinStyle(id)) continue;
        paragraphStyle(id, def.name.empty() ? id : def.name, def.basedOn.empty() ? "Normal" : def.basedOn, "", 0, false, true);
    }
    // the rest Word expects for footnotes, comments, headers and the table of contents
    s.open("w:style").attr("w:type", "paragraph").attr("w:styleId", "FootnoteText").leaf("w:name", {{"w:val", "footnote text"}}).leaf("w:basedOn", {{"w:val", "Normal"}});
    s.open("w:pPr").leaf("w:spacing", {{"w:after", "0"}, {"w:line", "240"}, {"w:lineRule", "auto"}}).close();
    s.open("w:rPr").leaf("w:sz", {{"w:val", halfPoints(doc.page.fontSize * 0.8)}}).close().close();
    s.open("w:style").attr("w:type", "character").attr("w:styleId", "FootnoteReference").leaf("w:name", {{"w:val", "footnote reference"}});
    s.open("w:rPr").leaf("w:vertAlign", {{"w:val", "superscript"}}).close().close();
    s.open("w:style").attr("w:type", "paragraph").attr("w:styleId", "CommentText").leaf("w:name", {{"w:val", "annotation text"}}).leaf("w:basedOn", {{"w:val", "Normal"}});
    s.open("w:rPr").leaf("w:sz", {{"w:val", "20"}}).close().close();
    s.open("w:style").attr("w:type", "character").attr("w:styleId", "CommentReference").leaf("w:name", {{"w:val", "annotation reference"}});
    s.open("w:rPr").leaf("w:sz", {{"w:val", "16"}}).close().close();
    for (const char* hf : {"Header", "Footer"}) {
        s.open("w:style").attr("w:type", "paragraph").attr("w:styleId", hf).leaf("w:name", {{"w:val", lower(hf)}}).leaf("w:basedOn", {{"w:val", "Normal"}});
        s.open("w:pPr").leaf("w:spacing", {{"w:after", "0"}}).close();
        s.open("w:rPr").leaf("w:color", {{"w:val", "666666"}}).leaf("w:sz", {{"w:val", "18"}}).close().close();
    }
    s.open("w:style").attr("w:type", "paragraph").attr("w:styleId", "TOCHeading").leaf("w:name", {{"w:val", "TOC Heading"}}).leaf("w:basedOn", {{"w:val", "Heading1"}}).leaf("w:next", {{"w:val", "Normal"}});
    s.open("w:pPr").leaf("w:outlineLvl", {{"w:val", "9"}}).close().close();
    const double contentW = doc.page.width - doc.page.left - doc.page.right;
    for (int i = 1; i <= 9; ++i) {
        s.open("w:style").attr("w:type", "paragraph").attr("w:styleId", "TOC" + std::to_string(i)).leaf("w:name", {{"w:val", "toc " + std::to_string(i)}}).leaf("w:basedOn", {{"w:val", "Normal"}}).leaf("w:next", {{"w:val", "Normal"}});
        s.open("w:pPr").open("w:tabs").leaf("w:tab", {{"w:val", "right"}, {"w:leader", "dot"}, {"w:pos", twips(contentW)}}).close();
        s.leaf("w:spacing", {{"w:after", "60"}}).leaf("w:ind", {{"w:left", twips((i - 1) * 14.0)}}).close();
        if (i == 1) s.open("w:rPr").leaf("w:b").close();
        s.close();
    }
    s.open("w:style").attr("w:type", "character").attr("w:styleId", "Hyperlink");
    s.leaf("w:name", {{"w:val", "Hyperlink"}}).leaf("w:uiPriority", {{"w:val", "99"}}).leaf("w:unhideWhenUsed");
    s.open("w:rPr").leaf("w:color", {{"w:val", "0563C1"}}).leaf("w:u", {{"w:val", "single"}}).close().close();
    for (const char* id : {"TableGrid", "TablePlain"}) {
        const bool grid = std::string(id) == "TableGrid";
        s.open("w:style").attr("w:type", "table").attr("w:styleId", id);
        s.leaf("w:name", {{"w:val", grid ? "Table Grid" : "Plain Table"}}).leaf("w:uiPriority", {{"w:val", "39"}});
        s.open("w:pPr").leaf("w:spacing", {{"w:after", "0"}, {"w:line", "240"}, {"w:lineRule", "auto"}}).close();
        s.open("w:tblPr");
        if (grid) {
            s.open("w:tblBorders");
            for (const char* side : {"w:top", "w:left", "w:bottom", "w:right", "w:insideH", "w:insideV"}) {
                s.leaf(side, {{"w:val", "single"}, {"w:sz", "4"}, {"w:space", "0"}, {"w:color", "999999"}});
            }
            s.close();
        }
        s.open("w:tblCellMar").leaf("w:left", {{"w:w", "108"}, {"w:type", "dxa"}}).leaf("w:right", {{"w:w", "108"}, {"w:type", "dxa"}}).close();
        s.close().close();
    }
    return s.str();
}

std::string numberingXml(const std::vector<DocxWriter::Num>& nums) {
    xml::Writer n;
    n.open("w:numbering").attr("xmlns:w", kW);
    const char* bullets[3] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"};  // • ◦ ▪
    // abstract 0: bullets; one abstract per numbered list so each keeps its own format and start
    auto abstractNum = [&](int id, const std::string& fmt, int start) {
        n.open("w:abstractNum").attr("w:abstractNumId", std::to_string(id)).leaf("w:multiLevelType", {{"w:val", "hybridMultilevel"}});
        for (int l = 0; l < 9; ++l) {
            n.open("w:lvl").attr("w:ilvl", std::to_string(l)).leaf("w:start", {{"w:val", std::to_string(l == 0 ? start : 1)}});
            if (fmt == "bullet") {
                n.leaf("w:numFmt", {{"w:val", "bullet"}}).leaf("w:lvlText", {{"w:val", bullets[l % 3]}});
            } else {
                const std::string f = l == 0 ? fmt : l % 3 == 1 ? "lowerLetter" : l % 3 == 2 ? "lowerRoman" : "decimal";
                const std::string suffix = f == "lowerLetter" && l == 0 ? ")" : ".";
                n.leaf("w:numFmt", {{"w:val", f}}).leaf("w:lvlText", {{"w:val", "%" + std::to_string(l + 1) + suffix}});
            }
            n.leaf("w:lvlJc", {{"w:val", "left"}});
            n.open("w:pPr").leaf("w:ind", {{"w:left", std::to_string(720 * (l + 1))}, {"w:hanging", "360"}}).close();
            n.close();
        }
        n.close();
    };
    abstractNum(0, "bullet", 1);
    for (std::size_t k = 0; k < nums.size(); ++k) abstractNum(static_cast<int>(k) + 1, nums[k].format, nums[k].start);
    n.open("w:num").attr("w:numId", "1").leaf("w:abstractNumId", {{"w:val", "0"}}).close();
    for (std::size_t k = 0; k < nums.size(); ++k) n.open("w:num").attr("w:numId", std::to_string(k + 2)).leaf("w:abstractNumId", {{"w:val", std::to_string(k + 1)}}).close();
    return n.str();
}

std::string relsXml(const std::vector<Rel>& rels) {
    xml::Writer r;
    r.open("Relationships").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
    for (const auto& rel : rels) {
        r.open("Relationship").attr("Id", rel.id).attr("Type", rel.type).attr("Target", rel.target);
        if (rel.external) r.attr("TargetMode", "External");
        r.close();
    }
    return r.str();
}

// A small run writer for notes and comments (no links, changes or comments of their own).
void simpleRuns(xml::Writer& x, const Runs& runs) {
    for (const auto& r : runs) {
        if (r.text.empty()) continue;
        x.open("w:r");
        if (r.bold || r.italic || r.underline || r.strike) {
            x.open("w:rPr");
            if (r.bold) x.leaf("w:b");
            if (r.italic) x.leaf("w:i");
            if (r.strike) x.leaf("w:strike");
            if (r.underline) x.leaf("w:u", {{"w:val", "single"}});
            x.close();
        }
        x.open("w:t").attr("xml:space", "preserve").text(r.text).close().close();
    }
}

}  // namespace

std::string writeDocx(const TextDocument& doc) {
    DocxWriter dw(doc);
    if (std::any_of(doc.blocks.begin(), doc.blocks.end(), [](const Block& b) { return b.kind == Block::Toc; })) dw.tocPages = headingPages(doc);
    std::string documentXml = dw.documentXml();
    std::vector<Rel> rels = {{"rId1", std::string(kRelBase) + "styles", "styles.xml", false},
                             {"rId2", std::string(kRelBase) + "numbering", "numbering.xml", false},
                             {"rId3", std::string(kRelBase) + "settings", "settings.xml", false}};
    for (const auto& r : dw.rels) rels.push_back(r);
    std::vector<raster::ZipEntry> entries;

    xml::Writer ct;
    ct.open("Types").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/content-types");
    ct.leaf("Default", {{"Extension", "rels"}, {"ContentType", "application/vnd.openxmlformats-package.relationships+xml"}});
    ct.leaf("Default", {{"Extension", "xml"}, {"ContentType", "application/xml"}});
    ct.leaf("Default", {{"Extension", "png"}, {"ContentType", "image/png"}});
    ct.leaf("Default", {{"Extension", "jpeg"}, {"ContentType", "image/jpeg"}});
    ct.leaf("Override", {{"PartName", "/word/document.xml"}, {"ContentType", std::string(kCtBase) + "document.main+xml"}});
    ct.leaf("Override", {{"PartName", "/word/styles.xml"}, {"ContentType", std::string(kCtBase) + "styles+xml"}});
    ct.leaf("Override", {{"PartName", "/word/numbering.xml"}, {"ContentType", std::string(kCtBase) + "numbering+xml"}});
    ct.leaf("Override", {{"PartName", "/word/settings.xml"}, {"ContentType", std::string(kCtBase) + "settings+xml"}});
    ct.leaf("Override", {{"PartName", "/docProps/core.xml"}, {"ContentType", "application/vnd.openxmlformats-package.core-properties+xml"}});
    for (const auto& p : dw.headerParts) {
        ct.leaf("Override", {{"PartName", "/word/" + p.file}, {"ContentType", std::string(kCtBase) + p.kind + "+xml"}});
        entries.push_back({"word/" + p.file, p.xml});
    }

    // footnotes, with the two separators Word expects
    if (!dw.footnotes.empty()) {
        xml::Writer f;
        f.open("w:footnotes").attr("xmlns:w", kW).attr("xmlns:r", kR);
        f.open("w:footnote").attr("w:type", "separator").attr("w:id", "-1").open("w:p").open("w:pPr").leaf("w:spacing", {{"w:after", "0"}}).close().open("w:r").leaf("w:separator").close().close().close();
        f.open("w:footnote").attr("w:type", "continuationSeparator").attr("w:id", "0").open("w:p").open("w:pPr").leaf("w:spacing", {{"w:after", "0"}}).close().open("w:r").leaf("w:continuationSeparator").close().close().close();
        for (std::size_t i = 0; i < dw.footnotes.size(); ++i) {
            f.open("w:footnote").attr("w:id", std::to_string(i + 1)).open("w:p");
            f.open("w:pPr").leaf("w:pStyle", {{"w:val", "FootnoteText"}}).close();
            f.open("w:r").open("w:rPr").leaf("w:rStyle", {{"w:val", "FootnoteReference"}}).close().leaf("w:footnoteRef").close();
            f.open("w:r").open("w:t").attr("xml:space", "preserve").text(" ").close().close();
            simpleRuns(f, parseInline(dw.footnotes[i]));
            f.close().close();
        }
        entries.push_back({"word/footnotes.xml", f.str()});
        rels.push_back({"rId4", std::string(kRelBase) + "footnotes", "footnotes.xml", false});
        ct.leaf("Override", {{"PartName", "/word/footnotes.xml"}, {"ContentType", std::string(kCtBase) + "footnotes+xml"}});
    }
    if (!doc.comments.empty()) {
        xml::Writer c;
        c.open("w:comments").attr("xmlns:w", kW);
        for (const auto& cm : doc.comments) {
            std::string initials;
            for (std::size_t i = 0; i < cm.author.size(); ++i) {
                if (std::isupper(static_cast<unsigned char>(cm.author[i])) && (i == 0 || cm.author[i - 1] == ' ')) initials += cm.author[i];
            }
            c.open("w:comment").attr("w:id", std::to_string(cm.id)).attr("w:author", cm.author.empty() ? "wetwaresuite" : cm.author).attr("w:initials", initials.empty() ? "W" : initials);
            if (!cm.date.empty()) c.attr("w:date", cm.date);
            for (const auto& line : splitLines(cm.text.empty() ? std::string(" ") : cm.text)) {
                c.open("w:p").open("w:pPr").leaf("w:pStyle", {{"w:val", "CommentText"}}).close();
                simpleRuns(c, parseInline(line));
                c.close();
            }
            c.close();
        }
        entries.push_back({"word/comments.xml", c.str()});
        rels.push_back({"rId5", std::string(kRelBase) + "comments", "comments.xml", false});
        ct.leaf("Override", {{"PartName", "/word/comments.xml"}, {"ContentType", std::string(kCtBase) + "comments+xml"}});
    }

    xml::Writer settings;
    settings.open("w:settings").attr("xmlns:w", kW);
    if (doc.trackChanges) settings.leaf("w:trackRevisions");
    settings.leaf("w:defaultTabStop", {{"w:val", "720"}});
    if (!dw.footnotes.empty()) settings.open("w:footnotePr").leaf("w:footnote", {{"w:id", "-1"}}).leaf("w:footnote", {{"w:id", "0"}}).close();
    settings.leaf("w:compat");

    xml::Writer root;
    root.open("Relationships").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
    root.leaf("Relationship", {{"Id", "rId1"}, {"Type", std::string(kRelBase) + "officeDocument"}, {"Target", "word/document.xml"}});
    root.leaf("Relationship", {{"Id", "rId2"}, {"Type", "http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties"}, {"Target", "docProps/core.xml"}});

    xml::Writer core;
    core.open("cp:coreProperties")
        .attr("xmlns:cp", "http://schemas.openxmlformats.org/package/2006/metadata/core-properties")
        .attr("xmlns:dc", "http://purl.org/dc/elements/1.1/")
        .attr("xmlns:dcterms", "http://purl.org/dc/terms/")
        .attr("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
    if (!doc.title.empty()) core.element("dc:title", doc.title);
    if (!doc.author.empty()) core.element("dc:creator", doc.author);
    core.open("dcterms:modified").attr("xsi:type", "dcterms:W3CDTF").text(isoNow()).close();

    entries.insert(entries.begin(), {{"[Content_Types].xml", ct.str()},
                                     {"_rels/.rels", root.str()},
                                     {"word/document.xml", documentXml},
                                     {"word/_rels/document.xml.rels", relsXml(rels)},
                                     {"word/styles.xml", stylesXml(doc)},
                                     {"word/numbering.xml", numberingXml(dw.nums)},
                                     {"word/settings.xml", settings.str()},
                                     {"docProps/core.xml", core.str()}});
    for (auto& m : dw.media) entries.push_back(std::move(m));
    return raster::writeZip(entries);
}

}  // namespace wetwaresuite::office
