// Board model: pads in board space, placement, connectivity, ratsnest, copper pours and DRC.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <queue>
#include <set>

#include "wetwaresuite/circuit/circuit.hpp"
#include "geom.hpp"

namespace wetwaresuite::circuit {

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string mm(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    std::string s = b;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string at(Pt p) { return "(" + mm(p.x) + ", " + mm(p.y) + ")"; }

}  // namespace

bool isPowerName(const std::string& netIn) {
    const std::string n = upper(netIn);
    if (n.empty()) return false;
    if (n[0] == '+' || n[0] == '-') return true;
    for (const char* p : {"GND", "AGND", "DGND", "PGND", "VCC", "VDD", "VSS", "VEE", "VIN", "VBAT", "VBUS", "VMOT", "V+", "V-"}) {
        if (n == p || (n.rfind(p, 0) == 0 && n.size() > std::string(p).size() && (n[std::string(p).size()] == '_' || std::isdigit(static_cast<unsigned char>(n[std::string(p).size()])))))
            return true;
    }
    // 5V, 3V3, 12V
    std::size_t i = 0;
    while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) ++i;
    if (i > 0 && i < n.size() && n[i] == 'V') {
        ++i;
        while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) ++i;
        return i == n.size();
    }
    return false;
}

void boardBounds(const Board& b, double& x0, double& y0, double& x1, double& y1) {
    x0 = y0 = 1e9;
    x1 = y1 = -1e9;
    for (const auto& p : b.outline) {
        x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
    }
    if (x0 > x1) x0 = y0 = x1 = y1 = 0;
}

double boardWidth(const Board& b) {
    double x0, y0, x1, y1;
    boardBounds(b, x0, y0, x1, y1);
    return x1 - x0;
}

double boardHeight(const Board& b) {
    double x0, y0, x1, y1;
    boardBounds(b, x0, y0, x1, y1);
    return y1 - y0;
}

namespace geom {

bool pointInPolygon(const std::vector<Pt>& poly, Pt p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Pt a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

double distanceToOutline(const std::vector<Pt>& poly, Pt p) {
    double d = 1e18;
    for (std::size_t i = 0; i < poly.size(); ++i) d = std::min(d, pointSeg(p, poly[i], poly[(i + 1) % poly.size()]));
    return d;
}

std::vector<Item> copperItems(const Circuit& c, const std::vector<WorldPad>& pads) {
    std::vector<Item> items;
    for (std::size_t i = 0; i < pads.size(); ++i) {
        Item it;
        it.kind = Item::Pad;
        it.index = static_cast<int>(i);
        it.shape = padShape(pads[i]);
        it.layers = pads[i].layers;
        it.net = pads[i].net;
        it.owner = pads[i].ref;
        items.push_back(std::move(it));
    }
    for (std::size_t i = 0; i < c.board.traces.size(); ++i) {
        const auto& t = c.board.traces[i];
        Item it;
        it.kind = Item::Trace;
        it.index = static_cast<int>(i);
        it.shape = capsule(t.a, t.b, t.width / 2);
        it.layers = t.layer == 0 ? 1 : 2;
        it.net = t.net;
        items.push_back(std::move(it));
    }
    for (std::size_t i = 0; i < c.board.vias.size(); ++i) {
        const auto& v = c.board.vias[i];
        Item it;
        it.kind = Item::Via;
        it.index = static_cast<int>(i);
        it.shape = capsule(v.pos, v.pos, v.diameter / 2);
        it.layers = 3;
        it.net = v.net;
        items.push_back(std::move(it));
    }
    return items;
}

Islands islands(const Circuit& c, const std::vector<WorldPad>& pads, const std::vector<ZoneFill>& fills) {
    Islands out;
    out.items = copperItems(c, pads);
    Uf uf(out.items.size());
    forNearPairs(out.items, 1e-6, [&](int i, int j) {
        const Item& a = out.items[static_cast<std::size_t>(i)];
        const Item& b = out.items[static_cast<std::size_t>(j)];
        if (a.net.empty() || a.net != b.net || !(a.layers & b.layers)) return;
        if (distance(a.shape, b.shape) <= 1e-6) uf.unite(i, j);
    });
    // zone islands
    for (const auto& f : fills) {
        std::vector<int> label(f.cells.size(), -1);
        std::vector<int> islandNode;
        int next = 0;
        for (int y = 0; y < f.ny; ++y) {
            for (int x = 0; x < f.nx; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * f.nx + x;
                if (!f.cells[k] || label[k] >= 0) continue;
                std::vector<std::size_t> stack{k};
                label[k] = next;
                while (!stack.empty()) {
                    const std::size_t q = stack.back();
                    stack.pop_back();
                    const int qx = static_cast<int>(q % f.nx), qy = static_cast<int>(q / f.nx);
                    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& d : nb) {
                        const int nx = qx + d[0], ny = qy + d[1];
                        if (!f.at(nx, ny)) continue;
                        const std::size_t r = static_cast<std::size_t>(ny) * f.nx + nx;
                        if (label[r] < 0) {
                            label[r] = next;
                            stack.push_back(r);
                        }
                    }
                }
                islandNode.push_back(-1);
                ++next;
            }
        }
        const int bit = f.layer == 0 ? 1 : 2;
        for (std::size_t i = 0; i < out.items.size(); ++i) {
            const Item& it = out.items[i];
            if (it.net != f.net || !(it.layers & bit)) continue;
            std::vector<Pt> samples = {it.shape.a};
            if (!it.shape.box) samples.push_back(it.shape.b);
            for (Pt p : samples) {
                const int cx = static_cast<int>(std::floor((p.x - f.x0) / f.cell)), cy = static_cast<int>(std::floor((p.y - f.y0) / f.cell));
                if (!f.at(cx, cy)) continue;
                const int l = label[static_cast<std::size_t>(cy) * f.nx + cx];
                if (islandNode[static_cast<std::size_t>(l)] < 0) islandNode[static_cast<std::size_t>(l)] = static_cast<int>(i);
                else uf.unite(islandNode[static_cast<std::size_t>(l)], static_cast<int>(i));
            }
        }
    }
    out.island.resize(out.items.size());
    for (std::size_t i = 0; i < out.items.size(); ++i) out.island[i] = uf.find(static_cast<int>(i));
    return out;
}

}  // namespace geom

