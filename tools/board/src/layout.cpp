// Automatic layout: layered (Sugiyama: cycle removal, ranking, crossing
// reduction, coordinate assignment) for flowcharts, tidy trees, two-sided
// mind maps, radial trees, force-directed graphs and grids. Nodes move;
// connectors are attached, so they follow.
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <numeric>
#include <set>

#include "internal.hpp"

namespace achclwss::board {

using namespace detail;

namespace {

struct Node {
    std::string id;
    double w = 0, h = 0;      // real size
    double x = 0, y = 0;      // new top-left
    double ox = 0, oy = 0;    // old top-left
    bool dummy = false;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<std::pair<int, int>> edges;  // unique, no self loops
    std::vector<std::vector<int>> out, in;
    void build() {
        out.assign(nodes.size(), {});
        in.assign(nodes.size(), {});
        for (auto [a, b] : edges) {
            out[static_cast<std::size_t>(a)].push_back(b);
            in[static_cast<std::size_t>(b)].push_back(a);
        }
    }
    std::vector<int> neighbors(int v) const {
        std::vector<int> n = out[static_cast<std::size_t>(v)];
        n.insert(n.end(), in[static_cast<std::size_t>(v)].begin(), in[static_cast<std::size_t>(v)].end());
        return n;
    }
};

// The element a connector end moves with: its outermost group, if any.
std::string unitOf(const Board& board, const std::string& id) {
    const Element* e = board.get(id);
    std::string unit = e ? e->id : id;
    for (int depth = 0; e && !e->parent.empty() && depth < 32; ++depth) {
        const Element* p = board.get(e->parent);
        if (!p) break;
        if (p->kind == Kind::Group) unit = p->id;
        e = p;
    }
    return unit;
}

bool insideGroup(const Board& board, const Element& e) {
    const Element* p = e.parent.empty() ? nullptr : board.get(e.parent);
    for (int depth = 0; p && depth < 32; ++depth) {
        if (p->kind == Kind::Group) return true;
        p = p->parent.empty() ? nullptr : board.get(p->parent);
    }
    return false;
}

Graph buildGraph(const Board& board, const LayoutOptions& o, std::string& error) {
    Graph g;
    std::map<std::string, int> index;
    auto addNode = [&](const Element& e) {
        if (index.count(e.id)) return;
        const Box b = elementBox(board, e);
        Node n;
        n.id = e.id;
        n.w = std::max(1.0, b.w());
        n.h = std::max(1.0, b.h());
        n.ox = b.x0;
        n.oy = b.y0;
        index[e.id] = static_cast<int>(g.nodes.size());
        g.nodes.push_back(n);
    };
    std::set<std::string> connected;
    for (const auto& c : board.elements) {
        if (c.kind != Kind::Connector || c.from.empty() || c.to.empty()) continue;
        connected.insert(unitOf(board, c.from));
        connected.insert(unitOf(board, c.to));
    }
    if (!o.ids.empty()) {
        for (const auto& id : o.ids) {
            const Element* e = board.get(id);
            if (!e) {
                error = "no element '" + id + "'";
                return g;
            }
            if (!isNode(e->kind)) continue;
            addNode(*board.get(unitOf(board, e->id)));
        }
    } else {
        for (const auto& e : board.elements) {
            if (!isNode(e.kind) || e.kind == Kind::Frame || insideGroup(board, e)) continue;
            const Layer* l = board.layer(e.layer);
            if (l && l->locked) continue;
            if (!connected.empty() && !connected.count(e.id)) continue;
            addNode(e);
        }
    }
    std::set<std::pair<int, int>> seen;
    for (const auto& c : board.elements) {
        if (c.kind != Kind::Connector || c.from.empty() || c.to.empty()) continue;
        auto a = index.find(unitOf(board, c.from)), b = index.find(unitOf(board, c.to));
        if (a == index.end() || b == index.end() || a->second == b->second) continue;
        if (seen.insert({a->second, b->second}).second) g.edges.push_back({a->second, b->second});
    }
    g.build();
    return g;
}

bool horizontal(const std::string& dir) { return dir == "LR" || dir == "RL"; }

// ---- layered --------------------------------------------------------------------------------

int crossings(const std::vector<int>& upper, const std::vector<int>& lower, const std::vector<std::vector<int>>& down,
              const std::vector<int>& pos) {
    std::vector<std::pair<int, int>> e;
    for (int u : upper) {
        for (int v : down[static_cast<std::size_t>(u)]) e.push_back({pos[static_cast<std::size_t>(u)], pos[static_cast<std::size_t>(v)]});
    }
    (void)lower;
    int c = 0;
    for (std::size_t i = 0; i < e.size(); ++i) {
        for (std::size_t j = i + 1; j < e.size(); ++j) {
            if ((e[i].first - e[j].first) * (e[i].second - e[j].second) < 0) ++c;
        }
    }
    return c;
}

void layered(Graph& g, const LayoutOptions& o) {
    const int n = static_cast<int>(g.nodes.size());
    const bool horiz = horizontal(o.direction);
    // 1. break cycles: DFS, reverse back edges
    std::vector<std::pair<int, int>> dag;
    {
        std::vector<int> state(static_cast<std::size_t>(n), 0);
        std::set<std::pair<int, int>> back;
        std::vector<int> order(static_cast<std::size_t>(n));
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return g.in[static_cast<std::size_t>(a)].empty() > g.in[static_cast<std::size_t>(b)].empty();
        });
        for (int root : order) {
            if (state[static_cast<std::size_t>(root)]) continue;
            std::vector<std::pair<int, std::size_t>> stack{{root, 0}};
            state[static_cast<std::size_t>(root)] = 1;
            while (!stack.empty()) {
                auto& [v, k] = stack.back();
                const auto& outs = g.out[static_cast<std::size_t>(v)];
                if (k < outs.size()) {
                    const int w = outs[k++];
                    if (state[static_cast<std::size_t>(w)] == 1) back.insert({v, w});
                    else if (state[static_cast<std::size_t>(w)] == 0) {
                        state[static_cast<std::size_t>(w)] = 1;
                        stack.push_back({w, 0});
                    }
                } else {
                    state[static_cast<std::size_t>(v)] = 2;
                    stack.pop_back();
                }
            }
        }
        std::set<std::pair<int, int>> have;
        for (auto e : g.edges) {
            auto d = back.count(e) ? std::make_pair(e.second, e.first) : e;
            if (have.insert(d).second) dag.push_back(d);
        }
    }
    std::vector<std::vector<int>> out(static_cast<std::size_t>(n)), in(static_cast<std::size_t>(n));
    for (auto [a, b] : dag) {
        out[static_cast<std::size_t>(a)].push_back(b);
        in[static_cast<std::size_t>(b)].push_back(a);
    }
    // 2. ranks: longest path from the sources, then sources pulled down next to their children
    std::vector<int> rank(static_cast<std::size_t>(n), 0), indeg(static_cast<std::size_t>(n));
    std::deque<int> q;
    for (int v = 0; v < n; ++v) {
        indeg[static_cast<std::size_t>(v)] = static_cast<int>(in[static_cast<std::size_t>(v)].size());
        if (!indeg[static_cast<std::size_t>(v)]) q.push_back(v);
    }
    std::vector<int> topo;
    while (!q.empty()) {
        const int v = q.front();
        q.pop_front();
        topo.push_back(v);
        for (int w : out[static_cast<std::size_t>(v)]) {
            rank[static_cast<std::size_t>(w)] = std::max(rank[static_cast<std::size_t>(w)], rank[static_cast<std::size_t>(v)] + 1);
            if (--indeg[static_cast<std::size_t>(w)] == 0) q.push_back(w);
        }
    }
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
        const int v = *it;
        if (!in[static_cast<std::size_t>(v)].empty() || out[static_cast<std::size_t>(v)].empty()) continue;
        int m = 1 << 30;
        for (int w : out[static_cast<std::size_t>(v)]) m = std::min(m, rank[static_cast<std::size_t>(w)]);
        rank[static_cast<std::size_t>(v)] = std::max(0, m - 1);
    }
    // 3. dummy nodes split edges longer than one rank
    std::vector<int> nrank = rank;
    std::vector<double> cw(static_cast<std::size_t>(n)), rw(static_cast<std::size_t>(n));  // cross-axis, rank-axis sizes
    for (int v = 0; v < n; ++v) {
        cw[static_cast<std::size_t>(v)] = horiz ? g.nodes[static_cast<std::size_t>(v)].h : g.nodes[static_cast<std::size_t>(v)].w;
        rw[static_cast<std::size_t>(v)] = horiz ? g.nodes[static_cast<std::size_t>(v)].w : g.nodes[static_cast<std::size_t>(v)].h;
    }
    std::vector<std::vector<int>> down(static_cast<std::size_t>(n)), up(static_cast<std::size_t>(n));
    int total = n;
    for (auto [a, b] : dag) {
        int prev = a;
        for (int r = rank[static_cast<std::size_t>(a)] + 1; r < rank[static_cast<std::size_t>(b)]; ++r) {
            const int d = total++;
            nrank.push_back(r);
            cw.push_back(16);
            rw.push_back(0);
            down.emplace_back();
            up.emplace_back();
            down[static_cast<std::size_t>(prev)].push_back(d);
            up[static_cast<std::size_t>(d)].push_back(prev);
            prev = d;
        }
        down[static_cast<std::size_t>(prev)].push_back(b);
        up[static_cast<std::size_t>(b)].push_back(prev);
    }
    int ranks = 0;
    for (int r : nrank) ranks = std::max(ranks, r + 1);
    std::vector<std::vector<int>> layers(static_cast<std::size_t>(ranks));
    // initial order: BFS from the sources keeps families together
    {
        std::vector<char> seen(static_cast<std::size_t>(total), 0);
        std::deque<int> bq;
        for (int v : topo) {
            if (in[static_cast<std::size_t>(v)].empty()) {
                bq.push_back(v);
                seen[static_cast<std::size_t>(v)] = 1;
            }
        }
        for (int v = 0; v < total; ++v) {
            if (bq.empty() && !seen[static_cast<std::size_t>(v)]) {
                bq.push_back(v);
                seen[static_cast<std::size_t>(v)] = 1;
            }
            while (!bq.empty()) {
                const int u = bq.front();
                bq.pop_front();
                layers[static_cast<std::size_t>(nrank[static_cast<std::size_t>(u)])].push_back(u);
                for (int w : down[static_cast<std::size_t>(u)]) {
                    if (!seen[static_cast<std::size_t>(w)]) {
                        seen[static_cast<std::size_t>(w)] = 1;
                        bq.push_back(w);
                    }
                }
            }
        }
    }
    std::vector<int> pos(static_cast<std::size_t>(total), 0);
    auto setPos = [&] {
        for (auto& l : layers) {
            for (std::size_t i = 0; i < l.size(); ++i) pos[static_cast<std::size_t>(l[i])] = static_cast<int>(i);
        }
    };
    setPos();
    auto countAll = [&] {
        int c = 0;
        for (std::size_t r = 0; r + 1 < layers.size(); ++r) c += crossings(layers[r], layers[r + 1], down, pos);
        return c;
    };
    // 4. crossing reduction: barycentre sweeps, keep the best
    auto best = layers;
    int bestCross = countAll();
    for (int sweep = 0; sweep < 12 && bestCross > 0; ++sweep) {
        const bool goingDown = sweep % 2 == 0;
        for (std::size_t k = 1; k < layers.size(); ++k) {
            const std::size_t r = goingDown ? k : layers.size() - 1 - k;
            auto& l = layers[r];
            std::vector<std::pair<double, int>> keyed;
            for (int v : l) {
                const auto& nb = goingDown ? up[static_cast<std::size_t>(v)] : down[static_cast<std::size_t>(v)];
                double bc = pos[static_cast<std::size_t>(v)];
                if (!nb.empty()) {
                    bc = 0;
                    for (int w : nb) bc += pos[static_cast<std::size_t>(w)];
                    bc /= static_cast<double>(nb.size());
                }
                keyed.push_back({bc, v});
            }
            std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < l.size(); ++i) l[i] = keyed[i].second;
            for (std::size_t i = 0; i < l.size(); ++i) pos[static_cast<std::size_t>(l[i])] = static_cast<int>(i);
        }
        const int c = countAll();
        if (c < bestCross) {
            bestCross = c;
            best = layers;
        }
    }
    layers = best;
    setPos();
    // 5. coordinates across the rank: pack, then pull towards neighbours without overlapping
    std::vector<double> cx(static_cast<std::size_t>(total), 0);
    for (auto& l : layers) {
        double cur = 0;
        for (int v : l) {
            cx[static_cast<std::size_t>(v)] = cur + cw[static_cast<std::size_t>(v)] / 2;
            cur += cw[static_cast<std::size_t>(v)] + o.gapX;
        }
        const double shift = (cur - o.gapX) / 2;
        for (int v : l) cx[static_cast<std::size_t>(v)] -= shift;
    }
    auto pack = [&](std::vector<int>& l, const std::vector<double>& want) {
        // place at wanted centres, then remove overlaps in order, spreading the push both ways
        for (std::size_t i = 0; i < l.size(); ++i) cx[static_cast<std::size_t>(l[i])] = want[i];
        for (int it = 0; it < 30; ++it) {
            bool moved = false;
            for (std::size_t i = 1; i < l.size(); ++i) {
                const int a = l[i - 1], b = l[i];
                const double need = (cw[static_cast<std::size_t>(a)] + cw[static_cast<std::size_t>(b)]) / 2 + o.gapX;
                const double have = cx[static_cast<std::size_t>(b)] - cx[static_cast<std::size_t>(a)];
                if (have < need - 1e-6) {
                    const double d = (need - have) / 2;
                    cx[static_cast<std::size_t>(a)] -= d;
                    cx[static_cast<std::size_t>(b)] += d;
                    moved = true;
                }
            }
            if (!moved) break;
        }
        for (std::size_t i = 1; i < l.size(); ++i) {  // final strict pass
            const int a = l[i - 1], b = l[i];
            const double need = (cw[static_cast<std::size_t>(a)] + cw[static_cast<std::size_t>(b)]) / 2 + o.gapX;
            cx[static_cast<std::size_t>(b)] = std::max(cx[static_cast<std::size_t>(b)], cx[static_cast<std::size_t>(a)] + need);
        }
    };
    for (int it = 0; it < 17; ++it) {
        const bool goingDown = it % 2 == 0;
        for (std::size_t k = 0; k < layers.size(); ++k) {
            const std::size_t r = goingDown ? k : layers.size() - 1 - k;
            auto& l = layers[r];
            std::vector<double> want;
            for (int v : l) {
                const auto& a = goingDown ? up[static_cast<std::size_t>(v)] : down[static_cast<std::size_t>(v)];
                const auto& b = goingDown ? down[static_cast<std::size_t>(v)] : up[static_cast<std::size_t>(v)];
                const auto& nb = !a.empty() ? a : b;
                if (nb.empty()) {
                    want.push_back(cx[static_cast<std::size_t>(v)]);
                    continue;
                }
                std::vector<double> xs;
                for (int w : nb) xs.push_back(cx[static_cast<std::size_t>(w)]);
                std::sort(xs.begin(), xs.end());
                const double med = xs.size() % 2 ? xs[xs.size() / 2] : (xs[xs.size() / 2 - 1] + xs[xs.size() / 2]) / 2;
                want.push_back(med);
            }
            pack(l, want);
        }
    }
    // 6. rank positions
    std::vector<double> rankSize(static_cast<std::size_t>(ranks), 0), rankPos(static_cast<std::size_t>(ranks), 0);
    for (int v = 0; v < total; ++v) {
        rankSize[static_cast<std::size_t>(nrank[static_cast<std::size_t>(v)])] =
            std::max(rankSize[static_cast<std::size_t>(nrank[static_cast<std::size_t>(v)])], rw[static_cast<std::size_t>(v)]);
    }
    for (int r = 1; r < ranks; ++r) rankPos[static_cast<std::size_t>(r)] = rankPos[static_cast<std::size_t>(r - 1)] + rankSize[static_cast<std::size_t>(r - 1)] + o.gapY;
    const double span = ranks ? rankPos.back() + rankSize.back() : 0;
    for (int v = 0; v < n; ++v) {
        Node& nd = g.nodes[static_cast<std::size_t>(v)];
        const int r = nrank[static_cast<std::size_t>(v)];
        double along = rankPos[static_cast<std::size_t>(r)] + (rankSize[static_cast<std::size_t>(r)] - rw[static_cast<std::size_t>(v)]) / 2;
        if (o.direction == "BT" || o.direction == "RL") along = span - along - rw[static_cast<std::size_t>(v)];
        const double across = cx[static_cast<std::size_t>(v)] - cw[static_cast<std::size_t>(v)] / 2;
        nd.x = horiz ? along : across;
        nd.y = horiz ? across : along;
    }
}

