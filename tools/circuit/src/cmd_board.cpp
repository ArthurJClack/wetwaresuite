// circuit.* board tools beyond the basics: plane fan-out and the 3D view.
#include "cmd_common.hpp"

namespace wetwaresuite::circuit::cmd {

void registerBoardToolCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.fanout", "Fan Out to Planes",
             "Give every SMD pad of the named nets (default: every net with a pour) a short trace and a via, so it reaches the "
             "plane poured on another layer: the usual way to connect ground and power on a 4-layer board (pour GND on In1, "
             "+3V3 on In2, then fanout). Pads a pour already reaches, through-hole pads and pads with a via are left alone.",
             {docParam(), {"nets", ParamType::String, "nets to fan out, comma-separated (default: nets with a pour)", false}, previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    if (doc.circuit.board.outline.size() < 3) return Result::failure("no board yet: circuit.board or circuit.pcb_place first");
                    const Netlist nl = buildNetlist(doc.circuit);
                    std::vector<std::string> nets;
                    if (has(p, "nets")) {
                        for (const auto& n : splitList(stringParam(p, "nets"))) {
                            const Net* net = nl.net(n);
                            if (!net) return Result::failure("no net called " + n + " (circuit.nets lists them)");
                            nets.push_back(net->name);
                        }
                    }
                    if (nets.empty() && doc.circuit.board.zones.empty()) return Result::failure("no pours yet: give nets=, or pour a plane first (circuit.pour)");
                    const FanoutReport r = fanout(doc.circuit, nl, nets);
                    std::string s = "added " + std::to_string(r.vias) + " vias";
                    if (r.already) s += "; " + std::to_string(r.already) + " pads already had one";
                    for (const auto& f : r.failed) s += "\nno room for a via next to " + f;
                    return edited(s, p).with("vias", static_cast<std::int64_t>(r.vias)).with("failed", static_cast<std::int64_t>(r.failed.size()));
                },
                View::Pcb));
}

}  // namespace wetwaresuite::circuit::cmd
