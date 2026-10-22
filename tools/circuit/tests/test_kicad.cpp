// KiCad interchange: hand-written KiCad 7 schematic (hierarchical sheet, power symbols, a multi-unit
// part with a hidden power pin) and board snippets, and round trips through .kicad_sch / .kicad_pcb.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::circuit;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_kicad_" + std::to_string(stamp) + "_" + std::to_string(rd()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

void writeAll(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

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

Circuit circuitOf(Session& s, const std::string& id) { return s.workspace().getAs<CircuitDoc>(id)->circuit; }

// pin -> every pin on its net, so two circuits can be compared whatever their nets are called
std::map<std::string, std::string> partition(const Circuit& c) {
    const Netlist nl = buildNetlist(c);
    std::map<std::string, std::string> out;
    for (const auto& n : nl.nets) {
        std::vector<std::string> members;
        for (const auto& node : n.nodes) members.push_back(node.ref + "." + node.pin);
        std::sort(members.begin(), members.end());
        members.erase(std::unique(members.begin(), members.end()), members.end());
        std::string joined;
        for (const auto& m : members) joined += m + " ";
        for (const auto& m : members) out[m] = joined;
    }
    return out;
}

const char* kStroke = "(stroke (width 0) (type default))";

std::string libR() {
    return R"(    (symbol "Device:R" (pin_numbers hide) (pin_names (offset 0)) (in_bom yes) (on_board yes)
      (property "Reference" "R" (at 2.032 0 90) (effects (font (size 1.27 1.27))))
      (property "Value" "R" (at 0 0 90) (effects (font (size 1.27 1.27))))
      (property "Footprint" "" (at -1.778 0 90) (effects (font (size 1.27 1.27)) hide))
      (symbol "R_0_1"
        (rectangle (start -1.016 -2.54) (end 1.016 2.54) (stroke (width 0.254) (type default)) (fill (type none))))
      (symbol "R_1_1"
        (pin passive line (at 0 3.81 270) (length 1.27) (name "~" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 0 -3.81 90) (length 1.27) (name "~" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))))
)";
}

std::string libGnd() {
    return R"(    (symbol "power:GND" (power) (pin_names (offset 0)) (in_bom yes) (on_board yes)
      (property "Reference" "#PWR" (at 0 -6.35 0) (effects (font (size 1.27 1.27)) hide))
      (property "Value" "GND" (at 0 -3.81 0) (effects (font (size 1.27 1.27))))
      (symbol "GND_0_1"
        (polyline (pts (xy 0 0) (xy 0 -1.27) (xy 1.27 -1.27) (xy 0 -2.54) (xy -1.27 -1.27) (xy 0 -1.27)) (stroke (width 0) (type default)) (fill (type none))))
      (symbol "GND_1_1"
        (pin power_in line (at 0 0 270) (length 0) hide (name "GND" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))))
)";
}

std::string symbolInstance(const std::string& lib, const std::string& at, const std::string& ref, const std::string& value, const std::string& path,
                           int unit = 1, const std::string& extra = "") {
    static int n = 0;
    char uuid[48];
    std::snprintf(uuid, sizeof uuid, "aaaaaaaa-0000-0000-0000-%012d", ++n);
    return "  (symbol (lib_id \"" + lib + "\") (at " + at + ") (unit " + std::to_string(unit) + ") (in_bom yes) (on_board yes) (dnp no)\n    (uuid " + uuid +
           ")\n    (property \"Reference\" \"" + ref + "\" (at 0 0 0) (effects (font (size 1.27 1.27))))\n    (property \"Value\" \"" + value +
           "\" (at 0 0 0) (effects (font (size 1.27 1.27))))\n" + extra + "    (instances (project \"demo\" (path \"" + path + "\" (reference \"" + ref +
           "\") (unit " + std::to_string(unit) + "))))\n  )\n";
}

std::string wire(const std::string& a, const std::string& b) {
    return std::string("  (wire (pts (xy ") + a + ") (xy " + b + ")) " + kStroke + " (uuid bbbbbbbb-0000-0000-0000-000000000000))\n";
}

