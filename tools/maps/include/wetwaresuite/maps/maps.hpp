// maps: offline maps and lightweight GIS, headless. A map document holds
// layers: vector layers of features (points, lines, polygons with
// properties) and raster layers (georeferenced images and value grids).
// Coordinates are always stored as WGS84 longitude/latitude in degrees;
// other coordinate systems (Web Mercator, UTM, transverse Mercator) apply
// when importing, exporting, measuring and rendering.
#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::maps {

using raster::Color;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// ---- geometry -----------------------------------------------------------------------

struct Coord {
    double x = 0, y = 0;  // longitude, latitude in degrees (or projected metres)
    double z = kNaN;      // elevation in metres, NaN when unknown
    double t = kNaN;      // time in Unix seconds, NaN when unknown
};
using Line = std::vector<Coord>;
using Ring = std::vector<Coord>;      // closed implicitly: the first point is not repeated
using Polygon = std::vector<Ring>;    // [0] is the outer ring, the rest are holes

enum class GeomType { None, Point, Line, Polygon };
const char* toString(GeomType type);

struct Geometry {
    GeomType type = GeomType::None;
    std::vector<Coord> points;      // Point (MultiPoint when more than one)
    std::vector<Line> lines;        // Line (MultiLineString when more than one)
    std::vector<Polygon> polygons;  // Polygon (MultiPolygon when more than one)

    bool empty() const;
    std::size_t vertexCount() const;
    std::string typeName() const;  // GeoJSON name: Point, MultiLineString, ...
    // Calls fn for every vertex (mutable / const).
    void forEach(const std::function<void(Coord&)>& fn);
    void forEach(const std::function<void(const Coord&)>& fn) const;

    static Geometry point(double lon, double lat);
    static Geometry line(Line l);
    static Geometry polygon(Polygon p);
};

struct BBox {
    double minX = std::numeric_limits<double>::infinity(), minY = minX;
    double maxX = -std::numeric_limits<double>::infinity(), maxY = maxX;
    bool valid() const { return minX <= maxX && minY <= maxY; }
    void add(double x, double y);
    void add(const BBox& b);
    bool intersects(const BBox& b) const;
    bool contains(double x, double y) const { return x >= minX && x <= maxX && y >= minY && y <= maxY; }
    double width() const { return maxX - minX; }
    double height() const { return maxY - minY; }
};
BBox bboxOf(const Geometry& g);

// ---- properties -----------------------------------------------------------------------

struct Prop {
    enum Kind { Null, Bool, Number, String } kind = Null;
    double num = 0;  // Number, and Bool as 0/1
    std::string str;

    static Prop null() { return {}; }
    static Prop boolean(bool b) { return {Bool, b ? 1.0 : 0.0, {}}; }
    static Prop number(double v) { return {Number, v, {}}; }
    static Prop string(std::string s) { return {String, 0, std::move(s)}; }
    // "12.5" -> Number, "true"/"false" -> Bool, "" -> Null, anything else (and "007") -> String.
    static Prop infer(const std::string& text);

    bool isNull() const { return kind == Null; }
    std::string text() const;  // as written in CSV / labels
    std::optional<double> asNumber() const;
    bool truthy() const;
    bool operator==(const Prop& o) const;
};
using Props = std::map<std::string, Prop>;

struct Feature {
    Geometry geom;
    Props props;
};

// ---- coordinate systems ---------------------------------------------------------------

struct Crs {
    enum Kind { Geographic, WebMercator, TransverseMercator } kind = Geographic;
    int zone = 0;          // UTM zone 1..60 (0 = a custom transverse Mercator)
    bool south = false;    // UTM southern hemisphere (false northing 10,000 km)
    double lon0 = 0;       // central meridian (transverse Mercator)
    double k0 = 0.9996;    // scale factor on the central meridian
    double falseEasting = 500000, falseNorthing = 0;

    static Crs wgs84() { return {}; }
    static Crs webMercator();
    static Crs utm(int zone, bool south);
    static Crs utmFor(double lon, double lat);         // the UTM zone a point is in
    static Crs localTm(double lon0);                   // k0 = 1, no false origin: for local metric work
    // "wgs84", "epsg:4326", "lonlat", "webmercator", "epsg:3857", "utm33n", "utm 33s", "epsg:32633",
    // "auto" (not handled here). Also reads a .prj WKT. Empty when unknown.
    static std::optional<Crs> parse(const std::string& text);
    static const std::vector<std::string>& names();   // examples for help texts

