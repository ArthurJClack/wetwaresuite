// Function graphs: sampled curves drawn with axes, a grid, labels and a legend.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/calc/calc.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::calc {

namespace {

// 1, 2 or 5 times a power of ten, so about `count` lines fit
double niceStep(double span, int count) {
    if (span <= 0 || !std::isfinite(span)) return 1;
    double raw = span / count;
    double p = std::pow(10.0, std::floor(std::log10(raw)));
    double m = raw / p;
    return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
}

const raster::Color kCurveColours[] = {{37, 99, 235, 255}, {220, 38, 38, 255}, {22, 163, 74, 255}, {217, 119, 6, 255},
                                       {147, 51, 234, 255}, {8, 145, 178, 255}, {219, 39, 119, 255}, {75, 85, 99, 255}};

}  // namespace

std::vector<Curve> sampleCurves(const State& state, const std::vector<std::string>& expressions, const GraphOptions& o, int samples) {
    samples = std::clamp(samples, 2, 100000);
    std::vector<Curve> out;
    for (const auto& e : expressions) {
        Compiled c(state, e);  // throws on a syntax error, so mistakes show
        Curve curve;
        curve.expression = e;
        auto names = c.names();
        std::string var = "x";
        if (std::find(names.begin(), names.end(), "x") == names.end() && names.size() == 1) var = names[0];
        for (int i = 0; i < samples; ++i) {
            double x = o.xmin + (o.xmax - o.xmin) * i / (samples - 1);
            double y = c.at(var, x);
            curve.xs.push_back(x);
            curve.ys.push_back(std::isfinite(y) ? y : NAN);
        }
        out.push_back(std::move(curve));
    }
    return out;
}

