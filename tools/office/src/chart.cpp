// Bar, horizontal bar, line and pie charts drawn with the raster painter and
// the preview font, plus the sheet grid laid out as pages.
#include "wetwaresuite/office/chart.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::office {

using raster::Color;

const std::vector<std::string>& chartTypes() {
    static const std::vector<std::string> t = {"bar", "hbar", "line", "pie"};
    return t;
}

namespace {

constexpr double kPi = 3.14159265358979323846;
const Color kInk{0x33, 0x33, 0x33, 255};
const Color kMutedInk{0x66, 0x66, 0x66, 255};
const Color kGrid{0xE2, 0xE2, 0xE2, 255};
const Color kAxis{0x99, 0x99, 0x99, 255};

const std::vector<Color>& palette() {
    static const std::vector<Color> p = {
        {0x2B, 0x57, 0x9A, 255}, {0xE9, 0x7B, 0x26, 255}, {0x3A, 0x9B, 0x5C, 255}, {0xC9, 0x3C, 0x3C, 255}, {0x7E, 0x57, 0xC2, 255},
        {0x1F, 0x9E, 0xA8, 255}, {0xD8, 0xA9, 0x1E, 255}, {0x8C, 0x56, 0x4B, 255}, {0xE3, 0x77, 0xC2, 255}, {0x7F, 0x7F, 0x7F, 255},
    };
    return p;
}

Color seriesColor(std::size_t i) { return palette()[i % palette().size()]; }

const pdf::Font kSans{pdf::Family::Sans, false, false};
const pdf::Font kBold{pdf::Family::Sans, true, false};

double niceStep(double raw) {
    if (raw <= 0 || !std::isfinite(raw)) return 1;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / mag;
    double nice = f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10;
    return nice * mag;
}

std::string tickLabel(double v, double step) {
    int decimals = std::max(0, static_cast<int>(-std::floor(std::log10(step) + 1e-9)));
    if (step * std::pow(10.0, decimals) - std::floor(step * std::pow(10.0, decimals) + 1e-9) > 1e-6) ++decimals;
    decimals = std::min(decimals, 6);
    std::string fmt = "#,##0";
    if (decimals > 0) fmt += "." + std::string(static_cast<std::size_t>(decimals), '0');
    if (std::fabs(v) < step * 1e-9) v = 0;
    return formatValue(CellValue::num(v), fmt);
}

// Cuts text to fit maxWidth pixels, adding an ellipsis.
std::string fitText(const std::string& text, const pdf::Font& f, double size, double maxWidth) {
    if (pdf::textWidthPx(text, f, size) <= maxWidth) return text;
    auto cps = pdf::utf8Decode(text);
    while (!cps.empty()) {
        cps.pop_back();
        std::string s;
        for (unsigned c : cps) s += pdf::utf8Encode(c);
        s += "\xE2\x80\xA6";
        if (pdf::textWidthPx(s, f, size) <= maxWidth) return s;
    }
    return {};
}

void text(raster::Image& img, double x, double y, const std::string& s, const pdf::Font& f, double size, Color c, int align = 0) {
    if (align) {
        const double w = pdf::textWidthPx(s, f, size);
        x -= align == 1 ? w / 2 : w;
    }
    pdf::drawText(img, x, y, s, f, size, c);
}

struct Axis {
    double lo = 0, hi = 1, step = 0.2;
};

Axis valueAxis(const Chart& c, bool includeZero) {
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (const auto& s : c.series) {
        for (double v : s.values) {
            if (!std::isfinite(v)) continue;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (!std::isfinite(lo)) lo = 0, hi = 1;
    if (includeZero) {
        lo = std::min(lo, 0.0);
        hi = std::max(hi, 0.0);
    }
    if (hi - lo < 1e-12) {
        hi += std::max(1.0, std::fabs(hi) * 0.1);
        if (!includeZero) lo -= std::max(1.0, std::fabs(lo) * 0.1);
    }
    Axis a;
    a.step = niceStep((hi - lo) / 5);
    a.lo = std::floor(lo / a.step + 1e-9) * a.step;
    a.hi = std::ceil(hi / a.step - 1e-9) * a.step;
    if (a.hi - a.lo < a.step * 0.5) a.hi = a.lo + a.step;
    return a;
}

// Draws a legend row centred at y; returns its height.
double legendRow(raster::Image& img, const std::vector<std::pair<std::string, Color>>& entries, double y, double size) {
    double total = 0;
    std::vector<std::string> labels;
    for (const auto& e : entries) {
        std::string l = fitText(e.first, kSans, size, 180);
        labels.push_back(l);
        total += size + 6 + pdf::textWidthPx(l, kSans, size) + 18;
    }
    double x = std::max(10.0, (img.width() - total) / 2);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        raster::fillRect(img, x, y - size * 0.8, size * 0.8, size * 0.8, entries[i].second);
        x += size + 6;
        text(img, x - 4, y, labels[i], kSans, size, kInk);
        x += pdf::textWidthPx(labels[i], kSans, size) + 18;
    }
    return size + 12;
}

void drawPie(const Chart& c, raster::Image& img, double top) {
    const auto& values = c.series.front().values;
    double sum = 0;
    for (double v : values) {
        if (std::isfinite(v) && v > 0) sum += v;
    }
    const double W = img.width(), H = img.height();
    const double size = std::max(11.0, H / 34);
    const double legendW = std::min(W * 0.4, 260.0);
    const double r = std::max(10.0, std::min((W - legendW - 40) / 2, (H - top - 20) / 2));
    const double cx = 20 + r + std::max(0.0, (W - legendW - 40 - 2 * r) / 2), cy = top + (H - top) / 2;
    if (sum <= 0) {
        text(img, cx, cy, "no positive values", kSans, size, kMutedInk, 1);
        return;
    }
    double angle = -kPi / 2;
    std::size_t colorIndex = 0;
    std::vector<std::pair<std::string, Color>> legend;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double v = values[i];
        const Color col = seriesColor(colorIndex++);
        const std::string name = i < c.categories.size() ? c.categories[i] : std::to_string(i + 1);
        if (!std::isfinite(v) || v <= 0) continue;
        const double sweep = v / sum * 2 * kPi;
        std::vector<raster::Point> pts = {{cx, cy}};
        const int steps = std::max(2, static_cast<int>(sweep * r / 3));
        for (int k = 0; k <= steps; ++k) {
            double a = angle + sweep * k / steps;
            pts.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
        }
        raster::fillPolygon(img, pts, col);
        // white separators
        raster::drawLine(img, cx, cy, cx + r * std::cos(angle), cy + r * std::sin(angle), Color{255, 255, 255, 255}, 2);
        const double pct = v / sum * 100;
        if (pct >= 5) {
            const double mid = angle + sweep / 2;
            char buf[16];
            std::snprintf(buf, sizeof buf, "%.0f%%", pct);
            text(img, cx + r * 0.62 * std::cos(mid), cy + r * 0.62 * std::sin(mid) + size * 0.35, buf, kBold, size, Color{255, 255, 255, 255}, 1);
        }
        char buf[24];
        std::snprintf(buf, sizeof buf, " (%.1f%%)", pct);
        legend.push_back({name + buf, col});
        angle += sweep;
    }
    raster::drawLine(img, cx, cy, cx + r * std::cos(angle), cy + r * std::sin(angle), Color{255, 255, 255, 255}, 2);
    // legend on the right
    double ly = cy - legend.size() * (size + 8) / 2 + size;
    const double lx = cx + r + 30;
    for (const auto& [label, col] : legend) {
        if (ly > H - 4) break;
        raster::fillRect(img, lx, ly - size * 0.8, size * 0.8, size * 0.8, col);
        text(img, lx + size + 2, ly, fitText(label, kSans, size, W - lx - size - 10), kSans, size, kInk);
        ly += size + 8;
    }
}

}  // namespace

