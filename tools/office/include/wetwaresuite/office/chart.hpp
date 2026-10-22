// Charts (bar, line, area, pie, scatter) drawn straight to PNG pixels, and the
// sheet grid (styles, merges, conditional formats, in-sheet charts) as pages
// for previews and PDF export.
#pragma once

#include <string>
#include <vector>

#include "wetwaresuite/office/sheet.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::office {

struct ChartSeries {
    std::string name;
    std::vector<double> values;  // NaN marks a missing point
    int field = -1;              // the column (row when byRows) of the source range it came from
};

struct Chart {
    std::string type = "bar";  // bar, hbar, line, area, pie, scatter (the first series gives the x values)
    std::string title;
    std::vector<std::string> categories;
    std::vector<ChartSeries> series;
    int width = 800, height = 500;  // pixels
};

const std::vector<std::string>& chartTypes();

// How a range is read for a chart: the range clipped to the used area, whether its first row
// (column when byRows) holds series names and its first column (row) category labels.
struct ChartLayout {
    Range range;
    bool byRows = false, header = false, labels = false;
};
ChartLayout chartLayout(const Workbook& wb, const Range& range, bool byRows = false);

// Reads a range laid out the usual way: an optional header row of series names and an
// optional first column of category labels (both detected from text cells).
// Columns are series unless byRows is set. Throws std::runtime_error when there are no numbers.
Chart chartFromRange(const Workbook& wb, const Range& range, bool byRows = false);
// A chart that sits in a sheet, read from its data range. Throws std::runtime_error.
Chart sheetChart(const Workbook& wb, int sheet, const SheetChart& chart);

raster::Image renderChart(const Chart& chart);

// The cells of a range as a grid page in points: header letters and row numbers when
// `headers` is set, gridlines, formatted values, cell styles, merges and conditional formats,
// and the sheet's charts that sit inside the range.
pdf::Page sheetGridPage(const Workbook& wb, const Range& range, bool headers);
// The used range of a sheet split over pages (A4 landscape) for PDF export.
std::vector<pdf::Page> layoutSheet(const Workbook& wb, int sheet);

}  // namespace wetwaresuite::office
