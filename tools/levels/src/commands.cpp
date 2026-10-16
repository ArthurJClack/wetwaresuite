// Every levels.* command. Edits run inside a LevelEdit, so each is one undo
// step and a failed edit leaves the map as it was.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include "achclwss/levels/levels.hpp"
#include "achclwss/raster/codec.hpp"

namespace achclwss::levels {

namespace {

namespace fs = std::filesystem;
using Edit = std::function<Result(LevelDoc&, const Params&)>;

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string num(double v) {
    if (std::abs(v - std::round(v)) < 1e-9) return std::to_string(static_cast<long long>(std::llround(v)));
    std::ostringstream o;
    o << v;
    return o.str();
}

std::string plural(std::size_t n, const std::string& word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); }

int clampInt(std::int64_t v, int lo, int hi) { return static_cast<int>(std::clamp<std::int64_t>(v, lo, hi)); }

std::vector<std::string> splitList(const std::string& text, char sep = ',') {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + sep) {
        if (c == sep) {
            const auto a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
            if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

// ---- shared params ----------------------------------------------------------------

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open level, e.g. lvl1", true}; }
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG render of the map to the result", false, {}, false}; }
ParamSpec layerParam(const std::string& what = "tile layer name or index (0 = bottom); default: the top tile layer") {
    return {"layer", ParamType::String, what, false};
}
ParamSpec tileParam(const std::string& name = "tile", const std::string& what = "tile id within the tileset (0 = its first tile, -1 = empty)",
                    bool required = true) {
    return {name, ParamType::Int, what, required};
}
ParamSpec tilesetParam() {
    return {"tileset", ParamType::String, "tileset name or index the tile ids refer to (default: the first tileset)", false};
}

// ---- lookups ------------------------------------------------------------------------

// The tile layer a command paints on: "layer", else the top non-collision tile layer.
Layer* tileLayer(Map& m, const Params& p, std::string& error, const std::string& key = "layer") {
    if (has(p, key)) {
        Layer* l = m.layer(stringParam(p, key));
        if (!l) error = "no layer called '" + stringParam(p, key) + "' (levels.info lists them)";
        else if (l->kind != LayerKind::Tiles) error = "'" + l->name + "' is an object layer, not a tile layer";
        else return l;
        return nullptr;
    }
    for (auto it = m.layers.rbegin(); it != m.layers.rend(); ++it) {
        if (it->kind == LayerKind::Tiles && !it->collision) return &*it;
    }
    for (auto it = m.layers.rbegin(); it != m.layers.rend(); ++it) {
        if (it->kind == LayerKind::Tiles) return &*it;
    }
    error = "the map has no tile layer; add one with levels.layer_add";
    return nullptr;
}

// Tile id within a tileset -> gid. -1 = empty. With no tilesets, tile n is gid n+1 (drawn as a colour).
bool gidFor(const Map& m, std::int64_t tile, const Params& p, std::uint32_t& out, std::string& error) {
    if (tile < 0) {
        out = 0;
        return true;
    }
    if (m.tilesets.empty()) {
        if (tile > 100000) {
            error = "tile id too large";
            return false;
        }
        out = static_cast<std::uint32_t>(tile) + 1;
        return true;
    }
    const Tileset* t = has(p, "tileset") ? m.tileset(stringParam(p, "tileset")) : &m.tilesets.front();
    if (!t) {
        error = "no tileset called '" + stringParam(p, "tileset") + "'";
        return false;
    }
    if (tile >= t->tileCount) {
        error = "tileset " + t->name + " has tiles 0.." + std::to_string(t->tileCount - 1) + ", not " + std::to_string(tile);
        return false;
    }
    out = t->firstGid + static_cast<std::uint32_t>(tile);
    return true;
}

bool gidParam(const Map& m, const Params& p, const std::string& key, std::uint32_t& out, std::string& error) {
    if (!gidFor(m, intParam(p, key, 0), p, out, error)) return false;
    if (out && boolParam(p, "flip_h", false)) out |= kFlipH;
    if (out && boolParam(p, "flip_v", false)) out |= kFlipV;
    return true;
}

bool gidList(const Map& m, const NumberList& tiles, const Params& p, std::vector<std::uint32_t>& out, std::string& error) {
    for (double t : tiles) {
        std::uint32_t g = 0;
        if (!gidFor(m, static_cast<std::int64_t>(std::llround(t)), p, g, error)) return false;
        out.push_back(g);
    }
    return true;
}

// How a gid reads to people: "terrain:5" (tileset:local id), "empty", or "tile 3" with no tileset.
std::string tileName(const Map& m, std::uint32_t raw) {
    const std::uint32_t g = raw & kGidMask;
    if (!g) return "empty";
    std::string flips = std::string(raw & kFlipH ? " flip_h" : "") + (raw & kFlipV ? " flip_v" : "") + (raw & kFlipD ? " flip_d" : "");
    if (const Tileset* t = m.tilesetFor(g)) return t->name + ":" + std::to_string(g - t->firstGid) + flips;
    if (m.tilesets.empty()) return "tile " + std::to_string(g - 1) + flips;
    return "gid " + std::to_string(g) + flips;
}

std::int64_t localId(const Map& m, std::uint32_t raw) {
    const std::uint32_t g = raw & kGidMask;
    if (!g) return -1;
    if (const Tileset* t = m.tilesetFor(g)) return g - t->firstGid;
    return m.tilesets.empty() ? g - 1 : g;
}

CellRect regionParam(const Map& m, const Params& p) {
    CellRect r{0, 0, m.width, m.height};
    if (has(p, "x")) r.x = static_cast<int>(intParam(p, "x"));
    if (has(p, "y")) r.y = static_cast<int>(intParam(p, "y"));
    r.w = has(p, "width") ? static_cast<int>(intParam(p, "width")) : m.width - r.x;
    r.h = has(p, "height") ? static_cast<int>(intParam(p, "height")) : m.height - r.y;
    return r;
}

std::vector<ParamSpec> regionParams() {
    return {{"x", ParamType::Int, "region left column (default 0)", false},
            {"y", ParamType::Int, "region top row (default 0)", false},
            {"width", ParamType::Int, "region width in cells (default: to the right edge)", false},
            {"height", ParamType::Int, "region height in cells (default: to the bottom edge)", false}};
}

Properties parseProps(const std::string& text) {
    Properties out;
    const char sep = text.find(';') != std::string::npos || text.find('\n') != std::string::npos ? ';' : ',';
    std::string t = text;
    std::replace(t.begin(), t.end(), '\n', ';');
    for (const auto& item : splitList(t, sep)) {
        auto eq = item.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        out[k] = inferProperty(v);
    }
    return out;
}

std::string propsText(const Properties& props) {
    std::string s;
    for (const auto& [k, v] : props) s += (s.empty() ? "" : "; ") + k + "=" + v.value;
    return s;
}

const std::vector<std::string>& propertyTypes() {
    static const std::vector<std::string> t = {"auto", "string", "int", "float", "bool", "color", "file"};
    return t;
}

Property makeProperty(const Params& p, const std::string& value) {
    const std::string type = stringParam(p, "value_type", "auto");
    if (type == "auto") return inferProperty(value);
    return {type, value};
}

// ---- results ------------------------------------------------------------------------

Result& maybePreview(Result& r, const LevelDoc& doc, const Params& p) {
    if (r.ok && boolParam(p, "preview", false)) r.attach(*doc.preview());
    return r;
}

Result edited(const std::string& message, const Params& p, int changed = -1) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"));
    if (changed >= 0) r.with("changed", static_cast<std::int64_t>(changed));
    return r;
}

ActionFactory levelCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<LevelDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open level called '" + id + "' (use levels.new or levels.open first)"); });
        }
        auto withPreview = [edit](LevelDoc& d, const Params& q) {
            Result r = edit(d, q);
            return maybePreview(r, d, q);
        };
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, withPreview, p] {
                try {
                    return withPreview(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<LevelEdit>(doc, [withPreview, p](LevelDoc& d) { return withPreview(d, p); });
    };
}

Result docCreated(Session& session, std::shared_ptr<LevelDoc> doc, const std::string& verb, const Params& p) {
    const std::string id = session.workspace().add(doc, "lvl");
    Result r = Result::success(verb + " " + id + ": " + doc->summary(), id);
    r.with("doc", id)
        .with("width", static_cast<std::int64_t>(doc->map.width))
        .with("height", static_cast<std::int64_t>(doc->map.height))
        .with("tile_width", static_cast<std::int64_t>(doc->map.tileWidth))
        .with("tile_height", static_cast<std::int64_t>(doc->map.tileHeight))
        .with("layers", static_cast<std::int64_t>(doc->map.layers.size()))
        .with("tilesets", static_cast<std::int64_t>(doc->map.tilesets.size()))
        .with("objects", static_cast<std::int64_t>(doc->map.objectCount()));
    std::vector<std::string> missing;
    for (const auto& t : doc->map.tilesets) {
        if (!t.image.empty() && !t.pixels) missing.push_back(t.image);
    }
    if (!missing.empty()) {
        r.message += "\nwarning: tileset images not found (tiles draw as colours): ";
        for (std::size_t i = 0; i < missing.size(); ++i) r.message += (i ? ", " : "") + missing[i];
    }
    return maybePreview(r, *doc, p);
}

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

std::string layerLine(const Map& m, std::size_t i) {
    const Layer& l = m.layers[i];
    std::string s = std::to_string(i) + ": " + l.name + " (" + (l.kind == LayerKind::Tiles ? (l.collision ? "collision grid" : "tiles") : "objects");
    if (l.kind == LayerKind::Tiles) {
        std::size_t used = 0;
        for (auto c : l.cells) used += (c & kGidMask) ? 1 : 0;
        s += ", " + std::to_string(used) + " cells filled";
    } else {
        s += ", " + plural(l.objects.size(), "object");
    }
    if (!l.visible) s += ", hidden";
    if (l.opacity < 1) s += ", opacity " + num(l.opacity);
    if (!l.props.empty()) s += ", " + propsText(l.props);
    return s + ")";
}

std::string objectLine(const MapObject& o, const std::string& layer) {
    std::string s = "#" + std::to_string(o.id);
    if (!o.name.empty()) s += " " + o.name;
    if (!o.type.empty()) s += " [" + o.type + "]";
    s += " at " + num(o.x) + "," + num(o.y);
    if (o.shape != "point") s += " size " + num(o.width) + "x" + num(o.height);
    if (o.shape != "rect") s += " " + o.shape;
    s += " on " + layer;
    if (!o.props.empty()) s += " {" + propsText(o.props) + "}";
    return s;
}

// The object layer object_add uses: "layer" (created when missing), else the top object layer, else a new "objects".
Layer* objectLayer(Map& m, const Params& p, std::string& error) {
    if (has(p, "layer")) {
        if (Layer* l = m.layer(stringParam(p, "layer"))) {
            if (l->kind != LayerKind::Objects) {
                error = "'" + l->name + "' is a tile layer, not an object layer";
                return nullptr;
            }
            return l;
        }
        return &m.addLayer(stringParam(p, "layer"), LayerKind::Objects);
    }
    for (auto it = m.layers.rbegin(); it != m.layers.rend(); ++it) {
        if (it->kind == LayerKind::Objects) return &*it;
    }
    return &m.addLayer("objects", LayerKind::Objects);
}

bool validName(const std::string& n) { return !n.empty() && n.size() <= 128 && n.find_first_of("\"<>&\n") == std::string::npos; }

// ---- command groups -----------------------------------------------------------------

void registerFileCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"levels.new", "New Level",
             "Create an empty tile map and return its id (e.g. lvl1): width x height cells of tile_width x tile_height "
             "px, with one tile layer. Then add a tileset (levels.tileset_add, or levels.tileset_placeholder to draw "
             "one) and paint, fill or generate. Without a tileset, tile n draws as a flat colour.",
             {{"width", ParamType::Int, "map width in cells", false, {}, std::int64_t{32}},
              {"height", ParamType::Int, "map height in cells", false, {}, std::int64_t{24}},
              {"tile_width", ParamType::Int, "tile width in px", false, {}, std::int64_t{16}},
              {"tile_height", ParamType::Int, "tile height in px (default: tile_width)", false},
              {"layer", ParamType::String, "name of the first tile layer", false, {}, std::string("ground")},
              {"background", ParamType::String, "background colour, e.g. #203040 (default transparent)", false},
              previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    const int w = clampInt(intParam(p, "width", 32), 1, 4096), h = clampInt(intParam(p, "height", 24), 1, 4096);
                    const int tw = clampInt(intParam(p, "tile_width", 16), 1, 1024);
                    const int th = clampInt(intParam(p, "tile_height", tw), 1, 1024);
                    auto doc = std::make_shared<LevelDoc>();
                    doc->map = newMap(w, h, tw, th);
                    if (has(p, "background")) {
                        auto c = Color::parse(stringParam(p, "background"));
                        if (!c) return Result::failure("background isn't a colour: " + stringParam(p, "background"));
                        doc->map.background = "#" + c->hex().substr(1, 6);
                    }
                    const std::string layer = stringParam(p, "layer", "ground");
                    if (!layer.empty()) doc->map.addLayer(layer, LayerKind::Tiles);
                    return docCreated(session, doc, "created", p);
                });
            });

    reg.add({"levels.open", "Open Level",
             "Open a map: Tiled .tmx (CSV, base64, zlib or gzip data; embedded or external .tsx tilesets) or .tmj, the "
             "plain .json format levels.save writes, or an LDtk .ldtk/.json project (one level; IntGrid layers become "
             "collision grids, entities become objects). Tileset images load from paths relative to the map.",
             {{"path", ParamType::String, "map file", true},
              {"level", ParamType::String, "LDtk: level identifier (default: the first level)", false},
              previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        const fs::path path = stringParam(p, "path");
                        auto doc = std::make_shared<LevelDoc>();
                        if (has(p, "level")) {
                            std::ifstream in(path, std::ios::binary);
                            if (!in) return Result::failure("can't read " + path.string());
                            std::stringstream s;
                            s << in.rdbuf();
                            doc->map = fromLdtk(s.str(), path.parent_path(), stringParam(p, "level"));
                        } else {
                            doc->map = loadMap(path);
                        }
                        doc->path = path.string();
                        return docCreated(session, doc, "opened " + path.string() + " as", p);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"levels.save", "Save Level",
             "Save a map; the extension picks the format: .tmx (Tiled XML), .tmj (Tiled JSON), .json (plain JSON: rows "
             "of gids, tilesets, objects with properties; reopen with levels.open), .ldtk (LDtk-lite project JSON), or "
             ".png/.jpg/.bmp/.tga for a flattened render of the visible tile layers. Tileset image paths are written "
             "relative to the saved file. For CSV use levels.export_csv.",
             {docParam(),
              {"path", ParamType::String, "output file", true},
              {"format", ParamType::String, "override the extension", false, {"auto", "tmx", "tmj", "json", "ldtk", "png"}, std::string("auto")},
              {"encoding", ParamType::String, "TMX/TMJ tile data: csv (readable), base64, or zlib (smallest)", false, {"csv", "base64", "zlib"},
               std::string("csv")},
              {"scale", ParamType::Number, "image exports: zoom factor", false, {}, 1.0}},
             false},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                const fs::path path = stringParam(p, "path");
                std::string format = stringParam(p, "format", "auto");
                if (format == "auto") {
                    std::string ext = path.extension().string();
                    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    // an existing .json is overwritten as plain JSON, whatever it held before
                    format = ext == ".json" ? "json" : formatFor(path);
                }
                if (format == "csv") return Result::failure("CSV holds one layer: use levels.export_csv");
                if (format.empty()) return Result::failure("unknown extension on " + path.string() + " (use .tmx, .tmj, .json, .ldtk or .png)");
                std::vector<std::string> warnings;
                if (format == "ldtk") {
                    if (auto parent = path.parent_path(); !parent.empty()) {
                        std::error_code ec;
                        fs::create_directories(parent, ec);
                    }
                    std::ofstream out(path, std::ios::binary);
                    out << toLdtk(doc.map, path.parent_path(), &warnings);
                    if (!out) return Result::failure("can't write " + path.string());
                } else {
                    saveMap(doc.map, path, format, stringParam(p, "encoding", "csv"), numberParam(p, "scale", 1.0));
                }
                if (format != "png") doc.path = path.string();
                Result r = Result::success("saved " + path.string() + " (" + format + ")", path.string());
                for (const auto& w : warnings) r.message += "\nwarning: " + w;
                std::error_code ec;
                r.with("path", path.string()).with("format", format).with("bytes", static_cast<std::int64_t>(fs::file_size(path, ec)));
                return r;
            }, false));

    reg.add({"levels.close", "Close Level", "Close an open level (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<CloseAction>(session, stringParam(p, "doc")); });

    reg.add({"levels.info", "Level Info",
             "Describe a map: size, tilesets (tile ranges and images), layers bottom to top with how many cells or "
             "objects each holds, and map properties.",
             {docParam(), previewParam()}, false, true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                const Map& m = doc.map;
                Result r = edited(doc.summary(), p);
                r.message += "\ntilesets:";
                if (m.tilesets.empty()) r.message += " none (tiles draw as colours)";
                for (std::size_t i = 0; i < m.tilesets.size(); ++i) {
                    const Tileset& t = m.tilesets[i];
                    r.message += "\n  " + std::to_string(i) + ": " + t.name + " tiles 0.." + std::to_string(t.tileCount - 1) + " (gids " +
                                 std::to_string(t.firstGid) + ".." + std::to_string(t.firstGid + t.tileCount - 1) + "), " +
                                 std::to_string(t.columns) + " columns of " + std::to_string(t.tileWidth) + "x" + std::to_string(t.tileHeight) +
                                 (t.image.empty() ? "" : ", " + t.image) + (t.image.empty() || t.pixels ? "" : " (missing)") +
                                 (t.tileProps.empty() ? "" : ", " + plural(t.tileProps.size(), "tile") + " with properties");
                }
                r.message += "\nlayers (bottom first):";
                for (std::size_t i = 0; i < m.layers.size(); ++i) r.message += "\n  " + layerLine(m, i);
                if (!m.props.empty()) r.message += "\nproperties: " + propsText(m.props);
                std::string layers, tilesets;
                for (const auto& l : m.layers) layers += (layers.empty() ? "" : ",") + l.name;
                for (const auto& t : m.tilesets) tilesets += (tilesets.empty() ? "" : ",") + t.name;
                r.with("width", static_cast<std::int64_t>(m.width))
                    .with("height", static_cast<std::int64_t>(m.height))
                    .with("tile_width", static_cast<std::int64_t>(m.tileWidth))
                    .with("tile_height", static_cast<std::int64_t>(m.tileHeight))
                    .with("layers", layers)
                    .with("tilesets", tilesets)
                    .with("objects", static_cast<std::int64_t>(m.objectCount()));
                return r;
            }, false));

    reg.add({"levels.preview", "Preview Level",
             "Render the map to a PNG attachment (nothing is written): tile layers bottom to top, collision grids as a "
             "red overlay, objects as coloured boxes (points as dots), optionally a grid. Use it to look at the map.",
             {docParam(),
              {"layers", ParamType::String, "comma-separated layer names or indexes to draw (default: all visible)", false},
              {"scale", ParamType::Number, "zoom factor (default: fit small maps to about 512 px)", false},
              {"grid", ParamType::Bool, "draw cell lines", false, {}, false},
              {"objects", ParamType::Bool, "draw objects", false, {}, true},
              {"collision", ParamType::Bool, "draw collision grids", false, {}, true},
              {"max_size", ParamType::Int, "longest side of the PNG in px", false, {}, std::int64_t{1024}}},
             false, true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                RenderOptions o;
                const int big = std::max(doc.map.width * doc.map.tileWidth, doc.map.height * doc.map.tileHeight);
                o.scale = has(p, "scale") ? numberParam(p, "scale") : (big < 512 ? std::max(1.0, std::floor(512.0 / big)) : 1.0);
                o.grid = boolParam(p, "grid", false);
                o.objects = boolParam(p, "objects", true);
                o.collision = boolParam(p, "collision", true);
                if (has(p, "layers")) {
                    o.layers = splitList(stringParam(p, "layers"));
                    for (const auto& k : o.layers) {
                        if (doc.map.layerIndex(k) < 0) return Result::failure("no layer called '" + k + "'");
                    }
                }
                Image img = render(doc.map, o);
                Result r = edited("rendered " + std::to_string(img.width()) + "x" + std::to_string(img.height()) + " px", p);
                r.attach({"image/png", raster::previewPng(img, clampInt(intParam(p, "max_size", 1024), 16, 8192)), "level.png"});
                return r;
            }, false));
}

void registerTilesetCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"levels.tileset_add", "Add Tileset",
             "Slice an image (png, jpg, bmp, tga, gif, psd...) into tiles and add it as a tileset. Tiles are numbered "
             "from 0, left to right, top to bottom; margin is the border around the sheet and spacing the gap between "
             "tiles (as in Tiled). Sprite sheets from forge.sprite_sheet layout=grid padding=0 work directly.",
             {docParam(),
              {"path", ParamType::String, "tileset image", true},
              {"tile_width", ParamType::Int, "tile width in px (default: the map's)", false},
              {"tile_height", ParamType::Int, "tile height in px (default: the map's)", false},
              {"margin", ParamType::Int, "px around the edge of the image", false, {}, std::int64_t{0}},
              {"spacing", ParamType::Int, "px between tiles", false, {}, std::int64_t{0}},
              {"name", ParamType::String, "tileset name (default: the file name)", false},
              previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                Tileset t = loadTileset(stringParam(p, "path"), static_cast<int>(intParam(p, "tile_width", m.tileWidth)),
                                        static_cast<int>(intParam(p, "tile_height", m.tileHeight)), static_cast<int>(intParam(p, "margin", 0)),
                                        static_cast<int>(intParam(p, "spacing", 0)), stringParam(p, "name"));
                if (m.tileset(t.name)) return Result::failure("there is already a tileset called '" + t.name + "' (give name=)");
                Tileset& added = addTileset(m, std::move(t));
                Result r = edited("added tileset " + added.name + ": " + std::to_string(added.tileCount) + " tiles (" + std::to_string(added.columns) +
                                      " columns), gids " + std::to_string(added.firstGid) + ".." +
                                      std::to_string(added.firstGid + added.tileCount - 1),
                                  p);
                r.with("tileset", added.name)
                    .with("tiles", static_cast<std::int64_t>(added.tileCount))
                    .with("columns", static_cast<std::int64_t>(added.columns))
                    .with("first_gid", static_cast<std::int64_t>(added.firstGid));
                return r;
            }));

    reg.add({"levels.tileset_placeholder", "Placeholder Tileset",
             "Draw a placeholder tileset image, write it to `path` and add it to the map: style=colors gives one "
             "distinct colour per tile (with its id's bits as dots along the top); style=autotile gives the 16 tiles "
             "of the 4-bit auto-tile rule (tile n has a dark rim on each side whose neighbour bit is off: N=1 E=2 S=4 "
             "W=8), ready for levels.autotile base=<its first tile>. Good for prototyping before the art exists.",
             {docParam(),
              {"path", ParamType::String, "PNG to write", true},
              {"style", ParamType::String, "what to draw", false, {"colors", "autotile"}, std::string("colors")},
              {"tiles", ParamType::Int, "number of tiles (autotile: 16)", false, {}, std::int64_t{16}},
              {"columns", ParamType::Int, "tiles per row (default 8, autotile 4)", false},
              {"colors", ParamType::String, "comma-separated colours for the tiles (autotile: the body colour)", false},
              {"name", ParamType::String, "tileset name (default: the file name)", false},
              previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                const std::string style = stringParam(p, "style", "colors");
                const int tiles = style == "autotile" ? 16 : clampInt(intParam(p, "tiles", 16), 1, 4096);
                const int columns = clampInt(intParam(p, "columns", style == "autotile" ? 4 : std::min(tiles, 8)), 1, 256);
                std::vector<Color> colors;
                for (const auto& c : splitList(stringParam(p, "colors"))) {
                    auto col = Color::parse(c);
                    if (!col) return Result::failure("not a colour: " + c);
                    colors.push_back(*col);
                }
                const fs::path path = stringParam(p, "path");
                if (!raster::canWrite(path)) return Result::failure("write the tileset as .png");
                if (auto parent = path.parent_path(); !parent.empty()) {
                    std::error_code ec;
                    fs::create_directories(parent, ec);
                }
                raster::save(placeholderTileset(tiles, columns, m.tileWidth, m.tileHeight, style, colors), path);
                Tileset t = loadTileset(path, m.tileWidth, m.tileHeight, 0, 0, stringParam(p, "name"));
                t.tileCount = tiles;
                if (m.tileset(t.name)) return Result::failure("there is already a tileset called '" + t.name + "' (give name=)");
                Tileset& added = addTileset(m, std::move(t));
                Result r = edited("drew " + std::to_string(tiles) + " " + style + " tiles to " + path.string() + " and added tileset " + added.name +
                                      " (gids " + std::to_string(added.firstGid) + ".." + std::to_string(added.firstGid + added.tileCount - 1) + ")",
                                  p);
                r.with("tileset", added.name).with("tiles", static_cast<std::int64_t>(tiles)).with("first_gid", static_cast<std::int64_t>(added.firstGid)).with("path", path.string());
                return r;
            }));

    reg.add({"levels.tile_property", "Tile Property",
             "Set (or with an empty value, remove) a custom property on one tile of a tileset, e.g. solid=true for "
             "levels.collision solid=property, or damage=5. Saved in TMX/TMJ/JSON like Tiled's tile properties.",
             {docParam(), tileParam("tile", "tile id within the tileset"), tilesetParam(),
              {"key", ParamType::String, "property name", true},
              {"value", ParamType::String, "value; empty removes the property", false, {}, std::string()},
              {"value_type", ParamType::String, "property type (auto: from the value)", false, propertyTypes(), std::string("auto")}},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                if (m.tilesets.empty()) return Result::failure("the map has no tileset yet");
                Tileset* t = has(p, "tileset") ? m.tileset(stringParam(p, "tileset")) : &m.tilesets.front();
                if (!t) return Result::failure("no tileset called '" + stringParam(p, "tileset") + "'");
                const int tile = static_cast<int>(intParam(p, "tile"));
                if (tile < 0 || tile >= t->tileCount) return Result::failure("tileset " + t->name + " has tiles 0.." + std::to_string(t->tileCount - 1));
                const std::string key = stringParam(p, "key"), value = stringParam(p, "value");
                if (value.empty()) {
                    t->tileProps[tile].erase(key);
                    if (t->tileProps[tile].empty()) t->tileProps.erase(tile);
                    return edited("removed " + key + " from " + t->name + ":" + std::to_string(tile), p);
                }
                t->tileProps[tile][key] = makeProperty(p, value);
                return edited("set " + t->name + ":" + std::to_string(tile) + " " + key + "=" + value, p);
            }));
}

void registerLayerCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"levels.layer_add", "Add Layer",
             "Add a tile layer or an object layer (for entities: spawns, pickups, triggers). New layers go on top "
             "unless index is given (0 = bottom).",
             {docParam(),
              {"name", ParamType::String, "layer name", true},
              {"kind", ParamType::String, "what the layer holds", false, {"tiles", "objects"}, std::string("tiles")},
              {"index", ParamType::Int, "position, 0 = bottom (default: top)", false},
              {"collision", ParamType::Bool, "tile layers: mark as a collision grid (drawn as an overlay)", false, {}, false}},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (!validName(name)) return Result::failure("layer names can't be empty or contain quotes, <, > or &");
                if (doc.map.layer(name)) return Result::failure("there is already a layer called '" + name + "'");
                const LayerKind kind = stringParam(p, "kind", "tiles") == "objects" ? LayerKind::Objects : LayerKind::Tiles;
                Layer& l = doc.map.addLayer(name, kind, has(p, "index") ? static_cast<int>(intParam(p, "index")) : -1);
                l.collision = boolParam(p, "collision", false) && kind == LayerKind::Tiles;
                Result r = edited("added " + std::string(kind == LayerKind::Tiles ? "tile" : "object") + " layer " + name + " at index " +
                                      std::to_string(doc.map.layerIndex(name)),
                                  p);
                return r.with("layer", name).with("index", static_cast<std::int64_t>(doc.map.layerIndex(name)));
            }));

    reg.add({"levels.layer_remove", "Remove Layer", "Delete a layer and everything on it.",
             {docParam(), {"layer", ParamType::String, "layer name or index", true}}, true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                const int i = doc.map.layerIndex(stringParam(p, "layer"));
                if (i < 0) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                const std::string name = doc.map.layers[static_cast<std::size_t>(i)].name;
                doc.map.layers.erase(doc.map.layers.begin() + i);
                return edited("removed layer " + name, p);
            }));

    reg.add({"levels.layer_rename", "Rename Layer", "Rename a layer.",
             {docParam(), {"layer", ParamType::String, "layer name or index", true}, {"name", ParamType::String, "new name", true}}, true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Layer* l = doc.map.layer(stringParam(p, "layer"));
                if (!l) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                const std::string name = stringParam(p, "name");
                if (!validName(name)) return Result::failure("layer names can't be empty or contain quotes, <, > or &");
                if (const Layer* other = doc.map.layer(name); other && other != l) return Result::failure("there is already a layer called '" + name + "'");
                const std::string old = l->name;
                l->name = name;
                return edited("renamed " + old + " to " + name, p);
            }));

    reg.add({"levels.layer_move", "Reorder Layer",
             "Move a layer in the stack: to an index (0 = bottom) or up, down, top or bottom. Layers draw bottom first.",
             {docParam(), {"layer", ParamType::String, "layer name or index", true},
              {"index", ParamType::Int, "new position, 0 = bottom", false},
              {"direction", ParamType::String, "instead of index", false, {"up", "down", "top", "bottom"}}},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                auto& layers = doc.map.layers;
                const int from = doc.map.layerIndex(stringParam(p, "layer"));
                if (from < 0) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                const int last = static_cast<int>(layers.size()) - 1;
                int to = from;
                if (has(p, "index")) to = clampInt(intParam(p, "index"), 0, last);
                else if (has(p, "direction")) {
                    const std::string d = stringParam(p, "direction");
                    to = d == "up" ? std::min(last, from + 1) : d == "down" ? std::max(0, from - 1) : d == "top" ? last : 0;
                } else {
                    return Result::failure("give index= or direction=");
                }
                Layer l = std::move(layers[static_cast<std::size_t>(from)]);
                layers.erase(layers.begin() + from);
                layers.insert(layers.begin() + to, std::move(l));
                std::string order;
                for (const auto& x : layers) order += (order.empty() ? "" : ", ") + x.name;
                Result r = edited("moved " + layers[static_cast<std::size_t>(to)].name + " to index " + std::to_string(to) + " (bottom to top: " + order + ")", p);
                return r.with("index", static_cast<std::int64_t>(to));
            }));

    reg.add({"levels.layer_set", "Layer Settings", "Change a layer's visibility, opacity or collision flag.",
             {docParam(), {"layer", ParamType::String, "layer name or index", true},
              {"visible", ParamType::Bool, "shown in renders and marked visible in exports", false},
              {"opacity", ParamType::Number, "0..1", false},
              {"collision", ParamType::Bool, "tile layers: treat as a collision grid", false}, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Layer* l = doc.map.layer(stringParam(p, "layer"));
                if (!l) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                if (has(p, "visible")) l->visible = boolParam(p, "visible");
                if (has(p, "opacity")) l->opacity = std::clamp(numberParam(p, "opacity"), 0.0, 1.0);
                if (has(p, "collision")) l->collision = boolParam(p, "collision") && l->kind == LayerKind::Tiles;
                return edited("layer " + layerLine(doc.map, static_cast<std::size_t>(doc.map.layerIndex(l->name))), p);
            }));

    reg.add({"levels.property_set", "Set Property",
             "Set (or with an empty value, remove) a custom property on the map, or on a layer when layer= is given. "
             "Object properties use levels.object_set; tile properties levels.tile_property.",
             {docParam(), {"key", ParamType::String, "property name", true},
              {"value", ParamType::String, "value; empty removes it", false, {}, std::string()},
              {"layer", ParamType::String, "layer name or index (default: the map itself)", false},
              {"value_type", ParamType::String, "property type (auto: from the value)", false, propertyTypes(), std::string("auto")}},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Properties* props = &doc.map.props;
                std::string where = "map";
                if (has(p, "layer")) {
                    Layer* l = doc.map.layer(stringParam(p, "layer"));
                    if (!l) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                    props = &l->props;
                    where = "layer " + l->name;
                }
                const std::string key = stringParam(p, "key"), value = stringParam(p, "value");
                if (value.empty()) {
                    props->erase(key);
                    return edited("removed " + key + " from " + where, p);
                }
                (*props)[key] = makeProperty(p, value);
                return edited("set " + key + "=" + value + " on " + where + " (" + (*props)[key].type + ")", p);
            }));
}

