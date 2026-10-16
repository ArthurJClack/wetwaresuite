#include "media.hpp"

#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/cut/cut.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

std::optional<std::string> ffmpegPath() {
    if (auto p = findProgram("ffmpeg")) return p->string();
    return std::nullopt;
}

std::optional<std::string> ffprobePath() {
    if (auto p = findProgram("ffprobe")) return p->string();
    return std::nullopt;
}

std::string missingFfmpeg() {
    return "video editing needs ffmpeg, which isn't installed (or isn't on PATH). Install it, it's free: "
           "Debian/Ubuntu `sudo apt install ffmpeg`, macOS `brew install ffmpeg`, Windows `winget install ffmpeg`.";
}

std::string runFfmpeg(std::vector<std::string> args) {
    auto exe = ffmpegPath();
    if (!exe) return missingFfmpeg();
    args[0] = *exe;
    ProcessResult r = runProgram(args);
    if (r.exitCode == 0) return {};
    std::string out = r.output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (out.size() > 1500) out = "..." + out.substr(out.size() - 1500);
    if (r.exitCode < 0) return "couldn't start ffmpeg";
    return "ffmpeg failed (exit " + std::to_string(r.exitCode) + ")" + (out.empty() ? std::string() : ": " + out);
}

TempDir::TempDir() {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    for (int attempt = 0; attempt < 100; ++attempt) {
        fs::path p = fs::temp_directory_path() / ("wetwaresuite-cut-" + std::to_string(gen() % 1000000000000ULL));
        std::error_code ec;
        if (fs::create_directory(p, ec)) {
            path_ = p;
            return;
        }
    }
    throw std::runtime_error("can't create a temporary folder");
}

TempDir::~TempDir() {
    std::error_code ec;
    if (!path_.empty()) fs::remove_all(path_, ec);
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    return static_cast<bool>(out);
}

MediaInfo probe(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw std::runtime_error("file not found: " + path.string());
    auto exe = ffprobePath();
    if (!exe) throw std::runtime_error(missingFfmpeg() + " (ffprobe comes with it)");
    ProcessResult r = runProgram({*exe, "-v", "error", "-print_format", "json", "-show_format", "-show_streams", path.string()});
    if (r.exitCode != 0) throw std::runtime_error("ffprobe can't read " + path.string() + ": " + r.output);
    // ffprobe may print warnings before the JSON
    const auto brace = r.output.find('{');
    if (brace == std::string::npos) throw std::runtime_error("ffprobe printed nothing for " + path.string());
    try {
        MediaInfo m = parseProbe(r.output.substr(brace));
        if (!m.hasVideo && !m.hasAudio) throw std::runtime_error(path.string() + " has no video or audio streams");
        return m;
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& e) {
        throw std::runtime_error("couldn't read ffprobe's answer for " + path.string() + ": " + e.what());
    }
}

std::string findFont() {
    static const std::string cached = [] {
        std::vector<fs::path> candidates;
#ifdef _WIN32
        const char* windir = std::getenv("WINDIR");
        fs::path fonts = fs::path(windir ? windir : "C:\\Windows") / "Fonts";
        for (const char* f : {"arial.ttf", "segoeui.ttf", "calibri.ttf", "tahoma.ttf", "verdana.ttf"}) candidates.push_back(fonts / f);
#elif defined(__APPLE__)
        for (const char* f : {"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
                              "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/SFNS.ttf"}) {
            candidates.push_back(f);
        }
#else
        for (const char* f : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                              "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                              "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                              "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
                              "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                              "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
                              "/usr/share/fonts/truetype/crosextra/Carlito-Regular.ttf"}) {
            candidates.push_back(f);
        }
#endif
        std::error_code ec;
        for (const auto& c : candidates) {
            if (fs::is_regular_file(c, ec)) return c.string();
        }
        if (auto fc = findProgram("fc-match")) {
            ProcessResult r = runProgram({fc->string(), "-f", "%{file}", "sans-serif"});
            if (r.exitCode == 0 && !r.output.empty() && fs::is_regular_file(r.output, ec)) return r.output;
        }
#ifndef _WIN32
        // any TrueType font at all
        for (const char* dir : {"/usr/share/fonts", "/usr/local/share/fonts", "/Library/Fonts"}) {
            if (!fs::is_directory(dir, ec)) continue;
            int seen = 0;
            for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator() && seen < 5000; it.increment(ec), ++seen) {
                if (ec) break;
                const std::string ext = it->path().extension().string();
                if ((ext == ".ttf" || ext == ".otf") && it->is_regular_file(ec)) return it->path().string();
            }
        }
#endif
        return std::string();
    }();
    return cached;
}

std::unique_ptr<Attachment> Timeline::preview() const {
    if (duration() <= 0 || !ffmpegPath()) return nullptr;
    try {
        TempDir tmp;
        RenderRequest rq;
        rq.output = (tmp.path() / "preview.png").string();
        rq.at = 0;
        rq.stillWidth = std::min(settings.width, 480);
        rq.fontFile = findFont();
        RenderPlan plan = planRender(*this, rq);
        if (!runFfmpeg(plan.args).empty()) return nullptr;
        auto a = std::make_unique<Attachment>();
        a->mime = "image/png";
        a->bytes = readFile(rq.output);
        a->name = "timeline.png";
        if (a->bytes.empty()) return nullptr;
        return a;
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // namespace wetwaresuite::cut
