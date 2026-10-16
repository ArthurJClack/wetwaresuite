// Presets, placement, slicing, G-code and layer previews.
#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <tuple>

#include "achclwss/mesh/ops.hpp"
#include "achclwss/raster/paint.hpp"
#include "achclwss/slicer/slicer.hpp"

namespace achclwss::slicer {

using mesh::Vec3;

// ---------------------------------------------------------------- presets

namespace {

const char* kMarlinStart =
    "G90 ; absolute positioning\n"
    "M82 ; absolute extrusion\n"
    "M140 S{bed_temp} ; heat the bed\n"
    "M104 S{first_layer_temp} ; heat the nozzle\n"
    "G28 ; home all axes\n"
    "M190 S{bed_temp} ; wait for the bed\n"
    "M109 S{first_layer_temp} ; wait for the nozzle\n"
    "G92 E0\n"
    "G1 Z2.0 F3000\n"
    "G1 X0.5 Y20 Z0.3 F5000 ; purge line along the left edge\n"
    "G1 X0.5 Y{purge_end} Z0.3 F1500 E15\n"
    "G1 X1.0 Y{purge_end} Z0.3 F5000\n"
    "G1 X1.0 Y20 Z0.3 F1500 E30\n"
    "G92 E0\n"
    "G1 Z2.0 F3000\n";

const char* kPrusaStart =
    "M862.3 P \"MK4\" ; printer model check\n"
    "G90 ; absolute positioning\n"
    "M82 ; absolute extrusion\n"
    "M140 S{bed_temp}\n"
    "M104 S170 ; warm the nozzle without oozing while probing\n"
    "M190 S{bed_temp}\n"
    "G28 ; home\n"
    "G29 ; mesh bed levelling\n"
    "M104 S{first_layer_temp}\n"
    "M109 S{first_layer_temp}\n"
    "G92 E0\n"
    "G1 Z0.3 F720\n"
    "G1 X10 Y0.5 F5000 ; purge line along the front edge\n"
    "G1 X100 Y0.5 E15 F1000\n"
    "G92 E0\n"
    "G1 Z2.0 F3000\n";

const char* kBambuStart =
    "G90 ; absolute positioning\n"
    "M82 ; absolute extrusion\n"
    "M140 S{bed_temp}\n"
    "M104 S{first_layer_temp}\n"
    "G28 ; home\n"
    "M190 S{bed_temp}\n"
    "M109 S{first_layer_temp}\n"
    "G92 E0\n"
    "G1 Z0.3 F720\n"
    "G1 X20 Y0.5 F6000 ; purge line along the front edge\n"
    "G1 X120 Y0.5 E15 F1200\n"
    "G92 E0\n"
    "G1 Z2.0 F3000\n";

const char* kEnd =
    "G91 ; relative moves\n"
    "G1 E-2 F2700 ; pull the filament back\n"
    "G90\n"
    "G1 Z{park_z} F600 ; lift clear of the print\n"
    "G1 X0 Y{bed_y} F3000 ; present the print\n"
    "M104 S0 ; nozzle off\n"
    "M140 S0 ; bed off\n"
    "M107 ; fan off\n"
    "M84 ; motors off\n";

}  // namespace

const std::vector<std::string>& printerNames() {
    static const std::vector<std::string> names = {"ender3", "prusa_mk4", "bambu_a1", "custom"};
    return names;
}

const std::vector<std::string>& filamentNames() {
    static const std::vector<std::string> names = {"pla", "petg", "abs", "tpu"};
    return names;
}

const std::vector<std::string>& qualityNames() {
    static const std::vector<std::string> names = {"draft", "normal", "fine"};
    return names;
}

std::optional<Printer> printerPreset(const std::string& name) {
    Printer p;
    p.name = name;
    p.endGcode = kEnd;
    if (name == "ender3" || name == "custom") {
        p.bedX = 220, p.bedY = 220, p.bedZ = 250;
        p.startGcode = kMarlinStart;
    } else if (name == "prusa_mk4") {
        p.bedX = 250, p.bedY = 210, p.bedZ = 220;
        p.startGcode = kPrusaStart;
    } else if (name == "bambu_a1") {
        p.bedX = 256, p.bedY = 256, p.bedZ = 256;
        p.startGcode = kBambuStart;
    } else {
        return std::nullopt;
    }
    return p;
}

std::optional<Filament> filamentPreset(const std::string& name) {
    Filament f;
    f.name = name;
    if (name == "pla") {
        f.nozzleTemp = 205, f.firstLayerTemp = 210, f.bedTemp = 60, f.fan = 100, f.density = 1.24;
    } else if (name == "petg") {
        f.nozzleTemp = 235, f.firstLayerTemp = 240, f.bedTemp = 80, f.fan = 50, f.density = 1.27;
    } else if (name == "abs") {
        f.nozzleTemp = 245, f.firstLayerTemp = 250, f.bedTemp = 100, f.fan = 0, f.density = 1.04;
    } else if (name == "tpu") {
        f.nozzleTemp = 225, f.firstLayerTemp = 225, f.bedTemp = 50, f.fan = 50, f.density = 1.21, f.maxSpeed = 25;
    } else {
        return std::nullopt;
    }
    return f;
}

bool applyQuality(Settings& s, const std::string& name) {
    if (name == "draft") {
        s.layerHeight = 0.28, s.firstLayerHeight = 0.28, s.perimeters = 2, s.topLayers = 3, s.bottomLayers = 3, s.speed = 60;
    } else if (name == "normal") {
        s.layerHeight = 0.2, s.firstLayerHeight = 0.2, s.perimeters = 2, s.topLayers = 4, s.bottomLayers = 4, s.speed = 50;
    } else if (name == "fine") {
        s.layerHeight = 0.12, s.firstLayerHeight = 0.2, s.perimeters = 3, s.topLayers = 7, s.bottomLayers = 6, s.speed = 40;
    } else {
        return false;
    }
    s.quality = name;
    return true;
}

// ---------------------------------------------------------------- placement

void dropToBed(mesh::Mesh& m) {
    if (m.empty()) return;
    const mesh::Bounds b = mesh::bounds(m);
    if (b.min.z != 0) mesh::transform(m, mesh::Mat4::translate({0, 0, -b.min.z}));
}

void moveCenterTo(mesh::Mesh& m, double x, double y) {
    if (m.empty()) return;
    const mesh::Bounds b = mesh::bounds(m);
    const Vec3 c = b.center();
    mesh::transform(m, mesh::Mat4::translate({x - c.x, y - c.y, -b.min.z}));
}

void centerOnBed(mesh::Mesh& m, const Printer& printer) { moveCenterTo(m, printer.bedX / 2, printer.bedY / 2); }

bool layFlat(mesh::Mesh& m) {
    if (m.empty()) return false;
    // group triangles by plane (normal + offset), sum their areas
    std::map<std::tuple<long, long, long, long>, std::pair<double, Vec3>> groups;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        Vec3 n = m.faceNormal(t);
        const double a = length(n) * 0.5;
        if (a <= 1e-12) continue;
        n = n / (2 * a);
        const double d = dot(n, m.corner(t, 0));
        auto key = std::make_tuple(std::lround(n.x * 1000), std::lround(n.y * 1000), std::lround(n.z * 1000), std::lround(d * 50));
        auto& g = groups[key];
        g.first += a;
        g.second += n * a;
    }
    std::vector<std::pair<double, Vec3>> sorted;
    for (const auto& [k, g] : groups) sorted.push_back({g.first, normalize(g.second)});
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [a, n] : sorted) {
        // the face must be on the outside: nothing sticks out further along n
        double far = -1e300, faceD = -1e300;
        for (const auto& p : m.positions) far = std::max(far, dot(n, p));
        for (std::size_t t = 0; t < m.triangleCount(); ++t) {
            Vec3 fn = normalize(m.faceNormal(t));
            if (dot(fn, n) > 0.999) faceD = std::max(faceD, dot(n, m.corner(t, 0)));
        }
        if (faceD < far - 0.05) continue;
        const Vec3 down{0, 0, -1};
        const double c = std::clamp(dot(n, down), -1.0, 1.0);
        if (c > 1 - 1e-9) return false;  // already flat on it
        const mesh::Bounds b = mesh::bounds(m);
        const Vec3 center = b.center();
        Vec3 axis = cross(n, down);
        if (length(axis) < 1e-9) axis = {1, 0, 0};
        const double degrees = std::acos(c) * 180.0 / mesh::kPi;
        mesh::transform(m, mesh::Mat4::translate(center) * mesh::Mat4::rotateAxis(axis, degrees) * mesh::Mat4::translate(-center));
        moveCenterTo(m, center.x, center.y);
        return true;
    }
    return false;
}

