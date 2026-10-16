#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/maps/maps.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::maps;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^ rd();
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_maps_" + std::to_string(stamp));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

void writeText(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

double num(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return -999999;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return -999998;
}

std::string str(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return {};
    return toText(*v);
}

Map& mapOf(Session& s, const std::string& id) { return s.workspace().getAs<MapDoc>(id)->map; }

void testGeodesy() {
    // Vincenty's classic test line: Flinders Peak -> Buninyong
    const Inverse inv = geodesicInverse(144.424867889, -37.951033416, 143.926495528, -37.652821139);
    CHECK(near(inv.distance, 54972.271, 0.01));
    CHECK(near(inv.azimuth1, 306.868158, 1e-5));
    const Coord back = geodesicDirect(144.424867889, -37.951033416, inv.azimuth1, inv.distance);
    CHECK(near(back.x, 143.926495528, 1e-8));
    CHECK(near(back.y, -37.652821139, 1e-8));
    CHECK(near(geodesicDistance(0, 0, 1, 0), 111319.49, 0.05));
    // nearly antipodal falls back without hanging
    CHECK(geodesicDistance(0, 0, 179.7, 0.5) > 19.9e6);
    // a 1x1 degree cell at the equator is about 12,300 km^2
    const double a = ringArea({{0, 0}, {1, 0}, {1, 1}, {0, 1}});
    CHECK(near(a / 1e6, 12308.8, 25));
    // orientation doesn't matter, holes subtract
    CHECK(near(ringArea({{0, 1}, {1, 1}, {1, 0}, {0, 0}}), a, 1));
    Polygon donut{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {{0.25, 0.25}, {0.25, 0.75}, {0.75, 0.75}, {0.75, 0.25}}};
    CHECK(near(polygonArea(donut), a * 0.75, a * 0.01));
}

void testProjections() {
    const Crs wm = Crs::webMercator();
    CHECK(near(wm.forward({180, 0}).x, 20037508.34, 0.01));
    CHECK(near(wm.forward({0, 85.0511287798}).y, 20037508.34, 1));
    CHECK(near(wm.inverse(wm.forward({12.5, 41.9})).y, 41.9, 1e-9));
    // 3 degrees east of the central meridian on the equator (zone 31): E = 833978.556
    const Crs z31 = Crs::utm(31, false);
    const Coord e = z31.forward({6, 0});
    CHECK(near(e.x, 833978.556, 0.01));
    CHECK(near(e.y, 0, 0.001));
    const Crs z33 = Crs::utmFor(13.4, 52.52);
    CHECK(z33.zone == 33 && !z33.south);
    const Coord b = z33.forward({13.4, 52.52});
    CHECK(b.x > 391000 && b.x < 392000 && b.y > 5819000 && b.y < 5821000);
    const Coord ll = z33.inverse(b);
    CHECK(near(ll.x, 13.4, 1e-9) && near(ll.y, 52.52, 1e-9));
    CHECK(Crs::utmFor(151.2, -33.9).south && Crs::utmFor(151.2, -33.9).zone == 56);
    CHECK(Crs::utmFor(5, 60).zone == 32);  // Norway exception
    CHECK(Crs::parse("EPSG:32633")->zone == 33);
    CHECK(Crs::parse("utm 33s")->south);
    CHECK(Crs::parse("epsg:3857")->kind == Crs::WebMercator);
    CHECK(Crs::parse("WGS84")->geographic());
    CHECK(!Crs::parse("epsg:2154"));
    CHECK(!Crs::parse("banana"));
    // .prj WKT round trip
    CHECK(Crs::parse(Crs::utm(17, true).wkt())->epsg() == 32717);
    CHECK(Crs::parse(wm.wkt())->kind == Crs::WebMercator);
    CHECK(Crs::parse(Crs::wgs84().wkt())->geographic());
}

void testExpressions() {
    Feature f;
    f.geom = Geometry::point(2.35, 48.85);
    f.props["name"] = Prop::string("Paris");
    f.props["pop"] = Prop::number(2.1e6);
    f.props["capital"] = Prop::boolean(true);
    f.props["code"] = Prop::infer("075");
    CHECK(f.props["code"].kind == Prop::String);
    CHECK(Prop::infer("12.5").kind == Prop::Number && Prop::infer("1e3").num == 1000);
    CHECK(Expr::compile("pop > 1e6 and name == 'Paris'").test(f));
    CHECK(!Expr::compile("pop > 1e6 and not capital").test(f));
    CHECK(Expr::compile("name in ('Lyon', 'Paris')").test(f));
    CHECK(Expr::compile("name not in ('Lyon')").test(f));
    CHECK(Expr::compile("name like 'par%'").test(f));
    CHECK(Expr::compile("name contains 'ari' or missing > 3").test(f));
    CHECK(Expr::compile("missing is null").test(f));
    CHECK(Expr::compile("round(pop / 1e6, 1) == 2.1").test(f));
    CHECK(Expr::compile("$x > 2 and $y < 49 and $type == 'point'").test(f));
    CHECK(Expr::compile("upper(name) + '!'").eval(f).str == "PARIS!");
    CHECK(Expr::compile("code == '075'").test(f));
    const Expr e = Expr::compile("[my field] >= 3 or other = 1");
    CHECK(e.fields().size() == 2);
    bool threw = false;
    try {
        Expr::compile("pop > ");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void testGeoJsonAndWkt(const fs::path& dir) {
    Geometry g = parseWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))");
    CHECK(g.type == GeomType::Polygon && g.polygons[0].size() == 2 && g.polygons[0][0].size() == 4);
    CHECK(parseWkt("MULTIPOINT ((1 2), (3 4))").points.size() == 2);
    CHECK(parseWkt("MULTIPOINT (1 2, 3 4)").points.size() == 2);
    CHECK(near(parseWkt("POINT Z (1 2 3)").points[0].z, 3, 0));
    CHECK(parseWkt("SRID=4326;LINESTRING (0 0, 1 1)").lines.size() == 1);
    CHECK(parseWkt(toWkt(g)).polygons[0][1].size() == 4);
    Feature f{g, {{"name", Prop::string("box")}, {"n", Prop::number(3)}}};
    const std::string json = toGeoJson({f});
    auto back = parseGeoJson(json);
    CHECK(back.size() == 1 && back[0].props["name"].str == "box" && back[0].props["n"].num == 3);
    CHECK(back[0].geom.polygons[0].size() == 2);
    // projected output says its crs and reads back to lon/lat
    ExportOptions o;
    o.crs = Crs::webMercator();
    auto merc = parseGeoJson(toGeoJson({Feature{Geometry::point(10, 50), {}}}, o));
    CHECK(near(merc[0].geom.points[0].x, 10, 1e-6) && near(merc[0].geom.points[0].y, 50, 1e-6));
    // GeometryCollection splits, CRLF is fine
    auto gc = parseGeoJson("{\r\n\"type\":\"Feature\",\"properties\":{\"a\":1},\r\n\"geometry\":{\"type\":\"GeometryCollection\",\"geometries\":[{\"type\":\"Point\",\"coordinates\":[1,2]},{\"type\":\"LineString\",\"coordinates\":[[0,0],[1,1]]}]}}\r\n");
    CHECK(gc.size() == 2 && gc[1].geom.type == GeomType::Line);
    // track times survive GeoJSON
    Line track{{0, 0, 10, 1000}, {0.01, 0, 12, 1100}};
    auto tj = parseGeoJson(toGeoJson({Feature{Geometry::line(track), {}}}));
    CHECK(near(tj[0].geom.lines[0][1].t, 1100, 1e-6) && near(tj[0].geom.lines[0][1].z, 12, 1e-9));
    (void)dir;
}

void testKmlGpxCsv(const fs::path& dir) {
    const std::string kml =
        "<?xml version=\"1.0\"?>\r\n<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Folder><name>Trip</name>\r\n"
        "<Placemark><name>Cafe &amp; Bar</name><ExtendedData><Data name=\"stars\"><value>4</value></Data></ExtendedData>"
        "<Point><coordinates>2.35,48.85,35</coordinates></Point></Placemark>\r\n"
        "<Placemark><name>Park</name><Polygon><outerBoundaryIs><LinearRing><coordinates>0,0 1,0 1,1 0,1 0,0</coordinates></LinearRing></outerBoundaryIs>"
        "<innerBoundaryIs><LinearRing><coordinates>0.2,0.2 0.4,0.2 0.4,0.4 0.2,0.2</coordinates></LinearRing></innerBoundaryIs></Polygon></Placemark>"
        "<Placemark><MultiGeometry><LineString><coordinates>0,0 1,1</coordinates></LineString><LineString><coordinates>2,2 3,3</coordinates></LineString></MultiGeometry></Placemark>"
        "</Folder></Document></kml>";
    auto k = parseKml(kml);
    CHECK(k.size() == 3);
    CHECK(k[0].props["name"].str == "Cafe & Bar" && k[0].props["stars"].num == 4 && k[0].props["folder"].str == "Trip");
    CHECK(near(k[0].geom.points[0].z, 35, 0));
    CHECK(k[1].geom.polygons[0].size() == 2);
    CHECK(k[2].geom.lines.size() == 2);
    Layer kl;
    kl.name = "k";
    kl.features = k;
    auto k2 = parseKml(toKml(kl));
    CHECK(k2.size() == 3 && k2[1].geom.polygons[0].size() == 2);

    // a GPX track along the equator: 0.001 deg ~ 111.3 m every 10 s, climbing 1 m then 5 m steps
    std::string gpx = "<?xml version=\"1.0\"?>\n<gpx version=\"1.1\"><wpt lat=\"0.0005\" lon=\"0.002\"><ele>12</ele><name>Spring</name></wpt><trk><name>Run</name><trkseg>\n";
    const double eles[] = {10, 11, 10, 15, 20, 25, 20, 15, 16, 10, 10};
    for (int i = 0; i <= 10; ++i) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "<trkpt lat=\"0\" lon=\"%.4f\"><ele>%.1f</ele><time>2024-05-01T08:00:%02dZ</time></trkpt>\r\n", i * 0.001, eles[i], i * 5);
        gpx += buf;
    }
    gpx += "</trkseg></trk></gpx>\n";
    GpxData d = parseGpx(gpx);
    CHECK(d.tracks.size() == 1 && d.waypoints.size() == 1 && d.waypoints[0].props["name"].str == "Spring");
    std::vector<const Line*> segs{&d.tracks[0].geom.lines[0]};
    TrackStats s = trackStats(segs, 3);
    CHECK(near(s.distance, 1113.19, 0.5));
    CHECK(near(s.gain, 15, 1e-9));   // 10 -> 15 -> 20 -> 25 (the 1 m wiggle is ignored)
    CHECK(near(s.loss, 15, 1e-9));   // 25 -> 20 -> 15 -> 10
    CHECK(near(s.duration, 50, 1e-9) && near(s.avgSpeed, 1113.19 / 50, 0.05));
    CHECK(near(s.maxSpeed, 111.319 / 5, 0.05));
    CHECK(near(s.movingTime, 50, 1e-9));
    Layer tl;
    tl.name = "t";
    tl.features = d.tracks;
    tl.features.push_back(d.waypoints[0]);
    GpxData d2 = parseGpx(toGpx(tl));
    CHECK(d2.tracks.size() == 1 && d2.waypoints.size() == 1 && d2.tracks[0].geom.lines[0].size() == 11);
    CHECK(near(d2.tracks[0].geom.lines[0][3].t, d.tracks[0].geom.lines[0][3].t, 1e-6));

    // CSV: header detection, DMS, semicolons, CRLF
    ImportOptions io;
    auto rows = parseCsv("name;Latitude;Longitude;zip\r\nA;48.85;2.35;01234\r\nB;51\xC2\xB0" "30'0\"N;0\xC2\xB0" "7'30\"W;99\r\nC;;;\r\n", io);
    CHECK(rows.size() == 2);
    CHECK(rows[0].props["zip"].str == "01234");
    CHECK(near(rows[1].geom.points[0].y, 51.5, 1e-9) && near(rows[1].geom.points[0].x, -0.125, 1e-9));
    io.crs = Crs::utm(33, false);
    auto utmRows = parseCsv("id,x,y\n1,391440.032,5820079.667\n", io);
    CHECK(utmRows.size() == 1 && near(utmRows[0].geom.points[0].x, 13.4, 1e-6));
    ImportOptions wk;
    auto wktRows = parseCsv("id,wkt\n1,\"LINESTRING (0 0, 1 1)\"\n", wk);
    CHECK(wktRows.size() == 1 && wktRows[0].geom.type == GeomType::Line);
    Layer cl;
    cl.features = rows;
    const std::string csv = toCsv(cl);
    CHECK(csv.find("lat,lon") != std::string::npos && csv.find("01234") != std::string::npos);
    (void)dir;
}