Chart chartFromRange(const Workbook& wb, const Range& range, bool byRows) {
    if (range.sheet < 0 || range.sheet >= static_cast<int>(wb.sheets.size())) throw std::runtime_error("no such sheet");
    const Sheet& sh = wb.sheets[static_cast<std::size_t>(range.sheet)];
    // clip whole-column/row ranges to the used area
    Range r = range;
    int ur1, uc1, ur2, uc2;
    if (!sh.bounds(ur1, uc1, ur2, uc2)) throw std::runtime_error("the range is empty");
    r.r2 = std::min(r.r2, ur2);
    r.c2 = std::min(r.c2, uc2);
    if (r.r1 > r.r2 || r.c1 > r.c2) throw std::runtime_error("the range is empty");
    const int N = byRows ? r.cols() : r.rows();  // records (categories)
    const int M = byRows ? r.rows() : r.cols();  // fields (series)
    auto cell = [&](int i, int j) -> const Cell* {
        return byRows ? sh.get(r.r1 + j, r.c1 + i) : sh.get(r.r1 + i, r.c1 + j);
    };
    auto isText = [&](int i, int j) {
        const Cell* c = cell(i, j);
        return c && c->value.type == CellValue::Text;
    };
    auto isDate = [&](int i, int j) {
        const Cell* c = cell(i, j);
        return c && c->value.type == CellValue::Number && isDateFormat(c->format);
    };
    bool header = false;
    for (int j = M >= 2 ? 1 : 0; j < M; ++j) header = header || isText(0, j);
    if (M == 1 && N > 1) header = isText(0, 0);
    const int i0 = header ? 1 : 0;
    bool labels = false;
    if (M >= 2) {
        for (int i = i0; i < N; ++i) labels = labels || isText(i, 0) || isDate(i, 0);
    }
    Chart c;
    const int j0 = labels ? 1 : 0;
    for (int i = i0; i < N && i - i0 < 1000; ++i) {
        if (labels) {
            const Cell* lc = cell(i, 0);
            c.categories.push_back(lc ? formatValue(lc->value, lc->format) : std::string());
        } else {
            c.categories.push_back(std::to_string(i - i0 + 1));
        }
    }
    for (int j = j0; j < M; ++j) {
        ChartSeries s;
        if (header) {
            const Cell* hc = cell(0, j);
            s.name = hc ? formatValue(hc->value, hc->format) : std::string();
        }
        if (s.name.empty()) s.name = "Series " + std::to_string(j - j0 + 1);
        bool any = false;
        for (int i = i0; i < N && i - i0 < 1000; ++i) {
            const Cell* vc = cell(i, j);
            if (vc && vc->value.type == CellValue::Number) {
                s.values.push_back(vc->value.number);
                any = true;
            } else {
                s.values.push_back(std::numeric_limits<double>::quiet_NaN());
            }
        }
        if (any) c.series.push_back(std::move(s));
    }
    if (c.series.empty()) throw std::runtime_error("no numbers to chart in " + rangeName(range));
    return c;
}

