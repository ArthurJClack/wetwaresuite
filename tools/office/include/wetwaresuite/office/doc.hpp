// Text documents as a list of blocks (headings, paragraphs, lists, tables,
// images, code, quotes, page breaks, a table of contents) with styled runs,
// paragraph formats, named styles, headers and footers, footnotes, comments and
// tracked changes. Read from and written to DOCX, ODT, Markdown, HTML and plain
// text, and laid out into PDF pages by the suite's own layout engine.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/office/color.hpp"
#include "wetwaresuite/pdf/page.hpp"

namespace wetwaresuite::office {

struct Run {
    std::string text;  // may hold '\n' for a line break and '\t'
    bool bold = false, italic = false, underline = false, code = false, strike = false;
    int vertical = 0;             // 1 superscript, -1 subscript
    std::uint32_t color = 0;      // text colour (with kColorSet); 0 = automatic
    std::uint32_t highlight = 0;  // background colour behind the text
    double size = 0;              // pt; 0 = the paragraph's size
    std::string font;             // "", "sans", "serif" or "mono"
    std::string link;             // URL, empty when not a link
    std::string footnote;         // a footnote reference: the note's text as inline Markdown (`text` stays empty)
    int comment = 0;              // id of the comment (TextDocument::comments) on this text, 0 = none
    int change = 0;               // id of the tracked change (TextDocument::changes) this text belongs to, 0 = none
    bool sameStyle(const Run& o) const {
        return bold == o.bold && italic == o.italic && underline == o.underline && code == o.code && strike == o.strike && vertical == o.vertical &&
               color == o.color && highlight == o.highlight && size == o.size && font == o.font && link == o.link && footnote.empty() &&
               o.footnote.empty() && comment == o.comment && change == o.change;
    }
};
using Runs = std::vector<Run>;

struct ListItem {
    int level = 0;  // nesting, 0 = top
    Runs runs;
};

// Paragraph formatting on top of the paragraph's style.
struct ParaFormat {
    char align = 0;                  // 0 the style's (left), 'l', 'c', 'r' or 'j' (justified)
    double indent = 0;               // left indent, pt
    double indentRight = 0;          // right indent, pt
    double firstLine = 0;            // extra indent of the first line, pt (negative: hanging)
    double before = -1, after = -1;  // space above and below, pt; -1 = the style's
    double lineSpacing = 0;          // line height as a multiple of single spacing; 0 = the style's
    bool keepWithNext = false;
    bool pageBreakBefore = false;
    bool any() const {
        return align || indent || indentRight || firstLine || before >= 0 || after >= 0 || lineSpacing > 0 || keepWithNext || pageBreakBefore;
    }
};

// A named paragraph style. Built-in ones (Normal, Title, Subtitle, Heading1-6, Quote, Code, Caption) have
// defaults that an entry in TextDocument::styles changes; other names are styles of your own.
struct StyleDef {
    std::string name;          // shown name, e.g. "Heading 1"
    std::string basedOn;       // style id it starts from ("" = Normal)
    std::string font;          // "", "sans", "serif", "mono"
    double size = 0;           // pt; 0 = inherited
    std::uint32_t color = 0;   // 0 = inherited
    int bold = -1, italic = -1, underline = -1;  // -1 = inherited
    ParaFormat para;           // align, indents, spacing (-1 / 0 = inherited)
};

// Merged table cells: the top-left one holds the content.
struct CellSpan {
    int row = 0, col = 0, rows = 1, cols = 1;
};
struct CellFormat {
    std::uint32_t fill = 0;  // background
    char align = 0;          // 0, 'l', 'c', 'r'
    char valign = 0;         // 0 top, 'm' middle, 'b' bottom
};

struct Block {
    enum Kind { Heading, Paragraph, List, Table, Image, PageBreak, Code, Quote, Toc };
    Kind kind = Paragraph;
    int level = 1;     // heading level 1..6; table of contents: deepest heading level listed
    Runs runs;         // heading, paragraph, quote, table-of-contents title; code keeps its text in one run
    std::string style;  // paragraph: style id ("" = Normal, "Title", "Subtitle", "Caption" or your own)
    ParaFormat fmt;     // text blocks and images (align)
    bool ordered = false;
    std::string numbering;  // list: bullet, decimal, lowerLetter, upperLetter, lowerRoman, upperRoman ("" = by `ordered`)
    int start = 1;          // numbered list: first number
    std::vector<ListItem> items;          // list
    std::vector<std::vector<Runs>> rows;  // table cells
    bool header = true;                   // table: first row is a header
    std::vector<CellSpan> merges;         // table: merged areas
    std::vector<double> colWidths;        // table: relative column widths (empty = fit the contents)
    std::vector<std::vector<CellFormat>> cellFormats;  // table: per cell (may be smaller than the table)
    bool borders = true;                  // table: grid lines
    bool banded = false;                  // table: shade every other row
    std::string image;                    // encoded PNG or JPEG bytes
    std::shared_ptr<const pdf::ImageData> pixels;
    double width = 0;  // image width in points; 0 = natural size, shrunk to fit the page
    std::string alt;   // image description / caption
};

const char* kindName(Block::Kind kind);

struct PageSetup {
    double width = 595.28, height = 841.89;  // A4 portrait, points
    double top = 72, bottom = 72, left = 72, right = 72;  // margins, points
    pdf::Family font = pdf::Family::Sans;
    double fontSize = 11;
    bool pageNumbers = true;  // "n / N" at the bottom when there is no footer
};

// Header and footer text: inline Markdown with fields {page}, {pages}, {title}, {author} and {date}.
// "left | centre | right" puts parts at the left, middle and right; without "|" the text is centred.
struct HeaderFooter {
    std::string header, footer;
    bool differentFirst = false;            // the first page has its own (often empty) header and footer
    std::string firstHeader, firstFooter;
};

struct Comment {
    int id = 0;
    std::string author, date, text;
    bool resolved = false;
};

struct TrackedChange {
    int id = 0;
    bool insert = true;  // false: a deletion (the text stays, struck through, until accepted)
    std::string author, date;
};

struct TextDocument {
    std::string title, author;
    std::vector<Block> blocks;
    PageSetup page;
    HeaderFooter headerFooter;
    std::map<std::string, StyleDef> styles;  // style id -> changes to a built-in style, or a style of your own
    std::vector<Comment> comments;
    std::vector<TrackedChange> changes;
    bool trackChanges = false;  // edits are recorded as insertions and deletions
    bool showMarkup = true;     // previews and PDFs show tracked changes and comment highlights