void testShapefile(const fs::path& dir) {
    Layer l;
    l.name = "parts";
    Feature a{Geometry::polygon({{{0, 0}, {2, 0}, {2, 2}, {0, 2}}, {{0.5, 0.5}, {0.5, 1}, {1, 1}, {1, 0.5}}}), {}};
    a.props["name"] = Prop::string("Zoë's field");
    a.props["area"] = Prop::number(12.25);
    a.props["ok"] = Prop::boolean(true);
    Feature b{Geometry::polygon({{{5, 5}, {6, 5}, {6, 6}}}), {}};
    b.props["name"] = Prop::string("tri");
    b.props["area"] = Prop::number(3);
    Feature c{Geometry::point(1, 1), {{"name", Prop::string("pt")}}};
    Feature d{Geometry::line({{0, 0, 5}, {1, 1, 7}}), {{"name", Prop::string("road")}}};
    l.features = {a, b, c, d};
    ExportOptions o;
    o.crs = Crs::utm(31, false);
    const auto files = writeShapefile(l, dir / "parts.shp", o);
    CHECK(files.size() == 15);  // three geometry types x shp/shx/dbf/prj/cpg
    CHECK(fs::exists(dir / "parts_polygons.shp") && fs::exists(dir / "parts_lines.dbf"));
    std::vector<std::string> warnings;
    Layer back = readShapefile(dir / "parts_polygons.shp", std::nullopt, &warnings);
    CHECK(back.features.size() == 2);
    CHECK(back.features[0].props["name"].str == "Zoë's field");
    CHECK(near(back.features[0].props["area"].num, 12.25, 1e-9) && back.features[0].props["ok"].truthy());
    CHECK(back.features[0].geom.polygons.size() == 1 && back.features[0].geom.polygons[0].size() == 2);
    CHECK(near(back.features[0].geom.polygons[0][0][1].x, 2, 1e-6));  // reprojected from UTM 31N via .prj
    CHECK(signedArea(back.features[0].geom.polygons[0][0]) > 0);       // outer rings come back counter-clockwise
    Layer lines = readShapefile(dir / "parts_lines.shp", std::nullopt);
    CHECK(lines.features.size() == 1 && near(lines.features[0].geom.lines[0][1].z, 7, 1e-9));
    Layer pts = readShapefile(dir / "parts_points.shp", std::nullopt);
    CHECK(pts.features.size() == 1 && near(pts.features[0].geom.points[0].y, 1, 1e-7));
    // .shx index is consistent: 100-byte header, one 8-byte record per shape
    CHECK(fs::file_size(dir / "parts_polygons.shx") == 100 + 2 * 8);
}

