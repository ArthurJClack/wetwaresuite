// The link graph: building it, a force layout, JSON/DOT and a PNG picture.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <random>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "wetwaresuite/notes/notes.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::notes {

Graph buildGraph(const Vault& vault, const GraphOptions& o) {
    Graph g;
    std::unordered_map<std::string, int> index;
    auto node = [&](const std::string& id, const std::string& label, const std::string& kind) {
        auto it = index.find(id);
        if (it != index.end()) return it->second;
        int k = static_cast<int>(g.nodes.size());
        g.nodes.push_back({id, label, kind, 0, 0, 0});
        index.emplace(id, k);
        return k;
    };
    std::set<std::pair<int, int>> seen;
    auto edge = [&](int a, int b) {
        if (a == b || !seen.insert({a, b}).second) return;
        g.edges.emplace_back(a, b);
    };
    const std::string pathFilter = lower(o.path);
    auto included = [&](const Note& n) { return pathFilter.empty() || lower(n.rel).find(pathFilter) != std::string::npos; };

    // which notes: everything, or a neighbourhood of the focus note
    std::set<std::string> keep;
    if (!o.focus.empty()) {
        std::deque<std::pair<std::string, int>> queue{{o.focus, 0}};
        keep.insert(o.focus);
        while (!queue.empty()) {
            auto [rel, d] = queue.front();
            queue.pop_front();
            if (d >= o.depth) continue;
            std::vector<std::string> next;
            if (const Note* n = vault.note(rel)) {
                for (const auto& l : n->links) {
                    std::string to = vault.resolve(l, rel);
                    if (!to.empty() && vault.isNote(to)) next.push_back(to);
                }
            }
            for (const auto& [from, link] : vault.backlinks(rel)) next.push_back(from->rel);
            for (const auto& r : next) {
                if (keep.insert(r).second) queue.emplace_back(r, d + 1);
            }
        }
    }
    for (const auto& [rel, n] : vault.notes) {
        if (!o.focus.empty() && !keep.count(rel)) continue;
        if (!included(n)) continue;
        node(rel, n.title, "note");
    }
    const std::size_t noteCount = g.nodes.size();
    for (std::size_t i = 0; i < noteCount; ++i) {
        const std::string rel = g.nodes[i].id;
        const Note& n = *vault.note(rel);
        for (const auto& l : n.links) {
            std::string to = vault.resolve(l, rel);
            if (to.empty()) {
                if (o.unresolved && !l.target.empty()) edge(static_cast<int>(i), node("?" + lower(l.target), l.target, "unresolved"));
                continue;
            }
            if (vault.isNote(to)) {
                if (auto it = index.find(to); it != index.end()) edge(static_cast<int>(i), it->second);
            } else if (o.attachments) {
                auto slash = to.rfind('/');
                edge(static_cast<int>(i), node(to, slash == std::string::npos ? to : to.substr(slash + 1), "attachment"));
            }
        }
        if (o.tags) {
            for (const auto& t : n.tags) edge(static_cast<int>(i), node("tag:" + lower(t), "#" + t, "tag"));
        }
    }
    for (const auto& [a, b] : g.edges) {
        ++g.nodes[static_cast<std::size_t>(a)].degree;
        ++g.nodes[static_cast<std::size_t>(b)].degree;
    }
    if (!o.orphans) {
        Graph f;
        std::vector<int> remap(g.nodes.size(), -1);
        for (std::size_t i = 0; i < g.nodes.size(); ++i) {
            if (g.nodes[i].degree == 0 && g.nodes[i].id != o.focus) continue;
            remap[i] = static_cast<int>(f.nodes.size());
            f.nodes.push_back(g.nodes[i]);
        }
        for (const auto& [a, b] : g.edges) f.edges.emplace_back(remap[static_cast<std::size_t>(a)], remap[static_cast<std::size_t>(b)]);
        g = std::move(f);
    }
    return g;
}

