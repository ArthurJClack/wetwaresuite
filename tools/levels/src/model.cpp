// The map model, painting, generators and rendering.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <stdexcept>

#include "achclwss/levels/levels.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/ops.hpp"
#include "achclwss/raster/paint.hpp"

namespace achclwss::levels {

namespace {

bool parseIndex(const std::string& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    out = std::stoi(s);
    return true;
}

}  // namespace

Property inferProperty(const std::string& value) {
    if (value == "true" || value == "false") return {"bool", value};
    if (!value.empty()) {
        std::size_t i = (value[0] == '-' || value[0] == '+') ? 1 : 0;
        bool digits = i < value.size(), dot = false;
        for (std::size_t k = i; k < value.size(); ++k) {
            if (value[k] == '.' && !dot) dot = true;
            else if (value[k] < '0' || value[k] > '9') digits = false;
        }
        if (digits && value.back() != '.') return {dot ? "float" : "int", value};
        if (value[0] == '#' && (value.size() == 7 || value.size() == 9) && Color::parse(value)) return {"color", value};
    }
    return {"string", value};
}

// ---- Map ----------------------------------------------------------------------

int Map::layerIndex(const std::string& key) const {
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].name == key) return static_cast<int>(i);
    }
    int idx = 0;
    if (parseIndex(key, idx) && idx < static_cast<int>(layers.size())) return idx;
    return -1;
}

Layer* Map::layer(const std::string& key) {
    int i = layerIndex(key);
    return i < 0 ? nullptr : &layers[static_cast<std::size_t>(i)];
}

const Layer* Map::layer(const std::string& key) const {
    int i = layerIndex(key);
    return i < 0 ? nullptr : &layers[static_cast<std::size_t>(i)];
}

Layer& Map::addLayer(const std::string& name, LayerKind kind, int index) {
    Layer l;
    l.id = nextLayerId++;
    l.name = name;
    l.kind = kind;
    if (kind == LayerKind::Tiles) l.cells.assign(static_cast<std::size_t>(width) * height, 0);
    if (index < 0 || index > static_cast<int>(layers.size())) index = static_cast<int>(layers.size());
    return *layers.insert(layers.begin() + index, std::move(l));
}

const Tileset* Map::tilesetFor(std::uint32_t gid) const {
    gid &= kGidMask;
    if (gid == 0) return nullptr;
    const Tileset* best = nullptr;
    for (const auto& t : tilesets) {
        if (t.firstGid <= gid && (!best || t.firstGid > best->firstGid)) best = &t;
    }
    return best && best->contains(gid) ? best : nullptr;
}

Tileset* Map::tileset(const std::string& key) {
    for (auto& t : tilesets) {
        if (t.name == key) return &t;
    }
    int idx = 0;
    if (parseIndex(key, idx) && idx < static_cast<int>(tilesets.size())) return &tilesets[static_cast<std::size_t>(idx)];
    return nullptr;
}

const Tileset* Map::tileset(const std::string& key) const { return const_cast<Map*>(this)->tileset(key); }

MapObject* Map::object(int id, Layer** owner) {
    for (auto& l : layers) {
        for (auto& o : l.objects) {
            if (o.id == id) {
                if (owner) *owner = &l;
                return &o;
            }
        }
    }
    return nullptr;
}

std::size_t Map::objectCount() const {
    std::size_t n = 0;
    for (const auto& l : layers) n += l.objects.size();
    return n;
}

std::uint32_t Map::nextGid() const {
    std::uint32_t g = 1;
    for (const auto& t : tilesets) g = std::max(g, t.firstGid + static_cast<std::uint32_t>(t.tileCount));
    return g;
}

const Properties* Map::tileProperties(std::uint32_t gid) const {
    const Tileset* t = tilesetFor(gid);
    if (!t) return nullptr;
    auto it = t->tileProps.find(static_cast<int>((gid & kGidMask) - t->firstGid));
    return it == t->tileProps.end() ? nullptr : &it->second;
}

