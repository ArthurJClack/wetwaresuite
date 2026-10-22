// Tests for the maps toolset added on top of the basics: feature and vertex
// editing, overlays, dissolve, select by location, track tools, FIT and OSM
// files, routing, search, bookmarks, the view and maps.hit.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/maps/maps.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::maps;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^ rd();
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_maps_tools_" + std::to_string(stamp));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

void writeText(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
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
    return v ? toText(*v) : std::string();
}

Map& mapOf(Session& s, const std::string& id) { return s.workspace().getAs<MapDoc>(id)->map; }

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::string newMap(Session& s) {
    Result r = run(s, "maps.new");
    return toText(r.value);
}

// A square of `side` metres with its south-west corner at lat,lon, as maps.feature_add points.
std::string square(double lat, double lon, double side) {
    const double dLat = side / 111320.0, dLon = side / (111320.0 * std::cos(lat * kDeg));
    char buf[256];
    std::snprintf(buf, sizeof buf, "%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f", lat, lon, lat, lon + dLon, lat + dLat, lon + dLon, lat + dLat, lon);
    return buf;
}

void testEditing() {
    Session s;
    registerCommands(s);
    const std::string m = newMap(s);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=paths points=48,2,48.01,2,48.02,2 props=\"name=Path\"").ok);
    Result r = run(s, "maps.feature_update doc=" + m + " layer=paths index=0 props=\"surface=gravel; width=3\" unset=name");
    CHECK(r.ok);
    const Feature& f0 = mapOf(s, m).layers[0].features[0];
    CHECK(f0.props.at("width").num == 3 && !f0.props.count("name"));
    r = run(s, "maps.vertices doc=" + m + " layer=paths index=0");
    CHECK(r.ok && num(r, "count") == 3);
    CHECK(run(s, "maps.vertex_move doc=" + m + " layer=paths index=0 vertex=1 to=48.01,2.001").ok);
    CHECK(near(mapOf(s, m).layers[0].features[0].geom.lines[0][1].x, 2.001, 1e-12));
    // a new corner on the nearest edge: between 1 and 2
    r = run(s, "maps.vertex_add doc=" + m + " layer=paths index=0 at=48.015,2.0007");
    CHECK(r.ok && num(r, "vertex") == 2);
    CHECK(mapOf(s, m).layers[0].features[0].geom.lines[0].size() == 4);
    CHECK(run(s, "maps.vertex_delete doc=" + m + " layer=paths index=0 vertex=2").ok);
    CHECK(mapOf(s, m).layers[0].features[0].geom.lines[0].size() == 3);
    CHECK(run(s, "maps.vertex_move doc=" + m + " layer=paths index=0 vertex=1 to=48.01,2").ok);
    // split at 1.5 km and merge back
    const double total = length(mapOf(s, m).layers[0].features[0].geom);
    r = run(s, "maps.split doc=" + m + " layer=paths index=0 distance=1500");
    CHECK(r.ok && near(num(r, "first_m"), 1500, 1) && near(num(r, "first_m") + num(r, "second_m"), total, 0.5));
    CHECK(mapOf(s, m).layers[0].features.size() == 2);
    CHECK(mapOf(s, m).layers[0].features[1].props.at("surface").str == "gravel");
    r = run(s, "maps.merge doc=" + m + " layer=paths indexes=1,0");
    CHECK(r.ok && num(r, "parts") == 1);
    CHECK(mapOf(s, m).layers[0].features.size() == 1 && near(length(mapOf(s, m).layers[0].features[0].geom), total, 0.5));
    CHECK(run(s, "undo").ok);
    CHECK(mapOf(s, m).layers[0].features.size() == 2);
    // a split at a point
    r = run(s, "maps.split doc=" + m + " layer=paths index=0 at=48.005,2.0001");
    CHECK(r.ok && mapOf(s, m).layers[0].features.size() == 3);
    // move a feature 1 km east
    const Coord before = centroid(mapOf(s, m).layers[0].features[0].geom);
    CHECK(run(s, "maps.feature_move doc=" + m + " layer=paths index=0 dx=1000").ok);
    const Coord after = centroid(mapOf(s, m).layers[0].features[0].geom);
    CHECK(near(geodesicDistance(before.x, before.y, after.x, after.y), 1000, 2));
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=pins lat=10 lon=10").ok);
    CHECK(run(s, "maps.feature_move doc=" + m + " layer=pins index=0 to=11,12").ok);
    CHECK(mapOf(s, m).layer("pins")->features[0].geom.points[0].x == 12);
    CHECK(!run(s, "maps.vertex_delete doc=" + m + " layer=pins index=0 vertex=0").ok);  // a point's only vertex
}

