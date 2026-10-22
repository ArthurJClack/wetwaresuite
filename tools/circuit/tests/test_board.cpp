// Boards beyond the basics: the larger parts library, 4-layer boards with planes and fan-out,
// thermal reliefs, non-plated holes, DRC catches, fab output that parses, assembly files and 3D.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::circuit;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_board_" + std::to_string(stamp) + "_" + std::to_string(rd()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

Result run(cli::Batch& b, const std::string& line) {
    Result r = b.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

std::int64_t intField(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return -999;
    if (auto i = std::get_if<std::int64_t>(v)) return *i;
    return -998;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// A copy of the circuit (never a reference held across commands).
Circuit circuitOf(Session& s, const std::string& id) { return s.workspace().getAs<CircuitDoc>(id)->circuit; }

// ---- a small RS-274X reader: checks the syntax and collects what is drawn ------------------

struct GerberInfo {
    bool ok = true;
    std::string error;
    int flashes = 0, draws = 0, regions = 0;
    std::set<int> apertures;
    std::string function;
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    std::vector<std::pair<double, double>> flashAt;  // mm
};

GerberInfo parseGerber(const std::string& text) {
    GerberInfo g;
    auto fail = [&](const std::string& e) {
        if (g.ok) g.ok = false, g.error = e;
    };
    bool format = false, units = false, ended = false, inRegion = false;
    int current = -1;
    double x = 0, y = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const char ch = text[i];
        if (ch == '\n' || ch == '\r' || ch == ' ') {
            ++i;
            continue;
        }
        if (ended) {
            fail("data after M02");
            break;
        }
        if (ch == '%') {
            const auto end = text.find('%', i + 1);
            if (end == std::string::npos) {
                fail("unterminated % block");
                break;
            }
            const std::string ext = text.substr(i + 1, end - i - 1);
            if (ext.empty() || ext.back() != '*') fail("extended command without *: " + ext);
            if (ext.rfind("FSLAX46Y46", 0) == 0) format = true;
            else if (ext.rfind("MOMM", 0) == 0) units = true;
            else if (ext.rfind("ADD", 0) == 0) {
                std::size_t k = 3;
                int code = 0;
                while (k < ext.size() && std::isdigit(static_cast<unsigned char>(ext[k]))) code = code * 10 + (ext[k++] - '0');
                if (code < 10) fail("aperture code below 10");
                const char shape = k < ext.size() ? ext[k] : '?';
                if (shape != 'C' && shape != 'R' && shape != 'O') fail(std::string("unknown aperture template ") + shape);
                if (ext.find(',', k) == std::string::npos) fail("aperture without size");
                g.apertures.insert(code);
            } else if (ext.rfind("TF.FileFunction,", 0) == 0) {
                g.function = ext.substr(16, ext.size() - 17);
            } else if (ext.rfind("TF.", 0) != 0 && ext.rfind("LP", 0) != 0) {
                fail("unexpected extended command " + ext);
            }
            i = end + 1;
            continue;
        }
        const auto star = text.find('*', i);
        if (star == std::string::npos) {
            fail("word without *");
            break;
        }
        const std::string w = text.substr(i, star - i);
        i = star + 1;
        if (w == "G01" || w == "G75") continue;
        if (w == "G36") {
            if (inRegion) fail("nested G36");
            inRegion = true;
            ++g.regions;
            continue;
        }
        if (w == "G37") {
            if (!inRegion) fail("G37 without G36");
            inRegion = false;
            continue;
        }
        if (w == "M02") {
            ended = true;
            continue;
        }
        if (w.size() >= 2 && w[0] == 'D' && std::isdigit(static_cast<unsigned char>(w[1]))) {
            current = std::stoi(w.substr(1));
            if (!g.apertures.count(current)) fail("aperture D" + std::to_string(current) + " used before it is defined");
            continue;
        }
        if (w.empty() || (w[0] != 'X' && w[0] != 'Y')) {
            fail("unknown word " + w);
            continue;
        }
        if (!format || !units) fail("coordinates before %FS / %MO");
        const auto xp = w.find('X'), yp = w.find('Y'), dp = w.find('D');
        if (dp == std::string::npos) {
            fail("coordinate word without D code: " + w);
            continue;
        }
        if (xp != std::string::npos) x = std::stoll(w.substr(xp + 1, (yp != std::string::npos ? yp : dp) - xp - 1)) / 1e6;
        if (yp != std::string::npos) y = std::stoll(w.substr(yp + 1, dp - yp - 1)) / 1e6;
        const std::string d = w.substr(dp);
        if (d != "D01" && d != "D02" && d != "D03") fail("bad operation " + d);
        if (!inRegion && d != "D02" && current < 0) fail("draw with no aperture selected");
        if (d == "D03") {
            if (inRegion) fail("flash inside a region");
            ++g.flashes;
            g.flashAt.push_back({x, y});
        }
        if (d == "D01" && !inRegion) ++g.draws;
        g.minX = std::min(g.minX, x), g.maxX = std::max(g.maxX, x), g.minY = std::min(g.minY, y), g.maxY = std::max(g.maxY, y);
    }
    if (!ended) fail("no M02 at the end");
    if (inRegion) fail("region not closed");
    return g;
}

struct DrillInfo {
    bool ok = true;
    std::string error;
    std::map<int, double> tools;
    int holes = 0;
};

DrillInfo parseDrill(const std::string& text) {
    DrillInfo d;
    std::istringstream in(text);
    std::string line;
    bool header = true, started = false;
    int tool = -1;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == ';') continue;
        if (!started) {
            if (line != "M48") d.ok = false, d.error = "no M48 first";
            started = true;
            continue;
        }
        if (header) {
            if (line == "%") header = false;
            else if (line[0] == 'T') {
                const auto c = line.find('C');
                d.tools[std::stoi(line.substr(1, c - 1))] = std::stod(line.substr(c + 1));
            }
            continue;
        }
        if (line == "M30") break;
        if (line[0] == 'T') {
            tool = std::stoi(line.substr(1));
            if (!d.tools.count(tool)) d.ok = false, d.error = "undefined tool";
        } else if (line[0] == 'X') {
            if (tool < 0) d.ok = false, d.error = "hole before a tool";
            ++d.holes;
        }
    }
    return d;
}

