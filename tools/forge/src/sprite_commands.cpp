// forge.sprite_sheet / forge.sprite_split: sheets, atlases and frames.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

#include "achclwss/forge/forge.hpp"
#include "achclwss/forge/texture.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/document.hpp"
#include "achclwss/raster/ops.hpp"
#include "common.hpp"
#include "images.hpp"

namespace achclwss::forge::detail {

using raster::ImageDoc;
using json = nlohmann::ordered_json;

namespace {

struct Frame {
    std::string name;
    Image image;     // what goes on the sheet (trimmed when trimming)
    int srcW = 0, srcH = 0;
    int offX = 0, offY = 0;  // where `image` sat in the original
};

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + ",") {
        if (c == ',') {
            const auto a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
            if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

json rectJson(int x, int y, int w, int h) { return json{{"x", x}, {"y", y}, {"w", w}, {"h", h}}; }

bool safeRelative(const fs::path& p) {
    if (p.empty() || p.is_absolute() || p.has_root_name()) return false;
    for (const auto& part : p) {
        if (part == "..") return false;
    }
    return true;
}

}  // namespace

void registerSpriteCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"forge.sprite_sheet",
             "Pack sprite sheet",
             "Pack a folder (or a list) of images into one sprite sheet PNG plus a JSON atlas of frame name -> x, y, "
             "w, h in the TexturePacker 'JSON hash' format that Phaser, PixiJS, Godot importers, Unity importers and "
             "most engines read. layout=packed shelf-packs tightly (tallest first); layout=grid puts every frame in "
             "equal cells (classic animation strips). Frames are ordered by name, walk_2 before walk_10. The sheet "
             "opens as a new image.",
             {{"input_dir", ParamType::String, "folder of frame images", false},
              {"files", ParamType::String, "comma-separated image files or open image ids, instead of input_dir", false},
              {"output", ParamType::String, "sheet image to write, e.g. hero.png", true},
              {"atlas", ParamType::String, "JSON atlas to write (default: output with .json)", false},
              {"layout", ParamType::String, "how to arrange frames", false, {"packed", "grid"}, std::string("packed")},
              {"columns", ParamType::Int, "grid: frames per row (0 = about square)", false, {}, std::int64_t{0}},
              {"padding", ParamType::Int, "empty px between frames and around the edge (default 2 packed, 0 grid)", false},
              {"pot", ParamType::Bool, "round the sheet size up to powers of two", false, {}, false},
              {"trim", ParamType::Bool, "packed: crop transparent borders off each frame (the atlas records the offsets)", false, {}, false},
              imagePreviewParam()},
             false},
            [s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([s, p]() -> Result {
                    const fs::path output = stringParam(p, "output");
                    if (lowerExt(output) != "png") return Result::failure("output should be a .png (sheets need transparency)");
                    std::vector<Frame> frames;
                    std::error_code ec;
                    if (p.count("files")) {
                        for (const auto& ref : splitList(stringParam(p, "files"))) {
                            std::string error;
                            auto img = resolveImage(*s, ref, error);
                            if (!img) return Result::failure(error);
                            std::string name = fs::path(ref).filename().string();
                            if (auto doc = s->workspace().getAs<ImageDoc>(ref)) {
                                name = doc->path.empty() ? ref + ".png" : fs::path(doc->path).filename().string();
                            }
                            frames.push_back({name, std::move(*img)});
                        }
                    } else if (p.count("input_dir")) {
                        const fs::path dir = stringParam(p, "input_dir");
                        if (!fs::is_directory(dir, ec)) return Result::failure("not a folder: " + dir.string());
                        std::vector<fs::path> files;
                        for (const auto& e : fs::directory_iterator(dir, ec)) {
                            if (!e.is_regular_file() || !raster::canOpenImage(e.path())) continue;
                            if (fs::exists(output, ec) && fs::equivalent(e.path(), output, ec)) continue;  // a previous sheet
                            files.push_back(e.path());
                        }
                        std::sort(files.begin(), files.end(),
                                  [](const fs::path& a, const fs::path& b) { return naturalLess(a.filename().string(), b.filename().string()); });
                        for (const auto& f : files) {
                            try {
                                frames.push_back({f.filename().string(), raster::openImage(f)->flatten()});
                            } catch (const std::exception& e) {
                                return Result::failure(f.string() + ": " + e.what());
                            }
                        }
                    } else {
                        return Result::failure("give input_dir= (a folder of frames) or files= (a comma-separated list)");
                    }
                    if (frames.empty()) return Result::failure("no images to pack");

                    // unique names
                    std::map<std::string, int> seen;
                    for (auto& f : frames) {
                        if (int n = seen[f.name]++; n > 0) f.name = fs::path(f.name).stem().string() + "_" + std::to_string(n) + fs::path(f.name).extension().string();
                    }
                    const bool grid = stringParam(p, "layout", "packed") == "grid";
                    const bool trim = boolParam(p, "trim", false) && !grid;
                    const int padding = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "padding", grid ? 0 : 2), 0, 256));
                    std::vector<PackItem> items;
                    for (auto& f : frames) {
                        f.srcW = f.image.width();
                        f.srcH = f.image.height();
                        if (trim) {
                            Rect r = opaqueBounds(f.image);
                            if (r.empty()) r = {0, 0, 1, 1};
                            f.offX = r.x;
                            f.offY = r.y;
                            f.image = raster::crop(f.image, r.x, r.y, r.w, r.h);
                        }
                        items.push_back({f.image.width(), f.image.height()});
                    }
                    const bool pot = boolParam(p, "pot", false);
                    PackResult pack = grid ? packGrid(items, static_cast<int>(intParam(p, "columns", 0)), padding, pot) : packShelf(items, padding, pot);
                    if (pack.width > 16384 || pack.height > 16384) {
                        return Result::failure("the sheet would be " + std::to_string(pack.width) + "x" + std::to_string(pack.height) +
                                               " px (limit 16384); split the frames into several sheets");
                    }
                    Image sheet(pack.width, pack.height);
                    json framesJson = json::object();
                    for (std::size_t i = 0; i < frames.size(); ++i) {
                        const auto& f = frames[i];
                        Rect r = pack.rects[i];
                        if (grid) {  // centred in its cell
                            r.x += (r.w - f.image.width()) / 2;
                            r.y += (r.h - f.image.height()) / 2;
                            r.w = f.image.width();
                            r.h = f.image.height();
                        }
                        blit(sheet, f.image, r.x, r.y);
                        const bool trimmed = f.image.width() != f.srcW || f.image.height() != f.srcH;
                        framesJson[f.name] = json{{"frame", rectJson(r.x, r.y, r.w, r.h)},
                                                  {"rotated", false},
                                                  {"trimmed", trimmed},
                                                  {"spriteSourceSize", rectJson(f.offX, f.offY, f.image.width(), f.image.height())},
                                                  {"sourceSize", json{{"w", f.srcW}, {"h", f.srcH}}}};
                    }
                    fs::path atlas = p.count("atlas") ? fs::path(stringParam(p, "atlas")) : fs::path(output).replace_extension(".json");
                    json doc = {{"frames", framesJson},
                                {"meta", json{{"app", "achclwss forge"},
                                              {"version", "1.0"},
                                              {"image", output.filename().string()},
                                              {"format", "RGBA8888"},
                                              {"size", json{{"w", pack.width}, {"h", pack.height}}},
                                              {"scale", "1"}}}};
                    makeParent(atlas);
                    {
                        std::ofstream out(atlas, std::ios::binary);
                        if (!out) return Result::failure("can't write " + atlas.string());
                        out << doc.dump(2) << "\n";
                    }
                    Result r = Result::success("packed " + std::to_string(frames.size()) + " frames into " + std::to_string(pack.width) + "x" +
                                               std::to_string(pack.height) + " " + output.string() + " + " + atlas.string());
                    r.with("frames", static_cast<std::int64_t>(frames.size())).with("atlas", atlas.string());
                    return generated(*s, std::move(sheet), std::move(r), p);
                });
            });

    reg.add({"forge.sprite_split",
             "Split sprite sheet",
             "Cut a sprite sheet into numbered frame files (<prefix>_00.png, _01, ... row by row), by columns/rows "
             "or by cell size, with optional padding between cells; or by a JSON atlas (TexturePacker hash or array, "
             "e.g. one forge.sprite_sheet wrote), which restores the original names and untrimmed sizes.",
             {docSourceParam(), inputParam(),
              {"output_dir", ParamType::String, "folder to write frames into", true},
              {"cols", ParamType::Int, "frames across", false},
              {"rows", ParamType::Int, "frames down (default 1 when cell_height isn't given)", false},
              {"cell_width", ParamType::Int, "frame width in px, instead of cols", false},
              {"cell_height", ParamType::Int, "frame height in px, instead of rows", false},
              {"padding", ParamType::Int, "empty px between cells and around the edge", false, {}, std::int64_t{0}},
              {"atlas", ParamType::String, "JSON atlas to cut by instead of a grid", false},
              {"prefix", ParamType::String, "file name start (default: the sheet's name)", false},
              {"skip_empty", ParamType::Bool, "don't write fully transparent frames", false, {}, true},
              {"format", ParamType::String, "frame file format", false, {"png", "tga", "bmp"}, std::string("png")}},
             false},
            [s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([s, p]() -> Result {
                    std::string error, sheetName;
                    auto src = sourceImage(*s, p, error, &sheetName);
                    if (!src) return Result::failure(error);
                    const Image& sheet = *src;
                    const fs::path dir = stringParam(p, "output_dir");
                    const std::string format = stringParam(p, "format", "png");
                    const std::string prefix = stringParam(p, "prefix", sheetName.empty() ? "frame" : sheetName);
                    const bool skipEmpty = boolParam(p, "skip_empty", true);
                    std::error_code ec;
                    fs::create_directories(dir, ec);
                    if (ec) return Result::failure("can't create " + dir.string() + ": " + ec.message());
                    std::vector<std::string> written;
                    int skipped = 0;

                    auto emit = [&](const Image& frame, const fs::path& rel) -> std::string {
                        if (skipEmpty && opaqueBounds(frame).empty()) {
                            ++skipped;
                            return {};
                        }
                        const fs::path file = dir / rel;
                        if (auto err = writeImage(frame, file.string()); !err.empty()) return err;
                        written.push_back(file.string());
                        return {};
                    };

                    if (p.count("atlas")) {
                        const std::string atlasPath = stringParam(p, "atlas");
                        std::ifstream in(atlasPath, std::ios::binary);
                        if (!in) return Result::failure("can't read atlas " + atlasPath);
                        json a = json::parse(in, nullptr, false);
                        if (a.is_discarded() || !a.contains("frames")) return Result::failure(atlasPath + " isn't a JSON atlas with \"frames\"");
                        std::vector<std::pair<std::string, json>> entries;
                        if (a["frames"].is_object()) {
                            for (auto it = a["frames"].begin(); it != a["frames"].end(); ++it) entries.emplace_back(it.key(), it.value());
                        } else if (a["frames"].is_array()) {
                            for (const auto& f : a["frames"]) entries.emplace_back(f.value("filename", std::string("frame")), f);
                        }
                        for (const auto& [name, f] : entries) {
                            if (!f.contains("frame")) continue;
                            const json& fr = f["frame"];
                            const int x = fr.value("x", 0), y = fr.value("y", 0), w = fr.value("w", 0), h = fr.value("h", 0);
                            if (w <= 0 || h <= 0) continue;
                            const bool rotated = f.value("rotated", false);
                            Image img = rotated ? raster::rotate90(raster::crop(sheet, x, y, h, w), -1) : raster::crop(sheet, x, y, w, h);
                            if (f.value("trimmed", false) && f.contains("sourceSize") && f.contains("spriteSourceSize")) {
                                Image full(f["sourceSize"].value("w", w), f["sourceSize"].value("h", h));
                                blit(full, img, f["spriteSourceSize"].value("x", 0), f["spriteSourceSize"].value("y", 0));
                                img = std::move(full);
                            }
                            fs::path rel = fs::path(name).replace_extension("." + format);
                            if (!safeRelative(rel)) return Result::failure("atlas frame name '" + name + "' points outside output_dir");
                            if (auto err = emit(img, rel); !err.empty()) return Result::failure(err);
                        }
                    } else {
                        const int pad = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "padding", 0), 0, 4096));
                        int cw = static_cast<int>(intParam(p, "cell_width", 0)), ch = static_cast<int>(intParam(p, "cell_height", 0));
                        int cols = static_cast<int>(intParam(p, "cols", 0)), rows = static_cast<int>(intParam(p, "rows", 0));
                        if (cw <= 0 && cols <= 0) return Result::failure("give cols= or cell_width=");
                        if (ch <= 0 && rows <= 0) rows = 1;
                        if (cw > 0) cols = (sheet.width() - pad) / (cw + pad);
                        else cw = (sheet.width() - pad * (cols + 1)) / cols;
                        if (ch > 0) rows = (sheet.height() - pad) / (ch + pad);
                        else ch = (sheet.height() - pad * (rows + 1)) / rows;
                        if (cw < 1 || ch < 1 || cols < 1 || rows < 1) return Result::failure("those cells don't fit in a " + std::to_string(sheet.width()) + "x" + std::to_string(sheet.height()) + " sheet");
                        const int total = cols * rows;
                        const int digits = std::max(2, static_cast<int>(std::to_string(total - 1).size()));
                        int index = 0;
                        for (int r = 0; r < rows; ++r) {
                            for (int c = 0; c < cols; ++c) {
                                Image frame = raster::crop(sheet, pad + c * (cw + pad), pad + r * (ch + pad), cw, ch);
                                if (skipEmpty && opaqueBounds(frame).empty()) {
                                    ++skipped;
                                    continue;
                                }
                                std::string n = std::to_string(index++);
                                n.insert(0, static_cast<std::size_t>(std::max(0, digits - static_cast<int>(n.size()))), '0');
                                if (auto err = emit(frame, prefix + "_" + n + "." + format); !err.empty()) return Result::failure(err);
                            }
                        }
                    }
                    if (written.empty()) return Result::failure("no frames written (all empty?)");
                    Result r = Result::success("wrote " + std::to_string(written.size()) + " frames to " + dir.string() +
                                               (skipped ? " (" + std::to_string(skipped) + " empty skipped)" : ""));
                    return r.with("frames", static_cast<std::int64_t>(written.size()))
                        .with("skipped", static_cast<std::int64_t>(skipped))
                        .with("output_dir", dir.string())
                        .with("first", written.front())
                        .with("last", written.back());
                });
            });
}

}  // namespace achclwss::forge::detail
