#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "wetwaresuite/mesh/csg.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/ops.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::mesh;
namespace fs = std::filesystem;

namespace {

bool approx(double a, double b, double rel) { return std::fabs(a - b) <= rel * std::max(std::fabs(a), std::fabs(b)) + 1e-12; }

// Area of a regular n-gon inscribed in radius r: what our round primitives really are.
double ngon(double r, int n) { return 0.5 * n * r * r * std::sin(2 * kPi / n); }

bool sameBounds(const Mesh& a, const Mesh& b, double tol) {
    Bounds x = bounds(a), y = bounds(b);
    return length(x.min - y.min) <= tol && length(x.max - y.max) <= tol;
}

std::size_t nonBackground(const raster::Image& img, raster::Color bg) {
    std::size_t n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) n += img.at(x, y) == bg ? 0 : 1;
    }
    return n;
}

double totalArea(const std::vector<Loop>& loops) {
    double a = 0;
    for (const auto& l : loops) a += loopArea(l);
    return a;
}

}  // namespace

int main() {
    // ---- math ----
    {
        Mat4 r = Mat4::rotateZ(90);
        Vec3 p = r.point({1, 0, 0});
        CHECK(p.x == 0 && p.y == 1 && p.z == 0);
        Mat4 t = Mat4::translate({1, 2, 3}) * Mat4::scale(2);
        CHECK(t.point({1, 1, 1}) == (Vec3{3, 4, 5}));
        Vec3 q = t.inverse().point({3, 4, 5});
        CHECK(length(q - Vec3{1, 1, 1}) < 1e-12);
        Vec3 rx = Mat4::rotate({90, 0, 0}).point({0, 1, 0});
        CHECK(length(rx - Vec3{0, 0, 1}) < 1e-12);
        CHECK(Mat4::scale({-1, 1, 1}).determinant3() < 0);
        Bounds b;
        CHECK(b.empty());
        b.add({1, 2, 3});
        b.add({-1, 0, 5});
        CHECK(b.size() == (Vec3{2, 2, 2}) && b.center() == (Vec3{0, 1, 4}));
    }

    // ---- primitives: exact volumes and closed surfaces ----
    {
        Mesh b = box({2, 3, 4});
        CHECK(b.triangleCount() == 12 && b.vertexCount() == 8);
        CHECK(approx(volume(b), 24, 1e-12));
        CHECK(approx(surfaceArea(b), 2 * (6 + 8 + 12), 1e-12));
        CHECK(isWatertight(b));
        CHECK(bounds(b).min == (Vec3{-1, -1.5, -2}));
        CHECK(bounds(box({1, 1, 1}, false)).min == (Vec3{0, 0, 0}));

        Mesh c = cylinder(2, 5, 48);
        CHECK(isWatertight(c));
        CHECK(approx(volume(c), ngon(2, 48) * 5, 1e-9));
        CHECK(approx(volume(c), kPi * 4 * 5, 0.01));
        CHECK(approx(bounds(c).size().x, 4, 1e-12) && approx(bounds(c).size().z, 5, 1e-12));

        Mesh k = cone(2, 1, 3, 64, false);
        CHECK(isWatertight(k));
        CHECK(approx(volume(k), kPi * 3 / 3 * (4 + 2 + 1), 0.01));
        CHECK(bounds(k).min.z == 0 && bounds(k).max.z == 3);
        Mesh point = cone(1, 0, 2, 32);
        CHECK(isWatertight(point) && approx(volume(point), ngon(1, 32) * 2 / 3, 1e-9));

        Mesh s = sphere(1.5, 96, 48);
        CHECK(isWatertight(s));
        CHECK(approx(volume(s), 4.0 / 3.0 * kPi * 1.5 * 1.5 * 1.5, 0.01));
        CHECK(approx(bounds(s).size().z, 3, 1e-12) && approx(bounds(s).size().x, 3, 1e-12));

        Mesh t = torus(3, 1, 96, 48);
        CHECK(isWatertight(t));
        CHECK(approx(volume(t), 2 * kPi * kPi * 3 * 1, 0.01));

        Mesh pl = plane(4, 2, 4, 2);
        CHECK(pl.triangleCount() == 16 && pl.hasNormals() && pl.hasUvs());
        CHECK(!isWatertight(pl) && approx(surfaceArea(pl), 8, 1e-12));
        CHECK(boundaryEdgeCount(pl) == 12);

        bool threw = false;
        try {
            box({0, 1, 1});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- extrude and revolve ----
    {
        Polygon2 frame{rectangle2(10, 6), {rectangle2(4, 2)}};
        Mesh e = extrude(frame, 2);
        CHECK(isWatertight(e));
        CHECK(approx(volume(e), (60 - 8) * 2, 1e-9));
        CHECK(bounds(e).min.z == 0 && bounds(e).max.z == 2);

        // Clockwise outer, two holes, an L-shaped (concave) outline.
        Polygon2 ell{{{0, 0}, {0, 4}, {2, 4}, {2, 2}, {6, 2}, {6, 0}}, {circle2(0.5, 16, {1, 1}), circle2(0.4, 12, {4.5, 1})}};
        Mesh l = extrude(ell, 1, true);
        CHECK(isWatertight(l));
        CHECK(approx(volume(l), 16 - ngon(0.5, 16) - ngon(0.4, 12), 1e-9));

        Mesh ring = revolve({{1, 0}, {2, 0}, {2, 1}, {1, 1}}, 360, 64);
        CHECK(isWatertight(ring));
        CHECK(approx(volume(ring), (ngon(2, 64) - ngon(1, 64)) * 1, 1e-9));
        Mesh half = revolve({{0, 0}, {1, 0}, {1, 2}, {0, 2}}, 180, 32);
        CHECK(isWatertight(half));
        CHECK(approx(volume(half), kPi * 2 / 2, 0.01));
        Mesh quarterRing = revolve({{1, 0}, {2, 0}, {2, 1}, {1, 1}}, 90, 16);
        CHECK(isWatertight(quarterRing) && volume(quarterRing) > 0);
    }

    // ---- mesh operations ----
    {
        Mesh b = box({1, 1, 1});
        Mesh u = unweld(b);
        CHECK(u.vertexCount() == 36);
        CHECK(isWatertight(u));  // matched by position
        weld(u);
        CHECK(u.vertexCount() == 8 && isWatertight(u));

        Mesh n = b;
        computeNormals(n, 30);
        CHECK(n.vertexCount() == 24 && n.hasNormals());  // split at every 90 degree edge
        Mesh sm = sphere(1, 32, 16);
        const auto before = sm.vertexCount();
        computeNormals(sm, 30);
        CHECK(sm.vertexCount() == before);  // smooth everywhere

        Mesh f = b;
        flip(f);
        CHECK(approx(volume(f), -1, 1e-12));
        Mesh m = b;
        transform(m, Mat4::scale({-2, 1, 1}));
        CHECK(approx(volume(m), 2, 1e-12));  // mirrored but still outward
        transform(m, Mat4::translate({10, 0, 0}));
        CHECK(approx(bounds(m).center().x, 10, 1e-12));

        Mesh two = box({1, 1, 1});
        append(two, m);
        CHECK(two.triangleCount() == 24 && approx(volume(two), 3, 1e-12));

        Mesh tol = box({1, 1, 1});
        tol.positions[0].x += 1e-7;
        Mesh extra = tol;
        append(extra, box({1, 1, 1}));
        weld(extra, 1e-5);
        CHECK(extra.vertexCount() == 8);
    }

    // ---- CSG ----
    {
        Mesh a = box({2, 2, 2});
        Mesh c = cylinder(0.5, 3, 32);
        Mesh diff = booleanDifference(a, c);
        CHECK(isWatertight(diff));
        CHECK(approx(volume(diff), 8 - ngon(0.5, 32) * 2, 0.01));

        Mesh uni = booleanUnion(a, c);
        CHECK(isWatertight(uni));
        CHECK(approx(volume(uni), 8 + ngon(0.5, 32) * 1, 0.01));

        Mesh inter = booleanIntersection(a, c);
        CHECK(isWatertight(inter));
        CHECK(approx(volume(inter), ngon(0.5, 32) * 2, 0.01));

        // Overlapping boxes with coplanar faces.
        Mesh b2 = box({2, 2, 2});
        transform(b2, Mat4::translate({1, 0, 0}));
        Mesh u2 = booleanUnion(a, b2);
        CHECK(isWatertight(u2) && approx(volume(u2), 12, 0.01));
        Mesh d2 = booleanDifference(a, b2);
        CHECK(isWatertight(d2) && approx(volume(d2), 4, 0.01));
        Mesh i2 = booleanIntersection(a, b2);
        CHECK(isWatertight(i2) && approx(volume(i2), 4, 0.01));

        // Cylinder through the side, off-centre (lots of T-junctions).
        Mesh side = cylinder(0.6, 4, 24);
        transform(side, Mat4::translate({0.3, 0, 0.2}) * Mat4::rotateX(90));
        Mesh d3 = booleanDifference(a, side);
        CHECK(isWatertight(d3));
        CHECK(approx(volume(d3), 8 - ngon(0.6, 24) * 2, 0.01));

        // Flush cut: the cutter shares the top face's plane.
        Mesh notch = box({1, 1, 1}, false);
        transform(notch, Mat4::translate({-0.5, -0.5, 0}));
        Mesh d4 = booleanDifference(a, notch);
        CHECK(isWatertight(d4) && approx(volume(d4), 7, 1e-9));
        // Chained operations on rotated parts stay closed.
        Mesh chain = a;
        for (int i = 0; i < 4; ++i) {
            Mesh bar = box({3, 0.4, 0.4});
            transform(bar, Mat4::rotate({0, 0, 22.5 * i}) * Mat4::translate({0, 0, 1}));
            chain = booleanUnion(chain, bar);
        }
        chain = booleanDifference(chain, sphere(0.8, 24, 12));
        CHECK(isWatertight(chain));
        CHECK(volume(chain) > 8 - 4.0 / 3 * kPi * 0.512 && volume(chain) < 8 + 4 * 3 * 0.16);

        // Disjoint inputs short-circuit.
        Mesh far = box({1, 1, 1});
        transform(far, Mat4::translate({10, 0, 0}));
        CHECK(approx(volume(booleanUnion(a, far)), 9, 1e-12));
        CHECK(booleanIntersection(a, far).empty());
        CHECK(approx(volume(booleanDifference(a, far)), 8, 1e-12));

        // A sphere minus a box corner.
        Mesh s = sphere(1, 24, 12);
        Mesh corner = box({1, 1, 1}, false);
        Mesh sd = booleanDifference(s, corner);
        CHECK(isWatertight(sd));
        CHECK(approx(volume(sd), volume(s) * 7 / 8, 0.02));
    }

    // ---- slicing ----
    {
        Mesh cube = box({2, 2, 2}, false);
        auto loops = slice(cube, 1.0);
        CHECK(loops.size() == 1);
        CHECK(loops.size() == 1 && approx(loopArea(loops[0]), 4, 1e-12));  // counter-clockwise
        // Exactly at the top face: vertices on the plane count as above, so the top edges form the loop.
        auto top = slice(cube, 2.0);
        CHECK(top.size() == 1 && approx(totalArea(top), 4, 1e-12));
        CHECK(slice(cube, 0.0).empty());
        CHECK(slice(cube, 5.0).empty());

        // Two stacked boxes: vertices exactly at the slice height (unwelded to also test exact-position matching).
        Mesh upper = box({2, 2, 1}, false);
        transform(upper, Mat4::translate({0, 0, 1}));
        Mesh tall = booleanUnion(box({2, 2, 1}, false), upper);
        CHECK(isWatertight(tall) && approx(volume(tall), 8, 1e-9));
        auto mid = slice(unweld(tall), 1.0);
        CHECK(mid.size() == 1 && approx(totalArea(mid), 4, 1e-9));

        Mesh cyl = cylinder(1, 2, 40, false);
        auto cl = slice(cyl, 0.5);
        CHECK(cl.size() == 1 && cl[0].size() == 40);
        CHECK(approx(totalArea(cl), ngon(1, 40), 1e-9));

        Mesh holed = booleanDifference(box({4, 4, 2}, false), [] {
            Mesh c = cylinder(1, 4, 32);
            transform(c, Mat4::translate({2, 2, 1}));
            return c;
        }());
        auto hl = slice(holed, 1.0);
        CHECK(hl.size() == 2);
        CHECK(approx(totalArea(hl), 16 - ngon(1, 32), 1e-6));
        int ccw = 0, cw = 0;
        for (const auto& l : hl) (loopArea(l) > 0 ? ccw : cw)++;
        CHECK(ccw == 1 && cw == 1);

        // Sphere sliced exactly through its equator vertices.
        Mesh sp = sphere(1, 32, 16);
        auto eq = slice(sp, 0.0);
        CHECK(eq.size() == 1 && approx(totalArea(eq), ngon(1, 32), 1e-9));

        auto layers = sliceLayers(cyl, {1.5, 0.25, 3.0});
        CHECK(layers.size() == 3 && layers[0].size() == 1 && layers[1].size() == 1 && layers[2].empty());
    }

    // ---- decimate ----
    {
        Mesh s = sphere(1, 64, 32);
        Mesh d = decimate(s, 500);
        CHECK(d.triangleCount() <= 500 && d.triangleCount() > 100);
        CHECK(approx(volume(d), volume(s), 0.15));
        CHECK(decimate(box({1, 1, 1}), 100).triangleCount() == 12);
        CHECK(clusterVertices(s, 0.5).triangleCount() < s.triangleCount());
    }

    // ---- STEP ----
    {
        // flat faces become one B-rep face each
        const std::string text = writeStep(box({20, 10, 5}));
        CHECK(text.rfind("ISO-10303-21;", 0) == 0 && text.find("MANIFOLD_SOLID_BREP") != std::string::npos);
        std::size_t faces = 0;
        for (std::size_t at = text.find("ADVANCED_FACE"); at != std::string::npos; at = text.find("ADVANCED_FACE", at + 1)) ++faces;
        CHECK(faces == 6);
        Mesh back = readStep(text);
        CHECK(approx(volume(back), 1000, 1e-9) && isWatertight(back));
        // parts written by OpenCASCADE (gmsh): planes, cylinders, cones, spheres, tori, rational B-splines, an assembly
        const std::string data = WETWARESUITE_MESH_TEST_DATA;
        struct Sample {
            const char* name;
            double volume;  // what OpenCASCADE reports for the exact solid
            double tolerance;
        };
        for (const Sample& sample : {Sample{"filleted", 10950.6254, 0.002}, Sample{"cone", 3267.2564, 0.005}, Sample{"sphere", 4188.7902, 0.01},
                                     Sample{"torus", 9869.6044, 0.005}, Sample{"loft", 9726.6155, 0.005}, Sample{"two", 1125.0, 1e-9}}) {
            std::string notes;
            Mesh m;
            try {
                std::ifstream in(data + "/occ-" + sample.name + ".step", std::ios::binary);
                std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                m = readStep(bytes, &notes);
            } catch (const std::exception& e) {
                std::printf("  %s: %s\n", sample.name, e.what());
            }
            const bool ok = isWatertight(m) && approx(volume(m), sample.volume, sample.volume * sample.tolerance);
            if (!ok) std::printf("  STEP %s: volume %g, %s\n", sample.name, volume(m), isWatertight(m) ? "closed" : "open");
            CHECK(ok);
        }
        bool threw = false;
        try {
            readStep("solid cube\nendsolid\n");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- IO round trips ----
    {
        auto dir = fs::temp_directory_path() / "wetwaresuite_test_mesh";
        fs::remove_all(dir);
        fs::create_directories(dir);
        Mesh shape = booleanDifference(box({20, 10, 5}), cylinder(2, 10, 24));
        const double v = volume(shape);

        CHECK(readableExtensions().size() == 10 && writableExtensions().size() == 10);
        CHECK(canRead("a.STL") && canWrite("x.glb") && canRead("a.FBX") && !canRead("a.dae"));
        for (const auto& ext : writableExtensions()) {
            const fs::path p = dir / ("shape." + ext);
            save(p, shape);
            Mesh back = load(p);
            const bool step = ext == "step" || ext == "stp";  // flat faces come back re-triangulated
            const bool ok = (step || back.triangleCount() == shape.triangleCount()) && approx(volume(back), v, 1e-5) &&
                            sameBounds(back, shape, 1e-4) && isWatertight(back);
            if (!ok) std::printf("round trip failed for .%s\n", ext.c_str());
            CHECK(ok);
        }
        // STEP faces whose edges carry points on a straight run (CSG cuts leave them) still come back closed
        for (const auto& [r, segments] : std::vector<std::pair<double, int>>{{1, 12}, {1.37, 20}, {1, 20}, {3.59, 24}}) {
            Mesh cut = booleanDifference(box({20, 10, 5}), cylinder(r, 10, segments));
            Mesh back = readStep(writeStep(cut));
            const bool ok = isWatertight(back) && approx(volume(back), volume(cut), 1e-5);
            if (!ok) std::printf("STEP collinear edge points: r %g, %d segments\n", r, segments);
            CHECK(ok);
        }
        // FBX: an ASCII file as Blender/Maya write them (Y-up, cm, quads, a translated and scaled model)
        {
            const std::string ascii = R"fbx(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 100, "Geometry::Cube", "Mesh" {
		Vertices: *24 {
			a: -1,-1,1,1,-1,1,-1,1,1,1,1,1,-1,1,-1,1,1,-1,-1,-1,-1,1,-1,-1
		}
		PolygonVertexIndex: *24 {
			a: 0,1,3,-3,2,3,5,-5,4,5,7,-7,6,7,1,-1,1,7,5,-4,6,0,2,-5
		}
		GeometryVersion: 124
		LayerElementUV: 0 {
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *8 {
				a: 0,0,1,0,1,1,0,1
			}
			UVIndex: *24 {
				a: 0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3
			}
		}
	}
	Model: 200, "Model::Cube", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",10,20,30
			P: "Lcl Scaling", "Lcl Scaling", "", "A",2,2,2
		}
	}
}
Connections:  {
	C: "OO",200,0
	C: "OO",100,200
}
)fbx";
            Mesh cube = readFbx(ascii);
            CHECK(cube.triangleCount() == 12 && cube.hasUvs() && isWatertight(cube));
            CHECK(approx(volume(cube), 64, 1e-9));  // 2x2x2 scaled by 2
            // Y-up (10, 20, 30) lands at (10, -30, 20) Z-up
            const Bounds cb = bounds(cube);
            CHECK(approx(cb.center().x, 10, 1e-9) && approx(cb.center().y, -30, 1e-9) && approx(cb.center().z, 20, 1e-9));
            // binary written by us reads back the same, UVs included
            Mesh back = readFbx(writeFbx(cube));
            CHECK(back.triangleCount() == 12 && back.hasUvs() && approx(volume(back), 64, 1e-6) && sameBounds(back, cube, 1e-6));
            bool threw = false;
            try {
                readFbx("Objects: {\n");
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
        }
        // ASCII variants.
        for (const char* ext : {"stl", "ply"}) {
            SaveOptions o;
            o.ascii = true;
            std::string bytes = encode(shape, ext, o);
            CHECK(bytes.find(std::string(ext) == "stl" ? "solid" : "format ascii") != std::string::npos);
            Mesh back = decode(bytes, ext);
            CHECK(back.triangleCount() == shape.triangleCount() && approx(volume(back), v, 1e-5));
        }
        // Attributes survive OBJ, PLY and glTF.
        Mesh pl = plane(2, 2, 2, 2);
        for (const char* ext : {"obj", "ply", "glb", "gltf"}) {
            Mesh back = decode(encode(pl, ext), ext);
            const bool ok = back.hasUvs() && back.hasNormals() && back.triangleCount() == 8 && sameBounds(back, pl, 1e-6) &&
                            length(back.normals[0] - Vec3{0, 0, 1}) < 1e-6;
            if (!ok) std::printf("attributes lost in .%s\n", ext);
            CHECK(ok);
        }
        // Hand-written OBJ with quads, negative indices and v//vn faces.
        const std::string obj =
            "# a quad and a triangle\nmtllib x.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvn 0 0 1\n"
            "usemtl red\nf 1//1 2//1 3//1 4//1\nv 0 0 1\nf -4 -3 -1\n";
        Mesh o = readObj(obj);
        CHECK(o.triangleCount() == 3 && o.hasNormals() == false);  // the last face has no normal, so normals are dropped
        // Hand-written OFF with a comment and a quad.
        Mesh off = readOff("OFF\n# square\n4 1 0\n0 0 0\n1 0 0\n1 1 0\n0 1 0\n4 0 1 2 3\n");
        CHECK(off.triangleCount() == 2 && approx(surfaceArea(off), 1, 1e-12));
        // Hand-written ascii PLY with an extra element and quads.
        Mesh ply = readPly(
            "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
            "property uchar red\nelement face 1\nproperty list uchar int vertex_indices\nelement edge 1\n"
            "property int vertex1\nproperty int vertex2\nend_header\n0 0 0 255\n1 0 0 255\n1 1 0 255\n0 1 0 255\n4 0 1 2 3\n0 1\n");
        CHECK(ply.triangleCount() == 2);
        // Binary STL is detected even when the header starts with "solid".
        std::string stl = writeStl(shape);
        stl.replace(0, 5, "solid");
        CHECK(readStl(stl).triangleCount() == shape.triangleCount());
        // glTF with an external .bin buffer next to it.
        {
            std::string gltf = writeGltf(shape);
            const auto at = gltf.find("data:application/octet-stream;base64,");
            CHECK(at != std::string::npos);
            // Extract the buffer, write it next to the file, point the uri at it.
            const auto end = gltf.find('"', at);
            const std::string uri = gltf.substr(at, end - at);
            Mesh fromEmbedded = readGltf(gltf);
            std::string glb = writeGlb(shape);
            CHECK(glb.size() > 12 && glb.compare(0, 4, "glTF") == 0);
            const std::uint32_t jsonLen = static_cast<unsigned char>(glb[12]) | (static_cast<unsigned char>(glb[13]) << 8) |
                                          (static_cast<unsigned char>(glb[14]) << 16) | (static_cast<unsigned char>(glb[15]) << 24);
            const std::string bin = glb.substr(20 + jsonLen + 8);
            {
                std::ofstream(dir / "ext.bin", std::ios::binary) << bin;
            }
            std::string external = gltf.substr(0, at) + "ext.bin" + gltf.substr(end);
            {
                std::ofstream(dir / "ext.gltf", std::ios::binary) << external;
            }
            Mesh fromFile = load(dir / "ext.gltf");
            CHECK(fromFile.triangleCount() == shape.triangleCount() && approx(volume(fromFile), v, 1e-5));
            CHECK(fromEmbedded.triangleCount() == shape.triangleCount());
            bool threw = false;
            try {
                readGltf(external);  // no base directory to find ext.bin in
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
            (void)uri;
        }
        // Z-up survives glTF: the tall side stays tall.
        Mesh tall = box({1, 1, 5});
        CHECK(approx(bounds(decode(writeGlb(tall), "glb")).size().z, 5, 1e-6));
        // Errors are readable, not crashes.
        bool threw = false;
        try {
            load(dir / "missing.stl");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            decode("garbage", "ply");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            decode("not a zip", "3mf");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        fs::remove_all(dir);
    }

    // ---- render ----
    {
        Mesh shape = booleanDifference(box({2, 2, 2}), cylinder(0.6, 3, 32));
        RenderOptions o;
        auto t0 = std::chrono::steady_clock::now();
        raster::Image img = render(shape, 256, 256, o);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(img.width() == 256 && img.height() == 256);
        const std::size_t covered = nonBackground(img, o.background);
        CHECK(covered > 256 * 256 / 5);
        CHECK(covered < 256 * 256);              // margins stay background
        CHECK(img.at(0, 0) == o.background);
        CHECK(ms < 2000);
        for (const auto& name : viewNames()) {
            RenderOptions v;
            v.view = *parseView(name);
            v.outline = false;
            v.smooth = name == "top";
            CHECK(nonBackground(render(shape, 64, 48, v), v.background) > 64 * 48 / 10);
        }
        CHECK(parseView("side") == View::Right && !parseView("nope"));
        // Top view of a holed box shows the background through the hole.
        RenderOptions top;
        top.view = View::Top;
        raster::Image ti = render(shape, 101, 101, top);
        CHECK(ti.at(50, 50) == top.background);
        CHECK(ti.at(20, 20) != top.background);
        RenderOptions clear;
        clear.background = {0, 0, 0, 0};
        raster::Image ci = render(shape, 64, 64, clear);
        CHECK(ci.at(0, 0).a == 0 && ci.at(32, 20).a == 255);
        CHECK(nonBackground(render(Mesh{}, 16, 16), o.background) == 0);
        CHECK(nonBackground(render(plane(1, 1), 32, 32), o.background) > 100);  // flat, open meshes too

        // orbit camera: pitch 90 is the top view, the size holds while turning, zoom and pan move it
        RenderOptions front;
        front.outline = front.antialias = false;
        RenderOptions orb = front;
        orb.orbit = Orbit{0, 90, 1, 0, 0};
        const raster::Image oi = render(shape, 81, 81, orb);
        CHECK(oi.at(40, 40) == front.background && oi.at(40, 25) != front.background);  // down through the hole
        std::size_t lastCovered = 0;
        bool steady = true;
        for (double yaw : {0.0, 20.0, 45.0, 90.0}) {
            orb.orbit->yaw = yaw;
            orb.orbit->pitch = 0;
            const std::size_t c = nonBackground(render(box({2, 2, 2}), 100, 100, orb), front.background);
            if (lastCovered && (c < lastCovered / 2 || c > lastCovered * 2)) steady = false;
            lastCovered = c;
        }
        CHECK(steady && lastCovered > 0);
        orb.orbit = Orbit{30, 20, 1, 0, 0};
        const std::size_t base = nonBackground(render(shape, 100, 100, orb), front.background);
        orb.orbit->zoom = 2;
        CHECK(nonBackground(render(shape, 100, 100, orb), front.background) > base * 2);
        orb.orbit->zoom = 1;
        orb.orbit->panX = 3;  // pushed off to the right, out of view
        CHECK(nonBackground(render(shape, 100, 100, orb), front.background) < base / 4);
        orb.orbit = Orbit{0, 0, 1, 0, 0};  // front
        CHECK(nonBackground(render(shape, 64, 64, orb), front.background) > 64 * 64 / 10);
        // documents: 0 x 0 asks, a real size draws
        auto md = newMesh(shape);
        std::vector<std::uint8_t> rgba;
        CHECK(md->renderView(OrbitView{}, 0, 0, rgba) && rgba.empty());
        CHECK(md->renderView(OrbitView{}, 40, 30, rgba) && rgba.size() == 40u * 30u * 4u);
        // lines alone draw (a sketch on an empty part), and fit the camera
        RenderOptions lineOnly;
        lineOnly.lines.push_back({{{0, 0, 0}, {10, 0, 0}, {10, 5, 0}}, {255, 0, 0, 255}});
        CHECK(nonBackground(render(Mesh{}, 64, 64, lineOnly), lineOnly.background) > 20);
    }

    // ---- document + commands ----
    {
        Session s;
        auto doc = newMesh(box({1, 2, 3}));
        CHECK(doc->kind() == "mesh");
        CHECK(doc->summary().find("12 triangles") != std::string::npos);
        CHECK(doc->summary().find("watertight") != std::string::npos);
        auto pv = doc->preview();
        CHECK(pv && pv->mime == "image/png" && raster::decode(pv->bytes).width() == 512);
        const std::string id = s.workspace().add(doc, "mesh");
        CHECK(id == "mesh1");

        s.registry().add({"test.scale", "Scale", "scale a mesh", {docParam(), {"factor", ParamType::Number, "factor", true}, previewParam(), viewParam()}, true},
                         meshCommand(s, [](MeshDoc& d, const Params& p) {
                             const double f = numberParam(p, "factor");
                             if (f <= 0) return Result::failure("factor must be positive");
                             transform(d.mesh, Mat4::scale(f));
                             return meshResult("scaled", d, p);
                         }));
        Result r = s.run("test.scale", {{"doc", id}, {"factor", 2.0}, {"preview", true}, {"view", std::string("front")}});
        CHECK(r.ok && r.attachments.size() == 1);
        CHECK(approx(volume(doc->mesh), 48, 1e-12));
        CHECK(std::get<std::int64_t>(*r.field("triangles")) == 12);
        CHECK(s.history().undo());
        CHECK(approx(volume(doc->mesh), 6, 1e-12));
        CHECK(!s.run("test.scale", {{"doc", id}, {"factor", -1.0}}).ok);
        CHECK(approx(volume(doc->mesh), 6, 1e-12));
        CHECK(!s.run("test.scale", {{"doc", std::string("mesh9")}, {"factor", 2.0}}).ok);
    }

    return check::summary("mesh");
}
