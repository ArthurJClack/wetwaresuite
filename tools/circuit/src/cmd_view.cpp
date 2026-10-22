// circuit.view and circuit.hit: what the window shows of a circuit, and what is under a point of
// that picture, so clicks in the window turn into the ordinary circuit.* commands.
#include "cmd_common.hpp"

namespace wetwaresuite::circuit::cmd {

namespace {

double segDist(Pt p, Pt a, Pt b) {
    const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

Result hitSchematic(const Circuit& c, Pt p, Pt& out) {
    const Netlist nl = buildNetlist(c);
    const Pt snap{std::round(p.x), std::round(p.y)};
    Result r = Result::success("");
    out = snap;
    r.with("exact_x", p.x).with("exact_y", p.y);
    // a pin first (the end of a wire goes there), then labels, wires and part bodies
    double best = 0.6;
    const Part* bestPart = nullptr;
    const PinDef* bestPin = nullptr;
    for (const auto& part : c.parts) {
        auto sym = symbolOf(part);
        if (!sym) continue;
        for (const auto& pin : sym->pins) {
            const Pt at = pinPos(part, pin);
            const double d = std::hypot(at.x - p.x, at.y - p.y);
            if (d < best) best = d, bestPart = &part, bestPin = &pin;
        }
    }
    if (bestPin) {
        const Pt at = pinPos(*bestPart, *bestPin);
        const std::string net = nl.netOf(bestPart->ref, bestPin->number);
        r.message = "pin " + bestPart->ref + "." + bestPin->number + (bestPin->name.empty() || bestPin->name == bestPin->number ? "" : " (" + bestPin->name + ")") +
                    (net.empty() ? "" : " on " + net);
        r.with("kind", "pin").with("part", bestPart->ref).with("pin", bestPin->number).with("net", net);
        out = at;
        return r;
    }
    for (const auto& l : c.labels) {
        if (std::hypot(l.pos.x - p.x, l.pos.y - p.y) < 0.6) {
            r.message = "label " + l.name;
            r.with("kind", "label").with("net", l.name);
            out = l.pos;
            return r;
        }
    }
    for (const auto& w : c.wires) {
        for (std::size_t i = 1; i < w.pts.size(); ++i) {
            if (segDist(p, w.pts[i - 1], w.pts[i]) < 0.35) {
                r.message = "wire at " + pt(snap);
                r.with("kind", "wire");
                return r;
            }
        }
    }
    for (auto it = c.parts.rbegin(); it != c.parts.rend(); ++it) {
        auto sym = symbolOf(*it);
        if (!sym) continue;
        const Pt a = toSheet(*it, {sym->bodyMinX, sym->bodyMinY}), b = toSheet(*it, {sym->bodyMaxX, sym->bodyMaxY});
        const double m = sym->power ? 0.6 : 0.2;
        if (p.x >= std::min(a.x, b.x) - m && p.x <= std::max(a.x, b.x) + m && p.y >= std::min(a.y, b.y) - m && p.y <= std::max(a.y, b.y) + m) {
            r.message = "part " + it->ref + " (" + (sym->title.empty() ? sym->name : sym->title) + (it->value.empty() ? "" : ", " + it->value) + ") at " + pt(it->pos);
            r.with("kind", "part").with("part", it->ref).with("part_x", it->pos.x).with("part_y", it->pos.y).with("rot", static_cast<std::int64_t>(it->rot));
            return r;
        }
    }
    r.message = "nothing at " + pt(snap);
    r.with("kind", "none");
    return r;
}

Result hitBoard(const Circuit& c, Pt p, Pt& out) {
    const Netlist nl = buildNetlist(c);
    const Pt snap{std::round(p.x * 20) / 20, std::round(p.y * 20) / 20};  // 0.05 mm
    Result r = Result::success("");
    out = snap;
    r.with("exact_x", p.x).with("exact_y", p.y);
    for (const auto& v : c.board.vias) {
        if (std::hypot(v.pos.x - p.x, v.pos.y - p.y) <= v.diameter / 2) {
            r.message = "via" + (v.net.empty() ? "" : " on " + v.net) + " at " + pt(v.pos);
            r.with("kind", "via").with("net", v.net);
            out = v.pos;
            return r;
        }
    }
    for (const auto& pad : worldPads(c, nl)) {
        if (std::abs(pad.pos.x - p.x) <= pad.w / 2 && std::abs(pad.pos.y - p.y) <= pad.h / 2) {
            r.message = "pad " + pad.ref + "." + pad.number + (pad.net.empty() ? "" : " on " + pad.net);
            // a trace started or ended here lands on the pad's centre
            r.with("kind", "pad").with("part", pad.ref).with("pin", pad.number).with("net", pad.net);
            out = pad.pos;
            return r;
        }
    }
    for (const auto& t : c.board.traces) {
        if (segDist(p, t.a, t.b) <= t.width / 2 + 0.1) {
            r.message = "trace" + (t.net.empty() ? "" : " on " + t.net) + " (" + layerName(t.layer) + ", " + num(t.width) + " mm)";
            r.with("kind", "trace").with("net", t.net).with("layer", std::string(layerShort(t.layer)));
            return r;
        }
    }
    for (auto it = c.parts.rbegin(); it != c.parts.rend(); ++it) {
        if (!it->pcb.placed) continue;
        auto fp = footprintOf(*it);
        if (!fp) continue;
        const Pt a = toBoard(it->pcb, {fp->x0, fp->y0}), b = toBoard(it->pcb, {fp->x1, fp->y1});
        if (p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y)) {
            r.message = "footprint " + it->ref + " (" + fp->name + ") at " + pt(it->pcb.pos) + (it->pcb.bottom ? ", bottom" : "");
            r.with("kind", "footprint").with("part", it->ref).with("part_x", it->pcb.pos.x).with("part_y", it->pcb.pos.y).with("rot", static_cast<std::int64_t>(it->pcb.rot));
            return r;
        }
    }
    r.message = std::string(insideBoard(c.board, p) ? "board" : "nothing") + " at " + pt(snap) + " mm";
    r.with("kind", "none");
    return r;
}

}  // namespace

void registerViewCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.view", "Show Schematic or Board",
             "Pick what the window shows of a circuit: the schematic, the board (pcb) or the assembled board in 3D. Clicks on the "
             "picture are read with circuit.hit.",
             {docParam(), {"view", ParamType::String, "what to show", true, {"schematic", "pcb", "3d"}}}, false},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const std::string v = lower(stringParam(p, "view"));
                    if (v != "schematic" && v != "pcb" && v != "3d") return Result::failure("view is schematic, pcb or 3d");
                    doc.view = v;
                    return edited("showing the " + std::string(v == "pcb" ? "board" : v == "3d" ? "board in 3D" : "schematic"), p).with("view", v);
                },
                View::Schematic, false));

    reg.add({"circuit.hit", "What Is Here",
             "Say what is at a point of the window's picture of a circuit (the image circuit.preview or the window shows for "
             "view=): a pin, label, wire or part on the schematic; a pad, via, trace or footprint on the board. Gives the point in "
             "sheet grid units or board mm (snapped: to the grid, or to a pad's centre) so it can go to circuit.place, "
             "circuit.wire, circuit.move, circuit.trace, circuit.via or circuit.footprint_move.",
             {docParam(),
              {"x", ParamType::Number, "picture x, pixels", true},
              {"y", ParamType::Number, "picture y, pixels", true},
              {"view", ParamType::String, "which picture (default: what the window shows)", false, {"schematic", "pcb"}}},
             false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const std::string v = has(p, "view") ? lower(stringParam(p, "view")) : doc.view;
                    const ViewFrame f = viewFrame(doc.circuit, v);
                    if (f.scale <= 0) return Result::failure("the 3D view can't be clicked: show the schematic or the board (circuit.view)");
                    const Pt at{f.origin.x + numberParam(p, "x", 0) / f.scale, f.origin.y + numberParam(p, "y", 0) / f.scale};
                    Pt out = at;
                    Result r = f.view == "pcb" ? hitBoard(doc.circuit, at, out) : hitSchematic(doc.circuit, at, out);
                    r.with("x", out.x).with("y", out.y).with("doc", stringParam(p, "doc")).with("view", f.view);
                    return r;
                },
                View::Schematic, false));
}

}  // namespace wetwaresuite::circuit::cmd