const std::string kRoot = "/11111111-1111-1111-1111-111111111111";

std::string rootSchematic() {
    std::string s = "(kicad_sch (version 20230121) (generator eeschema)\n  (uuid 11111111-1111-1111-1111-111111111111)\n  (paper \"A4\")\n"
                    "  (title_block (title \"Demo\"))\n  (lib_symbols\n";
    s += libR() + libGnd();
    s += R"(    (symbol "Device:LED" (pin_numbers hide) (pin_names (offset 1.016) hide) (in_bom yes) (on_board yes)
      (property "Reference" "D" (at 0 2.54 0) (effects (font (size 1.27 1.27))))
      (property "Value" "LED" (at 0 -2.54 0) (effects (font (size 1.27 1.27))))
      (symbol "LED_0_1"
        (polyline (pts (xy -1.27 -1.27) (xy -1.27 1.27)) (stroke (width 0.254) (type default)) (fill (type none)))
        (polyline (pts (xy 1.27 -1.27) (xy 1.27 1.27) (xy -1.27 0) (xy 1.27 -1.27)) (stroke (width 0.254) (type default)) (fill (type none))))
      (symbol "LED_1_1"
        (pin passive line (at -3.81 0 0) (length 2.54) (name "K" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 3.81 0 180) (length 2.54) (name "A" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))))
    (symbol "power:+5V" (power) (pin_names (offset 0)) (in_bom yes) (on_board yes)
      (property "Reference" "#PWR" (at 0 -3.81 0) (effects (font (size 1.27 1.27)) hide))
      (property "Value" "+5V" (at 0 3.556 0) (effects (font (size 1.27 1.27))))
      (symbol "+5V_0_1"
        (polyline (pts (xy -0.762 1.27) (xy 0 2.54) (xy 0.762 1.27)) (stroke (width 0) (type default)) (fill (type none)))
        (polyline (pts (xy 0 0) (xy 0 2.54)) (stroke (width 0) (type default)) (fill (type none))))
      (symbol "+5V_1_1"
        (pin power_in line (at 0 0 90) (length 0) hide (name "+5V" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))))
    (symbol "test:DUAL" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 5.08 0) (effects (font (size 1.27 1.27))))
      (property "Value" "DUAL" (at 0 -5.08 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "DUAL_0_1"
        (rectangle (start -2.54 -2.54) (end 2.54 2.54) (stroke (width 0.254) (type default)) (fill (type background))))
      (symbol "DUAL_1_1"
        (pin input line (at -5.08 0 0) (length 2.54) (name "A" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin output line (at 5.08 0 180) (length 2.54) (name "Y" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 0 5.08 270) (length 2.54) hide (name "VCC" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27))))))
      (symbol "DUAL_2_1"
        (pin input line (at -5.08 0 0) (length 2.54) (name "B" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin output line (at 5.08 0 180) (length 2.54) (name "Z" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))))
  )
)";
    // +5V -> R1 -> (LED_A, to the sub-sheet) -> D1 (turned 90 degrees) -> GND
    s += symbolInstance("power:+5V", "101.6 43.18 0", "#PWR01", "+5V", kRoot);
    s += symbolInstance("Device:R", "101.6 50.8 0", "R1", "330", kRoot, 1,
                        "    (property \"Footprint\" \"Resistor_SMD:R_0805_2012Metric\" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n"
                        "    (property \"MPN\" \"RC0805FR-07330RL\" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n");
    s += symbolInstance("Device:LED", "101.6 63.5 90", "D1", "red", kRoot);
    s += symbolInstance("power:GND", "101.6 69.85 0", "#PWR02", "GND", kRoot);
    s += symbolInstance("test:DUAL", "152.4 50.8 0", "U1", "DUAL", kRoot, 1);
    s += symbolInstance("test:DUAL", "152.4 76.2 0", "U1", "DUAL", kRoot, 2);
    s += wire("101.6 43.18", "101.6 46.99");
    s += wire("101.6 54.61", "101.6 59.69");
    s += wire("101.6 67.31", "101.6 69.85");
    s += wire("101.6 57.15", "127 57.15");
    s += "  (junction (at 101.6 57.15) (diameter 0) (color 0 0 0 0) (uuid cccccccc-0000-0000-0000-000000000000))\n";
    s += "  (label \"LED_A\" (at 110 57.15 0) (effects (font (size 1.27 1.27)) (justify left bottom)) (uuid dddddddd-0000-0000-0000-000000000000))\n";
    for (const char* nc : {"147.32 50.8", "157.48 50.8", "147.32 76.2", "157.48 76.2"})
        s += std::string("  (no_connect (at ") + nc + ") (uuid eeeeeeee-0000-0000-0000-000000000000))\n";
    s += R"(  (sheet (at 127 50.8) (size 12.7 12.7) (fields_autoplaced)
    (stroke (width 0.1524) (type solid)) (fill (color 0 0 0 0.0000))
    (uuid 22222222-2222-2222-2222-222222222222)
    (property "Sheetname" "Sub" (at 127 50.0884 0) (effects (font (size 1.27 1.27)) (justify left bottom)))
    (property "Sheetfile" "sub.kicad_sch" (at 127 64.0846 0) (effects (font (size 1.27 1.27)) (justify left top)))
    (pin "IN" input (at 127 57.15 180) (effects (font (size 1.27 1.27)) (justify left)) (uuid ffffffff-0000-0000-0000-000000000000))
  )
  (sheet_instances (path "/" (page "1")) (path "/22222222-2222-2222-2222-222222222222" (page "2")))
)
)";
    return s;
}

