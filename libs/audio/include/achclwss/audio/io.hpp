// Reading and writing audio files. WAV, MP3, FLAC and Ogg Vorbis decode built
// in (dr_mp3, dr_flac and stb_vorbis are vendored); WAV encodes built in.
// Everything else (Opus, M4A/AAC, AIFF, WMA, the audio track of a video, and
// encoding FLAC/MP3/OGG/Opus/M4A) goes through ffmpeg when it's on PATH.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "achclwss/audio/buffer.hpp"

namespace achclwss::audio {

enum class SampleFormat { Pcm8, Pcm16, Pcm24, Pcm32, Float32 };

struct SaveOptions {
    SampleFormat format = SampleFormat::Pcm16;  // WAV (and FLAC through ffmpeg: 16 or 24)
    bool dither = true;                         // TPDF dither when going to 8/16-bit
    int bitrateKbps = 0;                        // lossy formats; 0 = encoder default (mp3 VBR ~190k, ogg q5, opus 96k, m4a 160k)
};

// Lowercase extensions without the dot.
const std::vector<std::string>& builtinReadExtensions();   // wav mp3 flac ogg
const std::vector<std::string>& builtinWriteExtensions();  // wav
const std::vector<std::string>& ffmpegReadExtensions();    // opus m4a aac aiff ... and video containers
const std::vector<std::string>& ffmpegWriteExtensions();   // flac mp3 ogg opus m4a aac aiff

bool ffmpegAvailable();
bool canRead(const std::filesystem::path& path);   // built in, or ffmpeg is installed for it
bool canWrite(const std::filesystem::path& path);

// Throws std::runtime_error with a readable reason.
Buffer load(const std::filesystem::path& path);
// Decodes a WAV/MP3/FLAC/Ogg file held in memory; the format is sniffed from the bytes.
Buffer decode(const std::string& bytes);
std::string encodeWav(const Buffer& b, const SaveOptions& options = {});
void save(const Buffer& b, const std::filesystem::path& path, const SaveOptions& options = {});

// "16", "24", "32f", "8", "32" -> format; false when unknown.
bool parseSampleFormat(const std::string& text, SampleFormat& out);
std::string toString(SampleFormat f);

}  // namespace achclwss::audio
