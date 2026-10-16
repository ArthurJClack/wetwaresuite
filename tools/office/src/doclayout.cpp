// Page layout for documents: word wrap with mixed styles, headings that keep
// with the next lines, lists, tables with content-sized columns and
// repeated header rows, images, code and quotes, and page numbers.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/office/doc.hpp"
#include "text.hpp"

#include "textlayout.hpp"

namespace wetwaresuite::office {

namespace textlayout {

using pdf::Color;
using pdf::Font;

std::vector<Piece> pieces(const Runs& runs, const Style& st) {
    std::vector<Piece> out;
    for (const auto& r : runs) {
        Piece base;
        base.font = st.font;
        if (r.code) base.font.family = pdf::Family::Mono;
        base.font.bold = st.font.bold || r.bold;
        base.font.italic = st.font.italic || r.italic;
        base.size = r.code ? st.size * 0.92 : st.size;
        base.color = st.color;
        base.underline = r.underline;
        if (!r.link.empty()) {
            base.link = r.link;
            base.color = st.link;
            base.underline = true;
        }
        std::string cur;
        int kind = -1;  // 0 word, 1 spaces
        auto flush = [&] {
            if (cur.empty()) return;
            Piece p = base;
            p.text = cur;
            p.width = pdf::textWidth(p.font, cur, p.size);
            out.push_back(p);
            cur.clear();
        };
        for (char c : r.text) {
            if (c == '\n') {
                flush();
                Piece p = base;
                p.text = "\n";
                out.push_back(p);
                kind = -1;
                continue;
            }
            int k = (c == ' ' || c == '\t') ? 1 : 0;
            if (k != kind) flush();
            kind = k;
            cur += c == '\t' ? std::string("    ") : std::string(1, c);
        }
        flush();
    }
    return out;
}

// Splits a word that is wider than the line into pieces that fit.
static std::vector<Piece> splitWord(const Piece& p, double maxWidth) {
    std::vector<Piece> out;
    Piece cur = p;
    cur.text.clear();
    for (unsigned cp : pdf::utf8Decode(p.text)) {
        std::string ch = pdf::utf8Encode(cp);
        double w = pdf::textWidth(p.font, cur.text + ch, p.size);
        if (w > maxWidth && !cur.text.empty()) {
            cur.width = pdf::textWidth(p.font, cur.text, p.size);
            out.push_back(cur);
            cur.text.clear();
        }
        cur.text += ch;
    }
    if (!cur.text.empty()) {
        cur.width = pdf::textWidth(p.font, cur.text, p.size);
        out.push_back(cur);
    }
    return out;
}

std::vector<Line> wrap(const std::vector<Piece>& in, double maxWidth, double baseSize) {
    std::vector<Line> lines;
    Line line;
    line.size = baseSize;
    auto push = [&] {
        // drop trailing spaces
        while (!line.pieces.empty() && line.pieces.back().space()) {
            line.width -= line.pieces.back().width;
            line.pieces.pop_back();
            line.xs.pop_back();
        }
        lines.push_back(line);
        line = Line();
        line.size = baseSize;
    };
    std::vector<Piece> queue;
    for (const auto& p : in) {
        if (!p.space() && !p.newline() && p.width > maxWidth) {
            for (auto& s : splitWord(p, maxWidth)) queue.push_back(s);
        } else {
            queue.push_back(p);
        }
    }
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const Piece& p = queue[i];
        if (p.newline()) {
            push();
            continue;
        }
        if (p.space() && line.pieces.empty()) continue;
        if (!p.space() && line.width + p.width > maxWidth + 0.01 && !line.pieces.empty()) push();
        line.xs.push_back(line.width);
        line.pieces.push_back(p);
        line.width += p.width;
        line.size = std::max(line.size, p.size);
    }
    if (!line.pieces.empty() || lines.empty()) push();
    return lines;
}

void drawLine(pdf::Page& page, const Line& l, double x, double y) {
    const double baseline = y + l.size * 0.92;
    for (std::size_t i = 0; i < l.pieces.size(); ++i) {
        const Piece& p = l.pieces[i];
        if (p.space() && !p.underline) continue;
        pdf::TextOp t;
        t.x = x + l.xs[i];
        t.y = baseline;
        t.font = p.font;
        t.size = p.size;
        t.color = p.color;
        t.text = p.text;
        t.underline = p.underline;
        t.link = p.link;
        page.ops.emplace_back(std::move(t));
    }
}

}  // namespace textlayout

