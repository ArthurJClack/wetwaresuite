// A tiny parallel-for over rows for the pixel loops. Each row is handled by
// exactly one thread running the same code, so the output never depends on
// how many threads ran it.
#pragma once

#include <cstddef>
#include <functional>

namespace wetwaresuite::raster {

// Threads used by parallelRows: 0 (the default) means one per CPU core. Returns the previous setting.
int setThreadCount(int threads);
int threadCount();

// Runs fn(begin, end) over contiguous slices of [0, rows). `costPerRow` is roughly the work in one
// row (e.g. its pixel count); small jobs run inline on the calling thread to skip thread start-up.
void parallelRows(int rows, std::size_t costPerRow, const std::function<void(int begin, int end)>& fn);

}  // namespace wetwaresuite::raster
