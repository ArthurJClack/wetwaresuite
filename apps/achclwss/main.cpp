// achclwss: every tool in one headless binary. Run single commands, batch
// scripts, or serve everything to Claude over MCP.
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "achclwss/core/cli.hpp"
#include "achclwss/mcp/server.hpp"
#include "suite.hpp"

namespace {

constexpr const char* kVersion = "0.2.0";

constexpr const char* kInstructions =
    "ACHCLWSS is a creative suite you drive with tools. Workflow: open or create a document (photo_open, photo_new, "
    "paint_new, vector_new), which returns a doc id like img1; pass that id as `doc` to edit it; call photo_preview "
    "(or pass preview=true on most edits) to see the result as an image; then save with photo_save or vector_save. "
    "Every edit is undoable with undo/redo, and `documents` lists what's open. Coordinates are pixels from the "
    "top-left. Colours accept #rrggbb, #rrggbbaa, rgb(r,g,b) or names. Strokes take points as a flat [x1,y1,x2,y2,...] "
    "list. convert_file changes file formats; convert_formats shows what's available. toolkit_inspect identifies "
    "any file.";

void usage() {
    std::cout << "achclwss " << kVersion << ": the whole suite, headless\n\n"
              << "usage:\n"
                 "  achclwss list [tool]              list commands (optionally for one tool, e.g. photo)\n"
                 "  achclwss help <command>           show a command's parameters\n"
                 "  achclwss run <command> key=value  run one command, e.g. run convert.file input=a.png output=a.jpg\n"
                 "  achclwss <command> key=value      same as run\n"
                 "  achclwss batch [file] [--keep-going]  run a script of commands (stdin if no file)\n"
                 "  achclwss mcp [--tools a,b]        serve all (or some) tools to Claude over MCP on stdin/stdout\n"
                 "  achclwss --version\n";
}

std::string toolOf(const std::string& id) { return id.substr(0, id.find('.')); }

int list(const achclwss::Session& session, const std::string& tool) {
    std::string current;
    for (const auto& info : session.registry().list()) {
        if (!tool.empty() && toolOf(info.id) != tool) continue;
        if (toolOf(info.id) != current) {
            current = toolOf(info.id);
            std::cout << (current.empty() ? "" : "\n") << current << "\n";
        }
        std::cout << "  " << info.id << "  " << info.title << (info.undoable ? "" : "  (read-only/output)") << "\n";
    }
    return 0;
}

int help(const achclwss::Session& session, const std::string& id) {
    const auto* info = session.registry().find(id);
    if (!info) {
        std::cerr << "unknown command: " << id << " (try `achclwss list`)\n";
        return 2;
    }
    std::cout << info->id << ": " << info->title << "\n" << info->description << "\n\nusage: " << achclwss::cli::usage(*info) << "\n";
    for (const auto& p : info->params) {
        std::cout << "  " << p.name << " (" << achclwss::toString(p.type) << (p.required ? ", required" : "") << ")  "
                  << p.description;
        if (!p.choices.empty()) {
            std::cout << " [";
            for (std::size_t i = 0; i < p.choices.size(); ++i) std::cout << (i ? "|" : "") << p.choices[i];
            std::cout << "]";
        }
        std::string d = achclwss::toText(p.defaultValue);
        if (!d.empty()) std::cout << " (default " << d << ")";
        std::cout << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace achclwss;
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        usage();
        return args.empty() ? 2 : 0;
    }

    Session session;
    suite::registerAll(session);
    const std::string cmd = args[0];

    try {
        if (cmd == "--version") {
            std::cout << "achclwss " << kVersion << "\n";
            return 0;
        }
        if (cmd == "list") return list(session, args.size() > 1 ? args[1] : "");
        if (cmd == "help" && args.size() == 2) return help(session, args[1]);
        if (cmd == "mcp") {
            if (args.size() == 3 && args[1] == "--tools") {
                std::set<std::string> keep;
                std::string item;
                for (char c : args[2] + ",") {
                    if (c == ',') {
                        if (!item.empty()) keep.insert(item);
                        item.clear();
                    } else {
                        item += c;
                    }
                }
                Session filtered;
                suite::registerAll(filtered, keep);
                mcp::Server server(filtered, {"achclwss", kVersion, kInstructions});
                server.run(std::cin, std::cout);
                return 0;
            }
            mcp::Server server(session, {"achclwss", kVersion, kInstructions});
            server.run(std::cin, std::cout);
            return 0;
        }
        if (cmd == "batch") {
            bool keepGoing = false;
            std::string file;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--keep-going") keepGoing = true;
                else file = args[i];
            }
            cli::Batch batch(session);
            if (file.empty() || file == "-") return batch.run(std::cin, std::cout, std::cerr, keepGoing) == 0 ? 0 : 1;
            std::ifstream in(file);
            if (!in) {
                std::cerr << "cannot open " << file << "\n";
                return 2;
            }
            return batch.run(in, std::cout, std::cerr, keepGoing) == 0 ? 0 : 1;
        }

        // run <command> ... or <command> ...
        std::size_t start = cmd == "run" ? 1 : 0;
        if (start >= args.size()) {
            usage();
            return 2;
        }
        const std::string id = args[start];
        const auto* info = session.registry().find(id);
        if (!info) {
            std::cerr << "unknown command: " << id << " (try `achclwss list`)\n";
            return 2;
        }
        Params params = cli::parseArgs(*info, {args.begin() + static_cast<std::ptrdiff_t>(start) + 1, args.end()});
        Result r = session.run(id, params);
        std::string text = cli::format(r);
        (r.ok ? std::cout : std::cerr) << text << (text.empty() ? "" : "\n");
        return r.ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