std::string subSchematic() {
    const std::string path = kRoot + "/22222222-2222-2222-2222-222222222222";
    std::string s = "(kicad_sch (version 20230121) (generator eeschema)\n  (uuid 33333333-3333-3333-3333-333333333333)\n  (paper \"A4\")\n  (lib_symbols\n";
    s += libR() + libGnd() + "  )\n";
    s += symbolInstance("Device:R", "50.8 50.8 90", "R2", "10k", path);
    s += symbolInstance("power:GND", "54.61 50.8 0", "#PWR03", "GND", path);
    s += "  (hierarchical_label \"IN\" (shape input) (at 46.99 50.8 180) (effects (font (size 1.27 1.27)) (justify right)) (uuid 44444444-0000-0000-0000-000000000000))\n";
    s += ")\n";
    return s;
}

void testSchematicRead(const fs::path& dir) {
    writeAll(dir / "demo.kicad_sch", rootSchematic());
    writeAll(dir / "sub.kicad_sch", subSchematic());
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    Result r = run(b, "circuit.open path=" + q(dir / "demo.kicad_sch") + " -> d");
    CHECK(r.ok);
    CHECK(r.message.find("flattened") != std::string::npos);
    const Circuit c = circuitOf(s, std::get<std::string>(r.value));
    CHECK(c.title == "Demo");
    const Netlist nl = buildNetlist(c);
    CHECK(nl.netOf("R1", "1") == "+5V");
    const std::string a = nl.netOf("R1", "2");
    CHECK(!a.empty() && a == nl.netOf("D1", "2") && a == nl.netOf("R2", "1"));  // through the sheet pin
    const Net* an = nl.net(a);
    CHECK(an && std::find(an->names.begin(), an->names.end(), "LED_A") != an->names.end() &&
          std::find(an->names.begin(), an->names.end(), "Sub/IN") != an->names.end());
    CHECK(nl.netOf("D1", "1") == "GND" && nl.netOf("R2", "2") == "GND");
    CHECK(nl.netOf("U1", "8") == "VCC");  // hidden power pin: an implicit global net
    // R keeps KiCad's drawing but is known as a resistor (simulation, 3D); the footprint maps to ours
    const Part* r1 = c.part("R1");
    CHECK(r1 && r1->customSymbol && r1->symbol == "resistor" && r1->footprint == "R_0805" && r1->fields.at("MPN") == "RC0805FR-07330RL");
    const Part* d1 = c.part("D1");
    CHECK(d1 && d1->rot == 90 && !d1->mirror);
    // two units of U1: one reference, one footprint, no duplicate-reference error, one BOM row
    int units = 0, withFp = 0;
    for (const auto& p : c.parts) {
        if (p.ref == "U1") ++units, withFp += !p.footprint.empty();
    }
    CHECK(units == 2 && withFp == 1);
    for (const auto& i : erc(c, nl)) {
        CHECK(i.code != "duplicate_ref");
        if (i.severity == "error") std::printf("  erc: %s\n", i.message.c_str());
    }
    const std::string bom = bomCsv(c, false);
    CHECK(std::count(bom.begin(), bom.end(), '\n') == 5);  // header, D1, R1, R2, U1
    CHECK(bom.find("SOIC-8") != std::string::npos);

    // KiCad -> .kicad_sch -> back: the same connections and net names
    const std::string again = toKicadSchematic(c);
    std::vector<std::string> warnings;
    const Circuit c2 = fromKicadSchematic(again, "", &warnings);
    CHECK(partition(c2) == partition(c));
    const Netlist nl2 = buildNetlist(c2);
    CHECK(nl2.netOf("R1", "1") == "+5V" && nl2.netOf("D1", "1") == "GND" && nl2.netOf("U1", "8") == "VCC");
    CHECK(c2.part("R1") && c2.part("R1")->symbol == "resistor");
    // and through .circuit.json, custom symbols included
    const Circuit c3 = fromJson(toJson(c));
    CHECK(partition(c3) == partition(c));
    CHECK(c3.part("R1") && c3.part("R1")->customSymbol && c3.part("R1")->customSymbol->shapes.size() == r1->customSymbol->shapes.size());
    // the schematic draws (KiCad symbols included)
    CHECK(pageSvg(schematicPage(c)).find("<svg") != std::string::npos);
}

