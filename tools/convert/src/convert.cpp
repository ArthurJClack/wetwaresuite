#include "wetwaresuite/convert/convert.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/ops.hpp"

namespace wetwaresuite::convert {

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
const std::set<std::string> kStill = {"png", "jpg", "jpeg", "bmp", "tga", "ppm", "webp", "avif", "tif", "tiff", "jxl", "ico", "exr"};
const std::set<std::string> kPlainMarkup = {"md", "markdown", "rst", "org", "tex"};
const std::set<std::string> kOffice = {"doc", "docx", "odt", "rtf", "xls", "xlsx", "ods", "ppt", "pptx", "odp", "wpd", "pages", "key", "numbers"};
const std::set<std::string> kOfficeOut = {"pdf", "docx", "doc", "odt", "rtf", "txt", "html", "xlsx", "xls", "ods", "csv", "pptx", "ppt", "odp", "epub"};
const std::set<std::string> kMarkup = {"md", "markdown", "html", "htm", "rst", "tex", "org", "txt", "epub", "docx", "odt"};
const std::set<std::string> kMarkupOut = {"md", "html", "rst", "tex", "org", "txt", "epub", "docx", "odt", "pdf"};
const std::set<std::string> kVectorIn = {"svg"};
const std::set<std::string> kVectorOut = {"svg", "png", "jpg", "jpeg", "bmp", "tga", "ppm"};
const std::set<std::string> kMeshIn(mesh::readableExtensions().begin(), mesh::readableExtensions().end());
const std::set<std::string> kMeshOut(mesh::writableExtensions().begin(), mesh::writableExtensions().end());
const std::set<std::string> kMeshRender = {"png", "jpg", "jpeg", "bmp", "tga", "ppm"};  // mesh -> rendered preview
const std::set<std::string> k3dLater = {"fbx", "dae", "step", "stp", "iges", "igs", "usd", "usda", "usdc", "usdz", "blend", "3ds", "x3d", "wrl"};

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
    if (in(kStill, to)) args.insert(args.end(), {"-frames:v", "1"});      // video in, picture out: one frame
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
    fs::path tmp = fs::temp_directory_path(ec) / ("wetwaresuite-convert-" + std::to_string(std::hash<std::string>{}(input.string())));
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
    if (extOf(output) == "pdf" && soffice()) {
        // pandoc's own PDF needs LaTeX; going through .docx and LibreOffice needs nothing extra
        std::error_code ec;
        fs::path tmp = fs::temp_directory_path(ec) / ("wetwaresuite-md-" + std::to_string(std::hash<std::string>{}(input.string())));
        fs::create_directories(tmp, ec);
        fs::path docx = tmp / input.filename();
        docx.replace_extension(".docx");
        auto r = runProgram({*exe, input.string(), "-o", docx.string()});
        Result out = r.exitCode == 0 ? officeConvert(docx, output) : Result::failure("pandoc failed: " + r.output);
        fs::remove_all(tmp, ec);
        if (out.ok) out.message = "converted " + input.string() + " to " + output.string() + " with pandoc and LibreOffice";
        return out;
    }
    auto r = runProgram({*exe, input.string(), "-o", output.string()});
    if (r.exitCode != 0) return Result::failure("pandoc failed: " + r.output);
    return Result::success("converted " + input.string() + " to " + output.string() + " with pandoc");
}

