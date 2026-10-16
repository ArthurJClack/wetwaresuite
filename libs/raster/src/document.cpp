#include "wetwaresuite/raster/document.hpp"

#include <algorithm>
#include <cctype>

#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::raster {

namespace {

std::string ext(const std::filesystem::path& path) {
    std::string e = path.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

}  // namespace

std::string ImageDoc::summary() const {
    std::string s = std::to_string(image.width) + "x" + std::to_string(image.height) + ", " +
                    std::to_string(image.layers.size()) + (image.layers.size() == 1 ? " layer" : " layers");
    if (!image.layers.empty()) s += ", active layer " + std::to_string(active) + " \"" + image.layers[active].name + "\"";
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> ImageDoc::preview() const {
    return std::make_unique<Attachment>(Attachment{"image/png", previewPng(flatten(), 512), "preview.png"});
}

std::shared_ptr<ImageDoc> newImage(int width, int height, Color background) {
    auto doc = std::make_shared<ImageDoc>();
    doc->image.width = width;
    doc->image.height = height;
    doc->image.layers.push_back({"Background", Image(width, height, background)});
    return doc;
}

bool canOpenImage(const std::filesystem::path& path) { return ext(path) == "ora" || canRead(path); }
bool canSaveImage(const std::filesystem::path& path) { return ext(path) == "ora" || canWrite(path); }

std::shared_ptr<ImageDoc> openImage(const std::filesystem::path& path) {
    auto doc = std::make_shared<ImageDoc>();
    if (ext(path) == "ora") {
        doc->image = loadOra(path);
    } else {
        Image img = load(path);
        doc->image.width = img.width();
        doc->image.height = img.height();
        doc->image.layers.push_back({"Background", std::move(img)});
    }
    doc->active = static_cast<int>(doc->image.layers.size()) - 1;
    doc->path = path.string();
    return doc;
}

void saveImage(const ImageDoc& doc, const std::filesystem::path& path, int quality) {
    if (ext(path) == "ora") saveOra(doc.image, path);
    else save(doc.flatten(), path, quality);
}

Result ImageEdit::apply() {
    before_ = doc_->image;
    beforeActive_ = doc_->active;
    Result r = fn_(*doc_);
    if (!r.ok) revert();  // failed edits leave the document untouched
    return r;
}

void ImageEdit::revert() {
    doc_->image = before_;
    doc_->active = beforeActive_;
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open image, e.g. img1", true}; }

ParamSpec layerParam() {
    return {"layer", ParamType::Int, "layer index, 0 = bottom (default: the active layer)", false};
}

ParamSpec previewParam() {
    return {"preview", ParamType::Bool, "attach a PNG preview of the result", false, {}, false};
}

ParamSpec colorParam(const std::string& name, const std::string& description, bool required, const std::string& fallback) {
    return {name, ParamType::String, description + " (#rrggbb, #rrggbbaa, rgb(r,g,b) or a name)", required, {},
            fallback.empty() ? Value{} : Value{fallback}};
}

ActionFactory imageCommand(Session& session, std::function<Result(ImageDoc&, const Params&)> edit, bool undoable) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<ImageDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open image called '" + id + "' (use photo.open or paint.new first)"); });
        }
        if (!undoable) return std::make_unique<FunctionAction>([doc, edit, p] { return edit(*doc, p); });
        return std::make_unique<ImageEdit>(doc, [edit, p](ImageDoc& d) { return edit(d, p); });
    };
}

int layerIndex(const ImageDoc& doc, const Params& p, std::string& error) {
    auto idx = intParam(p, "layer", doc.active);
    if (idx < 0 || idx >= static_cast<std::int64_t>(doc.image.layers.size())) {
        error = "layer " + std::to_string(idx) + " doesn't exist (this image has " +
                std::to_string(doc.image.layers.size()) + ", numbered from 0 at the bottom)";
        return -1;
    }
    return static_cast<int>(idx);
}

Result& maybePreview(Result& r, const ImageDoc& doc, const Params& p) {
    if (r.ok && boolParam(p, "preview", false)) r.attach(*doc.preview());
    return r;
}

bool colorArg(const Params& p, const std::string& key, const std::string& fallback, Color& out, std::string& error) {
    std::string text = stringParam(p, key, fallback);
    auto c = Color::parse(text);
    if (!c) {
        error = "'" + text + "' isn't a colour; use #rrggbb, #rrggbbaa, rgb(r,g,b) or a name like red";
        return false;
    }
    out = *c;
    return true;
}

}  // namespace wetwaresuite::raster