// ---- trees ----------------------------------------------------------------------------------

struct Tree {
    std::vector<int> roots;
    std::vector<std::vector<int>> kids;
};

Tree spanningTree(const Graph& g) {
    const int n = static_cast<int>(g.nodes.size());
    Tree t;
    t.kids.assign(static_cast<std::size_t>(n), {});
    std::vector<char> seen(static_cast<std::size_t>(n), 0);
    std::vector<int> order;
    for (int v = 0; v < n; ++v) {
        if (g.in[static_cast<std::size_t>(v)].empty()) order.push_back(v);
    }
    for (int v = 0; v < n; ++v) order.push_back(v);
    for (int r : order) {
        if (seen[static_cast<std::size_t>(r)]) continue;
        t.roots.push_back(r);
        seen[static_cast<std::size_t>(r)] = 1;
        std::deque<int> q{r};
        while (!q.empty()) {
            const int v = q.front();
            q.pop_front();
            std::vector<int> nb = g.out[static_cast<std::size_t>(v)];
            for (int w : g.in[static_cast<std::size_t>(v)]) nb.push_back(w);  // undirected fallback
            for (int w : nb) {
                if (seen[static_cast<std::size_t>(w)]) continue;
                seen[static_cast<std::size_t>(w)] = 1;
                t.kids[static_cast<std::size_t>(v)].push_back(w);
                q.push_back(w);
            }
        }
    }
    return t;
}

