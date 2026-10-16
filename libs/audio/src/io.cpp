#include "achclwss/audio/io.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>
#include <stdexcept>

#include "achclwss/core/process.hpp"
#include "decoders.hpp"

namespace achclwss::audio {

namespace fs = std::filesystem;

namespace {

std::string extOf(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes;
}

std::uint32_t u32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

Buffer fromInterleaved(const detail::Decoded& d) {
    const std::size_t ch = d.channels;
    const std::size_t frames = ch ? d.samples.size() / ch : 0;
    Buffer b(static_cast<int>(ch), frames, static_cast<int>(d.rate));
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < ch; ++c) b.channels[c][f] = d.samples[f * ch + c];
    }
    return b;
}

bool isWav(const std::string& s) {
    return s.size() >= 12 && std::memcmp(s.data(), "RIFF", 4) == 0 && std::memcmp(s.data() + 8, "WAVE", 4) == 0;
}

Buffer decodeWav(const std::string& bytes) {
    const auto* d = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    std::size_t pos = 12;
    int tag = 0, channels = 0, bits = 0, rate = 0, align = 0;
    const unsigned char* data = nullptr;
    std::size_t dataSize = 0;
    while (pos + 8 <= n) {
        const std::uint32_t size = u32(d + pos + 4);
        const unsigned char* body = d + pos + 8;
        const std::size_t avail = n - pos - 8;
        if (std::memcmp(d + pos, "fmt ", 4) == 0 && size >= 16 && avail >= 16) {
            tag = u16(body);
            channels = u16(body + 2);
            rate = static_cast<int>(u32(body + 4));
            align = u16(body + 12);
            bits = u16(body + 14);
            if (tag == 0xFFFE && size >= 40 && avail >= 40) tag = u16(body + 24);  // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (std::memcmp(d + pos, "data", 4) == 0) {
            data = body;
            dataSize = std::min<std::size_t>(size, avail);  // streamed files leave the size at 0 or 0xFFFFFFFF
            if (size == 0 || size == 0xFFFFFFFFu) dataSize = avail;
            break;
        }
        pos += 8 + static_cast<std::size_t>(size) + (size & 1);
    }
    if (!tag || !data) throw std::runtime_error("broken WAV file: no fmt or data chunk");
    if (channels <= 0 || rate <= 0) throw std::runtime_error("broken WAV file: no channels or sample rate");
    if (tag != 1 && tag != 3) throw std::runtime_error("WAV codec " + std::to_string(tag) + " isn't supported (only PCM and float)");
    const int bytesPer = bits / 8;
    if ((tag == 1 && (bits < 8 || bits > 32 || bits % 8)) || (tag == 3 && bits != 32 && bits != 64)) {
        throw std::runtime_error("WAV with " + std::to_string(bits) + "-bit samples isn't supported");
    }
    if (align < bytesPer * channels) align = bytesPer * channels;
    const std::size_t frames = dataSize / static_cast<std::size_t>(align);
    Buffer b(channels, frames, rate);
    for (std::size_t f = 0; f < frames; ++f) {
        const unsigned char* fp = data + f * static_cast<std::size_t>(align);
        for (int c = 0; c < channels; ++c) {
            const unsigned char* p = fp + static_cast<std::size_t>(c) * bytesPer;
            float v = 0;
            if (tag == 3 && bits == 32) {
                std::uint32_t u = u32(p);
                std::memcpy(&v, &u, 4);
            } else if (tag == 3) {
                std::uint64_t u = static_cast<std::uint64_t>(u32(p)) | static_cast<std::uint64_t>(u32(p + 4)) << 32;
                double dv;
                std::memcpy(&dv, &u, 8);
                v = static_cast<float>(dv);
            } else if (bits == 8) {
                v = (static_cast<int>(p[0]) - 128) / 128.0f;
            } else if (bits == 16) {
                v = static_cast<std::int16_t>(u16(p)) / 32768.0f;
            } else if (bits == 24) {
                std::int32_t s = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) << 8 | static_cast<std::uint32_t>(p[1]) << 16 |
                                                           static_cast<std::uint32_t>(p[2]) << 24) >> 8;
                v = static_cast<float>(s / 8388608.0);
            } else {
                v = static_cast<float>(static_cast<std::int32_t>(u32(p)) / 2147483648.0);
            }
            b.channels[static_cast<std::size_t>(c)][f] = std::isfinite(v) ? v : 0.0f;
        }
    }
    return b;
}

void put16(std::string& s, std::uint16_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>(v >> 8);
}
void put32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

fs::path tempPath(const std::string& ext) {
    static std::atomic<unsigned> counter{0};
    static const unsigned salt = std::random_device{}();
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) dir = ".";
    return dir / ("achclwss-audio-" + std::to_string(salt) + "-" + std::to_string(++counter) + "." + ext);
}