void registerPaintCommands(Session& session) {
    auto& reg = session.registry();
    const ParamSpec flipH{"flip_h", ParamType::Bool, "mirror the tile horizontally", false, {}, false};
    const ParamSpec flipV{"flip_v", ParamType::Bool, "mirror the tile vertically", false, {}, false};

    reg.add({"levels.paint", "Paint Tile",
             "Put a tile in one cell (x = column, y = row, from the top-left), or in many: cells=x1,y1,x2,y2,...",
             {docParam(), layerParam(), {"x", ParamType::Int, "column", false}, {"y", ParamType::Int, "row", false},
              {"cells", ParamType::NumberList, "x,y pairs instead of x and y", false}, tileParam(), tilesetParam(), flipH, flipV,
              previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                Layer* l = tileLayer(m, p, error);
                std::uint32_t gid = 0;
                if (!l || !gidParam(m, p, "tile", gid, error)) return Result::failure(error);
                NumberList cells = listParam(p, "cells");
                if (has(p, "x") || has(p, "y")) {
                    if (!has(p, "x") || !has(p, "y")) return Result::failure("give both x and y");
                    cells.push_back(static_cast<double>(intParam(p, "x")));
                    cells.push_back(static_cast<double>(intParam(p, "y")));
                }
                if (cells.empty() || cells.size() % 2) return Result::failure("give x and y, or cells=x1,y1,x2,y2,...");
                int changed = 0, outside = 0;
                for (std::size_t i = 0; i + 1 < cells.size(); i += 2) {
                    const int x = static_cast<int>(cells[i]), y = static_cast<int>(cells[i + 1]);
                    if (!m.inside(x, y)) {
                        ++outside;
                        continue;
                    }
                    if (m.at(*l, x, y) != gid) ++changed;
                    m.set(*l, x, y, gid);
                }
                if (outside && outside * 2 == static_cast<int>(cells.size())) {
                    return Result::failure("outside the " + std::to_string(m.width) + "x" + std::to_string(m.height) + " map (cells are 0.." +
                                           std::to_string(m.width - 1) + ", 0.." + std::to_string(m.height - 1) + ")");
                }
                Result r = edited("painted " + tileName(m, gid) + " in " + plural(cells.size() / 2 - static_cast<std::size_t>(outside), "cell") + " of " + l->name +
                                      (outside ? " (" + std::to_string(outside) + " outside the map skipped)" : ""),
                                  p, changed);
                return r;
            }));

    reg.add({"levels.fill_rect", "Fill Rectangle", "Fill a rectangle of cells with a tile (outline=true for just its border, e.g. walls).",
             {docParam(), layerParam(), {"x", ParamType::Int, "left column", true}, {"y", ParamType::Int, "top row", true},
              {"width", ParamType::Int, "cells across", true}, {"height", ParamType::Int, "cells down", true}, tileParam(), tilesetParam(),
              {"outline", ParamType::Bool, "only the border", false, {}, false}, flipH, flipV, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                Layer* l = tileLayer(m, p, error);
                std::uint32_t gid = 0;
                if (!l || !gidParam(m, p, "tile", gid, error)) return Result::failure(error);
                const CellRect rect{static_cast<int>(intParam(p, "x")), static_cast<int>(intParam(p, "y")), static_cast<int>(intParam(p, "width")),
                                    static_cast<int>(intParam(p, "height"))};
                if (rect.w <= 0 || rect.h <= 0) return Result::failure("width and height must be positive");
                const int n = fillRect(m, *l, rect, gid, boolParam(p, "outline", false));
                return edited("filled " + std::to_string(rect.w) + "x" + std::to_string(rect.h) + (boolParam(p, "outline", false) ? " outline" : "") +
                                  " with " + tileName(m, gid) + " (" + plural(static_cast<std::size_t>(n), "cell") + " changed)",
                              p, n);
            }));

    reg.add({"levels.flood_fill", "Flood Fill",
             "Bucket fill: replace the connected area of cells matching the tile at (x, y) (empty counts too) with a tile.",
             {docParam(), layerParam(), {"x", ParamType::Int, "column", true}, {"y", ParamType::Int, "row", true}, tileParam(), tilesetParam(),
              {"diagonal", ParamType::Bool, "spread across corners too", false, {}, false}, flipH, flipV, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                Layer* l = tileLayer(m, p, error);
                std::uint32_t gid = 0;
                if (!l || !gidParam(m, p, "tile", gid, error)) return Result::failure(error);
                const int x = static_cast<int>(intParam(p, "x")), y = static_cast<int>(intParam(p, "y"));
                if (!m.inside(x, y)) return Result::failure("(" + std::to_string(x) + ", " + std::to_string(y) + ") is outside the map");
                const std::string was = tileName(m, m.at(*l, x, y));
                const int n = floodFill(m, *l, x, y, gid, boolParam(p, "diagonal", false));
                return edited("filled " + plural(static_cast<std::size_t>(n), "cell") + " of " + was + " with " + tileName(m, gid), p, n);
            }));

    reg.add({"levels.line", "Tile Line", "Draw a straight line of tiles from (x1, y1) to (x2, y2), both ends included.",
             {docParam(), layerParam(), {"x1", ParamType::Int, "start column", true}, {"y1", ParamType::Int, "start row", true},
              {"x2", ParamType::Int, "end column", true}, {"y2", ParamType::Int, "end row", true}, tileParam(), tilesetParam(), flipH, flipV,
              previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                Layer* l = tileLayer(m, p, error);
                std::uint32_t gid = 0;
                if (!l || !gidParam(m, p, "tile", gid, error)) return Result::failure(error);
                const int n = drawLine(m, *l, static_cast<int>(intParam(p, "x1")), static_cast<int>(intParam(p, "y1")),
                                       static_cast<int>(intParam(p, "x2")), static_cast<int>(intParam(p, "y2")), gid);
                return edited("drew a line of " + tileName(m, gid) + " (" + plural(static_cast<std::size_t>(n), "cell") + " changed)", p, n);
            }));

    reg.add({"levels.erase", "Erase Tiles", "Empty one cell, a rectangle (width/height), or the whole layer (all=true).",
             {docParam(), layerParam(), {"x", ParamType::Int, "column", false}, {"y", ParamType::Int, "row", false},
              {"width", ParamType::Int, "cells across", false, {}, std::int64_t{1}}, {"height", ParamType::Int, "cells down", false, {}, std::int64_t{1}},
              {"all", ParamType::Bool, "clear the whole layer", false, {}, false}, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                Layer* l = tileLayer(m, p, error);
                if (!l) return Result::failure(error);
                CellRect r{0, 0, m.width, m.height};
                if (!boolParam(p, "all", false)) {
                    if (!has(p, "x") || !has(p, "y")) return Result::failure("give x and y (and width/height), or all=true");
                    r = {static_cast<int>(intParam(p, "x")), static_cast<int>(intParam(p, "y")), static_cast<int>(intParam(p, "width", 1)),
                         static_cast<int>(intParam(p, "height", 1))};
                }
                const int n = fillRect(m, *l, r, 0);
                return edited("erased " + plural(static_cast<std::size_t>(n), "cell") + " on " + l->name, p, n);
            }));

    reg.add({"levels.stamp", "Stamp Region",
             "Copy a block of tiles to (x, y): either a region of a layer (from_x, from_y, width, height; from_layer "
             "and from_doc default to this layer and level) or a block straight from the tileset image "
             "(tileset_rect=column,row,width,height in tiles, e.g. a 2x3 tree). Empty source cells are skipped unless "
             "skip_empty=false.",
             {docParam(), layerParam("tile layer to stamp onto (default: the top tile layer)"),
              {"x", ParamType::Int, "destination column", true}, {"y", ParamType::Int, "destination row", true},
              {"from_doc", ParamType::String, "level to copy from (default: this one)", false},
              {"from_layer", ParamType::String, "layer to copy from (default: the destination layer)", false},
              {"from_x", ParamType::Int, "source left column", false}, {"from_y", ParamType::Int, "source top row", false},
              {"width", ParamType::Int, "block width in cells", false}, {"height", ParamType::Int, "block height in cells", false},
              {"tileset_rect", ParamType::NumberList, "column,row,width,height of a block of tiles in the tileset image", false}, tilesetParam(),
              {"skip_empty", ParamType::Bool, "leave cells under empty source cells alone", false, {}, true}, previewParam()},
             true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                Session* s = &session;
                return levelCommand(session, [s](LevelDoc& doc, const Params& q) {
                    Map& m = doc.map;
                    std::string error;
                    Layer* l = tileLayer(m, q, error);
                    if (!l) return Result::failure(error);
                    std::vector<std::uint32_t> block;
                    int w = 0, h = 0;
                    if (has(q, "tileset_rect")) {
                        const NumberList tr = listParam(q, "tileset_rect");
                        if (tr.size() != 4) return Result::failure("tileset_rect is column,row,width,height");
                        if (m.tilesets.empty()) return Result::failure("the map has no tileset");
                        const Tileset* t = has(q, "tileset") ? m.tileset(stringParam(q, "tileset")) : &m.tilesets.front();
                        if (!t) return Result::failure("no tileset called '" + stringParam(q, "tileset") + "'");
                        const int c0 = static_cast<int>(tr[0]), r0 = static_cast<int>(tr[1]);
                        w = static_cast<int>(tr[2]);
                        h = static_cast<int>(tr[3]);
                        if (w <= 0 || h <= 0 || c0 < 0 || r0 < 0 || c0 + w > t->columns) return Result::failure("that block isn't inside the tileset's " + std::to_string(t->columns) + " columns");
                        for (int j = 0; j < h; ++j) {
                            for (int i = 0; i < w; ++i) {
                                const int id = (r0 + j) * t->columns + c0 + i;
                                if (id >= t->tileCount) return Result::failure("that block runs past the tileset's last tile");
                                block.push_back(t->firstGid + static_cast<std::uint32_t>(id));
                            }
                        }
                    } else {
                        const Map* srcMap = &m;
                        std::shared_ptr<LevelDoc> other;
                        if (has(q, "from_doc") && stringParam(q, "from_doc") != stringParam(q, "doc")) {
                            other = s->workspace().getAs<LevelDoc>(stringParam(q, "from_doc"));
                            if (!other) return Result::failure("no open level called '" + stringParam(q, "from_doc") + "'");
                            srcMap = &other->map;
                        }
                        const Layer* src = l;
                        if (has(q, "from_layer")) src = srcMap->layer(stringParam(q, "from_layer"));
                        else if (other) src = srcMap->layer(l->name);
                        if (!src || src->kind != LayerKind::Tiles) return Result::failure("no tile layer to copy from (give from_layer=)");
                        if (!has(q, "from_x") || !has(q, "from_y") || !has(q, "width") || !has(q, "height")) {
                            return Result::failure("give from_x, from_y, width and height (or tileset_rect)");
                        }
                        w = static_cast<int>(intParam(q, "width"));
                        h = static_cast<int>(intParam(q, "height"));
                        if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > 16LL * 1024 * 1024) return Result::failure("bad block size");
                        block = copyRegion(*srcMap, *src, {static_cast<int>(intParam(q, "from_x")), static_cast<int>(intParam(q, "from_y")), w, h});
                        if (srcMap != &m) {
                            // gids only mean the same thing when the tilesets line up
                            for (auto& g : block) {
                                const Tileset* t = srcMap->tilesetFor(g);
                                if (!t) continue;
                                const Tileset* mine = m.tileset(t->name);
                                if (mine) g = (g & ~kGidMask) | (mine->firstGid + ((g & kGidMask) - t->firstGid));
                            }
                        }
                    }
                    const int n = stamp(m, *l, static_cast<int>(intParam(q, "x")), static_cast<int>(intParam(q, "y")), w, h, block,
                                        boolParam(q, "skip_empty", true));
                    return edited("stamped a " + std::to_string(w) + "x" + std::to_string(h) + " block at " + std::to_string(intParam(q, "x")) + "," +
                                      std::to_string(intParam(q, "y")) + " (" + plural(static_cast<std::size_t>(n), "cell") + " changed)",
                                  q, n);
                })(p);
            });
}