void testOverlay() {
    Session s;
    registerCommands(s);
    const std::string m = newMap(s);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=a kind=polygon points=" + square(45, 7, 1000) + " props=\"name=A\"").ok);
    // B is offset half a square east: they overlap by half
    const double dLon = 500 / (111320.0 * std::cos(45 * kDeg));
    char sw[64];
    std::snprintf(sw, sizeof sw, "%.7f", 7 + dLon);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=b kind=polygon points=" + square(45, 7 + dLon, 1000) + " props=\"name=B; zone=2\"").ok);
    const double sq = area(mapOf(s, m).layers[0].features[0].geom);
    Result r = run(s, "maps.overlay doc=" + m + " layer=a with=b op=intersection");
    CHECK(r.ok && near(num(r, "area_m2") / sq, 0.5, 0.02));
    CHECK(mapOf(s, m).layer("a intersection")->features[0].props.at("with_name").str == "B");
    CHECK(mapOf(s, m).layer("a intersection")->features[0].props.at("zone").num == 2);
    r = run(s, "maps.overlay doc=" + m + " layer=a with=b op=difference");
    CHECK(r.ok && near(num(r, "area_m2") / sq, 0.5, 0.02));
    r = run(s, "maps.overlay doc=" + m + " layer=a with=b op=union");
    CHECK(r.ok && near(num(r, "area_m2") / sq, 1.5, 0.03));
    CHECK(mapOf(s, m).layer("a union")->features[0].geom.polygons.size() == 1);
    r = run(s, "maps.overlay doc=" + m + " layer=a with=b op=sym_difference");
    CHECK(r.ok && near(num(r, "area_m2") / sq, 1.0, 0.03));
    // a line across A: the part inside is exactly the square's width
    const double dLat = 500 / 111320.0;
    char line[128];
    std::snprintf(line, sizeof line, "%.7f,%.7f,%.7f,%.7f", 45 + dLat, 6.99, 45 + dLat, 7.03);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=road points=" + line).ok);
    r = run(s, "maps.overlay doc=" + m + " layer=road with=a op=intersection");
    CHECK(r.ok && near(num(r, "length_m"), 1000, 2));
    r = run(s, "maps.overlay doc=" + m + " layer=road with=a op=difference output=outside");
    CHECK(r.ok && mapOf(s, m).layer("outside")->features[0].geom.lines.size() == 2);
    // select by location
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=pts wkt=\"MULTIPOINT ((7.001 45.001))\"").ok);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=pts lat=45.0045 lon=7.0005").ok);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=pts lat=46 lon=8").ok);
    r = run(s, "maps.select_location doc=" + m + " layer=pts by=a mode=within");
    CHECK(r.ok && num(r, "count") == 2);
    r = run(s, "maps.select_location doc=" + m + " layer=pts by=a mode=disjoint");
    CHECK(r.ok && num(r, "count") == 1);
    r = run(s, "maps.select_location doc=" + m + " layer=pts by=a mode=near distance=200000");
    CHECK(r.ok && num(r, "count") == 3);
    r = run(s, "maps.select_location doc=" + m + " layer=a by=road mode=intersects");
    CHECK(r.ok && num(r, "count") == 1);
    // dissolve two squares that share an edge
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=fields kind=polygon points=" + square(40, 10, 1000) + " props=\"farm=x; ha=100\"").ok);
    const double dLon40 = 1000 / (111320.0 * std::cos(40 * kDeg));
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=fields kind=polygon points=" + square(40, 10 + dLon40, 1000) + " props=\"farm=x; ha=100\"").ok);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=fields kind=polygon points=" + square(41, 10, 1000) + " props=\"farm=y; ha=50\"").ok);
    r = run(s, "maps.dissolve doc=" + m + " layer=fields field=farm sum=ha");
    CHECK(r.ok && num(r, "features") == 2);
    const Layer* d = mapOf(s, m).layer("fields dissolved");
    CHECK(d && d->features[0].props.at("ha").num == 200 && d->features[0].props.at("count").num == 2);
    CHECK(d && d->features[0].geom.polygons.size() == 1);
    CHECK(d && near(area(d->features[0].geom), 2 * area(mapOf(s, m).layer("fields")->features[0].geom), 2 * area(mapOf(s, m).layer("fields")->features[0].geom) * 0.02));
    // the countries of Europe into one
    CHECK(run(s, "maps.add_countries doc=" + m + " where=\"continent == 'South America'\"").ok);
    r = run(s, "maps.dissolve doc=" + m + " layer=countries field=continent sum=pop_est resolution=400");
    CHECK(r.ok && num(r, "features") == 1);
}