Map newMap(int width, int height, int tileWidth, int tileHeight) {
    Map m;
    m.width = width;
    m.height = height;
    m.tileWidth = tileWidth;
    m.tileHeight = tileHeight;
    return m;
}

Tileset loadTileset(const std::filesystem::path& image, int tileWidth, int tileHeight, int margin, int spacing, const std::string& name) {
    if (tileWidth <= 0 || tileHeight <= 0) throw std::runtime_error("tile size must be positive");
    Tileset t;
    t.name = name.empty() ? image.stem().string() : name;
    t.tileWidth = tileWidth;
    t.tileHeight = tileHeight;
    t.margin = std::max(0, margin);
    t.spacing = std::max(0, spacing);
    std::error_code ec;
    auto abs = std::filesystem::absolute(image, ec);
    t.image = (ec ? image : abs).lexically_normal().string();
    auto img = std::make_shared<Image>(raster::load(image));
    t.imageWidth = img->width();
    t.imageHeight = img->height();
    t.columns = (t.imageWidth - 2 * t.margin + t.spacing) / (tileWidth + t.spacing);
    const int rows = (t.imageHeight - 2 * t.margin + t.spacing) / (tileHeight + t.spacing);
    if (t.columns < 1 || rows < 1) {
        throw std::runtime_error(image.string() + " (" + std::to_string(t.imageWidth) + "x" + std::to_string(t.imageHeight) +
                                 ") is smaller than one " + std::to_string(tileWidth) + "x" + std::to_string(tileHeight) + " tile");
    }
    t.tileCount = t.columns * rows;
    t.pixels = std::move(img);
    return t;
}

Tileset& addTileset(Map& map, Tileset tileset) {
    tileset.firstGid = map.nextGid();
    map.tilesets.push_back(std::move(tileset));
    return map.tilesets.back();
}

// ---- painting -----------------------------------------------------------------

CellRect clip(const Map& map, CellRect r) {
    int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    int x1 = std::min(map.width, r.x + r.w), y1 = std::min(map.height, r.y + r.h);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

int fillRect(const Map& map, Layer& layer, CellRect r, std::uint32_t gid, bool outline) {
    const CellRect c = clip(map, r);
    int n = 0;
    for (int y = c.y; y < c.y + c.h; ++y) {
        for (int x = c.x; x < c.x + c.w; ++x) {
            if (outline && x != r.x && y != r.y && x != r.x + r.w - 1 && y != r.y + r.h - 1) continue;
            if (map.at(layer, x, y) != gid) {
                map.set(layer, x, y, gid);
                ++n;
            }
        }
    }
    return n;
}

int floodFill(const Map& map, Layer& layer, int x, int y, std::uint32_t gid, bool diagonal) {
    if (!map.inside(x, y)) return 0;
    const std::uint32_t target = map.at(layer, x, y);
    if (target == gid) return 0;
    std::deque<std::pair<int, int>> q{{x, y}};
    map.set(layer, x, y, gid);
    int n = 1;
    while (!q.empty()) {
        auto [cx, cy] = q.front();
        q.pop_front();
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx == 0 && dy == 0) || (!diagonal && dx != 0 && dy != 0)) continue;
                const int nx = cx + dx, ny = cy + dy;
                if (!map.inside(nx, ny) || map.at(layer, nx, ny) != target) continue;
                map.set(layer, nx, ny, gid);
                ++n;
                q.emplace_back(nx, ny);
            }
        }
    }
    return n;
}

int drawLine(const Map& map, Layer& layer, int x1, int y1, int x2, int y2, std::uint32_t gid) {
    int dx = std::abs(x2 - x1), dy = -std::abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1, err = dx + dy, n = 0;
    while (true) {
        if (map.inside(x1, y1) && map.at(layer, x1, y1) != gid) {
            map.set(layer, x1, y1, gid);
            ++n;
        }
        if (x1 == x2 && y1 == y2) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }
    return n;
}

