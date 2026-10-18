#include "wetwaresuite/raster/document.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <stdexcept>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/parallel.hpp"
#include "wetwaresuite/raster/selection.hpp"

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

void ImageDoc::touch(int layer, int x, int y, int width, int height) {
    if (layer < 0 || layer >= static_cast<int>(image.layers.size())) return;
    const Image& img = image.layers[static_cast<std::size_t>(layer)].image;
    PixelRect r{std::max(0, x), std::max(0, y), std::min(img.width(), x + width), std::min(img.height(), y + height)};
    if (r.empty()) return;
    if (recording_) recording_->save(layer, img, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
    markChanged(r);
}

ImageDoc::Changes ImageDoc::takeChanges() {
    Changes c = changes_;
    changes_ = {};
    return c;
}

namespace {

std::atomic<std::size_t> gUndoBytes{0};

bool sameShape(const LayeredImage& a, const LayeredImage& b) {
    if (a.width != b.width || a.height != b.height || a.layers.size() != b.layers.size()) return false;
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        if (a.layers[i].image.width() != b.layers[i].image.width() || a.layers[i].image.height() != b.layers[i].image.height()) {
            return false;
        }
    }
    return true;
}

std::vector<std::pair<int, int>> layerSizes(const LayeredImage& img) {
    std::vector<std::pair<int, int>> v{{img.width, img.height}};
    for (const auto& l : img.layers) v.emplace_back(l.image.width(), l.image.height());
    return v;
}

}  // namespace

std::size_t undoMemoryBytes() { return gUndoBytes.load(); }

ImageEdit::~ImageEdit() { account(0); }

void ImageEdit::account(std::size_t bytes) {
    gUndoBytes += bytes;
    gUndoBytes -= bytes_;
    bytes_ = bytes;
}

void ImageEdit::clear() {
    patches_.clear();
    patches_.shrink_to_fit();
    whole_.reset();
    masks_.clear();
    selection_.reset();
    account(0);
}

void ImageEdit::save(int layer, const Image& from, int x, int y, int w, int h) {
    Patch p{layer, x, y, w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
    for (int row = 0; row < h; ++row) std::copy_n(from.pixel(x, y + row), static_cast<std::size_t>(w) * 4, p.px.data() + static_cast<std::size_t>(row) * w * 4);
    account(bytes_ + p.px.size() + sizeof(Patch));
    patches_.push_back(std::move(p));
}

void ImageEdit::diff(const LayeredImage& before) {
    const LayeredImage& now = doc_->image;
    struct Job {
        int layer, ty;
    };
    std::vector<Job> jobs;
    for (std::size_t i = 0; i < now.layers.size(); ++i) {
        const int rows = (now.layers[i].image.height() + kTile - 1) / kTile;
        for (int ty = 0; ty < rows; ++ty) jobs.push_back({static_cast<int>(i), ty});
    }
    // which tiles differ, found across threads; the old tiles are then copied in a fixed order
    std::vector<std::vector<int>> changed(jobs.size());
    parallelRows(static_cast<int>(jobs.size()), static_cast<std::size_t>(std::max(1, now.width)) * kTile, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            const Image& a = before.layers[static_cast<std::size_t>(jobs[j].layer)].image;
            const Image& c = now.layers[static_cast<std::size_t>(jobs[j].layer)].image;
            const int y0 = jobs[j].ty * kTile, y1 = std::min(c.height(), y0 + kTile);
            for (int x0 = 0; x0 < c.width(); x0 += kTile) {
                const std::size_t n = static_cast<std::size_t>(std::min(kTile, c.width() - x0)) * 4;
                for (int y = y0; y < y1; ++y) {
                    if (std::memcmp(a.pixel(x0, y), c.pixel(x0, y), n) != 0) {
                        changed[static_cast<std::size_t>(j)].push_back(x0);
                        break;
                    }
                }
            }
        }
    });
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        const Image& a = before.layers[static_cast<std::size_t>(jobs[j].layer)].image;
        const int y0 = jobs[j].ty * kTile, h = std::min(kTile, a.height() - y0);
        for (int x0 : changed[j]) {
            const int w = std::min(kTile, a.width() - x0);
            save(jobs[j].layer, a, x0, y0, w, h);
            doc_->markChanged({x0, y0, x0 + w, y0 + h});
        }
    }
}

