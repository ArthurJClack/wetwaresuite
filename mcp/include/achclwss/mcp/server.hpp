// MCP server over stdio (newline-delimited JSON-RPC 2.0). Every command in a
// Session's registry becomes an MCP tool, plus undo, redo and history, so
// Claude can do anything the tool's UI can.
#pragma once

#include <iosfwd>
#include <optional>
#include <string>

#include "achclwss/core/command.hpp"

namespace achclwss::mcp {

class Server {
public:
    Server(Session& session, std::string name, std::string version);

    // Handles one JSON-RPC message; returns the response line, or nothing for notifications.
    std::optional<std::string> handle(const std::string& line);

    // Reads messages from `in` until EOF, writing responses to `out`.
    void run(std::istream& in, std::ostream& out);

    // "toolkit.inspect" -> "toolkit_inspect"
    static std::string toolName(const std::string& commandId);

private:
    Session& session_;
    std::string name_;
    std::string version_;
};

}  // namespace achclwss::mcp
