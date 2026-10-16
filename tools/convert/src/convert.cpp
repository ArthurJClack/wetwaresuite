#include "achclwss/convert/convert.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "achclwss/core/process.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/document.hpp"
#include "achclwss/raster/ops.hpp"

namespace achclwss::convert {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string extOf(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower(e);
}

const std::set<std::string> kRasterIn = {"png", "jpg", "jpeg", "bmp", "tga", "gif", "psd", "hdr", "pic", "ppm", "pgm", "pnm", "ora"};
const std::set<std::string> kRasterOut = {"png", "jpg", "jpeg", "bmp", "tga", "ppm", "ora"};
const std::set<std::string> kAudio = {"wav", "mp3", "flac", "ogg", "opus", "aac", "m4a", "aiff", "aif", "wma", "ac3", "amr"};
const std::set<std::string> kVideo = {"mp4", "mkv", "mov", "webm", "avi", "wmv", "flv", "m4v", "mpg", "mpeg", "3gp", "ts", "gif"};
const std::set<std::string> kFfmpegImages = {"webp", "avif", "heic", "heif", "tif", "tiff", "jxl", "ico", "exr"};
const std::set<std::string> kOffice = {"doc", "docx", "odt", "rtf", "xls", "xlsx", "ods", "ppt", "pptx", "odp", "wpd", "pages", "key", "numbers"};
const std::set<std::string> kOfficeOut = {"pdf", "docx", "doc", "odt", "rtf", "txt", "html", "xlsx", "xls", "ods", "csv", "pptx", "ppt", "odp", "epub"};
const std::set<std::string> kMarkup = {"md", "markdown", "html", "htm", "rst", "tex", "org", "txt", "epub", "docx", "odt"};
const std::set<std::string> kMarkupOut = {"md", "html", "rst", "tex", "org", "txt", "epub", "docx", "odt", "pdf"};
const std::set<std::string> kVectorIn = {"svg"};
const std::set<std::string> kVectorOut = {"svg", "png", "jpg", "jpeg", "bmp", "tga", "ppm"};
const std::set<std::string> k3d = {"obj", "fbx", "gltf", "glb", "dae", "ply", "stl", "3mf", "step", "stp", "iges", "igs", "usd", "usdz", "blend"};

bool in(const std::set<std::string>& set, const std::string& v) { return set.count(v) > 0; }

std::optional<std::string> program(const std::vector<std::string>& names) {
    for (const auto& n : names) {
        if (auto p = findProgram(n)) return p->string();
    }
    return std::nullopt;
}

std::optional<std::string> ffmpeg() { return program({"ffmpeg"}); }
std::optional<std::string> soffice() { return program({"soffice", "libreoffice"}); }
std::optional<std::string> pandoc() { return program({"pandoc"}); }

Result rasterConvert(const fs::path& input, const fs::path& output, const Params& o) {
    try {
        auto doc = raster::openImage(input);
        int w = static_cast<int>(intParam(o, "width", 0)), h = static_cast<int>(intParam(o, "height", 0));
        if (w > 0 || h > 0) {
            const double ow = doc->image.width, oh = doc->image.height;
            if (w <= 0) w = static_cast<int>(ow * h / oh + 0.5);
            if (h <= 0) h = static_cast<int>(oh * w / ow + 0.5);
            for (auto& layer : doc->image.layers) layer.image = raster::resize(layer.image, w, h);
            doc->image.width = w;
            doc->image.height = h;
        }
        raster::saveImage(*doc, output, static_cast<int>(intParam(o, "quality", 90)));
        return Result::success("converted " + input.string() + " to " + output.string());
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
}

Result ffmpegConvert(const fs::path& input, const fs::path& output, const Params& o) {
    auto exe = ffmpeg();
    if (!exe) return Result::failure("audio/video conversion needs ffmpeg; install it (it's free) and make sure it's on PATH");
    std::vector<std::string> args = {*exe, "-hide_banner", "-loglevel", "error", "-y", "-i", input.string()};
    const std::string to = extOf(output);
    int w = static_cast<int>(intParam(o, "width", 0)), h = static_cast<int>(intParam(o, "height", 0));
    if (w > 0 || h > 0) {
        args.insert(args.end(), {"-vf", "scale=" + std::to_string(w > 0 ? w : -2) + ":" + std::to_string(h > 0 ? h : -2)});
    }
    if (in(kAudio, to) && !in(kAudio, extOf(input))) args.push_back("-vn");  // video in, audio out: drop the picture
    if (o.count("quality")) {
        int q = static_cast<int>(std::clamp<std::int64_t>(intParam(o, "quality", 90), 1, 100));
        if (to == "mp3") args.insert(args.end(), {"-q:a", std::to_string((100 - q) * 9 / 100)});
        else if (to == "jpg" || to == "jpeg") args.insert(args.end(), {"-q:v", std::to_string(2 + (100 - q) * 29 / 100)});
        else if (in(kVideo, to) && to != "gif") args.insert(args.end(), {"-crf", std::to_string(18 + (100 - q) * 22 / 100)});
    }
    if (o.count("start")) args.insert(args.end(), {"-ss", toText(o.at("start"))});
    if (o.count("duration")) args.insert(args.end(), {"-t", toText(o.at("duration"))});
    args.push_back(output.string());
    auto r = runProgram(args);
    if (r.exitCode != 0) return Result::failure("ffmpeg failed (exit " + std::to_string(r.exitCode) + "): " + r.output);
    return Result::success("converted " + input.string() + " to " + output.string() + " with ffmpeg");
}

Result officeConvert(const fs::path& input, const fs::path& output) {
    auto exe = soffice();
    if (!exe) return Result::failure("office document conversion needs LibreOffice (soffice) on PATH");
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec) / ("achclwss-convert-" + std::to_string(std::hash<std::string>{}(input.string())));
    fs::create_directories(tmp, ec);
    auto r = runProgram({*exe, "--headless", "--norestore", "--convert-to", extOf(output), "--outdir", tmp.string(), input.string()});
    fs::path produced = tmp / input.filename();
    produced.replace_extension(output.extension());
    if (r.exitCode != 0 || !fs::exists(produced, ec)) {
        fs::remove_all(tmp, ec);
        return Result::failure("LibreOffice couldn't convert it: " + r.output);
    }
    fs::create_directories(output.parent_path().empty() ? fs::path(".") : output.parent_path(), ec);
    fs::copy_file(produced, output, fs::copy_options::overwrite_existing, ec);
    fs::remove_all(tmp);
    if (ec) return Result::failure("couldn't write " + output.string() + ": " + ec.message());
    return Result::success("converted " + input.string() + " to " + output.string() + " with LibreOffice");
}

Result pandocConvert(const fs::path& input, const fs::path& output) {
    auto exe = pandoc();
    if (!exe) return Result::failure("markup conversion needs pandoc on PATH");
    auto r = runProgram({*exe, input.string(), "-o", output.string()});
    if (r.exitCode != 0) return Result::failure("pandoc failed: " + r.output);
    return Result::success("converted " + input.string() + " to " + output.string() + " with pandoc");
}

Result vectorConvert(Session& s, const fs::path& input, const fs::path& output, const Params& o) {
    Params args = {{"input", input.string()}, {"output", output.string()}};
    if (o.count("scale")) args["scale"] = numberParam(o, "scale", 1.0);
    if (o.count("quality")) args["quality"] = intParam(o, "quality", 90);
    Result r = s.run("vector.convert", args);
    if (!r.ok) return r;
    return Result::success("converted " + input.string() + " to " + output.string());
}

std::string joined(const std::set<std::string>& s) {
    std::string out;
    for (const auto& v : s) out += (out.empty() ? "" : ", ") + v;
    return out;
}

}  // namespace