std::string graphPng(const std::vector<Curve>& curves, const GraphOptions& o) {
    const int W = std::clamp(o.width, 64, 8000), H = std::clamp(o.height, 64, 8000);
    const raster::Color bg = o.dark ? raster::Color{22, 24, 28, 255} : raster::Color{255, 255, 255, 255};
    const raster::Color gridC = o.dark ? raster::Color{50, 54, 62, 255} : raster::Color{228, 230, 234, 255};
    const raster::Color axisC = o.dark ? raster::Color{170, 175, 185, 255} : raster::Color{90, 95, 105, 255};
    const raster::Color textC = o.dark ? raster::Color{220, 222, 228, 255} : raster::Color{40, 44, 52, 255};
    raster::Image img(W, H, bg);

    double ymin = o.ymin, ymax = o.ymax;
    if (ymin == 0 && ymax == 0) {
        // fit, ignoring the far tails near poles (tan x) so the shape stays visible
        std::vector<double> ys;
        for (const auto& c : curves) {
            for (double y : c.ys) {
                if (std::isfinite(y)) ys.push_back(y);
            }
        }
        if (ys.empty()) {
            ymin = -10;
            ymax = 10;
        } else {
            std::sort(ys.begin(), ys.end());
            ymin = ys[static_cast<std::size_t>(0.02 * static_cast<double>(ys.size() - 1))];
            ymax = ys[static_cast<std::size_t>(0.98 * static_cast<double>(ys.size() - 1))];
            if (ys.size() < 50) {
                ymin = ys.front();
                ymax = ys.back();
            }
            if (ymax - ymin < 1e-12) {
                ymin -= 1;
                ymax += 1;
            }
            double pad = (ymax - ymin) * 0.08;
            ymin -= pad;
            ymax += pad;
        }
    }
    const double xmin = o.xmin, xmax = o.xmax;
    const double left = 56, right = W - 16.0, top = 16, bottom = H - 34.0;
    auto sx = [&](double x) { return left + (x - xmin) / (xmax - xmin) * (right - left); };
    auto sy = [&](double y) { return bottom - (y - ymin) / (ymax - ymin) * (bottom - top); };
    const pdf::Font font{pdf::Family::Sans, false, false};
    const double fontPx = std::clamp(H / 40.0, 11.0, 18.0);

    const double xs = niceStep(xmax - xmin, std::max(2, W / 90)), ys = niceStep(ymax - ymin, std::max(2, H / 60));
    for (double x = std::ceil(xmin / xs) * xs; x <= xmax + xs * 1e-9; x += xs) {
        double px = sx(x);
        if (o.grid) raster::drawLine(img, px, top, px, bottom, gridC, 1);
        std::string label = formatNumber(std::fabs(x) < xs * 1e-9 ? 0 : x, 6);
        double tw = pdf::textWidthPx(label, font, fontPx);
        pdf::drawText(img, px - tw / 2, bottom + fontPx + 6, label, font, fontPx, textC);
    }
    for (double y = std::ceil(ymin / ys) * ys; y <= ymax + ys * 1e-9; y += ys) {
        double py = sy(y);
        if (o.grid) raster::drawLine(img, left, py, right, py, gridC, 1);
        std::string label = formatNumber(std::fabs(y) < ys * 1e-9 ? 0 : y, 6);
        double tw = pdf::textWidthPx(label, font, fontPx);
        pdf::drawText(img, left - tw - 6, py + fontPx * 0.35, label, font, fontPx, textC);
    }
    raster::strokeRect(img, left, top, right - left, bottom - top, axisC, 1);
    if (xmin < 0 && xmax > 0) raster::drawLine(img, sx(0), top, sx(0), bottom, axisC, 1.5);
    if (ymin < 0 && ymax > 0) raster::drawLine(img, left, sy(0), right, sy(0), axisC, 1.5);

    for (std::size_t c = 0; c < curves.size(); ++c) {
        const raster::Color col = kCurveColours[c % 8];
        const Curve& cv = curves[c];
        for (std::size_t i = 1; i < cv.xs.size(); ++i) {
            double y0 = cv.ys[i - 1], y1 = cv.ys[i];
            if (!std::isfinite(y0) || !std::isfinite(y1)) continue;
            double p0 = sy(y0), p1 = sy(y1);
            // a jump across most of the view is a pole, not a steep line
            if (std::fabs(p1 - p0) > (bottom - top) * 1.5) continue;
            if ((p0 < top - 2000 && p1 < top - 2000) || (p0 > bottom + 2000 && p1 > bottom + 2000)) continue;
            p0 = std::clamp(p0, top - 2000, bottom + 2000);
            p1 = std::clamp(p1, top - 2000, bottom + 2000);
            raster::drawLine(img, sx(cv.xs[i - 1]), p0, sx(cv.xs[i]), p1, col, 2.5);
        }
    }
    // keep the curves inside the frame: repaint the margins
    raster::fillRect(img, 0, 0, W, top, bg);
    raster::fillRect(img, 0, bottom, W, H - bottom, bg);
    raster::fillRect(img, 0, 0, left, H, bg);
    raster::fillRect(img, right, 0, W - right, H, bg);
    raster::strokeRect(img, left, top, right - left, bottom - top, axisC, 1);
    for (double x = std::ceil(xmin / xs) * xs; x <= xmax + xs * 1e-9; x += xs) {
        std::string label = formatNumber(std::fabs(x) < xs * 1e-9 ? 0 : x, 6);
        double tw = pdf::textWidthPx(label, font, fontPx);
        pdf::drawText(img, sx(x) - tw / 2, bottom + fontPx + 6, label, font, fontPx, textC);
    }
    for (double y = std::ceil(ymin / ys) * ys; y <= ymax + ys * 1e-9; y += ys) {
        std::string label = formatNumber(std::fabs(y) < ys * 1e-9 ? 0 : y, 6);
        double tw = pdf::textWidthPx(label, font, fontPx);
        pdf::drawText(img, left - tw - 6, sy(y) + fontPx * 0.35, label, font, fontPx, textC);
    }
    // legend
    double ly = top + 8;
    for (std::size_t c = 0; c < curves.size(); ++c) {
        std::string label = "y = " + curves[c].expression;
        double tw = pdf::textWidthPx(label, font, fontPx);
        raster::fillRect(img, left + 8, ly, tw + 40, fontPx + 8, o.dark ? raster::Color{22, 24, 28, 220} : raster::Color{255, 255, 255, 220});
        raster::drawLine(img, left + 14, ly + fontPx / 2 + 4, left + 30, ly + fontPx / 2 + 4, kCurveColours[c % 8], 3);
        pdf::drawText(img, left + 36, ly + fontPx, label, font, fontPx, textC);
        ly += fontPx + 10;
    }
    return raster::encodePng(img);
}

}  // namespace wetwaresuite::calc