int stamp(const Map& map, Layer& layer, int x, int y, int w, int h, const std::vector<std::uint32_t>& src, bool skipEmpty) {
    int n = 0;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const std::uint32_t g = src[static_cast<std::size_t>(j) * w + i];
            if ((skipEmpty && g == 0) || !map.inside(x + i, y + j)) continue;
            if (map.at(layer, x + i, y + j) != g) {
                map.set(layer, x + i, y + j, g);
                ++n;
            }
        }
    }
    return n;
}

std::vector<std::uint32_t> copyRegion(const Map& map, const Layer& from, CellRect r) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(std::max(0, r.w)) * std::max(0, r.h), 0);
    for (int j = 0; j < r.h; ++j) {
        for (int i = 0; i < r.w; ++i) {
            if (map.inside(r.x + i, r.y + j)) out[static_cast<std::size_t>(j) * r.w + i] = map.at(from, r.x + i, r.y + j);
        }
    }
    return out;
}

int autotile(const Map& map, const Layer& src, Layer& dst, const std::function<bool(std::uint32_t)>& isTerrain, std::uint32_t base,
             CellRect region, bool edgesSolid) {
    const CellRect c = clip(map, region);
    const std::vector<std::uint32_t> before = src.cells;  // src and dst may be the same layer
    auto terrain = [&](int x, int y) {
        if (!map.inside(x, y)) return edgesSolid;
        return isTerrain(before[static_cast<std::size_t>(y) * map.width + x]);
    };
    int n = 0;
    for (int y = c.y; y < c.y + c.h; ++y) {
        for (int x = c.x; x < c.x + c.w; ++x) {
            if (!terrain(x, y)) continue;
            const std::uint32_t mask = (terrain(x, y - 1) ? 1u : 0u) | (terrain(x + 1, y) ? 2u : 0u) | (terrain(x, y + 1) ? 4u : 0u) |
                                       (terrain(x - 1, y) ? 8u : 0u);
            const std::uint32_t g = base + mask;
            if (map.at(dst, x, y) != g) {
                map.set(dst, x, y, g);
                ++n;
            }
        }
    }
    return n;
}

// ---- randomness ---------------------------------------------------------------

Rng::Rng(std::uint64_t seed) : s_(seed) {
    s_ = next() ^ 0x632BE59BD9B4E019ull;  // scramble so neighbouring seeds give unrelated streams
}

std::uint64_t Rng::next() {  // splitmix64
    std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double Rng::uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

int Rng::range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1));
}

int scatter(const Map& map, Layer& layer, const std::vector<std::uint32_t>& gids, const std::vector<double>& weights, CellRect region,
            double density, int count, std::uint64_t seed, const std::function<bool(int, int)>& allowed) {
    if (gids.empty()) return 0;
    Rng rng(seed);
    double total = 0;
    for (std::size_t i = 0; i < gids.size(); ++i) total += i < weights.size() ? std::max(0.0, weights[i]) : 1.0;
    auto pick = [&]() {
        double r = rng.uniform() * total;
        for (std::size_t i = 0; i < gids.size(); ++i) {
            r -= i < weights.size() ? std::max(0.0, weights[i]) : 1.0;
            if (r < 0) return gids[i];
        }
        return gids.back();
    };
    const CellRect c = clip(map, region);
    std::vector<std::pair<int, int>> cells;
    for (int y = c.y; y < c.y + c.h; ++y) {
        for (int x = c.x; x < c.x + c.w; ++x) {
            if (!allowed || allowed(x, y)) cells.emplace_back(x, y);
        }
    }
    int n = 0;
    if (count > 0) {
        // partial Fisher-Yates: `count` distinct cells
        const int k = std::min<int>(count, static_cast<int>(cells.size()));
        for (int i = 0; i < k; ++i) {
            const int j = rng.range(i, static_cast<int>(cells.size()) - 1);
            std::swap(cells[static_cast<std::size_t>(i)], cells[static_cast<std::size_t>(j)]);
            map.set(layer, cells[static_cast<std::size_t>(i)].first, cells[static_cast<std::size_t>(i)].second, pick());
            ++n;
        }
        return n;
    }
    for (auto [x, y] : cells) {
        if (rng.uniform() < density) {
            map.set(layer, x, y, pick());
            ++n;
        }
    }
    return n;
}