struct TempFile {
    fs::path path;
    explicit TempFile(const std::string& ext) : path(tempPath(ext)) {}
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

std::optional<fs::path> ffmpegPath() { return findProgram("ffmpeg"); }

Buffer loadWithFfmpeg(const fs::path& path) {
    auto exe = ffmpegPath();
    if (!exe) {
        throw std::runtime_error("opening ." + extOf(path) + " needs ffmpeg; install it (it's free) and make sure it's on PATH, or use wav, mp3, flac or ogg");
    }
    TempFile tmp("wav");
    auto r = runProgram({exe->string(), "-hide_banner", "-loglevel", "error", "-y", "-i", path.string(), "-vn", "-map", "0:a:0",
                         "-c:a", "pcm_f32le", "-f", "wav", tmp.path.string()});
    std::error_code ec;
    if (r.exitCode != 0 || !fs::exists(tmp.path, ec)) {
        throw std::runtime_error("ffmpeg couldn't read the audio in " + path.string() + ": " + r.output);
    }
    return decodeWav(readFile(tmp.path));
}

}  // namespace

const std::vector<std::string>& builtinReadExtensions() {
    static const std::vector<std::string> v = {"wav", "wave", "mp3", "flac", "ogg", "oga"};
    return v;
}
const std::vector<std::string>& builtinWriteExtensions() {
    static const std::vector<std::string> v = {"wav"};
    return v;
}
const std::vector<std::string>& ffmpegReadExtensions() {
    static const std::vector<std::string> v = {"opus", "m4a", "aac", "aiff", "aif", "wma", "ac3", "amr", "caf", "mka", "wv",
                                               "ape", "mp4", "mkv", "mov", "webm", "avi", "m4v", "3gp", "flv", "ts", "mpg"};
    return v;
}
const std::vector<std::string>& ffmpegWriteExtensions() {
    static const std::vector<std::string> v = {"flac", "mp3", "ogg", "opus", "m4a", "aac", "aiff"};
    return v;
}

bool ffmpegAvailable() { return ffmpegPath().has_value(); }

bool canRead(const fs::path& path) {
    const std::string e = extOf(path);
    return contains(builtinReadExtensions(), e) || (contains(ffmpegReadExtensions(), e) && ffmpegAvailable());
}

bool canWrite(const fs::path& path) {
    const std::string e = extOf(path);
    return contains(builtinWriteExtensions(), e) || (contains(ffmpegWriteExtensions(), e) && ffmpegAvailable());
}

Buffer decode(const std::string& bytes) {
    if (isWav(bytes)) return decodeWav(bytes);
    const auto* d = reinterpret_cast<const unsigned char*>(bytes.data());
    detail::Decoded out;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "fLaC", 4) == 0) {
        if (detail::decodeFlac(d, bytes.size(), out)) return fromInterleaved(out);
        throw std::runtime_error("broken FLAC file");
    }
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0) {
        if (detail::decodeVorbis(d, bytes.size(), out)) return fromInterleaved(out);
        throw std::runtime_error("this Ogg file isn't Vorbis (Opus and others need ffmpeg)");
    }
    if (detail::decodeMp3(d, bytes.size(), out)) return fromInterleaved(out);
    throw std::runtime_error("not an audio format I can decode (wav, mp3, flac, ogg)");
}

Buffer load(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw std::runtime_error("file not found: " + path.string());
    const std::string e = extOf(path);
    if (contains(builtinReadExtensions(), e)) {
        try {
            Buffer b = decode(readFile(path));
            if (b.channelCount() > 0) return b;
        } catch (const std::exception&) {
            if (!ffmpegAvailable()) throw;
        }
    }
    return loadWithFfmpeg(path);
}

std::string encodeWav(const Buffer& b, const SaveOptions& o) {
    const int ch = std::max(1, b.channelCount());
    const int bits = o.format == SampleFormat::Pcm8 ? 8 : o.format == SampleFormat::Pcm16 ? 16 : o.format == SampleFormat::Pcm24 ? 24 : 32;
    const int bytesPer = bits / 8;
    const std::uint64_t dataSize = static_cast<std::uint64_t>(b.frames()) * ch * bytesPer;
    if (dataSize > 0xFFFFFFF0ull - 64) throw std::runtime_error("too long for a WAV file (4 GB limit); save as flac instead");
    std::string s;
    s.reserve(static_cast<std::size_t>(dataSize) + 44);
    s += "RIFF";
    put32(s, static_cast<std::uint32_t>(36 + dataSize));
    s += "WAVEfmt ";
    put32(s, 16);
    put16(s, o.format == SampleFormat::Float32 ? 3 : 1);
    put16(s, static_cast<std::uint16_t>(ch));
    put32(s, static_cast<std::uint32_t>(b.rate));
    put32(s, static_cast<std::uint32_t>(b.rate * ch * bytesPer));
    put16(s, static_cast<std::uint16_t>(ch * bytesPer));
    put16(s, static_cast<std::uint16_t>(bits));
    s += "data";
    put32(s, static_cast<std::uint32_t>(dataSize));

    std::minstd_rand rng(12345);
    std::uniform_real_distribution<float> uni(-0.5f, 0.5f);
    const bool dither = o.dither && bits <= 16;
    const double scale = bits == 8 ? 128.0 : bits == 16 ? 32768.0 : bits == 24 ? 8388608.0 : 2147483648.0;
    for (std::size_t f = 0; f < b.frames(); ++f) {
        for (int c = 0; c < ch; ++c) {
            float v = b.channelCount() ? b.channels[static_cast<std::size_t>(c)][f] : 0.0f;
            if (o.format == SampleFormat::Float32) {
                std::uint32_t u;
                std::memcpy(&u, &v, 4);
                put32(s, u);
                continue;
            }
            double x = static_cast<double>(v) * scale;
            if (dither) x += static_cast<double>(uni(rng) + uni(rng));
            x = std::round(x);
            x = std::clamp(x, -scale, scale - 1);
            const auto i = static_cast<std::int64_t>(x);
            if (bits == 8) s += static_cast<char>(static_cast<std::uint8_t>(i + 128));
            else if (bits == 16) put16(s, static_cast<std::uint16_t>(static_cast<std::int16_t>(i)));
            else if (bits == 24) {
                const auto u = static_cast<std::uint32_t>(static_cast<std::int32_t>(i));
                s += static_cast<char>(u & 0xFF);
                s += static_cast<char>((u >> 8) & 0xFF);
                s += static_cast<char>((u >> 16) & 0xFF);
            } else put32(s, static_cast<std::uint32_t>(static_cast<std::int32_t>(i)));
        }
    }
    return s;
}

