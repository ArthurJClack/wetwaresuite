// forge texture commands: normal maps, tiling, pixel art, engine prep.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "achclwss/forge/forge.hpp"
#include "achclwss/forge/texture.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/document.hpp"
#include "achclwss/raster/ops.hpp"
#include "common.hpp"
#include "images.hpp"

namespace achclwss::forge::detail {

using raster::ImageDoc;

// ---- shared helpers (images.hpp) ----

std::optional<Image> resolveImage(Session& s, const std::string& ref, std::string& error) {
    if (auto doc = s.workspace().getAs<ImageDoc>(ref)) return doc->flatten();
    std::error_code ec;
    if (!fs::exists(ref, ec)) {
        error = "'" + ref + "' is neither an open image nor a file";
        return std::nullopt;
    }
    if (!raster::canOpenImage(ref)) {
        error = "can't read " + ref + ": unsupported image format";
        return std::nullopt;
    }
    try {
        return raster::openImage(ref)->flatten();
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

std::optional<Image> sourceImage(Session& s, const Params& p, std::string& error, std::string* name) {
    std::string ref;
    if (p.count("doc")) {
        ref = stringParam(p, "doc");
        if (!s.workspace().getAs<ImageDoc>(ref)) {
            error = "no open image called '" + ref + "' (use photo.open first, or input= for a file)";
            return std::nullopt;
        }
    } else if (p.count("input")) {
        ref = stringParam(p, "input");
    } else {
        error = "give doc= (an image opened with photo.open) or input= (an image file)";
        return std::nullopt;
    }
    if (name) {
        auto doc = s.workspace().getAs<ImageDoc>(ref);
        *name = doc ? (doc->path.empty() ? ref : fs::path(doc->path).stem().string()) : fs::path(ref).stem().string();
    }
    return resolveImage(s, ref, error);
}

std::string writeImage(const Image& img, const std::string& path) {
    if (!raster::canWrite(path)) return "can't write ." + lowerExt(path) + "; use one of: " + joinList(raster::writableExtensions());
    makeParent(path);
    try {
        raster::save(img, path);
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

Result generated(Session& s, Image img, Result r, const Params& p, bool previewByDefault) {
    if (p.count("output")) {
        const std::string out = stringParam(p, "output");
        if (auto err = writeImage(img, out); !err.empty()) return Result::failure(err);
        r.with("output", out);
    }
    auto doc = std::make_shared<ImageDoc>();
    doc->image.width = img.width();
    doc->image.height = img.height();
    doc->image.layers.push_back({"Background", img});
    if (p.count("output")) doc->path = stringParam(p, "output");
    const std::string id = s.workspace().add(doc, "img");
    r.message += (r.message.empty() ? "" : " ") + std::string("(opened as ") + id + ")";
    r.with("doc", id).with("width", static_cast<std::int64_t>(img.width())).with("height", static_cast<std::int64_t>(img.height()));
    if (boolParam(p, "preview", previewByDefault)) r.attach({"image/png", raster::previewPng(img, 512), id + ".png"});
    return r;
}

ParamSpec docSourceParam() { return {"doc", ParamType::String, "id of an open image to read (from photo.open)", false}; }
ParamSpec inputParam() { return {"input", ParamType::String, "image file to read instead of doc", false}; }
ParamSpec outputParam(const std::string& description) { return {"output", ParamType::String, description, false}; }
ParamSpec imagePreviewParam(bool fallback) {
    return {"preview", ParamType::Bool, "attach a PNG preview of the result", false, {}, fallback};
}

namespace {

using ImageFn = std::function<Result(Image&, const Params&)>;

ParamSpec editDocParam() {
    return {"doc", ParamType::String, "id of an open image to edit in place (from photo.open); layers are flattened", false};
}

Result finish(Result r, const Image& img, const Params& p, const std::string& docId) {
    if (!docId.empty()) r.with("doc", docId);
    r.with("width", static_cast<std::int64_t>(img.width())).with("height", static_cast<std::int64_t>(img.height()));
    if (p.count("output")) r.with("output", stringParam(p, "output"));
    if (boolParam(p, "preview", false)) r.attach({"image/png", raster::previewPng(img, 512), "preview.png"});
    return r;
}

// A command that changes one image: an open document in place (undoable,
// layers flattened), or input= file -> output= file.
ActionFactory filterCommand(Session& session, ImageFn fn) {
    return [&session, fn](const Params& p) -> std::unique_ptr<Action> {
        auto fail = [](std::string m) { return std::make_unique<FunctionAction>([m] { return Result::failure(m); }); };
        if (p.count("doc")) {
            const std::string id = stringParam(p, "doc");
            auto doc = session.workspace().getAs<ImageDoc>(id);
            if (!doc) return fail("no open image called '" + id + "' (use photo.open first, or input= for a file)");
            return std::make_unique<raster::ImageEdit>(doc, [fn, p, id](ImageDoc& d) {
                Image img = d.flatten();
                Result r;
                try {
                    r = fn(img, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
                if (!r.ok) return r;
                if (p.count("output")) {
                    if (auto err = writeImage(img, stringParam(p, "output")); !err.empty()) return Result::failure(err);
                }
                const std::string name = d.image.layers.size() == 1 ? d.image.layers[0].name : "Background";
                d.image.layers.clear();
                d.image.layers.push_back({name, img});
                d.image.width = img.width();
                d.image.height = img.height();
                d.active = 0;
                return finish(std::move(r), img, p, id);
            });
        }
        if (p.count("input")) {
            if (!p.count("output")) return fail("give output= for the result (or open the image with photo.open and pass doc=)");
            return std::make_unique<FunctionAction>([fn, p] {
                const std::string in = stringParam(p, "input");
                if (!raster::canOpenImage(in)) return Result::failure("can't read " + in + ": unsupported image format");
                try {
                    Image img = raster::openImage(in)->flatten();
                    Result r = fn(img, p);
                    if (!r.ok) return r;
                    if (auto err = writeImage(img, stringParam(p, "output")); !err.empty()) return Result::failure(err);
                    return finish(std::move(r), img, p, "");
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return fail("give doc= (an image opened with photo.open) or input= and output= (files)");
    };
}

// A command that reads one image and makes a new one (opened as a new document).
ActionFactory generatorCommand(Session& session, std::function<Result(const Image&, Image&, const Params&)> fn, bool previewByDefault = false) {
    return [&session, fn, previewByDefault](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([&session, fn, p, previewByDefault] {
            std::string error;
            auto src = sourceImage(session, p, error);
            if (!src) return Result::failure(error);
            Image out;
            Result r;
            try {
                r = fn(*src, out, p);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
            if (!r.ok) return r;
            return generated(session, std::move(out), std::move(r), p, previewByDefault);
        });
    };
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3g", v);
    return buf;
}

HeightSource heightSource(const std::string& s) {
    if (s == "red") return HeightSource::Red;
    if (s == "green") return HeightSource::Green;
    if (s == "blue") return HeightSource::Blue;
    if (s == "alpha") return HeightSource::Alpha;
    return HeightSource::Luma;
}

}  // namespace

void registerImageCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"forge.normal_map",
             "Normal map from height",
             "Make a tangent-space normal map from a height or greyscale image (white = raised) with a Sobel filter. "
             "OpenGL style (green up: Blender, Godot, Unity) by default; invert_y=true for DirectX style (Unreal). "
             "The result opens as a new image.",
             {docSourceParam(), inputParam(), outputParam("file to write the normal map to, e.g. brick_n.png"),
              {"strength", ParamType::Number, "bumpiness; 1 subtle, 2 default, 5+ strong", false, {}, 2.0},
              {"invert_y", ParamType::Bool, "DirectX convention (green down)", false, {}, false},
              {"wrap", ParamType::Bool, "sample across edges as if tiled (keep on for tileable textures)", false, {}, true},
              {"channel", ParamType::String, "which channel is the height", false, {"luma", "red", "green", "blue", "alpha"}, std::string("luma")},
              imagePreviewParam()},
             false},
            generatorCommand(session, [](const Image& src, Image& out, const Params& p) {
                NormalMapOptions o;
                o.strength = numberParam(p, "strength", 2.0);
                o.invertY = boolParam(p, "invert_y", false);
                o.wrap = boolParam(p, "wrap", true);
                o.source = heightSource(stringParam(p, "channel", "luma"));
                if (o.strength < 0) return Result::failure("strength can't be negative");
                out = normalMap(src, o);
                return Result::success(std::string("normal map made (") + (o.invertY ? "DirectX" : "OpenGL") + ", strength " + num(o.strength) + ")");
            }));

    reg.add({"forge.seamless",
             "Make tileable",
             "Make a texture tile without visible seams: blends it with a copy offset by half its size so the edges "
             "wrap around. Wider blends hide seams better but ghost more. Check with forge.tile_preview.",
             {editDocParam(), inputParam(), outputParam(),
              {"blend", ParamType::Number, "transition width at each edge, as a fraction of the size (0.02..0.5)", false, {}, 0.25},
              imagePreviewParam()},
             true},
            filterCommand(session, [](Image& img, const Params& p) {
                const double before = seamError(img);
                img = makeSeamless(img, numberParam(p, "blend", 0.25));
                const double after = seamError(img);
                return Result::success("made tileable (edge difference " + num(before) + " -> " + num(after) + ")")
                    .with("seam_before", before)
                    .with("seam_after", after);
            }));

    reg.add({"forge.tile_preview",
             "Tile preview",
             "Repeat a texture in a grid (3x3 by default) to check that it tiles. Attaches the picture; output= saves "
             "it. The result opens as a new image.",
             {docSourceParam(), inputParam(), outputParam(),
              {"tiles", ParamType::Int, "repeats across and down", false, {}, std::int64_t{3}},
              imagePreviewParam(true)},
             false},
            generatorCommand(
                session,
                [](const Image& src, Image& out, const Params& p) {
                    const int n = static_cast<int>(intParam(p, "tiles", 3));
                    if (n < 1 || n > 16) return Result::failure("tiles must be 1..16");
                    if (static_cast<std::int64_t>(src.width()) * n > 30000 || static_cast<std::int64_t>(src.height()) * n > 30000) {
                        return Result::failure("the preview would be larger than 30000 px");
                    }
                    out = tile(src, n, n);
                    return Result::success(std::to_string(n) + "x" + std::to_string(n) + " tile preview (edge difference " + num(seamError(src)) + ")")
                        .with("seam", seamError(src));
                },
                true));

    reg.add({"forge.palette",
             "Reduce palette",
             "Pixel-art colour reduction: map the image to at most `colors` colours picked by median cut or k-means "
             "(slower, often nicer), or to a palette you give. Optional Floyd-Steinberg dithering. Transparency is kept.",
             {editDocParam(), inputParam(), outputParam(),
              {"colors", ParamType::Int, "how many colours (2..256)", false, {}, std::int64_t{16}},
              {"method", ParamType::String, "how to pick colours", false, {"median_cut", "kmeans"}, std::string("median_cut")},
              {"dither", ParamType::Bool, "Floyd-Steinberg error diffusion (smoother gradients, noisier pixels)", false, {}, false},
              {"palette", ParamType::String, "use these colours instead, comma separated, e.g. #000000,#ffffff,#ff004d", false},
              imagePreviewParam()},
             true},
            filterCommand(session, [](Image& img, const Params& p) {
                std::vector<Color> pal;
                if (p.count("palette")) {
                    std::string item;
                    for (char c : stringParam(p, "palette") + ",") {
                        if (c != ',') {
                            item += c;
                            continue;
                        }
                        if (item.find_first_not_of(" \t") == std::string::npos) {
                            item.clear();
                            continue;
                        }
                        auto col = Color::parse(item);
                        if (!col) return Result::failure("'" + item + "' isn't a colour");
                        pal.push_back(*col);
                        item.clear();
                    }
                    if (pal.empty()) return Result::failure("palette has no colours");
                } else {
                    const auto n = intParam(p, "colors", 16);
                    if (n < 2 || n > 256) return Result::failure("colors must be 2..256");
                    pal = buildPalette(img, static_cast<int>(n), stringParam(p, "method", "median_cut") == "kmeans" ? QuantizeMethod::KMeans : QuantizeMethod::MedianCut);
                }
                const auto before = countColors(img);
                applyPalette(img, pal, boolParam(p, "dither", false));
                const auto after = countColors(img);
                std::vector<std::string> hex;
                for (const auto& c : pal) hex.push_back(c.hex().substr(0, 7));
                return Result::success("reduced " + std::to_string(before) + " colours to " + std::to_string(after))
                    .with("colors_before", static_cast<std::int64_t>(before))
                    .with("colors", static_cast<std::int64_t>(after))
                    .with("palette", joinList(hex, ","));
            }));

    reg.add({"forge.upscale",
             "Upscale pixel art",
             "Enlarge by a whole-number factor with nearest-neighbour sampling, so pixel art stays crisp.",
             {editDocParam(), inputParam(), outputParam(),
              {"factor", ParamType::Int, "2 = twice the size (1..16)", false, {}, std::int64_t{2}},
              imagePreviewParam()},
             true},
            filterCommand(session, [](Image& img, const Params& p) {
                const auto f = intParam(p, "factor", 2);
                if (f < 1 || f > 16) return Result::failure("factor must be 1..16");
                if (img.width() * f > 30000 || img.height() * f > 30000) return Result::failure("result would be larger than 30000 px");
                img = upscale(img, static_cast<int>(f));
                return Result::success("upscaled " + std::to_string(f) + "x to " + std::to_string(img.width()) + "x" + std::to_string(img.height()));
            }));

    reg.add({"forge.outline",
             "Outline sprite",
             "Draw an outline around a sprite's visible pixels (on transparent pixels next to them). expand=true "
             "grows the canvas so the outline is never cut off at the edges.",
             {editDocParam(), inputParam(), outputParam(),
              raster::colorParam("color", "outline colour", false, "#000000"),
              {"thickness", ParamType::Int, "outline width in px (1..32)", false, {}, std::int64_t{1}},
              {"diagonal", ParamType::Bool, "include diagonal neighbours (rounder, heavier corners)", false, {}, false},
              {"threshold", ParamType::Int, "alpha at or above this counts as solid (1..255)", false, {}, std::int64_t{128}},
              {"expand", ParamType::Bool, "grow the canvas by the thickness on every side", false, {}, false},
              imagePreviewParam()},
             true},
            filterCommand(session, [](Image& img, const Params& p) {
                Color c;
                std::string error;
                if (!raster::colorArg(p, "color", "#000000", c, error)) return Result::failure(error);
                const auto t = intParam(p, "thickness", 1), th = intParam(p, "threshold", 128);
                if (t < 1 || t > 32) return Result::failure("thickness must be 1..32");
                if (th < 1 || th > 255) return Result::failure("threshold must be 1..255");
                img = outline(img, c, static_cast<int>(t), boolParam(p, "diagonal", false), static_cast<int>(th), boolParam(p, "expand", false));
                return Result::success(std::to_string(t) + " px outline added");
            }));

    reg.add({"forge.resize_pot",
             "Resize to power of two",
             "Resize a texture so both sides are powers of two (256, 512, 1024...), which older engines, mobile GPUs "
             "and mipmapping want.",
             {editDocParam(), inputParam(), outputParam(),
              {"mode", ParamType::String, "which power of two", false, {"nearest", "up", "down"}, std::string("nearest")},
              {"square", ParamType::Bool, "make both sides the same (the larger one)", false, {}, false},
              {"filter", ParamType::String, "resampling filter (nearest for pixel art)", false, {"bilinear", "nearest"}, std::string("bilinear")},
              imagePreviewParam()},
             true},
            filterCommand(session, [](Image& img, const Params& p) {
                const std::string mode = stringParam(p, "mode", "nearest");
                auto pick = [&](std::int64_t v) {
                    if (mode == "up") return nextPowerOfTwo(v);
                    if (mode == "down") return std::max<std::int64_t>(1, nextPowerOfTwo(v) == v ? v : nextPowerOfTwo(v) / 2);
                    return nearestPowerOfTwo(v);
                };
                std::int64_t w = pick(img.width()), h = pick(img.height());
                if (boolParam(p, "square", false)) w = h = std::max(w, h);
                if (w > 16384 || h > 16384) return Result::failure("result would be larger than 16384 px");
                const int ow = img.width(), oh = img.height();
                if (w != ow || h != oh) {
                    img = raster::resize(img, static_cast<int>(w), static_cast<int>(h),
                                         stringParam(p, "filter", "bilinear") == "nearest" ? raster::Filter::Nearest : raster::Filter::Bilinear);
                }
                if (w == ow && h == oh) return Result::success("already " + std::to_string(w) + "x" + std::to_string(h) + ", powers of two");
                return Result::success("resized " + std::to_string(ow) + "x" + std::to_string(oh) + " -> " + std::to_string(w) + "x" + std::to_string(h));
            }));

    reg.add({"forge.trim",
             "Trim transparent edges",
             "Crop away fully transparent borders. Reports where the kept area was (x, y) so a sprite can be placed "
             "back exactly.",
             {editDocParam(), inputParam(), outputParam(),
              {"threshold", ParamType::Int, "pixels with alpha at or below this count as empty", false, {}, std::int64_t{0}},
              {"padding", ParamType::Int, "keep this many empty px around the content", false, {}, std::int64_t{0}},
              imagePreviewParam()},
             true},
            filterCommand(session, [](Image& img, const Params& p) {
                const int th = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "threshold", 0), 0, 254));
                const int pad = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "padding", 0), 0, 4096));
                Rect r = opaqueBounds(img, th);
                if (r.empty()) return Result::failure("the image is fully transparent; nothing to keep");
                const int ow = img.width(), oh = img.height();
                const int x0 = std::max(0, r.x - pad), y0 = std::max(0, r.y - pad);
                const int x1 = std::min(ow, r.x + r.w + pad), y1 = std::min(oh, r.y + r.h + pad);
                img = raster::crop(img, x0, y0, x1 - x0, y1 - y0);
                return Result::success("trimmed " + std::to_string(ow) + "x" + std::to_string(oh) + " to " + std::to_string(img.width()) + "x" +
                                       std::to_string(img.height()) + " at (" + std::to_string(x0) + ", " + std::to_string(y0) + ")")
                    .with("x", static_cast<std::int64_t>(x0))
                    .with("y", static_cast<std::int64_t>(y0))
                    .with("original_width", static_cast<std::int64_t>(ow))
                    .with("original_height", static_cast<std::int64_t>(oh));
            }));

    reg.add({"forge.channel_pack",
             "Pack channels",
             "Combine up to four greyscale maps into the R, G, B and A channels of one texture, e.g. an ORM map "
             "(red = ambient occlusion, green = roughness, blue = metallic) for glTF/Unreal. Each source is an open "
             "image id or a file; its brightness is used. Sizes follow the first source; others are resized. Missing "
             "channels are filled with the *_value numbers.",
             {{"red", ParamType::String, "source for red (image id or file)", false},
              {"green", ParamType::String, "source for green", false},
              {"blue", ParamType::String, "source for blue", false},
              {"alpha", ParamType::String, "source for alpha", false},
              {"red_value", ParamType::Int, "red when there's no source (0..255)", false, {}, std::int64_t{0}},
              {"green_value", ParamType::Int, "green when there's no source", false, {}, std::int64_t{0}},
              {"blue_value", ParamType::Int, "blue when there's no source", false, {}, std::int64_t{0}},
              {"alpha_value", ParamType::Int, "alpha when there's no source", false, {}, std::int64_t{255}},
              outputParam("file to write the packed texture to, e.g. rock_orm.png"),
              imagePreviewParam()},
             false},
            [s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([s, p] {
                    const char* names[4] = {"red", "green", "blue", "alpha"};
                    std::optional<Image> src[4];
                    int w = 0, h = 0;
                    for (int c = 0; c < 4; ++c) {
                        if (!p.count(names[c])) continue;
                        std::string error;
                        src[c] = resolveImage(*s, stringParam(p, names[c]), error);
                        if (!src[c]) return Result::failure(std::string(names[c]) + ": " + error);
                        if (w == 0) {
                            w = src[c]->width();
                            h = src[c]->height();
                        }
                    }
                    if (w == 0) return Result::failure("give at least one of red=, green=, blue=, alpha=");
                    Image out(w, h);
                    std::string used;
                    for (int c = 0; c < 4; ++c) {
                        const auto fillValue = static_cast<std::uint8_t>(
                            std::clamp<std::int64_t>(intParam(p, std::string(names[c]) + "_value", c == 3 ? 255 : 0), 0, 255));
                        std::vector<std::uint8_t> values;
                        if (src[c]) {
                            Image img = *src[c];
                            if (img.width() != w || img.height() != h) img = raster::resize(img, w, h);
                            values = channel(img, HeightSource::Luma);
                            used += std::string(used.empty() ? "" : ", ") + names[c];
                        }
                        std::uint8_t* px = out.data();
                        for (std::size_t i = 0, n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h); i < n; ++i) {
                            px[i * 4 + static_cast<std::size_t>(c)] = values.empty() ? fillValue : values[i];
                        }
                    }
                    return generated(*s, std::move(out), Result::success("packed " + used + " into one " + std::to_string(w) + "x" + std::to_string(h) + " texture"), p);
                });
            });
}

}  // namespace achclwss::forge::detail