    bool geographic() const { return kind == Geographic; }
    std::string name() const;   // "WGS84", "Web Mercator", "UTM 33N"
    std::string id() const;     // "EPSG:4326", "EPSG:3857", "EPSG:32633", or "TM lon0=..." for a custom TM
    int epsg() const;           // 0 for a custom TM
    std::string wkt() const;    // ESRI-style WKT for .prj files

    Coord forward(const Coord& lonlat) const;  // lon/lat degrees -> this CRS
    Coord inverse(const Coord& xy) const;      // this CRS -> lon/lat degrees
};

// The WGS84 ellipsoid.
constexpr double kA = 6378137.0;
constexpr double kF = 1.0 / 298.257223563;
constexpr double kAuthalicRadius = 6371007.180918475;

// ---- geodesy (WGS84 ellipsoid) ------------------------------------------------------

struct Inverse {
    double distance = 0;  // metres
    double azimuth1 = 0;  // initial bearing, degrees clockwise from north
    double azimuth2 = 0;  // final bearing
};
// Vincenty's inverse formula, falling back to a spherical estimate for nearly antipodal points.
Inverse geodesicInverse(double lon1, double lat1, double lon2, double lat2);
double geodesicDistance(double lon1, double lat1, double lon2, double lat2);
// Vincenty's direct formula: the point `distance` metres from (lon, lat) along `azimuth`.
Coord geodesicDirect(double lon, double lat, double azimuth, double distance);
double lineLength(const Line& line, bool closed = false);  // metres
// Area of a ring on the authalic sphere (great-circle edges), m², always positive.
double ringArea(const Ring& ring);
double polygonArea(const Polygon& polygon);  // outer minus holes
double area(const Geometry& g);              // m², polygons only
double length(const Geometry& g);            // m: lines, or polygon perimeters
Coord centroid(const Geometry& g);           // area/length/point weighted, in lon/lat
// A point guaranteed inside a polygon geometry (for labels); the centroid for others.
Coord labelPoint(const Geometry& g);

// ---- geometry operations (analysis.cpp) ---------------------------------------------

// Planar helpers on lon/lat or projected coordinates.
double signedArea(const Ring& ring);  // planar, positive = counter-clockwise
bool pointInRing(const Ring& ring, double x, double y);
bool pointInPolygon(const Polygon& polygon, double x, double y);
bool pointInGeometry(const Geometry& g, double lon, double lat);

// Douglas-Peucker with a tolerance in metres. Rings keep at least 3 points, lines 2;
// rings that collapse are dropped. Returns the simplified geometry (maybe empty).
Geometry simplify(const Geometry& g, double toleranceMetres);

// Rectangle clip (lon/lat box): Sutherland-Hodgman for polygons, segment clipping for lines.
Geometry clip(const Geometry& g, const BBox& box);

// Buffer by `distance` metres (negative shrinks polygons). Points become geodesic circles;
// lines and polygons go through a distance field on a local metric grid of about
// `resolution` cells along the longer side, then contours, so overlaps merge cleanly.
Geometry buffer(const Geometry& g, double distance, int resolution = 600, int circleSegments = 48);
// Buffers several geometries into one dissolved geometry.
Geometry bufferUnion(const std::vector<const Geometry*>& gs, double distance, int resolution = 600);

// Closest point of a geometry to (lon, lat) and its geodesic distance; distance 0 inside polygons.
struct Nearest {
    double distance = std::numeric_limits<double>::infinity();
    Coord point;
};
Nearest nearestPoint(const Geometry& g, double lon, double lat);

// Filter expressions: fields, numbers, 'strings', + - * / %, == != < <= > >=, and or not,
// "field in ('a', 'b')", "field contains 'x'", "field is null", $area, $length, $id.
class Expr {
public:
    static Expr compile(const std::string& text);  // throws std::runtime_error with the position
    Prop eval(const Feature& f, std::size_t index = 0) const;
    bool test(const Feature& f, std::size_t index = 0) const { return eval(f, index).truthy(); }
    const std::string& text() const { return text_; }
    const std::vector<std::string>& fields() const { return fields_; }  // property names it reads
    struct Node;

private:
    std::string text_;
    std::vector<std::string> fields_;
    std::shared_ptr<const Node> root_;
};

