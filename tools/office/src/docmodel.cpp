// Document model helpers: named styles, tracked changes, the outline for a
// table of contents, list numbering and header/footer fields.
#include <algorithm>
#include <ctime>

#include "wetwaresuite/office/doc.hpp"
#include "text.hpp"

namespace wetwaresuite::office {

const Comment* TextDocument::comment(int id) const {
    for (const auto& c : comments) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

const TrackedChange* TextDocument::change(int id) const {
    for (const auto& c : changes) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

int TextDocument::nextCommentId() const {
    int n = 0;
    for (const auto& c : comments) n = std::max(n, c.id);
    return n + 1;
}

int TextDocument::nextChangeId() const {
    int n = 0;
    for (const auto& c : changes) n = std::max(n, c.id);
    return n + 1;
}

// ---- styles --------------------------------------------------------------------------

namespace {

const std::vector<std::string>& builtins() {
    static const std::vector<std::string> ids = {"Normal",   "Title",    "Subtitle", "Heading1", "Heading2", "Heading3", "Heading4",
                                                 "Heading5", "Heading6", "Quote",    "Code",     "Caption",  "ListParagraph"};
    return ids;
}

// The built-in look of a style, before the document's own changes.
StyleDef builtinDef(const std::string& id) {
    StyleDef s;
    s.name = id;
    if (id == "Title") {
        s.name = "Title";
        s.size = 2.4;  // sizes below 4 are multiples of the body size
        s.color = 0x1F3864 | kColorSet;
        s.bold = 1;
        s.para.after = 12;
        s.para.before = 0;
    } else if (id == "Subtitle") {
        s.size = 1.4;
        s.color = 0x595959 | kColorSet;
        s.italic = 1;
        s.para.after = 14;
    } else if (startsWith(id, "Heading") && id.size() == 8) {
        static const double scale[6] = {1.8, 1.45, 1.22, 1.08, 1.0, 1.0};
        static const double before[6] = {16, 13, 10, 8, 6, 6};
        const int lv = std::clamp(id[7] - '0', 1, 6);
        s.name = "Heading " + std::to_string(lv);
        s.size = scale[lv - 1];
        s.bold = 1;
        s.italic = lv >= 5 ? 1 : -1;
        s.color = 0x1F3864 | kColorSet;
        s.para.before = before[lv - 1];
        s.para.after = 4;
        s.para.keepWithNext = true;
    } else if (id == "Quote") {
        s.italic = 1;
        s.color = 0x555555 | kColorSet;
        s.para.indent = 18;
        s.para.after = 8;
    } else if (id == "Code") {
        s.font = "mono";
        s.size = 0.86;
        s.para.after = 8;
        s.para.lineSpacing = 1.0;
    } else if (id == "Caption") {
        s.size = 0.85;
        s.italic = 1;
        s.color = 0x44546A | kColorSet;
        s.para.after = 10;
    } else if (id == "ListParagraph") {
        s.name = "List Paragraph";
        s.para.after = 2;
    } else if (id == "Normal") {
        s.para.after = -1;  // body default
    }
    return s;
}

pdf::Family familyOf(const std::string& f, pdf::Family fallback) {
    const std::string l = lower(f);
    if (l == "serif" || l == "times") return pdf::Family::Serif;
    if (l == "mono" || l == "courier") return pdf::Family::Mono;
    if (l == "sans" || l == "helvetica" || l == "arial") return pdf::Family::Sans;
    return fallback;
}

void apply(ResolvedStyle& r, const StyleDef& s, double bodySize) {
    if (!s.font.empty()) r.font = familyOf(s.font, r.font);
    if (s.size > 0) r.size = s.size < 4 ? bodySize * s.size : s.size;
    if (s.color) r.color = s.color;
    if (s.bold >= 0) r.bold = s.bold != 0;
    if (s.italic >= 0) r.italic = s.italic != 0;
    if (s.underline >= 0) r.underline = s.underline != 0;
    const ParaFormat& p = s.para;
    if (p.align) r.align = p.align;
    if (p.indent) r.indent = p.indent;
    if (p.indentRight) r.indentRight = p.indentRight;
    if (p.firstLine) r.firstLine = p.firstLine;
    if (p.before >= 0) r.before = p.before;
    if (p.after >= 0) r.after = p.after;
    if (p.lineSpacing > 0) r.lineSpacing = p.lineSpacing;
    if (p.keepWithNext) r.keepWithNext = true;
}

}  // namespace

bool isBuiltinStyle(const std::string& id) { return std::find(builtins().begin(), builtins().end(), id) != builtins().end(); }
std::vector<std::string> builtinStyleIds() { return builtins(); }

std::string styleIdOf(const std::string& nameOrId) {
    std::string t = trimCopy(nameOrId);
    const std::string l = lower(t);
    std::string squeezed;
    for (char c : l) {
        if (c != ' ' && c != '_' && c != '-') squeezed += c;
    }
    for (const auto& id : builtins()) {
        if (lower(id) == squeezed) return id;
    }
    if (squeezed == "body" || squeezed == "bodytext" || squeezed == "default" || squeezed == "textbody") return "Normal";
    if (squeezed == "quotation" || squeezed == "intensequote") return "Quote";
    return t;
}

ResolvedStyle resolveStyle(const TextDocument& doc, const std::string& styleId) {
    ResolvedStyle r;
    r.font = doc.page.font;
    r.size = doc.page.fontSize;
    r.after = doc.page.fontSize * 0.75;
    // the chain from Normal down to this style
    std::vector<std::string> chain;
    std::string id = styleId.empty() ? "Normal" : styleId;
    for (int hops = 0; hops < 10 && !id.empty(); ++hops) {
        if (std::find(chain.begin(), chain.end(), id) != chain.end()) break;
        chain.push_back(id);
        if (id == "Normal") break;
        auto it = doc.styles.find(id);
        std::string base = it != doc.styles.end() && !it->second.basedOn.empty() ? it->second.basedOn : (isBuiltinStyle(id) || it != doc.styles.end() ? "Normal" : "");
        id = base;
    }
    if (chain.empty() || chain.back() != "Normal") chain.push_back("Normal");
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (isBuiltinStyle(*it)) apply(r, builtinDef(*it), doc.page.fontSize);
        if (auto own = doc.styles.find(*it); own != doc.styles.end()) apply(r, own->second, doc.page.fontSize);
    }
    return r;
}

std::string blockStyleId(const Block& b) {
    switch (b.kind) {
        case Block::Heading: return "Heading" + std::to_string(std::clamp(b.level, 1, 6));
        case Block::Quote: return b.style.empty() ? "Quote" : b.style;
        case Block::Code: return "Code";
        case Block::List: return "ListParagraph";
        case Block::Image: return b.style.empty() ? "Normal" : b.style;
        default: return b.style.empty() ? "Normal" : b.style;
    }
}

// ---- tracked changes ------------------------------------------------------------------

namespace {

// Applies `keep(run)` to every run list in the document; runs it rejects are dropped.
template <typename F>
void eachRuns(TextDocument& doc, F fn) {
    for (auto& b : doc.blocks) {
        fn(b.runs);
        for (auto& it : b.items) fn(it.runs);
        for (auto& row : b.rows) {
            for (auto& cell : row) fn(cell);
        }
    }
}

int settle(TextDocument& doc, int id, bool accept) {
    std::vector<int> ids;
    for (const auto& c : doc.changes) {
        if (id == 0 || c.id == id) ids.push_back(c.id);
    }
    if (ids.empty()) return 0;
    auto matches = [&](int c) { return c && std::find(ids.begin(), ids.end(), c) != ids.end(); };
    eachRuns(doc, [&](Runs& runs) {
        Runs out;
        for (auto& r : runs) {
            if (!matches(r.change)) {
                out.push_back(std::move(r));
                continue;
            }
            const TrackedChange* ch = doc.change(r.change);
            const bool keep = ch && (ch->insert == accept);
            if (!keep) continue;
            r.change = 0;
            out.push_back(std::move(r));
        }
        runs = std::move(out);
        mergeRuns(runs);
    });
    // list items, paragraphs and table rows left with nothing at all go too
    std::vector<Block> kept;
    for (auto& b : doc.blocks) {
        if (b.kind == Block::List) {
            std::vector<ListItem> items;
            for (auto& it : b.items) {
                if (!it.runs.empty()) items.push_back(std::move(it));
            }
            b.items = std::move(items);
            if (b.items.empty()) continue;
        }
        const bool text = b.kind == Block::Paragraph || b.kind == Block::Heading || b.kind == Block::Quote;
        if (text && b.runs.empty()) continue;
        kept.push_back(std::move(b));
    }
    doc.blocks = std::move(kept);
    doc.changes.erase(std::remove_if(doc.changes.begin(), doc.changes.end(), [&](const TrackedChange& c) { return matches(c.id); }), doc.changes.end());
    return static_cast<int>(ids.size());
}

}  // namespace

int acceptChanges(TextDocument& doc, int id) { return settle(doc, id, true); }
int rejectChanges(TextDocument& doc, int id) { return settle(doc, id, false); }

TextDocument acceptedView(const TextDocument& doc) {
    TextDocument copy = doc;
    acceptChanges(copy, 0);
    return copy;
}

// ---- outline, numbering, fields ---------------------------------------------------------------

std::vector<std::pair<int, std::string>> outline(const TextDocument& doc, int maxLevel) {
    std::vector<std::pair<int, std::string>> out;
    for (const auto& b : doc.blocks) {
        if (b.kind == Block::Heading && b.level <= maxLevel) {
            std::string t;
            for (const auto& r : b.runs) {
                const TrackedChange* ch = r.change ? doc.change(r.change) : nullptr;
                if (ch && !ch->insert) continue;  // deleted text isn't in the outline
                t += r.text;
            }
            for (auto& c : t) {
                if (c == '\n' || c == '\t') c = ' ';
            }
            out.emplace_back(b.level, trimCopy(t));
        }
    }
    return out;
}

namespace {

std::string roman(int n, bool upperCase) {
    static const std::pair<int, const char*> parts[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
                                                        {50, "l"},   {40, "xl"},  {10, "x"},  {9, "ix"},   {5, "v"},   {4, "iv"}, {1, "i"}};
    std::string s;
    for (const auto& [v, t] : parts) {
        while (n >= v) {
            s += t;
            n -= v;
        }
    }
    return upperCase ? upper(s) : s;
}

std::string letters(int n, bool upperCase) {
    std::string s;
    while (n > 0) {
        --n;
        s.insert(s.begin(), static_cast<char>((upperCase ? 'A' : 'a') + n % 26));
        n /= 26;
    }
    return s;
}

}  // namespace

std::string listLabel(const std::string& numbering, int n) {
    if (numbering == "lowerLetter") return letters(n, false) + ")";
    if (numbering == "upperLetter") return letters(n, true) + ".";
    if (numbering == "lowerRoman") return roman(n, false) + ".";
    if (numbering == "upperRoman") return roman(n, true) + ".";
    return std::to_string(n) + ".";
}

std::string fillFields(const std::string& text, int page, int pages, const TextDocument& doc) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            const std::size_t close = text.find('}', i);
            if (close != std::string::npos && close - i < 12) {
                const std::string key = lower(text.substr(i + 1, close - i - 1));
                std::string v;
                bool known = true;
                if (key == "page") v = std::to_string(page);
                else if (key == "pages") v = std::to_string(pages);
                else if (key == "title") v = doc.title;
                else if (key == "author") v = doc.author;
                else if (key == "date") {
                    const std::time_t now = std::time(nullptr);
                    char buf[16];
                    std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&now));
                    v = buf;
                } else known = false;
                if (known) {
                    out += v;
                    i = close;
                    continue;
                }
            }
        }
        out += text[i];
    }
    return out;
}

}  // namespace wetwaresuite::office
