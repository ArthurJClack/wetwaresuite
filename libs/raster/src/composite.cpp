// Flattening a layer stack: blend modes, masks, clipping, groups (pass-through or isolated),
// adjustment layers and layer styles. Plain pixel layers take the fast path straight into
// blendOnto, so a stack of ordinary layers costs what it always did.
#include <algorithm>

#include "composite_internal.hpp"
#include "wetwaresuite/raster/adjustments.hpp"
#include "wetwaresuite/raster/parallel.hpp"

namespace wetwaresuite::raster {

namespace detail {

std::vector<std::uint8_t> layerAlpha(const Layer& l, int x, int y, int w, int h) {
    const bool pixels = l.hasPixels();
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * h, pixels ? 0 : 255);
    const bool masked = usesMask(l);
    for (int row = 0; row < h; ++row) {
        const int sy = y + row;
        std::uint8_t* out = a.data() + static_cast<std::size_t>(row) * w;
        if (pixels && (sy < 0 || sy >= l.image.height())) continue;
        for (int col = 0; col < w; ++col) {
            const int sx = x + col;
            int v = 255;
            if (pixels) {
                if (sx < 0 || sx >= l.image.width()) continue;
                v = l.image.pixel(sx, sy)[3];
            }
            if (masked) v = sx >= 0 && sy >= 0 && sx < l.mask.width && sy < l.mask.height ? v * l.mask.at(sx, sy) / 255 : 0;
            out[col] = static_cast<std::uint8_t>(v);
        }
    }
    return a;
}

}  // namespace detail