// ---- GPX tracks -----------------------------------------------------------------------

struct TrackStats {
    double distance = 0;           // m
    double gain = 0, loss = 0;     // m, with a small hysteresis against GPS noise
    double minEle = kNaN, maxEle = kNaN;
    double startTime = kNaN, endTime = kNaN;
    double duration = 0;           // s, NaN-free (0 without times)
    double movingTime = 0;         // s spent above 0.5 m/s
    double avgSpeed = 0, movingSpeed = 0, maxSpeed = 0;  // m/s
    std::size_t points = 0;
    bool hasTime = false, hasEle = false;
};
TrackStats trackStats(const std::vector<const Line*>& segments, double hysteresis = 3.0);

// ---- raster layers --------------------------------------------------------------------

// Pixel (col, row) of the top-left pixel corner -> coordinates: x = a*col + b*row + c, y = d*col + e*row + f.
struct Affine {
    double a = 1, b = 0, c = 0, d = 0, e = -1, f = 0;
    Coord apply(double col, double row) const { return {a * col + b * row + c, d * col + e * row + f}; }
    bool invert(double x, double y, double& col, double& row) const;
};

struct Grid {
    int width = 0, height = 0;
    raster::Image image;       // RGBA pixels; empty for value-only grids (shown with a colour ramp)
    std::vector<float> values;  // single-band values (elevation, density), empty for plain images
    double nodata = kNaN;
    double minValue = 0, maxValue = 0;
    Affine geo;                 // pixel corner -> CRS coordinates
    Crs crs;
    std::string source;         // file it came from

    bool hasValues() const { return !values.empty(); }
    void updateRange();
    BBox bounds() const;        // lon/lat box covering the grid
    // Value (or luminance of the pixel) at lon/lat; NaN outside or at nodata.
    double sample(double lon, double lat) const;
};

// Colour ramps for value grids and graduated styles: gray, terrain, heat, viridis, blues, reds, greens, spectral.
const std::vector<std::string>& rampNames();
Color rampColor(const std::string& ramp, double t);  // t in 0..1

// ---- layers and maps ------------------------------------------------------------------

struct Style {
    Color stroke{0x2b, 0x5c, 0xab, 255};
    double width = 1.5;        // px
    Color fill{0x2b, 0x5c, 0xab, 255};
    double fillOpacity = 0.35;
    double opacity = 1.0;      // whole layer
    double pointSize = 7;      // px diameter
    std::string marker = "circle";  // circle, square, triangle, diamond
    std::string dash;          // "" solid, "dash", "dot"
    std::string labelField;    // property shown as a label, empty = none
    double labelSize = 11;     // px
    Color labelColor{0x22, 0x22, 0x22, 255};
    bool halo = true;
    std::string mode = "single";  // single, categorized, graduated
    std::string field;            // property categorized/graduated by
    std::string ramp = "viridis"; // graduated colour ramp, or "#rrggbb,#rrggbb" ends
    std::vector<Color> palette;   // categorized colours (empty = automatic)
    int classes = 5;
    std::string method = "quantile";  // graduated: quantile, equal, jenks-lite (natural)
    bool sizeByValue = false;     // graduated points: grow with the value as well
    std::string rasterRamp = "terrain";  // value grids
    bool hillshade = false;       // value grids: shade relief
};

struct StyleClass {
    std::string label;
    Color color;
    double size = 0;        // point size override (graduated sizeByValue)
    double lo = 0, hi = 0;  // graduated range
    Prop value;             // categorized value
    bool other = false;     // "other" bucket of a categorized style
};
struct Layer {
    std::string name;
    std::vector<Feature> features;  // vector layers
    std::shared_ptr<const Grid> grid;  // raster layers
    Style style;
    bool visible = true;
    std::string source;  // file it came from

    bool isRaster() const { return grid != nullptr; }
    BBox bounds() const;
    std::vector<std::string> fields() const;  // property names in first-seen order
    std::string geometrySummary() const;      // "12 points, 3 polygons"
};