Fit checkFit(const mesh::Mesh& m, const Printer& p) {
    if (m.empty()) return {false, "the model has no triangles"};
    const mesh::Bounds b = mesh::bounds(m);
    const Vec3 s = b.size();
    char buf[256];
    constexpr double eps = 1e-6;
    if (s.x > p.bedX + eps || s.y > p.bedY + eps || s.z > p.bedZ + eps) {
        std::snprintf(buf, sizeof buf, "the model is %.1f x %.1f x %.1f mm but the %s prints at most %.0f x %.0f x %.0f mm", s.x, s.y,
                      s.z, p.name.c_str(), p.bedX, p.bedY, p.bedZ);
        return {false, buf};
    }
    if (b.min.x < -eps || b.min.y < -eps || b.max.x > p.bedX + eps || b.max.y > p.bedY + eps) {
        std::snprintf(buf, sizeof buf, "the model hangs off the bed (it spans x %.1f..%.1f, y %.1f..%.1f); run slicer.place to centre it",
                      b.min.x, b.max.x, b.min.y, b.max.y);
        return {false, buf};
    }
    return {};
}

// ---------------------------------------------------------------- slicing

const char* toString(PathType t) {
    switch (t) {
        case PathType::Skirt: return "skirt";
        case PathType::Brim: return "brim";
        case PathType::OuterWall: return "outer wall";
        case PathType::InnerWall: return "inner wall";
        case PathType::SolidInfill: return "solid infill";
        case PathType::Infill: return "infill";
        case PathType::Support: return "support";
    }
    return "?";
}

