// Fan-out: a short trace and a via from each SMD pad of a net to reach a plane (a pour on an
// inner layer, or on the far side), the usual way to connect parts to ground and power planes.
#include <algorithm>
#include <cmath>
#include <set>

#include "wetwaresuite/circuit/circuit.hpp"
#include "geom.hpp"

namespace wetwaresuite::circuit {

FanoutReport fanout(Circuit& c, const Netlist& nl, const std::vector<std::string>& netsIn) {
    FanoutReport rep;
    std::set<std::string> nets(netsIn.begin(), netsIn.end());
    if (nets.empty()) {
        // every net with a pour on a layer that SMD pads can't reach directly
        for (const auto& z : c.board.zones) nets.insert(z.net);
    }
    if (nets.empty() || c.board.outline.size() < 3) return rep;
    const Rules& r = c.board.rules;
    const auto pads = worldPads(c, nl);
    auto items = geom::copperItems(c, pads);
    // planes per net: which layers have a pour of it
    std::map<std::string, int> planeMask;
    for (const auto& z : c.board.zones) planeMask[z.net] |= layerBit(z.layer);

    for (std::size_t i = 0; i < pads.size(); ++i) {
        const WorldPad& pad = pads[i];
        if (pad.drill > 0 || pad.net.empty() || !nets.count(pad.net)) continue;
        // a plane on the pad's own layer already reaches it
        const int mask = planeMask.count(pad.net) ? planeMask[pad.net] : 0;
        if (mask & pad.layers) continue;
        // already has a via of its net touching it or one short trace away
        bool done = false;
        const geom::Shape padShape = geom::padShape(pad);
        for (const auto& v : c.board.vias) {
            if (v.net != pad.net) continue;
            if (geom::distance(padShape, geom::capsule(v.pos, v.pos, v.diameter / 2)) <= 1e-6) done = true;
            for (const auto& t : c.board.traces) {
                if (done) break;
                if (t.net != pad.net || !(layerBit(t.layer) & pad.layers)) continue;
                const auto ts = geom::capsule(t.a, t.b, t.width / 2);
                if (geom::distance(ts, padShape) <= 1e-6 && geom::distance(ts, geom::capsule(v.pos, v.pos, v.diameter / 2)) <= 1e-6) done = true;
            }
            if (done) break;
        }
        if (done) {
            ++rep.already;
            continue;
        }
        const Part* part = c.part(pad.ref);
        const Pt centre = part ? part->pcb.pos : pad.pos;
        const int layer = (pad.layers & 1) ? 0 : 1;
        const double width = std::min(isPowerName(pad.net) ? r.powerTrace : r.trace, std::min(pad.w, pad.h));
        const double vr = r.viaDiameter / 2;
        // directions: away from the part's centre first, then the others
        double ax = pad.pos.x - centre.x, ay = pad.pos.y - centre.y;
        std::vector<Pt> dirs;
        if (std::abs(ax) >= std::abs(ay)) dirs = {{ax >= 0 ? 1.0 : -1.0, 0}, {0, ay >= 0 ? 1.0 : -1.0}, {0, ay >= 0 ? -1.0 : 1.0}, {ax >= 0 ? -1.0 : 1.0, 0}};
        else dirs = {{0, ay >= 0 ? 1.0 : -1.0}, {ax >= 0 ? 1.0 : -1.0, 0}, {ax >= 0 ? -1.0 : 1.0, 0}, {0, ay >= 0 ? -1.0 : 1.0}};
        const double k = std::sqrt(0.5);
        for (Pt d : {Pt{k, k}, Pt{-k, k}, Pt{k, -k}, Pt{-k, -k}}) dirs.push_back(d);
        bool placed = false;
        for (const Pt d : dirs) {
            const double reach = std::abs(d.x) * pad.w / 2 + std::abs(d.y) * pad.h / 2;
            for (double extra : {0.0, 0.2, 0.45, 0.8, 1.3, 2.0}) {
                const double dist = reach + vr + r.clearance + extra;
                const Pt vp{pad.pos.x + d.x * dist, pad.pos.y + d.y * dist};
                const geom::Shape via = geom::capsule(vp, vp, vr);
                const geom::Shape trace = geom::capsule(pad.pos, vp, width / 2);
                if (!insideBoard(c.board, vp) || geom::distanceToOutline(c.board.outline, vp) < vr + r.edgeClearance) continue;
                bool ok = true;
                for (std::size_t j = 0; j < items.size() && ok; ++j) {
                    const auto& it = items[j];
                    if (!it.net.empty() && it.net == pad.net) continue;
                    if (geom::distance(it.shape, via) + 1e-6 < r.clearance) ok = false;
                    else if ((it.layers & layerBit(layer)) && geom::distance(it.shape, trace) + 1e-6 < r.clearance) ok = false;
                }
                if (!ok) continue;
                c.board.traces.push_back({pad.pos, vp, width, layer, pad.net});
                c.board.vias.push_back({vp, r.viaDrill, r.viaDiameter, pad.net});
                geom::Item ti;
                ti.kind = geom::Item::Trace;
                ti.shape = trace;
                ti.layers = layerBit(layer);
                ti.net = pad.net;
                items.push_back(ti);
                geom::Item vi;
                vi.kind = geom::Item::Via;
                vi.shape = via;
                vi.layers = kAllCopper;
                vi.net = pad.net;
                items.push_back(vi);
                placed = true;
                ++rep.vias;
                break;
            }
            if (placed) break;
        }
        if (!placed) rep.failed.push_back(pad.ref + "." + pad.number + " (" + pad.net + ")");
    }
    return rep;
}

}  // namespace wetwaresuite::circuit
