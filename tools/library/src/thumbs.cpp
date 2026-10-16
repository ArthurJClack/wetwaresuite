// Thumbnails for every format the suite can draw (images, SVG, meshes) or
// that a helper program can (video frames and audio waveforms with ffmpeg,
// PDF pages with pdftoppm), perceptual hashes for finding similar images,
// and contact sheets.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <thread>

#include "achclwss/core/process.hpp"
#include "achclwss/library/library.hpp"
#include "achclwss/mesh/io.hpp"
#include "achclwss/mesh/render.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/document.hpp"
#include "achclwss/raster/ops.hpp"
#include "achclwss/vector/vector.hpp"

namespace achclwss::library {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Luma of an image scaled to w x h, row by row.
std::vector<double> grey(const raster::Image& img, int w, int h) {
    raster::Image small = raster::resize(img, w, h, raster::Filter::Bilinear);
    std::vector<double> out(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto* p = small.pixel(x, y);
            // transparent pixels count as white so cut-outs compare by shape
            double a = p[3] / 255.0;
            double l = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            out[static_cast<std::size_t>(y) * w + x] = l * a + 255.0 * (1 - a);
        }
    }
    return out;
}

raster::Image fit(const raster::Image& img, int size) {
    if (img.empty()) return img;
    if (img.width() <= size && img.height() <= size) return img;
    double s = std::min(static_cast<double>(size) / img.width(), static_cast<double>(size) / img.height());
    int w = std::max(1, static_cast<int>(std::lround(img.width() * s)));
    int h = std::max(1, static_cast<int>(std::lround(img.height() * s)));
    return raster::resize(img, w, h, raster::Filter::Bilinear);
}

fs::path tempFile(const std::string& ext) {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    std::string name = "achclwss-thumb-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000000) +
                       "-" + std::to_string(counter++) + ext;
    return dir / name;
}

raster::Image loadAndRemove(const fs::path& file) {
    raster::Image img;
    std::error_code ec;
    if (fs::exists(file, ec)) {
        try {
            img = raster::load(file);
        } catch (const std::exception&) {
        }
    }
    fs::remove(file, ec);
    return img;
}

raster::Image ffmpegFrame(const fs::path& path, int size, bool audio, std::string* why) {
    static const auto exe = findProgram("ffmpeg");
    if (!exe) {
        if (why) *why = "needs ffmpeg on PATH";
        return {};
    }
    fs::path out = tempFile(".png");
    const std::string s = std::to_string(size);
    if (audio) {
        runProgram({exe->string(), "-v", "error", "-y", "-i", path.string(), "-filter_complex",
                    "showwavespic=s=" + s + "x" + std::to_string(size / 2) + ":colors=#4a90d9", "-frames:v", "1", out.string()});
        auto img = loadAndRemove(out);
        if (img.empty() && why) *why = "ffmpeg couldn't draw its waveform";
        return img;
    }
    const std::string scale = "scale=" + s + ":" + s + ":force_original_aspect_ratio=decrease";
    // a second in skips black lead-ins; short clips and stills fall back to the first frame
    runProgram({exe->string(), "-v", "error", "-y", "-ss", "1", "-i", path.string(), "-frames:v", "1", "-vf", scale, out.string()});
    auto img = loadAndRemove(out);
    if (img.empty()) {
        runProgram({exe->string(), "-v", "error", "-y", "-i", path.string(), "-frames:v", "1", "-vf", scale, out.string()});
        img = loadAndRemove(out);
    }
    if (img.empty() && why) *why = "ffmpeg couldn't read a frame";
    return img;
}

raster::Image pdfPage(const fs::path& path, int size, std::string* why) {
    static const auto exe = findProgram("pdftoppm");
    if (!exe) {
        if (why) *why = "needs pdftoppm (poppler) on PATH";
        return {};
    }
    fs::path prefix = tempFile("");
    runProgram({exe->string(), "-png", "-f", "1", "-l", "1", "-scale-to", std::to_string(size), "-singlefile", path.string(),
                prefix.string()});
    fs::path out = prefix;
    out += ".png";
    auto img = loadAndRemove(out);
    if (img.empty() && why) *why = "pdftoppm couldn't render it";
    return img;
}

// Draws a small number (3x5 pixel digits, scaled) on a dark box.
void drawNumber(raster::Image& img, int x0, int y0, int number, int scale) {
    static const char* digits[10] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111",
                                     "101101111001001", "111100111001111", "111100111101111", "111001001001001",
                                     "111101111101111", "111101111001111"};
    const std::string text = std::to_string(number);
    const int w = static_cast<int>(text.size()) * 4 * scale + scale, h = 7 * scale;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (img.contains(x0 + x, y0 + y)) img.set(x0 + x, y0 + y, {20, 20, 24, 255});
        }
    }
    for (std::size_t c = 0; c < text.size(); ++c) {
        const char* g = digits[text[c] - '0'];
        for (int gy = 0; gy < 5; ++gy) {
            for (int gx = 0; gx < 3; ++gx) {
                if (g[gy * 3 + gx] != '1') continue;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        int px = x0 + scale + static_cast<int>(c) * 4 * scale + gx * scale + sx, py = y0 + scale + gy * scale + sy;
                        if (img.contains(px, py)) img.set(px, py, {255, 255, 255, 255});
                    }
                }
            }
        }
    }
}

}  // namespace

