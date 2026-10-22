// Image trace: a bitmap becomes filled paths. Pixels are sorted into colours
// (a threshold for black and white, k-means for more), each colour's pixel
// edges are followed into closed loops, and the loops are smoothed into
// curves by the same fitter vector.simplify uses.
#include <algorithm>
#include <array>
#include <cmath>

#include "internal.hpp"

namespace wetwaresuite::vector {

namespace {

struct Rgb {
    double r = 0, g = 0, b = 0;
};

double d2(const Rgb& a, const std::uint8_t* p) {
    const double dr = a.r - p[0], dg = a.g - p[1], db = a.b - p[2];
    return dr * dr + dg * dg + db * db;
}

// Loops around the pixels where mask is set, inside on the right (clockwise on screen), in pixel corners.
std::vector<std::vector<Point>> loops(const std::vector<std::uint8_t>& mask, int W, int H) {
    auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && mask[static_cast<std::size_t>(y) * W + x]; };
    struct E {
        int x0, y0, x1, y1;
        bool used = false;
    };
    std::vector<E> edges;
    const std::size_t V = static_cast<std::size_t>(W + 1) * (H + 1);
    std::vector<std::array<int, 2>> out(V, {-1, -1});
    auto addEdge = [&](int x0, int y0, int x1, int y1) {
        const int id = static_cast<int>(edges.size());
        edges.push_back({x0, y0, x1, y1});
        auto& slot = out[static_cast<std::size_t>(y0) * (W + 1) + x0];
        (slot[0] < 0 ? slot[0] : slot[1]) = id;
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!in(x, y)) continue;
            if (!in(x, y - 1)) addEdge(x, y, x + 1, y);
            if (!in(x + 1, y)) addEdge(x + 1, y, x + 1, y + 1);
            if (!in(x, y + 1)) addEdge(x + 1, y + 1, x, y + 1);
            if (!in(x - 1, y)) addEdge(x, y + 1, x, y);
        }
    }
    std::vector<std::vector<Point>> result;
    for (std::size_t start = 0; start < edges.size(); ++start) {
        if (edges[start].used) continue;
        std::vector<Point> loop;
        int cur = static_cast<int>(start);
        while (cur >= 0 && !edges[static_cast<std::size_t>(cur)].used) {
            E& e = edges[static_cast<std::size_t>(cur)];
            e.used = true;
            loop.push_back({static_cast<double>(e.x0), static_cast<double>(e.y0)});
            const auto& slot = out[static_cast<std::size_t>(e.y1) * (W + 1) + e.x1];
            int next = -1;
            if (slot[1] < 0) next = slot[0];
            else {  // two ways on (pixels touching at a corner): turn right, keeping the pieces apart
                const int dx = e.x1 - e.x0, dy = e.y1 - e.y0;
                for (int c : slot) {
                    const E& f = edges[static_cast<std::size_t>(c)];
                    const int fx = f.x1 - f.x0, fy = f.y1 - f.y0;
                    if (dx * fy - dy * fx > 0 && !f.used) next = c;
                }
                if (next < 0) next = edges[static_cast<std::size_t>(slot[0])].used ? slot[1] : slot[0];
            }
            cur = next;
        }
        // drop points in the middle of straight runs
        std::vector<Point> pts;
        const std::size_t n = loop.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Point a = loop[(i + n - 1) % n], p = loop[i], b = loop[(i + 1) % n];
            if ((p.x - a.x) * (b.y - p.y) - (p.y - a.y) * (b.x - p.x) != 0) pts.push_back(p);
        }
        if (pts.size() >= 3) result.push_back(std::move(pts));
    }
    return result;
}

// Pixel corners to a smooth outline: the middles of the edges (so staircases become straight
// slopes), plus the corners where two long straight edges meet (so squares stay square).
std::vector<Point> soften(const std::vector<Point>& pts) {
    const std::size_t n = pts.size();
    auto edgeLen = [&](std::size_t i) {
        const Point a = pts[i % n], b = pts[(i + 1) % n];
        return std::hypot(b.x - a.x, b.y - a.y);
    };
    std::vector<Point> out;
    for (std::size_t i = 0; i < n; ++i) {
        const Point a = pts[i], b = pts[(i + 1) % n];
        if (edgeLen(i) >= 3 && edgeLen(i + n - 1) >= 3) out.push_back(a);
        out.push_back({(a.x + b.x) / 2, (a.y + b.y) / 2});
    }
    return out;
}

}  // namespace

