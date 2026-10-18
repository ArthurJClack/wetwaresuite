// MCP server over stdio (newline-delimited JSON-RPC 2.0). Every command in a
// Session's registry becomes an MCP tool, plus undo, redo, history and
// documents. Open documents are also MCP resources with live previews, so
// Claude can do anything a tool's UI can and look at the result.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>

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
    const std::string& toolsJson();                           // cached until the registry changes
    std::string commandFor(const std::string& tool);          // MCP tool name -> command id ("" = none)
    Session& session_;
    ServerInfo info_;
    std::string tools_;
    std::uint64_t toolsRevision_ = ~std::uint64_t{0};
    std::unordered_map<std::string, std::string> commands_;  // tool name -> command id
    std::uint64_t commandsRevision_ = ~std::uint64_t{0};
};

// The "tools" array of a tools/list answer (every command plus undo, redo, history, documents)
// as compact JSON. toolsListJson writes the text directly (fast: the suite has ~700 tools);
// toolsListJsonReference builds it with nlohmann::json. Both give the same bytes.
std::string toolsListJson(const CommandRegistry& registry);
std::string toolsListJsonReference(const CommandRegistry& registry);

std::string base64(const std::string& bytes);

}  // namespace wetwaresuite::mcp