// ---- generators ---------------------------------------------------------------

std::vector<bool> caves(int w, int h, const CaveOptions& o) {
    Rng rng(o.seed);
    auto idx = [w](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
    std::vector<bool> wall(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) wall[idx(x, y)] = x == 0 || y == 0 || x == w - 1 || y == h - 1 || rng.uniform() < o.fill;
    }
    for (int s = 0; s < o.steps; ++s) {
        std::vector<bool> next = wall;
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) n += (dx || dy) && wall[idx(x + dx, y + dy)];
                }
                next[idx(x, y)] = n > 4 || (n == 4 && wall[idx(x, y)]);
            }
        }
        wall.swap(next);
    }
    if (o.connect) {
        std::vector<int> region(wall.size(), -1);
        int best = -1;
        std::size_t bestSize = 0;
        int id = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (wall[idx(x, y)] || region[idx(x, y)] >= 0) continue;
                std::size_t size = 0;
                std::deque<std::pair<int, int>> q{{x, y}};
                region[idx(x, y)] = id;
                while (!q.empty()) {
                    auto [cx, cy] = q.front();
                    q.pop_front();
                    ++size;
                    const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (auto& dd : d) {
                        const int nx = cx + dd[0], ny = cy + dd[1];
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || wall[idx(nx, ny)] || region[idx(nx, ny)] >= 0) continue;
                        region[idx(nx, ny)] = id;
                        q.emplace_back(nx, ny);
                    }
                }
                if (size > bestSize) {
                    bestSize = size;
                    best = id;
                }
                ++id;
            }
        }
        for (std::size_t i = 0; i < wall.size(); ++i) {
            if (!wall[i] && region[i] != best) wall[i] = true;
        }
    }
    return wall;
}

namespace {

struct Leaf {
    CellRect r;
    int a = -1, b = -1;  // children
    int room = -1;
};

}  // namespace