void testRoundTrip(const fs::path& dir) {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new title=trip -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=attiny85 x=0 y=0").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=capacitor value=100n x=-12 y=-4").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=resistor value=330 x=12 y=-4").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=led x=18 y=-4").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=header pins=3 ref=J1 x=20 y=6").ok);
    CHECK(run(b, "circuit.net doc=$c name=LED pins=U1.PB0,R1.1").ok);
    CHECK(run(b, "circuit.connect doc=$c from=R1.2 to=D1.A").ok);
    CHECK(run(b, "circuit.net doc=$c name=IN pins=U1.PB3,J1.3").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=gnd pins=U1.GND,C1.2,D1.K,J1.1").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=+5v pins=U1.VCC,C1.1,J1.2").ok);
    CHECK(run(b, "circuit.no_connect doc=$c pins=U1.PB5,U1.PB1,U1.PB2,U1.PB4").ok);
    CHECK(run(b, "circuit.board doc=$c width=40 height=30").ok);
    CHECK(run(b, "circuit.pcb_place doc=$c").ok);
    CHECK(run(b, "circuit.footprint_move doc=$c ref=R1 side=bottom rot=90").ok);
    CHECK(run(b, "circuit.pour doc=$c net=GND layer=B").ok);
    CHECK(run(b, "circuit.fanout doc=$c").ok);
    CHECK(run(b, "circuit.route doc=$c skip=GND").ok);
    const Circuit orig = circuitOf(s, "ckt1");
    const Netlist onl = buildNetlist(orig);
    CHECK(intField(run(b, "circuit.drc doc=$c"), "errors") == 0);

    // a KiCad project: schematic + board, reopened together
    Result r = run(b, "circuit.save doc=$c path=" + q(dir / "trip.kicad_pro"));
    CHECK(r.ok && fs::exists(dir / "trip.kicad_sch") && fs::exists(dir / "trip.kicad_pcb"));
    r = run(b, "circuit.open path=" + q(dir / "trip.kicad_pro") + " -> k");
    CHECK(r.ok);
    const Circuit back = circuitOf(s, std::get<std::string>(r.value));
    CHECK(partition(back) == partition(orig));
    CHECK(back.board.traces.size() == orig.board.traces.size() && back.board.vias.size() == orig.board.vias.size());
    CHECK(back.board.zones.size() == 1 && back.board.zones[0].net == "GND" && back.board.zones[0].layer == 1);
    CHECK(std::abs(boardWidth(back.board) - 40) < 1e-6 && std::abs(boardHeight(back.board) - 30) < 1e-6);
    // every pad lands where it was, on the same net
    const Netlist bnl = buildNetlist(back);
    auto pads = worldPads(orig, onl), pads2 = worldPads(back, bnl);
    CHECK(pads.size() == pads2.size());
    for (const auto& p : pads) {
        bool found = false;
        for (const auto& p2 : pads2) {
            if (p2.ref == p.ref && p2.number == p.number) {
                found = std::hypot(p.pos.x - p2.pos.x, p.pos.y - p2.pos.y) < 1e-3 && p.net == p2.net && std::abs(p.w - p2.w) < 1e-3 &&
                        std::abs(p.h - p2.h) < 1e-3 && p.layers == p2.layers;
            }
        }
        CHECK(found);
        if (!found) std::printf("  pad %s.%s moved\n", p.ref.c_str(), p.number.c_str());
    }
    r = run(b, "circuit.drc doc=$k");
    CHECK(intField(r, "errors") == 0);
    if (intField(r, "errors") != 0) std::printf("%s\n", r.message.c_str());

    // the board alone: parts get boxed symbols, pins labelled with the pads' nets
    r = run(b, "circuit.open path=" + q(dir / "trip.kicad_pcb") + " -> p");
    CHECK(r.ok);
    const Circuit pcbOnly = circuitOf(s, std::get<std::string>(r.value));
    const Netlist pnl = buildNetlist(pcbOnly);
    for (const auto& p : pads) {
        if (!p.net.empty()) CHECK(pnl.netOf(p.ref, p.number) == p.net);
    }
    CHECK(intField(run(b, "circuit.drc doc=$p"), "errors") == 0);
    CHECK(intField(run(b, "circuit.erc doc=$p"), "errors") == 0);

    // import_board onto the original schematic (undoable)
    CHECK(run(b, "circuit.unroute doc=$c").ok);
    r = run(b, "circuit.import_board doc=$c path=" + q(dir / "trip.kicad_pcb"));
    CHECK(r.ok && intField(r, "placed") == 5);
    CHECK(circuitOf(s, "ckt1").board.traces.size() == orig.board.traces.size());
    CHECK(run(b, "undo").ok);
    CHECK(circuitOf(s, "ckt1").board.traces.empty());
}