std::vector<TracedRegion> traceImage(const raster::Image& img, const TraceSettings& st) {
    const int W = img.width(), H = img.height();
    std::vector<TracedRegion> out;
    if (W == 0 || H == 0) return out;
    const std::size_t N = static_cast<std::size_t>(W) * H;
    std::vector<int> label(N, -1);
    std::vector<Rgb> palette;
    if (st.colors <= 2) {
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const std::uint8_t* p = img.pixel(x, y);
                const double lum = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                if (p[3] >= 128 && lum < st.threshold) {
                    label[static_cast<std::size_t>(y) * W + x] = 0;
                }
            }
        }
        palette.push_back(Rgb{0, 0, 0});  // black and white: the ink is black
    } else {
        // k-means on a sample, seeded far apart (k-means++ without the randomness: farthest first)
        std::vector<const std::uint8_t*> sample;
        const std::size_t step = std::max<std::size_t>(1, N / 20000);
        for (std::size_t i = 0; i < N; i += step) {
            const std::uint8_t* p = img.data() + i * 4;
            if (p[3] >= 128) sample.push_back(p);
        }
        if (sample.empty()) return out;
        const int k = std::clamp(st.colors, 2, 64);
        palette.push_back({static_cast<double>(sample[0][0]), static_cast<double>(sample[0][1]), static_cast<double>(sample[0][2])});
        std::vector<double> best(sample.size(), 1e300);
        while (static_cast<int>(palette.size()) < k) {
            std::size_t far = 0;
            for (std::size_t i = 0; i < sample.size(); ++i) {
                best[i] = std::min(best[i], d2(palette.back(), sample[i]));
                if (best[i] > best[far]) far = i;
            }
            if (best[far] < 1) break;  // fewer distinct colours than asked for
            palette.push_back({static_cast<double>(sample[far][0]), static_cast<double>(sample[far][1]), static_cast<double>(sample[far][2])});
        }
        for (int iter = 0; iter < 10; ++iter) {
            std::vector<Rgb> sum(palette.size());
            std::vector<double> cnt(palette.size(), 0);
            for (const auto* p : sample) {
                std::size_t bi = 0;
                for (std::size_t c = 1; c < palette.size(); ++c) {
                    if (d2(palette[c], p) < d2(palette[bi], p)) bi = c;
                }
                sum[bi].r += p[0], sum[bi].g += p[1], sum[bi].b += p[2];
                ++cnt[bi];
            }
            for (std::size_t c = 0; c < palette.size(); ++c) {
                if (cnt[c] > 0) palette[c] = {sum[c].r / cnt[c], sum[c].g / cnt[c], sum[c].b / cnt[c]};
            }
        }
        for (std::size_t i = 0; i < N; ++i) {
            const std::uint8_t* p = img.data() + i * 4;
            if (p[3] < 128) continue;
            std::size_t bi = 0;
            for (std::size_t c = 1; c < palette.size(); ++c) {
                if (d2(palette[c], p) < d2(palette[bi], p)) bi = c;
            }
            label[i] = static_cast<int>(bi);
        }
    }
    std::vector<std::uint8_t> mask(N);
    for (std::size_t c = 0; c < palette.size(); ++c) {
        const Rgb& col = palette[c];
        if (st.ignoreWhite && st.colors > 2 && col.r > 235 && col.g > 235 && col.b > 235) continue;
        std::size_t count = 0;
        for (std::size_t i = 0; i < N; ++i) {
            mask[i] = label[i] == static_cast<int>(c);
            count += mask[i];
        }
        if (!count) continue;
        TracedRegion region;
        region.color = {static_cast<std::uint8_t>(std::lround(col.r)), static_cast<std::uint8_t>(std::lround(col.g)), static_cast<std::uint8_t>(std::lround(col.b)), 255};
        BezierPath path;
        for (auto& loop : loops(mask, W, H)) {
            const double area = std::abs(signedArea(loop));
            if (area < st.minArea) continue;
            region.area += signedArea(loop);
            if (st.smooth <= 0) {
                SubPath sp;
                sp.closed = true;
                for (const auto& p : loop) sp.nodes.push_back({p, p, p});
                path.push_back(std::move(sp));
                continue;
            }
            const std::vector<Point> soft = soften(loop);
            const double corner = 135 - 65 * std::clamp(st.smooth, 0.0, 1.0);  // smoother: only sharp turns stay corners
            path.push_back(simplify(soft, true, std::max(0.1, st.tolerance), corner));
        }
        if (path.empty()) continue;
        region.area = std::abs(region.area);
        region.d = pathData(path);
        out.push_back(std::move(region));
    }
    std::stable_sort(out.begin(), out.end(), [](const TracedRegion& a, const TracedRegion& b) { return a.area > b.area; });
    return out;
}

}  // namespace wetwaresuite::vector
