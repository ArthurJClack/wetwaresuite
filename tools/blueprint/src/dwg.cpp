// DWG through an outside converter: LibreDWG (dwg2dxf / dxf2dwg, GPL-3.0) or the
// free ODA File Converter. DWG is a closed binary format, so instead of a reader
// of our own we hand it to one of those and read or write the DXF in between.
#include <cctype>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/blueprint/blueprint.hpp"
#include "wetwaresuite/core/process.hpp"

namespace wetwaresuite::blueprint {

namespace fs = std::filesystem;

namespace {

constexpr const char* kInstallHint =
    "DWG needs a converter on PATH: LibreDWG (dwg2dxf and dxf2dwg, free, GPL) or the free ODA File Converter "
    "(ODAFileConverter); or save the drawing as DXF, which every CAD program opens";

std::optional<fs::path> oda() {
    if (auto p = findProgram("ODAFileConverter")) return p;
    return findProgram("ODAFileConverter.exe");
}

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path d = fs::temp_directory_path() / ("wetwaresuite-dwg-" + std::to_string(stamp) + "-" + std::to_string(rd()));
    fs::create_directories(d);
    return d;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string tail(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s.size() > 300 ? s.substr(s.size() - 300) : s;
}

// Runs the converter from `in` (a file) to a file of type `toExt` in a fresh temp folder; returns its path.
fs::path convert(const fs::path& in, const std::string& toExt, const fs::path& dir) {
    fs::path out = dir / ("out." + toExt);
    std::string why;
    const bool toDwg = toExt == "dwg";
    if (auto exe = findProgram(toDwg ? "dxf2dwg" : "dwg2dxf")) {
        auto r = runProgram({exe->string(), "-y", "-o", out.string(), in.string()});
        if (fs::exists(out) && fs::file_size(out) > 0) return out;
        why = exe->filename().string() + ": " + tail(r.output);
    }
    if (auto exe = oda()) {
        // ODAFileConverter <in folder> <out folder> <version> <DWG|DXF> <recurse> <audit> [filter]
        fs::path src = dir / "in";
        fs::create_directories(src);
        fs::copy_file(in, src / ("drawing" + in.extension().string()), fs::copy_options::overwrite_existing);
        fs::path dst = dir / "oda";
        fs::create_directories(dst);
        auto r = runProgram({exe->string(), src.string(), dst.string(), "ACAD2018", toDwg ? "DWG" : "DXF", "0", "1"});
        for (const auto& f : fs::directory_iterator(dst)) {
            std::string e = f.path().extension().string();
            for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (e == "." + toExt) return f.path();
        }
        why += (why.empty() ? "" : "; ") + std::string("ODAFileConverter: ") + tail(r.output);
    }
    if (why.empty()) throw std::runtime_error(kInstallHint);
    throw std::runtime_error("could not convert " + in.filename().string() + " to ." + toExt + (why.empty() ? "" : " (" + why + ")"));
}

}  // namespace

bool dwgSupported() { return findProgram("dwg2dxf") || findProgram("dxf2dwg") || oda(); }

std::string dwgToDxf(const fs::path& dwg) {
    if (!fs::exists(dwg)) throw std::runtime_error("cannot open " + dwg.string());
    fs::path dir = tempDir();
    try {
        std::string text = readAll(convert(dwg, "dxf", dir));
        std::error_code ec;
        fs::remove_all(dir, ec);
        return text;
    } catch (...) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        throw;
    }
}

void dxfToDwg(const std::string& dxf, const fs::path& dwg) {
    fs::path dir = tempDir();
    try {
        const fs::path in = dir / "drawing.dxf";
        {
            std::ofstream out(in, std::ios::binary);
            out << dxf;
            if (!out) throw std::runtime_error("cannot write " + in.string());
        }
        fs::path made = convert(in, "dwg", dir);
        fs::copy_file(made, dwg, fs::copy_options::overwrite_existing);
        std::error_code ec;
        fs::remove_all(dir, ec);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        throw;
    }
}

}  // namespace wetwaresuite::blueprint