Pt toBoard(const PcbPlace& place, Pt p) {
    if (place.bottom) p.x = -p.x;
    const int r = ((place.rot % 360) + 360) % 360;
    Pt q = p;
    if (r == 90) q = {p.y, -p.x};
    else if (r == 180) q = {-p.x, -p.y};
    else if (r == 270) q = {-p.y, p.x};
    return {place.pos.x + q.x, place.pos.y + q.y};
}

std::vector<WorldPad> worldPads(const Circuit& c, const Netlist& nl) {
    std::vector<WorldPad> out;
    for (const auto& part : c.parts) {
        if (!part.pcb.placed) continue;
        auto fp = footprintOf(part);
        if (!fp) continue;
        const int r = ((part.pcb.rot % 360) + 360) % 360;
        for (const auto& pad : fp->pads) {
            WorldPad w;
            w.ref = part.ref;
            w.number = pad.number;
            w.net = nl.netOf(part.ref, pad.number);
            w.pos = toBoard(part.pcb, pad.pos);
            w.w = (r == 90 || r == 270) ? pad.h : pad.w;
            w.h = (r == 90 || r == 270) ? pad.w : pad.h;
            w.shape = pad.shape;
            w.drill = pad.drill;
            w.layers = pad.drill > 0 ? 3 : (part.pcb.bottom ? 2 : 1);
            out.push_back(std::move(w));
        }
    }
    return out;
}

bool insideBoard(const Board& b, Pt p) { return b.outline.size() >= 3 && geom::pointInPolygon(b.outline, p); }