raster::Image renderChart(const Chart& c) {
    const int W = std::clamp(c.width, 120, 4000), H = std::clamp(c.height, 90, 4000);
    raster::Image img(W, H, Color{255, 255, 255, 255});
    const double size = std::clamp(H / 40.0, 10.0, 18.0);
    double top = 14;
    if (!c.title.empty()) {
        const double ts = size * 1.5;
        text(img, W / 2.0, top + ts, fitText(c.title, kBold, ts, W - 20), kBold, ts, kInk, 1);
        top += ts + 14;
    }
    if (c.series.empty()) return img;
    if (c.type == "pie") {
        drawPie(c, img, top);
        return img;
    }
    if (c.series.size() > 1) {
        std::vector<std::pair<std::string, Color>> entries;
        for (std::size_t i = 0; i < c.series.size(); ++i) entries.push_back({c.series[i].name, seriesColor(i)});
        top += legendRow(img, entries, top + size, size) + 4;
    }
    const bool horizontal = c.type == "hbar";
    const bool line = c.type == "line";
    const Axis ax = valueAxis(c, !line);
    std::size_t n = c.categories.size();
    for (const auto& s : c.series) n = std::max(n, s.values.size());
    if (n == 0) return img;
    auto catLabel = [&](std::size_t i) { return i < c.categories.size() ? c.categories[i] : std::to_string(i + 1); };

    // tick labels decide the margins
    std::vector<std::pair<double, std::string>> ticks;
    for (double v = ax.lo; v <= ax.hi + ax.step * 0.01; v += ax.step) ticks.push_back({v, tickLabel(v, ax.step)});
    double tickW = 0;
    for (const auto& t : ticks) tickW = std::max(tickW, pdf::textWidthPx(t.second, kSans, size));
    double catW = 0;
    for (std::size_t i = 0; i < n; ++i) catW = std::max(catW, pdf::textWidthPx(catLabel(i), kSans, size));

    double x0, x1 = W - 20, y0 = top + 6, y1 = H - (size * 2.2);
    if (horizontal) {
        x0 = std::min(W * 0.35, catW + 18);
        x1 = W - std::max(20.0, pdf::textWidthPx(ticks.back().second, kSans, size) / 2 + 6);
    } else {
        x0 = tickW + 16;
    }
    if (x1 - x0 < 20 || y1 - y0 < 20) return img;
    auto valuePos = [&](double v) {
        const double t = (v - ax.lo) / (ax.hi - ax.lo);
        return horizontal ? x0 + t * (x1 - x0) : y1 - t * (y1 - y0);
    };
    // grid and value labels
    for (const auto& [v, label] : ticks) {
        const double p = valuePos(v);
        if (horizontal) {
            raster::drawLine(img, p, y0, p, y1, kGrid, 1);
            text(img, p, y1 + size * 1.4, label, kSans, size, kMutedInk, 1);
        } else {
            raster::drawLine(img, x0, p, x1, p, kGrid, 1);
            text(img, x0 - 8, p + size * 0.35, label, kSans, size, kMutedInk, 2);
        }
    }
    const double zero = valuePos(std::clamp(0.0, ax.lo, ax.hi));
    const double span = horizontal ? (y1 - y0) : (x1 - x0);
    const double group = span / static_cast<double>(n);
    // category labels, thinned when they would overlap
    std::size_t every = 1;
    if (!horizontal) {
        double widest = 0;
        for (std::size_t i = 0; i < n; ++i) widest = std::max(widest, pdf::textWidthPx(catLabel(i), kSans, size) + 8);
        if (line || widest > group) every = static_cast<std::size_t>(std::ceil(std::min(widest, 160.0) / group));
        every = std::max<std::size_t>(1, every);
    } else {
        every = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(size * 1.3 / group)));
    }
    for (std::size_t i = 0; i < n; i += every) {
        const double mid = (horizontal ? y0 : x0) + group * (static_cast<double>(i) + 0.5);
        if (horizontal) {
            text(img, x0 - 8, mid + size * 0.35, fitText(catLabel(i), kSans, size, x0 - 12), kSans, size, kInk, 2);
        } else {
            const double room = group * static_cast<double>(every) - 4;
            text(img, mid, y1 + size * 1.4, fitText(catLabel(i), kSans, size, std::max(room, 30.0)), kSans, size, kInk, 1);
        }
    }
    const std::size_t ns = c.series.size();
    if (line) {
        for (std::size_t s = 0; s < ns; ++s) {
            const Color col = seriesColor(s);
            const auto& vals = c.series[s].values;
            double px = 0, py = 0;
            bool have = false;
            const double lw = std::max(2.0, size / 5);
            for (std::size_t i = 0; i < vals.size(); ++i) {
                if (!std::isfinite(vals[i])) {
                    have = false;
                    continue;
                }
                const double x = x0 + group * (static_cast<double>(i) + 0.5), y = valuePos(vals[i]);
                if (have) raster::drawLine(img, px, py, x, y, col, lw);
                px = x, py = y, have = true;
            }
            if (n <= 60) {
                for (std::size_t i = 0; i < vals.size(); ++i) {
                    if (!std::isfinite(vals[i])) continue;
                    raster::fillEllipse(img, x0 + group * (static_cast<double>(i) + 0.5), valuePos(vals[i]), lw * 1.6, lw * 1.6, col);
                }
            }
        }
    } else {
        const double inner = group * 0.78;
        const double bw = inner / static_cast<double>(ns);
        for (std::size_t s = 0; s < ns; ++s) {
            const Color col = seriesColor(s);
            const auto& vals = c.series[s].values;
            for (std::size_t i = 0; i < vals.size(); ++i) {
                if (!std::isfinite(vals[i])) continue;
                const double start = (horizontal ? y0 : x0) + group * static_cast<double>(i) + (group - inner) / 2 + bw * static_cast<double>(s);
                const double v = valuePos(std::clamp(vals[i], ax.lo, ax.hi));
                const double a = std::min(v, zero), b = std::max(v, zero);
                const double gap = bw > 6 ? 1 : 0;
                if (horizontal) raster::fillRect(img, a, start + gap, std::max(1.0, b - a), bw - 2 * gap, col);
                else raster::fillRect(img, start + gap, a, bw - 2 * gap, std::max(1.0, b - a), col);
            }
        }
    }
    // axis line at zero (or the bottom)
    if (horizontal) raster::drawLine(img, zero, y0, zero, y1, kAxis, 1);
    else raster::drawLine(img, x0, line ? y1 : zero, x1, line ? y1 : zero, kAxis, 1);
    return img;
}

