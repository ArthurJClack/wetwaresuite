// Rasterising a vector document through the raster library's antialiased
// polygon filler and round brush.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::vector {

namespace {

// fillPolygon takes one even-odd contour, so subpaths are joined with bridge
// edges that go out and come straight back; each pair crosses every scanline
// twice at the same x and cancels, leaving holes and separate pieces intact.
void fillOutline(raster::Image& img, const Outline& outline, Color color) {
    std::vector<Point> pts;
    const Point* anchor = nullptr;
    for (const auto& c : outline) {
        if (c.points.size() < 3) continue;
        if (!anchor) {
            pts = c.points;
            pts.push_back(c.points.front());
            anchor = &c.points.front();
            continue;
        }
        pts.insert(pts.end(), c.points.begin(), c.points.end());
        pts.push_back(c.points.front());
        pts.push_back(*anchor);
    }
    if (pts.size() >= 3) raster::fillPolygon(img, pts, color);
}

void strokeOutline(raster::Image& img, const Outline& outline, Color color, double width) {
    raster::Brush brush;
    brush.color = color;
    brush.hardness = 1.0;
    brush.spacing = 0.05;
    brush.pressureSize = false;
    brush.size = std::max(1.0, width);
    if (width < 1) brush.color.a = static_cast<std::uint8_t>(color.a * std::max(0.0, width) + 0.5);
    for (const auto& c : outline) {
        if (c.points.empty()) continue;
        std::vector<raster::StrokePoint> sp;
        sp.reserve(c.points.size() + 1);
        for (const auto& p : c.points) sp.push_back({p.x, p.y, 1.0});
        if (c.closed) sp.push_back({c.points.front().x, c.points.front().y, 1.0});
        raster::stroke(img, brush, sp);
    }
}

void drawShape(raster::Image& img, const Shape& s, double scale) {
    Outline o = outline(s, scale);
    if (scale != 1.0) {
        for (auto& c : o) {
            for (auto& p : c.points) p = {p.x * scale, p.y * scale};
        }
    }
    if (s.fill && s.fill->a > 0 && s.kind != ShapeKind::Line) fillOutline(img, o, *s.fill);
    if (s.stroke && s.stroke->a > 0 && s.strokeWidth > 0) {
        const double k = std::sqrt(std::abs(s.transform.sx * s.transform.sy));
        strokeOutline(img, o, *s.stroke, s.strokeWidth * k * scale);
    }
}

}  // namespace

raster::Image render(const VectorDoc& doc, double scale) {
    if (!(scale > 0)) scale = 1.0;
    const int w = std::max(1, static_cast<int>(std::lround(doc.width * scale)));
    const int h = std::max(1, static_cast<int>(std::lround(doc.height * scale)));
    raster::Image img(w, h, doc.background);
    for (const auto& s : doc.shapes) {
        if (!s.visible || s.opacity <= 0) continue;
        if (s.opacity >= 1) {
            drawShape(img, s, scale);
            continue;
        }
        raster::Image layer(w, h);  // group opacity: fill and stroke fade together
        drawShape(layer, s, scale);
        raster::blendOnto(img, layer, 0, 0, raster::Blend::Normal, s.opacity);
    }
    return img;
}

std::string VectorDoc::summary() const {
    std::string s = std::to_string(width) + "x" + std::to_string(height) + ", " + std::to_string(shapes.size()) +
                    (shapes.size() == 1 ? " shape" : " shapes");
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> VectorDoc::preview() const {
    const double scale = std::min(4.0, 512.0 / std::max({width, height, 1}));
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(render(*this, scale)), "preview.png"});
}

int VectorDoc::find(const std::string& idOrName) const {
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].id == idOrName) return static_cast<int>(i);
    }
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].name == idOrName) return static_cast<int>(i);
    }
    return -1;
}

std::string VectorDoc::add(Shape shape) {
    const int n = nextId++;
    shape.id = "s" + std::to_string(n);
    if (shape.name.empty()) shape.name = std::string(toString(shape.kind)) + " " + std::to_string(n);
    shapes.push_back(std::move(shape));
    return shapes.back().id;
}

}  // namespace wetwaresuite::vector
