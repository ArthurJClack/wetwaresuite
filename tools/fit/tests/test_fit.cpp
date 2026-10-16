// Scan fit tests: the algorithms on synthetic "phone scans" (noisy, holey,
// in metres and Y-up like the apps write them), then the commands end to end
// and the MCP server. Scans stay a few thousand triangles so this runs fast.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/fit/fit.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/topology.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::fit;
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

double numField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    if (!v) return -1e300;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return -1e300;
}

NumberList listField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<NumberList>(*v) ? std::get<NumberList>(*v) : NumberList{};
}

std::string strField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : "";
}

// Small deterministic noise in [-a, a].
double noise(int i, int j, double a) {
    std::uint32_t h = static_cast<std::uint32_t>(i) * 73856093u ^ static_cast<std::uint32_t>(j) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return a * ((h % 2001) / 1000.0 - 1.0);
}

// A heightfield surface z = f(x, y) on a grid, skipping cells where skip() is true.
template <typename F, typename S>
Mesh surface(double x0, double x1, double y0, double y1, double step, F f, S skip, double amp) {
    Mesh m;
    const int nx = static_cast<int>((x1 - x0) / step) + 1, ny = static_cast<int>((y1 - y0) / step) + 1;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const double x = x0 + i * step, y = y0 + j * step;
            m.addVertex({x, y, f(x, y) + noise(i, j, amp)});
        }
    for (int j = 0; j + 1 < ny; ++j)
        for (int i = 0; i + 1 < nx; ++i) {
            const double cx = x0 + (i + 0.5) * step, cy = y0 + (j + 0.5) * step;
            if (skip(cx, cy)) continue;
            const auto a = static_cast<std::uint32_t>(j * nx + i);
            m.addTriangle(a, a + 1, a + 1 + static_cast<std::uint32_t>(nx));
            m.addTriangle(a, a + 1 + static_cast<std::uint32_t>(nx), a + static_cast<std::uint32_t>(nx));
        }
    mesh::cleanup(m);
    return m;
}

double dash(double x, double y) { return 20 + 25 * std::cos(y / 80.0) - 0.0008 * x * x; }

// The dash scan: a curved panel with a speaker opening, a little hole, a floating bit and a floor under it.
Mesh dashScan() {
    Mesh m = surface(-150, 150, -60, 60, 3, dash,
                     [](double x, double y) { return std::hypot(x - 90, y) < 40 || std::hypot(x + 60, y - 20) < 4; }, 0.12);
    Mesh junk = mesh::sphere(5, 12, 6);
    mesh::transform(junk, Mat4::translate({0, 0, 110}));
    mesh::append(m, junk);
    Mesh floor = surface(-220, 220, -160, 160, 20, [](double, double) { return -15.0; }, [](double, double) { return false; }, 0.0);
    mesh::append(m, floor);
    return m;
}

// Writes a mesh as a phone app would: metres, Y up.
void writePhoneObj(const fs::path& path, const Mesh& m) {
    std::ofstream f(path);
    f << "# scanner app export\n";
    for (const auto& p : m.positions) f << "v " << p.x / 1000 << " " << p.z / 1000 << " " << -p.y / 1000 << "\n";
    for (std::size_t t = 0; t < m.triangleCount(); ++t)
        f << "f " << m.indices[t * 3] + 1 << " " << m.indices[t * 3 + 1] + 1 << " " << m.indices[t * 3 + 2] + 1 << "\n";
}

// Half a pipe of radius r along X, seen from above (like a scanned roll bar), noisy.
Mesh pipeScan(double r) {
    Mesh m;
    const int na = 24, nl = 30;
    for (int j = 0; j <= nl; ++j)
        for (int i = 0; i <= na; ++i) {
            const double a = -0.1 + (mesh::kPi + 0.2) * i / na, x = -60 + 120.0 * j / nl;
            const double rr = r + noise(i, j, 0.08);
            m.addVertex({x, std::cos(a) * rr, 40 + std::sin(a) * rr});
        }
    for (int j = 0; j < nl; ++j)
        for (int i = 0; i < na; ++i) {
            const auto a = static_cast<std::uint32_t>(j * (na + 1) + i), b = a + na + 1;
            m.addTriangle(a, a + 1, b);
            m.addTriangle(a + 1, b + 1, b);
        }
    return m;
}