int placeFootprints(Circuit& c, const Netlist& nl, bool replaceAll, double spacing) {
    std::vector<Part*> todo;
    for (auto& p : c.parts) {
        if (!footprintOf(p)) continue;
        if (p.pcb.placed && !replaceAll) continue;
        todo.push_back(&p);
    }
    if (todo.empty()) return 0;
    // order by connectivity: breadth-first from the part with the most pins, so neighbours sit together
    std::map<std::string, std::set<std::string>> adj;
    for (const auto& n : nl.nets) {
        if (n.nodes.size() > 12) continue;  // big nets (GND) say little about locality
        for (const auto& a : n.nodes) {
            for (const auto& b : n.nodes) {
                if (a.ref != b.ref) adj[a.ref].insert(b.ref);
            }
        }
    }
    std::vector<Part*> order;
    std::set<std::string> seen;
    auto pinCount = [](const Part* p) {
        auto fp = footprintOf(*p);
        return fp ? fp->pads.size() : 0;
    };
    std::vector<Part*> bySize = todo;
    std::stable_sort(bySize.begin(), bySize.end(), [&](const Part* a, const Part* b) { return pinCount(a) > pinCount(b); });
    std::map<std::string, Part*> byRef;
    for (Part* p : todo) byRef[p->ref] = p;
    for (Part* start : bySize) {
        if (seen.count(start->ref)) continue;
        std::queue<Part*> q;
        q.push(start);
        seen.insert(start->ref);
        while (!q.empty()) {
            Part* p = q.front();
            q.pop();
            order.push_back(p);
            for (const auto& r : adj[p->ref]) {
                auto it = byRef.find(r);
                if (it != byRef.end() && !seen.count(r)) {
                    seen.insert(r);
                    q.push(it->second);
                }
            }
        }
    }
    double area = 0;
    for (Part* p : order) {
        auto fp = footprintOf(*p);
        area += (fp->x1 - fp->x0 + spacing) * (fp->y1 - fp->y0 + spacing);
    }
    const double margin = 3;
    const bool haveBoard = c.board.outline.size() >= 3;
    double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (haveBoard) boardBounds(c.board, bx0, by0, bx1, by1);
    double rowWidth = haveBoard ? bx1 - bx0 - 2 * margin : std::max(30.0, std::sqrt(area) * 1.3);
    for (Part* p : order) {
        auto fp = footprintOf(*p);
        rowWidth = std::max(rowWidth, fp->x1 - fp->x0);
    }
    const double ox = haveBoard ? bx0 + margin : margin, oy = haveBoard ? by0 + margin : margin;
    double x = 0, y = 0, rowH = 0, maxX = 0;
    auto snap = [](double v) { return std::round(v / 1.27) * 1.27; };
    for (Part* p : order) {
        auto fp = footprintOf(*p);
        const double w = fp->x1 - fp->x0, h = fp->y1 - fp->y0;
        if (x > 0 && x + w > rowWidth) {
            x = 0;
            y += rowH + spacing;
            rowH = 0;
        }
        p->pcb.placed = true;
        p->pcb.rot = 0;
        p->pcb.bottom = false;
        p->pcb.pos = {snap(ox + x - fp->x0), snap(oy + y - fp->y0)};
        x += w + spacing;
        maxX = std::max(maxX, x);
        rowH = std::max(rowH, h);
    }
    if (!haveBoard) {
        const double W = std::ceil(maxX - spacing + 2 * margin), H = std::ceil(y + rowH + 2 * margin);
        c.board.outline = {{0, 0}, {W, 0}, {W, H}, {0, H}};
    }
    return static_cast<int>(order.size());
}