// ---- sheet grid ----------------------------------------------------------------------

namespace {

const double kRowH = 15;
const double kFont = 9.5;

double colWidthPt(const Sheet& sh, int c) {
    auto it = sh.colWidths.find(c);
    const double chars = it == sh.colWidths.end() ? 8.43 : it->second;
    return std::max(6.0, (chars * 7 + 5) * 0.75);
}

void drawGrid(pdf::Page& page, const Sheet& sh, int r1, int c1, int r2, int c2, double ox, double oy, bool headers) {
    const pdf::Color line{0xD4, 0xD4, 0xD4, 255};
    const pdf::Color headBg{0xF2, 0xF2, 0xF2, 255};
    const pdf::Color ink{0x22, 0x22, 0x22, 255};
    const pdf::Font f{pdf::Family::Sans, false, false};
    const pdf::Font fb{pdf::Family::Sans, true, false};
    double rowHeadW = 0;
    if (headers) rowHeadW = std::max(26.0, pdf::textWidth(f, std::to_string(r2 + 1), kFont) + 10);
    std::vector<double> xs = {ox + rowHeadW};
    for (int c = c1; c <= c2; ++c) xs.push_back(xs.back() + colWidthPt(sh, c));
    const double top = oy + (headers ? kRowH : 0);
    const double bottom = top + (r2 - r1 + 1) * kRowH;
    if (headers) {
        page.rect(ox, oy, xs.back() - ox, kRowH, headBg);
        page.rect(ox, top, rowHeadW, bottom - top, headBg);
        for (int c = c1; c <= c2; ++c) {
            const std::string name = colName(c);
            const double cw = xs[static_cast<std::size_t>(c - c1) + 1] - xs[static_cast<std::size_t>(c - c1)];
            page.text(xs[static_cast<std::size_t>(c - c1)] + (cw - pdf::textWidth(f, name, kFont)) / 2, oy + kRowH - 4, name, f, kFont, {0x55, 0x55, 0x55, 255});
        }
        for (int r = r1; r <= r2; ++r) {
            const std::string num = std::to_string(r + 1);
            page.text(ox + rowHeadW - 5 - pdf::textWidth(f, num, kFont), top + (r - r1 + 1) * kRowH - 4, num, f, kFont, {0x55, 0x55, 0x55, 255});
        }
    }
    // gridlines
    for (int r = 0; r <= r2 - r1 + 1; ++r) page.line(headers ? ox : xs.front(), top + r * kRowH, xs.back(), top + r * kRowH, line, 0.5);
    if (headers) page.line(ox, oy, xs.back(), oy, line, 0.5);
    for (std::size_t i = 0; i < xs.size(); ++i) page.line(xs[i], headers ? oy : top, xs[i], bottom, line, 0.5);
    if (headers) page.line(ox, oy, ox, bottom, line, 0.5);
    // values
    for (int r = r1; r <= r2; ++r) {
        const double baseline = top + (r - r1 + 1) * kRowH - 4;
        for (int c = c1; c <= c2; ++c) {
            const Cell* cell = sh.get(r, c);
            if (!cell || cell->value.empty()) continue;
            std::string s = formatValue(cell->value, cell->format);
            if (s.empty()) continue;
            const pdf::Font& ff = cell->style.bold ? fb : f;
            const std::size_t ci = static_cast<std::size_t>(c - c1);
            const double x = xs[ci], w = xs[ci + 1] - xs[ci];
            pdf::Color col = ink;
            const bool number = cell->value.type == CellValue::Number;
            const bool right = number || cell->value.type == CellValue::Bool;
            const bool centre = cell->value.isError();
            if (centre) col = {0xC0, 0x00, 0x00, 255};
            double room = w - 6;
            if (!right && !centre) {
                // text spills over empty neighbours
                for (int n = c + 1; n <= c2; ++n) {
                    const Cell* nc = sh.get(r, n);
                    if (nc && !nc->value.empty()) break;
                    room += xs[static_cast<std::size_t>(n - c1) + 1] - xs[static_cast<std::size_t>(n - c1)];
                }
            }
            double tw = pdf::textWidth(ff, s, kFont);
            if (tw > room) {
                if (number) {
                    s = "###";
                    tw = pdf::textWidth(ff, s, kFont);
                } else {
                    auto cps = pdf::utf8Decode(s);
                    while (!cps.empty() && tw > room) {
                        cps.pop_back();
                        s.clear();
                        for (unsigned cp : cps) s += pdf::utf8Encode(cp);
                        tw = pdf::textWidth(ff, s, kFont);
                    }
                }
            }
            double tx = x + 3;
            if (right) tx = x + w - 3 - tw;
            else if (centre) tx = x + (w - tw) / 2;
            page.text(tx, baseline, s, ff, kFont, col);
        }
    }
}

}  // namespace