std::string isoAt(int seconds) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "2024-05-01T%02d:%02d:%02dZ", 8 + seconds / 3600, seconds / 60 % 60, seconds % 60);
    return buf;
}

void testTracks(const fs::path& dir) {
    // 3 km due north, a point every 10 m and 3 s, with a 20 minute stop half way
    std::string gpx = "<?xml version=\"1.0\"?><gpx version=\"1.1\" creator=\"test\"><trk><name>Run</name><trkseg>";
    for (int i = 0; i <= 300; ++i) {
        const int t = i * 3 + (i > 150 ? 1200 : 0);
        char buf[200];
        std::snprintf(buf, sizeof buf, "<trkpt lat=\"%.8f\" lon=\"7\"><ele>%.1f</ele><time>%s</time></trkpt>", 46 + i * 10 / 111194.9, 500.0 + (i % 2) * 0.5 + i * 0.2,
                      isoAt(t).c_str());
        gpx += buf;
    }
    gpx += "</trkseg></trk></gpx>";
    writeText(dir / "run.gpx", gpx);
    Session s;
    registerCommands(s);
    Result r = run(s, "maps.open path=" + q(dir / "run.gpx"));
    CHECK(r.ok);
    const std::string m = toText(r.value);
    r = run(s, "maps.track_splits doc=" + m + " path=" + q(dir / "splits.csv"));
    CHECK(r.ok && num(r, "splits") == 3);
    CHECK(fs::exists(dir / "splits.csv"));
    const auto splits = trackSplits({&mapOf(s, m).layers[0].features[0].geom.lines[0]}, 1000);
    CHECK(splits.size() == 3 && near(splits[0].distance, 1000, 1e-6) && near(splits[0].duration, 300, 1));
    CHECK(near(splits[1].duration, 300 + 1200, 2));  // the stop is in the second kilometre
    r = run(s, "maps.track_split doc=" + m + " gap_minutes=5");
    CHECK(r.ok && num(r, "parts") == 2 && mapOf(s, m).layers[0].features.size() == 2);
    CHECK(mapOf(s, m).layers[0].features[1].props.at("part").num == 2);
    r = run(s, "maps.track_join doc=" + m);
    CHECK(r.ok && mapOf(s, m).layers[0].features.size() == 1 && near(num(r, "length_m"), 3000, 3));
    r = run(s, "maps.track_trim doc=" + m + " from=500 to=-500");
    CHECK(r.ok && near(num(r, "length_m"), 2000, 2));
    CHECK(run(s, "undo").ok);
    r = run(s, "maps.track_trim doc=" + m + " start=" + isoAt(150) + " end=" + isoAt(750));  // 50 to 250 points: 0.5 to 1.5 km... minus the stop
    CHECK(r.ok && near(num(r, "length_m"), 1010, 15));
    CHECK(run(s, "undo").ok);
    // smoothing lowers the zig-zag climb
    r = run(s, "maps.track_smooth doc=" + m + " window=5 output=smooth");
    CHECK(r.ok && mapOf(s, m).layer("smooth") != nullptr);
    std::vector<const Line*> raw{&mapOf(s, m).layers[0].features[0].geom.lines[0]}, smooth{&mapOf(s, m).layer("smooth")->features[0].geom.lines[0]};
    CHECK(trackStats(smooth, 0).gain < trackStats(raw, 0).gain);
    // a GPS jump is removed
    Line jumpy = mapOf(s, m).layers[0].features[0].geom.lines[0];
    jumpy[10].x += 0.05;
    CHECK(removeSpikes(jumpy, 20) == 1);
    CHECK(run(s, "maps.track_reverse doc=" + m + " layer=smooth").ok);
    CHECK(std::isnan(mapOf(s, m).layer("smooth")->features[0].geom.lines[0][0].t));
    CHECK(mapOf(s, m).layer("smooth")->features[0].geom.lines[0][0].y > 46.02);
    // a heatmap of the tracks follows their lines, not one dot each
    r = run(s, "maps.heatmap doc=" + m + " layer=\"run\" radius=100 resolution=100");
    CHECK(r.ok);
    const Grid& g = *mapOf(s, m).layers.back().grid;
    CHECK(g.sample(7, 46.001) > 0 && g.sample(7, 46.025) > 0);
    // elevations from a DEM
    auto dem = std::make_shared<Grid>();
    dem->width = dem->height = 10;
    dem->values.assign(100, 1234.0f);
    dem->geo = {0.01, 0, 6.95, 0, -0.01, 46.05};
    dem->updateRange();
    Layer dl;
    dl.name = "dem";
    dl.grid = dem;
    mapOf(s, m).layers.push_back(dl);
    r = run(s, "maps.track_elevation doc=" + m + " layer=run raster=dem");
    CHECK(r.ok && num(r, "set") == 301 && mapOf(s, m).layer("run")->features[0].geom.lines[0][5].z == 1234);
}