// ---- tests -----------------------------------------------------------------------------

void testParts() {
    // every new part resolves, has pads for its pins and sensible pin types
    for (const char* name : {"zener", "schottky", "fuse", "ferrite_bead", "buzzer", "ams1117", "lm317", "ne555", "lm358", "atmega328p",
                             "atmega328p_au", "attiny85", "rp2040", "esp32_wroom", "rpi_pico", "usb_micro", "screw_terminal", "test_point"}) {
        auto s = findSymbol(name, 3);
        CHECK(s != nullptr);
        if (!s) continue;
        auto f = findFootprint(s->footprint);
        CHECK(f != nullptr);
        if (!f) continue;
        for (const auto& p : s->pins) {
            bool found = false;
            for (const auto& pad : f->pads) found = found || pad.number == p.number;
            CHECK(found);
            if (!found) std::printf("  %s pin %s has no pad in %s\n", name, p.number.c_str(), f->name.c_str());
        }
    }
    CHECK(findSymbol("rp2040")->pins.size() == 57);
    CHECK(findFootprint("QFN-56")->pads.size() == 57);
    CHECK(findSymbol("atmega328p")->pins.size() == 28 && findSymbol("atmega328p_au")->pins.size() == 32);
    CHECK(findFootprint("TQFP-32")->pads.size() == 32);
    CHECK(findSymbol("esp32_wroom")->pins.size() == 39 && findFootprint("ESP32-WROOM-32")->pads.size() == 39);
    CHECK(findSymbol("rpi_pico")->pins.size() == 40 && findFootprint("RPi_Pico")->pads.size() == 40);
    // QFN numbering runs counter-clockwise from the top of the left side
    auto qfn = findFootprint("QFN-16");
    CHECK(qfn->pads[0].pos.x < 0 && qfn->pads[0].pos.y < 0 && qfn->pads[4].pos.y > 0 && qfn->pads[8].pos.x > 0 && qfn->pads[12].pos.y < 0);
    CHECK(qfn->pads.size() == 17 && qfn->pads[16].number == "17");
    // KiCad names
    CHECK(findFootprint("Package_DFN_QFN:QFN-56-1EP_7x7mm_P0.4mm_EP3.2x3.2mm")->name == "QFN-56");
    CHECK(findFootprint("Package_QFP:TQFP-32_7x7mm_P0.8mm")->name == "TQFP-32");
    CHECK(findFootprint("Package_QFP:LQFP-48_7x7mm_P0.5mm")->name == "TQFP-48");
    CHECK(findFootprint("Package_TO_SOT_SMD:SOT-23-5")->pads.size() == 5);
    CHECK(findFootprint("SOT-23-6")->pads.size() == 6 && findFootprint("SOT-23")->pads.size() == 3);
    CHECK(findFootprint("Resistor_SMD:R_0402_1005Metric")->name == "R_0402");
    CHECK(findFootprint("TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-3_1x03_P5.08mm_Horizontal")->pads.size() == 3);
    CHECK(findFootprint("MountingHole:MountingHole_3.2mm_M3")->pads[0].plated == false);
    // Arduino aliases on the bare chip
    Circuit c;
    Part u;
    u.ref = "U1";
    u.symbol = "atmega328p";
    c.parts.push_back(u);
    std::string err;
    CHECK(resolvePin(c, "U1.D13", err).pin && resolvePin(c, "U1.D13", err).pin->number == "19");
    CHECK(resolvePin(c, "U1.A0", err).pin && resolvePin(c, "U1.A0", err).pin->number == "23");
    CHECK(resolvePin(c, "U1.TX", err).pin && resolvePin(c, "U1.TX", err).pin->number == "3");
    // the two GND pins are one net (stacked power pins)
    const Netlist nl = buildNetlist(c);
    CHECK(!nl.netOf("U1", "8").empty() && nl.netOf("U1", "8") == nl.netOf("U1", "22"));
}