Rooms bspRooms(int w, int h, const RoomOptions& o) {
    Rng rng(o.seed);
    Rooms out;
    out.wall.assign(static_cast<std::size_t>(w) * h, true);
    const int minRoom = std::max(2, o.minRoom), maxRoom = std::max(minRoom, o.maxRoom);
    const int pad = std::max(0, o.padding);
    const int minLeaf = minRoom + 2 * pad;
    std::vector<Leaf> leaves{{CellRect{1, 1, w - 2, h - 2}}};
    // split until leaves are small enough
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        CellRect r = leaves[i].r;
        const bool tooBig = r.w > maxRoom + 2 * pad || r.h > maxRoom + 2 * pad;
        if (!tooBig && rng.uniform() < 0.6) continue;
        bool vertical = r.w > r.h ? true : (r.h > r.w ? false : rng.uniform() < 0.5);
        if (r.w >= 1.25 * r.h) vertical = true;
        else if (r.h >= 1.25 * r.w) vertical = false;
        const int len = vertical ? r.w : r.h;
        if (len < 2 * minLeaf) continue;
        const int cut = rng.range(minLeaf, len - minLeaf);
        Leaf a, b;
        if (vertical) {
            a.r = {r.x, r.y, cut, r.h};
            b.r = {r.x + cut, r.y, r.w - cut, r.h};
        } else {
            a.r = {r.x, r.y, r.w, cut};
            b.r = {r.x, r.y + cut, r.w, r.h - cut};
        }
        leaves[i].a = static_cast<int>(leaves.size());
        leaves.push_back(a);
        leaves[i].b = static_cast<int>(leaves.size());
        leaves.push_back(b);
    }
    auto carve = [&](CellRect r) {
        const CellRect c{std::max(1, r.x), std::max(1, r.y), 0, 0};
        const int x1 = std::min(w - 1, r.x + r.w), y1 = std::min(h - 1, r.y + r.h);
        for (int y = c.y; y < y1; ++y) {
            for (int x = c.x; x < x1; ++x) out.wall[static_cast<std::size_t>(y) * w + x] = false;
        }
    };
    for (auto& l : leaves) {
        if (l.a >= 0) continue;
        const int maxW = std::min(maxRoom, l.r.w - 2 * pad), maxH = std::min(maxRoom, l.r.h - 2 * pad);
        if (maxW < minRoom || maxH < minRoom) continue;
        const int rw = rng.range(minRoom, maxW), rh = rng.range(minRoom, maxH);
        const int rx = rng.range(l.r.x + pad, l.r.x + l.r.w - pad - rw), ry = rng.range(l.r.y + pad, l.r.y + l.r.h - pad - rh);
        l.room = static_cast<int>(out.rooms.size());
        out.rooms.push_back({rx, ry, rw, rh});
        carve(out.rooms.back());
    }
    // connect siblings bottom-up with L-shaped corridors between a room of each side
    const int cw = std::max(1, o.corridorWidth);
    std::function<int(int)> anyRoom = [&](int i) -> int {
        const Leaf& l = leaves[static_cast<std::size_t>(i)];
        if (l.room >= 0) return l.room;
        if (l.a < 0) return -1;
        int r = rng.uniform() < 0.5 ? anyRoom(l.a) : anyRoom(l.b);
        if (r < 0) r = anyRoom(l.a);
        if (r < 0) r = anyRoom(l.b);
        return r;
    };
    for (std::size_t i = leaves.size(); i-- > 0;) {
        const Leaf& l = leaves[i];
        if (l.a < 0) continue;
        const int ra = anyRoom(l.a), rb = anyRoom(l.b);
        if (ra < 0 || rb < 0) continue;
        const CellRect A = out.rooms[static_cast<std::size_t>(ra)], B = out.rooms[static_cast<std::size_t>(rb)];
        const int ax = A.x + A.w / 2, ay = A.y + A.h / 2;  // centre to centre keeps corridors short
        const int bx = B.x + B.w / 2, by = B.y + B.h / 2;
        const int off = (cw - 1) / 2;
        if (rng.uniform() < 0.5) {
            carve({std::min(ax, bx) - off, ay - off, std::abs(bx - ax) + cw, cw});
            carve({bx - off, std::min(ay, by) - off, cw, std::abs(by - ay) + cw});
        } else {
            carve({ax - off, std::min(ay, by) - off, cw, std::abs(by - ay) + cw});
            carve({std::min(ax, bx) - off, by - off, std::abs(bx - ax) + cw, cw});
        }
    }
    return out;
}