// Tidy-ish tree: each subtree gets the cross-axis room its leaves need; parents centre over children.
void tree(Graph& g, const LayoutOptions& o) {
    const bool horiz = horizontal(o.direction);
    Tree t = spanningTree(g);
    const std::size_t n = g.nodes.size();
    auto cross = [&](int v) { return horiz ? g.nodes[static_cast<std::size_t>(v)].h : g.nodes[static_cast<std::size_t>(v)].w; };
    auto along = [&](int v) { return horiz ? g.nodes[static_cast<std::size_t>(v)].w : g.nodes[static_cast<std::size_t>(v)].h; };
    std::vector<double> extent(n, 0);
    std::vector<int> depth(n, 0);
    std::function<double(int, int)> measure = [&](int v, int d) {
        depth[static_cast<std::size_t>(v)] = d;
        double sum = 0;
        for (std::size_t i = 0; i < t.kids[static_cast<std::size_t>(v)].size(); ++i) {
            sum += measure(t.kids[static_cast<std::size_t>(v)][i], d + 1) + (i ? o.gapX : 0);
        }
        return extent[static_cast<std::size_t>(v)] = std::max(cross(v), sum);
    };
    double offset = 0;
    std::vector<double> cpos(n, 0);
    std::function<void(int, double)> place = [&](int v, double start) {
        const auto& ks = t.kids[static_cast<std::size_t>(v)];
        double sum = 0;
        for (std::size_t i = 0; i < ks.size(); ++i) sum += extent[static_cast<std::size_t>(ks[i])] + (i ? o.gapX : 0);
        double cur = start + (extent[static_cast<std::size_t>(v)] - sum) / 2;
        for (int k : ks) {
            place(k, cur);
            cur += extent[static_cast<std::size_t>(k)] + o.gapX;
        }
        if (ks.empty()) cpos[static_cast<std::size_t>(v)] = start + extent[static_cast<std::size_t>(v)] / 2;
        else cpos[static_cast<std::size_t>(v)] = (cpos[static_cast<std::size_t>(ks.front())] + cpos[static_cast<std::size_t>(ks.back())]) / 2;
    };
    for (int r : t.roots) {
        measure(r, 0);
        place(r, offset);
        offset += extent[static_cast<std::size_t>(r)] + o.gapX * 2;
    }
    int maxDepth = 0;
    for (int d : depth) maxDepth = std::max(maxDepth, d);
    std::vector<double> size(static_cast<std::size_t>(maxDepth + 1), 0), at(static_cast<std::size_t>(maxDepth + 1), 0);
    for (std::size_t v = 0; v < n; ++v) size[static_cast<std::size_t>(depth[v])] = std::max(size[static_cast<std::size_t>(depth[v])], along(static_cast<int>(v)));
    for (int d = 1; d <= maxDepth; ++d) at[static_cast<std::size_t>(d)] = at[static_cast<std::size_t>(d - 1)] + size[static_cast<std::size_t>(d - 1)] + o.gapY;
    const double span = at.back() + size.back();
    for (std::size_t v = 0; v < n; ++v) {
        Node& nd = g.nodes[v];
        double a = at[static_cast<std::size_t>(depth[v])] + (size[static_cast<std::size_t>(depth[v])] - along(static_cast<int>(v))) / 2;
        if (o.direction == "BT" || o.direction == "RL") a = span - a - along(static_cast<int>(v));
        const double c = cpos[v] - cross(static_cast<int>(v)) / 2;
        nd.x = horiz ? a : c;
        nd.y = horiz ? c : a;
    }
}

