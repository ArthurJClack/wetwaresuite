// A grid maze router (A* on two layers with vias). Each grid point keeps the distance to
// the nearest copper of two different nets, so a trace of net n may use a point when the
// nearest copper of any *other* net is at least clearance + half its width away.
#include <cstdint>
#include <optional>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>

#include "achclwss/circuit/circuit.hpp"
#include "geom.hpp"

namespace achclwss::circuit {

namespace {

constexpr float kFar = 1e9f;

struct Field {
    int nx = 0, ny = 0, layers = 2;
    double x0 = 0, y0 = 0, p = 0.127;
    std::vector<float> d1, d2;
    std::vector<int> n1;

    std::size_t idx(int x, int y, int l) const { return (static_cast<std::size_t>(l) * ny + y) * nx + x; }
    Pt pt(int x, int y) const { return {x0 + x * p, y0 + y * p}; }
    float dist(std::size_t k, int net) const { return n1[k] == net ? d2[k] : d1[k]; }

    void stamp(const geom::Shape& s, int layerMask, int net, double reach, double offset = 0) {
        const int xa = std::max(0, static_cast<int>(std::floor((s.x0() - reach - x0) / p)));
        const int xb = std::min(nx - 1, static_cast<int>(std::ceil((s.x1() + reach - x0) / p)));
        const int ya = std::max(0, static_cast<int>(std::floor((s.y0() - reach - y0) / p)));
        const int yb = std::min(ny - 1, static_cast<int>(std::ceil((s.y1() + reach - y0) / p)));
        for (int l = 0; l < layers; ++l) {
            if (!(layerMask & (1 << l))) continue;
            for (int y = ya; y <= yb; ++y) {
                for (int x = xa; x <= xb; ++x) {
                    const float d = static_cast<float>(geom::distance(s, pt(x, y)) - offset);
                    if (d > reach) continue;
                    const std::size_t k = idx(x, y, l);
                    if (n1[k] == net) {
                        d1[k] = std::min(d1[k], d);
                    } else if (d < d1[k]) {
                        d2[k] = d1[k];
                        d1[k] = d;
                        n1[k] = net;
                    } else {
                        d2[k] = std::min(d2[k], d);
                    }
                }
            }
        }
    }
};

struct NetCopper {
    std::vector<geom::Shape> shapes;
    std::vector<int> layers;
    std::vector<int> padIndex;  // -1 for traces/vias
};

}  // namespace

RouteReport autoroute(Circuit& c, const Netlist& nl, const RouteOptions& options) {
    RouteReport rep;
    if (c.board.outline.size() < 3) {
        rep.failedLinks.push_back("no board outline");
        return rep;
    }
    const Rules& rules = c.board.rules;
    const auto start = std::chrono::steady_clock::now();
    const int layers = std::clamp(options.layers, 1, 2);
    const auto pads = worldPads(c, nl);

    // which nets, in which order (short ones first)
    std::map<std::string, std::vector<int>> netPads;
    for (std::size_t i = 0; i < pads.size(); ++i) {
        if (!pads[i].net.empty()) netPads[pads[i].net].push_back(static_cast<int>(i));
    }
    std::set<std::string> wanted(options.nets.begin(), options.nets.end());
    std::set<std::string> skipped(options.skip.begin(), options.skip.end());
    std::vector<std::string> nets;
    for (const auto& [n, v] : netPads) {
        if (v.size() < 2 || skipped.count(n)) continue;
        if (!wanted.empty() && !wanted.count(n)) continue;
        nets.push_back(n);
    }
    auto hpwl = [&](const std::string& n) {
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (int i : netPads[n]) {
            const Pt p = pads[static_cast<std::size_t>(i)].pos;
            x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
        }
        return (x1 - x0) + (y1 - y0);
    };
    std::stable_sort(nets.begin(), nets.end(), [&](const std::string& a, const std::string& b) { return hpwl(a) < hpwl(b); });
    std::map<std::string, int> netId;
    int nextId = 1;
    for (const auto& [n, v] : netPads) netId[n] = nextId++;
    for (const auto& t : c.board.traces) {
        if (!t.net.empty() && !netId.count(t.net)) netId[t.net] = nextId++;
    }
    for (const auto& v : c.board.vias) {
        if (!v.net.empty() && !netId.count(v.net)) netId[v.net] = nextId++;
    }

    double bx0, by0, bx1, by1;
    boardBounds(c.board, bx0, by0, bx1, by1);
    double p = std::max(0.05, rules.grid);
    while (((bx1 - bx0) / p + 1) * ((by1 - by0) / p + 1) > 4e6) p *= 1.25;
    const double infl = 0.71 * p;
    const double maxHalf = std::max({rules.trace, rules.powerTrace, rules.viaDiameter}) / 2;
    const double reach = std::max(rules.clearance, rules.edgeClearance) + maxHalf + infl + p;

    const std::vector<Trace> originalTraces = c.board.traces;
    const std::vector<Via> originalVias = c.board.vias;

    auto runPass = [&](const std::vector<std::string>& order, RouteReport& r) {
        c.board.traces = originalTraces;
        c.board.vias = originalVias;
        Field f;
        f.layers = layers;
        f.p = p;
        f.x0 = std::floor(bx0 / p) * p;
        f.y0 = std::floor(by0 / p) * p;
        f.nx = static_cast<int>(std::ceil((bx1 - f.x0) / p)) + 1;
        f.ny = static_cast<int>(std::ceil((by1 - f.y0) / p)) + 1;
        const std::size_t cells = static_cast<std::size_t>(f.nx) * f.ny * layers;
        f.d1.assign(cells, kFar);
        f.d2.assign(cells, kFar);
        f.n1.assign(cells, 0);
        int uniq = -1;
        // board edge: outside = blocked, near the edge = scaled so one threshold works
        for (int l = 0; l < layers; ++l) {
            for (int y = 0; y < f.ny; ++y) {
                for (int x = 0; x < f.nx; ++x) {
                    if (!insideBoard(c.board, f.pt(x, y))) {
                        const std::size_t k = f.idx(x, y, l);
                        f.d1[k] = f.d2[k] = -kFar;
                        f.n1[k] = -999999;
                    }
                }
            }
        }
        const int edgeNet = -999998;
        for (std::size_t i = 0; i < c.board.outline.size(); ++i) {
            f.stamp(geom::capsule(c.board.outline[i], c.board.outline[(i + 1) % c.board.outline.size()], 0), 3, edgeNet, reach,
                    rules.edgeClearance - rules.clearance);
        }
        // the edge must never count as "own net": give d2 the same value
        for (std::size_t k = 0; k < cells; ++k) {
            if (f.n1[k] == edgeNet) f.d2[k] = std::min(f.d2[k], f.d1[k]);
        }
        const auto items = geom::copperItems(c, pads);
        for (const auto& it : items) f.stamp(it.shape, it.layers, it.net.empty() ? uniq-- : netId[it.net], reach);

        std::vector<float> g(cells);
        std::vector<int> parent(cells);
        std::vector<int> stampOf(cells, 0);
        std::vector<std::uint8_t> dirOf(cells, 255);
        int gen = 0;

        for (const auto& net : order) {
            const int nid = netId[net];
            const bool power = isPowerName(net);
            for (;;) {
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(options.maxSeconds)) {
                    r.failedLinks.push_back(net + ": out of time");
                    ++r.failed;
                    break;
                }
                // clusters of this net's copper
                NetCopper cu;
                for (int i : netPads[net]) {
                    cu.shapes.push_back(geom::padShape(pads[static_cast<std::size_t>(i)]));
                    cu.layers.push_back(pads[static_cast<std::size_t>(i)].layers);
                    cu.padIndex.push_back(i);
                }
                for (const auto& t : c.board.traces) {
                    if (t.net != net) continue;
                    cu.shapes.push_back(geom::capsule(t.a, t.b, t.width / 2));
                    cu.layers.push_back(t.layer == 0 ? 1 : 2);
                    cu.padIndex.push_back(-1);
                }
                for (const auto& v : c.board.vias) {
                    if (v.net != net) continue;
                    cu.shapes.push_back(geom::capsule(v.pos, v.pos, v.diameter / 2));
                    cu.layers.push_back(3);
                    cu.padIndex.push_back(-1);
                }
                geom::Uf uf(cu.shapes.size());
                for (std::size_t i = 0; i < cu.shapes.size(); ++i) {
                    for (std::size_t j = i + 1; j < cu.shapes.size(); ++j) {
                        if ((cu.layers[i] & cu.layers[j]) && geom::distance(cu.shapes[i], cu.shapes[j]) <= 1e-6) uf.unite(static_cast<int>(i), static_cast<int>(j));
                    }
                }
                std::set<int> roots;
                for (std::size_t i = 0; i < cu.shapes.size(); ++i) {
                    if (cu.padIndex[i] >= 0) roots.insert(uf.find(static_cast<int>(i)));
                }
                if (roots.size() < 2) break;
                ++r.connections;
                const int srcRoot = uf.find(0);
                // mark target points; seed sources
                ++gen;
                std::vector<std::uint8_t> isTarget(cells, 0);
                std::vector<Pt> targetCentres;
                using Node = std::pair<float, std::size_t>;
                std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
                const double hw = (power ? rules.powerTrace : rules.trace) / 2;
                auto passable = [&](std::size_t k, double half) { return f.dist(k, nid) >= rules.clearance + half + infl; };
                std::vector<std::size_t> sources;
                for (std::size_t i = 0; i < cu.shapes.size(); ++i) {
                    const bool src = uf.find(static_cast<int>(i)) == srcRoot;
                    const auto& s = cu.shapes[i];
                    if (!src && cu.padIndex[i] >= 0) targetCentres.push_back(pads[static_cast<std::size_t>(cu.padIndex[i])].pos);
                    const int xa = std::max(0, static_cast<int>(std::floor((s.x0() - f.x0) / p)));
                    const int xb = std::min(f.nx - 1, static_cast<int>(std::ceil((s.x1() - f.x0) / p)));
                    const int ya = std::max(0, static_cast<int>(std::floor((s.y0() - f.y0) / p)));
                    const int yb = std::min(f.ny - 1, static_cast<int>(std::ceil((s.y1() - f.y0) / p)));
                    for (int l = 0; l < layers; ++l) {
                        if (!(cu.layers[i] & (1 << l))) continue;
                        bool any = false;
                        for (int y = ya; y <= yb; ++y) {
                            for (int x = xa; x <= xb; ++x) {
                                if (geom::signedDistance(s, f.pt(x, y)) > 1e-9) continue;
                                any = true;
                                const std::size_t k = f.idx(x, y, l);
                                if (src) sources.push_back(k);
                                else isTarget[k] = 1;
                            }
                        }
                        if (!any) {
                            // tiny copper between grid points: use the nearest point
                            const int x = std::clamp(static_cast<int>(std::lround((s.a.x - f.x0) / p)), 0, f.nx - 1);
                            const int y = std::clamp(static_cast<int>(std::lround((s.a.y - f.y0) / p)), 0, f.ny - 1);
                            const std::size_t k = f.idx(x, y, l);
                            if (src) sources.push_back(k);
                            else isTarget[k] = 1;
                        }
                    }
                }
                auto heuristic = [&](int x, int y) {
                    double best = 1e18;
                    const Pt q = f.pt(x, y);
                    for (const auto& t : targetCentres) {
                        const double dx = std::abs(q.x - t.x) / p, dy = std::abs(q.y - t.y) / p;
                        best = std::min(best, std::max(dx, dy) + 0.414 * std::min(dx, dy));
                    }
                    return static_cast<float>(best);
                };
                auto tryRoute = [&](double half, std::size_t& hit) {
                    ++gen;
                    while (!open.empty()) open.pop();
                    for (std::size_t k : sources) {
                        if (!passable(k, half)) continue;
                        stampOf[k] = gen;
                        g[k] = 0;
                        parent[k] = -1;
                        dirOf[k] = 255;
                        const int x = static_cast<int>(k % f.nx), y = static_cast<int>((k / f.nx) % f.ny);
                        open.push({heuristic(x, y), k});
                    }
                    const int dx8[8] = {1, -1, 0, 0, 1, 1, -1, -1};
                    const int dy8[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                    std::size_t expanded = 0;
                    while (!open.empty()) {
                        auto [fcost, k] = open.top();
                        open.pop();
                        const int x = static_cast<int>(k % f.nx), y = static_cast<int>((k / f.nx) % f.ny), l = static_cast<int>(k / (static_cast<std::size_t>(f.nx) * f.ny));
                        const float gk = g[k];
                        if (fcost - heuristic(x, y) > gk + 1e-3f) continue;  // stale entry
                        if (isTarget[k]) {
                            hit = k;
                            return true;
                        }
                        if ((++expanded & 0xFFFF) == 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(options.maxSeconds)) return false;
                        auto relax = [&](std::size_t nk, float cost, std::uint8_t dir) {
                            if (stampOf[nk] == gen && g[nk] <= cost) return;
                            stampOf[nk] = gen;
                            g[nk] = cost;
                            parent[nk] = static_cast<int>(k);
                            dirOf[nk] = dir;
                            const int nx = static_cast<int>(nk % f.nx), ny = static_cast<int>((nk / f.nx) % f.ny);
                            open.push({cost + heuristic(nx, ny), nk});
                        };
                        for (int d = 0; d < 8; ++d) {
                            const int nx = x + dx8[d], ny = y + dy8[d];
                            if (nx < 0 || ny < 0 || nx >= f.nx || ny >= f.ny) continue;
                            const std::size_t nk = f.idx(nx, ny, l);
                            if (!passable(nk, half)) continue;
                            if (d >= 4 && (!passable(f.idx(x + dx8[d], y, l), half) || !passable(f.idx(x, y + dy8[d], l), half))) continue;
                            float cost = gk + (d >= 4 ? 1.414f : 1.0f);
                            if (dirOf[k] != 255 && dirOf[k] != d) cost += 0.35f;
                            relax(nk, cost, static_cast<std::uint8_t>(d));
                        }
                        if (layers == 2) {
                            const std::size_t other = f.idx(x, y, 1 - l);
                            const double vr = rules.viaDiameter / 2;
                            if (f.dist(f.idx(x, y, 0), nid) >= rules.clearance + vr && f.dist(f.idx(x, y, 1), nid) >= rules.clearance + vr) {
                                relax(other, gk + static_cast<float>(options.viaCost), 8);
                            }
                        }
                    }
                    return false;
                };
                std::size_t hit = 0;
                double half = hw;
                bool ok = tryRoute(half, hit);
                if (!ok && power && rules.powerTrace > rules.trace) {
                    half = rules.trace / 2;
                    ok = tryRoute(half, hit);
                }
                if (!ok) {
                    std::string from, to;
                    if (!cu.padIndex.empty()) from = pads[static_cast<std::size_t>(cu.padIndex[0])].ref + "." + pads[static_cast<std::size_t>(cu.padIndex[0])].number;
                    for (std::size_t i = 0; i < cu.shapes.size(); ++i) {
                        if (cu.padIndex[i] >= 0 && uf.find(static_cast<int>(i)) != srcRoot) {
                            to = pads[static_cast<std::size_t>(cu.padIndex[i])].ref + "." + pads[static_cast<std::size_t>(cu.padIndex[i])].number;
                            break;
                        }
                    }
                    r.failedLinks.push_back(net + ": " + from + " -> " + to);
                    r.failed += static_cast<int>(roots.size()) - 1;
                    break;
                }
                // walk back
                std::vector<std::size_t> path;
                for (long k = static_cast<long>(hit); k >= 0; k = parent[static_cast<std::size_t>(k)]) path.push_back(static_cast<std::size_t>(k));
                std::reverse(path.begin(), path.end());
                auto layerOf = [&](std::size_t k) { return static_cast<int>(k / (static_cast<std::size_t>(f.nx) * f.ny)); };
                auto ptOf = [&](std::size_t k) { return f.pt(static_cast<int>(k % f.nx), static_cast<int>((k / f.nx) % f.ny)); };
                // extend into the pad centres at both ends when that stays inside the pad
                auto padAt = [&](Pt q, int l) -> std::optional<Pt> {
                    for (int i : netPads[net]) {
                        const auto& pd = pads[static_cast<std::size_t>(i)];
                        if (!(pd.layers & (1 << l))) continue;
                        if (geom::signedDistance(geom::padShape(pd), q) <= 1e-9) return pd.pos;
                    }
                    return std::nullopt;
                };
                std::vector<std::pair<Pt, int>> pts;
                for (std::size_t k : path) pts.push_back({ptOf(k), layerOf(k)});
                if (auto c0 = padAt(pts.front().first, pts.front().second)) pts.insert(pts.begin(), {*c0, pts.front().second});
                if (auto c1 = padAt(pts.back().first, pts.back().second)) pts.push_back({*c1, pts.back().second});
                // segments, merging straight runs
                const double width = half * 2;
                std::vector<Trace> newTraces;
                std::vector<Via> newVias;
                std::size_t runStart = 0;
                auto flush = [&](std::size_t a, std::size_t b) {
                    if (b <= a) return;
                    std::vector<Pt> run;
                    for (std::size_t i = a; i <= b; ++i) run.push_back(pts[i].first);
                    // drop collinear middle points
                    std::vector<Pt> simple{run.front()};
                    for (std::size_t i = 1; i + 1 < run.size(); ++i) {
                        const Pt u = simple.back(), v = run[i], w = run[i + 1];
                        const double cross = (v.x - u.x) * (w.y - v.y) - (v.y - u.y) * (w.x - v.x);
                        if (std::abs(cross) > 1e-9) simple.push_back(v);
                    }
                    simple.push_back(run.back());
                    for (std::size_t i = 1; i < simple.size(); ++i) {
                        if (std::hypot(simple[i].x - simple[i - 1].x, simple[i].y - simple[i - 1].y) < 1e-9) continue;
                        newTraces.push_back({simple[i - 1], simple[i], width, pts[a].second, net});
                    }
                };
                for (std::size_t i = 1; i < pts.size(); ++i) {
                    if (pts[i].second != pts[i - 1].second) {
                        flush(runStart, i - 1);
                        newVias.push_back({pts[i].first, rules.viaDrill, rules.viaDiameter, net});
                        runStart = i;
                    }
                }
                flush(runStart, pts.size() - 1);
                for (const auto& t : newTraces) {
                    c.board.traces.push_back(t);
                    f.stamp(geom::capsule(t.a, t.b, t.width / 2), t.layer == 0 ? 1 : 2, nid, reach);
                }
                for (const auto& v : newVias) {
                    c.board.vias.push_back(v);
                    f.stamp(geom::capsule(v.pos, v.pos, v.diameter / 2), 3, nid, reach);
                }
                r.segments += static_cast<int>(newTraces.size());
                r.vias += static_cast<int>(newVias.size());
                ++r.routed;
            }
        }
    };

    runPass(nets, rep);
    if (rep.failed > 0 && nets.size() > 1) {
        // second try: the nets that failed go first
        std::vector<std::string> order;
        std::set<std::string> failedNets;
        for (const auto& l : rep.failedLinks) failedNets.insert(l.substr(0, l.find(':')));
        for (const auto& n : nets) {
            if (failedNets.count(n)) order.push_back(n);
        }
        for (const auto& n : nets) {
            if (!failedNets.count(n)) order.push_back(n);
        }
        const auto firstTraces = c.board.traces;
        const auto firstVias = c.board.vias;
        RouteReport second;
        runPass(order, second);
        if (second.failed >= rep.failed) {
            c.board.traces = firstTraces;
            c.board.vias = firstVias;
        } else {
            rep = second;
        }
    }
    return rep;
}

}  // namespace achclwss::circuit