void testFourLayer(const fs::path& dir) {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new title=four -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=attiny85 x=0 y=0").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=capacitor value=100n x=-12 y=-4").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=resistor value=330 x=12 y=-4").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=led x=18 y=-4").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=header pins=3 ref=J1 x=20 y=6").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=mounting_hole x=30 y=0").ok);
    CHECK(run(b, "circuit.net doc=$c name=LED pins=U1.PB0,R1.1").ok);
    CHECK(run(b, "circuit.connect doc=$c from=R1.2 to=D1.A").ok);
    CHECK(run(b, "circuit.net doc=$c name=IN pins=U1.PB3,J1.3").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=gnd pins=U1.GND,C1.2,D1.K,J1.1").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=+5v pins=U1.VCC,C1.1,J1.2").ok);
    CHECK(run(b, "circuit.no_connect doc=$c pins=U1.PB5,U1.PB1,U1.PB2,U1.PB4").ok);
    Result r = run(b, "circuit.erc doc=$c");
    CHECK(intField(r, "errors") == 0);
    if (intField(r, "errors") != 0) std::printf("%s\n", r.message.c_str());
    CHECK(!run(b, "circuit.board doc=$c width=40 height=30 layers=3").ok);
    r = run(b, "circuit.board doc=$c width=40 height=30 layers=4");
    CHECK(r.ok && intField(r, "layers") == 4);
    r = run(b, "circuit.pcb_place doc=$c");
    CHECK(r.ok && intField(r, "placed") == 6);
    {
        // the mounting hole went to a corner, inside the board
        const Circuit c = circuitOf(s, "ckt1");
        const Part* h = c.part("H1");
        CHECK(h && h->pcb.placed && h->pcb.pos.x < 6 && h->pcb.pos.y < 6 && insideBoard(c.board, h->pcb.pos));
    }
    CHECK(run(b, "circuit.pour doc=$c net=GND layer=In1").ok);
    CHECK(run(b, "circuit.pour doc=$c net=+5V layer=In2").ok);
    r = run(b, "circuit.fanout doc=$c");
    CHECK(r.ok && intField(r, "vias") == 3 && intField(r, "failed") == 0);  // C1.1, C1.2, D1.K: the DIP reaches the planes itself
    // running it again adds nothing
    CHECK(intField(run(b, "circuit.fanout doc=$c"), "vias") == 0);
    r = run(b, "circuit.route doc=$c skip=GND,+5V");
    CHECK(r.ok && intField(r, "failed") == 0);
    CHECK(intField(run(b, "circuit.ratsnest doc=$c"), "unrouted") == 0);
    r = run(b, "circuit.drc doc=$c");
    CHECK(intField(r, "errors") == 0);
    if (intField(r, "errors") != 0) std::printf("%s\n", r.message.c_str());
    {
        // the planes join every GND / +5V pad (through holes directly, SMD pads through the fan-out vias)
        const Circuit c = circuitOf(s, "ckt1");
        const auto fills = fillZones(c, buildNetlist(c));
        CHECK(fills.size() == 2 && fills[0].layer == 2 && fills[1].layer == 3);
    }
    // traces on inner layers only on 4-layer boards; a trace drawn on In1 is checked like any other
    CHECK(run(b, "circuit.trace doc=$c layer=In1 points=5,15,30,15 net=IN width=0.3").ok);
    CHECK(run(b, "undo").ok);
    // fab output: four copper layers, all parse; the NPTH hole goes in its own drill file
    r = run(b, "circuit.gerber doc=$c path=" + q(dir / "four"));
    CHECK(r.ok);
    for (const char* f : {"four-F_Cu.gtl", "four-In1_Cu.g2", "four-In2_Cu.g3", "four-B_Cu.gbl", "four-F_Mask.gts", "four-B_Mask.gbs", "four-F_Paste.gtp",
                          "four-F_Silkscreen.gto", "four-Edge_Cuts.gm1"}) {
        const GerberInfo g = parseGerber(readAll(dir / "four" / f));
        CHECK(g.ok);
        if (!g.ok) std::printf("  %s: %s\n", f, g.error.c_str());
    }
    const GerberInfo in1 = parseGerber(readAll(dir / "four" / "four-In1_Cu.g2"));
    CHECK(in1.function == "Copper,L2,Inr" && in1.regions > 0);
    CHECK(parseGerber(readAll(dir / "four" / "four-B_Cu.gbl")).function == "Copper,L4,Bot");
    // every top pad and via is flashed on F.Cu, inside the board outline (y negated in Gerber)
    {
        const Circuit c = circuitOf(s, "ckt1");
        const auto pads = worldPads(c, buildNetlist(c));
        int topPads = 0;
        for (const auto& p : pads) topPads += (p.layers & 1) && p.plated ? 1 : 0;
        const GerberInfo top = parseGerber(readAll(dir / "four" / "four-F_Cu.gtl"));
        CHECK(top.flashes == topPads + static_cast<int>(c.board.vias.size()));
        CHECK(top.minX >= 0 && top.maxX <= 40 && top.maxY <= 0 && top.minY >= -30);
        for (const auto& p : pads) {
            if (!(p.layers & 1) || !p.plated) continue;
            bool found = false;
            for (const auto& [fx, fy] : top.flashAt) found = found || (std::abs(fx - p.pos.x) < 1e-3 && std::abs(fy + p.pos.y) < 1e-3);
            CHECK(found);
        }
    }
    const DrillInfo pth = parseDrill(readAll(dir / "four" / "four.drl"));
    CHECK(pth.ok && pth.holes > 8);
    const DrillInfo npth = parseDrill(readAll(dir / "four" / "four-NPTH.drl"));
    CHECK(npth.ok && npth.holes == 1 && npth.tools.size() == 1 && std::abs(npth.tools.begin()->second - 3.2) < 1e-9);
    // assembly files: the pick-and-place lists placed parts (not the hole), y up from the bottom-left corner
    const std::string pos = readAll(dir / "four" / "four-pos.csv");
    CHECK(pos.rfind("Designator,Val,Package,Mid X,Mid Y,Rotation,Layer\n", 0) == 0);
    CHECK(pos.find("U1,ATtiny85-20PU,DIP-8,") != std::string::npos && pos.find("H1") == std::string::npos);
    CHECK(pos.find(",-") == std::string::npos);
    CHECK(readAll(dir / "four" / "four-bom.csv").find("ATtiny85") != std::string::npos);
    r = run(b, "circuit.placement doc=$c");
    CHECK(r.ok && intField(r, "parts") == 5);
    // 3D view
    r = run(b, "circuit.preview doc=$c view=3d size=600");
    CHECK(r.ok && r.attachments.size() == 1);
    CHECK(run(b, "circuit.export doc=$c view=3d mask=black from_below=true path=" + q(dir / "b3d.png")).ok);
    CHECK(fs::file_size(dir / "b3d.png") > 5000);
    CHECK(!run(b, "circuit.export doc=$c view=3d path=" + q(dir / "b3d.svg")).ok);
    // back to 2 layers: refused while inner traces exist, pours on inner layers dropped
    r = run(b, "circuit.board doc=$c layers=2");
    CHECK(r.ok);
    {
        const Circuit c = circuitOf(s, "ckt1");
        CHECK(c.board.layers == 2 && c.board.zones.empty());
    }
    CHECK(!run(b, "circuit.pour doc=$c net=GND layer=In1").ok);
    CHECK(run(b, "undo").ok);
    // layers survive save and open
    CHECK(run(b, "circuit.save doc=$c path=" + q(dir / "four.circuit.json")).ok);
    const Circuit back = fromJson(readAll(dir / "four.circuit.json"));
    CHECK(back.board.layers == 4 && back.board.zones.size() == 2 && back.board.zones[0].layer == 2 && back.board.zones[0].thermal);
}