void testRasters(const fs::path& dir) {
    // value grid -> GeoTIFF -> back
    Grid g;
    g.width = 4;
    g.height = 3;
    g.values = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -9999};
    g.nodata = -9999;
    g.crs = Crs::utm(33, false);
    g.geo = {30, 0, 400000, 0, -30, 5800000};
    g.updateRange();
    CHECK(g.maxValue == 11);
    auto t = readGeoTiff(encodeGeoTiff(g));
    CHECK(t->width == 4 && t->height == 3 && t->values.size() == 12 && t->values[5] == 6);
    CHECK(t->crs.zone == 33 && near(t->geo.c, 400000, 1e-6) && near(t->geo.e, -30, 1e-9));
    CHECK(t->nodata == -9999);
    const Coord centre = g.crs.inverse({400000 + 45, 5800000 - 45});  // pixel (1,1)
    CHECK(t->sample(centre.x, centre.y) == 6);
    // RGBA image GeoTIFF
    Grid img;
    img.width = 2;
    img.height = 2;
    img.image = raster::Image(2, 2, {200, 10, 10, 255});
    img.geo = {0.5, 0, 10, 0, -0.5, 50};
    auto ti = readGeoTiff(encodeGeoTiff(img));
    CHECK(!ti->image.empty() && ti->image.at(1, 1).r == 200 && ti->crs.geographic());
    // PNG + world file
    raster::save(raster::Image(10, 10, {0, 0, 255, 255}), dir / "blue.png");
    writeText(dir / "blue.pgw", "0.1\r\n0\r\n0\r\n-0.1\r\n5.05\r\n45.95\r\n");
    auto w = readRaster(dir / "blue.png", std::nullopt, std::nullopt);
    const BBox b = w->bounds();
    CHECK(near(b.minX, 5, 1e-9) && near(b.maxY, 46, 1e-9) && near(b.maxX, 6, 1e-9));
    CHECK(near(w->sample(5.5, 45.5), 0.0722 * 255, 0.01));
    bool threw = false;
    try {
        raster::save(raster::Image(4, 4), dir / "nowhere.png");
        readRaster(dir / "nowhere.png", std::nullopt, std::nullopt);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    // write a picture with world file and read it back in place
    const auto files = writeRaster(g, dir / "grid.png", "viridis");
    CHECK(files.size() == 3 && fs::exists(dir / "grid.pgw") && fs::exists(dir / "grid.prj"));
    auto gp = readRaster(dir / "grid.png", std::nullopt, std::nullopt);
    CHECK(gp->crs.zone == 33 && near(gp->geo.c, 400000, 1e-6));
}