std::uint64_t dHash(const raster::Image& img) {
    auto g = grey(img, 9, 8);
    std::uint64_t h = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            h = (h << 1) | (g[static_cast<std::size_t>(y) * 9 + x] < g[static_cast<std::size_t>(y) * 9 + x + 1] ? 1u : 0u);
        }
    }
    return h;
}

std::uint64_t pHash(const raster::Image& img) {
    constexpr int N = 32;
    auto g = grey(img, N, N);
    static const auto table = [] {
        std::vector<double> t(static_cast<std::size_t>(8) * N);
        for (int u = 0; u < 8; ++u) {
            for (int x = 0; x < N; ++x) t[static_cast<std::size_t>(u) * N + x] = std::cos((2 * x + 1) * u * kPi / (2 * N));
        }
        return t;
    }();
    // 2D DCT, only the 8x8 lowest frequencies
    std::vector<double> rows(static_cast<std::size_t>(N) * 8);  // per row y, coefficient u
    for (int y = 0; y < N; ++y) {
        for (int u = 0; u < 8; ++u) {
            double s = 0;
            for (int x = 0; x < N; ++x) s += g[static_cast<std::size_t>(y) * N + x] * table[static_cast<std::size_t>(u) * N + x];
            rows[static_cast<std::size_t>(y) * 8 + u] = s;
        }
    }
    double coeff[64];
    for (int v = 0; v < 8; ++v) {
        for (int u = 0; u < 8; ++u) {
            double s = 0;
            for (int y = 0; y < N; ++y) s += rows[static_cast<std::size_t>(y) * 8 + u] * table[static_cast<std::size_t>(v) * N + y];
            coeff[v * 8 + u] = s;
        }
    }
    std::vector<double> ac(coeff + 1, coeff + 64);
    std::nth_element(ac.begin(), ac.begin() + 31, ac.end());
    const double median = ac[31];
    std::uint64_t h = 0;
    for (int i = 0; i < 64; ++i) h = (h << 1) | (coeff[i] > median ? 1u : 0u);
    return h;
}

int hammingDistance(std::uint64_t a, std::uint64_t b) {
    std::uint64_t x = a ^ b;
    int n = 0;
    while (x) {
        x &= x - 1;
        ++n;
    }
    return n;
}

raster::Image renderThumbnail(const fs::path& path, const std::string& type, const std::string& ext, int size, std::string* why) {
    try {
        if (ext == "svg") {
            auto doc = vector::openSvg(path);
            double s = std::min(static_cast<double>(size) / std::max(1, doc->width), static_cast<double>(size) / std::max(1, doc->height));
            return fit(vector::render(*doc, s), size);
        }
        if (ext == "ora") return fit(raster::openImage(path)->flatten(), size);
        if (raster::canRead(path)) return fit(raster::load(path), size);
        if (mesh::canRead(path)) {
            auto m = mesh::load(path);
            if (m.empty()) {
                if (why) *why = "the mesh is empty";
                return {};
            }
            return mesh::render(m, size, size);
        }
    } catch (const std::exception& e) {
        if (why) *why = e.what();
        return {};
    }
    if (type == "video" || type == "image" || type == "raw" || type == "texture") return ffmpegFrame(path, size, false, why);
    if (type == "audio") return ffmpegFrame(path, size, true, why);
    if (ext == "pdf") return pdfPage(path, size, why);
    if (why) *why = "no thumbnailer for " + (type.empty() ? ext : type) + " files";
    return {};
}

fs::path thumbPath(const Catalog& catalog, const Item& item, int size) {
    fs::path dir = catalog.file.parent_path();
    if (dir.empty()) dir = ".";
    return dir / kThumbDir / (item.hash + "-" + std::to_string(size) + ".png");
}

raster::Image contactSheet(const std::vector<raster::Image>& thumbs, int cell, int columns) {
    if (thumbs.empty()) return {};
    columns = std::max(1, std::min(columns, static_cast<int>(thumbs.size())));
    const int pad = std::max(4, cell / 32);
    const int rows = (static_cast<int>(thumbs.size()) + columns - 1) / columns;
    raster::Image sheet(columns * (cell + pad) + pad, rows * (cell + pad) + pad, {48, 50, 56, 255});
    const int scale = std::max(1, cell / 96);
    for (std::size_t i = 0; i < thumbs.size(); ++i) {
        const int cx = pad + static_cast<int>(i % static_cast<std::size_t>(columns)) * (cell + pad);
        const int cy = pad + static_cast<int>(i / static_cast<std::size_t>(columns)) * (cell + pad);
        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < cell; ++x) sheet.set(cx + x, cy + y, {64, 66, 74, 255});
        }
        const raster::Image t = fit(thumbs[i], cell);
        if (!t.empty()) raster::blendOnto(sheet, t, cx + (cell - t.width()) / 2, cy + (cell - t.height()) / 2, raster::Blend::Normal, 1.0);
        drawNumber(sheet, cx, cy, static_cast<int>(i) + 1, scale);
    }
    return sheet;
}

}  // namespace achclwss::library
