// Helpers the paint command files share.
#pragma once

#include <algorithm>
#include <string>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::paint {

inline std::string idOf(const Params& p) { return stringParam(p, "doc"); }

// The usual result after an edit: doc, active layer, layer count, optional preview.
inline Result done(const std::string& message, raster::ImageDoc& doc, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", idOf(p)).with("layer", static_cast<std::int64_t>(doc.active)).with("layers", static_cast<std::int64_t>(doc.image.layers.size()));
    return maybePreview(r, doc, p);
}

// A 0..100 param as 0..1.
inline double percent(const Params& p, const std::string& key, double fallback) {
    return std::clamp(numberParam(p, key, fallback), 0.0, 100.0) / 100.0;
}

// The brush settings every brush command takes (points, preset, size, hardness, flow, spacing,
// scatter, tip, texture, symmetry, layer, preview), and reading them back.
std::vector<ParamSpec> brushParams(bool withColor, const std::string& preset);
bool readBrush(const raster::ImageDoc& doc, const Params& p, raster::Brush& brush, std::vector<raster::StrokePoint>& points, std::string& error);

void registerLayerCommands(Session& session);  // layers.cpp
void registerGroupCommands(Session& session);  // layers.cpp
void registerStyleCommands(Session& session);  // layers.cpp
void registerTextCommands(Session& session);   // text.cpp
void registerRetouchBrushes(Session& session); // brushes.cpp: stroke, erase, clone, dodge, smudge...
void registerFillCommands(Session& session);   // brushes.cpp: pattern fill, gradient

}  // namespace wetwaresuite::paint