pdf::Page sheetGridPage(const Workbook& wb, const Range& range, bool headers) {
    const Sheet& sh = wb.sheets.at(static_cast<std::size_t>(range.sheet));
    pdf::Page page;
    double w = headers ? std::max(26.0, pdf::textWidth(pdf::Font{}, std::to_string(range.r2 + 1), kFont) + 10) : 0;
    for (int c = range.c1; c <= range.c2; ++c) w += colWidthPt(sh, c);
    page.width = w + 1;
    page.height = (range.rows() + (headers ? 1 : 0)) * kRowH + 1;
    page.rect(0, 0, page.width, page.height, pdf::Color{255, 255, 255, 255});
    drawGrid(page, sh, range.r1, range.c1, range.r2, range.c2, 0.5, 0.5, headers);
    return page;
}

std::vector<pdf::Page> layoutSheet(const Workbook& wb, int sheet) {
    const Sheet& sh = wb.sheets.at(static_cast<std::size_t>(sheet));
    std::vector<pdf::Page> pages;
    int r1, c1, r2, c2;
    const double W = 841.89, H = 595.28, margin = 36;
    if (!sh.bounds(r1, c1, r2, c2)) {
        pdf::Page p;
        p.width = W;
        p.height = H;
        pages.push_back(p);
        return pages;
    }
    r1 = 0;
    c1 = 0;
    // column groups that fit the page width
    std::vector<std::pair<int, int>> groups;
    int start = c1;
    double used = 0;
    for (int c = c1; c <= c2; ++c) {
        const double cw = std::min(colWidthPt(sh, c), W - 2 * margin);
        if (used + cw > W - 2 * margin && c > start) {
            groups.push_back({start, c - 1});
            start = c;
            used = 0;
        }
        used += cw;
    }
    groups.push_back({start, c2});
    const int rowsPerPage = std::max(1, static_cast<int>((H - 2 * margin - 14) / kRowH));
    for (const auto& [ga, gb] : groups) {
        for (int r = r1; r <= r2; r += rowsPerPage) {
            pdf::Page p;
            p.width = W;
            p.height = H;
            const int re = std::min(r2, r + rowsPerPage - 1);
            p.text(margin, margin - 8, sh.name + "  " + cellName(r, ga) + ":" + cellName(re, gb), pdf::Font{pdf::Family::Sans, true, false}, 8,
                   pdf::Color{0x77, 0x77, 0x77, 255});
            drawGrid(p, sh, r, ga, re, gb, margin, margin, false);
            pages.push_back(std::move(p));
        }
    }
    if (pages.size() > 1) {
        const std::size_t n = pages.size();
        for (std::size_t i = 0; i < n; ++i) {
            std::string label = std::to_string(i + 1) + " / " + std::to_string(n);
            pdf::Font f{pdf::Family::Sans, false, false};
            pages[i].text((W - pdf::textWidth(f, label, 8)) / 2, H - margin / 2, label, f, 8, pdf::Color{0x88, 0x88, 0x88, 255});
        }
    }
    return pages;
}

}  // namespace wetwaresuite::office
