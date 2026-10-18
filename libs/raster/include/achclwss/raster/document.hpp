// The open-image document photo and paint both edit, plus the helpers their
// commands share: snapshot undo, doc/layer params, previews.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "achclwss/core/command.hpp"
#include "achclwss/raster/ora.hpp"

namespace achclwss::raster {

class ImageDoc : public Document {
public:
    LayeredImage image;
    int active = 0;  // index of the layer edits go to

    std::string kind() const override { return "image"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;

    Image flatten() const { return composite(image.layers, image.width, image.height); }
    Layer& activeLayer() { return image.layers.at(static_cast<std::size_t>(active)); }
};

std::shared_ptr<ImageDoc> newImage(int width, int height, Color background);
// .ora keeps layers; anything else stb reads opens as one "Background" layer.
std::shared_ptr<ImageDoc> openImage(const std::filesystem::path& path);
// .ora keeps layers; other formats are flattened.
void saveImage(const ImageDoc& doc, const std::filesystem::path& path, int quality = 90);
bool canOpenImage(const std::filesystem::path& path);
bool canSaveImage(const std::filesystem::path& path);

// Undoable edit: snapshots the document before running `fn`, restores it on undo.
class ImageEdit : public Action {
public:
    ImageEdit(std::shared_ptr<ImageDoc> doc, std::function<Result(ImageDoc&)> fn)
        : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<ImageDoc> doc_;
    std::function<Result(ImageDoc&)> fn_;
    LayeredImage before_;
    int beforeActive_ = 0;
};

// Shared param specs.
ParamSpec docParam();
ParamSpec layerParam();    // optional index, 0 = bottom; defaults to the active layer
ParamSpec previewParam();  // optional: attach a PNG preview to the result
ParamSpec colorParam(const std::string& name, const std::string& description, bool required, const std::string& fallback);

// Builds an action factory for a command that edits one image document. `edit` runs inside an
// ImageEdit (or plain, when undoable is false); missing docs become a failed result, not a crash.
ActionFactory imageCommand(Session& session, std::function<Result(ImageDoc&, const Params&)> edit, bool undoable = true);

// Resolves the "layer" param; returns -1 and sets error when out of range.
int layerIndex(const ImageDoc& doc, const Params& p, std::string& error);

// Adds a preview attachment when the "preview" param is true.
Result& maybePreview(Result& r, const ImageDoc& doc, const Params& p);

// Parses a colour param or produces a readable error.
bool colorArg(const Params& p, const std::string& key, const std::string& fallback, Color& out, std::string& error);

}  // namespace achclwss::raster
