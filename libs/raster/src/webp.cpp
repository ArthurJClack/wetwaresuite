// WebP through libwebp, when the build has it (see cmake/webp.cmake).
#include <cstring>
#include <stdexcept>
#include <string>

#include "wetwaresuite/raster/codec.hpp"

#if WETWARESUITE_HAS_WEBP
#include "webp/decode.h"
#include "webp/encode.h"
#endif

namespace wetwaresuite::raster {

#if WETWARESUITE_HAS_WEBP

bool webpSupported() { return true; }

Image decodeWebp(const std::string& bytes) {
    int w = 0, h = 0;
    uint8_t* data = WebPDecodeRGBA(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &w, &h);
    if (!data) throw std::runtime_error("WebP: cannot decode image (animated WebP isn't supported)");
    Image img(w, h);
    std::memcpy(img.data(), data, img.bytes());
    WebPFree(data);
    return img;
}

std::string encodeWebp(const Image& img, int quality) {
    if (img.empty()) throw std::runtime_error("image is empty");
    uint8_t* out = nullptr;
    const int stride = img.width() * 4;
    const std::size_t n = quality >= 100 ? WebPEncodeLosslessRGBA(img.data(), img.width(), img.height(), stride, &out)
                                         : WebPEncodeRGBA(img.data(), img.width(), img.height(), stride,
                                                          static_cast<float>(quality < 1 ? 1 : quality), &out);
    if (!n || !out) throw std::runtime_error("WebP encoding failed (images must be at most 16383x16383)");
    std::string s(reinterpret_cast<const char*>(out), n);
    WebPFree(out);
    return s;
}

#else

bool webpSupported() { return false; }

Image decodeWebp(const std::string&) {
    throw std::runtime_error("this build has no WebP support (it was built without libwebp)");
}

std::string encodeWebp(const Image&, int) {
    throw std::runtime_error("this build has no WebP support (it was built without libwebp)");
}

#endif

}  // namespace wetwaresuite::raster
