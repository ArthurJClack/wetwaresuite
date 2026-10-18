// circuit.* file, library and output commands.
#include "cmd_common.hpp"

namespace wetwaresuite::circuit::cmd {

namespace {

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

Result created(Session& session, std::shared_ptr<CircuitDoc> doc, const std::string& verb, const Params& p) {
    const std::string id = session.workspace().add(doc, "ckt");
    Result r = Result::success(verb + " " + id + ": " + doc->summary(), id);
    r.with("doc", id).with("parts", static_cast<std::int64_t>(doc->circuit.parts.size()));
    return maybePreview(r, *doc, p, View::Schematic);
}

SheetStyle sheetStyle(const Params& p) {
    SheetStyle s;
    s.grid = boolParam(p, "grid", false);
    s.highlightNet = stringParam(p, "highlight");
    s.pinNumbers = boolParam(p, "pin_numbers", true);
    return s;
}

PcbStyle pcbStyle(const Params& p) {
    PcbStyle s;
    const std::string layers = lower(stringParam(p, "layers", "both"));
    s.top = layers != "bottom" && layers != "b";
    s.bottom = layers != "top" && layers != "f";
    s.mirror = boolParam(p, "from_below", false);
    s.ratsnest = boolParam(p, "ratsnest", true);
    s.labels = boolParam(p, "net_names", false);
    s.highlightNet = stringParam(p, "highlight");
    return s;
}

std::vector<ParamSpec> viewParams() {
    return {{"view", ParamType::String, "what to draw", false, {"schematic", "pcb"}, std::string("schematic")},
            {"grid", ParamType::Bool, "schematic: draw the 0.1 in grid", false, {}, false},
            {"highlight", ParamType::String, "net name to draw in a highlight colour", false},
            {"layers", ParamType::String, "pcb: top, bottom or both", false, {"both", "top", "bottom"}, std::string("both")},
            {"from_below", ParamType::Bool, "pcb: mirror to look from the bottom side", false, {}, false},
            {"ratsnest", ParamType::Bool, "pcb: draw unrouted connections", false, {}, true},
            {"net_names", ParamType::Bool, "pcb: write net names on pads", false, {}, false}};
}

}  // namespace

void registerFileCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.new", "New Circuit",
             "Create an empty circuit (schematic + board) and return its id, e.g. ckt1. Then place parts (circuit.place), join pins "
             "(circuit.connect, circuit.net, circuit.power), check with circuit.erc, and lay out the board (circuit.board, "
             "circuit.pcb_place, circuit.route, circuit.drc, circuit.gerber).",
             {{"title", ParamType::String, "title shown in the title block", false}, previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto doc = std::make_shared<CircuitDoc>();
                    doc->circuit.title = stringParam(p, "title");
                    return created(session, doc, "created", p);
                });
            });

    reg.add({"circuit.open", "Open Circuit",
             "Open a .circuit.json file, or import a KiCad netlist (.net): its parts are mapped to library symbols (R, C, LED, "
             "Conn_01xN ...; anything else becomes a generic IC), laid out on a grid and joined with net labels.",
             {{"path", ParamType::String, "file to open (.circuit.json, .json or .net)", true}, previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        const fs::path path = stringParam(p, "path");
                        const std::string text = readFile(path);
                        auto doc = std::make_shared<CircuitDoc>();
                        std::vector<std::string> warnings;
                        const std::string ext = lower(path.extension().string());
                        if (ext == ".net" || text.find("(export") != std::string::npos) {
                            doc->circuit = fromKicadNetlist(text, &warnings);
                        } else {
                            doc->circuit = fromJson(text);
                        }
                        doc->path = path.string();
                        Result r = created(session, doc, "opened " + path.string() + " as", p);
                        for (const auto& w : warnings) r.message += "\nwarning: " + w;
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"circuit.save", "Save Circuit", "Save the circuit (schematic and board) as .circuit.json; reopen it with circuit.open.",
             {docParam(), {"path", ParamType::String, "output file, e.g. blinky.circuit.json", true}}, false},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    std::string error;
                    if (!writeFile(path, toJson(doc.circuit), error)) return Result::failure(error);
                    doc.path = path.string();
                    return edited("saved " + path.string(), p).with("path", path.string());
                },
                View::Schematic, false));

    reg.add({"circuit.close", "Close Circuit", "Close an open circuit (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<CloseAction>(session, stringParam(p, "doc")); });

    reg.add({"circuit.info", "Circuit Info", "Describe a circuit: parts, nets, wires, board size, traces, vias and pours.", {docParam(), previewParam()},
             false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Circuit& c = doc.circuit;
                    const Netlist nl = buildNetlist(c);
                    Result r = edited(doc.summary(), p);
                    if (!c.title.empty()) r.message = c.title + ": " + r.message;
                    int placed = 0, withFp = 0;
                    for (const auto& part : c.parts) {
                        if (!footprintOf(part)) continue;
                        ++withFp;
                        if (part.pcb.placed) ++placed;
                    }
                    if (withFp) r.message += "\nfootprints on the board: " + std::to_string(placed) + " of " + std::to_string(withFp);
                    for (const auto& z : c.board.zones) r.message += "\npour: " + z.net + " on " + (z.layer ? "B.Cu" : "F.Cu");
                    r.with("parts", static_cast<std::int64_t>(c.parts.size()))
                        .with("nets", static_cast<std::int64_t>(nl.nets.size()))
                        .with("wires", static_cast<std::int64_t>(c.wires.size()))
                        .with("labels", static_cast<std::int64_t>(c.labels.size()))
                        .with("board_width", boardWidth(c.board))
                        .with("board_height", boardHeight(c.board))
                        .with("traces", static_cast<std::int64_t>(c.board.traces.size()))
                        .with("vias", static_cast<std::int64_t>(c.board.vias.size()));
                    return r;
                },
                View::Schematic, false));

    reg.add({"circuit.symbols", "List Symbols",
             "The built-in symbol library: name, category, what the pins are and the default footprint. Use the name (or an alias "
             "such as R, C, LED, npn, uno) with circuit.place.",
             {{"category", ParamType::String, "only this category", false,
               {"passive", "semiconductor", "ic", "switch", "connector", "module", "power", "source"}}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    const std::string cat = stringParam(p, "category");
                    std::string s;
                    std::int64_t n = 0;
                    for (const auto& e : symbolCatalog()) {
                        if (!cat.empty() && e.category != cat) continue;
                        s += "\n" + e.name + " [" + e.category + "]: " + e.description + (e.aliases.empty() ? "" : " (aliases: " + e.aliases + ")");
                        ++n;
                    }
                    Result r = Result::success(std::to_string(n) + " symbols:" + s);
                    r.with("count", n);
                    return r;
                });
            });

    reg.add({"circuit.footprints", "List Footprints",
             "The built-in footprints (SMD 0603/0805/1206, SOT-23, SOIC-N, through-hole axial, radial, DIP-N, TO-92, TO-220, pin "
             "headers, switches, dev boards). Names with N take a pin count, e.g. SOIC-8, DIP-28, PinHeader_1x06. KiCad library "
             "names such as Resistor_SMD:R_0805_2012Metric are understood too.",
             {}, false, true},
            [](const Params&) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([] {
                    std::string s;
                    for (const auto& e : footprintCatalog()) s += "\n" + e.name + ": " + e.description;
                    return Result::success(std::to_string(footprintCatalog().size()) + " footprint families:" + s);
                });
            });

    reg.add({"circuit.symbol_info", "Symbol Info",
             "Pins of a library symbol (number, name, type, position relative to the part origin in grid units) and its default "
             "footprint, with an optional picture.",
             {{"symbol", ParamType::String, "symbol name or alias", true},
              {"pins", ParamType::Int, "pin count for ic, header and header2", false},
              {"pin_names", ParamType::String, "ic: comma-separated pin names in pin order", false},
              previewParam()},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    std::string error;
                    const auto names = has(p, "pin_names") ? splitList(stringParam(p, "pin_names")) : std::vector<std::string>{};
                    auto sym = findSymbol(stringParam(p, "symbol"), static_cast<int>(intParam(p, "pins", 0)), names, &error);
                    if (!sym) return Result::failure(error);
                    std::string s = sym->name + ": " + sym->title + "\nprefix " + sym->prefix + ", default value " + sym->value +
                                    ", footprint " + (sym->footprint.empty() ? "none (simulation only)" : sym->footprint) + "\npins:";
                    for (const auto& pin : sym->pins) {
                        s += "\n  " + pin.number + (pin.name != pin.number ? " " + pin.name : "") + " (" + toString(pin.type) + ") at " + pt(pin.pos);
                    }
                    Result r = Result::success(s);
                    r.with("pins", static_cast<std::int64_t>(sym->pins.size())).with("footprint", sym->footprint);
                    if (boolParam(p, "preview", false)) {
                        Circuit c;
                        Part part;
                        part.ref = sym->prefix + "1";
                        part.symbol = sym->name;
                        part.value = sym->value;
                        part.pins = static_cast<int>(intParam(p, "pins", 0));
                        part.pinNames = names;
                        if (sym->name == "power") part.symbol = stringParam(p, "symbol");
                        c.parts.push_back(part);
                        SheetStyle st;
                        st.titleBlock = false;
                        st.grid = true;
                        r.attach(schematicPng(c, st, 600));
                    }
                    return r;
                });
            });

    reg.add({"circuit.preview", "Preview Circuit",
             "Render the schematic or the board to a PNG without changing anything. view=pcb draws copper (top red, bottom blue), "
             "pads, vias, silkscreen, pours and the ratsnest.",
             [] {
                 std::vector<ParamSpec> v = {docParam()};
                 for (auto& s : viewParams()) v.push_back(s);
                 v.push_back({"size", ParamType::Int, "longest side in pixels", false, {}, std::int64_t{1400}});
                 return v;
             }(),
             false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const double size = static_cast<double>(std::clamp<std::int64_t>(intParam(p, "size", 1400), 200, 6000));
                    Result r = edited("preview of " + stringParam(p, "doc"), p);
                    if (stringParam(p, "view", "schematic") == "pcb") r.attach(pcbPng(doc.circuit, pcbStyle(p), size));
                    else r.attach(schematicPng(doc.circuit, sheetStyle(p), size));
                    return r;
                },
                View::Schematic, false));

    reg.add({"circuit.export", "Export Drawing",
             "Write the schematic or the board as a picture: .svg, .pdf (schematic only) or .png; the extension picks the "
             "format. For fabrication files use circuit.gerber.",
             [] {
                 std::vector<ParamSpec> v = {docParam(), {"path", ParamType::String, "output file (.svg, .pdf, .png)", true}};
                 for (auto& s : viewParams()) v.push_back(s);
                 v.push_back({"scale", ParamType::Number, "png: pixels per grid unit (schematic) or per mm (pcb)", false});
                 return v;
             }(),
             false},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    const std::string ext = lower(path.extension().string());
                    const bool pcb = stringParam(p, "view", "schematic") == "pcb";
                    std::string data;
                    if (pcb) {
                        const Netlist nl = buildNetlist(doc.circuit);
                        if (ext == ".svg") data = pcbSvg(doc.circuit, nl, pcbStyle(p));
                        else if (ext == ".png") data = raster::encodePng(renderPcb(doc.circuit, nl, has(p, "scale") ? numberParam(p, "scale") : pcbScale(doc.circuit, 2000), pcbStyle(p)));
                        else return Result::failure("the board exports as .svg or .png (Gerber via circuit.gerber)");
                    } else {
                        const pdf::Page page = schematicPage(doc.circuit, sheetStyle(p));
                        if (ext == ".svg") data = pageSvg(page);
                        else if (ext == ".pdf") {
                            pdf::DocInfo info;
                            info.title = doc.circuit.title;
                            data = pdf::writePdf({page}, info);
                        } else if (ext == ".png") {
                            const double scale = has(p, "scale") ? numberParam(p, "scale") / 7.2 : std::clamp(2400.0 / std::max(page.width, page.height), 1.0, 4.0);
                            data = raster::encodePng(pdf::renderPage(page, scale));
                        } else {
                            return Result::failure("the schematic exports as .svg, .pdf or .png");
                        }
                    }
                    std::string error;
                    if (!writeFile(path, data, error)) return Result::failure(error);
                    return edited("wrote " + path.string() + " (" + std::to_string(data.size()) + " bytes)", p).with("path", path.string());
                },
                View::Schematic, false));
}

}  // namespace wetwaresuite::circuit::cmd