std::vector<Airwire> ratsnest(const Circuit& c, const Netlist& nl) {
    const auto pads = worldPads(c, nl);
    const auto fills = fillZones(c, nl);
    const auto isl = geom::islands(c, pads, fills);
    std::map<std::string, std::vector<int>> byNet;  // pad indices
    for (std::size_t i = 0; i < pads.size(); ++i) {
        if (!pads[i].net.empty()) byNet[pads[i].net].push_back(static_cast<int>(i));
    }
    std::vector<Airwire> out;
    for (const auto& [net, idx] : byNet) {
        // clusters of pads already joined by copper
        std::map<int, std::vector<int>> clusters;
        for (int i : idx) clusters[isl.island[static_cast<std::size_t>(i)]].push_back(i);
        if (clusters.size() < 2) continue;
        std::vector<std::vector<int>> cl;
        for (auto& [k, v] : clusters) cl.push_back(v);
        // Prim over clusters
        std::vector<bool> in(cl.size(), false);
        in[0] = true;
        for (std::size_t step = 1; step < cl.size(); ++step) {
            double best = 1e18;
            int bi = -1, bj = -1, bc = -1;
            for (std::size_t a = 0; a < cl.size(); ++a) {
                if (!in[a]) continue;
                for (std::size_t b = 0; b < cl.size(); ++b) {
                    if (in[b]) continue;
                    for (int i : cl[a]) {
                        for (int j : cl[b]) {
                            const double d = std::hypot(pads[static_cast<std::size_t>(i)].pos.x - pads[static_cast<std::size_t>(j)].pos.x,
                                                        pads[static_cast<std::size_t>(i)].pos.y - pads[static_cast<std::size_t>(j)].pos.y);
                            if (d < best) best = d, bi = i, bj = j, bc = static_cast<int>(b);
                        }
                    }
                }
            }
            if (bc < 0) break;
            in[static_cast<std::size_t>(bc)] = true;
            const auto& pa = pads[static_cast<std::size_t>(bi)];
            const auto& pb = pads[static_cast<std::size_t>(bj)];
            out.push_back({net, pa.pos, pb.pos, pa.ref + "." + pa.number, pb.ref + "." + pb.number});
        }
    }
    return out;
}

