// The document's structure: groups and layers (parent links), symbols and
// instances, cached text-on-path geometry, and transforming whole subtrees.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "internal.hpp"

namespace wetwaresuite::vector {

namespace {

constexpr double kPi = 3.14159265358979323846;

int findIn(const std::vector<Shape>& list, const std::string& id) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

void outlineRec(const std::vector<Shape>& list, const std::vector<Symbol>& symbols, int idx, double detail, const Matrix& m, Outline& out,
                int depth) {
    if (depth > 16 || idx < 0) return;
    const Shape& s = list[static_cast<std::size_t>(idx)];
    if (s.kind == ShapeKind::Group) {
        if (!s.clip.empty()) {
            if (int c = findIn(list, s.clip); c >= 0) {
                outlineRec(list, symbols, c, detail, m, out, depth + 1);
                return;
            }
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].parent == s.id && list[i].id != s.mask) outlineRec(list, symbols, static_cast<int>(i), detail, m, out, depth + 1);
        }
        return;
    }
    if (s.kind == ShapeKind::Instance) {
        const Symbol* sym = nullptr;
        for (const auto& y : symbols) {
            if (y.name == s.symbol) sym = &y;
        }
        if (!sym) return;
        const Matrix inner = m * shapeMatrix(s) * Matrix::translate(s.cx, s.cy);
        for (std::size_t i = 0; i < sym->shapes.size(); ++i) {
            if (sym->shapes[i].parent.empty()) outlineRec(sym->shapes, symbols, static_cast<int>(i), detail, inner, out, depth + 1);
        }
        return;
    }
    Outline o = outline(s, detail * std::max(1e-6, m.scaleFactor()));
    if (!m.identity()) {
        for (auto& c : o) {
            for (auto& p : c.points) p = m.apply(p);
        }
    }
    out.insert(out.end(), o.begin(), o.end());
}

Bounds symbolBounds(const Symbol& sym, const std::vector<Symbol>& symbols) {
    Outline o;
    for (std::size_t i = 0; i < sym.shapes.size(); ++i) {
        if (sym.shapes[i].parent.empty()) outlineRec(sym.shapes, symbols, static_cast<int>(i), 1.0, Matrix{}, o, 1);
    }
    return bounds(o);
}

bool near(const Matrix& a, const Matrix& b) {
    const double k = 1e-6 * std::max({1.0, std::abs(a.e), std::abs(a.f)});
    return std::abs(a.a - b.a) < 1e-7 && std::abs(a.b - b.b) < 1e-7 && std::abs(a.c - b.c) < 1e-7 && std::abs(a.d - b.d) < 1e-7 &&
           std::abs(a.e - b.e) < k && std::abs(a.f - b.f) < k;
}

}  // namespace

namespace detail {

Transform decomposeMatrix(const Matrix& m, Point centre) {
    Transform t;
    t.sx = std::hypot(m.a, m.b);
    t.rotate = t.sx > 0 ? std::atan2(m.b, m.a) * 180 / kPi : 0;
    const double r = t.rotate * kPi / 180;
    t.sy = -m.c * std::sin(r) + m.d * std::cos(r);
    const Matrix lin = Matrix::rotate(t.rotate) * Matrix::scale(t.sx, t.sy);
    const Point ac = lin.apply(centre);
    t.tx = m.e - centre.x + ac.x;
    t.ty = m.f - centre.y + ac.y;
    auto snap = [](double& v, double target) {
        if (std::abs(v - target) < 1e-9) v = target;
    };
    snap(t.sx, 1);
    snap(t.sy, 1);
    snap(t.rotate, 0);
    snap(t.tx, 0);
    snap(t.ty, 0);
    return t;
}

std::string duplicateSubtree(VectorDoc& doc, int index, const std::string& newParent) {
    const std::vector<int> tree = doc.subtree(index);
    std::map<std::string, std::string> ids;
    std::vector<Shape> copies;
    for (int i : tree) {
        Shape c = doc.shapes[static_cast<std::size_t>(i)];
        const std::string old = c.id;
        c.id = "s" + std::to_string(doc.nextId++);
        ids[old] = c.id;
        copies.push_back(std::move(c));
    }
    for (std::size_t k = 0; k < copies.size(); ++k) {
        Shape& c = copies[k];
        if (k == 0) c.parent = newParent;
        else if (auto it = ids.find(c.parent); it != ids.end()) c.parent = it->second;
        if (auto it = ids.find(c.clip); it != ids.end()) c.clip = it->second;
        if (auto it = ids.find(c.mask); it != ids.end()) c.mask = it->second;
        if (auto it = ids.find(c.onPath); it != ids.end()) c.onPath = it->second;
    }
    // the copy goes just above the original's last shape, so it stacks right above it
    const int insertAt = *std::max_element(tree.begin(), tree.end()) + 1;
    const std::string root = copies.front().id;
    doc.shapes.insert(doc.shapes.begin() + insertAt, copies.begin(), copies.end());
    return root;
}

void eraseSubtree(VectorDoc& doc, int index) {
    std::vector<int> tree = doc.subtree(index);
    std::sort(tree.rbegin(), tree.rend());
    std::set<std::string> gone;
    for (int i : tree) gone.insert(doc.shapes[static_cast<std::size_t>(i)].id);
    for (int i : tree) doc.shapes.erase(doc.shapes.begin() + i);
    for (auto& s : doc.shapes) {
        if (gone.count(s.clip)) s.clip.clear();
        if (gone.count(s.mask)) s.mask.clear();
    }
    if (gone.count(doc.currentLayer)) doc.currentLayer.clear();
}

}  // namespace detail

