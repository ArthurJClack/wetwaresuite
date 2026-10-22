// DOCX (read and write) and ODT (read) for the block document model.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "text.hpp"
#include "xml.hpp"

namespace wetwaresuite::office {

namespace {

const char* kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
const char* kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char* kRelImage = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
const char* kRelLink = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";

std::string twips(double pt) { return std::to_string(static_cast<long>(std::lround(pt * 20))); }
std::string emu(double pt) { return std::to_string(static_cast<long long>(std::llround(pt * 12700))); }

bool isJpeg(const std::string& b) { return b.size() > 2 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8; }

// Natural size of an image block in points, fitted to `maxWidth`.
void imageSize(const Block& b, double maxWidth, double& w, double& h) {
    double pw = b.pixels ? b.pixels->pixels.width() : 100, ph = b.pixels ? b.pixels->pixels.height() : 100;
    if (pw <= 0) pw = 1;
    w = b.width > 0 ? b.width : pw * 0.75;
    if (w > maxWidth) w = maxWidth;
    h = w * ph / pw;
}

struct DocxWriter {
    const TextDocument& doc;
    xml::Writer w;
    struct Rel {
        std::string id, type, target;
        bool external = false;
    };
    std::vector<Rel> relList;
    std::vector<raster::ZipEntry> media;
    std::map<std::string, std::string> linkIds;
    int orderedLists = 0;
    int pictures = 0;

    std::string addRel(const std::string& type, const std::string& target, bool external) {
        std::string id = "rId" + std::to_string(relList.size() + 10);
        relList.push_back({id, type, target, external});
        return id;
    }