// Two-sided mind map: the root in the middle, branches alternate right and left (balanced by size).
void mindmap(Graph& g, const LayoutOptions& o) {
    Tree t = spanningTree(g);
    const std::size_t n = g.nodes.size();
    const double gx = o.gapX, gy = std::max(8.0, o.gapY * 0.25);
    std::vector<double> extent(n, 0);
    std::function<double(int)> measure = [&](int v) {
        double sum = 0;
        const auto& ks = t.kids[static_cast<std::size_t>(v)];
        for (std::size_t i = 0; i < ks.size(); ++i) sum += measure(ks[i]) + (i ? gy : 0);
        return extent[static_cast<std::size_t>(v)] = std::max(g.nodes[static_cast<std::size_t>(v)].h, sum);
    };
    std::function<void(int, double, double, int)> place = [&](int v, double edgeX, double top, int side) {
        Node& nd = g.nodes[static_cast<std::size_t>(v)];
        nd.x = side > 0 ? edgeX : edgeX - nd.w;
        nd.y = top + (extent[static_cast<std::size_t>(v)] - nd.h) / 2;
        const auto& ks = t.kids[static_cast<std::size_t>(v)];
        double sum = 0;
        for (std::size_t i = 0; i < ks.size(); ++i) sum += extent[static_cast<std::size_t>(ks[i])] + (i ? gy : 0);
        double cur = top + (extent[static_cast<std::size_t>(v)] - sum) / 2;
        const double childEdge = side > 0 ? nd.x + nd.w + gx : nd.x - gx;
        for (int k : ks) {
            place(k, childEdge, cur, side);
            cur += extent[static_cast<std::size_t>(k)] + gy;
        }
    };
    double offsetY = 0;
    for (int r : t.roots) {
        measure(r);
        Node& root = g.nodes[static_cast<std::size_t>(r)];
        std::vector<int> right, left;
        double hr = 0, hl = 0;
        for (int k : t.kids[static_cast<std::size_t>(r)]) {
            if (hr <= hl) {
                right.push_back(k);
                hr += extent[static_cast<std::size_t>(k)] + gy;
            } else {
                left.push_back(k);
                hl += extent[static_cast<std::size_t>(k)] + gy;
            }
        }
        const double height = std::max({root.h, hr - gy, hl - gy});
        root.x = 0;
        root.y = offsetY + (height - root.h) / 2;
        auto side = [&](const std::vector<int>& ks, double h, int dir) {
            double cur = offsetY + (height - (h - gy)) / 2;
            for (int k : ks) {
                place(k, dir > 0 ? root.x + root.w + gx * 1.3 : root.x - gx * 1.3, cur, dir);
                cur += extent[static_cast<std::size_t>(k)] + gy;
            }
        };
        side(right, hr, 1);
        side(left, hl, -1);
        offsetY += height + o.gapY * 2;
    }
}

