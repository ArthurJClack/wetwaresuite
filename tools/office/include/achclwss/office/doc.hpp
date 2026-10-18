// Text documents as a simple list of blocks (headings, paragraphs, lists,
// tables, images, code, quotes, page breaks) with styled runs, read from and
// written to DOCX, Markdown, HTML, ODT (read) and plain text, and laid out
// into PDF pages by the suite's own layout engine.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "achclwss/pdf/page.hpp"

namespace achclwss::office {

struct Run {
    std::string text;  // may hold '\n' for a line break and '\t'
    bool bold = false, italic = false, underline = false, code = false;
    std::string link;  // URL, empty when not a link
    bool sameStyle(const Run& o) const {
        return bold == o.bold && italic == o.italic && underline == o.underline && code == o.code && link == o.link;
    }
};
using Runs = std::vector<Run>;

struct ListItem {
    int level = 0;  // nesting, 0 = top
    Runs runs;
};

struct Block {
    enum Kind { Heading, Paragraph, List, Table, Image, PageBreak, Code, Quote };
    Kind kind = Paragraph;
    int level = 1;     // heading level 1..6
    Runs runs;         // heading, paragraph, quote; code keeps its text in one run
    bool ordered = false;
    std::vector<ListItem> items;          // list
    std::vector<std::vector<Runs>> rows;  // table cells
    bool header = true;                   // table: first row is a header
    std::string image;                    // encoded PNG or JPEG bytes
    std::shared_ptr<const pdf::ImageData> pixels;
    double width = 0;  // image width in points; 0 = natural size, shrunk to fit the page
    std::string alt;   // image description / caption
};

const char* kindName(Block::Kind kind);

struct PageSetup {
    double width = 595.28, height = 841.89;  // A4 portrait, points
    double margin = 72;                      // all four sides, points
    pdf::Family font = pdf::Family::Sans;
    double fontSize = 11;
    bool pageNumbers = true;
};

struct TextDocument {
    std::string title, author;
    std::vector<Block> blocks;
    PageSetup page;
};

// Inline Markdown (**bold**, *italic*, <u>underline</u>, `code`, [text](url)) to runs and back.
Runs parseInline(const std::string& markdown);
std::string inlineMarkdown(const Runs& runs);
std::string plainText(const Runs& runs);
std::string blockText(const Block& b);  // plain text of any block (lists one item per line, tables tab-separated)
void mergeRuns(Runs& runs);             // joins neighbours with the same style

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

// Lays the document out on pages: word wrap, headings, lists, tables, images, page numbers.
std::vector<pdf::Page> layoutDocument(const TextDocument& doc);

}  // namespace achclwss::office