#include "tiff_fixtures.inc"

void testTiffCodecs() {
    auto rgb = readGeoTiff(kLzwRgb);
    CHECK(rgb->width == 16 && rgb->height == 8 && !rgb->image.empty());
    CHECK(rgb->image.at(5, 3).r == 80 && rgb->image.at(5, 3).g == 96 && rgb->image.at(5, 3).b == 100);
    CHECK(rgb->image.at(15, 7).r == 240 && rgb->image.at(15, 7).g == 224);
    bool placed = true;
    auto gray = readGeoTiff(kDeflateGray16, nullptr, &placed);
    CHECK(!placed && gray->hasValues() && gray->width == 16);
    CHECK(gray->values[3 * 16 + 7] == 703.0f && gray->maxValue == 1507);
}

void testAnalysis() {
    // a point buffer is a geodesic circle
    Geometry circle = buffer(Geometry::point(10, 50), 1000);
    CHECK(near(area(circle), 3.14159265 * 1e6, 0.01 * 3.14159265e6));
    // a 10 km line buffered by 500 m: 2*d*L + pi*d^2
    const Coord end = geodesicDirect(10, 50, 90, 10000);
    Geometry road = Geometry::line({{10, 50}, end});
    Geometry rb = buffer(road, 500);
    CHECK(near(area(rb), 2 * 500 * 10000 + 3.14159265 * 250000, 0.02 * 1.08e7));
    CHECK(pointInGeometry(rb, 10.05, 50.002) && !pointInGeometry(rb, 10.05, 50.02));
    // a polygon grows and shrinks
    Geometry sq = Geometry::polygon({{{10, 50}, {10.1, 50}, {10.1, 50.1}, {10, 50.1}}});
    const double a0 = area(sq);
    CHECK(area(buffer(sq, 100)) > a0);
    const double shrunk = area(buffer(sq, -500));
    CHECK(shrunk < a0 && shrunk > a0 * 0.6);
    // two overlapping circles dissolve into one polygon
    Geometry p1 = Geometry::point(0, 0), p2 = Geometry::point(0.01, 0);
    Geometry u = bufferUnion({&p1, &p2}, 1000);
    CHECK(u.polygons.size() == 1);
    // a ring buffer keeps its hole
    Geometry ring = Geometry::line({{0, 0}, {0.1, 0}, {0.1, 0.1}, {0, 0.1}, {0, 0}});
    Geometry rbuf = buffer(ring, 200);
    CHECK(rbuf.polygons.size() == 1 && rbuf.polygons[0].size() == 2);

    // simplify a noisy line
    Line noisy;
    for (int i = 0; i <= 200; ++i) noisy.push_back({i * 0.0001, (i % 2) * 0.000001});
    Geometry s = simplify(Geometry::line(noisy), 5);
    CHECK(s.lines[0].size() == 2);
    // clip
    Geometry c = clip(Geometry::line({{-1, 0.5}, {2, 0.5}}), BBox{0, 0, 1, 1});
    CHECK(c.lines.size() == 1 && near(c.lines[0][0].x, 0, 1e-12) && near(c.lines[0][1].x, 1, 1e-12));
    Geometry cp = clip(Geometry::polygon({{{-1, -1}, {2, -1}, {2, 2}, {-1, 2}}}), BBox{0, 0, 1, 1});
    CHECK(near(std::abs(signedArea(cp.polygons[0][0])), 1, 1e-12));
    CHECK(clip(Geometry::point(5, 5), BBox{0, 0, 1, 1}).empty());
    // nearest
    const Nearest n = nearestPoint(Geometry::line({{0, 0}, {0, 1}}), 0.01, 0.5);
    CHECK(near(n.distance, 1113.2, 1) && near(n.point.y, 0.5, 1e-6));
    CHECK(nearestPoint(sq, 10.05, 50.05).distance == 0);
    // heatmap peak sits on the cluster
    std::vector<Feature> pts;
    for (int i = 0; i < 20; ++i) pts.push_back({Geometry::point(10 + i * 0.0001, 50), {}});
    pts.push_back({Geometry::point(10.5, 50.3), {}});
    HeatmapOptions ho;
    ho.radius = 2000;
    auto hm = heatmap(pts, ho);
    CHECK(hm->sample(10.001, 50) > hm->sample(10.5, 50.3) * 5);
    CHECK(hm->maxValue > 10);
}

