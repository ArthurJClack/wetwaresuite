#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::circuit;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_circuit_" + std::to_string(stamp) + "_" + std::to_string(rd()));
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

double numField(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return -999;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return -998;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

Circuit& circuitOf(Session& s, const std::string& id) { return s.workspace().getAs<CircuitDoc>(id)->circuit; }

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

void testValues() {
    CHECK(near(*parseValue("4k7"), 4700, 1e-9));
    CHECK(near(*parseValue("10k"), 10000, 1e-9));
    CHECK(near(*parseValue("100n"), 1e-7, 1e-18));
    CHECK(near(*parseValue("1M"), 1e6, 1e-6));
    CHECK(near(*parseValue("1meg"), 1e6, 1e-6));
    CHECK(near(*parseValue("2m"), 2e-3, 1e-12));
    CHECK(near(*parseValue("5V"), 5, 1e-12));
    CHECK(near(*parseValue("2R2"), 2.2, 1e-12));
    CHECK(near(*parseValue("4.7uF"), 4.7e-6, 1e-15));
    CHECK(!parseValue("red"));
    CHECK(!parseValue("LM358"));
    CHECK(formatValue(4700) == "4.7k");
    CHECK(isPowerName("GND") && isPowerName("+5V") && isPowerName("3V3") && isPowerName("VCC") && !isPowerName("SDA") && !isPowerName("LED1"));
}

void testLibrary() {
    for (const auto& e : symbolCatalog()) {
        auto s = findSymbol(e.name, 6);
        CHECK(s != nullptr);
        if (!s) continue;
        CHECK(!s->pins.empty() || s->name == "mounting_hole");
        for (const auto& p : s->pins) CHECK(std::abs(p.pos.x - std::round(p.pos.x)) < 1e-9 && std::abs(p.pos.y - std::round(p.pos.y)) < 1e-9);
        if (!s->footprint.empty()) {
            auto f = findFootprint(s->footprint);
            CHECK(f != nullptr);
            if (!f) continue;
            for (const auto& p : s->pins) {
                bool found = false;
                for (const auto& pad : f->pads) found = found || pad.number == p.number;
                CHECK(found || p.type == PinType::NotConnected || s->name == "opamp");
            }
        }
    }
    CHECK(canonicalSymbol("R") == "resistor" && canonicalSymbol("uno") == "arduino_uno" && canonicalSymbol("+5V") == "power");
    CHECK(findSymbol("+3v3")->value == "+3V3");
    CHECK(findSymbol("ic", 8, {"VCC", "GND"})->pins[0].name == "VCC");
    CHECK(findSymbol("nope") == nullptr);
    CHECK(findFootprint("SOIC-14")->pads.size() == 14);
    CHECK(findFootprint("DIP-28")->pads.size() == 28);
    CHECK(findFootprint("PinHeader_2x05")->pads.size() == 10);
    CHECK(findFootprint("Resistor_SMD:R_0805_2012Metric")->name == "R_0805");
    CHECK(findFootprint("Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical") == nullptr ||
          findFootprint("PinHeader_1x04_P2.54mm_Vertical")->pads.size() == 4);
    CHECK(findFootprint("Arduino_Uno_Shield")->pads.size() == 32);
    CHECK(findFootprint("BluePill")->pads.size() == 40);
    CHECK(findFootprint("XYZ") == nullptr);
    // DIP numbering: pad 5 of DIP-8 is opposite pad 4
    auto dip = findFootprint("DIP-8");
    CHECK(near(dip->pads[4].pos.y, dip->pads[3].pos.y, 1e-9) && dip->pads[4].pos.x > 0);
}

void testSchematic(const fs::path& dir) {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new title=divider -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=resistor value=10k x=0 y=0").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=resistor value=10k x=0 y=6 rot=90").ok);
    Result r = run(b, "circuit.place doc=$c symbol=capacitor value=1u");
    CHECK(r.ok && numField(r, "x") > 0);
    CHECK(run(b, "circuit.power doc=$c symbol=+5v pins=R1.1").ok);
    CHECK(run(b, "circuit.connect doc=$c from=R1.2 to=R2.2").ok);
    CHECK(run(b, "circuit.net doc=$c name=MID pins=R1.2,C1.1").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=gnd pins=R2.1,C1.2").ok);
    Circuit& c = circuitOf(s, "ckt1");
    Netlist nl = buildNetlist(c);
    CHECK(nl.netOf("R1", "2") == "MID");
    CHECK(nl.netOf("R2", "2") == "MID");
    CHECK(nl.netOf("C1", "1") == "MID");
    CHECK(nl.netOf("R1", "1") == "+5V");
    CHECK(nl.netOf("R2", "1") == "GND" && nl.netOf("C1", "2") == "GND");
    r = run(b, "circuit.erc doc=$c");
    CHECK(r.ok && intField(r, "errors") == 0 && intField(r, "warnings") == 0);
    // moving a part keeps its wires on
    CHECK(run(b, "circuit.move doc=$c ref=R2 dx=-4 dy=2").ok);
    nl = buildNetlist(c);
    CHECK(nl.netOf("R2", "2") == "MID" && nl.netOf("R2", "1") == "GND");
    CHECK(run(b, "circuit.move doc=$c ref=R2 rot=0").ok);
    nl = buildNetlist(c);
    CHECK(nl.netOf("R2", "2") == "MID" && nl.netOf("R2", "1") == "GND");
    // simulation
    r = run(b, "circuit.sim_op doc=$c");
    CHECK(r.ok && near(numField(r, "V(MID)"), 2.5, 1e-6));
    r = run(b, "circuit.sim_tran doc=$c tstop=50m step=0.1m probes=MID preview=true path=" + q(dir / "tran.csv"));
    CHECK(r.ok && r.attachments.size() == 1 && near(numField(r, "V(MID)"), 2.5, 0.01));
    CHECK(readAll(dir / "tran.csv").rfind("time,V(MID)", 0) == 0);
    // ERC catches problems
    CHECK(run(b, "circuit.place doc=$c symbol=led ref=D9 x=40 y=40").ok);
    r = run(b, "circuit.erc doc=$c");
    CHECK(intField(r, "errors") == 2 && r.message.find("D9.1") != std::string::npos);
    CHECK(run(b, "circuit.no_connect doc=$c pins=D9.1,D9.2").ok);
    CHECK(intField(run(b, "circuit.erc doc=$c"), "errors") == 0);
    CHECK(run(b, "circuit.net doc=$c name=+5V pins=R2.1").ok);  // GND and +5V on one net
    r = run(b, "circuit.erc doc=$c");
    CHECK(r.message.find("shorted_power") != std::string::npos);
    CHECK(run(b, "undo").ok);
    c.parts.push_back(c.parts[0]);  // duplicate R1
    CHECK(erc(c, buildNetlist(c)).size() >= 1);
    bool dup = false;
    for (const auto& i : erc(c, buildNetlist(c))) dup = dup || i.code == "duplicate_ref";
    CHECK(dup);
    c.parts.pop_back();
    CHECK(!run(b, "circuit.connect doc=$c from=R1.9 to=R2.1").ok);
    CHECK(!run(b, "circuit.place doc=$c symbol=flux_capacitor").ok);
    // undo restores
    const std::size_t parts = c.parts.size();
    CHECK(run(b, "circuit.remove doc=$c ref=C1").ok);
    CHECK(c.parts.size() < parts);
    CHECK(run(b, "undo").ok);
    CHECK(c.parts.size() == parts);
    // files and exports
    CHECK(run(b, "circuit.save doc=$c path=" + q(dir / "div.circuit.json")).ok);
    CHECK(run(b, "circuit.open path=" + q(dir / "div.circuit.json") + " -> d").ok);
    CHECK(buildNetlist(circuitOf(s, "ckt2")).nets.size() == buildNetlist(c).nets.size());
    CHECK(run(b, "circuit.export doc=$c path=" + q(dir / "s.svg")).ok);
    CHECK(readAll(dir / "s.svg").find("<svg") != std::string::npos);
    CHECK(run(b, "circuit.export doc=$c path=" + q(dir / "s.pdf")).ok);
    CHECK(readAll(dir / "s.pdf").rfind("%PDF", 0) == 0);
    CHECK(run(b, "circuit.export doc=$c path=" + q(dir / "s.png")).ok);
    CHECK(readAll(dir / "s.png").substr(1, 3) == "PNG");
    r = run(b, "circuit.bom doc=$c");
    CHECK(r.ok && r.message.find("2,R1 R2,10k,R_0805") != std::string::npos);
    // netlist round trip through KiCad format, with Windows line endings
    CHECK(run(b, "circuit.export_netlist doc=$c path=" + q(dir / "div.net")).ok);
    std::string net = readAll(dir / "div.net");
    std::string crlf;
    for (char ch : net) crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
    std::ofstream(dir / "crlf.net", std::ios::binary) << crlf;
    r = run(b, "circuit.open path=" + q(dir / "crlf.net"));
    CHECK(r.ok);
    if (r.ok) {
        const Netlist a = buildNetlist(c), k = buildNetlist(circuitOf(s, std::get<std::string>(r.value)));
        CHECK(k.netOf("R1", "2") == "MID" && k.netOf("C1", "1") == "MID" && k.netOf("R2", "1") == "GND");
        const Result e = run(b, "circuit.erc doc=" + std::get<std::string>(r.value));
        CHECK(intField(e, "errors") == 2);  // D9's no-connect marks aren't part of a netlist
        if (intField(e, "errors") != 2) std::printf("%s\n", e.message.c_str());
    }
}

