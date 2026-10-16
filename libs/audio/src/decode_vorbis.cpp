#include "decoders.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace wetwaresuite::audio::detail {

bool decodeVorbis(const unsigned char* data, std::size_t size, Decoded& out) {
    int error = 0;
    stb_vorbis* v = stb_vorbis_open_memory(data, static_cast<int>(size), &error, nullptr);
    if (!v) return false;
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    out.channels = static_cast<unsigned>(info.channels);
    out.rate = info.sample_rate;
    if (info.channels <= 0) {
        stb_vorbis_close(v);
        return false;
    }
    std::vector<float> chunk(static_cast<std::size_t>(4096) * static_cast<std::size_t>(info.channels));
    for (;;) {
        const int n = stb_vorbis_get_samples_float_interleaved(v, info.channels, chunk.data(), static_cast<int>(chunk.size()));
        if (n <= 0) break;
        out.samples.insert(out.samples.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n) * info.channels);
    }
    stb_vorbis_close(v);
    return out.rate > 0;
}

}  // namespace wetwaresuite::audio::detail
