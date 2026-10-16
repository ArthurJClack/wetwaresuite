#include "decoders.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace achclwss::audio::detail {

bool decodeMp3(const unsigned char* data, std::size_t size, Decoded& out) {
    drmp3_config config{};
    drmp3_uint64 frames = 0;
    float* pcm = drmp3_open_memory_and_read_pcm_frames_f32(data, size, &config, &frames, nullptr);
    if (!pcm) return false;
    out.channels = config.channels;
    out.rate = config.sampleRate;
    out.samples.assign(pcm, pcm + frames * config.channels);
    drmp3_free(pcm, nullptr);
    return out.channels > 0 && out.rate > 0 && frames > 0;
}

bool decodeFlac(const unsigned char* data, std::size_t size, Decoded& out) {
    unsigned channels = 0, rate = 0;
    drflac_uint64 frames = 0;
    float* pcm = drflac_open_memory_and_read_pcm_frames_f32(data, size, &channels, &rate, &frames, nullptr);
    if (!pcm) return false;
    out.channels = channels;
    out.rate = rate;
    out.samples.assign(pcm, pcm + frames * channels);
    drflac_free(pcm, nullptr);
    return channels > 0 && rate > 0;
}

}  // namespace achclwss::audio::detail
