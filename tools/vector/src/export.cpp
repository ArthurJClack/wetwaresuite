// Vector PDF and EPS out, and saveDoc. Both walk the shape tree like the
// renderer: paths keep their curves, strokes keep caps/joins/dashes, gradients
// become smooth shadings, groups with opacity or blend modes become PDF
// transparency groups, clip groups clip and opacity masks become soft masks.
// What has no vector form (effects, patterns, freeform gradients) is drawn
// into a picture at twice the size and placed there. Text goes out as outlines.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>

#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::vector {

namespace {

constexpr double kPt = 0.75;  // CSS px to PDF points

std::string n4(double v) {
    if (std::abs(v) < 5e-5) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? std::string("0") : s;
}

std::string rgb3(Color c) { return n4(c.r / 255.0) + " " + n4(c.g / 255.0) + " " + n4(c.b / 255.0); }

// Path construction in either language.
std::string pathOps(const BezierPath& bp, bool ps) {
    std::string out;
    for (const auto& sp : bp) {
        if (sp.nodes.empty()) continue;
        out += n4(sp.nodes[0].p.x) + " " + n4(sp.nodes[0].p.y) + (ps ? " moveto\n" : " m\n");
        const std::size_t n = sp.nodes.size(), segs = sp.closed ? n : n - 1;
        for (std::size_t i = 0; i < segs; ++i) {
            const Node& a = sp.nodes[i];
            const Node& b = sp.nodes[(i + 1) % n];
            const bool line = a.out.x == a.p.x && a.out.y == a.p.y && b.in.x == b.p.x && b.in.y == b.p.y;
            if (line) out += n4(b.p.x) + " " + n4(b.p.y) + (ps ? " lineto\n" : " l\n");
            else {
                out += n4(a.out.x) + " " + n4(a.out.y) + " " + n4(b.in.x) + " " + n4(b.in.y) + " " + n4(b.p.x) + " " + n4(b.p.y) + (ps ? " curveto\n" : " c\n");
            }
        }
        if (sp.closed) out += ps ? "closepath\n" : "h\n";
    }
    return out;
}

BezierPath outlinePath(const Outline& o) {
    BezierPath bp;
    for (const auto& c : o) {
        SubPath sp;
        sp.closed = true;
        for (const auto& p : c.points) sp.nodes.push_back({p, p, p});
        if (sp.nodes.size() >= 3) bp.push_back(std::move(sp));
    }
    return bp;
}

const char* pdfBlend(const std::string& b) {
    static const std::map<std::string, const char*> names = {
        {"normal", "Normal"},          {"multiply", "Multiply"},       {"screen", "Screen"},         {"overlay", "Overlay"},
        {"darken", "Darken"},          {"lighten", "Lighten"},         {"color_dodge", "ColorDodge"}, {"color_burn", "ColorBurn"},
        {"hard_light", "HardLight"},   {"soft_light", "SoftLight"},    {"difference", "Difference"}, {"exclusion", "Exclusion"},
        {"hue", "Hue"},                {"saturation", "Saturation"},   {"color", "Color"},           {"luminosity", "Luminosity"}};
    auto it = names.find(b);
    return it == names.end() ? "Normal" : it->second;
}

// A gradient as a PDF function of t (stitched linear pieces).
std::string gradientFunction(const Gradient& g) {
    std::vector<GradientStop> stops = g.stops;
    if (stops.empty()) stops.push_back({0, Color{0, 0, 0, 255}});
    if (stops.front().offset > 0) stops.insert(stops.begin(), {0, stops.front().color});
    if (stops.back().offset < 1) stops.push_back({1, stops.back().color});
    std::string fns, bounds, encode;
    for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
        fns += "<< /FunctionType 2 /Domain [0 1] /C0 [" + rgb3(stops[i].color) + "] /C1 [" + rgb3(stops[i + 1].color) + "] /N 1 >> ";
        if (i + 2 < stops.size()) bounds += n4(std::max(stops[i + 1].offset, 1e-6 * static_cast<double>(i + 1))) + " ";
        encode += "0 1 ";
    }
    if (stops.size() == 2) return "<< /FunctionType 2 /Domain [0 1] /C0 [" + rgb3(stops[0].color) + "] /C1 [" + rgb3(stops[1].color) + "] /N 1 >>";
    return "<< /FunctionType 3 /Domain [0 1] /Functions [" + fns + "] /Bounds [" + bounds + "] /Encode [" + encode + "] >>";
}