std::string VectorDoc::summary() const {
    std::string s = std::to_string(width) + "x" + std::to_string(height) + ", " + std::to_string(shapes.size()) +
                    (shapes.size() == 1 ? " shape" : " shapes");
    if (artboards.size() > 1) s += ", " + std::to_string(artboards.size()) + " artboards";
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
    if (shape.parent.empty() && !currentLayer.empty() && findIn(shapes, currentLayer) >= 0 && !shape.layer) shape.parent = currentLayer;
    shapes.push_back(std::move(shape));
    return shapes.back().id;
}

std::vector<int> VectorDoc::children(const std::string& groupId) const {
    std::vector<int> out;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].parent == groupId) out.push_back(static_cast<int>(i));
    }
    return out;
}

std::vector<int> VectorDoc::subtree(int index) const {
    std::vector<int> out{index};
    std::set<std::string> ids{shapes[static_cast<std::size_t>(index)].id};
    // children always come from ids already in the tree; repeat until nothing new joins
    bool grew = true;
    while (grew) {
        grew = false;
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            if (!shapes[i].parent.empty() && ids.count(shapes[i].parent) && !ids.count(shapes[i].id)) {
                ids.insert(shapes[i].id);
                out.push_back(static_cast<int>(i));
                grew = true;
            }
        }
    }
    std::sort(out.begin() + 1, out.end());
    return out;
}

bool VectorDoc::hiddenOrInside(int index) const {
    for (int guard = 0; index >= 0 && guard < 64; ++guard) {
        const Shape& s = shapes[static_cast<std::size_t>(index)];
        if (!s.visible) return true;
        index = s.parent.empty() ? -1 : findIn(shapes, s.parent);
    }
    return false;
}

bool VectorDoc::lockedOrInside(int index) const {
    for (int guard = 0; index >= 0 && guard < 64; ++guard) {
        const Shape& s = shapes[static_cast<std::size_t>(index)];
        if (s.locked) return true;
        index = s.parent.empty() ? -1 : findIn(shapes, s.parent);
    }
    return false;
}

const Symbol* VectorDoc::findSymbol(const std::string& name) const {
    for (const auto& s : symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::optional<Color> VectorDoc::swatch(const std::string& name) const {
    for (const auto& [n, c] : swatches) {
        if (n == name) return c;
    }
    return std::nullopt;
}

Artboard VectorDoc::artboard(int index) const {
    if (artboards.empty() || index < 0 || index >= static_cast<int>(artboards.size())) {
        return {"canvas", 0, 0, static_cast<double>(width), static_cast<double>(height)};
    }
    return artboards[static_cast<std::size_t>(index)];
}

void VectorDoc::refresh() {
    for (auto& s : shapes) {
        if (s.kind == ShapeKind::Text && !s.onPath.empty()) {
            const int p = findIn(shapes, s.onPath);
            if (p < 0 || shapes[static_cast<std::size_t>(p)].kind == ShapeKind::Text) continue;
            const Shape& path = shapes[static_cast<std::size_t>(p)];
            BezierPath bp = shapePath(path);
            transformPath(bp, shapeMatrix(path));
            s.d = pathData(bp);
        }
    }
    for (auto& s : shapes) {
        if (s.kind != ShapeKind::Instance) continue;
        const Symbol* sym = findSymbol(s.symbol);
        if (!sym) continue;
        const Bounds b = symbolBounds(*sym, symbols);
        if (b.empty) continue;
        s.x = b.x0 + s.cx;
        s.y = b.y0 + s.cy;
        s.w = std::max(b.width(), 1e-6);
        s.h = std::max(b.height(), 1e-6);
    }
}

Outline outlineIn(const VectorDoc& doc, int index, double detail) {
    Outline out;
    if (index < 0 || index >= static_cast<int>(doc.shapes.size())) return out;
    outlineRec(doc.shapes, doc.symbols, index, detail, Matrix{}, out, 0);
    return out;
}

Bounds shapeBounds(const VectorDoc& doc, int index) { return bounds(outlineIn(doc, index, 1.0)); }

void applyMatrix(VectorDoc& doc, int index, const Matrix& m) {
    Shape& s = doc.shapes[static_cast<std::size_t>(index)];
    if (s.kind == ShapeKind::Group) {
        for (int c : doc.children(s.id)) applyMatrix(doc, c, m);
        return;
    }
    if (s.kind == ShapeKind::Text && !s.onPath.empty()) return;  // it follows its path
    const Matrix full = m * shapeMatrix(s);
    const Bounds b = bounds(localOutline(s, 1.0));
    const Point centre = b.empty ? Point{0, 0} : Point{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
    const Transform t = detail::decomposeMatrix(full, centre);
    Shape trial = s;
    trial.transform = t;
    if (near(shapeMatrix(trial), full) || s.kind == ShapeKind::Text || s.kind == ShapeKind::Instance) {
        s.transform = t;  // text and instances keep the nearest transform rather than turning into outlines
        return;
    }
    // skewed or stretched after turning: the transform can't say it, so the geometry takes it
    BezierPath bp = shapePath(s);
    transformPath(bp, full);
    s.strokeWidth *= full.scaleFactor();  // the renderer scaled it by the old transform; now the geometry is final
    s.kind = ShapeKind::Path;
    s.d = pathData(bp);
    s.points.clear();
    s.transform = {};
}

}  // namespace wetwaresuite::vector
