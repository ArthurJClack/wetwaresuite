// Thin wrappers over the vendored decoders, each compiled in its own file
// with third-party warnings silenced.
#pragma once

#include <cstddef>
#include <vector>

namespace achclwss::audio::detail {

// Interleaved float samples; false when the bytes aren't that format.
struct Decoded {
    unsigned channels = 0;
    unsigned rate = 0;
    std::vector<float> samples;
};

bool decodeMp3(const unsigned char* data, std::size_t size, Decoded& out);
bool decodeFlac(const unsigned char* data, std::size_t size, Decoded& out);
bool decodeVorbis(const unsigned char* data, std::size_t size, Decoded& out);

}  // namespace achclwss::audio::detail