namespace {

double hashNoise(std::uint64_t seed, int x, int y) {
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 0x9E3779B185EBCA87ull) ^
                      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

double smooth(double t) { return t * t * (3 - 2 * t); }

double valueNoise(std::uint64_t seed, double x, double y) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const double fx = smooth(x - x0), fy = smooth(y - y0);
    const double a = hashNoise(seed, x0, y0), b = hashNoise(seed, x0 + 1, y0);
    const double c = hashNoise(seed, x0, y0 + 1), d = hashNoise(seed, x0 + 1, y0 + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

}  // namespace

std::vector<double> noise(int w, int h, const NoiseOptions& o) {
    std::vector<double> out(static_cast<std::size_t>(w) * h);
    const int octaves = std::clamp(o.octaves, 1, 10);
    const double scale = std::max(0.5, o.scale);
    double lo = 1e9, hi = -1e9;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double amp = 1, freq = 1 / scale, sum = 0, norm = 0;
            for (int k = 0; k < octaves; ++k) {
                sum += amp * valueNoise(o.seed + static_cast<std::uint64_t>(k) * 1013, x * freq, y * freq);
                norm += amp;
                amp *= o.persistence;
                freq *= 2;
            }
            const double v = sum / norm;
            out[static_cast<std::size_t>(y) * w + x] = v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    // stretch to the full 0..1 range so thresholds mean the same at any size
    if (hi - lo > 1e-9) {
        for (auto& v : out) v = (v - lo) / (hi - lo);
    }
    return out;
}

std::vector<CellRect> mergeRects(int w, int h, const std::vector<bool>& solid) {
    std::vector<CellRect> done, open;
    for (int y = 0; y < h; ++y) {
        std::vector<CellRect> runs;
        for (int x = 0; x < w;) {
            if (!solid[static_cast<std::size_t>(y) * w + x]) {
                ++x;
                continue;
            }
            int x1 = x;
            while (x1 < w && solid[static_cast<std::size_t>(y) * w + x1]) ++x1;
            runs.push_back({x, y, x1 - x, 1});
            x = x1;
        }
        std::vector<CellRect> next;
        for (auto& run : runs) {
            auto it = std::find_if(open.begin(), open.end(), [&](const CellRect& o) { return o.x == run.x && o.w == run.w; });
            if (it != open.end()) {
                CellRect grown = *it;
                grown.h += 1;
                next.push_back(grown);
                open.erase(it);
            } else {
                next.push_back(run);
            }
        }
        for (auto& o : open) done.push_back(o);
        open = std::move(next);
    }
    for (auto& o : open) done.push_back(o);
    std::sort(done.begin(), done.end(), [](const CellRect& a, const CellRect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    return done;
}

// ---- rendering ----------------------------------------------------------------

Color placeholderColor(std::uint32_t gid) {
    gid &= kGidMask;
    // golden-angle hues, two lightness bands
    const double hue = std::fmod(gid * 137.508, 360.0);
    const double s = 0.55, l = (gid % 2) ? 0.55 : 0.42;
    const double c = (1 - std::abs(2 * l - 1)) * s, hp = hue / 60.0, x = c * (1 - std::abs(std::fmod(hp, 2.0) - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1) r = c, g = x;
    else if (hp < 2) r = x, g = c;
    else if (hp < 3) g = c, b = x;
    else if (hp < 4) g = x, b = c;
    else if (hp < 5) r = x, b = c;
    else r = c, b = x;
    const double m = l - c / 2;
    auto to8 = [](double v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0, 1.0) * 255 + 0.5); };
    return {to8(r + m), to8(g + m), to8(b + m), 255};
}

namespace {

void over(std::uint8_t* d, const std::uint8_t* s, double opacity) {
    const double sa = s[3] / 255.0 * opacity;
    if (sa <= 0) return;
    const double da = d[3] / 255.0, oa = sa + da * (1 - sa);
    for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>((s[c] * sa + d[c] * da * (1 - sa)) / oa + 0.5);
    d[3] = static_cast<std::uint8_t>(oa * 255 + 0.5);
}

void drawTile(Image& out, const Map& map, std::uint32_t raw, int px, int py, double opacity) {
    const std::uint32_t gid = raw & kGidMask;
    const Tileset* t = map.tilesetFor(gid);
    if (!t || !t->pixels) {
        const Color c = placeholderColor(gid);
        const std::uint8_t fill[4] = {c.r, c.g, c.b, 255};
        const std::uint8_t edge[4] = {static_cast<std::uint8_t>(c.r * 3 / 4), static_cast<std::uint8_t>(c.g * 3 / 4),
                                      static_cast<std::uint8_t>(c.b * 3 / 4), 255};
        for (int y = 0; y < map.tileHeight; ++y) {
            for (int x = 0; x < map.tileWidth; ++x) {
                if (!out.contains(px + x, py + y)) continue;
                const bool border = x == 0 || y == 0 || x == map.tileWidth - 1 || y == map.tileHeight - 1;
                over(out.pixel(px + x, py + y), border ? edge : fill, opacity);
            }
        }
        return;
    }
    const int id = static_cast<int>(gid - t->firstGid);
    const int sx = t->tileX(id), sy = t->tileY(id);
    // Tiled anchors tiles taller than the grid at the bottom-left of the cell
    const int oy = py + map.tileHeight - t->tileHeight;
    const bool fh = raw & kFlipH, fv = raw & kFlipV, fd = raw & kFlipD;
    for (int y = 0; y < t->tileHeight; ++y) {
        for (int x = 0; x < t->tileWidth; ++x) {
            int u = x, v = y;
            if (fd) std::swap(u, v);
            if (fh) u = t->tileWidth - 1 - u;
            if (fv) v = t->tileHeight - 1 - v;
            if (!t->pixels->contains(sx + u, sy + v) || !out.contains(px + x, oy + y)) continue;
            over(out.pixel(px + x, oy + y), t->pixels->pixel(sx + u, sy + v), opacity);
        }
    }
}

Color typeColor(const std::string& type) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : type) h = (h ^ c) * 16777619u;
    Color c = placeholderColor(h % 997 + 1);
    return c;
}

}  // namespace