// Radial tree: depth d on a circle, each subtree a wedge sized by its leaves.
void radial(Graph& g, const LayoutOptions& o) {
    Tree t = spanningTree(g);
    const std::size_t n = g.nodes.size();
    std::vector<double> leaves(n, 1);
    std::vector<int> depth(n, 0);
    std::function<double(int, int)> count = [&](int v, int d) {
        depth[static_cast<std::size_t>(v)] = d;
        const auto& ks = t.kids[static_cast<std::size_t>(v)];
        if (ks.empty()) return leaves[static_cast<std::size_t>(v)] = 1;
        double s = 0;
        for (int k : ks) s += count(k, d + 1);
        return leaves[static_cast<std::size_t>(v)] = s;
    };
    double maxSize = 0;
    for (const auto& nd : g.nodes) maxSize = std::max({maxSize, nd.w, nd.h});
    const double ring = maxSize + o.gapY;
    double offsetX = 0;
    for (int r : t.roots) {
        count(r, 0);
        std::function<void(int, double, double)> place = [&](int v, double a0, double a1) {
            Node& nd = g.nodes[static_cast<std::size_t>(v)];
            const double rr = ring * depth[static_cast<std::size_t>(v)];
            const double a = (a0 + a1) / 2;
            nd.x = offsetX + rr * std::cos(a) - nd.w / 2;
            nd.y = rr * std::sin(a) - nd.h / 2;
            double cur = a0;
            for (int k : t.kids[static_cast<std::size_t>(v)]) {
                const double span = (a1 - a0) * leaves[static_cast<std::size_t>(k)] / leaves[static_cast<std::size_t>(v)];
                place(k, cur, cur + span);
                cur += span;
            }
        };
        place(r, -kPi, kPi);
        int md = 0;
        for (std::size_t v = 0; v < n; ++v) md = std::max(md, depth[v]);
        offsetX += ring * (2 * md + 1) + o.gapX;
    }
}