void testKicadImport(const fs::path& dir) {
    const std::string text =
        "(export (version \"E\")\n (design (source \"/tmp/x.kicad_sch\"))\n (components\n"
        "  (comp (ref \"U1\") (value \"NE555\") (footprint \"Package_DIP:DIP-8_W7.62mm\") (libsource (lib \"Timer\") (part \"NE555P\")))\n"
        "  (comp (ref \"R1\") (value \"1k\") (footprint \"Resistor_SMD:R_0805_2012Metric\") (libsource (lib \"Device\") (part \"R\")))\n"
        "  (comp (ref \"J1\") (value \"Conn\") (libsource (lib \"Connector\") (part \"Conn_01x02_Male\"))))\n"
        " (nets\n"
        "  (net (code \"1\") (name \"/VCC\") (node (ref \"U1\") (pin \"8\") (pinfunction \"VCC\")) (node (ref \"R1\") (pin \"1\")) (node (ref \"J1\") (pin \"1\")))\n"
        "  (net (code \"2\") (name \"GND\") (node (ref \"U1\") (pin \"1\") (pinfunction \"GND\")) (node (ref \"J1\") (pin \"2\")))\n"
        "  (net (code \"3\") (name \"Net-(R1-Pad2)\") (node (ref \"U1\") (pin \"7\")) (node (ref \"R1\") (pin \"2\")))))\n";
    std::ofstream(dir / "t.net", std::ios::binary) << text;
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    Result r = run(b, "circuit.open path=" + q(dir / "t.net"));
    CHECK(r.ok && r.message.find("generic") != std::string::npos);
    Circuit& c = circuitOf(s, "ckt1");
    CHECK(c.part("U1") && c.part("U1")->symbol == "ic" && c.part("U1")->footprint == "DIP-8");
    CHECK(c.part("R1")->symbol == "resistor" && c.part("R1")->footprint == "R_0805");
    CHECK(c.part("J1")->symbol == "header" && c.part("J1")->pins == 2);
    const Netlist nl = buildNetlist(c);
    CHECK(nl.netOf("U1", "8") == "VCC" && nl.netOf("R1", "1") == "VCC" && nl.netOf("J1", "1") == "VCC");
    CHECK(nl.netOf("U1", "1") == "GND" && nl.netOf("R1", "2") == nl.netOf("U1", "7"));
    std::string error;
    CHECK(resolvePin(c, "U1.VCC", error).part != nullptr);
    CHECK(!run(b, "circuit.open path=" + q(dir / "missing.net")).ok);
}