namespace {

using detail::layerAlpha;
using detail::usesMask;

struct Area {
    int x, y, w, h;
};

void compositeRange(Image& out, const std::vector<Layer>& layers, int lo, int hi, int depth, const Area& r);

// Multiplies the alpha of `img` (the area's size) by `alpha` (may be null).
void cutAlpha(Image& img, const std::uint8_t* alpha) {
    if (!alpha) return;
    std::uint8_t* p = img.data();
    const std::size_t n = static_cast<std::size_t>(img.width()) * img.height();
    for (std::size_t i = 0; i < n; ++i) p[i * 4 + 3] = static_cast<std::uint8_t>(p[i * 4 + 3] * alpha[i] / 255);
}

// A pixel layer drawn with its mask, clipping and fill opacity applied.
void drawPixels(Image& out, const Layer& l, const Area& r, const std::uint8_t* clipAlpha) {
    if (!usesMask(l) && !clipAlpha && l.fill >= 1) {
        blendOnto(out, l.image, -r.x, -r.y, l.blend, l.opacity);
        return;
    }
    Image part(r.w, r.h);
    std::vector<std::uint8_t> alpha = layerAlpha(l, r.x, r.y, r.w, r.h);
    const int fill = static_cast<int>(std::clamp(l.fill, 0.0, 1.0) * 255 + 0.5);
    for (int row = 0; row < r.h; ++row) {
        const int sy = r.y + row;
        if (sy < 0 || sy >= l.image.height()) continue;
        for (int col = 0; col < r.w; ++col) {
            const int sx = r.x + col;
            if (sx < 0 || sx >= l.image.width()) continue;
            const std::size_t k = static_cast<std::size_t>(row) * r.w + col;
            int a = alpha[k] * fill / 255;
            if (clipAlpha) a = a * clipAlpha[k] / 255;
            std::uint8_t* d = part.pixel(col, row);
            const std::uint8_t* s = l.image.pixel(sx, sy);
            d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = static_cast<std::uint8_t>(a);
        }
    }
    blendOnto(out, part, 0, 0, l.blend, l.opacity);
}

// An adjustment layer: everything below, adjusted, faded in by opacity, mask and clipping.
void drawAdjustment(Image& out, const Layer& l, const Area& r, const std::uint8_t* clipAlpha) {
    auto type = l.settings.find("type");
    if (type == l.settings.end()) return;
    Image adjusted = out;
    applyAdjustment(adjusted, type->second, l.settings);
    const std::vector<std::uint8_t> mask = usesMask(l) ? layerAlpha(l, r.x, r.y, r.w, r.h) : std::vector<std::uint8_t>{};
    const float op = static_cast<float>(std::clamp(l.opacity * l.fill, 0.0, 1.0));
    const Blend mode = l.blend == Blend::Dissolve ? Blend::Normal : l.blend;
    parallelRows(r.h, static_cast<std::size_t>(r.w) * 2, [&](int b, int e) {
        for (int row = b; row < e; ++row) {
            for (int col = 0; col < r.w; ++col) {
                const std::size_t k = static_cast<std::size_t>(row) * r.w + col;
                std::uint8_t* d = out.pixel(col, row);
                if (d[3] == 0) continue;
                float t = op;
                if (!mask.empty()) t *= mask[k] / 255.0f;
                if (clipAlpha) t *= clipAlpha[k] / 255.0f;
                if (t <= 0) continue;
                const std::uint8_t* s = adjusted.pixel(col, row);
                const float bc[3] = {d[0] / 255.0f, d[1] / 255.0f, d[2] / 255.0f};
                const float sc[3] = {s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f};
                float m[3] = {sc[0], sc[1], sc[2]};
                if (mode != Blend::Normal) blendColor(mode, bc, sc, m);
                for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(std::clamp(bc[c] + (m[c] - bc[c]) * t, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    });
}

// A group: members [lo, header). Pass-through groups draw their members straight onto what's
// below; others flatten them on their own first and blend the result like one layer.
void drawGroup(Image& out, const std::vector<Layer>& layers, int lo, int header, const Area& r, const std::uint8_t* clipAlpha) {
    const Layer& g = layers[static_cast<std::size_t>(header)];
    const bool masked = usesMask(g);
    if (g.passThrough && !masked && !clipAlpha && g.opacity * g.fill >= 1) {
        compositeRange(out, layers, lo, header, g.depth + 1, r);
        return;
    }
    if (g.passThrough) {
        Image mixed = out;
        compositeRange(mixed, layers, lo, header, g.depth + 1, r);
        const std::vector<std::uint8_t> mask = masked ? layerAlpha(g, r.x, r.y, r.w, r.h) : std::vector<std::uint8_t>{};
        const float op = static_cast<float>(std::clamp(g.opacity * g.fill, 0.0, 1.0));
        std::uint8_t* d = out.data();
        const std::uint8_t* m = mixed.data();
        const std::size_t n = static_cast<std::size_t>(r.w) * r.h;
        for (std::size_t k = 0; k < n; ++k) {
            float t = op;
            if (!mask.empty()) t *= mask[k] / 255.0f;
            if (clipAlpha) t *= clipAlpha[k] / 255.0f;
            for (int c = 0; c < 4; ++c) d[k * 4 + c] = static_cast<std::uint8_t>(d[k * 4 + c] + (m[k * 4 + c] - d[k * 4 + c]) * t + 0.5f);
        }
        return;
    }
    Image buf(r.w, r.h);
    compositeRange(buf, layers, lo, header, g.depth + 1, r);
    if (masked) cutAlpha(buf, layerAlpha(g, r.x, r.y, r.w, r.h).data());
    cutAlpha(buf, clipAlpha);
    blendOnto(out, buf, 0, 0, g.blend == Blend::Dissolve ? Blend::Normal : g.blend, std::clamp(g.opacity * g.fill, 0.0, 1.0));
}

void drawLayer(Image& out, const Layer& l, const Area& r, const std::uint8_t* clipAlpha) {
    switch (l.kind) {
        case LayerKind::Group: return;  // a group with nothing in it
        case LayerKind::Adjustment: drawAdjustment(out, l, r, clipAlpha); return;
        default:
            if (l.style.any()) detail::drawStyledLayer(out, l, r.x, r.y, clipAlpha);
            else drawPixels(out, l, r, clipAlpha);
    }
}

// The alpha a clipped layer is cut to: its base's own alpha (a group's: its flattened members').
std::vector<std::uint8_t> baseAlphaOf(const std::vector<Layer>& layers, int lo, int index, const Area& r) {
    const Layer& b = layers[static_cast<std::size_t>(index)];
    if (b.kind != LayerKind::Group) return layerAlpha(b, r.x, r.y, r.w, r.h);
    Image buf(r.w, r.h);
    compositeRange(buf, layers, lo, index, b.depth + 1, r);
    std::vector<std::uint8_t> a(static_cast<std::size_t>(r.w) * r.h);
    for (std::size_t k = 0; k < a.size(); ++k) a[k] = buf.data()[k * 4 + 3];
    if (usesMask(b)) {
        const auto m = layerAlpha(b, r.x, r.y, r.w, r.h);
        for (std::size_t k = 0; k < a.size(); ++k) a[k] = static_cast<std::uint8_t>(a[k] * m[k] / 255);
    }
    return a;
}

void compositeRange(Image& out, const std::vector<Layer>& layers, int lo, int hi, int depth, const Area& r) {
    int base = -1, baseLo = -1;
    std::vector<std::uint8_t> baseAlpha;
    bool baseReady = false;
    for (int i = lo; i < hi;) {
        int index = i, membersLo = i, next = i + 1;
        if (layers[static_cast<std::size_t>(i)].depth > depth) {
            // members of a group: its header is the first layer above them back at this depth
            int j = i;
            while (j < hi && layers[static_cast<std::size_t>(j)].depth > depth) ++j;
            if (j >= hi || layers[static_cast<std::size_t>(j)].kind != LayerKind::Group) {
                compositeRange(out, layers, i, j, depth + 1, r);  // stray members (normalizeDepths prevents these)
                i = j;
                continue;
            }
            index = j;
            next = j + 1;
        }
        const Layer& l = layers[static_cast<std::size_t>(index)];
        if (!l.clip || base < 0) {
            base = index;
            baseLo = membersLo;
            baseReady = false;
        }
        const bool clipped = l.clip && base != index;
        if (!l.visible || (clipped && !layers[static_cast<std::size_t>(base)].visible)) {
            i = next;
            continue;
        }
        const std::uint8_t* clipAlpha = nullptr;
        if (clipped) {
            if (!baseReady) {
                baseAlpha = baseAlphaOf(layers, baseLo, base, r);
                baseReady = true;
            }
            clipAlpha = baseAlpha.data();
        }
        if (l.kind == LayerKind::Group) drawGroup(out, layers, membersLo, index, r, clipAlpha);
        else drawLayer(out, l, r, clipAlpha);
        i = next;
    }
}

}  // namespace

Image composite(const std::vector<Layer>& layers, int width, int height) { return compositeRegion(layers, 0, 0, width, height); }

Image compositeRegion(const std::vector<Layer>& layers, int x, int y, int width, int height) {
    Image out(width, height);
    if (width <= 0 || height <= 0) return out;
    compositeRange(out, layers, 0, static_cast<int>(layers.size()), 0, {x, y, width, height});
    return out;
}

}  // namespace wetwaresuite::raster