    const Comment* comment(int id) const;
    const TrackedChange* change(int id) const;
    int nextCommentId() const;
    int nextChangeId() const;
};

// The style a block is drawn with: built-in defaults, then the document's changes (follows basedOn).
struct ResolvedStyle {
    pdf::Family font = pdf::Family::Sans;
    double size = 11;
    std::uint32_t color = 0;
    bool bold = false, italic = false, underline = false;
    char align = 'l';
    double indent = 0, indentRight = 0, firstLine = 0, before = 0, after = 8, lineSpacing = 1.1;
    bool keepWithNext = false;
};
ResolvedStyle resolveStyle(const TextDocument& doc, const std::string& styleId);
std::string blockStyleId(const Block& b);  // "Heading2", "Quote", "Normal", the paragraph's style ...
bool isBuiltinStyle(const std::string& id);
std::vector<std::string> builtinStyleIds();
std::string styleIdOf(const std::string& nameOrId);  // "heading 1" / "Heading 1" -> "Heading1"; others unchanged

// Inline Markdown (**bold**, *italic*, <u>underline</u>, ~~strike~~, `code`, [text](url), <sup>2</sup>, <sub>2</sub>, <mark>, <span style="color:...">, ^[footnote]) to runs and back.
Runs parseInline(const std::string& markdown);
std::string inlineMarkdown(const Runs& runs);
std::string plainText(const Runs& runs);
std::string blockText(const Block& b);  // plain text of any block (lists one item per line, tables tab-separated)
void mergeRuns(Runs& runs);             // joins neighbours with the same style

// Tracked changes and comments.
int acceptChanges(TextDocument& doc, int id);  // id 0 = all; returns how many changes were settled
int rejectChanges(TextDocument& doc, int id);
// The final text (deletions dropped, insertions kept) without changing the document.
TextDocument acceptedView(const TextDocument& doc);
// Headings for a table of contents, in order: level and text.
std::vector<std::pair<int, std::string>> outline(const TextDocument& doc, int maxLevel);
// Number of a list item as shown ("3.", "b)", "iv.") for a numbering format.
std::string listLabel(const std::string& numbering, int n);
// Fills {page}, {pages}, {title}, {author} and {date} in header/footer text.
std::string fillFields(const std::string& text, int page, int pages, const TextDocument& doc);

// Loads image bytes into a block (non-PNG/JPEG images are re-encoded as PNG). Throws std::runtime_error.
void setImage(Block& b, const std::string& bytes);

// Files. Readers throw std::runtime_error. baseDir resolves relative image paths.
TextDocument readMarkdown(const std::string& text, const std::string& baseDir = {});
struct MediaFile {
    std::string path;  // relative path written into the document
    std::string bytes;
};
// Images become files under mediaPrefix (e.g. "report_media/image1.png"), returned in `media`.
std::string writeMarkdown(const TextDocument& doc, const std::string& mediaPrefix, std::vector<MediaFile>& media);
TextDocument readHtml(const std::string& html, const std::string& baseDir = {});
std::string writeHtml(const TextDocument& doc);  // one self-contained file, images as data URIs
std::string writeText(const TextDocument& doc);
TextDocument readDocx(const std::string& bytes);
std::string writeDocx(const TextDocument& doc);
TextDocument readOdt(const std::string& bytes);
std::string writeOdt(const TextDocument& doc);

// Lays the document out on pages: word wrap, styles, lists, tables, images, headers and footers,
// footnotes and the table of contents.
std::vector<pdf::Page> layoutDocument(const TextDocument& doc);
// The page each heading starts on (1-based), in outline() order; used for tables of contents.
std::vector<int> headingPages(const TextDocument& doc);

}  // namespace wetwaresuite::office