// ---- force-directed -------------------------------------------------------------------------

void removeOverlaps(Graph& g, double gap) {
    const std::size_t n = g.nodes.size();
    for (int it = 0; it < 200; ++it) {
        bool any = false;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                Node& a = g.nodes[i];
                Node& b = g.nodes[j];
                const double ox = std::min(a.x + a.w, b.x + b.w) + gap - std::max(a.x, b.x);
                const double oy = std::min(a.y + a.h, b.y + b.h) + gap - std::max(a.y, b.y);
                if (ox <= 0 || oy <= 0) continue;
                any = true;
                if (ox < oy) {
                    const double s = (a.x + a.w / 2 < b.x + b.w / 2 ? -1 : 1) * ox / 2;
                    a.x += s;
                    b.x -= s;
                } else {
                    const double s = (a.y + a.h / 2 < b.y + b.h / 2 ? -1 : 1) * oy / 2;
                    a.y += s;
                    b.y -= s;
                }
            }
        }
        if (!any) break;
    }
}

void force(Graph& g, const LayoutOptions& o) {
    const std::size_t n = g.nodes.size();
    if (n == 0) return;
    Rng rng(o.seed);
    double area = 0;
    for (const auto& nd : g.nodes) area += (nd.w + o.gapX) * (nd.h + o.gapY);
    const double k = std::sqrt(area / static_cast<double>(n)) * 0.9;
    std::vector<double> px(n), py(n), rad(n);
    // start on a spiral in BFS order so connected nodes begin near each other
    Tree t = spanningTree(g);
    std::vector<int> order;
    for (int r : t.roots) {
        std::deque<int> q{r};
        while (!q.empty()) {
            const int v = q.front();
            q.pop_front();
            order.push_back(v);
            for (int c : t.kids[static_cast<std::size_t>(v)]) q.push_back(c);
        }
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        const double a = static_cast<double>(i) * 2.399963, r = k * std::sqrt(static_cast<double>(i)) * 0.8;
        const auto v = static_cast<std::size_t>(order[i]);
        px[v] = r * std::cos(a) + rng.range(-1, 1);
        py[v] = r * std::sin(a) + rng.range(-1, 1);
    }
    for (std::size_t v = 0; v < n; ++v) rad[v] = std::hypot(g.nodes[v].w, g.nodes[v].h) / 2;
    double temp = k * 2;
    const int iters = std::clamp(o.iterations, 10, 5000);
    std::vector<double> dx(n), dy(n);
    for (int it = 0; it < iters; ++it) {
        std::fill(dx.begin(), dx.end(), 0);
        std::fill(dy.begin(), dy.end(), 0);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                double ddx = px[i] - px[j], ddy = py[i] - py[j];
                double d = std::hypot(ddx, ddy);
                if (d < 1e-3) {
                    ddx = rng.range(-1, 1);
                    ddy = rng.range(-1, 1);
                    d = std::hypot(ddx, ddy) + 1e-3;
                }
                const double eff = std::max(1.0, d - (rad[i] + rad[j]) * 0.6);
                const double f = k * k / eff;
                dx[i] += ddx / d * f;
                dy[i] += ddy / d * f;
                dx[j] -= ddx / d * f;
                dy[j] -= ddy / d * f;
            }
        }
        for (auto [a, b] : g.edges) {
            const auto ia = static_cast<std::size_t>(a), ib = static_cast<std::size_t>(b);
            const double ddx = px[ia] - px[ib], ddy = py[ia] - py[ib];
            const double d = std::max(1e-3, std::hypot(ddx, ddy));
            const double eff = std::max(1.0, d - (rad[ia] + rad[ib]) * 0.6);
            const double f = eff * eff / k;
            dx[ia] -= ddx / d * f;
            dy[ia] -= ddy / d * f;
            dx[ib] += ddx / d * f;
            dy[ib] += ddy / d * f;
        }
        for (std::size_t i = 0; i < n; ++i) {
            // gentle pull to the middle keeps separate pieces together
            dx[i] -= px[i] * 0.01;
            dy[i] -= py[i] * 0.01;
            const double d = std::hypot(dx[i], dy[i]);
            if (d > 0) {
                px[i] += dx[i] / d * std::min(d, temp);
                py[i] += dy[i] / d * std::min(d, temp);
            }
        }
        temp = std::max(k * 0.01, temp * 0.985);
    }
    for (std::size_t v = 0; v < n; ++v) {
        g.nodes[v].x = px[v] - g.nodes[v].w / 2;
        g.nodes[v].y = py[v] - g.nodes[v].h / 2;
    }
    removeOverlaps(g, std::min(o.gapX, o.gapY) * 0.5);
}