void testPcbRead() {
    // a board drawn by hand: a rotated top part, a bottom part, an arc track, a via, a solid pour
    const std::string pcb = R"((kicad_pcb (version 20221018) (generator pcbnew)
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (36 "B.SilkS" user) (37 "F.SilkS" user) (44 "Edge.Cuts" user))
  (net 0 "")
  (net 1 "GND")
  (net 2 "/SIG")
  (footprint "Resistor_SMD:R_0805_2012Metric" (layer "F.Cu") (at 110 60 90)
    (fp_text reference "R1" (at 0 -1.65 90) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))
    (fp_text value "1k" (at 0 1.65 90) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))
    (fp_line (start -0.2 -0.7) (end 0.2 -0.7) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))
    (pad "1" smd roundrect (at -1 0 90) (size 1 1.4) (layers "F.Cu" "F.Paste" "F.Mask") (net 2 "/SIG") (pinfunction "A"))
    (pad "2" smd roundrect (at 1 0 90) (size 1 1.4) (layers "F.Cu" "F.Paste" "F.Mask") (net 1 "GND"))
  )
  (footprint "Resistor_SMD:R_0805_2012Metric" (layer "B.Cu") (at 120 60)
    (fp_text reference "R2" (at 0 1.65) (layer "B.SilkS") (effects (font (size 1 1) (thickness 0.15)) (justify mirror)))
    (fp_text value "1k" (at 0 -1.65) (layer "B.Fab") (effects (font (size 1 1) (thickness 0.15)) (justify mirror)))
    (pad "1" smd roundrect (at -1 0.5 180) (size 1 1.4) (layers "B.Cu" "B.Paste" "B.Mask") (net 2 "/SIG"))
    (pad "2" smd roundrect (at 1 0.5 180) (size 1 1.4) (layers "B.Cu" "B.Paste" "B.Mask"))
  )
  (gr_rect (start 100 50) (end 130 70) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
  (arc (start 110 59) (mid 114 56) (end 118 59) (width 0.25) (layer "F.Cu") (net 2))
  (via (at 118 59) (size 0.8) (drill 0.4) (layers "F.Cu" "B.Cu") (net 2))
  (zone (net 1) (net_name "GND") (layer "B.Cu") (hatch edge 0.5) (connect_pads yes (clearance 0.4)) (min_thickness 0.3)
    (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))
    (polygon (pts (xy 100 50) (xy 130 50) (xy 130 70) (xy 100 70))))
)
)";
    std::vector<std::string> warnings;
    const Circuit c = fromKicadPcb(pcb, &warnings);
    CHECK(c.parts.size() == 2);
    CHECK(c.board.outline.size() == 4 && std::abs(boardWidth(c.board) - 30) < 1e-9 && std::abs(boardHeight(c.board) - 20) < 1e-9);
    const Netlist nl = buildNetlist(c);
    const auto pads = worldPads(c, nl);
    auto padAt = [&](const std::string& ref, const std::string& n) {
        for (const auto& p : pads) {
            if (p.ref == ref && p.number == n) return p;
        }
        return WorldPad{};
    };
    // R1 turned 90: its pad 1 (left) goes below the centre; pads are 1.4 wide x 1 high once turned
    const WorldPad a = padAt("R1", "1");
    CHECK(std::abs(a.pos.x - 10) < 1e-6 && std::abs(a.pos.y - 11) < 1e-6 && a.net == "SIG" && (a.layers & 1));
    CHECK(std::abs(a.w - 1.4) < 1e-6 && std::abs(a.h - 1) < 1e-6);
    // R2 on the bottom: KiCad's coordinates are already flipped
    const WorldPad b = padAt("R2", "1");
    CHECK(std::abs(b.pos.x - 19) < 1e-6 && std::abs(b.pos.y - 10.5) < 1e-6 && b.layers == 2 && b.net == "SIG");
    CHECK(padAt("R2", "2").net != "SIG" && nl.net(padAt("R2", "2").net)->nodes.size() == 1);  // no net in the file: a no-connect mark
    CHECK(c.noConnects.size() == 1);
    CHECK(c.board.traces.size() == 8 && c.board.traces[0].net == "SIG" && std::abs(c.board.traces[0].a.x - 10) < 1e-6);
    CHECK(c.board.vias.size() == 1 && c.board.vias[0].net == "SIG");
    CHECK(c.board.zones.size() == 1 && !c.board.zones[0].thermal && std::abs(c.board.zones[0].clearance - 0.4) < 1e-9);
    CHECK(c.part("R1")->customSymbol->pins[0].name == "A");
    // writing it back and reading it again keeps the pads in place
    const Circuit c2 = fromKicadPcb(toKicadPcb(c), nullptr);
    const auto pads2 = worldPads(c2, buildNetlist(c2));
    CHECK(pads2.size() == pads.size());
    for (const auto& p : pads) {
        bool found = false;
        for (const auto& p2 : pads2) found = found || (p2.ref == p.ref && p2.number == p.number && std::hypot(p.pos.x - p2.pos.x, p.pos.y - p2.pos.y) < 1e-3 && p.layers == p2.layers);
        CHECK(found);
    }
    // bad input is refused, not crashed on
    bool threw = false;
    try {
        fromKicadPcb("(kicad_sch)", nullptr);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        fromKicadSchematic("(kicad_sch (lib_symbols", "", nullptr);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    const fs::path dir = tempDir();
    testSchematicRead(dir);
    testRoundTrip(dir);
    testPcbRead();
    if (std::getenv("WETWARESUITE_KEEP_TEST_FILES")) std::printf("files kept in %s\n", dir.string().c_str());
    else fs::remove_all(dir);
    return check::summary("circuit_kicad");
}