std::string shadingDict(const Gradient& g) {
    if (g.radial) {
        return "<< /ShadingType 3 /ColorSpace /DeviceRGB /Coords [" + n4(g.cx) + " " + n4(g.cy) + " 0 " + n4(g.cx) + " " + n4(g.cy) + " " + n4(g.r) +
               "] /Function " + gradientFunction(g) + " /Extend [true true] >>";
    }
    return "<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [" + n4(g.x1) + " " + n4(g.y1) + " " + n4(g.x2) + " " + n4(g.y2) + "] /Function " +
           gradientFunction(g) + " /Extend [true true] >>";
}

// What both writers share: walking the tree with matrices, deciding what can go out as vectors.
struct Walk {
    const VectorDoc& doc;
    Matrix page;  // canvas -> page space
    std::map<const std::vector<Shape>*, std::map<std::string, std::vector<std::size_t>>> kidsCache;

    const std::vector<std::size_t>& kids(const std::vector<Shape>& list, const std::string& parent) {
        auto& m = kidsCache[&list];
        if (m.empty()) {
            for (std::size_t i = 0; i < list.size(); ++i) m[list[i].parent].push_back(i);
            m["\x01"];  // never empty again
        }
        static const std::vector<std::size_t> none;
        auto it = m.find(parent);
        return it == m.end() ? none : it->second;
    }
    static bool rasterOnly(const Shape& s) {
        return !s.effects.empty() || s.pattern || (s.gradient && s.gradient->freeform);
    }
};

class PdfOut {
public:
    explicit PdfOut(const VectorDoc& doc) : doc_(doc) {}

    std::string page(int artboard) {
        const Artboard ab = doc_.artboard(artboard);
        Walk w{doc_, Matrix::scale(kPt, -kPt) * Matrix::translate(-ab.x, -(ab.y + ab.h)), {}};
        walk_ = &w;
        area_ = ab;
        std::string content;
        if (doc_.background.a > 0) {
            content += rgb3(doc_.background) + " rg\n0 0 " + n4(ab.w * kPt) + " " + n4(ab.h * kPt) + " re f\n";
        }
        content += list(doc_.shapes, "", w.page, 0);
        const int stream = add("<< /Length " + std::to_string(pdf::deflate(content).size()) + " /Filter /FlateDecode >>\nstream\n" + pdf::deflate(content) + "\nendstream");
        const std::string pageDict = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + n4(ab.w * kPt) + " " + n4(ab.h * kPt) + "] /Resources " + resources() +
                                     " /Contents " + std::to_string(stream) + " 0 R /Group << /S /Transparency /CS /DeviceRGB >> >>";
        return pageDict;
    }

    std::string finish(const std::vector<std::string>& pages) {
        std::vector<int> pageIds;
        for (const auto& p : pages) pageIds.push_back(add(p));
        std::string kids;
        for (int id : pageIds) kids += std::to_string(id) + " 0 R ";
        objects_[0] = "<< /Type /Catalog /Pages 2 0 R >>";
        objects_[1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageIds.size()) + " >>";
        std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
        std::vector<std::size_t> offsets;
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            offsets.push_back(out.size());
            out += std::to_string(i + 1) + " 0 obj\n" + objects_[i] + "\nendobj\n";
        }
        const std::size_t xref = out.size();
        out += "xref\n0 " + std::to_string(objects_.size() + 1) + "\n0000000000 65535 f \n";
        for (std::size_t off : offsets) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
            out += buf;
        }
        out += "trailer\n<< /Size " + std::to_string(objects_.size() + 1) + " /Root 1 0 R /Info << /Producer (wetwaresuite vector) >> >>\nstartxref\n" +
               std::to_string(xref) + "\n%%EOF\n";
        return out;
    }

