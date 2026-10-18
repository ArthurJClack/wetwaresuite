// MCP server over stdio (newline-delimited JSON-RPC 2.0). Every command in a
// Session's registry becomes an MCP tool, plus undo, redo, history and
// documents. Open documents are also MCP resources with live previews, so
// Claude can do anything a tool's UI can and look at the result.
#pragma once

#include <iosfwd>
#include <optional>
#include <string>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::mcp {

struct ServerInfo {
    std::string name;
    std::string version;
    std::string instructions;  // sent to the client on initialize: how to use these tools
};

class Server {
public:
    Server(Session& session, ServerInfo info);
    Server(Session& session, std::string name, std::string version);

    // Handles one JSON-RPC message; returns the response line, or nothing for notifications.
    std::optional<std::string> handle(const std::string& line);

    // Reads messages from `in` until EOF, writing responses to `out`.
    void run(std::istream& in, std::ostream& out);

    // "photo.resize" -> "photo_resize"
    static std::string toolName(const std::string& commandId);

private:
    Session& session_;
    ServerInfo info_;
};

std::string base64(const std::string& bytes);

}  // namespace wetwaresuite::mcp
