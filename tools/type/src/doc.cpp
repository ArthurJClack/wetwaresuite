// The font document and its snapshot undo.
#include "achclwss/raster/codec.hpp"
#include "internal.hpp"

namespace achclwss::typeface {

std::string FontDoc::summary() const {
    std::size_t encoded = 0;
    for (const auto& g : font.glyphs) encoded += g.unicodes.empty() ? 0 : 1;
    std::string s = font.familyStyle() + ", " + std::to_string(font.glyphs.size()) + " glyphs (" + std::to_string(encoded) +
                    " encoded), " + std::to_string(font.kerning.size()) + " kerning pairs, " + std::to_string(font.metrics.unitsPerEm) +
                    " units/em";
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> FontDoc::preview() const {
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(overview(font)), "font.png"});
}

Result FontEdit::apply() {
    before_ = doc_->font;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void FontEdit::revert() { doc_->font = before_; }

}  // namespace achclwss::typeface