private:
    const VectorDoc& doc_;
    Walk* walk_ = nullptr;
    Artboard area_;
    std::vector<std::string> objects_{"", ""};  // 1 = catalog, 2 = pages, filled in at the end
    std::map<std::string, std::string> gstates_, patterns_, xobjects_;  // resource name -> object reference

    int add(std::string obj) {
        objects_.push_back(std::move(obj));
        return static_cast<int>(objects_.size());
    }

    std::string resources() {
        auto dict = [](const std::map<std::string, std::string>& m) {
            std::string s = "<< ";
            for (const auto& [k, v] : m) s += "/" + k + " " + v + " ";
            return s + ">>";
        };
        std::string r = "<< /ProcSet [/PDF /ImageC] ";
        if (!gstates_.empty()) r += "/ExtGState " + dict(gstates_) + " ";
        if (!patterns_.empty()) r += "/Pattern " + dict(patterns_) + " ";
        if (!xobjects_.empty()) r += "/XObject " + dict(xobjects_) + " ";
        return r + ">>";
    }

    std::string gstate(double fillAlpha, double strokeAlpha, const std::string& blend, const std::string& smask = "") {
        const std::string key = n4(fillAlpha) + "|" + n4(strokeAlpha) + "|" + blend + "|" + smask;
        for (const auto& [name, ref] : gstates_) {
            if (gsKeys_[name] == key) return name;
        }
        const std::string name = "G" + std::to_string(gstates_.size() + 1);
        std::string d = "<< /Type /ExtGState /ca " + n4(fillAlpha) + " /CA " + n4(strokeAlpha) + " /BM /" + pdfBlend(blend);
        if (!smask.empty()) d += " /SMask << /S /Luminosity /G " + smask + " >>";
        gstates_[name] = std::to_string(add(d + " >>")) + " 0 R";
        gsKeys_[name] = key;
        return name;
    }
    std::map<std::string, std::string> gsKeys_;

    std::string shadingPattern(const Gradient& g, const Matrix& toPage) {
        const std::string name = "P" + std::to_string(patterns_.size() + 1);
        patterns_[name] = std::to_string(add("<< /PatternType 2 /Shading " + shadingDict(g) + " /Matrix [" + n4(toPage.a) + " " + n4(toPage.b) + " " +
                                             n4(toPage.c) + " " + n4(toPage.d) + " " + n4(toPage.e) + " " + n4(toPage.f) + "] >>")) +
                          " 0 R";
        return name;
    }

    // A form XObject (transparency group) holding `content`, for opacity/blend groups and masks.
    std::string form(const std::string& content, bool luminosity = false) {
        const std::string z = pdf::deflate(content);
        const std::string group = luminosity ? "/Group << /S /Transparency /CS /DeviceRGB >>" : "/Group << /S /Transparency /I true /CS /DeviceRGB >>";
        const int id = add("<< /Type /XObject /Subtype /Form /BBox [-100000 -100000 100000 100000] " + group + " /Resources " + resources() +
                           " /Length " + std::to_string(z.size()) + " /Filter /FlateDecode >>\nstream\n" + z + "\nendstream");
        return std::to_string(id) + " 0 R";
    }

    std::string image(const raster::Image& img) {
        std::string rgb, alpha;
        rgb.reserve(static_cast<std::size_t>(img.width()) * img.height() * 3);
        alpha.reserve(static_cast<std::size_t>(img.width()) * img.height());
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const std::uint8_t* p = img.pixel(x, y);
                rgb += static_cast<char>(p[0]), rgb += static_cast<char>(p[1]), rgb += static_cast<char>(p[2]);
                alpha += static_cast<char>(p[3]);
            }
        }
        const std::string za = pdf::deflate(alpha), zc = pdf::deflate(rgb);
        const std::string dims = "/Width " + std::to_string(img.width()) + " /Height " + std::to_string(img.height()) + " /BitsPerComponent 8";
        const int mask = add("<< /Type /XObject /Subtype /Image " + dims + " /ColorSpace /DeviceGray /Length " + std::to_string(za.size()) +
                             " /Filter /FlateDecode >>\nstream\n" + za + "\nendstream");
        const int id = add("<< /Type /XObject /Subtype /Image " + dims + " /ColorSpace /DeviceRGB /SMask " + std::to_string(mask) + " 0 R /Length " +
                           std::to_string(zc.size()) + " /Filter /FlateDecode >>\nstream\n" + zc + "\nendstream");
        const std::string name = "I" + std::to_string(xobjects_.size() + 1);
        xobjects_[name] = std::to_string(id) + " 0 R";
        return name;
    }

    std::string placeForm(const std::string& ref) {
        const std::string name = "F" + std::to_string(xobjects_.size() + 1);
        xobjects_[name] = ref;
        return name;
    }

    std::string list(const std::vector<Shape>& shapes, const std::string& parent, const Matrix& m, int depth) {
        std::string out;
        for (std::size_t i : walk_->kids(shapes, parent)) out += item(shapes, i, m, depth);
        return out;
    }

    // a shape drawn as a picture: its canvas box (only for shapes in the document itself)
    std::string rasterised(const std::vector<Shape>& shapes, std::size_t i) {
        if (&shapes != &doc_.shapes) return "";
        Bounds b = shapeBounds(doc_, static_cast<int>(i));
        if (b.empty) return "";
        double pad = 4;
        for (const auto& e : shapes[i].effects) pad = std::max(pad, e.radius * 3 + std::max(std::abs(e.dx), std::abs(e.dy)));
        pad += shapes[i].strokeWidth;
        b.x0 -= pad, b.y0 -= pad, b.x1 += pad, b.y1 += pad;
        const double scale = std::min(2.0, 4000.0 / std::max({b.width(), b.height(), 1.0}));
        raster::Image img = renderShape(doc_, static_cast<int>(i), b.x0, b.y0, b.width(), b.height(), scale);
        const std::string name = image(img);
        const Matrix& p = walk_->page;  // the picture's corners on the page (it's drawn upright, y up)
        const Point tl = p.apply({b.x0, b.y0}), br = p.apply({b.x1, b.y1});
        return "q " + n4(br.x - tl.x) + " 0 0 " + n4(tl.y - br.y) + " " + n4(tl.x) + " " + n4(br.y) + " cm /" + name + " Do Q\n";
    }

    std::string item(const std::vector<Shape>& shapes, std::size_t i, const Matrix& m, int depth) {
        if (depth > 24) return "";
        const Shape& s = shapes[i];
        if (!s.visible || s.opacity <= 0) return "";
        if (Walk::rasterOnly(s)) {
            std::string r = rasterised(shapes, i);
            if (!r.empty()) return r;
        }
        std::string body;
        if (s.kind == ShapeKind::Group) {
            for (std::size_t k : walk_->kids(shapes, s.id)) {
                if (shapes[k].id != s.clip && shapes[k].id != s.mask) body += item(shapes, k, m, depth + 1);
            }
            if (!s.clip.empty()) {
                for (std::size_t k : walk_->kids(shapes, s.id)) {
                    if (shapes[k].id != s.clip) continue;
                    Outline o;
                    clipOutline(shapes, k, m, o, depth + 1);
                    body = "q\n" + pathOps(outlinePath(o), false) + (shapes[k].fillRule == "evenodd" && shapes[k].kind != ShapeKind::Text ? "W* n\n" : "W n\n") + body + "Q\n";
                }
            }
            if (!s.mask.empty()) {
                for (std::size_t k : walk_->kids(shapes, s.id)) {
                    if (shapes[k].id != s.mask) continue;
                    Shape shown = shapes[k];
                    std::string maskContent = item(shapes, k, m, depth + 1);
                    if (!shapes[k].visible) {  // a hidden mask shape still masks
                        std::vector<Shape> one{shown};
                        one[0].visible = true;
                        one[0].parent.clear();
                        maskContent = item(one, 0, m, depth + 1);
                    }
                    const std::string gs = gstate(1, 1, "normal", form(maskContent, true));
                    body = "q /" + gs + " gs\n" + body + "Q\n";
                }
            }
        } else if (s.kind == ShapeKind::Instance) {
            if (const Symbol* sym = doc_.findSymbol(s.symbol)) body = list(sym->shapes, "", m * shapeMatrix(s) * Matrix::translate(s.cx, s.cy), depth + 1);
        } else {
            body = leaf(s, m);
        }
        if (s.opacity < 1 || s.blend != "normal") {
            const std::string gs = gstate(s.opacity, s.opacity, s.blend);
            return "q /" + gs + " gs /" + placeForm(form(body)) + " Do Q\n";
        }
        return body;
    }

    void clipOutline(const std::vector<Shape>& shapes, std::size_t i, const Matrix& m, Outline& out, int depth) {
        if (depth > 24) return;
        const Shape& s = shapes[i];
        if (s.kind == ShapeKind::Group) {
            for (std::size_t k : walk_->kids(shapes, s.id)) clipOutline(shapes, k, m, out, depth + 1);
            return;
        }
        if (s.kind == ShapeKind::Instance) {
            if (const Symbol* sym = doc_.findSymbol(s.symbol)) {
                const Matrix inner = m * shapeMatrix(s) * Matrix::translate(s.cx, s.cy);
                for (std::size_t k : walk_->kids(sym->shapes, "")) clipOutline(sym->shapes, k, inner, out, depth + 1);
            }
            return;
        }
        const Matrix full = m * shapeMatrix(s);
        Outline o = localOutline(s, 2.0);
        for (auto& c : o) {
            for (auto& p : c.points) p = full.apply(p);
        }
        out.insert(out.end(), o.begin(), o.end());
    }

    std::string leaf(const Shape& s, const Matrix& m) {
        const Matrix full = m * shapeMatrix(s);  // shape -> page
        BezierPath bp = shapePath(s);
        transformPath(bp, full);
        if (bp.empty()) return "";
        const std::string path = pathOps(bp, false);
        const bool canFill = s.kind != ShapeKind::Line;
        const bool evenOdd = s.kind != ShapeKind::Text && s.fillRule == "evenodd";
        const double k = full.scaleFactor();
        std::string out;
        auto item = [&](const AppearanceItem& a) {
            std::string o = "q /" + gstate(a.opacity, a.opacity, a.blend) + " gs\n";
            if (a.dx != 0 || a.dy != 0) {
                const Point d0 = m.apply({0, 0}), d1 = m.apply({a.dx, a.dy});
                o += "1 0 0 1 " + n4(d1.x - d0.x) + " " + n4(d1.y - d0.y) + " cm\n";
            }
            if (a.stroke) o += stroke(bp, s, a.width * k, a.style, rgb3(a.color) + " RG\n", rgb3(a.color) + " rg\n", full);
            else if (canFill) o += rgb3(a.color) + " rg\n" + path + (evenOdd ? "f*\n" : "f\n");
            return o + "Q\n";
        };
        for (const auto& a : s.appearance) {
            if (a.under) out += item(a);
        }
        if (canFill && (s.gradient || (s.fill && s.fill->a > 0))) {
            out += "q\n";
            if (s.fill && s.fill->a < 255 && !s.gradient) out += "/" + gstate(s.fill->a / 255.0, 1, "normal") + " gs\n";
            if (s.gradient && !s.gradient->stops.empty()) {
                const Bounds box = bounds(localOutline(s, 1.0));
                const Matrix toPage = full * Matrix::translate(box.x0, box.y0) * Matrix::scale(std::max(box.width(), 1e-9), std::max(box.height(), 1e-9));
                out += "/Pattern cs /" + shadingPattern(*s.gradient, toPage) + " scn\n";
            } else {
                out += rgb3(*s.fill) + " rg\n";
            }
            out += path + (evenOdd ? "f*\n" : "f\n") + "Q\n";
        }
        if (((s.stroke && s.stroke->a > 0) || s.strokeGradient) && s.strokeWidth > 0) {
            std::string q = "q\n";
            std::string sc, fc;
            if (s.strokeGradient && !s.strokeGradient->stops.empty()) {
                const Bounds box = bounds(localOutline(s, 1.0));
                const Matrix toPage = full * Matrix::translate(box.x0, box.y0) * Matrix::scale(std::max(box.width(), 1e-9), std::max(box.height(), 1e-9));
                const std::string pat = shadingPattern(*s.strokeGradient, toPage);
                sc = "/Pattern CS /" + pat + " SCN\n";
                fc = "/Pattern cs /" + pat + " scn\n";
            } else {
                if (s.stroke->a < 255) q += "/" + gstate(s.stroke->a / 255.0, s.stroke->a / 255.0, "normal") + " gs\n";
                sc = rgb3(*s.stroke) + " RG\n";
                fc = rgb3(*s.stroke) + " rg\n";
            }
            out += q + stroke(bp, s, s.strokeWidth * k, s.line, sc, fc, full) + "Q\n";
        }
        for (const auto& a : s.appearance) {
            if (!a.under) out += item(a);
        }
        return out;
    }

    // Plain strokes as PDF strokes; arrows, profiles and inside/outside as filled outlines.
    std::string stroke(const BezierPath& bp, const Shape& s, double width, const StrokeStyle& st, const std::string& strokeColour, const std::string& fillColour,
                       const Matrix& full) {
        (void)full;
        const bool closedShape = std::any_of(bp.begin(), bp.end(), [](const SubPath& sp) { return sp.closed; });
        const bool outlined = st.profile != "uniform" || (st.align != "center" && closedShape && s.kind != ShapeKind::Line) || st.startArrow != "none" ||
                              st.endArrow != "none";
        if (outlined) {
            const Outline line = flatten(bp, 2.0);
            Outline area = st.align != "center" && closedShape ? outlineStroke(line, width, st) : strokeArea(line, width, st);
            return fillColour + pathOps(outlinePath(area), false) + "f\n";
        }
        std::string o = n4(width) + " w " + std::to_string(st.cap == "butt" ? 0 : st.cap == "round" ? 1 : 2) + " J " +
                        std::to_string(st.join == "miter" ? 0 : st.join == "round" ? 1 : 2) + " j " + n4(st.miterLimit) + " M\n";
        if (!st.dash.empty()) {
            const double k = width / std::max(s.strokeWidth, 1e-9);
            o += "[";
            for (double d : st.dash) o += n4(d * k) + " ";
            o += "] " + n4(st.dashOffset * k) + " d\n";
        }
        return o + strokeColour + pathOps(bp, false) + "S\n";
    }
};