namespace {

using pdf::Color;
using pdf::Font;
using namespace textlayout;

const Color kText{0x22, 0x22, 0x22, 255};
const Color kHeading{0x1F, 0x38, 0x64, 255};
const Color kMuted{0x55, 0x55, 0x55, 255};

struct Layout {
    const TextDocument& doc;
    std::vector<pdf::Page> pages;
    double y = 0;
    double left, right, top, bottom;
    bool pageHasContent = false;

    explicit Layout(const TextDocument& d) : doc(d) {
        left = d.page.margin;
        right = d.page.width - d.page.margin;
        top = d.page.margin;
        bottom = d.page.height - d.page.margin;
        newPage();
    }

    pdf::Page& page() { return pages.back(); }

    void newPage() {
        pdf::Page p;
        p.width = doc.page.width;
        p.height = doc.page.height;
        pages.push_back(std::move(p));
        y = top;
        pageHasContent = false;
    }

    void ensure(double h) {
        if (y + h > bottom + 0.01 && pageHasContent) newPage();
    }

    double lineHeight(double size) const { return size * 1.32; }

    void drawLine(const Line& l, double x) { textlayout::drawLine(page(), l, x, y); }

    // Places wrapped lines starting at x, breaking pages between lines.
    void placeLines(const std::vector<Line>& lines, double x) {
        for (const auto& l : lines) {
            const double h = lineHeight(l.size);
            ensure(h);
            drawLine(l, x);
            y += h;
            pageHasContent = true;
        }
    }

    Style body() const {
        Style s;
        s.font.family = doc.page.font;
        s.size = doc.page.fontSize;
        return s;
    }

    void heading(const Block& b, const Block* next) {
        static const double scale[6] = {1.8, 1.45, 1.22, 1.08, 1.0, 1.0};
        static const double before[6] = {16, 13, 10, 8, 6, 6};
        Style s = body();
        const int lv = std::clamp(b.level, 1, 6);
        s.size = doc.page.fontSize * scale[lv - 1];
        s.font.bold = true;
        s.font.italic = lv >= 5;
        s.color = kHeading;
        auto lines = wrap(pieces(b.runs, s), right - left, s.size);
        if (pageHasContent) y += before[lv - 1];
        // keep the heading with the first lines of what follows
        double keep = lines.size() * lineHeight(s.size);
        if (next && (next->kind == Block::Paragraph || next->kind == Block::List || next->kind == Block::Quote)) keep += 2 * lineHeight(doc.page.fontSize);
        ensure(keep);
        placeLines(lines, left);
        y += 4;
    }

    void paragraph(const Runs& runs, Style s, double indent, double after) {
        auto lines = wrap(pieces(runs, s), right - left - indent, s.size);
        placeLines(lines, left + indent);
        y += after;
    }

    void list(const Block& b) {
        Style s = body();
        std::vector<int> counters(10, 0);
        for (const auto& it : b.items) {
            const int lv = std::clamp(it.level, 0, 9);
            for (int k = lv + 1; k < 10; ++k) counters[static_cast<std::size_t>(k)] = 0;
            const double indent = 20 + lv * 20;
            std::string marker;
            if (b.ordered) {
                int n = ++counters[static_cast<std::size_t>(lv)];
                if (lv % 3 == 1) marker = std::string(1, static_cast<char>('a' + (n - 1) % 26)) + ".";
                else marker = std::to_string(n) + ".";
            } else {
                marker = lv % 2 == 0 ? "\xE2\x80\xA2" : "\xE2\x80\x93";  // • and –
            }
            auto lines = wrap(pieces(it.runs, s), right - left - indent, s.size);
            const double h = lineHeight(s.size);
            ensure(h);
            Font mf = s.font;
            const double mw = pdf::textWidth(mf, marker, s.size);
            page().text(left + indent - 6 - mw, y + s.size * 0.92, marker, mf, s.size, b.ordered ? kText : kHeading);
            placeLines(lines, left + indent);
            y += 2;
        }
        y += 6;
    }