Image render(const Map& map, const RenderOptions& o) {
    const int pw = map.width * map.tileWidth, ph = map.height * map.tileHeight;
    Color bg{0, 0, 0, 0};
    if (auto c = Color::parse(map.background); c && !map.background.empty()) bg = *c;
    Image img(std::max(1, pw), std::max(1, ph), bg);
    auto wanted = [&](std::size_t i) {
        const Layer& l = map.layers[i];
        if (o.layers.empty()) return l.visible;
        for (const auto& k : o.layers) {
            if (map.layerIndex(k) == static_cast<int>(i)) return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < map.layers.size(); ++i) {
        const Layer& l = map.layers[i];
        if (l.kind != LayerKind::Tiles || !wanted(i)) continue;
        if (l.collision) continue;  // drawn as an overlay below
        for (int y = 0; y < map.height; ++y) {
            for (int x = 0; x < map.width; ++x) {
                const std::uint32_t g = map.at(l, x, y);
                if (g & kGidMask) drawTile(img, map, g, x * map.tileWidth, y * map.tileHeight, l.opacity);
            }
        }
    }
    const double s = std::clamp(o.scale, 0.05, 16.0);
    if (std::abs(s - 1.0) > 1e-9) {
        img = raster::resize(img, std::max(1, static_cast<int>(std::lround(pw * s))), std::max(1, static_cast<int>(std::lround(ph * s))),
                             s > 1 ? raster::Filter::Nearest : raster::Filter::Bilinear);
    }
    const double tw = map.tileWidth * s, th = map.tileHeight * s;
    if (o.collision) {
        for (std::size_t i = 0; i < map.layers.size(); ++i) {
            const Layer& l = map.layers[i];
            if (l.kind != LayerKind::Tiles || !l.collision || !wanted(i)) continue;
            for (int y = 0; y < map.height; ++y) {
                for (int x = 0; x < map.width; ++x) {
                    if (map.at(l, x, y) & kGidMask) raster::fillRect(img, x * tw, y * th, tw, th, Color{230, 40, 40, 110});
                }
            }
        }
    }
    if (o.grid) {
        const Color gc{0, 0, 0, 60};
        for (int x = 0; x <= map.width; ++x) raster::fillRect(img, x * tw, 0, 1, img.height(), gc);
        for (int y = 0; y <= map.height; ++y) raster::fillRect(img, 0, y * th, img.width(), 1, gc);
    }
    if (o.objects) {
        for (std::size_t i = 0; i < map.layers.size(); ++i) {
            const Layer& l = map.layers[i];
            if (l.kind != LayerKind::Objects || !wanted(i)) continue;
            for (const auto& ob : l.objects) {
                Color c = typeColor(ob.type.empty() ? ob.name : ob.type);
                if (l.collision || ob.type == "collision") c = {230, 40, 40, 255};
                Color fill = c;
                fill.a = 70;
                const double x = ob.x * s, y = ob.y * s, w = ob.width * s, h = ob.height * s;
                if (ob.shape == "point" || (w <= 0 && h <= 0)) {
                    const double r = std::max(3.0, std::min(tw, th) * 0.25);
                    raster::fillEllipse(img, x, y, r, r, c);
                    raster::strokeEllipse(img, x, y, r, r, Color{255, 255, 255, 255}, 1);
                } else if (ob.shape == "ellipse") {
                    raster::fillEllipse(img, x + w / 2, y + h / 2, w / 2, h / 2, fill);
                    raster::strokeEllipse(img, x + w / 2, y + h / 2, w / 2, h / 2, c, std::max(1.0, s));
                } else {
                    raster::fillRect(img, x, y, w, h, fill);
                    raster::strokeRect(img, x + 0.5, y + 0.5, std::max(0.0, w - 1), std::max(0.0, h - 1), c, std::max(1.0, s));
                }
            }
        }
    }
    return img;
}

Image placeholderTileset(int tiles, int columns, int tw, int th, const std::string& style, const std::vector<Color>& colors) {
    tiles = std::max(1, tiles);
    columns = columns > 0 ? columns : std::min(tiles, 8);
    const int rows = (tiles + columns - 1) / columns;
    Image img(columns * tw, rows * th);
    for (int i = 0; i < tiles; ++i) {
        const int ox = (i % columns) * tw, oy = (i / columns) * th;
        Color c = i < static_cast<int>(colors.size()) ? colors[static_cast<std::size_t>(i)] : placeholderColor(static_cast<std::uint32_t>(i + 1));
        if (style == "autotile") {
            // body colour with a dark rim on every side that has no neighbour (mask bits N1 E2 S4 W8)
            c = colors.empty() ? Color{96, 160, 72, 255} : colors[0];
            const Color rim{static_cast<std::uint8_t>(c.r / 3), static_cast<std::uint8_t>(c.g / 3), static_cast<std::uint8_t>(c.b / 3), 255};
            const int m = i % 16, bw = std::max(1, std::min(tw, th) / 6);
            for (int y = 0; y < th; ++y) {
                for (int x = 0; x < tw; ++x) {
                    bool edge = (!(m & 1) && y < bw) || (!(m & 2) && x >= tw - bw) || (!(m & 4) && y >= th - bw) || (!(m & 8) && x < bw);
                    img.set(ox + x, oy + y, edge ? rim : c);
                }
            }
            continue;
        }
        const Color edge{static_cast<std::uint8_t>(c.r * 3 / 4), static_cast<std::uint8_t>(c.g * 3 / 4), static_cast<std::uint8_t>(c.b * 3 / 4), c.a};
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) img.set(ox + x, oy + y, (x == 0 || y == 0 || x == tw - 1 || y == th - 1) ? edge : c);
        }
        // a small mark of i's bits along the top so tiles can be told apart even in similar hues
        for (int b = 0; b < 8 && (b + 1) * 3 < tw; ++b) {
            if (i >> b & 1) {
                for (int y = 2; y < std::min(th - 1, 4); ++y) img.set(ox + 2 + b * 3, oy + y, Color{255, 255, 255, 200});
            }
        }
    }
    return img;
}