struct Map {
    std::string title;
    std::vector<Layer> layers;  // bottom first

    Layer* layer(const std::string& nameOrIndex);
    const Layer* layer(const std::string& nameOrIndex) const;
    int layerIndex(const std::string& nameOrIndex) const;
    std::string uniqueName(const std::string& base) const;
    BBox bounds(bool visibleOnly = false) const;
};

// The classes a categorized/graduated style resolves to for this layer's features.
std::vector<StyleClass> styleClasses(const Layer& layer);

// The embedded Natural Earth 110m countries (public domain): name, iso_a3, continent, pop_est.
const std::vector<Feature>& countries();

// ---- files (io.cpp) -------------------------------------------------------------------

struct ImportOptions {
    std::optional<Crs> crs;        // source CRS when the file doesn't say (CSV x/y, shapefile without .prj, world files)
    std::string latColumn, lonColumn;  // CSV: default autodetect (lat/latitude/y, lon/lng/long/longitude/x)
    std::string wktColumn;         // CSV: a WKT geometry column (default autodetect "wkt"/"geometry"/"geom")
    std::string delimiter;         // CSV: default autodetect
    std::string what = "all";      // GPX: all, tracks, routes, waypoints
};

// Formats: geojson, kml, gpx, csv, shp, wkt. From the extension; .json is GeoJSON.
std::string vectorFormatFor(const std::filesystem::path& path);
// Reads a vector file into one or more layers (GPX gives tracks/routes/waypoints layers,
// a shapefile one layer). Throws std::runtime_error.
std::vector<Layer> importVector(const std::filesystem::path& path, const std::string& format, const ImportOptions& options,
                                std::vector<std::string>* warnings = nullptr);

struct ExportOptions {
    Crs crs;                 // output CRS (GeoJSON, CSV, shapefile, WKT; KML and GPX are always WGS84)
    int precision = 7;       // decimals for degrees (projected coords get 3)
};
// Writes one layer. A shapefile with mixed geometry writes one file set per type
// (name_points.shp, name_lines.shp, name_polygons.shp). Returns the files written.
std::vector<std::string> exportVector(const Layer& layer, const std::filesystem::path& path, const std::string& format,
                                      const ExportOptions& options);

// GeoJSON text <-> features.
std::vector<Feature> parseGeoJson(const std::string& text, const std::optional<Crs>& crs = std::nullopt);
std::string toGeoJson(const std::vector<Feature>& features, const ExportOptions& options = {});
Geometry parseWkt(const std::string& text);  // throws std::runtime_error
std::string toWkt(const Geometry& g, int precision = 7);
std::vector<Feature> parseKml(const std::string& text, std::vector<std::string>* warnings = nullptr);
std::string toKml(const Layer& layer);
struct GpxData {
    std::vector<Feature> tracks, routes, waypoints;
};
GpxData parseGpx(const std::string& text);
std::string toGpx(const Layer& layer);
std::vector<Feature> parseCsv(const std::string& text, const ImportOptions& options, std::vector<std::string>* warnings = nullptr);
std::string toCsv(const Layer& layer, const ExportOptions& options = {});
// Plain CSV table (header + rows of strings), for joins. Throws on empty input.
struct Table {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
};
Table readTable(const std::string& text, const std::string& delimiter = {});

// Shapefile (.shp + .shx + .dbf, .prj read if present).
Layer readShapefile(const std::filesystem::path& shp, const std::optional<Crs>& crs, std::vector<std::string>* warnings = nullptr);
std::vector<std::string> writeShapefile(const Layer& layer, const std::filesystem::path& shp, const ExportOptions& options);

// Raster files: PNG/JPG/BMP/TGA/GIF with a world file (.pgw/.jgw/.wld/...), or GeoTIFF
// (strips or tiles; none/LZW/Deflate/PackBits; 8/16-bit int or 32-bit float; geokeys for
// 4326, 3857 and UTM). `bbox` (lon/lat) places an image without a world file.
std::shared_ptr<Grid> readRaster(const std::filesystem::path& path, const std::optional<Crs>& crs, const std::optional<BBox>& bbox,
                                 std::vector<std::string>* warnings = nullptr);