void registerGeneratorCommands(Session& session) {
    auto& reg = session.registry();

    {
        std::vector<ParamSpec> ps{docParam(), layerParam("tile layer holding the terrain (default: the top tile layer)"),
                                  {"target", ParamType::String, "layer to write the auto-tiles to (default: the same layer)", false},
                                  {"base", ParamType::Int, "tile id of the first of the 16 auto-tiles (mask 0)", false, {}, std::int64_t{0}},
                                  tilesetParam(),
                                  {"match", ParamType::NumberList, "tile ids that count as terrain (default: any non-empty cell)", false},
                                  {"edges", ParamType::String, "whether cells beyond the map count as terrain", false, {"solid", "empty"}, std::string("solid")}};
        for (auto& r : regionParams()) ps.push_back(r);
        ps.push_back(previewParam());
        reg.add({"levels.autotile", "Auto-tile",
                 "Pick the right edge/corner tile for every terrain cell with the 16-tile (4-bit) rule: mask = N 1 + E 2 "
                 "+ S 4 + W 8 for each side whose neighbour is terrain too, and the cell becomes tile base + mask. Lay "
                 "your 16 tiles out in that order (levels.tileset_placeholder style=autotile draws a template). Paint "
                 "rough terrain with any tile, then run this; the result can go to another layer (target=).",
                 ps, true},
                levelCommand(session, [](LevelDoc& doc, const Params& p) {
                    Map& m = doc.map;
                    std::string error;
                    Layer* src = tileLayer(m, p, error);
                    if (!src) return Result::failure(error);
                    Layer* dst = has(p, "target") ? tileLayer(m, p, error, "target") : src;
                    if (!dst) {
                        if (m.layer(stringParam(p, "target"))) return Result::failure(error);
                        const int si = m.layerIndex(src->name);
                        dst = &m.addLayer(stringParam(p, "target"), LayerKind::Tiles, si + 1);
                        src = &m.layers[static_cast<std::size_t>(si)];
                    }
                    std::uint32_t base = 0;
                    if (!gidFor(m, intParam(p, "base", 0), p, base, error)) return Result::failure(error);
                    if (const Tileset* t = m.tilesetFor(base); t && !t->contains(base + 15)) {
                        return Result::failure("base " + std::to_string(intParam(p, "base", 0)) + " leaves fewer than 16 tiles in " + t->name);
                    }
                    std::set<std::uint32_t> match;
                    std::vector<std::uint32_t> list;
                    if (!gidList(m, listParam(p, "match"), p, list, error)) return Result::failure(error);
                    match.insert(list.begin(), list.end());
                    auto isTerrain = [&](std::uint32_t g) {
                        g &= kGidMask;
                        if (!g) return false;
                        if (match.empty()) return true;
                        return match.count(g) > 0 || (g >= base && g < base + 16);  // already auto-tiled cells stay terrain
                    };
                    const int n = autotile(m, *src, *dst, isTerrain, base, regionParam(m, p), stringParam(p, "edges", "solid") == "solid");
                    return edited("auto-tiled " + plural(static_cast<std::size_t>(n), "cell") + " on " + dst->name + " with tiles " + tileName(m, base) +
                                      ".." + tileName(m, base + 15),
                                  p, n);
                }));
    }

    {
        std::vector<ParamSpec> ps{docParam(), layerParam(),
                                  {"tiles", ParamType::NumberList, "tile ids to scatter, e.g. 4,5,6", true},
                                  {"weights", ParamType::NumberList, "relative chance of each tile (default equal)", false}, tilesetParam(),
                                  {"density", ParamType::Number, "chance per allowed cell, 0..1", false, {}, 0.1},
                                  {"count", ParamType::Int, "place exactly this many instead of density", false},
                                  {"seed", ParamType::Int, "random seed: same seed, same result", false, {}, std::int64_t{1}},
                                  {"only_empty", ParamType::Bool, "only cells that are empty on this layer", false, {}, true},
                                  {"on_layer", ParamType::String, "only cells where this layer has a tile...", false},
                                  {"on_tiles", ParamType::NumberList, "...one of these tile ids (default: any)", false},
                                  {"avoid_layer", ParamType::String, "skip cells where this layer has a tile (e.g. walls)", false}};
        for (auto& r : regionParams()) ps.push_back(r);
        ps.push_back(previewParam());
        reg.add({"levels.scatter", "Random Scatter",
                 "Sprinkle tiles at random (flowers on grass, rocks, treasure): by density or an exact count, optionally "
                 "weighted, only on empty cells, only on top of some tiles of another layer, and away from another "
                 "layer. Deterministic for a seed.",
                 ps, true},
                levelCommand(session, [](LevelDoc& doc, const Params& p) {
                    Map& m = doc.map;
                    std::string error;
                    Layer* l = tileLayer(m, p, error);
                    if (!l) return Result::failure(error);
                    std::vector<std::uint32_t> gids;
                    if (!gidList(m, listParam(p, "tiles"), p, gids, error)) return Result::failure(error);
                    if (gids.empty()) return Result::failure("give tiles=a,b,c");
                    const Layer* on = nullptr;
                    const Layer* avoid = nullptr;
                    if (has(p, "on_layer") && !(on = m.layer(stringParam(p, "on_layer")))) return Result::failure("no layer called '" + stringParam(p, "on_layer") + "'");
                    if (has(p, "avoid_layer") && !(avoid = m.layer(stringParam(p, "avoid_layer")))) return Result::failure("no layer called '" + stringParam(p, "avoid_layer") + "'");
                    std::vector<std::uint32_t> onList;
                    // on_tiles are ids in the tileset of on_layer's tiles; they use the same tileset= param
                    if (!gidList(m, listParam(p, "on_tiles"), p, onList, error)) return Result::failure(error);
                    std::set<std::uint32_t> onSet(onList.begin(), onList.end());
                    const bool onlyEmpty = boolParam(p, "only_empty", true);
                    const int li = m.layerIndex(l->name);
                    auto allowed = [&](int x, int y) {
                        const Layer& self = m.layers[static_cast<std::size_t>(li)];
                        if (onlyEmpty && (m.at(self, x, y) & kGidMask)) return false;
                        if (on && on->kind == LayerKind::Tiles) {
                            const std::uint32_t g = m.at(*on, x, y) & kGidMask;
                            if (!g || (!onSet.empty() && !onSet.count(g))) return false;
                        }
                        if (avoid && avoid->kind == LayerKind::Tiles && (m.at(*avoid, x, y) & kGidMask)) return false;
                        return true;
                    };
                    const int n = scatter(m, *l, gids, listParam(p, "weights"), regionParam(m, p), std::clamp(numberParam(p, "density", 0.1), 0.0, 1.0),
                                          static_cast<int>(std::max<std::int64_t>(0, intParam(p, "count", 0))),
                                          static_cast<std::uint64_t>(intParam(p, "seed", 1)), allowed);
                    return edited("scattered " + plural(static_cast<std::size_t>(n), "tile") + " on " + l->name, p, n);
                }));
    }

    const ParamSpec seed{"seed", ParamType::Int, "random seed: same seed, same map", false, {}, std::int64_t{1}};
    reg.add({"levels.gen_caves", "Generate Caves",
             "Fill a layer with organic caves from cellular automata: random walls (fill), then smoothing steps where "
             "a cell becomes wall when most neighbours are. connect=true walls off pockets that can't reach the main "
             "cave. Writes wall and floor tiles (floor=-1 leaves floor cells empty); follow with levels.autotile for "
             "edges. Returns the floor cell count.",
             {docParam(), layerParam(), tileParam("wall", "tile id for walls"), tileParam("floor", "tile id for floor (-1 = empty)", false),
              tilesetParam(), {"fill", ParamType::Number, "starting wall fraction 0..1", false, {}, 0.45},
              {"steps", ParamType::Int, "smoothing passes", false, {}, std::int64_t{5}},
              {"connect", ParamType::Bool, "keep only the largest connected cave", false, {}, true}, seed, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                Layer* l = tileLayer(m, p, error);
                std::uint32_t wall = 0, floor = 0;
                if (!l || !gidFor(m, intParam(p, "wall"), p, wall, error) || !gidFor(m, intParam(p, "floor", -1), p, floor, error)) return Result::failure(error);
                CaveOptions o;
                o.fill = std::clamp(numberParam(p, "fill", 0.45), 0.0, 1.0);
                o.steps = clampInt(intParam(p, "steps", 5), 0, 50);
                o.connect = boolParam(p, "connect", true);
                o.seed = static_cast<std::uint64_t>(intParam(p, "seed", 1));
                const auto grid = caves(m.width, m.height, o);
                std::size_t floors = 0;
                for (std::size_t i = 0; i < grid.size(); ++i) {
                    l->cells[i] = grid[i] ? wall : floor;
                    floors += grid[i] ? 0 : 1;
                }
                Result r = edited("generated caves on " + l->name + ": " + std::to_string(floors) + " floor cells (" +
                                      std::to_string(grid.empty() ? 0 : floors * 100 / grid.size()) + "%)",
                                  p);
                return r.with("floor_cells", static_cast<std::int64_t>(floors));
            }));

    reg.add({"levels.gen_rooms", "Generate Rooms",
             "Dungeon rooms and corridors by binary space partitioning: the map is split recursively, each leaf gets a "
             "room, and sibling rooms are joined by L-shaped corridors so every room is reachable. With objects_layer, "
             "each room is added as a 'room' object plus a 'spawn' point in the first room and an 'exit' in the last.",
             {docParam(), layerParam(), tileParam("wall", "tile id for walls (-1 = empty)"), tileParam("floor", "tile id for floor (-1 = empty)"),
              tilesetParam(), {"min_room", ParamType::Int, "smallest room side in cells", false, {}, std::int64_t{4}},
              {"max_room", ParamType::Int, "largest room side in cells", false, {}, std::int64_t{10}},
              {"corridor", ParamType::Int, "corridor width in cells", false, {}, std::int64_t{1}},
              {"padding", ParamType::Int, "minimum wall between neighbouring rooms' leaves", false, {}, std::int64_t{1}},
              {"objects_layer", ParamType::String, "object layer to add room/spawn/exit objects to (created if missing)", false}, seed,
              previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                Layer* l = tileLayer(m, p, error);
                std::uint32_t wall = 0, floor = 0;
                if (!l || !gidFor(m, intParam(p, "wall"), p, wall, error) || !gidFor(m, intParam(p, "floor"), p, floor, error)) return Result::failure(error);
                RoomOptions o;
                o.minRoom = clampInt(intParam(p, "min_room", 4), 2, 1000);
                o.maxRoom = clampInt(intParam(p, "max_room", 10), o.minRoom, 1000);
                o.corridorWidth = clampInt(intParam(p, "corridor", 1), 1, 10);
                o.padding = clampInt(intParam(p, "padding", 1), 0, 100);
                o.seed = static_cast<std::uint64_t>(intParam(p, "seed", 1));
                Rooms rooms = bspRooms(m.width, m.height, o);
                if (rooms.rooms.empty()) return Result::failure("no room fits; make the map bigger or min_room smaller");
                const std::string layerName = l->name;
                for (std::size_t i = 0; i < rooms.wall.size(); ++i) l->cells[i] = rooms.wall[i] ? wall : floor;
                if (has(p, "objects_layer")) {
                    Params q = p;
                    q["layer"] = stringParam(p, "objects_layer");
                    Layer* ol = objectLayer(m, q, error);
                    if (!ol) return Result::failure(error);
                    const double tw = m.tileWidth, th = m.tileHeight;
                    for (std::size_t i = 0; i < rooms.rooms.size(); ++i) {
                        const CellRect& r = rooms.rooms[i];
                        MapObject ob;
                        ob.id = m.nextObjectId++;
                        ob.name = "room" + std::to_string(i + 1);
                        ob.type = "room";
                        ob.x = r.x * tw;
                        ob.y = r.y * th;
                        ob.width = r.w * tw;
                        ob.height = r.h * th;
                        ol->objects.push_back(ob);
                    }
                    auto point = [&](const CellRect& r, const std::string& name) {
                        MapObject ob;
                        ob.id = m.nextObjectId++;
                        ob.name = name;
                        ob.type = name;
                        ob.shape = "point";
                        ob.x = (r.x + r.w / 2) * tw + tw / 2;
                        ob.y = (r.y + r.h / 2) * th + th / 2;
                        ol->objects.push_back(ob);
                    };
                    point(rooms.rooms.front(), "spawn");
                    point(rooms.rooms.back(), "exit");
                }
                Result r = edited("generated " + plural(rooms.rooms.size(), "room") + " on " + layerName, p);
                std::string list;
                for (const auto& rr : rooms.rooms) {
                    list += (list.empty() ? "" : "; ") + std::to_string(rr.x) + "," + std::to_string(rr.y) + " " + std::to_string(rr.w) + "x" + std::to_string(rr.h);
                }
                r.message += ": " + list;
                return r.with("rooms", static_cast<std::int64_t>(rooms.rooms.size()));
            }));

    reg.add({"levels.gen_noise", "Generate Noise Terrain",
             "Overworld terrain from fractal value noise (0..1, stretched to the full range): thresholds split it into "
             "bands and each band gets a tile, e.g. thresholds=0.35,0.45,0.75 tiles=water,sand,grass,mountain (as ids). "
             "tiles needs one more entry than thresholds; -1 leaves a band empty.",
             {docParam(), layerParam(), {"tiles", ParamType::NumberList, "tile id per band, lowest first", true},
              {"thresholds", ParamType::NumberList, "band boundaries in 0..1, ascending", true}, tilesetParam(),
              {"scale", ParamType::Number, "feature size in cells (bigger = broader)", false, {}, 16.0},
              {"octaves", ParamType::Int, "detail layers", false, {}, std::int64_t{4}},
              {"persistence", ParamType::Number, "how much each finer octave adds, 0..1", false, {}, 0.5}, seed, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                Layer* l = tileLayer(m, p, error);
                if (!l) return Result::failure(error);
                std::vector<std::uint32_t> gids;
                if (!gidList(m, listParam(p, "tiles"), p, gids, error)) return Result::failure(error);
                NumberList th = listParam(p, "thresholds");
                if (gids.size() != th.size() + 1) return Result::failure("give one more tile than thresholds (" + std::to_string(th.size()) + " thresholds -> " + std::to_string(th.size() + 1) + " tiles)");
                if (!std::is_sorted(th.begin(), th.end())) return Result::failure("thresholds must ascend");
                NoiseOptions o;
                o.scale = numberParam(p, "scale", 16);
                o.octaves = clampInt(intParam(p, "octaves", 4), 1, 10);
                o.persistence = std::clamp(numberParam(p, "persistence", 0.5), 0.0, 1.0);
                o.seed = static_cast<std::uint64_t>(intParam(p, "seed", 1));
                const auto v = noise(m.width, m.height, o);
                std::vector<std::size_t> bands(gids.size());
                for (std::size_t i = 0; i < v.size(); ++i) {
                    const std::size_t b = static_cast<std::size_t>(std::upper_bound(th.begin(), th.end(), v[i]) - th.begin());
                    l->cells[i] = gids[b];
                    bands[b]++;
                }
                Result r = edited("generated noise terrain on " + l->name, p);
                r.message += ":";
                for (std::size_t b = 0; b < bands.size(); ++b) r.message += " " + tileName(m, gids[b]) + " " + std::to_string(bands[b] * 100 / std::max<std::size_t>(1, v.size())) + "%";
                return r;
            }));
}