void grid(Graph& g, const LayoutOptions& o) {
    const std::size_t n = g.nodes.size();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    // reading order of where they are now (rows of similar y)
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const Node &na = g.nodes[a], &nb = g.nodes[b];
        if (std::abs(na.oy - nb.oy) > std::min(na.h, nb.h) / 2) return na.oy < nb.oy;
        return na.ox < nb.ox;
    });
    const int cols = o.columns > 0 ? o.columns : std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));
    double cw = 0, ch = 0;
    for (const auto& nd : g.nodes) {
        cw = std::max(cw, nd.w);
        ch = std::max(ch, nd.h);
    }
    for (std::size_t i = 0; i < n; ++i) {
        Node& nd = g.nodes[order[i]];
        const int c = static_cast<int>(i) % cols, r = static_cast<int>(i) / cols;
        nd.x = c * (cw + o.gapX) + (cw - nd.w) / 2;
        nd.y = r * (ch + o.gapY) + (ch - nd.h) / 2;
    }
}

}  // namespace

int layout(Board& board, const LayoutOptions& options, std::string& error) {
    LayoutOptions o = options;
    o.direction = lower(o.direction);
    for (auto& c : o.direction) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (o.direction == "TD") o.direction = "TB";
    if (o.direction != "TB" && o.direction != "BT" && o.direction != "LR" && o.direction != "RL") {
        error = "direction must be TB, BT, LR or RL";
        return -1;
    }
    Graph g = buildGraph(board, o, error);
    if (!error.empty()) return -1;
    if (g.nodes.empty()) return 0;
    const std::string alg = lower(o.algorithm);
    if (alg == "layered" || alg == "flowchart" || alg == "sugiyama") layered(g, o);
    else if (alg == "tree") tree(g, o);
    else if (alg == "mindmap") mindmap(g, o);
    else if (alg == "radial") radial(g, o);
    else if (alg == "force") force(g, o);
    else if (alg == "grid") grid(g, o);
    else {
        error = "unknown layout '" + o.algorithm + "' (layered, tree, mindmap, radial, force, grid)";
        return -1;
    }
    double minX = 1e300, minY = 1e300, oldX = 1e300, oldY = 1e300;
    for (const auto& nd : g.nodes) {
        minX = std::min(minX, nd.x);
        minY = std::min(minY, nd.y);
        oldX = std::min(oldX, nd.ox);
        oldY = std::min(oldY, nd.oy);
    }
    const double baseX = o.keepOrigin ? oldX : o.originX, baseY = o.keepOrigin ? oldY : o.originY;
    for (const auto& nd : g.nodes) {
        Element* e = board.get(nd.id);
        if (!e) continue;
        const double tx = std::round(nd.x - minX + baseX), ty = std::round(nd.y - minY + baseY);
        moveBy(board, *e, tx - nd.ox, ty - nd.oy);
    }
    refreshGroups(board, o.fitFrames);
    return static_cast<int>(g.nodes.size());
}

}  // namespace achclwss::board