// `georeferenced` (optional) says whether the file carried its own placement.
std::shared_ptr<Grid> readGeoTiff(const std::string& bytes, std::vector<std::string>* warnings = nullptr, bool* georeferenced = nullptr);
// PNG/JPG + world file + .prj, or .tif (float32 for value grids, RGBA8 for images). Returns files written.
std::vector<std::string> writeRaster(const Grid& grid, const std::filesystem::path& path, const std::string& ramp = "heat");
std::string encodeGeoTiff(const Grid& grid);
// Reads the six lines of a world file into a corner-based Affine.
std::optional<Affine> parseWorldFile(const std::string& text);
std::string worldFileText(const Affine& corner);

// Project files (.achmap, JSON): every layer with its style; rasters embedded as PNG/values.
std::string toProject(const Map& map);
Map fromProject(const std::string& text, const std::filesystem::path& dir);

// ---- analysis that builds layers ----------------------------------------------------

struct HeatmapOptions {
    double radius = 0;        // kernel radius in metres (0 = auto: 1/20 of the extent)
    int resolution = 400;     // cells along the longer side
    std::string weightField;  // property weighting each point, empty = 1
    std::optional<BBox> bbox; // default: the points' extent plus the radius
};
// Quartic-kernel density of point features on a Web Mercator grid.
std::shared_ptr<Grid> heatmap(const std::vector<Feature>& features, const HeatmapOptions& options);

// ---- rendering (render.cpp) -----------------------------------------------------------

struct RenderOptions {
    int width = 1024, height = 0;  // px; height 0 = from the extent's aspect ratio
    Crs crs = Crs::webMercator();
    std::optional<BBox> bbox;      // lon/lat; default: all visible layers plus a margin
    double margin = 0.06;          // fraction added around the auto extent
    // auto (an installed map pack, else countries), countries, none, pack, osm, or a .pmtiles file
    std::string basemap = "auto";
    std::string tileUrl = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";  // osm: a server, a {z}/{x}/{y} folder or a .pmtiles file
    std::string tileCache;         // default: the user cache dir + wetwaresuite/tiles
    bool offline = false;          // osm: use cached tiles only
    int maxTiles = 64;             // osm and map packs: most tiles one render uses (the zoom drops to fit)
    Color background{0xdd, 0xe8, 0xf0, 255};
    Color land{0xf4, 0xf1, 0xea, 255};
    Color borders{0xa8, 0xa4, 0x9c, 255};
    bool scaleBar = true, northArrow = true, legend = true, graticule = false, labels = true;
    double graticuleStep = 0;      // degrees, 0 = auto
    std::string title;
    double dpi = 96;               // PDF: px -> pt
};

struct RenderInfo {
    BBox bbox;               // lon/lat shown
    double metresPerPixel = 0;
    int width = 0, height = 0;
    int tilesUsed = 0, tilesFetched = 0, zoom = -1;
    std::string tileSource;   // the .pmtiles file drawn, empty for OSM/folder tiles
    std::string attribution;  // the credit drawn in the corner
    std::vector<std::string> warnings;
};

// Draws the map. format: png (raster PNG bytes), svg (text) or pdf (bytes).
std::string renderMap(const Map& map, const RenderOptions& options, const std::string& format, RenderInfo* info = nullptr);
raster::Image renderImage(const Map& map, const RenderOptions& options, RenderInfo* info = nullptr);

// Slippy-map tile maths (Web Mercator): which tile holds lon/lat at a zoom.
void lonLatToTile(double lon, double lat, int zoom, double& tx, double& ty);
void tileToLonLat(double tx, double ty, int zoom, double& lon, double& lat);
std::filesystem::path defaultTileCache();

// Elevation profile chart of (distance m, elevation m) pairs: png or svg bytes.
std::string renderProfile(const std::vector<std::pair<double, double>>& profile, int width, int height, const std::string& format,
                          const std::string& title);

// ---- documents and commands -----------------------------------------------------------

class MapDoc : public Document {
public:
    Map map;
    std::string kind() const override { return "map"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
};

// Undoable edit: snapshots the map before `fn`, restores it on undo or failure.
class MapEdit : public Action {
public:
    MapEdit(std::shared_ptr<MapDoc> doc, std::function<Result(MapDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<MapDoc> doc_;
    std::function<Result(MapDoc&)> fn_;
    Map before_;
};

// Registers every maps.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::maps