void testThermalAndDrc() {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=header pins=2 ref=J1").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=resistor").ok);
    CHECK(run(b, "circuit.net doc=$c name=A pins=J1.1,R1.1").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=gnd pins=J1.2,R1.2").ok);
    CHECK(run(b, "circuit.board doc=$c width=30 height=20").ok);
    CHECK(run(b, "circuit.footprint_move doc=$c ref=J1 x=8 y=10").ok);
    CHECK(run(b, "circuit.footprint_move doc=$c ref=R1 x=20 y=10").ok);
    CHECK(run(b, "circuit.route doc=$c nets=A").ok);
    // a thermal pour: the ring around J1.2 has gaps, yet the pad still joins the pour
    CHECK(run(b, "circuit.pour doc=$c net=GND layer=both clearance=0.4").ok);
    auto ringCells = [&](bool& connected) {
        const Circuit c = circuitOf(s, "ckt1");
        const Netlist nl = buildNetlist(c);
        const auto fills = fillZones(c, nl);
        Pt pad;
        for (const auto& p : worldPads(c, nl)) {
            if (p.ref == "J1" && p.number == "2") pad = p.pos;
        }
        connected = ratsnest(c, nl).empty();
        // cells on a circle just outside the pad (1.7 mm across) within the clearance gap
        int copper = 0, total = 0;
        for (int k = 0; k < 72; ++k) {
            const double a = k * 3.14159265358979 / 36, rr = 0.85 + 0.25;
            const Pt q{pad.x + rr * std::cos(a), pad.y + rr * std::sin(a)};
            const auto& f = fills[0];
            const int cx = static_cast<int>(std::floor((q.x - f.x0) / f.cell)), cy = static_cast<int>(std::floor((q.y - f.y0) / f.cell));
            ++total;
            copper += f.at(cx, cy) ? 1 : 0;
        }
        return std::make_pair(copper, total);
    };
    bool connected = false;
    auto [copper, total] = ringCells(connected);
    CHECK(connected);
    CHECK(copper > 0 && copper < total / 2);  // spokes only
    CHECK(run(b, "circuit.pour doc=$c net=GND layer=both clearance=0.4 thermal=false").ok);
    std::tie(copper, total) = ringCells(connected);
    CHECK(connected && copper == total);  // solid
    // DRC catches: a too-thin trace, a tiny via ring, a trace too near another net, a trace off the board
    CHECK(run(b, "circuit.unpour doc=$c").ok);
    CHECK(run(b, "circuit.route doc=$c").ok);
    CHECK(run(b, "circuit.trace doc=$c points=2,2,5,2 width=0.1 net=A").ok);
    Result r = run(b, "circuit.drc doc=$c");
    CHECK(r.message.find("[trace_width]") != std::string::npos);
    CHECK(run(b, "undo").ok);
    CHECK(run(b, "circuit.via doc=$c x=3 y=3 drill=0.4 diameter=0.5").ok);
    CHECK(run(b, "circuit.drc doc=$c").message.find("[annular_ring]") != std::string::npos);
    CHECK(run(b, "undo").ok);
    CHECK(run(b, "circuit.trace doc=$c points=1,19.9,10,19.9 net=A").ok);
    CHECK(run(b, "circuit.drc doc=$c").message.find("[edge_clearance]") != std::string::npos);
    CHECK(run(b, "undo").ok);
    {
        // 0.1 mm from J1.2 (GND) with net A: a clearance error
        const Circuit c = circuitOf(s, "ckt1");
        Pt p2;
        for (const auto& p : worldPads(c, buildNetlist(c))) {
            if (p.ref == "J1" && p.number == "2") p2 = p.pos;
        }
        const double x = p2.x + 0.85 + 0.1 + 0.125;
        char line[160];
        std::snprintf(line, sizeof line, "circuit.trace doc=$c layer=B net=A points=%.3f,%.3f,%.3f,%.3f", x, p2.y - 1, x, p2.y + 1);
        CHECK(run(b, line).ok);
        CHECK(run(b, "circuit.drc doc=$c").message.find("[clearance]") != std::string::npos);
        CHECK(run(b, "undo").ok);
    }
    r = run(b, "circuit.drc doc=$c");
    CHECK(intField(r, "errors") == 0);
    if (intField(r, "errors") != 0) std::printf("%s\n", r.message.c_str());
}

}  // namespace

int main() {
    const fs::path dir = tempDir();
    testParts();
    testFourLayer(dir);
    testThermalAndDrc();
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("circuit_board");
}