Result ImageEdit::apply() {
    clear();
    ImageDoc& doc = *doc_;
    beforeActive_ = doc.active;
    beforeWidth_ = doc.image.width;
    beforeHeight_ = doc.image.height;
    meta_.clear();
    for (const auto& l : doc.image.layers) meta_.push_back({l.name, l.opacity, l.blend, l.visible, l.maskEnabled, l.clip});
    const Mask beforeSelection = doc.selection;
    doc.ignoreSelection = false;

    // brushes inside a selection need the whole old picture to put back what's outside it
    if (mode_ == Mode::Declared && !doc.hasSelection()) {
        const auto shape = layerSizes(doc.image);
        doc.recording_ = this;
        Result r;
        try {
            r = fn_(doc);
        } catch (...) {
            doc.recording_ = nullptr;
            revert();
            throw;
        }
        doc.recording_ = nullptr;
        if (!r.ok) {
            revert();  // failed edits leave the document untouched
            return r;
        }
        if (layerSizes(doc.image) != shape) throw std::logic_error("a declared image edit changed the image size or layers");
        return r;
    }

    LayeredImage before = doc.image;  // short-lived: only the tiles that change are kept
    auto restore = [&] {
        doc.image = std::move(before);
        doc.active = beforeActive_;
        doc.selection = beforeSelection;
        doc.ignoreSelection = false;
    };
    Result r;
    try {
        r = fn_(doc);
    } catch (...) {
        restore();
        throw;
    }
    if (!r.ok) {  // failed edits leave the document untouched
        restore();
        return r;
    }
    const bool selectionChanged = doc.selection != beforeSelection;
    if (selectionChanged) {
        account(bytes_ + beforeSelection.v.size());
        selection_ = beforeSelection;
        doc.markAllChanged();
    }
    if (!sameShape(before, doc.image)) {
        whole_ = std::make_unique<LayeredImage>(std::move(before));
        std::size_t bytes = sizeof(LayeredImage);
        for (const auto& l : whole_->layers) bytes += l.image.bytes() + l.mask.v.size() + sizeof(Layer);
        account(bytes_ + bytes);
        doc.markAllChanged();
        doc.ignoreSelection = false;
        return r;
    }
    // pixel edits stay inside the selection
    if (doc.hasSelection() && !selectionChanged && !doc.ignoreSelection) {
        for (std::size_t i = 0; i < doc.image.layers.size(); ++i) {
            keepInside(doc.image.layers[i].image, before.layers[i].image, doc.selection);
        }
    }
    doc.ignoreSelection = false;
    diff(before);
    for (std::size_t i = 0; i < doc.image.layers.size(); ++i) {
        if (before.layers[i].mask != doc.image.layers[i].mask) {
            account(bytes_ + before.layers[i].mask.v.size());
            masks_.emplace_back(static_cast<int>(i), std::move(before.layers[i].mask));
            doc.markAllChanged();
        }
    }
    for (std::size_t i = 0; i < meta_.size(); ++i) {
        const Layer& l = doc.image.layers[i];
        const LayerMeta& m = meta_[i];
        if (l.opacity != m.opacity || l.blend != m.blend || l.visible != m.visible || l.maskEnabled != m.maskEnabled || l.clip != m.clip) {
            doc.markAllChanged();
        }
    }
    return r;
}

void ImageEdit::revert() {
    ImageDoc& doc = *doc_;
    if (whole_) {
        doc.image = std::move(*whole_);
        doc.markAllChanged();
    } else {
        // newest first, so where saved areas overlap the oldest copy (the original) wins
        for (auto it = patches_.rbegin(); it != patches_.rend(); ++it) {
            if (it->layer >= static_cast<int>(doc.image.layers.size())) continue;
            Image& img = doc.image.layers[static_cast<std::size_t>(it->layer)].image;
            for (int row = 0; row < it->h; ++row) {
                std::copy_n(it->px.data() + static_cast<std::size_t>(row) * it->w * 4, static_cast<std::size_t>(it->w) * 4,
                            img.pixel(it->x, it->y + row));
            }
            doc.markChanged({it->x, it->y, it->x + it->w, it->y + it->h});
        }
        for (std::size_t i = 0; i < meta_.size() && i < doc.image.layers.size(); ++i) {
            Layer& l = doc.image.layers[i];
            const LayerMeta& m = meta_[i];
            if (l.opacity != m.opacity || l.blend != m.blend || l.visible != m.visible || l.maskEnabled != m.maskEnabled ||
                l.clip != m.clip) {
                doc.markAllChanged();
            }
            l.name = m.name;
            l.opacity = m.opacity;
            l.blend = m.blend;
            l.visible = m.visible;
            l.maskEnabled = m.maskEnabled;
            l.clip = m.clip;
        }
        for (auto& [i, mask] : masks_) {
            if (i < static_cast<int>(doc.image.layers.size())) doc.image.layers[static_cast<std::size_t>(i)].mask = std::move(mask);
            doc.markAllChanged();
        }
        doc.image.width = beforeWidth_;
        doc.image.height = beforeHeight_;
    }
    if (selection_) {
        doc.selection = std::move(*selection_);
        doc.markAllChanged();
    }
    doc.active = beforeActive_;
    clear();
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

ActionFactory imageCommand(Session& session, std::function<Result(ImageDoc&, const Params&)> edit, bool undoable,
                           ImageEdit::Mode mode) {
    return [&session, edit, undoable, mode](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<ImageDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open image called '" + id + "' (use photo.open or paint.new first)"); });
        }
        if (!undoable) return std::make_unique<FunctionAction>([doc, edit, p] { return edit(*doc, p); });
        return std::make_unique<ImageEdit>(doc, [edit, p](ImageDoc& d) { return edit(d, p); }, mode);
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