void testRendering(const fs::path& dir) {
    Map m;
    m.title = "Test";
    Layer pts;
    pts.name = "points";
    pts.style.fill = {255, 0, 0, 255};
    pts.style.pointSize = 20;
    pts.style.labelField = "name";
    pts.features.push_back({Geometry::point(2.35, 48.85), {{"name", Prop::string("Paris")}, {"kind", Prop::string("a")}}});
    pts.features.push_back({Geometry::point(13.4, 52.52), {{"name", Prop::string("Berlin")}, {"kind", Prop::string("b")}}});
    m.layers.push_back(pts);
    RenderOptions o;
    o.width = 400;
    o.basemap = "none";
    o.legend = false;
    RenderInfo info;
    raster::Image img = renderImage(m, o, &info);
    CHECK(img.width() == 400 && img.height() > 50);
    int red = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const Color c = img.at(x, y);
            if (c.r > 220 && c.g < 40 && c.b < 40) ++red;
        }
    }
    CHECK(red > 200);  // two 20 px markers
    CHECK(info.metresPerPixel > 100);
    o.basemap = "countries";
    o.legend = true;
    o.graticule = true;
    const std::string svg = renderMap(m, o, "svg");
    CHECK(svg.find("<svg") == 39 || svg.find("<svg") != std::string::npos);
    CHECK(svg.find(">Paris</text>") != std::string::npos);
    const std::string pdf = renderMap(m, o, "pdf");
    CHECK(pdf.rfind("%PDF-", 0) == 0 && pdf.find("%%EOF") != std::string::npos);
    // categorized classes
    m.layers[0].style.mode = "categorized";
    m.layers[0].style.field = "kind";
    CHECK(styleClasses(m.layers[0]).size() == 2);
    // graduated quantiles
    Layer g;
    for (int i = 1; i <= 100; ++i) g.features.push_back({Geometry::point(i * 0.1, 0), {{"v", Prop::number(i)}}});
    g.style.mode = "graduated";
    g.style.field = "v";
    g.style.classes = 4;
    auto cls = styleClasses(g);
    CHECK(cls.size() == 4 && near(cls[0].hi, 25, 1e-9) && near(cls[3].hi, 100, 1e-9));
    g.style.method = "equal";
    CHECK(near(styleClasses(g)[1].hi, 50.5, 1e-9));
    // tile maths
    double tx = 0, ty = 0;
    lonLatToTile(0, 0, 1, tx, ty);
    CHECK(near(tx, 1, 1e-12) && near(ty, 1, 1e-12));
    double lon = 0, lat = 0;
    tileToLonLat(0, 0, 0, lon, lat);
    CHECK(near(lon, -180, 1e-9) && near(lat, 85.0511287798, 1e-6));
    // offline OSM basemap from a pre-filled cache (never touches the network)
    o.basemap = "osm";
    o.offline = true;
    o.tileCache = (dir / "tiles").string();
    o.legend = false;
    RenderInfo first;
    renderImage(m, o, &first);
    CHECK(first.zoom >= 0 && first.tilesUsed == 0);
    const fs::path server = dir / "tiles" / "https___tile_openstreetmap_org_";
    const raster::Image green(256, 256, {0, 200, 0, 255});
    const int z = first.zoom;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    lonLatToTile(first.bbox.minX, first.bbox.maxY, z, x0, y0);
    lonLatToTile(first.bbox.maxX, first.bbox.minY, z, x1, y1);
    for (int x = static_cast<int>(x0); x <= static_cast<int>(x1); ++x) {
        for (int y = static_cast<int>(y0); y <= static_cast<int>(y1); ++y) {
            fs::create_directories(server / std::to_string(z) / std::to_string(x));
            raster::save(green, server / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png"));
        }
    }
    RenderInfo second;
    raster::Image withTiles = renderImage(m, o, &second);
    CHECK(second.tilesUsed > 0 && second.tilesFetched == 0);
    CHECK(withTiles.at(withTiles.width() / 3, 5).g > 150);
}