// ---- FIT ----------------------------------------------------------------------------------

void put(std::string& s, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void testFit(const fs::path& dir) {
    std::string body;
    // definition 0: record (20): timestamp, lat, lon, altitude, heart rate
    body += static_cast<char>(0x40);
    body += '\0';
    body += '\0';  // little endian
    put(body, 20, 2);
    body += static_cast<char>(5);
    const int fields[5][3] = {{253, 4, 0x86}, {0, 4, 0x85}, {1, 4, 0x85}, {2, 2, 0x84}, {3, 1, 0x02}};
    for (const auto& f : fields) {
        body += static_cast<char>(f[0]);
        body += static_cast<char>(f[1]);
        body += static_cast<char>(f[2]);
    }
    // definition 1: compressed-timestamp records without the timestamp field
    body += static_cast<char>(0x41);
    body += '\0';
    body += '\0';
    put(body, 20, 2);
    body += static_cast<char>(2);
    for (const auto& f : {fields[1], fields[2]}) {
        body += static_cast<char>(f[0]);
        body += static_cast<char>(f[1]);
        body += static_cast<char>(f[2]);
    }
    const double semi = 2147483648.0 / 180.0;
    const std::uint32_t t0 = 1000000000;
    for (int i = 0; i < 10; ++i) {
        body += static_cast<char>(0x00);
        put(body, t0 + static_cast<std::uint32_t>(i) * 10, 4);
        put(body, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround((47 + i * 0.001) * semi))), 4);
        put(body, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(8.5 * semi))), 4);
        put(body, static_cast<std::uint64_t>((400 + i + 500) * 5), 2);
        body += static_cast<char>(120 + i);
    }
    // one compressed-timestamp record, 3 s after the last
    body += static_cast<char>(0x80 | (1 << 5) | ((t0 + 93) & 0x1F));
    put(body, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(47.011 * semi))), 4);
    put(body, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(8.5 * semi))), 4);
    // a record with no position (indoor): skipped
    body += static_cast<char>(0x00);
    put(body, t0 + 100, 4);
    put(body, 0x7FFFFFFF, 4);
    put(body, 0x7FFFFFFF, 4);
    put(body, 0xFFFF, 2);
    body += static_cast<char>(0xFF);
    // session (18) with sport = cycling
    body += static_cast<char>(0x42);
    body += '\0';
    body += '\0';
    put(body, 18, 2);
    body += static_cast<char>(1);
    body += static_cast<char>(5);
    body += static_cast<char>(1);
    body += static_cast<char>(0);
    body += static_cast<char>(0x02);
    body += static_cast<char>(2);
    std::string file;
    file += static_cast<char>(12);
    file += static_cast<char>(0x20);
    put(file, 2132, 2);
    put(file, body.size(), 4);
    file += ".FIT";
    file += body;
    put(file, 0, 2);  // CRC (not checked)
    writeText(dir / "ride.fit", file);

    Session s;
    registerCommands(s);
    Result r = run(s, "maps.open path=" + q(dir / "ride.fit"));
    CHECK(r.ok);
    const std::string fitId = toText(r.value);
    const Map& m = mapOf(s, fitId);
    CHECK(m.layers.size() == 1 && m.layers[0].features.size() == 1);
    const Feature& f = m.layers[0].features[0];
    CHECK(f.geom.lines[0].size() == 11);
    CHECK(near(f.geom.lines[0][0].y, 47, 1e-7) && near(f.geom.lines[0][0].x, 8.5, 1e-7));
    CHECK(near(f.geom.lines[0][3].z, 403, 0.01));
    CHECK(near(f.geom.lines[0][0].t, 1000000000.0 + 631065600, 0.5));
    CHECK(near(f.geom.lines[0][10].t - f.geom.lines[0][9].t, 3, 0.5));
    CHECK(f.props.at("sport").str == "cycling");
    CHECK(f.props.at("max_hr").num == 129);
    CHECK(r.message.find("without a GPS position") != std::string::npos);
    CHECK(!run(s, "maps.export doc=" + toText(r.value) + " path=" + q(dir / "back.fit")).ok);
    bool threw = false;
    try {
        parseFit("not a fit file at all");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- OSM, routing and search ---------------------------------------------------------------

// A 4 x 4 street grid, 100 m apart, south-west corner at 50,4.
std::string osmGrid() {
    const double dLat = 100 / 111194.9, dLon = 100 / (111194.9 * std::cos(50 * kDeg));
    std::string x = "<?xml version='1.0' encoding='UTF-8'?>\n<osm version=\"0.6\" generator=\"test\">\n";
    auto id = [](int r, int c) { return 100 + r * 10 + c; };
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "<node id=\"%d\" lat=\"%.8f\" lon=\"%.8f\"/>\n", id(r, c), 50 + r * dLat, 4 + c * dLon);
            x += buf;
        }
    }
    char buf[300];
    std::snprintf(buf, sizeof buf,
                  "<node id=\"900\" lat=\"%.8f\" lon=\"%.8f\"><tag k=\"amenity\" v=\"cafe\"/><tag k=\"name\" v=\"Caf\xC3\xA9 de la Gare\"/></node>\n",
                  50 + 1.5 * dLat, 4 + 1.5 * dLon);
    x += buf;
    // streets: rows are named, columns are avenues; row 1 is one-way east
    int way = 1;
    for (int r = 0; r < 4; ++r) {
        x += "<way id=\"" + std::to_string(way++) + "\">";
        for (int c = 0; c < 4; ++c) x += "<nd ref=\"" + std::to_string(id(r, c)) + "\"/>";
        x += "<tag k=\"highway\" v=\"residential\"/><tag k=\"name\" v=\"Row " + std::to_string(r) + " Street\"/>";
        if (r == 1) x += "<tag k=\"oneway\" v=\"yes\"/>";
        x += "</way>\n";
    }
    for (int c = 0; c < 4; ++c) {
        x += "<way id=\"" + std::to_string(way++) + "\">";
        for (int r = 0; r < 4; ++r) x += "<nd ref=\"" + std::to_string(id(r, c)) + "\"/>";
        x += "<tag k=\"highway\" v=\"" + std::string(c == 3 ? "footway" : "residential") + "\"/><tag k=\"name\" v=\"Avenue " + std::to_string(c) + "\"/></way>\n";
    }
    // a footpath straight across from the south-west to the north-east corner
    x += "<way id=\"50\"><nd ref=\"" + std::to_string(id(0, 0)) + "\"/><nd ref=\"" + std::to_string(id(3, 3)) + "\"/><tag k=\"highway\" v=\"footway\"/></way>\n";
    // a building
    x += "<way id=\"60\"><nd ref=\"" + std::to_string(id(1, 1)) + "\"/><nd ref=\"" + std::to_string(id(1, 2)) + "\"/><nd ref=\"" + std::to_string(id(2, 2)) +
         "\"/><nd ref=\"" + std::to_string(id(2, 1)) + "\"/><nd ref=\"" + std::to_string(id(1, 1)) + "\"/><tag k=\"building\" v=\"yes\"/></way>\n";
    x += "</osm>\n";
    return x;
}

