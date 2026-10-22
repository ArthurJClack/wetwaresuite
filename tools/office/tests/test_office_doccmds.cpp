// The Word-style document model and office.doc_* commands: formatting, styles,
// lists, tables with merges, headers and footers, the table of contents,
// footnotes, comments, tracked changes, and their DOCX / ODT round trips.
// Headless; needs no LibreOffice.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/office/office.hpp"
#include "wetwaresuite/pdf/reader.hpp"
#include "check.hpp"
#include "commands.hpp"
#include "text.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::office;
namespace fs = std::filesystem;

namespace {

fs::path tmpDir() {
    static const fs::path dir = fs::temp_directory_path() / ("wetwaresuite_doccmd_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    return dir;
}

Params P(std::initializer_list<std::pair<const std::string, Value>> l) { return Params(l); }
Value S(const char* s) { return std::string(s); }
Value I(std::int64_t v) { return v; }

TextDocument& docOf(Session& s, const char* id = "doc1") { return s.workspace().getAs<TextDoc>(id)->data; }

Result run(Session& s, const std::string& id, Params p) {
    Result r = s.run(id, p);
    if (!r.ok) std::printf("  %s failed: %s\n", id.c_str(), r.message.c_str());
    return r;
}

std::string pdfText(const TextDocument& d) {
    pdf::PdfFile f(pdf::writePdf(layoutDocument(d), {d.title, d.author, ""}));
    std::string t;
    for (int i = 0; i < f.pageCount(); ++i) t += f.pageText(i) + "\n";
    return t;
}

const Run* runWith(const Runs& rs, const std::string& text) {
    for (const auto& r : rs) {
        if (r.text.find(text) != std::string::npos) return &r;
    }
    return nullptr;
}

// A document that uses every part of the model.
void build(Session& s) {
    run(s, "office.doc_new", P({{"title", S("Field report")}, {"author", S("Sam")}}));
    run(s, "office.doc_add", P({{"text", S("Field report")}}));
    run(s, "office.doc_format", P({{"block", I(1)}, {"style", S("Title")}}));
    run(s, "office.doc_add", P({{"kind", S("heading")}, {"text", S("Summary")}}));
    run(s, "office.doc_add", P({{"text", S("The pilot ran for six weeks across three sites and met every target.")}}));
    run(s, "office.doc_add", P({{"kind", S("heading")}, {"text", S("Results")}}));
    run(s, "office.doc_add", P({{"kind", S("table")}, {"text", S("Site | Visits | Change\nNorth | 1,200 | +12%\nSouth | 950 | +4%\nTotal | 2,150 | +8%")}}));
    run(s, "office.doc_add", P({{"kind", S("numbered")}, {"numbering", S("lowerRoman")}, {"start", I(3)}, {"text", S("Keep the north schedule\nHire for south")}}));
    run(s, "office.doc_add", P({{"kind", S("heading")}, {"level", I(2)}, {"text", S("Notes")}}));
    run(s, "office.doc_add", P({{"text", S("Costs stayed under budget.")}}));
}

void testFormatting() {
    Session s;
    registerCommands(s);
    build(s);
    TextDocument& d = docOf(s);
    CHECK(d.blocks.size() == 8 && d.blocks[0].kind == Block::Paragraph && d.blocks[0].style == "Title");
    Result r = run(s, "office.doc_format", P({{"block", I(3)}, {"find", S("every target")}, {"bold", true}, {"color", S("#c00000")}, {"highlight", S("yellow")}}));
    CHECK(r.ok);
    const Run* hit = runWith(d.blocks[2].runs, "every target");
    CHECK(hit && hit->bold && rgbOf(hit->color) == 0xC00000 && rgbOf(hit->highlight) == 0xFFFF00 && hit->text == "every target");
    const Run* before = runWith(d.blocks[2].runs, "pilot");
    CHECK(before && !before->bold);
    r = run(s, "office.doc_format", P({{"block", I(3)}, {"align", S("justify")}, {"first_line", 18.0}, {"space_after", 12.0}, {"line_spacing", 1.5}}));
    CHECK(r.ok && d.blocks[2].fmt.align == 'j' && d.blocks[2].fmt.firstLine == 18 && d.blocks[2].fmt.after == 12 && d.blocks[2].fmt.lineSpacing == 1.5);
    r = run(s, "office.doc_format", P({{"block", I(3)}, {"find", S("nothing like this")}, {"bold", true}}));
    CHECK(!r.ok);
    // superscript and font on part of a list item
    r = run(s, "office.doc_format", P({{"block", I(6)}, {"item", I(2)}, {"find", S("south")}, {"position", S("superscript")}, {"font", S("serif")}}));
    const Run* sup = runWith(d.blocks[5].items[1].runs, "south");
    CHECK(r.ok && sup && sup->vertical == 1 && sup->font == "serif");
    // style= turns a paragraph into a heading and back
    r = run(s, "office.doc_format", P({{"block", I(8)}, {"style", S("Heading 3")}}));
    CHECK(r.ok && d.blocks[7].kind == Block::Heading && d.blocks[7].level == 3);
    r = run(s, "office.doc_format", P({{"block", I(8)}, {"style", S("Normal")}}));
    CHECK(r.ok && d.blocks[7].kind == Block::Paragraph && d.blocks[7].style.empty());
    // clear removes direct formatting
    r = run(s, "office.doc_format", P({{"block", I(3)}, {"clear", true}}));
    hit = runWith(d.blocks[2].runs, "every target");
    CHECK(r.ok && hit && !hit->bold && !hit->color && d.blocks[2].fmt.align == 0);
    s.history().undo();
    hit = runWith(d.blocks[2].runs, "every target");
    CHECK(hit && hit->bold && d.blocks[2].fmt.align == 'j');

    // styles
    r = run(s, "office.doc_style_define", P({{"name", S("Callout")}, {"italic", true}, {"color", S("#1f6f3f")}, {"indent", 24.0}, {"size", 12.0}}));
    CHECK(r.ok && d.styles.count("Callout") && d.styles["Callout"].italic == 1);
    r = run(s, "office.doc_format", P({{"block", I(8)}, {"style", S("Callout")}}));
    CHECK(r.ok && d.blocks[7].style == "Callout");
    const ResolvedStyle cs = resolveStyle(d, "Callout");
    CHECK(cs.italic && cs.size == 12 && cs.indent == 24 && rgbOf(cs.color) == 0x1F6F3F);
    r = run(s, "office.doc_style_define", P({{"name", S("Heading 1")}, {"color", S("#7a2e8e")}}));
    CHECK(r.ok && rgbOf(resolveStyle(d, "Heading1").color) == 0x7A2E8E && resolveStyle(d, "Heading1").bold);
    r = run(s, "office.doc_style_define", P({{"name", S("Loop")}, {"based_on", S("Loop")}}));
    CHECK(!r.ok);
    r = run(s, "office.doc_styles", {});
    CHECK(r.ok && r.message.find("Callout") != std::string::npos && r.message.find("(changed)") != std::string::npos);
    r = run(s, "office.doc_style_define", P({{"name", S("Callout")}, {"reset", true}}));
    CHECK(r.ok && !d.styles.count("Callout") && d.blocks[7].style.empty());

    // lists
    CHECK(d.blocks[5].numbering == "lowerRoman" && d.blocks[5].start == 3);
    r = run(s, "office.doc_list", P({{"block", I(6)}, {"numbering", S("upperLetter")}, {"start", I(1)}, {"item", I(2)}, {"level", I(2)}}));
    CHECK(r.ok && d.blocks[5].numbering == "upperLetter" && d.blocks[5].start == 1 && d.blocks[5].items[1].level == 1);
    CHECK(listLabel("lowerRoman", 4) == "iv." && listLabel("upperLetter", 28) == "AB." && listLabel("lowerLetter", 2) == "b)");

    // page setup with separate margins
    r = run(s, "office.doc_setup", P({{"margin_left", 30.0}, {"margin_top", 15.0}}));
    CHECK(r.ok && std::abs(d.page.left - 30 * 72 / 25.4) < 0.01 && std::abs(d.page.top - 15 * 72 / 25.4) < 0.01 && d.page.right == 72);
    r = run(s, "office.doc_setup", P({{"margin_left", 200.0}, {"margin_right", 200.0}}));
    CHECK(!r.ok);
}

void testTables() {
    Session s;
    registerCommands(s);
    build(s);
    TextDocument& d = docOf(s);
    Block& t = d.blocks[4];
    Result r = run(s, "office.doc_table_cell", P({{"block", I(5)}, {"row", I(4)}, {"col", I(1)}, {"to_col", I(3)}, {"fill", S("#dde7f3")}}));
    CHECK(r.ok && t.cellFormats.size() == 4 && rgbOf(t.cellFormats[3][2].fill) == 0xDDE7F3 && !t.cellFormats[0][0].fill);
    r = run(s, "office.doc_table_cell", P({{"block", I(5)}, {"row", I(2)}, {"col", I(2)}, {"to_row", I(4)}, {"to_col", I(3)}, {"align", S("right")}, {"valign", S("middle")}}));
    CHECK(r.ok && t.cellFormats[2][2].align == 'r' && t.cellFormats[1][1].valign == 'm');
    r = run(s, "office.doc_table_cell", P({{"block", I(5)}, {"row", I(1)}, {"col", I(1)}, {"text", S("**Place**")}}));
    CHECK(r.ok && plainText(t.rows[0][0]) == "Place" && t.rows[0][0][0].bold);
    r = run(s, "office.doc_table_insert", P({{"block", I(5)}, {"row", I(3)}, {"text", S("East | 400 | +9%")}}));
    CHECK(r.ok && t.rows.size() == 5 && plainText(t.rows[3][0]) == "East" && t.cellFormats.size() == 5 && rgbOf(t.cellFormats[4][0].fill) == 0xDDE7F3);
    r = run(s, "office.doc_table_merge", P({{"block", I(5)}, {"row", I(5)}, {"col", I(2)}, {"cols", I(2)}}));
    CHECK(r.ok && t.merges.size() == 1 && t.merges[0].cols == 2 && plainText(t.rows[4][1]) == "2,150\n+8%" && t.rows[4][2].empty());
    // a column inserted inside the merge widens it
    r = run(s, "office.doc_table_insert", P({{"block", I(5)}, {"col", I(2)}, {"where", S("right")}}));
    CHECK(r.ok && t.rows[0].size() == 4 && t.merges[0].cols == 3);
    r = run(s, "office.doc_table_delete", P({{"block", I(5)}, {"col", I(3)}}));
    CHECK(r.ok && t.rows[0].size() == 3 && t.merges[0].cols == 2 && plainText(t.rows[0][2]) == "Change");
    r = run(s, "office.doc_table_merge", P({{"block", I(5)}, {"row", I(2)}, {"col", I(1)}, {"rows", I(2)}}));
    CHECK(r.ok && t.merges.size() == 2);
    r = run(s, "office.doc_table_delete", P({{"block", I(5)}, {"row", I(2)}}));
    CHECK(r.ok && t.rows.size() == 4 && t.merges.size() == 1);  // the 2-row merge shrank to one cell
    r = run(s, "office.doc_table_merge", P({{"block", I(5)}, {"row", I(4)}, {"col", I(3)}, {"unmerge", true}}));
    CHECK(r.ok && t.merges.empty());
    r = run(s, "office.doc_table_format", P({{"block", I(5)}, {"widths", S("2,1,1")}, {"banded", true}, {"borders", false}}));
    CHECK(r.ok && t.colWidths.size() == 3 && t.colWidths[0] == 2 && t.banded && !t.borders);
    r = run(s, "office.doc_table_format", P({{"block", I(5)}, {"widths", S("1,1")}}));
    CHECK(!r.ok);
    r = run(s, "office.doc_table_delete", P({{"block", I(5)}, {"row", I(1)}, {"count", I(4)}}));
    CHECK(!r.ok);
    CHECK(pdfText(d).find("Place") != std::string::npos);
}

void testReview() {
    Session s;
    registerCommands(s);
    build(s);
    TextDocument& d = docOf(s);
    Result r = run(s, "office.doc_header_footer", P({{"header", S("{title} | | {date}")}, {"footer", S("Page {page} of {pages}")}}));
    CHECK(r.ok && d.headerFooter.footer == "Page {page} of {pages}");
    CHECK(fillFields("Page {page} of {pages} - {title}", 2, 5, d) == "Page 2 of 5 - Field report");
    r = run(s, "office.doc_toc", P({{"levels", I(2)}}));
    CHECK(r.ok && d.blocks[1].kind == Block::Toc && d.blocks[1].level == 2);  // after the Title paragraph
    r = run(s, "office.doc_toc", P({{"title", S("In this report")}}));
    CHECK(r.ok && plainText(d.blocks[1].runs) == "In this report" && d.blocks.size() == 9);
    r = run(s, "office.doc_footnote", P({{"block", I(4)}, {"after", S("six weeks")}, {"text", S("From *1 May* to 12 June.")}}));
    CHECK(r.ok);
    bool noteAfter = false;
    for (std::size_t i = 0; i + 1 < d.blocks[3].runs.size(); ++i) {
        if (endsWith(d.blocks[3].runs[i].text, "six weeks") && d.blocks[3].runs[i + 1].footnote == "From *1 May* to 12 June.") noteAfter = true;
    }
    CHECK(noteAfter);
    r = run(s, "office.doc_footnote", P({{"block", I(9)}, {"text", S("Second note")}}));
    CHECK(r.ok && r.message.find("footnote 2") != std::string::npos);
    r = run(s, "office.doc_footnotes", {});
    CHECK(r.ok && r.message.find("2 footnote") != std::string::npos);
    r = run(s, "office.doc_comment", P({{"block", I(4)}, {"find", S("three sites")}, {"text", S("Name them?")}, {"author", S("Ann")}}));
    CHECK(r.ok && d.comments.size() == 1 && d.comments[0].author == "Ann");
    const Run* c = runWith(d.blocks[3].runs, "three sites");
    CHECK(c && c->comment == 1 && c->text == "three sites");
    const std::string text = pdfText(d);
    CHECK(text.find("In this report") != std::string::npos && text.find("Page 1 of 1") != std::string::npos && text.find("From 1 May to 12 June.") != std::string::npos);

    // tracked changes
    r = run(s, "office.doc_track", P({{"on", true}, {"author", S("Ann")}}));
    CHECK(r.ok && d.trackChanges);
    r = run(s, "office.doc_edit", P({{"block", I(4)}, {"text", S("The pilot ran for six weeks across four sites and met every target.")}}));
    CHECK(r.ok && d.changes.size() == 2);
    const Run* gone = runWith(d.blocks[3].runs, "three");
    const Run* added = runWith(d.blocks[3].runs, "four");
    CHECK(gone && added && gone->change && added->change && !d.change(gone->change)->insert && d.change(added->change)->insert);
    const int delId = gone ? gone->change : 0;  // the runs move as later commands edit the document
    // the footnote survives the edit
    bool kept = false;
    for (const auto& x : d.blocks[3].runs) kept = kept || !x.footnote.empty();
    CHECK(kept);
    r = run(s, "office.doc_find_replace", P({{"find", S("budget")}, {"replace", S("plan")}}));
    CHECK(r.ok && d.changes.size() == 4 && plainText(d.blocks.back().runs).find("budgetplan") != std::string::npos);
    r = run(s, "office.doc_add", P({{"text", S("A new closing line.")}}));
    CHECK(r.ok && d.blocks.back().runs[0].change && d.changes.size() == 5);
    r = run(s, "office.doc_delete", P({{"block", I(3)}}));  // heading "Summary": marked, not removed
    CHECK(r.ok && d.blocks.size() == 10 && d.blocks[2].runs[0].change);
    r = run(s, "office.doc_changes", {});
    CHECK(r.ok && r.message.find("6 change") != std::string::npos);
    // deleting a pending insertion just removes it
    r = run(s, "office.doc_delete", P({{"block", I(10)}}));
    CHECK(r.ok && d.blocks.size() == 9);
    // a tracked document round-trips through DOCX with its changes, comments and notes
    TextDocument back = readDocx(writeDocx(d));
    CHECK(back.trackChanges && back.changes.size() >= 4 && back.comments.size() == 1 && back.comments[0].text == "Name them?");
    int notes = 0;
    for (const auto& b : back.blocks) {
        for (const auto& x : b.runs) notes += !x.footnote.empty();
    }
    CHECK(notes == 2);
    // accept one, reject the rest
    r = run(s, "office.doc_accept", P({{"id", I(delId)}}));
    CHECK(r.ok && plainText(d.blocks[3].runs).find("three") == std::string::npos);
    r = run(s, "office.doc_reject", {});
    CHECK(r.ok && d.changes.empty() && d.blocks[2].kind == Block::Heading && plainText(d.blocks[2].runs) == "Summary");
    CHECK(plainText(d.blocks[3].runs) == "The pilot ran for six weeks across  sites and met every target.");
    CHECK(plainText(d.blocks.back().runs) == "Costs stayed under budget.");
    // comments
    r = run(s, "office.doc_comment_resolve", P({{"id", I(1)}}));
    CHECK(r.ok && d.comments[0].resolved);
    r = run(s, "office.doc_comments", P({{"open_only", true}}));
    CHECK(r.ok && r.message == "no comments");
    r = run(s, "office.doc_comment_delete", P({{"all", true}}));
    CHECK(r.ok && d.comments.empty() && !runWith(d.blocks[3].runs, "sites")->comment);
    r = run(s, "office.doc_footnote_delete", P({{"number", I(1)}}));
    CHECK(r.ok && r.message.find("1 May") != std::string::npos);
    r = run(s, "office.doc_toc", P({{"remove", true}}));
    CHECK(r.ok && d.blocks[1].kind != Block::Toc);
}

// Everything the model holds survives DOCX and ODT.
void testRoundTrips() {
    Session s;
    registerCommands(s);
    build(s);
    run(s, "office.doc_format", P({{"block", I(3)}, {"find", S("every target")}, {"bold", true}, {"color", S("#c00000")}, {"highlight", S("yellow")}, {"strike", true}}));
    run(s, "office.doc_format", P({{"block", I(3)}, {"align", S("center")}, {"indent", 20.0}}));
    run(s, "office.doc_style_define", P({{"name", S("Callout")}, {"italic", true}, {"color", S("#1f6f3f")}, {"size", 12.0}}));
    run(s, "office.doc_style_define", P({{"name", S("Heading 1")}, {"color", S("#7a2e8e")}}));
    run(s, "office.doc_format", P({{"block", I(8)}, {"style", S("Callout")}}));
    run(s, "office.doc_table_merge", P({{"block", I(5)}, {"row", I(4)}, {"col", I(2)}, {"cols", I(2)}}));
    run(s, "office.doc_table_merge", P({{"block", I(5)}, {"row", I(2)}, {"col", I(1)}, {"rows", I(2)}}));
    run(s, "office.doc_table_cell", P({{"block", I(5)}, {"row", I(4)}, {"col", I(1)}, {"fill", S("#dde7f3")}}));
    run(s, "office.doc_table_format", P({{"block", I(5)}, {"widths", S("2,1,1")}}));
    run(s, "office.doc_header_footer", P({{"header", S("{title} | {date}")}, {"footer", S("Page {page} of {pages}")}}));
    run(s, "office.doc_toc", {});
    run(s, "office.doc_footnote", P({{"block", I(4)}, {"text", S("A *note*.")}}));
    run(s, "office.doc_comment", P({{"block", I(4)}, {"find", S("three")}, {"text", S("Which?")}, {"author", S("Ann")}}));
    run(s, "office.doc_setup", P({{"margin_left", 30.0}, {"landscape", true}}));
    const TextDocument& d = docOf(s);
    for (const bool odt : {false, true}) {
        const std::string bytes = odt ? writeOdt(d) : writeDocx(d);
        writeBytes((tmpDir() / (odt ? "rt.odt" : "rt.docx")).string(), bytes);
        TextDocument b = odt ? readOdt(bytes) : readDocx(bytes);
        const char* name = odt ? "odt" : "docx";
        std::printf("  checking %s\n", name);
        CHECK(b.title == "Field report" && b.blocks.size() == d.blocks.size());
        for (std::size_t i = 0; i < std::min(b.blocks.size(), d.blocks.size()); ++i) CHECK(b.blocks[i].kind == d.blocks[i].kind);
        CHECK(b.blocks[0].style == "Title");
        CHECK(b.blocks[1].kind == Block::Toc);
        const Run* hit = runWith(b.blocks[3].runs, "every target");
        CHECK(hit && hit->bold && hit->strike && rgbOf(hit->color) == 0xC00000 && rgbOf(hit->highlight) == 0xFFFF00);
        CHECK(b.blocks[3].fmt.align == 'c' && std::abs(b.blocks[3].fmt.indent - 20) < 0.1);
        const Run* com = runWith(b.blocks[3].runs, "three");
        CHECK(com && com->comment && b.comments.size() == 1 && b.comments[0].author == "Ann" && b.comments[0].text == "Which?");
        bool note = false;
        for (const auto& x : b.blocks[3].runs) note = note || x.footnote == "A *note*.";
        CHECK(note);
        CHECK(b.blocks[8].style == "Callout" && b.styles.count("Callout"));
        if (b.styles.count("Callout")) CHECK(resolveStyle(b, "Callout").italic && rgbOf(resolveStyle(b, "Callout").color) == 0x1F6F3F);
        CHECK(rgbOf(resolveStyle(b, "Heading1").color) == 0x7A2E8E);
        CHECK(!b.styles.count("Heading2"));  // unchanged built-ins stay unchanged
        const Block& t = b.blocks[5];
        CHECK(t.kind == Block::Table && t.merges.size() == 2 && t.colWidths.size() == 3);
        bool across = false, down = false;
        for (const auto& m : t.merges) {
            across = across || (m.row == 3 && m.col == 1 && m.cols == 2 && m.rows == 1);
            down = down || (m.row == 1 && m.col == 0 && m.rows == 2 && m.cols == 1);
        }
        CHECK(across && down);
        CHECK(t.cellFormats.size() == 4 && rgbOf(t.cellFormats[3][0].fill) == 0xDDE7F3);
        CHECK(b.blocks[6].kind == Block::List && b.blocks[6].numbering == "lowerRoman" && b.blocks[6].start == 3);
        CHECK(startsWith(b.headerFooter.header, "Field report | 20") && b.headerFooter.footer == "Page {page} of {pages}");  // {title} and {date} are filled in
        CHECK(b.page.width > b.page.height && std::abs(b.page.left - d.page.left) < 0.5 && std::abs(b.page.top - d.page.top) < 0.5);
    }
}

}  // namespace

int main() {
    testFormatting();
    testTables();
    testReview();
    testRoundTrips();
    return check::summary("office doc commands");
}