json call(mcp::Server& s, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*s.handle(req.dump()))["result"];
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_fit_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ---- maths ----
    {
        const double m[9] = {4, 1, 0, 1, 3, 0, 0, 0, 1};
        const Eigen3 e = eigenSymmetric(m);
        CHECK(near(e.values[0], 1, 1e-9) && e.values[1] < e.values[2]);
        CHECK(near(std::fabs(e.vectors[0].z), 1, 1e-9));
        const Mat4 r = rotationBetween({0, 0, 1}, {1, 0, 0});
        CHECK(near(r.direction({0, 0, 1}).x, 1, 1e-9));
        const Mat4 flipR = rotationBetween({0, 0, 1}, {0, 0, -1});
        CHECK(near(flipR.direction({0, 0, 1}).z, -1, 1e-9));
        const Frame f = frameFor({1, 2, 3}, {0, 1, 1}, 30);
        CHECK(near(mesh::dot(f.u, f.v), 0, 1e-9) && near(mesh::length(mesh::cross(f.u, f.v) - f.n), 0, 1e-9));
        CHECK(near(mesh::length(f.toLocal(f.toWorld(3, -2, 5)) - Vec3{3, -2, 5}), 0, 1e-9));
    }

    // ---- index ----
    {
        auto s = std::make_shared<const Mesh>(mesh::sphere(10, 24, 12));
        const Bvh b(s);
        auto hit = b.ray({0, 0, 50}, {0, 0, -1});
        CHECK(hit && near(hit->t, 40, 0.05));
        CHECK(b.rayAll({0, 0, 50}, {0, 0, -1}).size() >= 2);
        CHECK(!b.ray({30, 0, 50}, {0, 0, -1}));
        const auto n = b.nearest({0, 25, 0});
        CHECK(near(n.distance, 15, 0.1));
        // nearest agrees with brute force
        Vec3 q{3, -7, 12};
        double brute = 1e300;
        for (std::size_t t = 0; t < s->triangleCount(); ++t)
            brute = std::min(brute, mesh::length(mesh::closestPointOnTriangle(q, s->corner(t, 0), s->corner(t, 1), s->corner(t, 2)) - q));
        CHECK(near(b.nearest(q).distance, brute, 1e-9));
    }

    // ---- fits ----
    {
        // tilted noisy plane with a bump (outliers)
        Mesh pl = surface(-30, 30, -30, 30, 2, [](double x, double y) { return 0.2 * x + 0.1 * y + (std::hypot(x - 10, y) < 5 ? 4.0 : 0.0); },
                          [](double, double) { return false; }, 0.05);
        const auto vn = mesh::vertexNormals(pl);
        const PlaneFit f = fitPlane(patchAround(pl, vn, {0, 0, 0}, 25));
        const Vec3 truth = mesh::normalize(Vec3{-0.2, -0.1, 1});
        CHECK(mesh::dot(f.normal, truth) > std::cos(1.0 * mesh::kPi / 180));
        CHECK(f.rms < 0.1 && f.used < f.total);

        Mesh sp = mesh::sphere(20, 40, 20);
        const SphereFit sf = fitSphere(patchAround(sp, mesh::vertexNormals(sp), {0, 0, 20}, 15));
        CHECK(near(sf.radius, 20, 0.4) && mesh::length(sf.center) < 0.5);

        Mesh pipe = pipeScan(12);
        const CylinderFit cf = fitCylinder(patchAround(pipe, mesh::vertexNormals(pipe), {0, 0, 52}, 25));
        CHECK(near(cf.radius, 12, 0.3));
        CHECK(std::fabs(cf.axis.x) > std::cos(3.0 * mesh::kPi / 180));
        CHECK(near(cf.center.z, 40, 0.5) && near(cf.center.y, 0, 0.5));

        std::vector<Vec3> ring;
        for (int i = 0; i < 40; ++i) ring.push_back({5 + 30 * std::cos(i * 0.157), 3 + 30 * std::sin(i * 0.157), 7 + noise(i, 1, 0.1)});
        const CircleFit cc = fitCircle(ring);
        CHECK(near(cc.radius, 30, 0.3) && near(cc.center.x, 5, 0.3) && near(std::fabs(cc.normal.z), 1, 0.01));

        auto smooth = smoothCurve({{0, 0, 0}, {10, 0, 0}, {20, 0, 0}}, 2, 1);
        CHECK(smooth.size() == 21 && near(smooth.back().x, 20, 1e-9));
    }

    // ---- cleanup and sections ----
    {
        Mesh m = dashScan();
        const std::size_t before = m.triangleCount();
        const FloorResult fl = removeFloor(m, 0);
        CHECK(fl.found && near(fl.point.z, -15, 1.5) && m.triangleCount() < before);
        CHECK(pieces(m).size() == 2);
        CHECK(keepLargest(m, 1, 0) > 0 && pieces(m).size() == 1);
        const auto loops = boundaryLoops(m);
        CHECK(loops.size() == 3);  // the outer edge, the speaker opening, the small hole
        const HoleStats hs = fillHoles(m, 20);
        CHECK(hs.filled == 1 && hs.skipped == 2 && boundaryLoops(m).size() == 2);
        CHECK(noiseLevel(m) > 0 && noiseLevel(m) < 0.5);

        Mesh b = mesh::box({10, 20, 30});
        const Section s = section(b, {0, 0, 0}, {0, 0, 1});
        CHECK(s.lines.size() == 1 && s.closed[0] && near(polylineLength(s.lines[0], true), 60, 1e-6));
        const Section open = section(m, {0, 0, 0}, {1, 0, 0});
        CHECK(!open.lines.empty() && !open.closed[0]);
        Mesh c = m;
        CHECK(cropPlane(c, {0, 0, 0}, {1, 0, 0}) > 0 && mesh::bounds(c).min.x > -2);
        Bounds bb;
        bb.add({-50, -50, -100});
        bb.add({50, 50, 100});
        Mesh c2 = m;
        CHECK(cropBox(c2, bb, true) > 0 && mesh::bounds(c2).max.x < 52);

        const auto pts = readPoints("# header\nx,y,z\n1,2,3\n4 5 6 255 255 255\n7;8;9\n");
        CHECK(pts.size() == 3 && pts[2].z == 9);
        std::vector<Vec3> cloud;
        for (int i = 0; i < 2000; ++i) {
            const double a = i * 2.399963, zz = 1 - 2.0 * (i + 0.5) / 2000;
            const double rr = std::sqrt(1 - zz * zz) * 20;
            cloud.push_back({rr * std::cos(a), rr * std::sin(a), zz * 20});
        }
        Mesh fromCloud = pointsToSurface(cloud, 2);
        CHECK(!fromCloud.empty() && near(mesh::bounds(fromCloud).size().x, 42, 4));
    }

    // ---- builders ----
    {
        Mesh m = surface(-80, 80, -60, 60, 2.5, dash, [](double x, double y) { return std::hypot(x, y - 5) < 6; }, 0.1);
        auto shared = std::make_shared<const Mesh>(m);
        const Bvh b(shared);
        ConformSpec s;
        s.frame = frameFor({0, 0, dash(0, 0)}, {0, 0, 1});
        s.footprint = rectFootprint(50, 40, 6);
        s.gap = 0.5;
        s.thickness = 3;
        ConformStats st;
        Mesh plate = conformPart(b, s, &st);
        CHECK(mesh::isWatertight(plate) && mesh::volume(plate) > 0);
        CHECK(st.missed > 0);  // the hole under it got filled in
        const FitReport fr = checkFit(b, plate, 3, s.gap);
        CHECK(fr.near > 0 && fr.minGap > 0.2 && fr.inside == 0 && fr.contact > 0.3);
        const PrintReport pr = checkPrint(plate);
        CHECK(pr.watertight && near(pr.thinnest, 3, 0.6));

        s.block = true;
        s.tiltU = 10;
        Mesh block = conformPart(b, s, &st);
        CHECK(mesh::isWatertight(block) && st.maxThickness > 3.5);

        s.footprint = circleFootprint(60);
        s.footprint.holes.push_back(circleFootprint(30).outer);
        Mesh ring = conformPart(b, s, nullptr);
        CHECK(mesh::isWatertight(ring));

        ConformSpec off = s;
        off.frame = frameFor({500, 0, 0}, {0, 0, 1});
        bool threw = false;
        try {
            conformPart(b, off, nullptr);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);

        ClampSpec cs;
        cs.axis = {1, 0, 0};
        cs.radius = 12;
        auto one = clampParts(cs);
        CHECK(one.size() == 1 && isClosed(one[0]) && mesh::volume(one[0]) > 0);
        cs.pieces = 2;
        auto two = clampParts(cs);
        CHECK(two.size() == 2 && isClosed(two[0]) && isClosed(two[1]));

        Mesh strip = stripPart({{0, 0, 0}, {10, 0, 1}, {20, 0, 3}}, {0, 1, 0}, {0, 0, 1}, 0.4, 2, 8);
        CHECK(mesh::isWatertight(strip) && mesh::volume(strip) > 0);
        CHECK(isClosed(ballMount()) && isClosed(cameraMount()) && isClosed(goproMount()));
        CHECK(mesh::isWatertight(cupSolid(20, 2, 30, 2)) && mesh::isWatertight(cupSolid(20, 2, 30, 0)));
        CHECK(screwClearance("M4") == 4.5 && countersinkDiameter("M3") > 6 && screwClearance("M7") == 0);
        CHECK(gapColor(-1, 0.4).r == 168 && gapColor(0.4, 0.4).b > 200 && gapColor(10, 0.4).r > 200);
    }

    // ---- commands ----
    {
        Session s;
        fit::registerCommands(s);
        const fs::path obj = dir / "dash_scan.obj";
        writePhoneObj(obj, dashScan());
        Result o = s.run("fit.open", {{"path", obj.string()}, {"preview", true}});
        CHECK(o.ok && strField(o, "doc") == "fit1" && numField(o, "scale") == 1000 && o.attachments.size() == 1);
        const NumberList sz = listField(o, "size");
        CHECK(sz.size() == 3 && near(sz[0], 440, 2) && near(sz[2], 130, 3));  // floor 440 wide; z from the floor (-15) to the junk's top (115)
        Params d{{"doc", std::string("fit1")}};
        auto with = [&](Params extra) {
            Params q = d;
            for (auto& [k, v] : extra) q[k] = v;
            return q;
        };
        CHECK(s.run("fit.info", d).ok);
        CHECK(s.run("fit.remove_floor", d).ok);
        CHECK(s.run("fit.keep_largest", d).ok);
        Result fh = s.run("fit.fill_holes", with({{"max_size", 20.0}}));
        CHECK(fh.ok && numField(fh, "filled") == 1);
        CHECK(s.run("fit.smooth", with({{"iterations", std::int64_t{2}}})).ok);
        Result info = s.run("fit.info", d);
        CHECK(info.ok && info.message.find("1 piece") != std::string::npos);

        // measuring
        Result dist = s.run("fit.distance", with({{"from", NumberList{-100, 0, 37}}, {"to", NumberList{100, 0, 37}}, {"snap", false}}));
        CHECK(dist.ok && near(numField(dist, "distance"), 200, 1e-9));
        CHECK(s.run("fit.distance", with({{"from", NumberList{-100, 0, 90}}, {"to", NumberList{100, 0, 90}}})).ok);
        const fs::path svg = dir / "section.svg";
        Result sec = s.run("fit.section", with({{"axis", std::string("x")}, {"at", 0.0}, {"path", svg.string()}}));
        CHECK(sec.ok && near(numField(sec, "width"), 120, 4) && fs::exists(svg));
        CHECK(s.run("fit.section", with({{"axis", std::string("y")}, {"at", 0.0}, {"path", (dir / "section.png").string()}})).ok);
        Result rad = s.run("fit.radius", with({{"at", NumberList{0, 0, 45}}, {"radius", 30.0}}));
        CHECK(rad.ok && numField(rad, "radius") > 40);
        Result sn = s.run("fit.snap", with({{"point", NumberList{0, 0, 80}}}));
        CHECK(sn.ok && near(listField(sn, "point")[2], dash(0, 0), 1) && listField(sn, "normal")[2] > 0.9);
        Result pk = s.run("fit.pick", with({{"u", 0.5}, {"v", 0.5}, {"view", std::string("top")}, {"show", std::string("scan")}}));
        CHECK(pk.ok && strField(pk, "on") == "scan");
        CHECK(!s.run("fit.pick", with({{"u", 0.01}, {"v", 0.01}, {"view", std::string("top")}})).ok);

        // scale from a "ruler" measurement: the scan is right, so the factor is about 1
        Result sc = s.run("fit.scale", with({{"from", NumberList{-100, 0, 40}}, {"to", NumberList{100, 0, 40}}, {"length", 200.0}}));
        CHECK(sc.ok && near(numField(sc, "factor"), 1, 1e-9));

        // references
        Result pl = s.run("fit.plane", with({{"at", NumberList{0, 0, 45}}, {"radius", 12.0}}));
        CHECK(pl.ok && strField(pl, "name") == "plane1" && listField(pl, "direction")[2] > 0.99);
        Result ci = s.run("fit.circle", with({{"at", NumberList{90, 0, 40}}}));
        CHECK(ci.ok && near(numField(ci, "radius"), 40, 2.5));
        Result cu = s.run("fit.curve", with({{"at", NumberList{-120, 0, 40}}, {"axis", std::string("x")}, {"length", 60.0}}));
        CHECK(cu.ok && near(numField(cu, "length"), 60, 6));
        CHECK(s.run("fit.point", with({{"at", NumberList{10, 10, 40}}})).ok);
        CHECK(s.run("fit.refs", d).ok);
        CHECK(!s.run("fit.cylinder", with({{"at", NumberList{0, 0, 45}}, {"radius", 3.0}})).ok);  // too few points

        // parts
        Result plate = s.run("fit.plate", with({{"at", NumberList{-40, 0, 45}}, {"width", 50.0}, {"height", 40.0}, {"preview", true}}));
        CHECK(plate.ok && strField(plate, "part") == "plate1" && std::get<bool>(*plate.field("watertight")));
        Result ck = s.run("fit.check", with({{"part", std::string("plate1")}}));
        CHECK(ck.ok && numField(ck, "min_gap") > 0.1 && numField(ck, "inside") == 0 && ck.attachments.size() == 1);
        Result hole = s.run("fit.hole", with({{"part", std::string("plate1")}, {"screw", std::string("M4")}, {"countersink", true}}));
        CHECK(hole.ok && std::get<bool>(*hole.field("watertight")));
        CHECK(!s.run("fit.hole", with({{"part", std::string("plate1")}, {"screw", std::string("M7")}})).ok);
        Result mount = s.run("fit.mount", with({{"part", std::string("plate1")}, {"type", std::string("magnet")}, {"depth", 1.5}, {"at", NumberList{-55, 0, 50}}}));
        CHECK(mount.ok);
        Result blk = s.run("fit.block", with({{"plane", std::string("plane1")}, {"shape", std::string("circle")}, {"diameter", 30.0}, {"tilt", NumberList{10, 0}}}));
        CHECK(blk.ok && strField(blk, "part") == "block1");
        Result phone = s.run("fit.phone_mount", with({{"at", NumberList{40, -30, 40}}, {"screws", true}}));
        CHECK(phone.ok && strField(phone, "part") == "phone1");
        Result pod = s.run("fit.gauge_pod", with({{"at", NumberList{-110, 20, 40}}, {"gauge", 52.0}}));
        CHECK(pod.ok);
        Result sw = s.run("fit.switch_panel", with({{"at", NumberList{0, 30, 40}}, {"switches", std::int64_t{2}}}));
        CHECK(sw.ok && std::get<bool>(*sw.field("watertight")));
        Result ring = s.run("fit.speaker_ring", with({{"circle", std::string("circle1")}, {"speaker", std::string("4")}, {"flange", 8.0}}));
        CHECK(ring.ok);
        Result strip = s.run("fit.strip", with({{"curve", std::string("curve1")}}));
        CHECK(strip.ok && std::get<bool>(*strip.field("watertight")));
        Result pr = s.run("fit.printability", with({{"part", std::string("plate1")}}));
        CHECK(pr.ok && std::get<bool>(*pr.field("watertight")) && numField(pr, "thinnest") > 0.5);

        // rebuild keeps later holes and mounts, and undo brings the old part back
        const std::size_t trisBefore = s.workspace().getAs<FitDoc>("fit1")->state.parts.at("plate1").mesh->triangleCount();
        Result rb = s.run("fit.rebuild", with({{"part", std::string("plate1")}, {"gap", 1.0}}));
        CHECK(rb.ok);
        auto doc = s.workspace().getAs<FitDoc>("fit1");
        CHECK(near(doc->state.parts.at("plate1").gap, 1.0, 1e-9) && doc->state.parts.size() == 7);
        Result ck2 = s.run("fit.check", with({{"part", std::string("plate1")}}));
        CHECK(ck2.ok && numField(ck2, "min_gap") > numField(ck, "min_gap") + 0.3);
        CHECK(s.history().undo());
        CHECK(near(doc->state.parts.at("plate1").gap, 0.4, 1e-9) && doc->state.parts.at("plate1").mesh->triangleCount() == trisBefore);

        // exports
        const fs::path stl = dir / "plate.stl";
        Result ex = s.run("fit.export", with({{"part", std::string("plate1")}, {"path", stl.string()}, {"shrink", std::string("abs")}}));
        CHECK(ex.ok && fs::exists(stl) && isClosed(mesh::load(stl)));
        const fs::path test = dir / "plate_test.stl";
        Result ts = s.run("fit.export", with({{"part", std::string("plate1")}, {"path", test.string()}, {"test_slice", 1.2}}));
        CHECK(ts.ok && mesh::bounds(mesh::load(test)).size().z < mesh::bounds(mesh::load(stl)).size().z);
        Result tp = s.run("fit.export", with({{"part", std::string("phone1")}, {"path", (dir / "phone_test.3mf").string()}, {"test_slice", 1.0}}));
        CHECK(tp.ok);
        Result all = s.run("fit.export", with({{"path", (dir / "all.stl").string()}, {"separate", true}}));
        CHECK(all.ok && numField(all, "files") == 7);
        CHECK(s.run("fit.export", with({{"path", (dir / "scene.png").string()}, {"size", std::int64_t{128}}})).ok);
        CHECK(!s.run("fit.export", with({{"path", (dir / "x.docx").string()}})).ok);
        Result pv = s.run("fit.preview", with({{"heat", std::string("plate1")}, {"size", std::int64_t{96}}}));
        CHECK(pv.ok && raster::decode(pv.attachments[0].bytes).width() == 96);

        // projects replay to the same parts
        const fs::path proj = dir / "dash.wwfit";
        CHECK(s.run("fit.save_project", with({{"path", proj.string()}})).ok);
        Result re = s.run("fit.open", {{"path", proj.string()}});
        CHECK(re.ok);
        auto doc2 = s.workspace().getAs<FitDoc>(strField(re, "doc"));
        CHECK(doc2 && doc2->state.parts.size() == doc->state.parts.size() && doc2->state.refs.size() == doc->state.refs.size());
        CHECK(doc2->state.parts.at("plate1").mesh->triangleCount() == doc->state.parts.at("plate1").mesh->triangleCount());

        // transforms move everything together
        const Vec3 before = doc->state.refs.at("plane1").point;
        CHECK(s.run("fit.move", with({{"by", NumberList{10, 0, 0}}})).ok);
        CHECK(near(doc->state.refs.at("plane1").point.x, before.x + 10, 1e-6));
        CHECK(s.run("fit.mirror", with({{"axis", std::string("x")}})).ok && near(doc->state.refs.at("plane1").point.x, -(before.x + 10), 1e-6));
        CHECK(isClosed(*doc->state.parts.at("plate1").mesh) && mesh::volume(*doc->state.parts.at("plate1").mesh) > 0);
        CHECK(s.run("fit.align", with({{"mode", std::string("plane")}, {"plane", std::string("plane1")}, {"face", std::string("front")}})).ok);
        CHECK(near(doc->state.refs.at("plane1").dir.y, -1, 1e-6) && mesh::length(doc->state.refs.at("plane1").point) < 1e-6);
        CHECK(s.run("fit.rotate", with({{"degrees", NumberList{0, 0, 90}}})).ok);
        CHECK(s.run("fit.align", d).ok);
        CHECK(s.run("fit.delete", with({{"name", std::string("block1")}})).ok && !doc->state.parts.count("block1"));
        CHECK(!s.run("fit.delete", with({{"name", std::string("nothing")}})).ok);
        Result cb = s.run("fit.combine", with({{"parts", std::string("plate1,strip1")}}));
        CHECK(cb.ok && doc->state.parts.count("combined1") && !doc->state.parts.count("plate1"));

        // a pipe for clamps
        const fs::path pipeFile = dir / "bar.stl";
        mesh::save(pipeFile, pipeScan(12));
        Result po = s.run("fit.open", {{"path", pipeFile.string()}});
        CHECK(po.ok && numField(po, "scale") == 1);
        Params pd{{"doc", strField(po, "doc")}};
        Params cy = pd;
        cy["at"] = NumberList{0, 0, 52};
        cy["radius"] = 25.0;
        Result cr = s.run("fit.cylinder", cy);
        CHECK(cr.ok && near(numField(cr, "radius"), 12, 0.4));
        Params cl = pd;
        cl["cylinder"] = std::string("cylinder1");
        Result clamp = s.run("fit.clamp", cl);
        CHECK(clamp.ok && std::get<bool>(*clamp.field("watertight")));
        Params mm = cl;
        mm["mount"] = std::string("camera");
        Result mir = s.run("fit.mirror_mount", mm);
        CHECK(mir.ok && strField(mir, "part") == "mirror1a");
        Result cup = s.run("fit.cupholder", cl);
        CHECK(cup.ok);
        Params ck3 = pd;
        ck3["part"] = std::string("clamp1");
        Result cc = s.run("fit.check", ck3);
        CHECK(cc.ok && numField(cc, "inside") < 0.02);

        CHECK(!s.run("fit.open", {{"path", (dir / "missing.obj").string()}}).ok);
        CHECK(!s.run("fit.open", {{"path", (dir / "section.svg").string()}}).ok);
        CHECK(!s.run("fit.plate", {{"doc", std::string("nope")}, {"at", NumberList{0, 0, 0}}}).ok);
        CHECK(s.run("fit.close", pd).ok && !s.run("fit.info", pd).ok);

        // point clouds open as a surface
        const fs::path xyz = dir / "cloud.xyz";
        {
            std::ofstream f(xyz);
            for (int j = 0; j < 60; ++j)
                for (int i = 0; i < 60; ++i) f << (i - 30) * 0.002 << " " << 0.01 * std::sin(i * 0.1) << " " << (j - 30) * 0.002 << "\n";
        }
        Result xo = s.run("fit.open", {{"path", xyz.string()}});
        CHECK(xo.ok && xo.message.find("points") != std::string::npos);
    }

    // ---- MCP ----
    {
        Session ms;
        fit::registerCommands(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "fit"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        bool sawPlate = false;
        for (const auto& t : list) {
            if (t["name"] == "fit_preview" || t["name"] == "fit_check" || t["name"] == "fit_pick") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (t["name"] == "fit_export" || t["name"] == "fit_plate") CHECK(t["annotations"]["readOnlyHint"] == false);
            if (t["name"] == "fit_plate") {
                sawPlate = true;
                CHECK(t["inputSchema"]["properties"]["shape"]["enum"].size() == 5);
            }
        }
        CHECK(sawPlate && list.size() >= 40);
        const fs::path stl = dir / "mcp_dash.stl";
        mesh::save(stl, surface(-60, 60, -40, 40, 3, dash, [](double, double) { return false; }, 0.1));
        json o = call(server, 2, "fit_open", {{"path", stl.string()}, {"preview", true}});
        CHECK(o["isError"] == false && o["structuredContent"]["doc"] == "fit1" && o["content"].size() == 2);
        json p = call(server, 3, "fit_phone_mount", {{"doc", "fit1"}, {"at", {0, 0, 45}}, {"preview", true}});
        CHECK(p["isError"] == false && p["content"].size() == 2);
        json bad = call(server, 4, "fit_clamp", {{"doc", "fit1"}, {"cylinder", "nope"}});
        CHECK(bad["isError"] == true);
        CHECK(call(server, 5, "undo", json::object())["isError"] == false);
    }

    fs::remove_all(dir);
    return check::summary("fit");
}