    void code(const Block& b) {
        Style s = body();
        s.font = Font{pdf::Family::Mono, false, false};
        s.size = doc.page.fontSize * 0.86;
        const double pad = 6;
        std::vector<Line> lines;
        for (const auto& raw : splitLines(plainText(b.runs))) {
            Run r;
            r.text = raw.empty() ? " " : raw;
            auto w = wrap(pieces({r}, s), right - left - 2 * pad, s.size);
            lines.insert(lines.end(), w.begin(), w.end());
        }
        if (lines.empty()) return;
        const double h = lineHeight(s.size);
        ensure(h + 2 * pad);
        std::size_t bgIndex = page().ops.size();
        double startY = y;
        y += pad;
        auto background = [&](double endY) {
            pdf::RectOp r{left, startY, right - left, endY - startY, Color{0xF4, 0xF4, 0xF4, 255}, std::nullopt, 1};
            page().ops.insert(page().ops.begin() + static_cast<std::ptrdiff_t>(bgIndex), r);
        };
        for (const auto& l : lines) {
            if (y + h + pad > bottom && pageHasContent) {
                background(y + pad);
                newPage();
                bgIndex = page().ops.size();
                startY = y;
                y += pad;
            }
            drawLine(l, left + pad);
            y += h;
            pageHasContent = true;
        }
        y += pad;
        background(y);
        y += 8;
    }

    void quote(const Block& b) {
        Style s = body();
        s.font.italic = true;
        s.color = kMuted;
        auto lines = wrap(pieces(b.runs, s), right - left - 18, s.size);
        const double h = lineHeight(s.size);
        double startY = y;
        for (const auto& l : lines) {
            if (y + h > bottom && pageHasContent) {
                page().rect(left + 2, startY, 3, y - startY, Color{0xCC, 0xCC, 0xCC, 255});
                newPage();
                startY = y;
            }
            drawLine(l, left + 18);
            y += h;
            pageHasContent = true;
        }
        page().rect(left + 2, startY, 3, y - startY, Color{0xCC, 0xCC, 0xCC, 255});
        y += 8;
    }

    void image(const Block& b) {
        if (!b.pixels || b.pixels->pixels.empty()) return;
        const double pw = b.pixels->pixels.width(), ph = b.pixels->pixels.height();
        double w = b.width > 0 ? b.width : pw * 0.75;
        w = std::min(w, right - left);
        double h = w * ph / pw;
        const double maxH = bottom - top;
        if (h > maxH) {
            h = maxH;
            w = h * pw / ph;
        }
        ensure(h);
        page().image(left + (right - left - w) / 2, y, w, h, b.pixels);
        y += h + 10;
        pageHasContent = true;
    }