namespace {

Region outersOnly(const Region& r) {
    Region out;
    for (const auto& l : r) {
        if (mesh::loopArea(l) > 0) out.push_back(l);
    }
    return out;
}

Region dropSmall(Region r, double minArea) {
    // drops islands (with their holes) and holes below minArea
    Region out;
    for (auto& island : islands(r)) {
        if (area(island) < minArea) continue;
        for (auto& l : island) {
            if (std::abs(mesh::loopArea(l)) >= minArea * 0.25) out.push_back(std::move(l));
        }
    }
    return out;
}

Loop convexHull(std::vector<Vec2> pts) {
    std::sort(pts.begin(), pts.end(), [](const Vec2& a, const Vec2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return {};
    Loop h(pts.size() * 2);
    std::size_t k = 0;
    for (const auto& p : pts) {
        while (k >= 2 && cross(h[k - 1] - h[k - 2], p - h[k - 2]) <= 0) --k;
        h[k++] = p;
    }
    for (std::size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        const Vec2& p = pts[i - 1];
        while (k >= t && cross(h[k - 1] - h[k - 2], p - h[k - 2]) <= 0) --k;
        h[k++] = p;
    }
    h.resize(k - 1);
    return h;
}

double dist2(const Vec2& a, const Vec2& b) { return dot(a - b, a - b); }

struct Island {
    Region outline;
    std::vector<Region> walls;  // walls[0] is the outer wall
    Region infillArea;
    Vec2 anchor;
};

class Orderer {
public:
    Orderer(const Settings& s, std::size_t layer) : seam_(s.seam), layer_(layer) {}

    Vec2 pos{0, 0};

    void loops(const Region& region, PathType type, std::vector<Path>& out) {
        std::vector<char> done(region.size(), 0);
        for (std::size_t n = 0; n < region.size(); ++n) {
            std::size_t best = 0;
            double bd = 1e300;
            for (std::size_t i = 0; i < region.size(); ++i) {
                if (done[i]) continue;
                double d = 1e300;
                for (const auto& p : region[i]) d = std::min(d, dist2(p, pos));
                if (d < bd) bd = d, best = i;
            }
            done[best] = 1;
            const Loop& l = region[best];
            if (l.size() < 3) continue;
            std::size_t s = seamIndex(l);
            Path path{type, {}, true};
            path.points.reserve(l.size());
            for (std::size_t k = 0; k < l.size(); ++k) path.points.push_back(l[(s + k) % l.size()]);
            pos = path.points.front();
            out.push_back(std::move(path));
        }
    }

    void segments(std::vector<Segment> segs, PathType type, std::vector<Path>& out) {
        std::vector<char> done(segs.size(), 0);
        for (std::size_t n = 0; n < segs.size(); ++n) {
            std::size_t best = 0;
            bool flip = false;
            double bd = 1e300;
            for (std::size_t i = 0; i < segs.size(); ++i) {
                if (done[i]) continue;
                const double d1 = dist2(segs[i].first, pos), d2 = dist2(segs[i].second, pos);
                if (d1 < bd) bd = d1, best = i, flip = false;
                if (d2 < bd) bd = d2, best = i, flip = true;
            }
            done[best] = 1;
            Segment s = segs[best];
            if (flip) std::swap(s.first, s.second);
            out.push_back({type, {s.first, s.second}, false});
            pos = s.second;
        }
    }

private:
    std::size_t seamIndex(const Loop& l) {
        std::size_t best = 0;
        if (seam_ == "rear") {
            for (std::size_t i = 1; i < l.size(); ++i) {
                if (l[i].y > l[best].y + 1e-6 || (std::abs(l[i].y - l[best].y) <= 1e-6 && l[i].x < l[best].x)) best = i;
            }
        } else if (seam_ == "random") {
            std::size_t h = (layer_ + 1) * 2654435761u + (++counter_) * 40503u;
            best = (h ^ (h >> 13)) % l.size();
        } else {
            double bd = 1e300;
            for (std::size_t i = 0; i < l.size(); ++i) {
                const double d = dist2(l[i], pos);
                if (d < bd) bd = d, best = i;
            }
        }
        return best;
    }

    std::string seam_;
    std::size_t layer_;
    std::size_t counter_ = 0;
};

// Runs fn(0..n-1) on all cores. Each index must touch only its own outputs.
void parallelFor(std::size_t n, const std::function<void(std::size_t)>& fn) {
    const std::size_t workers = std::min<std::size_t>(n, std::max(1u, std::thread::hardware_concurrency()));
    if (workers <= 1) {
        for (std::size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::exception_ptr error;
    std::mutex errorMutex;
    std::vector<std::thread> pool;
    for (std::size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            for (std::size_t i = next++; i < n; i = next++) {
                try {
                    fn(i);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!error) error = std::current_exception();
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

bool insideBed(const Path& p, const Printer& pr) {
    for (const auto& v : p.points) {
        if (v.x < 0 || v.y < 0 || v.x > pr.bedX || v.y > pr.bedY) return false;
    }
    return true;
}

}  // namespace

SliceResult slice(const mesh::Mesh& input, const Config& c) {
    const Settings& s = c.settings;
    const double lw = c.lineWidth();
    if (!(s.layerHeight > 0) || !(s.firstLayerHeight > 0)) throw std::runtime_error("layer heights must be positive");
    if (!(lw > 0)) throw std::runtime_error("line width must be positive");
    if (input.empty()) throw std::runtime_error("the model has no triangles");

    mesh::Mesh owned;
    const mesh::Mesh* mp = &input;
    const mesh::Bounds b0 = mesh::bounds(input);
    if (std::abs(b0.min.z) > 1e-9) {
        owned = input;
        dropToBed(owned);
        mp = &owned;
    }
    const mesh::Mesh& m = *mp;
    const double top = mesh::bounds(m).max.z;

    // layer heights
    std::size_t n = 1;
    if (top > s.firstLayerHeight) n += static_cast<std::size_t>(std::ceil((top - s.firstLayerHeight) / s.layerHeight - 1e-6));
    SliceResult result;
    result.layers.resize(n);
    std::vector<double> planes(n);
    for (std::size_t i = 0; i < n; ++i) {
        Layer& L = result.layers[i];
        L.height = i == 0 ? s.firstLayerHeight : s.layerHeight;
        L.z = s.firstLayerHeight + static_cast<double>(i) * s.layerHeight;
        if (i == 0) L.z = s.firstLayerHeight;
        planes[i] = std::min(L.z - L.height / 2, top - 1e-4);
    }
    const auto raw = mesh::sliceLayers(m, planes);

    // outlines and walls
    const double minArea = lw * lw * 0.25;
    std::vector<Region> outline(n);
    std::vector<std::vector<Island>> isl(n);
    std::vector<Region> infillRegion(n);
    const int P = std::max(0, s.perimeters);
    const double infillInset = P > 0 ? P * lw - 0.15 * lw : lw * 0.5;
    parallelFor(n, [&](std::size_t i) {
        std::vector<Loop> loops;
        for (const auto& l : raw[i]) {
            Loop sl = simplify(l, 0.01);
            if (sl.size() >= 3) loops.push_back(std::move(sl));
        }
        outline[i] = dropSmall(normalize(loops), minArea);
        for (auto& part : islands(outline[i])) {
            Island is;
            is.outline = part;
            for (int k = 0; k < P; ++k) {
                Region w = offset(part, -(lw / 2 + k * lw));
                if (w.empty()) break;
                is.walls.push_back(std::move(w));
            }
            is.infillArea = offset(part, -infillInset);
            is.anchor = part.front().front();
            infillRegion[i].insert(infillRegion[i].end(), is.infillArea.begin(), is.infillArea.end());
            isl[i].push_back(std::move(is));
        }
    });

    // top and bottom solid layers: a layer's infill is sparse only where every
    // layer within bottom_layers below and top_layers above has infill too
    std::vector<Region> solid(n), sparse(n);
    const bool allSolid = s.infill >= 99.999;
    parallelFor(n, [&](std::size_t i) {
        if (infillRegion[i].empty()) return;
        if (allSolid) {
            solid[i] = infillRegion[i];
            return;
        }
        const long lo = static_cast<long>(i) - s.bottomLayers, hi = static_cast<long>(i) + s.topLayers;
        Region inner;
        if (lo >= 0 && hi < static_cast<long>(n)) {
            std::vector<const Region*> stack;
            for (long j = lo; j <= hi; ++j) {
                const Region& other = infillRegion[static_cast<std::size_t>(j)];
                if (stack.empty() || other != *stack.back()) stack.push_back(&other);  // identical layers count once
            }
            inner = intersectAll(stack);
        }
        if (inner.empty()) {
            solid[i] = infillRegion[i];
            return;
        }
        Region sol = subtract(infillRegion[i], inner);
        if (!sol.empty()) sol = offset(offset(sol, -lw * 0.25), lw * 0.25);  // drop slivers thinner than half a line
        solid[i] = dropSmall(std::move(sol), minArea);
        sparse[i] = std::move(inner);
    });

    // supports: columns under overhangs, grown down layer by layer
    std::vector<Region> support(n);
    if (s.supports != "none" && n > 2) {
        const double reach = s.layerHeight * std::tan(std::clamp(s.supportAngle, 0.0, 89.0) * mesh::kPi / 180.0);
        const double xyGap = std::max(0.6, lw * 1.5);
        std::vector<Region> overhang(n);
        parallelFor(n, [&](std::size_t j) {
            if (j == 0 || outline[j].empty()) return;
            // grown back by the reach so the strips of neighbouring layers join up
            Region steep = dropSmall(subtract(outline[j], offset(outline[j - 1], reach)), 0.5);
            if (!steep.empty()) overhang[j] = offset(steep, reach * 1.1 + 0.05);
        });
        Region acc;
        for (std::size_t k = n; k-- > 0;) {
            if (k + 2 < n && !overhang[k + 2].empty()) acc = unite(acc, overhang[k + 2]);
            if (acc.empty()) continue;
            if (!outline[k].empty()) acc = subtract(acc, offset(outline[k], xyGap));
            acc = dropSmall(std::move(acc), 0.5);
            support[k] = acc;
        }
        if (s.supports == "buildplate") {
            for (std::size_t k = 1; k < n; ++k) support[k] = intersect(support[k], support[k - 1]);
        }
        if (s.brim > 0 && !support[0].empty()) support[0] = subtract(support[0], offset(outersOnly(outline[0]), s.brim + lw));
    }

    // toolpaths
    const double density = std::clamp(s.infill, 0.0, 100.0) / 100.0;
    Vec2 pos{0, 0};
    for (std::size_t i = 0; i < n; ++i) {
        Layer& L = result.layers[i];
        Orderer ord(s, i);
        ord.pos = pos;
        if (i == 0) {
            // skirt (outermost loop first), then brim (outside in), all kept on the bed
            const Region base = outersOnly(outline[0]);
            std::vector<Path> first;
            if (s.skirt > 0) {
                std::vector<Vec2> pts;
                for (const auto& l : base) pts.insert(pts.end(), l.begin(), l.end());
                for (const auto& l : support[0]) pts.insert(pts.end(), l.begin(), l.end());
                const Loop hull = convexHull(pts);
                const double d0 = s.skirtDistance + std::max(0.0, s.brim) + lw / 2;
                for (int k = s.skirt; k-- > 0 && !hull.empty();) ord.loops(offset(Region{hull}, d0 + k * lw), PathType::Skirt, first);
            }
            if (s.brim > 0 && !base.empty()) {
                const int count = std::max(1, static_cast<int>(std::lround(s.brim / lw)));
                for (int k = count; k-- > 0;) ord.loops(offset(base, lw / 2 + k * lw), PathType::Brim, first);
            }
            for (auto& p : first) {
                if (insideBed(p, c.printer)) {
                    L.paths.push_back(std::move(p));
                } else {
                    const std::string w = std::string("part of the ") + toString(p.type) + " would leave the bed and was left out";
                    if (std::find(result.warnings.begin(), result.warnings.end(), w) == result.warnings.end()) result.warnings.push_back(w);
                }
            }
        }

        // infill lines for the whole layer, then handed to their islands
        std::vector<Segment> solidLines, sparseLines;
        if (!solid[i].empty()) solidLines = fillLines(solid[i], lw, i % 2 ? 135.0 : 45.0);
        if (!sparse[i].empty() && density > 0) {
            auto add = [&](double spacing, double angle) {
                auto v = fillLines(sparse[i], spacing, angle);
                sparseLines.insert(sparseLines.end(), v.begin(), v.end());
            };
            if (s.infillPattern == "lines") {
                add(lw / density, i % 2 ? 135.0 : 45.0);
            } else if (s.infillPattern == "triangles") {
                for (double a : {0.0, 60.0, 120.0}) add(3 * lw / density, a);
            } else {
                for (double a : {45.0, 135.0}) add(2 * lw / density, a);
            }
        }
        auto tooShort = [&](const Segment& sg) { return length(sg.second - sg.first) < lw; };
        solidLines.erase(std::remove_if(solidLines.begin(), solidLines.end(), tooShort), solidLines.end());
        sparseLines.erase(std::remove_if(sparseLines.begin(), sparseLines.end(), tooShort), sparseLines.end());
        auto& parts = isl[i];
        std::vector<std::vector<Segment>> partSolid(parts.size()), partSparse(parts.size());
        auto assign = [&](const std::vector<Segment>& lines, std::vector<std::vector<Segment>>& to) {
            for (const auto& sg : lines) {
                const Vec2 mid = (sg.first + sg.second) * 0.5;
                std::size_t k = 0;
                while (k + 1 < parts.size() && !contains(parts[k].outline, mid)) ++k;
                if (!parts.empty()) to[k].push_back(sg);
            }
        };
        assign(solidLines, partSolid);
        assign(sparseLines, partSparse);

        std::vector<char> done(parts.size(), 0);
        for (std::size_t c2 = 0; c2 < parts.size(); ++c2) {
            std::size_t best = 0;
            double bd = 1e300;
            for (std::size_t k = 0; k < parts.size(); ++k) {
                if (done[k]) continue;
                double d = 1e300;
                for (const auto& p : parts[k].outline.front()) d = std::min(d, dist2(p, ord.pos));
                if (d < bd) bd = d, best = k;
            }
            done[best] = 1;
            const Island& is = parts[best];
            for (std::size_t w = is.walls.size(); w-- > 0;) ord.loops(is.walls[w], w == 0 ? PathType::OuterWall : PathType::InnerWall, L.paths);
            ord.segments(partSolid[best], PathType::SolidInfill, L.paths);
            ord.segments(partSparse[best], PathType::Infill, L.paths);
        }
        if (!support[i].empty()) {
            auto lines = fillLines(support[i], lw * 100.0 / std::clamp(s.supportDensity, 1.0, 100.0), 0.0);
            lines.erase(std::remove_if(lines.begin(), lines.end(), tooShort), lines.end());
            ord.segments(std::move(lines), PathType::Support, L.paths);
        }
        pos = ord.pos;
    }
    return result;
}

// ---------------------------------------------------------------- G-code

namespace {

std::string num(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string fillTemplate(std::string t, const std::map<std::string, std::string>& vars) {
    for (const auto& [k, v] : vars) {
        const std::string key = "{" + k + "}";
        for (auto pos = t.find(key); pos != std::string::npos; pos = t.find(key, pos + v.size())) t.replace(pos, key.size(), v);
    }
    // '|' separates lines in templates typed on one line
    std::replace(t.begin(), t.end(), '|', '\n');
    if (!t.empty() && t.back() != '\n') t += '\n';
    return t;
}

const char* cureType(PathType t) {
    switch (t) {
        case PathType::Skirt: return "SKIRT";
        case PathType::Brim: return "SKIRT";
        case PathType::OuterWall: return "WALL-OUTER";
        case PathType::InnerWall: return "WALL-INNER";
        case PathType::SolidInfill: return "SKIN";
        case PathType::Infill: return "FILL";
        case PathType::Support: return "SUPPORT";
    }
    return "FILL";
}

class Emitter {
public:
    explicit Emitter(const Config& c) : s_(c.settings) {
        const double r = c.filament.diameter / 2;
        filamentArea_ = mesh::kPi * r * r;
        out.reserve(1 << 20);
    }

    std::string out;
    double seconds = 0, filament = 0;
    Vec2 pos{0, 0};
    double z = 0;

    void line(const std::string& s) {
        out += s;
        out += '\n';
    }

    void retract() {
        if (retracted_ || s_.retraction <= 0) return;
        e_ -= s_.retraction;
        line("G1 E" + num(e_, 5) + " F" + num(s_.retractionSpeed * 60, 0));
        seconds += s_.retraction / std::max(1.0, s_.retractionSpeed);
        retracted_ = true;
    }

    void unretract() {
        if (!retracted_) return;
        e_ += s_.retraction;
        line("G1 E" + num(e_, 5) + " F" + num(s_.retractionSpeed * 60, 0));
        seconds += s_.retraction / std::max(1.0, s_.retractionSpeed);
        retracted_ = false;
    }

    void resetE() {
        line("G92 E0");
        e_ = 0;
    }

    void moveZ(double nz) {
        line("G0 Z" + num(nz, 3) + " F600");
        seconds += std::abs(nz - z) / 10.0;
        z = nz;
    }

    void travel(const Vec2& to) {
        const double d = length(to - pos);
        if (d < 1e-6) return;
        if (d > s_.retractMinTravel) retract();
        line("G0 X" + num(to.x, 3) + " Y" + num(to.y, 3) + " F" + num(s_.travelSpeed * 60, 0));
        seconds += d / std::max(1.0, s_.travelSpeed);
        pos = to;
    }

    void extrude(const Vec2& to, double width, double height, double speed) {
        const double d = length(to - pos);
        if (d < 1e-6) return;
        unretract();
        const double de = d * width * height / filamentArea_;
        e_ += de;
        filament += de;
        std::string f;
        if (speed != lastSpeed_) {
            f = " F" + num(speed * 60, 0);
            lastSpeed_ = speed;
        }
        line("G1 X" + num(to.x, 3) + " Y" + num(to.y, 3) + " E" + num(e_, 5) + f);
        seconds += d / std::max(0.1, speed);
        pos = to;
    }

    void forceSpeed() { lastSpeed_ = -1; }

private:
    const Settings& s_;
    double filamentArea_ = 1;
    double e_ = 0;
    bool retracted_ = false;
    double lastSpeed_ = -1;
};

}  // namespace

Gcode toGcode(const SliceResult& sliced, const Config& c, const std::string& modelName) {
    const Settings& s = c.settings;
    const Filament& f = c.filament;
    const double lw = c.lineWidth();
    auto cap = [&](double v) { return f.maxSpeed > 0 ? std::min(v, f.maxSpeed) : v; };

    double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300, maxz = 0;
    for (const auto& L : sliced.layers) {
        if (!L.paths.empty()) maxz = std::max(maxz, L.z);
        for (const auto& p : L.paths) {
            for (const auto& v : p.points) {
                minx = std::min(minx, v.x), miny = std::min(miny, v.y), maxx = std::max(maxx, v.x), maxy = std::max(maxy, v.y);
            }
        }
    }
    if (minx > maxx) minx = miny = maxx = maxy = 0;

    Emitter em(c);
    em.line("M107 ; fan off for the first layer");
    em.resetE();
    for (std::size_t i = 0; i < sliced.layers.size(); ++i) {
        const Layer& L = sliced.layers[i];
        em.line(";LAYER:" + std::to_string(i));
        em.line(";Z:" + num(L.z, 3));
        if (i == 1) {
            if (f.nozzleTemp != f.firstLayerTemp) em.line("M104 S" + num(f.nozzleTemp, 0));
            if (f.fan > 0) em.line("M106 S" + num(std::clamp(f.fan, 0.0, 100.0) * 2.55, 0));
        }
        em.retract();
        em.resetE();
        em.moveZ(L.z);
        const double speed = cap(i == 0 ? s.firstLayerSpeed : s.speed);
        const char* lastType = nullptr;
        for (const auto& p : L.paths) {
            if (p.points.size() < 2) continue;
            const char* t = cureType(p.type);
            if (t != lastType) {
                em.line(std::string(";TYPE:") + t);
                lastType = t;
            }
            em.travel(p.points.front());
            const double sp = p.type == PathType::OuterWall && i > 0 ? cap(speed * 0.75) : speed;
            for (std::size_t k = 1; k < p.points.size(); ++k) em.extrude(p.points[k], lw, L.height, sp);
            if (p.closed) em.extrude(p.points.front(), lw, L.height, sp);
        }
    }
    em.line(";END_GCODE");

    Gcode g;
    g.seconds = em.seconds;
    g.filamentMm = em.filament;
    const double r = f.diameter / 2;
    g.extrudedVolume = em.filament * mesh::kPi * r * r;
    g.filamentGrams = g.extrudedVolume / 1000.0 * f.density;

    std::map<std::string, std::string> vars = {
        {"nozzle_temp", num(f.nozzleTemp, 0)},
        {"first_layer_temp", num(f.firstLayerTemp, 0)},
        {"bed_temp", num(f.bedTemp, 0)},
        {"bed_x", num(c.printer.bedX, 1)},
        {"bed_y", num(c.printer.bedY, 1)},
        {"purge_end", num(std::max(30.0, c.printer.bedY - 20), 1)},
        {"park_z", num(std::min(c.printer.bedZ, maxz + 10), 2)},
    };
    std::string head;
    head += ";FLAVOR:Marlin\n";
    head += ";Generated by the ACHCLWSS slicer\n";
    head += ";TIME_ESTIMATE:" + num(std::round(g.seconds), 0) + "\n";
    head += ";TIME:" + num(std::round(g.seconds), 0) + "\n";
    head += ";Filament used: " + num(g.filamentMm / 1000.0, 4) + "m\n";
    head += ";FILAMENT_USED_MM:" + num(g.filamentMm, 1) + "\n";
    head += ";FILAMENT_USED_G:" + num(g.filamentGrams, 2) + "\n";
    head += ";LAYER_COUNT:" + std::to_string(sliced.layers.size()) + "\n";
    head += ";Layer height: " + num(s.layerHeight, 3) + "\n";
    if (!modelName.empty()) head += ";MODEL:" + modelName + "\n";
    head += ";PRINTER:" + c.printer.name + " bed " + num(c.printer.bedX, 1) + "x" + num(c.printer.bedY, 1) + "x" +
            num(c.printer.bedZ, 1) + " nozzle " + num(c.printer.nozzle, 2) + "\n";
    head += ";FILAMENT:" + f.name + " " + num(f.nozzleTemp, 0) + "C (first layer " + num(f.firstLayerTemp, 0) + "C) bed " +
            num(f.bedTemp, 0) + "C fan " + num(f.fan, 0) + "%\n";
    head += ";SETTINGS: quality " + s.quality + ", line width " + num(lw, 3) + ", perimeters " + std::to_string(s.perimeters) +
            ", top " + std::to_string(s.topLayers) + ", bottom " + std::to_string(s.bottomLayers) + ", infill " + num(s.infill, 1) +
            "% " + s.infillPattern + ", supports " + s.supports + "\n";
    head += ";MINX:" + num(minx, 3) + "\n;MINY:" + num(miny, 3) + "\n;MINZ:0\n;MAXX:" + num(maxx, 3) + "\n;MAXY:" + num(maxy, 3) +
            "\n;MAXZ:" + num(maxz, 3) + "\n";
    head += fillTemplate(c.printer.startGcode, vars);
    g.text = head + em.out + fillTemplate(c.printer.endGcode, vars);
    return g;
}

// ---------------------------------------------------------------- previews

raster::Image renderLayer(const SliceResult& sliced, std::size_t layer, const Config& c, int size, bool travel, bool wholeBed) {
    size = std::clamp(size, 64, 4096);
    double minx = 0, miny = 0, maxx = c.printer.bedX, maxy = c.printer.bedY;
    if (!wholeBed) {
        minx = miny = 1e300;
        maxx = maxy = -1e300;
        for (const auto& L : sliced.layers) {
            for (const auto& p : L.paths) {
                for (const auto& v : p.points) {
                    minx = std::min(minx, v.x), miny = std::min(miny, v.y), maxx = std::max(maxx, v.x), maxy = std::max(maxy, v.y);
                }
            }
        }
        if (minx > maxx) minx = miny = 0, maxx = c.printer.bedX, maxy = c.printer.bedY;
        const double pad = std::max(2.0, std::max(maxx - minx, maxy - miny) * 0.04);
        minx -= pad, miny -= pad, maxx += pad, maxy += pad;
    }
    const double scale = size / std::max(maxx - minx, maxy - miny);
    const double ox = (size - (maxx - minx) * scale) / 2, oy = (size - (maxy - miny) * scale) / 2;
    auto X = [&](double x) { return ox + (x - minx) * scale; };
    auto Y = [&](double y) { return size - (oy + (y - miny) * scale); };

    raster::Image img(size, size, {214, 218, 224, 255});
    raster::fillRect(img, X(0), Y(c.printer.bedY), c.printer.bedX * scale, c.printer.bedY * scale, {248, 248, 250, 255});
    const raster::Color grid{226, 229, 234, 255};
    for (double g = 10; g < c.printer.bedX; g += 10) raster::drawLine(img, X(g), Y(0), X(g), Y(c.printer.bedY), grid, 1);
    for (double g = 10; g < c.printer.bedY; g += 10) raster::drawLine(img, X(0), Y(g), X(c.printer.bedX), Y(g), grid, 1);
    if (layer >= sliced.layers.size()) return img;

    auto color = [](PathType t) -> raster::Color {
        switch (t) {
            case PathType::Skirt:
            case PathType::Brim: return {120, 120, 132, 255};
            case PathType::OuterWall: return {214, 56, 48, 255};
            case PathType::InnerWall: return {56, 168, 82, 255};
            case PathType::SolidInfill: return {226, 186, 34, 255};
            case PathType::Infill: return {236, 118, 32, 255};
            case PathType::Support: return {40, 176, 204, 255};
        }
        return {0, 0, 0, 255};
    };
    const double w = std::max(1.0, c.lineWidth() * scale * 0.9);
    const Layer& L = sliced.layers[layer];
    Vec2 pos{-1e9, -1e9};
    for (const auto& p : L.paths) {
        if (p.points.size() < 2) continue;
        if (travel && pos.x > -1e8 && length(p.points.front() - pos) > 1e-6) {
            raster::drawLine(img, X(pos.x), Y(pos.y), X(p.points.front().x), Y(p.points.front().y), {70, 120, 220, 150}, 1);
        }
        const raster::Color col = color(p.type);
        for (std::size_t k = 1; k < p.points.size(); ++k) {
            raster::drawLine(img, X(p.points[k - 1].x), Y(p.points[k - 1].y), X(p.points[k].x), Y(p.points[k].y), col, w);
        }
        if (p.closed) raster::drawLine(img, X(p.points.back().x), Y(p.points.back().y), X(p.points.front().x), Y(p.points.front().y), col, w);
        pos = p.closed ? p.points.front() : p.points.back();
    }
    return img;
}

}  // namespace achclwss::slicer