void testCommands(const fs::path& dir) {
    Session s;
    registerCommands(s);
    writeText(dir / "cafes.csv", "name,lat,lon,rating,country\nA,48.857,2.352,4.5,FRA\nB,48.86,2.34,3,FRA\nC,52.52,13.40,5,DEU\n");
    writeText(dir / "stats.csv", "code,gdp\nFRA,2.9\nDEU,4.2\nXXX,1\n");
    Result r = run(s, "maps.new title=Cafes");
    CHECK(r.ok);
    const std::string id = toText(r.value);
    CHECK(run(s, "maps.import doc=" + id + " path=" + q(dir / "cafes.csv")).ok);
    CHECK(mapOf(s, id).layers.size() == 1 && mapOf(s, id).layers[0].features.size() == 3);
    r = run(s, "maps.filter doc=" + id + " where=\"rating >= 4\"");
    CHECK(r.ok && num(r, "count") == 2);
    CHECK(run(s, "undo").ok);
    CHECK(mapOf(s, id).layers.size() == 1);
    r = run(s, "maps.add_countries doc=" + id + " where=\"continent == 'Europe'\"");
    CHECK(r.ok && num(r, "features") > 30);
    r = run(s, "maps.join doc=" + id + " layer=countries csv=" + q(dir / "stats.csv") + " key=iso_a3 csv_key=code");
    CHECK(r.ok && num(r, "matched") == 2 && num(r, "unmatched_rows") == 1);
    r = run(s, "maps.contains doc=" + id + " layer=countries lat=48.86 lon=2.35");
    CHECK(r.ok && num(r, "count") == 1 && r.message.find("France") != std::string::npos);
    r = run(s, "maps.spatial_join doc=" + id + " target=cafes join=countries fields=name prefix=in_ count_field=cafes");
    CHECK(r.ok && num(r, "matched") == 3);
    CHECK(mapOf(s, id).layer("cafes")->features[2].props["in_name"].str == "Germany");
    r = run(s, "maps.nearest doc=" + id + " layer=cafes lat=48.858 lon=2.351 count=2");
    CHECK(r.ok && num(r, "index") == 0 && num(r, "distance_m") < 200);
    r = run(s, "maps.calc doc=" + id + " layer=countries field=km2 expr=\"round($area / 1e6)\"");
    CHECK(r.ok && num(r, "changed") > 30);
    r = run(s, "maps.style doc=" + id + " layer=countries mode=graduated field=km2 ramp=blues");
    CHECK(r.ok && num(r, "classes") >= 3);
    CHECK(!run(s, "maps.style doc=" + id + " layer=countries mode=categorized field=nope").ok);
    r = run(s, "maps.buffer doc=" + id + " layer=cafes distance=500 output=walk");
    CHECK(r.ok && num(r, "features") == 3);
    r = run(s, "maps.heatmap doc=" + id + " layer=cafes radius=3000");
    CHECK(r.ok && num(r, "max") > 0);
    r = run(s, "maps.raster_save doc=" + id + " layer=\"cafes heatmap\" path=" + q(dir / "heat.tif"));
    CHECK(r.ok && fs::exists(dir / "heat.tif"));
    r = run(s, "maps.export doc=" + id + " layer=cafes path=" + q(dir / "out" / "cafes.shp") + " crs=webmercator");
    CHECK(r.ok && fs::exists(dir / "out" / "cafes.shp") && readAll(dir / "out" / "cafes.prj").find("Auxiliary_Sphere") != std::string::npos);
    r = run(s, "maps.export doc=" + id + " layer=walk path=" + q(dir / "walk.geojson"));
    CHECK(r.ok && readAll(dir / "walk.geojson").find("\"Polygon\"") != std::string::npos);
    r = run(s, "maps.export doc=" + id + " layer=cafes path=" + q(dir / "cafes.kml"));
    CHECK(r.ok);
    r = run(s, "maps.export doc=" + id + " layer=cafes path=" + q(dir / "cafes.gpx"));
    CHECK(r.ok);
    r = run(s, "maps.import doc=" + id + " path=" + q(dir / "out" / "cafes.shp") + " name=back");
    CHECK(r.ok && num(r, "features") == 3);
    CHECK(near(mapOf(s, id).layer("back")->features[0].geom.points[0].y, 48.857, 1e-6));
    r = run(s, "maps.render doc=" + id + " path=" + q(dir / "map.png") + " width=500");
    CHECK(r.ok && raster::load(dir / "map.png").width() == 500);
    CHECK(run(s, "maps.render doc=" + id + " path=" + q(dir / "map.pdf") + " center=48.86,2.35 radius_km=5 crs=auto").ok);
    CHECK(run(s, "maps.render doc=" + id + " path=" + q(dir / "map.svg") + " crs=wgs84 graticule=true").ok);
    r = run(s, "maps.preview doc=" + id);
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    r = run(s, "maps.save doc=" + id + " path=" + q(dir / "cafes.achmap"));
    CHECK(r.ok);
    r = run(s, "maps.open path=" + q(dir / "cafes.achmap"));
    CHECK(r.ok);
    const std::string id2 = toText(r.value);
    CHECK(mapOf(s, id2).layers.size() == mapOf(s, id).layers.size());
    CHECK(mapOf(s, id2).layer("cafes heatmap")->grid != nullptr);
    CHECK(mapOf(s, id2).layer("countries")->style.mode == "graduated");
    r = run(s, "maps.measure points=48.85,2.35,48.85,2.36,48.86,2.36 polygon=true");
    CHECK(r.ok && num(r, "area_m2") > 1e5);
    r = run(s, "maps.transform points=13.4,52.52 to=utm");
    CHECK(r.ok && str(r, "crs") == "EPSG:32633");
    r = run(s, "maps.bbox doc=" + id + " layer=cafes");
    CHECK(r.ok && near(num(r, "max_lat"), 52.52, 1e-9));
    CHECK(run(s, "maps.feature_add doc=" + id + " layer=route points=48.85,2.35,52.52,13.40 props=\"name=Train\"").ok);
    r = run(s, "maps.measure doc=" + id + " layer=route");
    CHECK(r.ok && near(num(r, "length_m"), 878e3, 5e3));
    CHECK(run(s, "maps.feature_delete doc=" + id + " layer=cafes where=\"name == 'B'\"").ok);
    CHECK(mapOf(s, id).layer("cafes")->features.size() == 2);
    CHECK(!run(s, "maps.filter doc=" + id + " where=\"rating >\"").ok);
    // GPX through the commands
    std::string gpx = "<gpx><trk><trkseg>";
    for (int i = 0; i < 5; ++i) gpx += "<trkpt lat=\"46\" lon=\"" + std::to_string(7 + i * 0.01) + "\"><ele>" + std::to_string(500 + i * 20) + "</ele><time>2024-01-01T10:0" + std::to_string(i) + ":00Z</time></trkpt>";
    gpx += "</trkseg></trk></gpx>";
    writeText(dir / "hike.gpx", gpx);
    r = run(s, "maps.open path=" + q(dir / "hike.gpx"));
    CHECK(r.ok);
    const std::string hike = toText(r.value);
    r = run(s, "maps.track_stats doc=" + hike);
    CHECK(r.ok && near(num(r, "gain_m"), 80, 1e-9) && near(num(r, "duration_s"), 240, 1e-9) && num(r, "avg_kmh") > 40);
    r = run(s, "maps.profile doc=" + hike + " path=" + q(dir / "profile.svg"));
    CHECK(r.ok && readAll(dir / "profile.svg").find("<svg") != std::string::npos && r.attachments.size() == 1);
    // raster commands
    raster::save(raster::Image(8, 8, {255, 255, 255, 255}), dir / "scan.png");
    r = run(s, "maps.raster_add doc=" + id + " path=" + q(dir / "scan.png") + " bbox=2,48,3,49");
    CHECK(r.ok);
    r = run(s, "maps.raster_sample doc=" + id + " layer=scan points=48.5,2.5,10,10");
    CHECK(r.ok && r.message.find("no data") != std::string::npos);
    CHECK(run(s, "maps.close doc=" + hike).ok);
    CHECK(!s.workspace().get(hike));
}