std::vector<ZoneFill> fillZones(const Circuit& c, const Netlist& nl) {
    std::vector<ZoneFill> out;
    if (c.board.zones.empty() || c.board.outline.size() < 3) return out;
    const auto pads = worldPads(c, nl);
    const auto items = geom::copperItems(c, pads);
    double bx0, by0, bx1, by1;
    boardBounds(c.board, bx0, by0, bx1, by1);
    for (std::size_t zi = 0; zi < c.board.zones.size(); ++zi) {
        const Zone& z = c.board.zones[zi];
        ZoneFill f;
        f.zone = static_cast<int>(zi);
        f.layer = z.layer;
        f.net = z.net;
        f.cell = 0.1;
        f.x0 = bx0, f.y0 = by0;
        f.nx = std::max(1, static_cast<int>(std::ceil((bx1 - bx0) / f.cell)));
        f.ny = std::max(1, static_cast<int>(std::ceil((by1 - by0) / f.cell)));
        if (static_cast<double>(f.nx) * f.ny > 2.5e7) continue;  // a board over 0.5 m wide: skip rather than eat memory
        f.cells.assign(static_cast<std::size_t>(f.nx) * f.ny, 0);
        const double half = f.cell * 0.71;
        // inside the outline (scanline)
        const auto& poly = c.board.outline;
        for (int y = 0; y < f.ny; ++y) {
            const double py = f.y0 + (y + 0.5) * f.cell;
            std::vector<double> xs;
            for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                const Pt a = poly[i], b = poly[j];
                if ((a.y > py) != (b.y > py)) xs.push_back((b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x);
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int xa = std::max(0, static_cast<int>(std::ceil((xs[k] - f.x0) / f.cell - 0.5)));
                const int xb = std::min(f.nx - 1, static_cast<int>(std::floor((xs[k + 1] - f.x0) / f.cell - 0.5)));
                for (int x = xa; x <= xb; ++x) f.cells[static_cast<std::size_t>(y) * f.nx + x] = 1;
            }
        }
        auto clearAround = [&](const geom::Shape& s, double margin) {
            const int xa = std::max(0, static_cast<int>(std::floor((s.x0() - margin - f.x0) / f.cell)));
            const int xb = std::min(f.nx - 1, static_cast<int>(std::ceil((s.x1() + margin - f.x0) / f.cell)));
            const int ya = std::max(0, static_cast<int>(std::floor((s.y0() - margin - f.y0) / f.cell)));
            const int yb = std::min(f.ny - 1, static_cast<int>(std::ceil((s.y1() + margin - f.y0) / f.cell)));
            for (int y = ya; y <= yb; ++y) {
                for (int x = xa; x <= xb; ++x) {
                    auto& cell = f.cells[static_cast<std::size_t>(y) * f.nx + x];
                    if (!cell) continue;
                    if (geom::distance(s, Pt{f.x0 + (x + 0.5) * f.cell, f.y0 + (y + 0.5) * f.cell}) < margin) cell = 0;
                }
            }
        };
        const double edge = std::max(c.board.rules.edgeClearance, z.clearance) + half;
        for (std::size_t i = 0; i < poly.size(); ++i) clearAround(geom::capsule(poly[i], poly[(i + 1) % poly.size()], 0), edge);
        const int bit = z.layer == 0 ? 1 : 2;
        for (const auto& it : items) {
            if (!(it.layers & bit)) continue;
            if (!it.net.empty() && it.net == z.net) continue;
            clearAround(it.shape, z.clearance + half);
        }
        // drill holes of own-net pads stay copper (plated); holes of vias likewise
        // drop islands that reach no copper of the zone's net
        std::vector<int> label(f.cells.size(), -1);
        int next = 0;
        std::vector<bool> keep;
        for (int y = 0; y < f.ny; ++y) {
            for (int x = 0; x < f.nx; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * f.nx + x;
                if (!f.cells[k] || label[k] >= 0) continue;
                std::vector<std::size_t> stack{k};
                label[k] = next;
                while (!stack.empty()) {
                    const std::size_t q = stack.back();
                    stack.pop_back();
                    const int qx = static_cast<int>(q % f.nx), qy = static_cast<int>(q / f.nx);
                    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& d : nb) {
                        const int nx = qx + d[0], ny = qy + d[1];
                        if (!f.at(nx, ny)) continue;
                        const std::size_t r = static_cast<std::size_t>(ny) * f.nx + nx;
                        if (label[r] < 0) {
                            label[r] = next;
                            stack.push_back(r);
                        }
                    }
                }
                keep.push_back(false);
                ++next;
            }
        }
        for (const auto& it : items) {
            if (it.net != z.net || !(it.layers & bit)) continue;
            std::vector<Pt> samples = {it.shape.a};
            if (!it.shape.box) samples.push_back(it.shape.b);
            for (Pt p : samples) {
                const int cx = static_cast<int>(std::floor((p.x - f.x0) / f.cell)), cy = static_cast<int>(std::floor((p.y - f.y0) / f.cell));
                if (f.at(cx, cy)) keep[static_cast<std::size_t>(label[static_cast<std::size_t>(cy) * f.nx + cx])] = true;
            }
        }
        for (std::size_t k = 0; k < f.cells.size(); ++k) {
            if (f.cells[k] && !keep[static_cast<std::size_t>(label[k])]) f.cells[k] = 0;
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<Issue> drc(const Circuit& c, const Netlist& nl) {
    std::vector<Issue> out;
    const Rules& r = c.board.rules;
    if (c.board.outline.size() < 3) out.push_back({"error", "no_board", "there is no board outline (circuit.board)"});
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (!sym || sym->power) continue;
        if (p.footprint.empty()) continue;
        if (!footprintOf(p)) out.push_back({"warning", "unknown_footprint", p.ref + ": footprint '" + p.footprint + "' isn't in the library"});
        else if (!p.pcb.placed) out.push_back({"warning", "unplaced", p.ref + " is not on the board (circuit.pcb_place)"});
    }
    const auto pads = worldPads(c, nl);
    const auto items = geom::copperItems(c, pads);
    for (std::size_t i = 0; i < c.board.traces.size(); ++i) {
        const auto& t = c.board.traces[i];
        if (t.width + 1e-9 < r.minTrace) {
            out.push_back({"error", "trace_width", "trace on " + std::string(t.layer ? "B.Cu" : "F.Cu") + " at " + at(t.a) + " is " + mm(t.width) +
                                                       " mm wide (minimum " + mm(r.minTrace) + ")"});
        }
    }
    for (const auto& v : c.board.vias) {
        if (v.drill + 1e-9 < r.minDrill) out.push_back({"error", "drill", "via at " + at(v.pos) + " drill " + mm(v.drill) + " mm < " + mm(r.minDrill)});
        if ((v.diameter - v.drill) / 2 + 1e-9 < r.minAnnular) {
            out.push_back({"error", "annular_ring", "via at " + at(v.pos) + " annular ring " + mm((v.diameter - v.drill) / 2) + " mm < " + mm(r.minAnnular)});
        }
    }
    int clearanceErrors = 0;
    const int maxListed = 40;
    auto describe = [&](const geom::Item& it) {
        if (it.kind == geom::Item::Pad) return "pad " + pads[static_cast<std::size_t>(it.index)].ref + "." + pads[static_cast<std::size_t>(it.index)].number;
        if (it.kind == geom::Item::Via) return "via at " + at(c.board.vias[static_cast<std::size_t>(it.index)].pos);
        const auto& t = c.board.traces[static_cast<std::size_t>(it.index)];
        return std::string("trace ") + (t.net.empty() ? "" : "(" + t.net + ") ") + at(t.a) + "-" + at(t.b);
    };
    geom::forNearPairs(items, r.clearance, [&](int i, int j) {
        const auto& a = items[static_cast<std::size_t>(i)];
        const auto& b = items[static_cast<std::size_t>(j)];
        if (!(a.layers & b.layers)) return;
        if (!a.net.empty() && a.net == b.net) return;
        if (a.kind == geom::Item::Pad && b.kind == geom::Item::Pad && a.owner == b.owner) return;  // footprint's own spacing
        const double d = geom::distance(a.shape, b.shape);
        if (d + 1e-4 >= r.clearance) return;
        ++clearanceErrors;
        if (clearanceErrors > maxListed) return;
        const std::string nets = " (" + (a.net.empty() ? std::string("no net") : a.net) + " / " + (b.net.empty() ? std::string("no net") : b.net) + ")";
        if (d <= 1e-6) out.push_back({"error", "short", describe(a) + " touches " + describe(b) + nets});
        else out.push_back({"error", "clearance", describe(a) + " is " + mm(d) + " mm from " + describe(b) + nets + ", minimum " + mm(r.clearance)});
    });
    if (clearanceErrors > maxListed) out.push_back({"error", "clearance", std::to_string(clearanceErrors - maxListed) + " more clearance errors not listed"});
    if (c.board.outline.size() >= 3) {
        int edgeErrors = 0;
        for (const auto& it : items) {
            const Pt probe = it.shape.a;
            double d;
            bool inside = insideBoard(c.board, probe) && (it.shape.box || insideBoard(c.board, it.shape.b));
            d = 1e18;
            for (std::size_t i = 0; i < c.board.outline.size(); ++i) {
                d = std::min(d, geom::distance(it.shape, geom::capsule(c.board.outline[i], c.board.outline[(i + 1) % c.board.outline.size()], 0)));
            }
            if (!inside || d + 1e-4 < r.edgeClearance) {
                if (++edgeErrors <= maxListed) {
                    out.push_back({"error", "edge_clearance", describe(it) + (inside ? " is " + mm(d) + " mm from the board edge" : " is outside the board")});
                }
            }
        }
    }
    // courtyard overlaps
    struct Box {
        std::string ref;
        double x0, y0, x1, y1;
        bool bottom;
    };
    std::vector<Box> boxes;
    for (const auto& p : c.parts) {
        if (!p.pcb.placed) continue;
        auto fp = footprintOf(p);
        if (!fp) continue;
        Pt a = toBoard(p.pcb, {fp->x0, fp->y0}), b = toBoard(p.pcb, {fp->x1, fp->y1});
        boxes.push_back({p.ref, std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y), p.pcb.bottom});
    }
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        for (std::size_t j = i + 1; j < boxes.size(); ++j) {
            const auto& a = boxes[i];
            const auto& b = boxes[j];
            if (a.bottom != b.bottom) continue;
            if (a.x0 < b.x1 - 0.01 && b.x0 < a.x1 - 0.01 && a.y0 < b.y1 - 0.01 && b.y0 < a.y1 - 0.01) {
                out.push_back({"warning", "courtyard_overlap", a.ref + " and " + b.ref + " overlap"});
            }
        }
    }
    const auto air = ratsnest(c, nl);
    int listed = 0;
    for (const auto& a : air) {
        if (++listed <= maxListed) out.push_back({"error", "unrouted", a.net + ": " + a.from + " to " + a.to + " is not routed"});
    }
    if (static_cast<int>(air.size()) > maxListed) out.push_back({"error", "unrouted", std::to_string(air.size() - maxListed) + " more unrouted connections"});
    return out;
}

}  // namespace wetwaresuite::circuit
