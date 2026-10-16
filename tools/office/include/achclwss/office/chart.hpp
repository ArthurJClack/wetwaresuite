// Charts (bar, line, pie) drawn straight to PNG pixels, and the sheet grid
// as pages for previews and PDF export.
#pragma once

#include <string>
#include <vector>

#include "achclwss/office/sheet.hpp"
#include "achclwss/pdf/page.hpp"
#include "achclwss/raster/image.hpp"

namespace achclwss::office {

struct ChartSeries {
    std::string name;
    std::vector<double> values;  // NaN marks a missing point
};

struct Chart {
    std::string type = "bar";  // bar, hbar, line, pie
    std::string title;
    std::vector<std::string> categories;
    std::vector<ChartSeries> series;
    int width = 800, height = 500;  // pixels
};

const std::vector<std::string>& chartTypes();

// Reads a range laid out the usual way: an optional header row of series names and an
// optional first column of category labels (both detected from text cells).
// Columns are series unless byRows is set. Throws std::runtime_error when there are no numbers.
Chart chartFromRange(const Workbook& wb, const Range& range, bool byRows = false);

raster::Image renderChart(const Chart& chart);

// The cells of a range as a grid page in points: header letters and row numbers when
// `headers` is set, gridlines, formatted values (numbers right-aligned), bold cells.
pdf::Page sheetGridPage(const Workbook& wb, const Range& range, bool headers);
// The used range of a sheet split over pages (A4 landscape) for PDF export.
std::vector<pdf::Page> layoutSheet(const Workbook& wb, int sheet);

}  // namespace achclwss::office
