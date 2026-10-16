// Helpers shared by the layout sources but not part of the public API.
#pragma once

#include <cmath>
#include <memory>
#include <string>

#include "wetwaresuite/layout/layout.hpp"
#include "wetwaresuite/pdfdoc/fonts.hpp"

namespace wetwaresuite::vector {
class VectorDoc;
}

namespace wetwaresuite::layout {

constexpr double kPi = 3.14159265358979323846;
constexpr double kPtToMm = 25.4 / 72.0;
constexpr double kMmToPt = 72.0 / 25.4;

struct Face {
    std::string key;  // PDF font resource key material: base font name or "ttf:<path>"
    bool standard = true;
    pdfdoc::Standard font = pdfdoc::Standard::Helvetica;
    std::string family = "helvetica";  // standard family (for the system substitute)
    bool bold = false, italic = false;
    std::shared_ptr<pdfdoc::TrueType> ttf;  // the face of a font_add family
    std::string label;                       // "Helvetica-Bold" or "brand bold"

    double advance(unsigned cp) const;  // 1/1000 em; cp must be WinAnsi-encodable
    pdfdoc::VMetrics metrics() const;
    // Outlines to draw: the TrueType itself, or a system substitute for a standard font. May be null.
    std::shared_ptr<pdfdoc::TrueType> outlines() const;
};

// Falls back to Helvetica when a family is unknown or its file can't be read.
std::shared_ptr<const Face> resolveFace(const Layout& doc, const std::string& family, bool bold, bool italic);
// The code point itself when WinAnsi has it, else '?'.
unsigned encodable(unsigned cp);

// A loaded picture, cached by path and modification time.
std::shared_ptr<const raster::Image> loadImage(const std::string& path, std::string* error = nullptr);
std::shared_ptr<const vector::VectorDoc> loadVector(const std::string& path, std::string* error = nullptr);
bool isVectorPath(const std::string& path);
std::string readFile(const std::string& path);  // throws

// Frame-local mm (x right, y down from the frame's top-left) to page/output coordinates.
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;  // x' = a x + c y + e, y' = b x + d y + f
    pdfdoc::Pt apply(double x, double y) const { return {a * x + c * y + e, b * x + d * y + f}; }
    Affine operator*(const Affine& m) const {  // this after m
        return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d, a * m.e + c * m.f + e,
                b * m.e + d * m.f + f};
    }
    Affine inverse() const {
        const double det = a * d - b * c;
        if (std::abs(det) < 1e-12) return {};
        Affine r{d / det, -b / det, -c / det, a / det, 0, 0};
        r.e = -(r.a * e + r.c * f);
        r.f = -(r.b * e + r.d * f);
        return r;
    }
};
// Frame-local mm -> page mm, with rotation about the centre (and mirroring for master frames).
Affine frameToPage(const Frame& f);
// Master frames as placed on a page (mirrored on left-hand pages of facing documents).
Frame placedMasterFrame(const Layout& doc, const Frame& f, int pageIndex);

// Visual bounds of a frame on its page, after rotation (mm).
Box frameBounds(const Frame& f);

std::string num(double v, int decimals = 2);
std::string lower(std::string s);

}  // namespace wetwaresuite::layout