class EpsOut {
public:
    EpsOut(const VectorDoc& doc, int artboard) : doc_(doc), ab_(doc.artboard(artboard)) {}

    std::string run() {
        Walk w{doc_, Matrix::scale(kPt, -kPt) * Matrix::translate(-ab_.x, -(ab_.y + ab_.h)), {}};
        walk_ = &w;
        const int W = static_cast<int>(std::ceil(ab_.w * kPt)), H = static_cast<int>(std::ceil(ab_.h * kPt));
        std::string out = "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 " + std::to_string(W) + " " + std::to_string(H) + "\n%%HiResBoundingBox: 0 0 " +
                          n4(ab_.w * kPt) + " " + n4(ab_.h * kPt) + "\n%%Creator: wetwaresuite vector\n%%LanguageLevel: 3\n%%Pages: 1\n%%EndComments\n%%Page: 1 1\ngsave\n";
        if (doc_.background.a > 0) out += rgb3(doc_.background) + " setrgbcolor\nnewpath 0 0 moveto " + n4(ab_.w * kPt) + " 0 lineto " + n4(ab_.w * kPt) + " " +
                                          n4(ab_.h * kPt) + " lineto 0 " + n4(ab_.h * kPt) + " lineto closepath fill\n";
        out += list(doc_.shapes, "", w.page, 0);
        return out + "grestore\nshowpage\n%%EOF\n";
    }

private:
    const VectorDoc& doc_;
    Artboard ab_;
    Walk* walk_ = nullptr;