void testRouting(const fs::path& dir) {
    writeText(dir / "town.osm", osmGrid());
    Session s;
    registerCommands(s);
    Result r = run(s, "maps.open path=" + q(dir / "town.osm"));
    CHECK(r.ok);
    const std::string m = toText(r.value);
    CHECK(mapOf(s, m).layer("town roads") && mapOf(s, m).layer("town roads")->features.size() == 9);
    CHECK(mapOf(s, m).layer("town areas") && mapOf(s, m).layer("town areas")->features.size() == 1);
    CHECK(mapOf(s, m).layer("town places") && mapOf(s, m).layer("town places")->features.size() == 1);
    const double dLat = 100 / 111194.9, dLon = 100 / (111194.9 * std::cos(50 * kDeg));
    auto at = [&](double r, double c) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.8f,%.8f", 50 + r * dLat, 4 + c * dLon);
        return std::string(buf);
    };
    // by car from the south-west to the north-east corner: 600 m on the grid (the diagonal is a footway)
    r = run(s, "maps.route doc=" + m + " from=" + at(0, 0) + " to=" + at(3, 2));
    CHECK(r.ok && near(num(r, "distance_m"), 500, 3));
    CHECK(r.message.find("Head") != std::string::npos && r.message.find("Arrive") != std::string::npos);
    CHECK(r.message.find("Turn") != std::string::npos);
    CHECK(mapOf(s, m).layer("route") && mapOf(s, m).layer("route")->features.size() == 1);
    // on foot the diagonal footway is shorter
    r = run(s, "maps.route doc=" + m + " from=" + at(0, 0) + " to=" + at(3, 3) + " profile=foot");
    CHECK(r.ok && near(num(r, "distance_m"), std::hypot(300.0, 300.0), 3));
    CHECK(near(num(r, "duration_s"), std::hypot(300.0, 300.0) / (5 / 3.6), 3));
    // row 1 is one-way east: going west along it a car has to go round
    r = run(s, "maps.route doc=" + m + " from=" + at(1, 2) + " to=" + at(1, 0) + " layer=\"town roads\"");
    CHECK(r.ok && num(r, "distance_m") > 300);
    r = run(s, "maps.route doc=" + m + " from=" + at(1, 2) + " to=" + at(1, 0) + " profile=bike");
    CHECK(r.ok && num(r, "distance_m") > 300);
    r = run(s, "maps.route doc=" + m + " from=" + at(1, 2) + " to=" + at(1, 0) + " profile=foot");
    CHECK(r.ok && near(num(r, "distance_m"), 200, 2));
    // part-way along a street, through a via point
    r = run(s, "maps.route doc=" + m + " from=" + at(0, 0.5) + " to=" + at(0, 2.5) + " via=" + at(2, 1));
    CHECK(r.ok && near(num(r, "distance_m"), 600, 4));
    CHECK(r.message.find("Reach stop 1") != std::string::npos);
    // shortest still reports a time
    r = run(s, "maps.route doc=" + m + " from=" + at(0, 0) + " to=" + at(3, 2) + " shortest=true");
    CHECK(r.ok && num(r, "duration_s") > 0);
    CHECK(!run(s, "maps.route doc=" + m + " from=10,10 to=" + at(0, 0)).ok);  // too far from any road
    // reachable on foot in 3 minutes: about 250 m along the streets
    r = run(s, "maps.reach doc=" + m + " from=" + at(0, 0) + " minutes=3 profile=foot buffer=20");
    CHECK(r.ok && num(r, "area_m2") > 5000 && num(r, "area_m2") < 120000);
    // search: accents and case don't matter, a typo is forgiven
    r = run(s, "maps.search doc=" + m + " query=\"cafe gare\"");
    CHECK(r.ok && str(r, "layer") == "town places" && near(num(r, "lat"), 50 + 1.5 * dLat, 1e-6));
    r = run(s, "maps.search doc=" + m + " query=\"Row 2 Stret\"");
    CHECK(r.ok && str(r, "name") == "Row 2 Street");
    r = run(s, "maps.search query=Switzerland");
    CHECK(r.ok && str(r, "name") == "Switzerland" && near(num(r, "lat"), 46.8, 1));
    r = run(s, "maps.search query=\"Germny\"");
    CHECK(r.ok && str(r, "name") == "Germany");
    CHECK(!run(s, "maps.search query=Atlantis").ok);
    // the building was a polygon with its tag
    CHECK(mapOf(s, m).layer("town areas")->features[0].props.at("building").str == "yes");
}