void registerObjectCommands(Session& session) {
    auto& reg = session.registry();
    const ParamSpec cellSpec{"cell", ParamType::NumberList, "column,row: place on a cell instead of x,y (size defaults to one tile)", false};
    const ParamSpec idSpec{"id", ParamType::Int, "object id (levels.object_list shows them)", true};

    reg.add({"levels.object_add", "Add Object",
             "Add an entity to an object layer: a rect (trigger zones, rooms), ellipse, or point (spawns). Position "
             "and size are in px from the map's top-left, or give cell=column,row. Custom properties as "
             "properties=\"hp=10; loot=gold\" (types inferred). The layer is created if it doesn't exist.",
             {docParam(), {"layer", ParamType::String, "object layer (default: the top one, or a new 'objects')", false},
              {"name", ParamType::String, "object name", false, {}, std::string()},
              {"type", ParamType::String, "object type/class, e.g. enemy, chest, spawn", false, {}, std::string()},
              {"x", ParamType::Number, "left in px", false}, {"y", ParamType::Number, "top in px", false}, cellSpec,
              {"width", ParamType::Number, "px (default: one tile; 0 for points)", false},
              {"height", ParamType::Number, "px (default: one tile; 0 for points)", false},
              {"shape", ParamType::String, "object shape", false, {"rect", "ellipse", "point"}, std::string("rect")},
              {"properties", ParamType::String, "custom properties: key=value; key2=value2", false}, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                Layer* l = objectLayer(m, p, error);
                if (!l) return Result::failure(error);
                MapObject ob;
                ob.name = stringParam(p, "name");
                ob.type = stringParam(p, "type");
                ob.shape = stringParam(p, "shape", "rect");
                const bool point = ob.shape == "point";
                if (has(p, "cell")) {
                    const NumberList c = listParam(p, "cell");
                    if (c.size() != 2) return Result::failure("cell is column,row");
                    ob.x = c[0] * m.tileWidth + (point ? m.tileWidth / 2.0 : 0);
                    ob.y = c[1] * m.tileHeight + (point ? m.tileHeight / 2.0 : 0);
                } else if (has(p, "x") && has(p, "y")) {
                    ob.x = numberParam(p, "x");
                    ob.y = numberParam(p, "y");
                } else {
                    return Result::failure("give x and y (px) or cell=column,row");
                }
                ob.width = point ? 0 : numberParam(p, "width", m.tileWidth);
                ob.height = point ? 0 : numberParam(p, "height", m.tileHeight);
                ob.props = parseProps(stringParam(p, "properties"));
                ob.id = m.nextObjectId++;
                l->objects.push_back(ob);
                Result r = edited("added object " + objectLine(ob, l->name), p);
                return r.with("id", static_cast<std::int64_t>(ob.id)).with("layer", l->name);
            }));

    reg.add({"levels.object_move", "Move Object", "Move an object to x,y (px) or cell=column,row, or by dx,dy px.",
             {docParam(), idSpec, {"x", ParamType::Number, "new left in px", false}, {"y", ParamType::Number, "new top in px", false}, cellSpec,
              {"dx", ParamType::Number, "move right by px", false}, {"dy", ParamType::Number, "move down by px", false},
              {"layer", ParamType::String, "also move it to this object layer", false}, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                Layer* owner = nullptr;
                MapObject* ob = m.object(static_cast<int>(intParam(p, "id")), &owner);
                if (!ob) return Result::failure("no object #" + std::to_string(intParam(p, "id")));
                const bool point = ob->shape == "point";
                if (has(p, "cell")) {
                    const NumberList c = listParam(p, "cell");
                    if (c.size() != 2) return Result::failure("cell is column,row");
                    ob->x = c[0] * m.tileWidth + (point ? m.tileWidth / 2.0 : 0);
                    ob->y = c[1] * m.tileHeight + (point ? m.tileHeight / 2.0 : 0);
                }
                if (has(p, "x")) ob->x = numberParam(p, "x");
                if (has(p, "y")) ob->y = numberParam(p, "y");
                ob->x += numberParam(p, "dx", 0);
                ob->y += numberParam(p, "dy", 0);
                if (has(p, "layer")) {
                    std::string error;
                    MapObject copy = *ob;
                    const int oi = m.layerIndex(owner->name);
                    Layer* to = objectLayer(m, p, error);
                    if (!to) return Result::failure(error);
                    Layer& from = m.layers[static_cast<std::size_t>(oi < 0 ? 0 : oi)];
                    if (to != &from) {
                        from.objects.erase(std::remove_if(from.objects.begin(), from.objects.end(), [&](const MapObject& o) { return o.id == copy.id; }),
                                           from.objects.end());
                        to->objects.push_back(copy);
                    }
                    ob = m.object(copy.id, &owner);
                }
                return edited("moved " + objectLine(*ob, owner->name), p).with("id", static_cast<std::int64_t>(ob->id));
            }));

    reg.add({"levels.object_set", "Set Object",
             "Change an object's name, type, size or shape, and/or set one custom property (key + value; an empty "
             "value removes it). properties=\"a=1; b=2\" sets several at once.",
             {docParam(), idSpec, {"name", ParamType::String, "new name", false}, {"type", ParamType::String, "new type", false},
              {"width", ParamType::Number, "px", false}, {"height", ParamType::Number, "px", false},
              {"shape", ParamType::String, "object shape", false, {"rect", "ellipse", "point"}},
              {"key", ParamType::String, "custom property name", false}, {"value", ParamType::String, "its value (empty removes it)", false},
              {"value_type", ParamType::String, "property type (auto: from the value)", false, propertyTypes(), std::string("auto")},
              {"properties", ParamType::String, "several properties: key=value; key2=value2", false}, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Layer* owner = nullptr;
                MapObject* ob = doc.map.object(static_cast<int>(intParam(p, "id")), &owner);
                if (!ob) return Result::failure("no object #" + std::to_string(intParam(p, "id")));
                if (has(p, "name")) ob->name = stringParam(p, "name");
                if (has(p, "type")) ob->type = stringParam(p, "type");
                if (has(p, "width")) ob->width = std::max(0.0, numberParam(p, "width"));
                if (has(p, "height")) ob->height = std::max(0.0, numberParam(p, "height"));
                if (has(p, "shape")) ob->shape = stringParam(p, "shape");
                if (has(p, "key")) {
                    const std::string v = stringParam(p, "value");
                    if (v.empty()) ob->props.erase(stringParam(p, "key"));
                    else ob->props[stringParam(p, "key")] = makeProperty(p, v);
                } else if (has(p, "value")) {
                    return Result::failure("value needs key=");
                }
                for (const auto& [k, v] : parseProps(stringParam(p, "properties"))) ob->props[k] = v;
                return edited("updated " + objectLine(*ob, owner->name), p).with("id", static_cast<std::int64_t>(ob->id));
            }));

    reg.add({"levels.object_remove", "Remove Object", "Delete an object.", {docParam(), idSpec, previewParam()}, true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Layer* owner = nullptr;
                const int id = static_cast<int>(intParam(p, "id"));
                MapObject* ob = doc.map.object(id, &owner);
                if (!ob) return Result::failure("no object #" + std::to_string(id));
                const std::string line = objectLine(*ob, owner->name);
                owner->objects.erase(std::remove_if(owner->objects.begin(), owner->objects.end(), [id](const MapObject& o) { return o.id == id; }),
                                     owner->objects.end());
                return edited("removed " + line, p);
            }));

    reg.add({"levels.object_list", "List Objects", "List objects with their ids, positions, sizes and properties, optionally of one type or layer.",
             {docParam(), {"layer", ParamType::String, "only this object layer", false}, {"type", ParamType::String, "only objects of this type", false}},
             false, true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                const Map& m = doc.map;
                std::string lines;
                std::size_t n = 0;
                std::string ids;
                for (const auto& l : m.layers) {
                    if (has(p, "layer") && m.layer(stringParam(p, "layer")) != &l) continue;
                    for (const auto& o : l.objects) {
                        if (has(p, "type") && o.type != stringParam(p, "type")) continue;
                        lines += "\n  " + objectLine(o, l.name);
                        ids += (ids.empty() ? "" : ",") + std::to_string(o.id);
                        ++n;
                    }
                }
                Result r = edited(plural(n, "object") + lines, p);
                return r.with("count", static_cast<std::int64_t>(n)).with("ids", ids);
            }, false));
}

void registerQueryCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"levels.collision", "Build Collision",
             "Build collision from tile layers: a cell is solid when a source layer has a solid tile there; solid=any "
             "(any tile), list (the ids in tiles=), or property (tiles with a property, default solid=true, set by "
             "levels.tile_property). output=objects writes merged rectangles (type 'collision') to an object layer, "
             "which engines load as static bodies; output=grid writes a 0/1 collision grid tile layer. The target "
             "layer is replaced.",
             {docParam(), {"from", ParamType::String, "comma-separated source layers (default: every non-collision tile layer)", false},
              {"solid", ParamType::String, "which tiles are solid", false, {"any", "list", "property"}, std::string("any")},
              {"tiles", ParamType::NumberList, "solid=list: the solid tile ids", false}, tilesetParam(),
              {"property", ParamType::String, "solid=property: the property that must be true", false, {}, std::string("solid")},
              {"output", ParamType::String, "what to write", false, {"objects", "grid"}, std::string("objects")},
              {"name", ParamType::String, "target layer name", false, {}, std::string("collision")}, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                std::string error;
                const std::string name = stringParam(p, "name", "collision");
                std::vector<const Layer*> sources;
                if (has(p, "from")) {
                    for (const auto& k : splitList(stringParam(p, "from"))) {
                        const Layer* l = m.layer(k);
                        if (!l || l->kind != LayerKind::Tiles) return Result::failure("no tile layer called '" + k + "'");
                        sources.push_back(l);
                    }
                } else {
                    for (const auto& l : m.layers) {
                        if (l.kind == LayerKind::Tiles && !l.collision && l.name != name) sources.push_back(&l);
                    }
                }
                if (sources.empty()) return Result::failure("no tile layers to build collision from");
                const std::string mode = stringParam(p, "solid", "any");
                std::vector<std::uint32_t> list;
                if (!gidList(m, listParam(p, "tiles"), p, list, error)) return Result::failure(error);
                if (mode == "list" && list.empty()) return Result::failure("solid=list needs tiles=");
                const std::set<std::uint32_t> listed(list.begin(), list.end());
                const std::string prop = stringParam(p, "property", "solid");
                auto solidTile = [&](std::uint32_t raw) {
                    const std::uint32_t g = raw & kGidMask;
                    if (!g) return false;
                    if (mode == "list") return listed.count(g) > 0;
                    if (mode == "property") {
                        const Properties* ps = m.tileProperties(g);
                        if (!ps) return false;
                        auto it = ps->find(prop);
                        return it != ps->end() && it->second.value != "false" && it->second.value != "0" && !it->second.value.empty();
                    }
                    return true;
                };
                std::vector<bool> solid(static_cast<std::size_t>(m.width) * m.height, false);
                std::size_t count = 0;
                for (std::size_t i = 0; i < solid.size(); ++i) {
                    for (const Layer* l : sources) {
                        if (solidTile(l->cells[i])) {
                            solid[i] = true;
                            ++count;
                            break;
                        }
                    }
                }
                // replace the target layer, keeping its place in the stack
                int at = m.layerIndex(name);
                if (at >= 0 && m.layers[static_cast<std::size_t>(at)].name == name) m.layers.erase(m.layers.begin() + at);
                else at = -1;
                Result r = edited("", p);
                if (stringParam(p, "output", "objects") == "grid") {
                    Layer& l = m.addLayer(name, LayerKind::Tiles, at);
                    l.collision = true;
                    for (std::size_t i = 0; i < solid.size(); ++i) l.cells[i] = solid[i] ? 1u : 0u;
                    r.message = "built collision grid " + name + ": " + plural(count, "solid cell");
                } else {
                    Layer& l = m.addLayer(name, LayerKind::Objects, at);
                    const auto rects = mergeRects(m.width, m.height, solid);
                    for (const auto& c : rects) {
                        MapObject ob;
                        ob.id = m.nextObjectId++;
                        ob.type = "collision";
                        ob.x = c.x * m.tileWidth;
                        ob.y = c.y * m.tileHeight;
                        ob.width = c.w * m.tileWidth;
                        ob.height = c.h * m.tileHeight;
                        l.objects.push_back(ob);
                    }
                    r.message = "built " + plural(rects.size(), "collision rectangle") + " covering " + plural(count, "solid cell") + " on " + name;
                    r.with("rectangles", static_cast<std::int64_t>(rects.size()));
                }
                return r.with("solid_cells", static_cast<std::int64_t>(count)).with("layer", name);
            }));

    reg.add({"levels.find", "Find Tiles",
             "Find the cells holding a tile: count and positions (x,y), on one layer or all tile layers.",
             {docParam(), tileParam("tile", "tile id within the tileset (-1 = empty cells)"), tilesetParam(),
              {"layer", ParamType::String, "only this layer (default: all tile layers)", false},
              {"limit", ParamType::Int, "most positions to list", false, {}, std::int64_t{100}}},
             false, true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                const Map& m = doc.map;
                std::string error;
                std::uint32_t gid = 0;
                if (!gidFor(m, intParam(p, "tile"), p, gid, error)) return Result::failure(error);
                const Layer* only = nullptr;
                if (has(p, "layer") && !(only = m.layer(stringParam(p, "layer")))) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                const std::size_t limit = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "limit", 100)));
                std::size_t total = 0;
                std::string text, positions;
                NumberList cells;
                for (const auto& l : m.layers) {
                    if (l.kind != LayerKind::Tiles || (only && only != &l)) continue;
                    std::size_t n = 0;
                    for (int y = 0; y < m.height; ++y) {
                        for (int x = 0; x < m.width; ++x) {
                            if ((m.at(l, x, y) & kGidMask) != gid) continue;
                            ++n;
                            if (cells.size() / 2 < limit) {
                                cells.push_back(x);
                                cells.push_back(y);
                                positions += (positions.empty() ? "" : " ") + std::to_string(x) + "," + std::to_string(y);
                            }
                        }
                    }
                    if (n) text += "\n  " + l.name + ": " + std::to_string(n);
                    total += n;
                }
                Result r = edited(std::to_string(total) + " cells of " + tileName(m, gid) + text +
                                      (positions.empty() ? "" : "\npositions: " + positions + (total > limit ? " ..." : "")),
                                  p);
                return r.with("count", static_cast<std::int64_t>(total)).with("cells", cells);
            }, false));

    reg.add({"levels.count", "Count Tiles", "How many cells use each tile, per tile layer (a tile histogram).",
             {docParam(), {"layer", ParamType::String, "only this layer (default: all tile layers)", false}}, false, true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                const Map& m = doc.map;
                const Layer* only = nullptr;
                if (has(p, "layer") && !(only = m.layer(stringParam(p, "layer")))) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                std::string text;
                std::size_t filled = 0, distinctTotal = 0;
                for (const auto& l : m.layers) {
                    if (l.kind != LayerKind::Tiles || (only && only != &l)) continue;
                    std::map<std::uint32_t, std::size_t> hist;
                    for (auto c : l.cells) {
                        if (c & kGidMask) hist[c & kGidMask]++;
                    }
                    std::vector<std::pair<std::uint32_t, std::size_t>> sorted(hist.begin(), hist.end());
                    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
                    std::size_t n = 0;
                    for (auto& [g, c] : sorted) n += c;
                    filled += n;
                    distinctTotal += sorted.size();
                    text += "\n" + l.name + ": " + std::to_string(n) + " filled, " + std::to_string(l.cells.size() - n) + " empty";
                    for (auto& [g, c] : sorted) text += "\n  " + tileName(m, g) + " x" + std::to_string(c);
                }
                Result r = edited(plural(filled, "filled cell") + text, p);
                return r.with("filled", static_cast<std::int64_t>(filled)).with("distinct", static_cast<std::int64_t>(distinctTotal));
            }, false));

    reg.add({"levels.cell", "Inspect Cell", "What is at a cell: the tile on every tile layer (with tile properties) and the objects that cover it.",
             {docParam(), {"x", ParamType::Int, "column", true}, {"y", ParamType::Int, "row", true}}, false, true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                const Map& m = doc.map;
                const int x = static_cast<int>(intParam(p, "x")), y = static_cast<int>(intParam(p, "y"));
                if (!m.inside(x, y)) return Result::failure("(" + std::to_string(x) + ", " + std::to_string(y) + ") is outside the map");
                std::string text = "cell " + std::to_string(x) + "," + std::to_string(y);
                Result r = edited("", p);
                for (const auto& l : m.layers) {
                    if (l.kind == LayerKind::Tiles) {
                        const std::uint32_t g = m.at(l, x, y);
                        text += "\n  " + l.name + ": " + tileName(m, g);
                        if (const Properties* ps = m.tileProperties(g)) text += " {" + propsText(*ps) + "}";
                        r.with(l.name, localId(m, g));
                    } else {
                        const double px = (x + 0.5) * m.tileWidth, py = (y + 0.5) * m.tileHeight;
                        for (const auto& o : l.objects) {
                            const bool hit = o.shape == "point" ? (o.x >= x * m.tileWidth && o.x < (x + 1) * m.tileWidth && o.y >= y * m.tileHeight && o.y < (y + 1) * m.tileHeight)
                                                                : (px >= o.x && px < o.x + o.width && py >= o.y && py < o.y + o.height);
                            if (hit) text += "\n  " + objectLine(o, l.name);
                        }
                    }
                }
                r.message = text;
                return r;
            }, false));

    reg.add({"levels.resize", "Resize Map",
             "Change the map size in cells. anchor says where the old content sits in the new map (top-left keeps it "
             "at 0,0; center centres it); offset_x/offset_y shift it by whole cells instead. Objects move with the "
             "tiles; cells pushed off the edge are lost.",
             {docParam(), {"width", ParamType::Int, "new width in cells", true}, {"height", ParamType::Int, "new height in cells", true},
              {"anchor", ParamType::String, "where the old content goes", false,
               {"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right"}, std::string("top-left")},
              {"offset_x", ParamType::Int, "shift old content right by cells (overrides anchor)", false},
              {"offset_y", ParamType::Int, "shift old content down by cells (overrides anchor)", false}, previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                const int w = clampInt(intParam(p, "width"), 1, 4096), h = clampInt(intParam(p, "height"), 1, 4096);
                const std::string a = stringParam(p, "anchor", "top-left");
                const double fx = a.find("left") != std::string::npos ? 0 : a.find("right") != std::string::npos ? 1 : 0.5;
                const double fy = a.find("top") != std::string::npos ? 0 : a.find("bottom") != std::string::npos ? 1 : 0.5;
                const int ox = has(p, "offset_x") ? static_cast<int>(intParam(p, "offset_x")) : static_cast<int>(std::floor((w - m.width) * fx));
                const int oy = has(p, "offset_y") ? static_cast<int>(intParam(p, "offset_y")) : static_cast<int>(std::floor((h - m.height) * fy));
                const int oldW = m.width, oldH = m.height;
                for (auto& l : m.layers) {
                    if (l.kind == LayerKind::Tiles) {
                        std::vector<std::uint32_t> cells(static_cast<std::size_t>(w) * h, 0);
                        for (int y = 0; y < oldH; ++y) {
                            for (int x = 0; x < oldW; ++x) {
                                const int nx = x + ox, ny = y + oy;
                                if (nx >= 0 && ny >= 0 && nx < w && ny < h) cells[static_cast<std::size_t>(ny) * w + nx] = l.cells[static_cast<std::size_t>(y) * oldW + x];
                            }
                        }
                        l.cells = std::move(cells);
                    }
                    for (auto& o : l.objects) {
                        o.x += ox * m.tileWidth;
                        o.y += oy * m.tileHeight;
                    }
                }
                m.width = w;
                m.height = h;
                Result r = edited("resized from " + std::to_string(oldW) + "x" + std::to_string(oldH) + " to " + std::to_string(w) + "x" + std::to_string(h) +
                                      " cells (content shifted " + std::to_string(ox) + "," + std::to_string(oy) + ")",
                                  p);
                return r.with("width", static_cast<std::int64_t>(w)).with("height", static_cast<std::int64_t>(h));
            }));

    reg.add({"levels.import_csv", "Import CSV Layer",
             "Read a CSV grid (one row per line, comma-separated) into a tile layer, created if missing. values=local "
             "reads tile ids of the first tileset (or tileset=) with -1 (or blank) as empty, as Tiled's CSV export and "
             "most engines write; values=gid reads raw gids with 0 as empty.",
             {docParam(), {"path", ParamType::String, "CSV file", true}, {"layer", ParamType::String, "target tile layer (default: the file name)", false},
              {"values", ParamType::String, "what the numbers are", false, {"local", "gid"}, std::string("local")}, tilesetParam(), previewParam()},
             true},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                const fs::path path = stringParam(p, "path");
                std::ifstream in(path, std::ios::binary);
                if (!in) return Result::failure("can't read " + path.string());
                std::stringstream s;
                s << in.rdbuf();
                const std::string name = has(p, "layer") ? stringParam(p, "layer") : path.stem().string();
                Layer* l = m.layer(name);
                if (l && l->kind != LayerKind::Tiles) return Result::failure("'" + name + "' is an object layer");
                if (!l) l = &m.addLayer(name, LayerKind::Tiles);
                const bool local = stringParam(p, "values", "local") == "local" && !l->collision;  // collision grids are plain 0/1
                Map view = m;
                if (local && has(p, "tileset")) {
                    const Tileset* t = m.tileset(stringParam(p, "tileset"));
                    if (!t) return Result::failure("no tileset called '" + stringParam(p, "tileset") + "'");
                    view.tilesets = {*t};
                }
                int rows = 0, cols = 0;
                l->cells = parseCsv(view, s.str(), local, rows, cols);
                Result r = edited("imported " + std::to_string(cols) + "x" + std::to_string(rows) + " CSV into " + name +
                                      (cols > m.width || rows > m.height ? " (cropped to the " + std::to_string(m.width) + "x" + std::to_string(m.height) + " map; levels.resize first to keep it all)" : ""),
                                  p);
                return r.with("rows", static_cast<std::int64_t>(rows)).with("columns", static_cast<std::int64_t>(cols)).with("layer", name);
            }));

    reg.add({"levels.export_csv", "Export CSV Layers",
             "Write tile layers as CSV grids: one layer to path=, or every tile layer to output_dir/<layer>.csv. "
             "values=local writes tile ids within each tileset with -1 for empty; values=gid writes raw gids, 0 empty.",
             {docParam(), {"layer", ParamType::String, "with path=: the layer to write (default: the top tile layer)", false},
              {"path", ParamType::String, "one CSV file", false}, {"output_dir", ParamType::String, "folder for one CSV per tile layer", false},
              {"values", ParamType::String, "what to write", false, {"local", "gid"}, std::string("local")}},
             false},
            levelCommand(session, [](LevelDoc& doc, const Params& p) {
                Map& m = doc.map;
                const bool local = stringParam(p, "values", "local") == "local";
                auto write = [&](const Layer& l, const fs::path& file) {
                    if (auto parent = file.parent_path(); !parent.empty()) {
                        std::error_code ec;
                        fs::create_directories(parent, ec);
                    }
                    std::ofstream out(file, std::ios::binary);
                    out << layerCsv(m, l, local);
                    return static_cast<bool>(out);
                };
                std::vector<std::string> files;
                if (has(p, "path")) {
                    std::string error;
                    Layer* l = tileLayer(m, p, error);
                    if (!l) return Result::failure(error);
                    if (!write(*l, stringParam(p, "path"))) return Result::failure("can't write " + stringParam(p, "path"));
                    files.push_back(stringParam(p, "path"));
                } else if (has(p, "output_dir")) {
                    for (const auto& l : m.layers) {
                        if (l.kind != LayerKind::Tiles) continue;
                        std::string safe = l.name;
                        for (auto& c : safe) {
                            if (std::string("/\\:*?\"<>|").find(c) != std::string::npos) c = '_';
                        }
                        const fs::path file = fs::path(stringParam(p, "output_dir")) / (safe + ".csv");
                        if (!write(l, file)) return Result::failure("can't write " + file.string());
                        files.push_back(file.string());
                    }
                    if (files.empty()) return Result::failure("the map has no tile layers");
                } else {
                    return Result::failure("give path= (one layer) or output_dir= (every tile layer)");
                }
                std::string list;
                for (const auto& f : files) list += (list.empty() ? "" : ", ") + f;
                Result r = edited("wrote " + plural(files.size(), "CSV file") + ": " + list, p);
                return r.with("files", list).with("count", static_cast<std::int64_t>(files.size()));
            }, false));
}

}  // namespace

void registerCommands(Session& session) {
    registerFileCommands(session);
    registerTilesetCommands(session);
    registerLayerCommands(session);
    registerPaintCommands(session);
    registerGeneratorCommands(session);
    registerObjectCommands(session);
    registerQueryCommands(session);
}

}  // namespace achclwss::levels