void layoutGraph(Graph& g, int iterations, unsigned seed) {
    const std::size_t n = g.nodes.size();
    if (n == 0) return;
    if (n == 1) {
        g.nodes[0].x = g.nodes[0].y = 0;
        return;
    }
    if (iterations <= 0) iterations = n > 2000 ? 60 : n > 500 ? 150 : 300;
    // start on a jittered spiral so the result is the same every run
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(-0.5, 0.5);
    const double area = static_cast<double>(n) * 100.0 * 100.0;
    const double k = std::sqrt(area / static_cast<double>(n));
    for (std::size_t i = 0; i < n; ++i) {
        double a = static_cast<double>(i) * 2.39996;
        double r = k * std::sqrt(static_cast<double>(i) + 1) * 0.6;
        g.nodes[i].x = r * std::cos(a) + jitter(rng) * k * 0.1;
        g.nodes[i].y = r * std::sin(a) + jitter(rng) * k * 0.1;
    }
    std::vector<double> dx(n), dy(n);
    double temp = k * std::sqrt(static_cast<double>(n)) * 0.3;
    const double cell = 2 * k;
    for (int it = 0; it < iterations; ++it) {
        std::fill(dx.begin(), dx.end(), 0.0);
        std::fill(dy.begin(), dy.end(), 0.0);
        // repulsion between nodes in nearby grid cells (Fruchterman-Reingold grid variant)
        std::unordered_map<std::int64_t, std::vector<std::size_t>> grid;
        auto key = [&](double x, double y) {
            auto cx = static_cast<std::int64_t>(std::floor(x / cell));
            auto cy = static_cast<std::int64_t>(std::floor(y / cell));
            return cx * 1000003 + cy;
        };
        for (std::size_t i = 0; i < n; ++i) grid[key(g.nodes[i].x, g.nodes[i].y)].push_back(i);
        for (std::size_t i = 0; i < n; ++i) {
            auto cx = static_cast<std::int64_t>(std::floor(g.nodes[i].x / cell));
            auto cy = static_cast<std::int64_t>(std::floor(g.nodes[i].y / cell));
            for (std::int64_t ox = -1; ox <= 1; ++ox) {
                for (std::int64_t oy = -1; oy <= 1; ++oy) {
                    auto f = grid.find((cx + ox) * 1000003 + (cy + oy));
                    if (f == grid.end()) continue;
                    for (std::size_t j : f->second) {
                        if (j == i) continue;
                        double ddx = g.nodes[i].x - g.nodes[j].x, ddy = g.nodes[i].y - g.nodes[j].y;
                        double d2 = ddx * ddx + ddy * ddy;
                        if (d2 < 1e-6) {
                            ddx = jitter(rng);
                            ddy = jitter(rng);
                            d2 = ddx * ddx + ddy * ddy + 1e-6;
                        }
                        double d = std::sqrt(d2);
                        if (d > cell) continue;
                        double force = k * k / d;
                        dx[i] += ddx / d * force;
                        dy[i] += ddy / d * force;
                    }
                }
            }
        }
        // attraction along edges
        for (const auto& [a, b] : g.edges) {
            auto& A = g.nodes[static_cast<std::size_t>(a)];
            auto& B = g.nodes[static_cast<std::size_t>(b)];
            double ddx = A.x - B.x, ddy = A.y - B.y;
            double d = std::sqrt(ddx * ddx + ddy * ddy) + 1e-9;
            double force = d * d / k;
            dx[static_cast<std::size_t>(a)] -= ddx / d * force;
            dy[static_cast<std::size_t>(a)] -= ddy / d * force;
            dx[static_cast<std::size_t>(b)] += ddx / d * force;
            dy[static_cast<std::size_t>(b)] += ddy / d * force;
        }
        // gentle gravity keeps loose parts (orphans, islands) from drifting away
        for (std::size_t i = 0; i < n; ++i) {
            const double pull = g.nodes[i].degree == 0 ? 0.8 : 0.2;  // orphans sit close around the rest
            dx[i] -= g.nodes[i].x * pull;
            dy[i] -= g.nodes[i].y * pull;
        }
        for (std::size_t i = 0; i < n; ++i) {
            double d = std::sqrt(dx[i] * dx[i] + dy[i] * dy[i]);
            if (d < 1e-9) continue;
            double step = std::min(d, temp);
            g.nodes[i].x += dx[i] / d * step;
            g.nodes[i].y += dy[i] / d * step;
        }
        temp *= 0.97;
        temp = std::max(temp, k * 0.05);
    }
}