Result meshConvert(const fs::path& input, const fs::path& output, const Params& o) {
    try {
        mesh::Mesh m = mesh::load(input);
        if (m.empty()) return Result::failure(input.string() + " has no triangles to convert");
        const double scale = numberParam(o, "scale", 1.0);
        if (scale <= 0) return Result::failure("scale must be positive");
        if (scale != 1.0) mesh::transform(m, mesh::Mat4::scale(scale));
        const std::string to = extOf(output);
        if (in(kMeshRender, to)) {
            mesh::RenderOptions ro;
            if (o.count("view")) {
                auto v = mesh::parseView(stringParam(o, "view"));
                if (!v) return Result::failure("unknown view '" + stringParam(o, "view") + "' (try iso, front, top, right)");
                ro.view = *v;
            }
            int w = static_cast<int>(intParam(o, "width", 0)), h = static_cast<int>(intParam(o, "height", 0));
            if (w <= 0 && h <= 0) w = h = 1024;
            if (w <= 0) w = h;
            if (h <= 0) h = w;
            if (w > 8192 || h > 8192) return Result::failure("renders are limited to 8192 pixels a side");
            raster::save(mesh::render(m, w, h, ro), output, static_cast<int>(intParam(o, "quality", 90)));
            return Result::success("rendered " + input.string() + " to " + output.string())
                .with("triangles", static_cast<std::int64_t>(m.triangleCount()));
        }
        mesh::SaveOptions so;
        so.ascii = boolParam(o, "ascii", false);
        mesh::save(output, m, so);
        return Result::success("converted " + input.string() + " to " + output.string())
            .with("triangles", static_cast<std::int64_t>(m.triangleCount()))
            .with("watertight", mesh::isWatertight(m));
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
}

Result vectorConvert(Session& s, const fs::path& input, const fs::path& output, const Params& o) {
    Params args = {{"input", input.string()}, {"output", output.string()}};
    if (o.count("scale")) args["scale"] = numberParam(o, "scale", 1.0);
    if (o.count("quality")) args["quality"] = intParam(o, "quality", 90);
    if (o.count("width")) args["width"] = intParam(o, "width", 0);
    if (o.count("height")) args["height"] = intParam(o, "height", 0);
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
    if (in(kMeshIn, from) && (in(kMeshOut, to) || in(kMeshRender, to))) return "mesh";
    if (in(kVectorIn, from) && in(kVectorOut, to) && session.registry().find("vector.convert")) return "vector";
    const bool mediaIn = in(kAudio, from) || in(kVideo, from) || in(kFfmpegImages, from) || in(kRasterIn, from);
    const bool mediaOut = in(kAudio, to) || in(kVideo, to) || in(kFfmpegImages, to) || in(kRasterOut, to);
    if (mediaIn && mediaOut && to != "ora" && from != "ora") {
        // video -> image only makes sense as a still; ffmpeg handles that too
        return "ffmpeg";
    }
    if (in(kOffice, from) && in(kOfficeOut, to)) return "libreoffice";
    if (in(kMarkup, from) && in(kMarkupOut, to)) {
        // rendered formats go straight to LibreOffice; plain-text markup needs pandoc to be laid out
        if (to == "pdf" && !in(kPlainMarkup, from) && soffice()) return "libreoffice";
        return "pandoc";
    }
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
    else if (backend == "mesh") r = meshConvert(input, output, o);
    else if (backend == "ffmpeg") r = ffmpegConvert(input, output, o);
    else if (backend == "libreoffice") r = officeConvert(input, output);
    else if (backend == "pandoc") r = pandocConvert(input, output);
    else if (in(k3dLater, from) || in(k3dLater, to)) {
        return Result::failure("." + (in(k3dLater, from) ? from : to) + " isn't supported yet; built-in 3D conversion covers " +
                               joined(kMeshIn) + " (run convert.formats for the list)");
    } else if (in(kMeshOut, to) || in(kMeshIn, from)) {
        return Result::failure("can't turn ." + from + " into ." + to + ": 3D meshes convert between " + joined(kMeshIn) +
                               ", and can be rendered to " + joined(kMeshRender));
    }
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
             "Convert one file to another format, picked from the output's extension. Images and 3D meshes "
             "(STL, OBJ, PLY, OFF, glTF, 3MF) use the built-in engines, and a mesh saved as .png renders a preview; "
             "SVG uses the vector tool; audio and video use ffmpeg; office documents use LibreOffice; "
             "markup uses pandoc. Run convert.formats to see what's available on this machine.",
             {{"input", ParamType::String, "file to convert", true},
              {"output", ParamType::String, "file to write; its extension sets the format, e.g. photo.jpg", true},
              {"quality", ParamType::Int, "1..100 for lossy formats (JPEG, MP3, video)", false},
              {"width", ParamType::Int, "resize images/video to this width (keeps aspect if height is unset); mesh renders default to 1024", false},
              {"height", ParamType::Int, "resize images/video to this height", false},
              {"scale", ParamType::Number, "SVG to raster: scale factor; meshes: scale the model (e.g. 25.4 for inches to mm)", false},
              {"ascii", ParamType::Bool, "meshes: write STL/PLY as text instead of binary", false, {}, false},
              {"view", ParamType::String, "mesh to image: camera", false, mesh::viewNames(), std::string("iso")},
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
             false, true},
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
                        "\n3D meshes (built in, ready): " + joined(kMeshIn) + " -> " + joined(kMeshOut) + ", or rendered to " +
                        joined(kMeshRender) +
                        "\nother 3D and CAD (" + joined(k3dLater) + "): coming with asset forge and cad";
                    return Result::success(text).with("mesh", true).with("ffmpeg", hasFfmpeg).with("libreoffice", hasOffice)
                        .with("pandoc", hasPandoc).with("vector", hasVector);
                });
            });

    reg.add({"convert.check",
             "Check conversion",
             "Say whether a file can be converted to a format and which backend would do it, without converting.",
             {{"input", ParamType::String, "file to convert", true}, {"to", ParamType::String, "target extension", true}},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    std::string to = lower(stringParam(p, "to"));
                    if (!to.empty() && to[0] == '.') to.erase(0, 1);
                    std::string b = backendFor(*s, stringParam(p, "input"), to);
                    if (b.empty()) return Result::failure("no converter to ." + to + " for " + stringParam(p, "input"));
                    bool ready = b == "raster" || b == "vector" || b == "mesh" || (b == "ffmpeg" && ffmpeg()) ||
                                 (b == "libreoffice" && soffice()) || (b == "pandoc" && pandoc());
                    return Result::success(b + (ready ? " can do it" : " would do it, but isn't installed"))
                        .with("backend", b).with("ready", ready);
                });
            });
}

}  // namespace wetwaresuite::convert