std::string backendFor(const Session& session, const fs::path& input, const std::string& targetRaw) {
    const std::string from = extOf(input), to = lower(targetRaw);
    if (in(kRasterIn, from) && in(kRasterOut, to)) return "raster";
    if (in(kVectorIn, from) && in(kVectorOut, to) && session.registry().find("vector.convert")) return "vector";
    const bool mediaIn = in(kAudio, from) || in(kVideo, from) || in(kFfmpegImages, from) || in(kRasterIn, from);
    const bool mediaOut = in(kAudio, to) || in(kVideo, to) || in(kFfmpegImages, to) || in(kRasterOut, to);
    if (mediaIn && mediaOut && to != "ora" && from != "ora") {
        // video -> image only makes sense as a still; ffmpeg handles that too
        return "ffmpeg";
    }
    if (in(kOffice, from) && in(kOfficeOut, to)) return "libreoffice";
    if (in(kMarkup, from) && in(kMarkupOut, to)) return to == "pdf" && soffice() ? "libreoffice" : "pandoc";
    return {};
}

Result convertFile(Session& session, const fs::path& input, const fs::path& output, const Params& o) {
    std::error_code ec;
    if (!fs::is_regular_file(input, ec)) return Result::failure("file not found: " + input.string());
    if (fs::exists(output, ec) && !boolParam(o, "overwrite", false)) {
        return Result::failure(output.string() + " already exists (pass overwrite=true to replace it)");
    }
    if (fs::exists(output, ec) && fs::equivalent(input, output, ec)) return Result::failure("input and output are the same file");
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path(), ec);

    const std::string from = extOf(input), to = extOf(output);
    if (to.empty()) return Result::failure("output needs a file extension so I know the format, e.g. out.png");
    const std::string backend = backendFor(session, input, to);
    Result r;
    if (backend == "raster") r = rasterConvert(input, output, o);
    else if (backend == "vector") r = vectorConvert(session, input, output, o);
    else if (backend == "ffmpeg") r = ffmpegConvert(input, output, o);
    else if (backend == "libreoffice") r = officeConvert(input, output);
    else if (backend == "pandoc") r = pandocConvert(input, output);
    else if (in(k3d, from) || in(k3d, to)) return Result::failure("3D and CAD conversion arrives with asset forge and cad in phase 3");
    else return Result::failure("no converter from ." + from + " to ." + to + " yet (run convert.formats to see what works)");
    if (!r.ok) return r;
    auto bytes = static_cast<std::int64_t>(fs::file_size(output, ec));
    return r.with("output", output.string()).with("backend", backend).with("bytes", bytes);
}

void registerCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"convert.file",
             "Convert file",
             "Convert one file to another format, picked from the output's extension. Images use the built-in "
             "engine; SVG uses the vector tool; audio and video use ffmpeg; office documents use LibreOffice; "
             "markup uses pandoc. Run convert.formats to see what's available on this machine.",
             {{"input", ParamType::String, "file to convert", true},
              {"output", ParamType::String, "file to write; its extension sets the format, e.g. photo.jpg", true},
              {"quality", ParamType::Int, "1..100 for lossy formats (JPEG, MP3, video)", false},
              {"width", ParamType::Int, "resize images/video to this width (keeps aspect if height is unset)", false},
              {"height", ParamType::Int, "resize images/video to this height", false},
              {"scale", ParamType::Number, "SVG to raster: scale factor", false},
              {"start", ParamType::Number, "audio/video: start time in seconds", false},
              {"duration", ParamType::Number, "audio/video: length in seconds", false},
              {"overwrite", ParamType::Bool, "replace the output if it exists", false, {}, false}},
             false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>(
                    [s, p] { return convertFile(*s, stringParam(p, "input"), stringParam(p, "output"), p); });
            });

    reg.add({"convert.batch",
             "Convert folder",
             "Convert every file in a folder that can become the target format. Files keep their names with the new "
             "extension; originals are never touched.",
             {{"input_dir", ParamType::String, "folder with files to convert", true},
              {"to", ParamType::String, "target extension, e.g. jpg, mp3, pdf", true},
              {"output_dir", ParamType::String, "where results go (default: next to each original)", false},
              {"recursive", ParamType::Bool, "include subfolders", false, {}, false},
              {"quality", ParamType::Int, "1..100 for lossy formats", false},
              {"width", ParamType::Int, "resize images/video to this width", false},
              {"overwrite", ParamType::Bool, "replace existing outputs", false, {}, false}},
             false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    fs::path inDir = stringParam(p, "input_dir");
                    std::string to = lower(stringParam(p, "to"));
                    if (!to.empty() && to[0] == '.') to.erase(0, 1);
                    std::error_code ec;
                    if (!fs::is_directory(inDir, ec)) return Result::failure("not a folder: " + inDir.string());
                    fs::path outDir = stringParam(p, "output_dir");
                    std::vector<fs::path> files;
                    auto consider = [&](const fs::directory_entry& e) {
                        if (e.is_regular_file() && extOf(e.path()) != to && !backendFor(*s, e.path(), to).empty()) files.push_back(e.path());
                    };
                    if (boolParam(p, "recursive", false)) {
                        for (const auto& e : fs::recursive_directory_iterator(inDir, ec)) consider(e);
                    } else {
                        for (const auto& e : fs::directory_iterator(inDir, ec)) consider(e);
                    }
                    std::sort(files.begin(), files.end());
                    if (files.empty()) return Result::failure("nothing in " + inDir.string() + " can be converted to ." + to);
                    int ok = 0;
                    std::string failures;
                    for (const auto& f : files) {
                        fs::path target = outDir.empty() ? f : outDir / fs::relative(f, inDir, ec);
                        target.replace_extension(to);
                        Result r = convertFile(*s, f, target, p);
                        if (r.ok) ++ok;
                        else failures += "\n  " + f.filename().string() + ": " + r.message;
                    }
                    std::string msg = "converted " + std::to_string(ok) + " of " + std::to_string(files.size()) + " files to ." + to + failures;
                    Result r = ok > 0 ? Result::success(msg) : Result::failure(msg);
                    return r.with("converted", static_cast<std::int64_t>(ok))
                        .with("failed", static_cast<std::int64_t>(files.size()) - ok);
                });
            });

    reg.add({"convert.formats",
             "List formats",
             "Show which conversions work on this machine and which helper programs are installed.",
             {},
             false},
            [s](const Params&) {
                return std::make_unique<FunctionAction>([s] {
                    auto yes = [](bool b) { return std::string(b ? "ready" : "not installed"); };
                    bool hasFfmpeg = ffmpeg().has_value(), hasOffice = soffice().has_value(), hasPandoc = pandoc().has_value();
                    bool hasVector = s->registry().find("vector.convert") != nullptr;
                    std::string text =
                        "images (built in, ready): " + joined(kRasterIn) + " -> " + joined(kRasterOut) +
                        "\nsvg (vector tool, " + yes(hasVector) + "): svg -> " + joined(kVectorOut) +
                        "\naudio/video/more images (ffmpeg, " + yes(hasFfmpeg) + "): " + joined(kAudio) + ", " + joined(kVideo) +
                        ", " + joined(kFfmpegImages) +
                        "\noffice documents (LibreOffice, " + yes(hasOffice) + "): " + joined(kOffice) + " -> " + joined(kOfficeOut) +
                        "\nmarkup (pandoc, " + yes(hasPandoc) + "): " + joined(kMarkup) + " -> " + joined(kMarkupOut) +
                        "\n3D and CAD: coming with asset forge and cad (phase 3)";
                    return Result::success(text).with("ffmpeg", hasFfmpeg).with("libreoffice", hasOffice)
                        .with("pandoc", hasPandoc).with("vector", hasVector);
                });
            });

    reg.add({"convert.check",
             "Check conversion",
             "Say whether a file can be converted to a format and which backend would do it, without converting.",
             {{"input", ParamType::String, "file to convert", true}, {"to", ParamType::String, "target extension", true}},
             false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    std::string to = lower(stringParam(p, "to"));
                    if (!to.empty() && to[0] == '.') to.erase(0, 1);
                    std::string b = backendFor(*s, stringParam(p, "input"), to);
                    if (b.empty()) return Result::failure("no converter to ." + to + " for " + stringParam(p, "input"));
                    bool ready = b == "raster" || b == "vector" || (b == "ffmpeg" && ffmpeg()) ||
                                 (b == "libreoffice" && soffice()) || (b == "pandoc" && pandoc());
                    return Result::success(b + (ready ? " can do it" : " would do it, but isn't installed"))
                        .with("backend", b).with("ready", ready);
                });
            });
}

}  // namespace achclwss::convert
