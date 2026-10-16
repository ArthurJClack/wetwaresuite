#include "achclwss/raster/codec.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "achclwss/raster/ops.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8
#include "stb_image.h"
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace achclwss::raster {

namespace {

std::string ext(const std::filesystem::path& path) {
    std::string e = path.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

Image fromStb(unsigned char* data, int w, int h) {
    if (!data) throw std::runtime_error(std::string("cannot decode image: ") + stbi_failure_reason());
    Image img(w, h);
    std::memcpy(img.data(), data, img.bytes());
    stbi_image_free(data);
    return img;
}

void appendBytes(void* context, void* data, int size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("failed writing " + path.string());
}

// Binary PPM: stb can read it but not write it.
std::string encodePpm(const Image& img) {
    std::string out = "P6\n" + std::to_string(img.width()) + " " + std::to_string(img.height()) + "\n255\n";
    out.reserve(out.size() + static_cast<std::size_t>(img.width()) * img.height() * 3);
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) out.append(reinterpret_cast<const char*>(img.pixel(x, y)), 3);
    }
    return out;
}

}  // namespace

const std::vector<std::string>& readableExtensions() {
    static const std::vector<std::string> list = {"png", "jpg", "jpeg", "bmp", "tga", "gif",
                                                  "psd", "hdr", "pic", "ppm", "pgm", "pnm"};
    return list;
}

const std::vector<std::string>& writableExtensions() {
    static const std::vector<std::string> list = {"png", "jpg", "jpeg", "bmp", "tga", "ppm"};
    return list;
}

bool canRead(const std::filesystem::path& path) { return contains(readableExtensions(), ext(path)); }
bool canWrite(const std::filesystem::path& path) { return contains(writableExtensions(), ext(path)); }

Image decode(const std::string& bytes) {
    int w = 0, h = 0, n = 0;
    auto* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                                       &w, &h, &n, 4);
    return fromStb(data, w, h);
}

Image load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) throw std::runtime_error("file not found: " + path.string());
    return decode(readFile(path));
}

std::string encodePng(const Image& img) {
    std::string out;
    if (!stbi_write_png_to_func(appendBytes, &out, img.width(), img.height(), 4, img.data(), img.width() * 4)) {
        throw std::runtime_error("PNG encoding failed");
    }
    return out;
}

std::string encodeJpeg(const Image& img, int quality) {
    // JPEG has no alpha: flatten onto white first
    Image flat(img.width(), img.height(), {255, 255, 255, 255});
    blendOnto(flat, img, 0, 0, Blend::Normal, 1.0);
    std::string out;
    if (!stbi_write_jpg_to_func(appendBytes, &out, flat.width(), flat.height(), 4, flat.data(), std::clamp(quality, 1, 100))) {
        throw std::runtime_error("JPEG encoding failed");
    }
    return out;
}

void save(const Image& img, const std::filesystem::path& path, int quality) {
    if (img.empty()) throw std::runtime_error("image is empty");
    const std::string e = ext(path);
    std::string bytes;
    if (e == "png") {
        bytes = encodePng(img);
    } else if (e == "jpg" || e == "jpeg") {
        bytes = encodeJpeg(img, quality);
    } else if (e == "bmp") {
        if (!stbi_write_bmp_to_func(appendBytes, &bytes, img.width(), img.height(), 4, img.data())) {
            throw std::runtime_error("BMP encoding failed");
        }
    } else if (e == "tga") {
        if (!stbi_write_tga_to_func(appendBytes, &bytes, img.width(), img.height(), 4, img.data())) {
            throw std::runtime_error("TGA encoding failed");
        }
    } else if (e == "ppm") {
        bytes = encodePpm(img);
    } else {
        throw std::runtime_error("can't write ." + e + " images yet (supported: png, jpg, bmp, tga, ppm, ora)");
    }
    writeFile(path, bytes);
}

std::string previewPng(const Image& img, int maxSize) {
    if (img.width() <= maxSize && img.height() <= maxSize) return encodePng(img);
    double scale = static_cast<double>(maxSize) / std::max(img.width(), img.height());
    int w = std::max(1, static_cast<int>(img.width() * scale));
    int h = std::max(1, static_cast<int>(img.height() * scale));
    return encodePng(resize(img, w, h, Filter::Bilinear));
}

}  // namespace achclwss::raster
