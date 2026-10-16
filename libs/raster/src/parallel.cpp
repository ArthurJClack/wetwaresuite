#include "wetwaresuite/raster/parallel.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <thread>
#include <vector>

namespace wetwaresuite::raster {

namespace {

std::atomic<int> gThreads{0};

}  // namespace

int setThreadCount(int threads) { return gThreads.exchange(std::max(0, threads)); }

int threadCount() {
    int n = gThreads.load();
    if (n > 0) return n;
    return static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 64u));
}

void parallelRows(int rows, std::size_t costPerRow, const std::function<void(int, int)>& fn) {
    if (rows <= 0) return;
    constexpr std::size_t kMinWork = 1 << 15;  // ~32k pixels per thread is worth a thread
    const std::size_t total = static_cast<std::size_t>(rows) * std::max<std::size_t>(1, costPerRow);
    int threads = std::min(threadCount(), rows);
    threads = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(threads), std::max<std::size_t>(1, total / kMinWork)));
    if (threads <= 1) {
        fn(0, rows);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads) - 1);
    std::exception_ptr error;
    std::atomic<bool> failed{false};
    auto slice = [&](int t) {
        const int b = static_cast<int>(static_cast<long long>(rows) * t / threads);
        const int e = static_cast<int>(static_cast<long long>(rows) * (t + 1) / threads);
        try {
            if (b < e) fn(b, e);
        } catch (...) {
            if (!failed.exchange(true)) error = std::current_exception();
        }
    };
    for (int t = 1; t < threads; ++t) pool.emplace_back(slice, t);
    slice(0);
    for (auto& th : pool) th.join();
    if (error) std::rethrow_exception(error);
}

}  // namespace wetwaresuite::raster