    std::string list(const std::vector<Shape>& shapes, const std::string& parent, const Matrix& m, int depth) {
        std::string out;
        for (std::size_t i : walk_->kids(shapes, parent)) out += item(shapes, i, m, depth);
        return out;
    }

    std::string item(const std::vector<Shape>& shapes, std::size_t i, const Matrix& m, int depth) {
        if (depth > 24) return "";
        const Shape& s = shapes[i];
        if (!s.visible || s.opacity <= 0) return "";
        if (s.kind == ShapeKind::Group) {
            std::string body;
            for (std::size_t k : walk_->kids(shapes, s.id)) {
                if (shapes[k].id != s.clip && shapes[k].id != s.mask) body += item(shapes, k, m, depth + 1);
            }
            for (std::size_t k : walk_->kids(shapes, s.id)) {
                if (shapes[k].id != s.clip) continue;
                const Matrix full = m * shapeMatrix(shapes[k]);
                BezierPath bp = shapes[k].kind == ShapeKind::Group ? BezierPath{} : shapePath(shapes[k]);
                transformPath(bp, full);
                body = "gsave\nnewpath\n" + pathOps(bp, true) + "clip newpath\n" + body + "grestore\n";
            }
            return body;
        }
        if (s.kind == ShapeKind::Instance) {
            const Symbol* sym = doc_.findSymbol(s.symbol);
            return sym ? list(sym->shapes, "", m * shapeMatrix(s) * Matrix::translate(s.cx, s.cy), depth + 1) : "";
        }
        const Matrix full = m * shapeMatrix(s);
        BezierPath bp = shapePath(s);
        transformPath(bp, full);
        if (bp.empty()) return "";
        const std::string path = "newpath\n" + pathOps(bp, true);
        const bool evenOdd = s.kind != ShapeKind::Text && s.fillRule == "evenodd";
        std::string out;
        if (s.kind != ShapeKind::Line && (s.gradient || s.fill || s.pattern)) {
            if (s.gradient && !s.gradient->stops.empty() && !s.gradient->freeform) {
                const Bounds box = bounds(localOutline(s, 1.0));
                const Matrix t = full * Matrix::translate(box.x0, box.y0) * Matrix::scale(std::max(box.width(), 1e-9), std::max(box.height(), 1e-9));
                out += "gsave\n" + path + (evenOdd ? "eoclip\n" : "clip\n") + "[" + n4(t.a) + " " + n4(t.b) + " " + n4(t.c) + " " + n4(t.d) + " " + n4(t.e) + " " +
                       n4(t.f) + "] concat\n" + shadingDict(*s.gradient) + " shfill\ngrestore\n";
            } else {
                Color c = s.fill.value_or(s.pattern ? s.pattern->color : Color{0, 0, 0, 255});
                if (s.gradient && s.gradient->freeform && !s.gradient->points.empty()) c = s.gradient->atPoint(0.5, 0.5);
                if (c.a > 0) out += rgb3(c) + " setrgbcolor\n" + path + (evenOdd ? "eofill\n" : "fill\n");
            }
        }
        if (s.stroke && s.stroke->a > 0 && s.strokeWidth > 0) {
            const double w = s.strokeWidth * full.scaleFactor();
            const bool closedShape = std::any_of(bp.begin(), bp.end(), [](const SubPath& sp) { return sp.closed; });
            if (s.line.profile != "uniform" || (s.line.align != "center" && closedShape) || s.line.startArrow != "none" || s.line.endArrow != "none") {
                const Outline line = flatten(bp, 2.0);
                const Outline area = s.line.align != "center" && closedShape ? outlineStroke(line, w, s.line) : strokeArea(line, w, s.line);
                out += rgb3(*s.stroke) + " setrgbcolor\nnewpath\n" + pathOps(outlinePath(area), true) + "fill\n";
            } else {
                out += rgb3(*s.stroke) + " setrgbcolor " + n4(w) + " setlinewidth " + std::to_string(s.line.cap == "butt" ? 0 : s.line.cap == "round" ? 1 : 2) +
                       " setlinecap " + std::to_string(s.line.join == "miter" ? 0 : s.line.join == "round" ? 1 : 2) + " setlinejoin " + n4(s.line.miterLimit) +
                       " setmiterlimit\n";
                if (!s.line.dash.empty()) {
                    const double k = full.scaleFactor();
                    out += "[";
                    for (double d : s.line.dash) out += n4(d * k) + " ";
                    out += "] " + n4(s.line.dashOffset * k) + " setdash\n";
                } else {
                    out += "[] 0 setdash\n";
                }
                out += path + "stroke\n";
            }
        }
        return out;
    }
};