    void textRun(const Run& r, bool forceBold = false) {
        // split on line breaks and tabs
        std::string cur;
        auto open = [&] {
            w.open("w:r");
            if (r.bold || forceBold || r.italic || r.underline || r.code || !r.link.empty()) {
                w.open("w:rPr");
                if (!r.link.empty()) w.leaf("w:rStyle", {{"w:val", "Hyperlink"}});
                if (r.code) w.leaf("w:rFonts", {{"w:ascii", "Courier New"}, {"w:hAnsi", "Courier New"}, {"w:cs", "Courier New"}});
                if (r.bold || forceBold) w.leaf("w:b");
                if (r.italic) w.leaf("w:i");
                if (r.underline && r.link.empty()) w.leaf("w:u", {{"w:val", "single"}});
                w.close();
            }
        };
        open();
        auto flushText = [&] {
            if (cur.empty()) return;
            w.open("w:t").attr("xml:space", "preserve").text(cur).close();
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

    void runs(const Runs& rs, bool forceBold = false) {
        for (std::size_t i = 0; i < rs.size(); ++i) {
            const Run& r = rs[i];
            if (r.link.empty()) {
                textRun(r, forceBold);
                continue;
            }
            auto it = linkIds.find(r.link);
            std::string id = it != linkIds.end() ? it->second : (linkIds[r.link] = addRel(kRelLink, r.link, true));
            w.open("w:hyperlink").attr("r:id", id).attr("w:history", "1");
            // consecutive runs with the same link share the hyperlink
            std::size_t j = i;
            while (j < rs.size() && rs[j].link == r.link) textRun(rs[j++], forceBold);
            w.close();
            i = j - 1;
        }
    }

    void paragraph(const std::string& style, const Runs& rs, int numId = 0, int ilvl = 0) {
        w.open("w:p");
        if (!style.empty() || numId) {
            w.open("w:pPr");
            if (!style.empty()) w.leaf("w:pStyle", {{"w:val", style}});
            if (numId) {
                w.open("w:numPr").leaf("w:ilvl", {{"w:val", std::to_string(ilvl)}}).leaf("w:numId", {{"w:val", std::to_string(numId)}}).close();
            }
            w.close();
        }
        runs(rs);
        w.close();
    }

    void image(const Block& b) {
        const double contentW = doc.page.width - 2 * doc.page.margin;
        double wpt, hpt;
        imageSize(b, contentW, wpt, hpt);
        ++pictures;
        std::string name = "image" + std::to_string(pictures) + (isJpeg(b.image) ? ".jpeg" : ".png");
        media.push_back({"word/media/" + name, b.image});
        std::string id = addRel(kRelImage, "media/" + name, false);
        const std::string cx = emu(wpt), cy = emu(hpt);
        w.open("w:p").open("w:r").open("w:drawing");
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
        w.close();  // spPr
        w.close().close().close();  // pic, graphicData, graphic
        w.close().close().close().close();  // inline, drawing, r, p
    }

    void table(const Block& b) {
        std::size_t cols = 0;
        for (const auto& r : b.rows) cols = std::max(cols, r.size());
        if (!cols) return;
        const double contentW = doc.page.width - 2 * doc.page.margin;
        const std::string colW = twips(contentW / static_cast<double>(cols));
        w.open("w:tbl");
        w.open("w:tblPr").leaf("w:tblStyle", {{"w:val", "TableGrid"}}).leaf("w:tblW", {{"w:w", "0"}, {"w:type", "auto"}}).leaf("w:tblLook", {{"w:val", "04A0"}, {"w:firstRow", "1"}, {"w:lastRow", "0"}, {"w:firstColumn", "0"}, {"w:lastColumn", "0"}, {"w:noHBand", "0"}, {"w:noVBand", "1"}}).close();
        w.open("w:tblGrid");
        for (std::size_t c = 0; c < cols; ++c) w.leaf("w:gridCol", {{"w:w", colW}});
        w.close();
        for (std::size_t r = 0; r < b.rows.size(); ++r) {
            const bool head = r == 0 && b.header;
            w.open("w:tr");
            if (head) w.open("w:trPr").leaf("w:tblHeader").close();
            for (std::size_t c = 0; c < cols; ++c) {
                w.open("w:tc").open("w:tcPr").leaf("w:tcW", {{"w:w", colW}, {"w:type", "dxa"}}).close();
                w.open("w:p");
                if (c < b.rows[r].size()) runs(b.rows[r][c], head);
                w.close();
                w.close();
            }
            w.close();
        }
        w.close();
    }

    std::string documentXml() {
        w.open("w:document").attr("xmlns:w", kW).attr("xmlns:r", kR).attr("xmlns:wp", "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing");
        w.open("w:body");
        for (std::size_t i = 0; i < doc.blocks.size(); ++i) {
            const Block& b = doc.blocks[i];
            switch (b.kind) {
                case Block::Heading: paragraph("Heading" + std::to_string(std::clamp(b.level, 1, 6)), b.runs); break;
                case Block::Paragraph: paragraph("", b.runs); break;
                case Block::Quote: paragraph("Quote", b.runs); break;
                case Block::Code: paragraph("Code", b.runs); break;
                case Block::PageBreak: w.open("w:p").open("w:r").leaf("w:br", {{"w:type", "page"}}).close().close(); break;
                case Block::List: {
                    int numId = b.ordered ? 2 + orderedLists++ : 1;
                    for (const auto& it : b.items) paragraph("ListParagraph", it.runs, numId, std::clamp(it.level, 0, 8));
                    break;
                }
                case Block::Table:
                    table(b);
                    if (i + 1 == doc.blocks.size()) w.leaf("w:p");
                    break;
                case Block::Image: image(b); break;
            }
        }
        w.open("w:sectPr");
        w.leaf("w:pgSz", {{"w:w", twips(doc.page.width)}, {"w:h", twips(doc.page.height)}});
        w.leaf("w:pgMar", {{"w:top", twips(doc.page.margin)},
                           {"w:right", twips(doc.page.margin)},
                           {"w:bottom", twips(doc.page.margin)},
                           {"w:left", twips(doc.page.margin)},
                           {"w:header", "708"},
                           {"w:footer", "708"},
                           {"w:gutter", "0"}});
        w.close();
        w.close().close();
        return w.str();
    }
};

std::string stylesXml(const TextDocument& doc) {
    xml::Writer s;
    const std::string font = doc.page.font == pdf::Family::Serif ? "Times New Roman" : doc.page.font == pdf::Family::Mono ? "Courier New" : "Calibri";
    const std::string size = std::to_string(static_cast<int>(std::lround(doc.page.fontSize * 2)));
    s.open("w:styles").attr("xmlns:w", kW);
    s.open("w:docDefaults");
    s.open("w:rPrDefault").open("w:rPr").leaf("w:rFonts", {{"w:ascii", font}, {"w:hAnsi", font}, {"w:eastAsia", font}, {"w:cs", font}});
    s.leaf("w:sz", {{"w:val", size}}).leaf("w:szCs", {{"w:val", size}}).leaf("w:lang", {{"w:val", "en-US"}}).close().close();
    s.open("w:pPrDefault").open("w:pPr").leaf("w:spacing", {{"w:after", "160"}, {"w:line", "264"}, {"w:lineRule", "auto"}}).close().close();
    s.close();
    s.open("w:style").attr("w:type", "paragraph").attr("w:default", "1").attr("w:styleId", "Normal").leaf("w:name", {{"w:val", "Normal"}}).leaf("w:qFormat").close();
    const int sizes[6] = {36, 30, 26, 24, 22, 22};
    for (int i = 1; i <= 6; ++i) {
        s.open("w:style").attr("w:type", "paragraph").attr("w:styleId", "Heading" + std::to_string(i));
        s.leaf("w:name", {{"w:val", "heading " + std::to_string(i)}}).leaf("w:basedOn", {{"w:val", "Normal"}}).leaf("w:next", {{"w:val", "Normal"}});
        s.leaf("w:uiPriority", {{"w:val", "9"}}).leaf("w:qFormat");
        s.open("w:pPr").leaf("w:keepNext").leaf("w:keepLines").leaf("w:spacing", {{"w:before", i <= 2 ? "360" : "240"}, {"w:after", "120"}});
        s.leaf("w:outlineLvl", {{"w:val", std::to_string(i - 1)}}).close();
        s.open("w:rPr").leaf("w:b").leaf("w:bCs");
        if (i >= 5) s.leaf("w:i");
        s.leaf("w:sz", {{"w:val", std::to_string(sizes[i - 1])}}).leaf("w:szCs", {{"w:val", std::to_string(sizes[i - 1])}}).close();
        s.close();
    }
    s.open("w:style").attr("w:type", "paragraph").attr("w:styleId", "ListParagraph");
    s.leaf("w:name", {{"w:val", "List Paragraph"}}).leaf("w:basedOn", {{"w:val", "Normal"}}).leaf("w:uiPriority", {{"w:val", "34"}}).leaf("w:qFormat");
    s.open("w:pPr").leaf("w:spacing", {{"w:after", "60"}}).leaf("w:ind", {{"w:left", "720"}}).leaf("w:contextualSpacing").close().close();
    s.open("w:style").attr("w:type", "paragraph").attr("w:styleId", "Quote");
    s.leaf("w:name", {{"w:val", "Quote"}}).leaf("w:basedOn", {{"w:val", "Normal"}}).leaf("w:next", {{"w:val", "Normal"}}).leaf("w:qFormat");
    s.open("w:pPr").leaf("w:ind", {{"w:left", "720"}, {"w:right", "720"}}).close();
    s.open("w:rPr").leaf("w:i").leaf("w:color", {{"w:val", "555555"}}).close().close();
    s.open("w:style").attr("w:type", "paragraph").attr("w:styleId", "Code");
    s.leaf("w:name", {{"w:val", "Code"}}).leaf("w:basedOn", {{"w:val", "Normal"}}).leaf("w:qFormat");
    s.open("w:pPr").leaf("w:shd", {{"w:val", "clear"}, {"w:color", "auto"}, {"w:fill", "F2F2F2"}}).leaf("w:spacing", {{"w:after", "160"}, {"w:line", "240"}, {"w:lineRule", "auto"}}).close();
    s.open("w:rPr").leaf("w:rFonts", {{"w:ascii", "Courier New"}, {"w:hAnsi", "Courier New"}, {"w:cs", "Courier New"}}).leaf("w:sz", {{"w:val", "19"}}).close().close();
    s.open("w:style").attr("w:type", "character").attr("w:styleId", "Hyperlink");
    s.leaf("w:name", {{"w:val", "Hyperlink"}}).leaf("w:uiPriority", {{"w:val", "99"}}).leaf("w:unhideWhenUsed");
    s.open("w:rPr").leaf("w:color", {{"w:val", "0563C1"}}).leaf("w:u", {{"w:val", "single"}}).close().close();
    s.open("w:style").attr("w:type", "table").attr("w:styleId", "TableGrid");
    s.leaf("w:name", {{"w:val", "Table Grid"}}).leaf("w:uiPriority", {{"w:val", "39"}});
    s.open("w:pPr").leaf("w:spacing", {{"w:after", "0"}, {"w:line", "240"}, {"w:lineRule", "auto"}}).close();
    s.open("w:tblPr").open("w:tblBorders");
    for (const char* side : {"w:top", "w:left", "w:bottom", "w:right", "w:insideH", "w:insideV"}) {
        s.leaf(side, {{"w:val", "single"}, {"w:sz", "4"}, {"w:space", "0"}, {"w:color", "999999"}});
    }
    s.close();
    s.open("w:tblCellMar").leaf("w:left", {{"w:w", "108"}, {"w:type", "dxa"}}).leaf("w:right", {{"w:w", "108"}, {"w:type", "dxa"}}).close();
    s.close().close();
    return s.str();
}

std::string numberingXml(int orderedLists) {
    xml::Writer n;
    n.open("w:numbering").attr("xmlns:w", kW);
    const char* bullets[3] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"};  // • ◦ ▪
    const char* fmts[3] = {"decimal", "lowerLetter", "lowerRoman"};
    for (int a = 0; a < 2; ++a) {
        n.open("w:abstractNum").attr("w:abstractNumId", std::to_string(a)).leaf("w:multiLevelType", {{"w:val", "hybridMultilevel"}});
        for (int l = 0; l < 9; ++l) {
            n.open("w:lvl").attr("w:ilvl", std::to_string(l)).leaf("w:start", {{"w:val", "1"}});
            if (a == 0) n.leaf("w:numFmt", {{"w:val", "bullet"}}).leaf("w:lvlText", {{"w:val", bullets[l % 3]}});
            else n.leaf("w:numFmt", {{"w:val", fmts[l % 3]}}).leaf("w:lvlText", {{"w:val", "%" + std::to_string(l + 1) + "."}});
            n.leaf("w:lvlJc", {{"w:val", "left"}});
            n.open("w:pPr").leaf("w:ind", {{"w:left", std::to_string(720 * (l + 1))}, {"w:hanging", "360"}}).close();
            n.close();
        }
        n.close();
    }
    n.open("w:num").attr("w:numId", "1").leaf("w:abstractNumId", {{"w:val", "0"}}).close();
    for (int k = 0; k < orderedLists; ++k) {
        n.open("w:num").attr("w:numId", std::to_string(k + 2)).leaf("w:abstractNumId", {{"w:val", "1"}});
        n.open("w:lvlOverride").attr("w:ilvl", "0").leaf("w:startOverride", {{"w:val", "1"}}).close();
        n.close();
    }
    return n.str();
}

std::string relsXml(const std::vector<DocxWriter::Rel>& rels) {
    xml::Writer r;
    r.open("Relationships").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
    for (const auto& rel : rels) {
        r.open("Relationship").attr("Id", rel.id).attr("Type", rel.type).attr("Target", rel.target);
        if (rel.external) r.attr("TargetMode", "External");
        r.close();
    }
    return r.str();
}

}  // namespace

std::string writeDocx(const TextDocument& doc) {
    DocxWriter dw{doc, {}, {}, {}, {}, 0, 0};
    std::string documentXml = dw.documentXml();
    std::vector<DocxWriter::Rel> rels = {{"rId1", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles", "styles.xml", false},
                                         {"rId2", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering", "numbering.xml", false}};
    for (const auto& r : dw.relList) rels.push_back(r);

    xml::Writer ct;
    ct.open("Types").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/content-types");
    ct.leaf("Default", {{"Extension", "rels"}, {"ContentType", "application/vnd.openxmlformats-package.relationships+xml"}});
    ct.leaf("Default", {{"Extension", "xml"}, {"ContentType", "application/xml"}});
    ct.leaf("Default", {{"Extension", "png"}, {"ContentType", "image/png"}});
    ct.leaf("Default", {{"Extension", "jpeg"}, {"ContentType", "image/jpeg"}});
    ct.leaf("Override", {{"PartName", "/word/document.xml"}, {"ContentType", "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"}});
    ct.leaf("Override", {{"PartName", "/word/styles.xml"}, {"ContentType", "application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"}});
    ct.leaf("Override", {{"PartName", "/word/numbering.xml"}, {"ContentType", "application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"}});
    ct.leaf("Override", {{"PartName", "/docProps/core.xml"}, {"ContentType", "application/vnd.openxmlformats-package.core-properties+xml"}});

    xml::Writer root;
    root.open("Relationships").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
    root.leaf("Relationship", {{"Id", "rId1"}, {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"}, {"Target", "word/document.xml"}});
    root.leaf("Relationship", {{"Id", "rId2"}, {"Type", "http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties"}, {"Target", "docProps/core.xml"}});

    xml::Writer core;
    core.open("cp:coreProperties")
        .attr("xmlns:cp", "http://schemas.openxmlformats.org/package/2006/metadata/core-properties")
        .attr("xmlns:dc", "http://purl.org/dc/elements/1.1/")
        .attr("xmlns:dcterms", "http://purl.org/dc/terms/")
        .attr("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
    if (!doc.title.empty()) core.element("dc:title", doc.title);
    if (!doc.author.empty()) core.element("dc:creator", doc.author);

    std::vector<raster::ZipEntry> entries = {
        {"[Content_Types].xml", ct.str()},
        {"_rels/.rels", root.str()},
        {"word/document.xml", documentXml},
        {"word/_rels/document.xml.rels", relsXml(rels)},
        {"word/styles.xml", stylesXml(doc)},
        {"word/numbering.xml", numberingXml(dw.orderedLists)},
        {"docProps/core.xml", core.str()},
    };
    for (auto& m : dw.media) entries.push_back(std::move(m));
    return raster::writeZip(entries);
}

// ---- reading DOCX -----------------------------------------------------------------

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
    return v.empty() || (v != "0" && v != "false" && v != "none");
}

struct DocxReader {
    std::map<std::string, std::string> files;
    std::map<std::string, std::string> rels;      // id -> target (resolved under word/)
    std::map<std::string, std::string> styleName;  // style id -> lower-case name
    std::map<std::string, std::string> styleBase;  // style id -> basedOn id
    std::map<std::string, int> styleOutline;
    std::map<std::string, std::map<int, bool>> numOrdered;  // numId -> level -> ordered
    TextDocument doc;

    void loadStyles() {
        std::string s = entry(files, "word/styles.xml");
        if (s.empty()) return;
        xml::Node root = xml::parse(s);
        for (const auto* st : root.all("style")) {
            std::string id = st->attr("w:styleId");
            if (const auto* n = st->child("name")) styleName[id] = lower(n->attr("w:val"));
            if (const auto* b = st->child("basedOn")) styleBase[id] = b->attr("w:val");
            if (const auto* p = st->child("pPr")) {
                if (const auto* o = p->child("outlineLvl")) styleOutline[id] = std::atoi(o->attr("w:val").c_str());
            }
        }
    }

    void loadNumbering() {
        std::string s = entry(files, "word/numbering.xml");
        if (s.empty()) return;
        xml::Node root = xml::parse(s);
        std::map<std::string, std::map<int, bool>> abstract;
        for (const auto* a : root.all("abstractNum")) {
            auto& levels = abstract[a->attr("w:abstractNumId")];
            for (const auto* l : a->all("lvl")) {
                const auto* f = l->child("numFmt");
                std::string fmt = f ? f->attr("w:val") : "bullet";
                levels[std::atoi(l->attr("w:ilvl").c_str())] = fmt != "bullet" && fmt != "none";
            }
        }
        for (const auto* n : root.all("num")) {
            const auto* a = n->child("abstractNumId");
            if (a) numOrdered[n->attr("w:numId")] = abstract[a->attr("w:val")];
        }
    }

    // Heading level (1..6) of a paragraph style, 0 if not a heading; follows basedOn.
    int headingLevel(const std::string& styleId) {
        std::string id = styleId;
        for (int hops = 0; hops < 8 && !id.empty(); ++hops) {
            const std::string name = styleName.count(id) ? styleName[id] : lower(id);
            if (name == "title") return 1;
            if (name == "subtitle") return 2;
            if (startsWith(name, "heading ") || startsWith(name, "heading")) {
                int n = std::atoi(name.c_str() + (name[7] == ' ' ? 8 : 7));
                if (n >= 1 && n <= 9) return std::min(n, 6);
            }
            if (styleOutline.count(id)) return std::min(styleOutline[id] + 1, 6);
            id = styleBase.count(id) ? styleBase[id] : "";
        }
        return 0;
    }

    std::string styleKind(const std::string& styleId) {
        std::string id = styleId;
        for (int hops = 0; hops < 8 && !id.empty(); ++hops) {
            const std::string name = styleName.count(id) ? styleName[id] : lower(id);
            if (name.find("quote") != std::string::npos) return "quote";
            if (name.find("code") != std::string::npos || name.find("source") != std::string::npos || name.find("preformatted") != std::string::npos ||
                name == "html preformatted") {
                return "code";
            }
            id = styleBase.count(id) ? styleBase[id] : "";
        }
        return "";
    }

    struct Para {
        Runs runs;
        std::vector<Block> images;  // images found in the paragraph
        bool pageBreakAfter = false;
        bool pageBreakBefore = false;
    };

    void collectRuns(const xml::Node& n, Para& p, const std::string& link) {
        for (const auto& c : n.children) {
            if (c.is("r")) {
                Run base;
                base.link = link;
                if (const auto* rp = c.child("rPr")) {
                    base.bold = onFlag(rp->child("b"));
                    base.italic = onFlag(rp->child("i"));
                    base.underline = onFlag(rp->child("u")) && link.empty();
                    if (const auto* f = rp->child("rFonts")) {
                        std::string fn = lower(f->attr("w:ascii"));
                        base.code = fn.find("courier") != std::string::npos || fn.find("consol") != std::string::npos || fn.find("mono") != std::string::npos;
                    }
                    if (const auto* rs = rp->child("rStyle")) {
                        std::string sv = lower(rs->attr("w:val"));
                        if (sv.find("code") != std::string::npos || sv.find("verbatim") != std::string::npos) base.code = true;
                        if (sv == "strong") base.bold = true;
                        if (sv == "emphasis") base.italic = true;
                    }
                }
                for (const auto& part : c.children) {
                    Run r = base;
                    if (part.is("t")) r.text = part.allText();
                    else if (part.is("tab")) r.text = "\t";
                    else if (part.is("br") || part.is("cr")) {
                        if (part.attr("w:type") == "page") {
                            p.pageBreakAfter = true;
                            continue;
                        }
                        r.text = "\n";
                    } else if (part.is("noBreakHyphen")) r.text = "-";
                    else if (part.is("sym")) r.text = "?";
                    else if (part.is("drawing") || part.is("pict") || part.is("object")) {
                        std::vector<const xml::Node*> blips;
                        part.descendants("blip", blips);
                        std::vector<const xml::Node*> imagedata;
                        part.descendants("imagedata", imagedata);
                        std::string rid = !blips.empty() ? blips[0]->attr("r:embed") : !imagedata.empty() ? imagedata[0]->attr("r:id") : "";
                        auto it = rels.find(rid);
                        if (it != rels.end()) {
                            std::string bytes = entry(files, it->second);
                            if (!bytes.empty()) {
                                Block b;
                                try {
                                    setImage(b, bytes);
                                    if (const auto* ext = part.find("extent")) b.width = std::atof(ext->attr("cx").c_str()) / 12700.0;
                                    if (const auto* dp = part.find("docPr")) b.alt = dp->attr("descr");
                                    p.images.push_back(std::move(b));
                                } catch (const std::exception&) {
                                }
                            }
                        }
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
                collectRuns(c, p, target);
            } else if (c.is("ins") || c.is("smartTag") || c.is("customXml") || c.is("fldSimple") || c.is("sdt") || c.is("sdtContent") || c.is("bdo") || c.is("dir")) {
                collectRuns(c, p, link);
            }
        }
    }

    void paragraph(const xml::Node& p, std::vector<Block>& out) {
        Para para;
        std::string style;
        std::string numId;
        int ilvl = 0;
        if (const auto* pp = p.child("pPr")) {
            if (const auto* ps = pp->child("pStyle")) style = ps->attr("w:val");
            if (const auto* np = pp->child("numPr")) {
                if (const auto* id = np->child("numId")) numId = id->attr("w:val");
                if (const auto* lv = np->child("ilvl")) ilvl = std::atoi(lv->attr("w:val").c_str());
            }
            para.pageBreakBefore = onFlag(pp->child("pageBreakBefore"));
        }
        collectRuns(p, para, "");
        mergeRuns(para.runs);
        if (para.pageBreakBefore) {
            Block pb;
            pb.kind = Block::PageBreak;
            out.push_back(pb);
        }
        const bool hasText = !trimCopy(plainText(para.runs)).empty();
        if (hasText) {
            int h = headingLevel(style);
            if (!numId.empty() && numId != "0" && !h) {
                const bool ordered = numOrdered.count(numId) && numOrdered[numId].count(ilvl) ? numOrdered[numId][ilvl] : false;
                if (out.empty() || out.back().kind != Block::List || out.back().ordered != ordered) {
                    Block b;
                    b.kind = Block::List;
                    b.ordered = ordered;
                    out.push_back(b);
                }
                ListItem it;
                it.level = std::clamp(ilvl, 0, 8);
                it.runs = para.runs;
                out.back().items.push_back(it);
            } else {
                Block b;
                if (h) {
                    b.kind = Block::Heading;
                    b.level = h;
                } else {
                    std::string k = styleKind(style);
                    b.kind = k == "quote" ? Block::Quote : k == "code" ? Block::Code : Block::Paragraph;
                }
                b.runs = para.runs;
                if (b.kind == Block::Code) {
                    Run r;
                    r.text = plainText(b.runs);
                    r.code = true;
                    b.runs = {r};
                    if (!out.empty() && out.back().kind == Block::Code) {  // one paragraph per line in many files
                        out.back().runs[0].text += "\n" + r.text;
                        b.kind = Block::PageBreak;  // marker: nothing to add
                    }
                }
                if (b.kind != Block::PageBreak) out.push_back(std::move(b));
            }
        }
        for (auto& img : para.images) out.push_back(std::move(img));
        if (para.pageBreakAfter) {
            Block pb;
            pb.kind = Block::PageBreak;
            out.push_back(pb);
        }
    }

    Runs cellRuns(const xml::Node& tc) {
        Runs all;
        bool first = true;
        std::vector<const xml::Node*> ps;
        tc.descendants("p", ps);
        for (const auto* p : ps) {
            Para para;
            collectRuns(*p, para, "");
            if (!first && !para.runs.empty()) {
                Run nl;
                nl.text = "\n";
                all.push_back(nl);
            }
            for (auto& r : para.runs) all.push_back(r);
            if (!para.runs.empty()) first = false;
        }
        mergeRuns(all);
        return all;
    }

    void body(const xml::Node& n, std::vector<Block>& out) {
        for (const auto& c : n.children) {
            if (c.is("p")) paragraph(c, out);
            else if (c.is("tbl")) {
                Block b;
                b.kind = Block::Table;
                b.header = true;  // the first row reads as a header, as most tables are written
                for (const auto* tr : c.all("tr")) {
                    std::vector<Runs> row;
                    for (const auto& cell : tr->children) {
                        if (cell.is("tc")) {
                            int span = 1;
                            if (const auto* pr = cell.child("tcPr")) {
                                if (const auto* gs = pr->child("gridSpan")) span = std::max(1, std::atoi(gs->attr("w:val").c_str()));
                            }
                            row.push_back(cellRuns(cell));
                            for (int k = 1; k < span; ++k) row.emplace_back();
                        } else if (cell.is("sdt")) {
                            for (const auto* tc : cell.all("sdtContent")) {
                                for (const auto* x : tc->all("tc")) row.push_back(cellRuns(*x));
                            }
                        }
                    }
                    b.rows.push_back(row);
                }
                std::size_t cols = 0;
                for (const auto& r : b.rows) cols = std::max(cols, r.size());
                for (auto& r : b.rows) r.resize(cols);
                if (!b.rows.empty()) out.push_back(std::move(b));
            } else if (c.is("sdt")) {
                if (const auto* sc = c.child("sdtContent")) body(*sc, out);
            } else if (c.is("customXml") || c.is("ins")) {
                body(c, out);
            }
        }
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
    r.loadStyles();
    r.loadNumbering();
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
            double left = std::atof(m->attr("w:left").c_str()) / 20;
            if (left > 0 && left < 200) r.doc.page.margin = left;
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

// ---- reading ODT ------------------------------------------------------------------

namespace {

struct OdtReader {
    std::map<std::string, std::string> files;
    struct TextStyle {
        bool bold = false, italic = false, underline = false, code = false;
        bool pageBreakBefore = false;
        std::string parent;
    };
    std::map<std::string, TextStyle> styles;
    std::map<std::string, bool> listOrdered;  // list style -> first level is numbered
    std::map<std::string, std::string> parentOf;
    TextDocument doc;

    void loadStyles(const xml::Node& container) {
        for (const auto* s : container.all("style")) {
            TextStyle t;
            t.parent = s->attr("style:parent-style-name");
            if (const auto* tp = s->child("text-properties")) {
                t.bold = tp->attr("fo:font-weight") == "bold";
                t.italic = tp->attr("fo:font-style") == "italic";
                std::string u = tp->attr("style:text-underline-style");
                t.underline = !u.empty() && u != "none";
                std::string fn = lower(tp->attr("style:font-name"));
                t.code = fn.find("mono") != std::string::npos || fn.find("courier") != std::string::npos;
            }
            if (const auto* pp = s->child("paragraph-properties")) t.pageBreakBefore = pp->attr("fo:break-before") == "page";
            styles[s->attr("style:name")] = t;
        }
        for (const auto* ls : container.all("list-style")) {
            bool ordered = false;
            for (const auto& c : ls->children) {
                if (c.is("list-level-style-number") || c.is("list-level-style-bullet")) {
                    ordered = c.is("list-level-style-number");
                    break;
                }
            }
            listOrdered[ls->attr("style:name")] = ordered;
        }
    }

    std::string styleChainName(const std::string& name) {
        // display names like "Heading_20_1", "Quotations", "Preformatted_20_Text"
        std::string n = name;
        for (int hops = 0; hops < 8 && !n.empty(); ++hops) {
            std::string l = lower(n);
            if (startsWith(l, "heading") || l == "title" || l.find("quotation") != std::string::npos || l.find("preformatted") != std::string::npos ||
                l.find("source") != std::string::npos) {
                return l;
            }
            n = styles.count(n) ? styles[n].parent : "";
        }
        return "";
    }

    void spans(const xml::Node& n, Runs& out, Run base) {
        for (const auto& c : n.children) {
            if (c.isText()) {
                Run r = base;
                r.text = c.text;
                out.push_back(r);
            } else if (c.is("s")) {
                Run r = base;
                r.text = std::string(static_cast<std::size_t>(std::max(1, std::atoi(c.attr("text:c").c_str()))), ' ');
                out.push_back(r);
            } else if (c.is("tab")) {
                Run r = base;
                r.text = "\t";
                out.push_back(r);
            } else if (c.is("line-break")) {
                Run r = base;
                r.text = "\n";
                out.push_back(r);
            } else if (c.is("span")) {
                Run r = base;
                auto it = styles.find(c.attr("text:style-name"));
                if (it != styles.end()) {
                    r.bold = r.bold || it->second.bold;
                    r.italic = r.italic || it->second.italic;
                    r.underline = r.underline || it->second.underline;
                    r.code = r.code || it->second.code;
                }
                spans(c, out, r);
            } else if (c.is("a")) {
                Run r = base;
                r.link = c.attr("xlink:href");
                spans(c, out, r);
            } else if (c.is("note") || c.is("annotation") || c.is("bookmark") || c.is("bookmark-start") || c.is("frame")) {
                continue;
            } else {
                spans(c, out, base);
            }
        }
    }

    void frames(const xml::Node& n, std::vector<Block>& out) {
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
            std::string w = f->attr("svg:width");
            double v = std::atof(w.c_str());
            if (endsWith(w, "cm")) b.width = v / 2.54 * 72;
            else if (endsWith(w, "mm")) b.width = v / 25.4 * 72;
            else if (endsWith(w, "in")) b.width = v * 72;
            else if (endsWith(w, "pt")) b.width = v;
            if (const auto* d = f->child("desc")) b.alt = d->allText();
            else b.alt = f->attr("draw:name");
            out.push_back(std::move(b));
        }
    }

    Block textBlock(const xml::Node& p, bool heading) {
        Block b;
        std::string style = p.attr("text:style-name");
        Run base;
        auto it = styles.find(style);
        if (it != styles.end()) {
            base.bold = it->second.bold;
            base.italic = it->second.italic;
        }
        spans(p, b.runs, base);
        mergeRuns(b.runs);
        if (heading) {
            b.kind = Block::Heading;
            b.level = std::clamp(std::atoi(p.attr("text:outline-level").c_str()), 1, 6);
            for (auto& r : b.runs) r.bold = false;
        } else {
            std::string chain = styleChainName(style);
            if (startsWith(chain, "heading")) {
                b.kind = Block::Heading;
                const auto digit = chain.find_first_of("0123456789");
                const int n = digit == std::string::npos ? 1 : std::atoi(chain.c_str() + digit);
                b.level = std::clamp(n, 1, 6);
            } else if (chain == "title") {
                b.kind = Block::Heading;
                b.level = 1;
            } else if (chain.find("quotation") != std::string::npos) {
                b.kind = Block::Quote;
            } else if (chain.find("preformatted") != std::string::npos || chain.find("source") != std::string::npos) {
                b.kind = Block::Code;
            }
        }
        return b;
    }

    void list(const xml::Node& l, int level, Block& block) {
        for (const auto* item : l.all("list-item")) {
            for (const auto& c : item->children) {
                if (c.is("p") || c.is("h")) {
                    ListItem it;
                    it.level = level;
                    spans(c, it.runs, Run{});
                    mergeRuns(it.runs);
                    if (!trimCopy(plainText(it.runs)).empty()) block.items.push_back(it);
                } else if (c.is("list")) {
                    list(c, level + 1, block);
                }
            }
        }
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
                if (!trimCopy(plainText(b.runs)).empty()) {
                    if (b.kind == Block::Code) {
                        Run r;
                        r.text = plainText(b.runs);
                        r.code = true;
                        b.runs = {r};
                        if (!out.empty() && out.back().kind == Block::Code) {
                            out.back().runs[0].text += "\n" + r.text;
                        } else {
                            out.push_back(std::move(b));
                        }
                    } else {
                        out.push_back(std::move(b));
                    }
                }
                frames(c, out);
            } else if (c.is("list")) {
                Block b;
                b.kind = Block::List;
                std::string ls = c.attr("text:style-name");
                b.ordered = listOrdered.count(ls) ? listOrdered[ls] : false;
                list(c, 0, b);
                if (!b.items.empty()) out.push_back(std::move(b));
            } else if (c.is("table")) {
                Block b;
                b.kind = Block::Table;
                std::vector<const xml::Node*> rows;
                std::function<void(const xml::Node&)> collect = [&](const xml::Node& x) {
                    for (const auto& ch : x.children) {
                        if (ch.is("table-row")) rows.push_back(&ch);
                        else if (ch.is("table-header-rows") || ch.is("table-rows") || ch.is("table-row-group")) collect(ch);
                    }
                };
                collect(c);
                for (const auto* row : rows) {
                    std::vector<Runs> cells;
                    for (const auto& cell : row->children) {
                        if (!cell.is("table-cell") && !cell.is("covered-table-cell")) continue;
                        Runs rs;
                        bool first = true;
                        for (const auto* p : cell.all("p")) {
                            if (!first) {
                                Run nl;
                                nl.text = "\n";
                                rs.push_back(nl);
                            }
                            spans(*p, rs, Run{});
                            first = false;
                        }
                        mergeRuns(rs);
                        int rep = std::max(1, std::atoi(cell.attr("table:number-columns-repeated").c_str()));
                        for (int k = 0; k < rep && k < 64; ++k) cells.push_back(rs);
                    }
                    b.rows.push_back(cells);
                }
                std::size_t cols = 0;
                for (const auto& r : b.rows) cols = std::max(cols, r.size());
                for (auto& r : b.rows) r.resize(cols);
                if (!b.rows.empty()) out.push_back(std::move(b));
            } else if (c.is("section") || c.is("index-body") || c.is("table-of-content")) {
                body(c, out);
            }
        }
    }
};

}  // namespace

TextDocument readOdt(const std::string& bytes) {
    OdtReader r;
    r.files = unzip(bytes);
    std::string content = entry(r.files, "content.xml");
    if (content.empty()) throw std::runtime_error("not an OpenDocument file (no content.xml)");
    if (std::string st = entry(r.files, "styles.xml"); !st.empty()) {
        xml::Node s = xml::parse(st);
        if (const auto* os = s.child("styles")) r.loadStyles(*os);
    }
    xml::Node root = xml::parse(content);
    if (const auto* as = root.child("automatic-styles")) r.loadStyles(*as);
    const xml::Node* bodyNode = root.child("body");
    const xml::Node* text = bodyNode ? bodyNode->child("text") : nullptr;
    if (!text) throw std::runtime_error("not a text document (no office:text)");
    r.body(*text, r.doc.blocks);
    if (std::string meta = entry(r.files, "meta.xml"); !meta.empty()) {
        try {
            xml::Node m = xml::parse(meta);
            if (const auto* t = m.find("title")) r.doc.title = t->allText();
            if (const auto* a = m.find("initial-creator")) r.doc.author = a->allText();
        } catch (const std::exception&) {
        }
    }
    return r.doc;
}

}  // namespace wetwaresuite::office