std::string graphJson(const Graph& g) {
    using nlohmann::json;
    json nodes = json::array(), edges = json::array();
    for (const auto& n : g.nodes) {
        nodes.push_back({{"id", n.id}, {"label", n.label}, {"kind", n.kind}, {"degree", n.degree},
                         {"x", std::round(n.x * 10) / 10}, {"y", std::round(n.y * 10) / 10}});
    }
    for (const auto& [a, b] : g.edges) {
        edges.push_back({{"source", g.nodes[static_cast<std::size_t>(a)].id}, {"target", g.nodes[static_cast<std::size_t>(b)].id}});
    }
    return json{{"nodes", nodes}, {"edges", edges}}.dump(1, ' ', false, json::error_handler_t::replace);
}

std::string graphDot(const Graph& g) {
    auto q = [](const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + "\"";
    };
    std::string out = "digraph vault {\n  graph [overlap=false, splines=true];\n  node [shape=box, style=\"rounded,filled\", fillcolor=\"#eef0ff\", fontname=\"Helvetica\"];\n";
    for (const auto& n : g.nodes) {
        out += "  " + q(n.id) + " [label=" + q(n.label);
        if (n.kind == "unresolved") out += ", style=\"rounded,dashed\", fillcolor=white, fontcolor=gray40";
        if (n.kind == "tag") out += ", shape=ellipse, fillcolor=\"#e6f6ec\"";
        if (n.kind == "attachment") out += ", shape=note, fillcolor=\"#f4f4f4\"";
        out += "];\n";
    }
    for (const auto& [a, b] : g.edges) {
        out += "  " + q(g.nodes[static_cast<std::size_t>(a)].id) + " -> " + q(g.nodes[static_cast<std::size_t>(b)].id) + ";\n";
    }
    return out + "}\n";
}

raster::Image drawGraph(const Graph& g, int width, int height, int labels, const std::string& focus) {
    raster::Image img(width, height, {252, 252, 250, 255});
    if (g.nodes.empty()) return img;
    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
    for (const auto& n : g.nodes) {
        minX = std::min(minX, n.x);
        maxX = std::max(maxX, n.x);
        minY = std::min(minY, n.y);
        maxY = std::max(maxY, n.y);
    }
    const bool showLabels = labels > 0 || (labels < 0 && g.nodes.size() <= 150);
    const double margin = showLabels ? 60 : 24;
    double sx = (width - 2 * margin) / std::max(1e-9, maxX - minX);
    double sy = (height - 2 * margin) / std::max(1e-9, maxY - minY);
    double s = std::min({sx, sy, 1.6});  // layout units: about 100 per link, so small graphs stay compact
    if (g.nodes.size() == 1) s = 1;
    double ox = (width - (maxX - minX) * s) / 2, oy = (height - (maxY - minY) * s) / 2;
    auto px = [&](const GraphNode& n) { return std::make_pair(ox + (n.x - minX) * s, oy + (n.y - minY) * s); };
    const raster::Color edgeColor{180, 182, 196, 255};
    for (const auto& [a, b] : g.edges) {
        auto [x1, y1] = px(g.nodes[static_cast<std::size_t>(a)]);
        auto [x2, y2] = px(g.nodes[static_cast<std::size_t>(b)]);
        raster::drawLine(img, x1, y1, x2, y2, edgeColor, 1.2);
    }
    for (const auto& n : g.nodes) {
        auto [x, y] = px(n);
        double r = 3.5 + std::sqrt(static_cast<double>(n.degree)) * 1.8;
        raster::Color c{79, 91, 213, 255};
        if (n.kind == "unresolved") c = {170, 170, 176, 255};
        if (n.kind == "tag") c = {47, 158, 91, 255};
        if (n.kind == "attachment") c = {200, 140, 60, 255};
        if (!focus.empty() && n.id == focus) {
            c = {229, 110, 40, 255};
            r += 3;
        }
        raster::fillEllipse(img, x, y, r, r, c);
    }
    if (showLabels) {
        for (const auto& n : g.nodes) {
            auto [x, y] = px(n);
            double r = 3.5 + std::sqrt(static_cast<double>(n.degree)) * 1.8;
            std::string label = n.label.size() > 40 ? n.label.substr(0, 38) + ".." : n.label;
            double size = 12;
            double w = pdf::textWidthPx(label, pdf::Font{}, size);
            pdf::drawText(img, x - w / 2, y + r + size + 1, label, pdf::Font{}, size, {50, 50, 58, 255});
        }
    }
    return img;
}

}  // namespace wetwaresuite::notes
