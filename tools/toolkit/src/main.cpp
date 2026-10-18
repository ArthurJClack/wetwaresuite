// achclwss-toolkit: the "open anything" tool. CLI today, Qt UI on top later.
#include <iostream>
#include <string>

#include "achclwss/mcp/server.hpp"
#include "achclwss/toolkit/detect.hpp"

namespace {

constexpr const char* kVersion = "0.1.0";

void usage() {
    std::cout << "usage: achclwss-toolkit <command> [args]\n\n"
                 "  inspect <file>                  say what a file is and which tool opens it\n"
                 "  hex <file> [offset] [length]    hex dump (default 256 bytes)\n"
                 "  mcp                             run as an MCP server on stdin/stdout\n"
                 "  --version\n";
}

int report(const achclwss::Result& result) {
    (result.ok ? std::cout : std::cerr) << result.message << (result.message.ends_with('\n') ? "" : "\n");
    return result.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace achclwss;

    Session session;
    toolkit::registerCommands(session.registry());

    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];

    try {
        if (command == "--version") {
            std::cout << "achclwss-toolkit " << kVersion << "\n";
            return 0;
        }
        if (command == "inspect" && argc == 3) {
            return report(session.run("toolkit.inspect", {{"path", std::string(argv[2])}}));
        }
        if (command == "hex" && argc >= 3 && argc <= 5) {
            Params p{{"path", std::string(argv[2])}};
            if (argc >= 4) p["offset"] = static_cast<std::int64_t>(std::stoll(argv[3], nullptr, 0));
            if (argc >= 5) p["length"] = static_cast<std::int64_t>(std::stoll(argv[4], nullptr, 0));
            return report(session.run("toolkit.hex", p));
        }
        if (command == "mcp" && argc == 2) {
            mcp::Server server(session, "achclwss-toolkit", kVersion);
            server.run(std::cin, std::cout);
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    usage();
    return 2;
}