std::string lowerExt(const std::filesystem::path& path) {
    std::string e = path.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return e;
}

void writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + path.string());
    out << bytes;
    if (!out) throw std::runtime_error("failed writing " + path.string());
}

}  // namespace

std::string toPdf(const VectorDoc& doc, int artboard) {
    PdfOut out(doc);
    std::vector<std::string> pages;
    if (artboard >= 0) pages.push_back(out.page(artboard));
    else {
        for (int i = 0; i < doc.artboardCount(); ++i) pages.push_back(out.page(doc.artboards.empty() ? -1 : i));
    }
    return out.finish(pages);
}

std::string toEps(const VectorDoc& doc, int artboard) { return EpsOut(doc, artboard).run(); }

void saveDoc(const VectorDoc& doc, const std::filesystem::path& path, double scale, int quality, int artboard) {
    const std::string e = lowerExt(path);
    if (e == "svg") return writeFile(path, toSvg(doc, artboard));
    if (e == "pdf" || e == "ai") return writeFile(path, toPdf(doc, artboard));
    if (e == "eps" || e == "ps") return writeFile(path, toEps(doc, std::max(0, artboard)));
    if (!raster::canWrite(path)) {
        std::string list = "svg, pdf, eps";
        for (const auto& x : raster::writableExtensions()) list += ", " + x;
        throw std::runtime_error("can't save ." + e + " files; use one of: " + list);
    }
    if (artboard >= 0) {
        const Artboard a = doc.artboard(artboard);
        raster::save(renderArea(doc, a.x, a.y, a.w, a.h, scale, true), path, quality);
        return;
    }
    raster::save(render(doc, scale), path, quality);
}

}  // namespace wetwaresuite::vector