    void table(const Block& b) {
        std::size_t cols = 0;
        for (const auto& r : b.rows) cols = std::max(cols, r.size());
        if (!cols) return;
        Style s = body();
        s.size = doc.page.fontSize * 0.95;
        const double pad = 4;
        const double W = right - left;
        // natural and minimum widths per column
        std::vector<double> nat(cols, 0), mn(cols, 0);
        for (std::size_t r = 0; r < b.rows.size(); ++r) {
            Style cs = s;
            cs.font.bold = r == 0 && b.header;
            for (std::size_t c = 0; c < b.rows[r].size(); ++c) {
                double lineW = 0;
                for (const auto& p : pieces(b.rows[r][c], cs)) {
                    if (p.newline()) {
                        nat[c] = std::max(nat[c], lineW);
                        lineW = 0;
                        continue;
                    }
                    lineW += p.width;
                    if (!p.space()) mn[c] = std::max(mn[c], std::min(p.width, W / 3));
                }
                nat[c] = std::max(nat[c], lineW);
            }
        }
        std::vector<double> widths(cols);
        double sumNat = 0, sumMin = 0;
        for (std::size_t c = 0; c < cols; ++c) {
            nat[c] += 2 * pad + 1;
            mn[c] += 2 * pad + 1;
            sumNat += nat[c];
            sumMin += mn[c];
        }
        if (sumNat <= W) {
            for (std::size_t c = 0; c < cols; ++c) widths[c] = nat[c] + (W - sumNat) / static_cast<double>(cols);
        } else if (sumMin < W) {
            double extra = W - sumMin, want = sumNat - sumMin;
            for (std::size_t c = 0; c < cols; ++c) widths[c] = mn[c] + (want > 0 ? extra * (nat[c] - mn[c]) / want : 0);
        } else {
            for (std::size_t c = 0; c < cols; ++c) widths[c] = W / static_cast<double>(cols);
        }
        const Color border{0x99, 0x99, 0x99, 255};
        auto rowLines = [&](std::size_t r) {
            Style cs = s;
            cs.font.bold = r == 0 && b.header;
            std::vector<std::vector<Line>> cells(cols);
            double h = 0;
            for (std::size_t c = 0; c < cols; ++c) {
                Runs runs = c < b.rows[r].size() ? b.rows[r][c] : Runs{};
                cells[c] = wrap(pieces(runs, cs), widths[c] - 2 * pad, cs.size);
                double ch = 0;
                for (const auto& l : cells[c]) ch += lineHeight(l.size);
                h = std::max(h, ch);
            }
            return std::make_pair(cells, h + 2 * pad);
        };
        auto drawRow = [&](std::size_t r) {
            auto [cells, h] = rowLines(r);
            if (y + h > bottom && pageHasContent) {
                newPage();
                if (b.header && r > 0) {
                    auto [hc, hh] = rowLines(0);
                    double x = left;
                    page().rect(left, y, W, hh, Color{0xEE, 0xEE, 0xEE, 255});
                    for (std::size_t c = 0; c < cols; ++c) {
                        page().rect(x, y, widths[c], hh, std::nullopt, border, 0.5);
                        double saveY = y;
                        y += pad;
                        for (const auto& l : hc[c]) {
                            drawLine(l, x + pad);
                            y += lineHeight(l.size);
                        }
                        y = saveY;
                        x += widths[c];
                    }
                    y += hh;
                }
            }
            double x = left;
            if (r == 0 && b.header) page().rect(left, y, W, h, Color{0xEE, 0xEE, 0xEE, 255});
            for (std::size_t c = 0; c < cols; ++c) {
                page().rect(x, y, widths[c], h, std::nullopt, border, 0.5);
                double saveY = y;
                y += pad;
                for (const auto& l : cells[c]) {
                    drawLine(l, x + pad);
                    y += lineHeight(l.size);
                }
                y = saveY;
                x += widths[c];
            }
            y += h;
            pageHasContent = true;
        };
        for (std::size_t r = 0; r < b.rows.size(); ++r) drawRow(r);
        y += 10;
    }
};

}  // namespace

std::vector<pdf::Page> layoutDocument(const TextDocument& doc) {
    Layout L(doc);
    for (std::size_t i = 0; i < doc.blocks.size(); ++i) {
        const Block& b = doc.blocks[i];
        const Block* next = i + 1 < doc.blocks.size() ? &doc.blocks[i + 1] : nullptr;
        switch (b.kind) {
            case Block::Heading: L.heading(b, next); break;
            case Block::Paragraph: L.paragraph(b.runs, L.body(), 0, doc.page.fontSize * 0.75); break;
            case Block::List: L.list(b); break;
            case Block::Table: L.table(b); break;
            case Block::Image: L.image(b); break;
            case Block::Code: L.code(b); break;
            case Block::Quote: L.quote(b); break;
            case Block::PageBreak:
                if (L.pageHasContent) L.newPage();
                break;
        }
    }
    if (doc.page.pageNumbers && L.pages.size() > 1) {
        const std::size_t n = L.pages.size();
        for (std::size_t i = 0; i < n; ++i) {
            std::string label = std::to_string(i + 1) + " / " + std::to_string(n);
            Font f{doc.page.font, false, false};
            double w = pdf::textWidth(f, label, 9);
            L.pages[i].text((doc.page.width - w) / 2, doc.page.height - doc.page.margin / 2 + 3, label, f, 9, Color{0x88, 0x88, 0x88, 255});
        }
    }
    return L.pages;
}

}  // namespace wetwaresuite::office