void testMcp(const fs::path& dir) {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}").value_or("");
    };
    const std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"maps_new", "maps_open", "maps_import", "maps_export", "maps_render", "maps_preview", "maps_buffer", "maps_filter",
                             "maps_join", "maps_heatmap", "maps_track_stats", "maps_measure", "maps_transform", "maps_style", "maps_nearest",
                             "maps_contains", "maps_simplify", "maps_clip", "maps_raster_add", "maps_add_countries", "maps_spatial_join"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string out = call("maps_new", R"({"title":"MCP"})");
    CHECK(out.find("map1") != std::string::npos);
    out = call("maps_feature_add", R"({"doc":"map1","layer":"pins","lat":40.4168,"lon":-3.7038,"props":"name=Madrid"})");
    CHECK(out.find("\"isError\":false") != std::string::npos || out.find("isError") == std::string::npos);
    out = call("maps_style", R"({"doc":"map1","layer":"pins","label":"name","preview":true})");
    CHECK(out.find("\"type\":\"image\"") != std::string::npos);
    out = call("maps_preview", R"({"doc":"map1","bbox":[-10,35,5,44]})");
    CHECK(out.find("image/png") != std::string::npos);
    const std::string path = (dir / "mcp.geojson").generic_string();
    out = call("maps_export", R"({"doc":"map1","layer":"pins","path":")" + path + R"("})");
    CHECK(fs::exists(dir / "mcp.geojson"));
    out = call("maps_measure", R"({"from":[40.4168,-3.7038],"to":[41.3874,2.1686]})");
    CHECK(out.find("distance_m") != std::string::npos);
}

}  // namespace

int main() {
    const fs::path dir = tempDir();
    testGeodesy();
    testProjections();
    testExpressions();
    testGeoJsonAndWkt(dir);
    testKmlGpxCsv(dir);
    testShapefile(dir);
    testRasters(dir);
    testTiffCodecs();
    testAnalysis();
    testRendering(dir);
    testCommands(dir);
    testMcp(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("maps");
}