// ---- documents ----------------------------------------------------------------

std::string LevelDoc::summary() const {
    std::size_t tileLayers = 0, objectLayers = 0;
    for (const auto& l : map.layers) (l.kind == LayerKind::Tiles ? tileLayers : objectLayers)++;
    std::string s = std::to_string(map.width) + "x" + std::to_string(map.height) + " cells of " + std::to_string(map.tileWidth) + "x" +
                    std::to_string(map.tileHeight) + " px, " + std::to_string(tileLayers) + " tile layer" + (tileLayers == 1 ? "" : "s") +
                    ", " + std::to_string(objectLayers) + " object layer" + (objectLayers == 1 ? "" : "s") + ", " +
                    std::to_string(map.objectCount()) + " object" + (map.objectCount() == 1 ? "" : "s") + ", " +
                    std::to_string(map.tilesets.size()) + " tileset" + (map.tilesets.size() == 1 ? "" : "s");
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> LevelDoc::preview() const {
    RenderOptions o;
    const int big = std::max(map.width * map.tileWidth, map.height * map.tileHeight);
    if (big > 0 && big < 512) o.scale = std::max(1.0, std::floor(512.0 / big));
    return std::make_unique<Attachment>(Attachment{"image/png", raster::previewPng(render(map, o), 1024), "preview.png"});
}

Result LevelEdit::apply() {
    before_ = doc_->map;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void LevelEdit::revert() { doc_->map = before_; }

}  // namespace achclwss::levels