// A board: placement, routing, pour, DRC and fabrication output.
void testBoard(const fs::path& dir) {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new title=leds -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=header pins=4 ref=J1").ok);
    for (int i = 1; i <= 3; ++i) {
        CHECK(run(b, "circuit.place doc=$c symbol=resistor value=330").ok);
        CHECK(run(b, "circuit.place doc=$c symbol=led").ok);
        const std::string n = std::to_string(i);
        CHECK(run(b, "circuit.net doc=$c name=IN" + n + " pins=J1." + std::to_string(i + 1) + ",R" + n + ".1").ok);
        CHECK(run(b, "circuit.connect doc=$c from=R" + n + ".2 to=D" + n + ".A").ok);
        CHECK(run(b, "circuit.net doc=$c name=GND pins=D" + n + ".K").ok);
    }
    CHECK(run(b, "circuit.power doc=$c symbol=gnd pins=J1.1").ok);
    Result r = run(b, "circuit.erc doc=$c");
    CHECK(intField(r, "errors") == 0);
    r = run(b, "circuit.drc doc=$c");
    CHECK(r.message.find("no_board") != std::string::npos);
    CHECK(!run(b, "circuit.route doc=$c").ok);
    r = run(b, "circuit.pcb_place doc=$c");
    CHECK(r.ok && intField(r, "placed") == 7);
    Circuit& c = circuitOf(s, "ckt1");
    CHECK(c.board.outline.size() == 4);
    r = run(b, "circuit.ratsnest doc=$c");
    CHECK(intField(r, "unrouted") == 9);
    r = run(b, "circuit.route doc=$c preview=true");
    CHECK(r.ok && intField(r, "failed") == 0 && r.attachments.size() == 1);
    CHECK(intField(run(b, "circuit.ratsnest doc=$c"), "unrouted") == 0);
    r = run(b, "circuit.drc doc=$c");
    CHECK(intField(r, "errors") == 0);
    if (intField(r, "errors") != 0) std::printf("%s\n", r.message.c_str());
    // a pour on the ground net
    CHECK(run(b, "circuit.unroute doc=$c net=GND").ok);
    CHECK(intField(run(b, "circuit.ratsnest doc=$c"), "unrouted") > 0);
    r = run(b, "circuit.pour doc=$c net=GND layer=both");
    CHECK(r.ok && numField(r, "area") > 10);
    r = run(b, "circuit.drc doc=$c");
    CHECK(r.ok);
    // the pour joins GND pads it reaches on both layers; route whatever is left
    CHECK(run(b, "circuit.route doc=$c").ok);
    r = run(b, "circuit.drc doc=$c");
    CHECK(intField(r, "errors") == 0);
    if (intField(r, "errors") != 0) std::printf("%s\n", r.message.c_str());
    // a deliberate short
    const auto pads = worldPads(c, buildNetlist(c));
    Pt a, bb;
    for (const auto& p : pads) {
        if (p.net == "IN1") a = p.pos;
        if (p.net == "IN2") bb = p.pos;
    }
    CHECK(run(b, "circuit.trace doc=$c layer=F net=IN1 points=" + std::to_string(a.x) + "," + std::to_string(a.y) + "," + std::to_string(bb.x) + "," +
                     std::to_string(bb.y))
              .ok);
    r = run(b, "circuit.drc doc=$c");
    CHECK(r.message.find("short") != std::string::npos);
    CHECK(run(b, "undo").ok);
    // fabrication files
    r = run(b, "circuit.gerber doc=$c path=" + q(dir / "fab"));
    CHECK(r.ok && intField(r, "files") == 12);
    const std::string top = readAll(dir / "fab" / "leds-F_Cu.gtl");
    CHECK(top.find("%FSLAX46Y46*%") != std::string::npos && top.find("M02*") != std::string::npos && top.find("G36*") != std::string::npos);
    CHECK(readAll(dir / "fab" / "leds-Edge_Cuts.gm1").find("D01*") != std::string::npos);
    const std::string drl = readAll(dir / "fab" / "leds.drl");
    CHECK(drl.rfind("M48", 0) == 0 && drl.find("T1C") != std::string::npos && drl.find("M30") != std::string::npos);
    CHECK(run(b, "circuit.gerber doc=$c path=" + q(dir / "fab.zip")).ok);
    CHECK(raster::readZip(readAll(dir / "fab.zip")).size() == 12);
    CHECK(run(b, "circuit.export doc=$c view=pcb path=" + q(dir / "b.svg")).ok);
    CHECK(run(b, "circuit.export doc=$c view=pcb path=" + q(dir / "b.png")).ok);
    CHECK(fs::file_size(dir / "b.png") > 1000);
    CHECK(run(b, "circuit.footprint_move doc=$c ref=J1 rot=90 side=bottom").ok);
    CHECK(c.part("J1")->pcb.bottom && c.part("J1")->pcb.rot == 90);
    // board survives save/open
    CHECK(run(b, "circuit.save doc=$c path=" + q(dir / "leds.circuit.json")).ok);
    Circuit back = fromJson(readAll(dir / "leds.circuit.json"));
    CHECK(back.board.traces.size() == c.board.traces.size() && back.board.zones.size() == 2 && back.part("J1")->pcb.bottom);
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}").value_or("");
    };
    const std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"circuit_new", "circuit_place", "circuit_connect", "circuit_net", "circuit_power", "circuit_erc", "circuit_bom",
                             "circuit_pcb_place", "circuit_route", "circuit_pour", "circuit_drc", "circuit_gerber", "circuit_preview", "circuit_sim_op"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string r = call("circuit_new", R"({"title":"mcp"})");
    CHECK(r.find("ckt1") != std::string::npos);
    r = call("circuit_place", R"({"doc":"ckt1","symbol":"resistor","preview":true})");
    CHECK(r.find("\"isError\":false") != std::string::npos && r.find("\"type\":\"image\"") != std::string::npos);
    r = call("circuit_preview", R"({"doc":"ckt1","view":"pcb"})");
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("circuit_place", R"({"doc":"nope","symbol":"resistor"})");
    CHECK(r.find("\"isError\":true") != std::string::npos);
}

}  // namespace

int main() {
    const fs::path dir = tempDir();
    testValues();
    testLibrary();
    testSchematic(dir);
    testKicadImport(dir);
    testBoard(dir);
    testMcp();
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("circuit");
}