void testViewAndHit(const fs::path& dir) {
    Session s;
    registerCommands(s);
    const std::string m = newMap(s);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=pins lat=48.8584 lon=2.2945 props=\"name=Eiffel\"").ok);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=pins lat=48.8606 lon=2.3376 props=\"name=Louvre\"").ok);
    CHECK(run(s, "maps.feature_add doc=" + m + " layer=walk points=48.8584,2.2945,48.8606,2.3376").ok);
    // what's under the Louvre's pixel
    Map& map = mapOf(s, m);
    const RenderOptions o = previewOptions(map);
    const Coord px = lonLatToPixel(map, o, 2.3376, 48.8606);
    Result r = run(s, "maps.hit doc=" + m + " x=" + std::to_string(px.x) + " y=" + std::to_string(px.y) + " layer=pins");
    CHECK(r.ok && str(r, "layer") == "pins" && num(r, "index") == 1 && str(r, "name") == "Louvre");
    CHECK(near(num(r, "lat"), 48.8606, 1e-4));
    r = run(s, "maps.hit doc=" + m + " x=" + std::to_string(px.x) + " y=" + std::to_string(px.y));
    CHECK(r.ok && str(r, "layer") == "walk" && str(r, "kind") == "vertex" && num(r, "vertex") == 1);
    const Coord ll = pixelToLonLat(map, o, px.x, px.y);
    CHECK(near(ll.x, 2.3376, 1e-6) && near(ll.y, 48.8606, 1e-6));
    r = run(s, "maps.hit doc=" + m + " x=2 y=2");
    CHECK(r.ok && str(r, "kind") == "none");
    // bookmarks and the view
    CHECK(run(s, "maps.bookmark doc=" + m + " name=Louvre center=48.8606,2.3376 radius_km=0.5").ok);
    CHECK(run(s, "maps.bookmark doc=" + m + " name=all").ok);
    r = run(s, "maps.bookmarks doc=" + m);
    CHECK(r.ok && num(r, "count") == 2);
    CHECK(run(s, "maps.view doc=" + m + " bookmark=louvre").ok);
    CHECK(map.view && near(map.view->width(), 1000 / (111320.0 * std::cos(48.86 * kDeg)), 1e-4));
    r = run(s, "maps.preview doc=" + m);
    CHECK(r.ok && str(r, "bbox").substr(0, 4) == "2.33");
    CHECK(run(s, "maps.view doc=" + m + " zoom=2").ok);
    CHECK(near(map.view->width(), 500 / (111320.0 * std::cos(48.86 * kDeg)), 1e-4));
    CHECK(run(s, "maps.view doc=" + m + " pan=0.5,0").ok);
    CHECK(run(s, "maps.render doc=" + m + " path=" + q(dir / "louvre.png") + " bookmark=Louvre width=300").ok);
    CHECK(!run(s, "maps.render doc=" + m + " path=" + q(dir / "x.png") + " bookmark=nowhere").ok);
    CHECK(run(s, "maps.save doc=" + m + " path=" + q(dir / "paris.wwmap")).ok);
    CHECK(run(s, "maps.bookmark_delete doc=" + m + " name=all").ok);
    CHECK(!run(s, "maps.bookmark_delete doc=" + m + " name=all").ok);
    CHECK(run(s, "maps.view doc=" + m + " reset=true").ok && !map.view);
    r = run(s, "maps.open path=" + q(dir / "paris.wwmap"));
    CHECK(r.ok);
    const std::string backId = toText(r.value);
    const Map& back = mapOf(s, backId);
    CHECK(back.bookmarks.size() == 2 && back.bookmarks[0].name == "Louvre" && back.view.has_value());
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    const std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"maps_feature_update", "maps_feature_move", "maps_vertices", "maps_vertex_move", "maps_vertex_add", "maps_vertex_delete",
                             "maps_split", "maps_merge", "maps_dissolve", "maps_overlay", "maps_select_location", "maps_track_splits",
                             "maps_track_smooth", "maps_track_trim", "maps_track_split", "maps_track_join", "maps_track_reverse",
                             "maps_track_elevation", "maps_route", "maps_reach", "maps_search", "maps_bookmark", "maps_bookmarks",
                             "maps_bookmark_delete", "maps_view", "maps_hit"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    const std::string out =
        server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"maps_search","arguments":{"query":"france"}}})").value_or("");
    CHECK(out.find("France") != std::string::npos);
}

}  // namespace

int main() {
    const fs::path dir = tempDir();
    setEnv("WETWARESUITE_HOME", (dir / "home").string());
    testEditing();
    testOverlay();
    testTracks(dir);
    testFit(dir);
    testRouting(dir);
    testViewAndHit(dir);
    testMcp();
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("maps tools");
}