void save(const Buffer& b, const fs::path& path, const SaveOptions& o) {
    const std::string e = extOf(path);
    if (b.channelCount() == 0) throw std::runtime_error("nothing to save: the audio has no channels");
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    if (e == "wav" || e == "wave") {
        const std::string bytes = encodeWav(b, o);
        std::ofstream out(path, std::ios::binary);
        if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("can't write " + path.string());
        return;
    }
    if (!contains(ffmpegWriteExtensions(), e)) {
        throw std::runtime_error("can't save ." + e + " audio; use wav, flac, mp3, ogg, opus, m4a, aac or aiff");
    }
    auto exe = ffmpegPath();
    if (!exe) throw std::runtime_error("saving ." + e + " needs ffmpeg; install it (it's free) and make sure it's on PATH, or save as .wav");

    const bool lossless = e == "flac" || e == "aiff";
    SaveOptions wavOptions = o;
    if (lossless) {
        if (wavOptions.format == SampleFormat::Float32 || wavOptions.format == SampleFormat::Pcm32) wavOptions.format = SampleFormat::Pcm24;
        if (wavOptions.format == SampleFormat::Pcm8) wavOptions.format = SampleFormat::Pcm16;
    } else {
        wavOptions.format = SampleFormat::Float32;  // let the encoder do the only quantisation
    }
    TempFile tmp("wav");
    {
        const std::string bytes = encodeWav(b, wavOptions);
        std::ofstream out(tmp.path, std::ios::binary);
        if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("can't write a temporary file");
    }
    std::vector<std::string> args = {exe->string(), "-hide_banner", "-loglevel", "error", "-y", "-i", tmp.path.string()};
    const std::string kbps = o.bitrateKbps > 0 ? std::to_string(o.bitrateKbps) + "k" : std::string();
    if (e == "flac") {
        args.insert(args.end(), {"-c:a", "flac"});
    } else if (e == "aiff") {
        args.insert(args.end(), {"-c:a", wavOptions.format == SampleFormat::Pcm24 ? "pcm_s24be" : "pcm_s16be"});
    } else if (e == "mp3") {
        args.insert(args.end(), {"-c:a", "libmp3lame"});
        if (kbps.empty()) args.insert(args.end(), {"-q:a", "2"});
        else args.insert(args.end(), {"-b:a", kbps});
    } else if (e == "ogg") {
        args.insert(args.end(), {"-c:a", "libvorbis"});
        if (kbps.empty()) args.insert(args.end(), {"-q:a", "5"});
        else args.insert(args.end(), {"-b:a", kbps});
    } else if (e == "opus") {
        args.insert(args.end(), {"-c:a", "libopus", "-b:a", kbps.empty() ? std::string("96k") : kbps});
    } else {  // m4a, aac
        args.insert(args.end(), {"-c:a", "aac", "-b:a", kbps.empty() ? std::string("160k") : kbps});
    }
    args.push_back(path.string());
    auto r = runProgram(args);
    if (r.exitCode != 0) throw std::runtime_error("ffmpeg couldn't write " + path.string() + ": " + r.output);
}

bool parseSampleFormat(const std::string& t, SampleFormat& out) {
    if (t == "8") out = SampleFormat::Pcm8;
    else if (t == "16") out = SampleFormat::Pcm16;
    else if (t == "24") out = SampleFormat::Pcm24;
    else if (t == "32") out = SampleFormat::Pcm32;
    else if (t == "32f" || t == "float" || t == "f32") out = SampleFormat::Float32;
    else return false;
    return true;
}

std::string toString(SampleFormat f) {
    switch (f) {
        case SampleFormat::Pcm8: return "8";
        case SampleFormat::Pcm16: return "16";
        case SampleFormat::Pcm24: return "24";
        case SampleFormat::Pcm32: return "32";
        case SampleFormat::Float32: return "32f";
    }
    return "16";
}

}  // namespace achclwss::audio
