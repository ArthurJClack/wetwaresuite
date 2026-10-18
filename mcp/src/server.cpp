#include "achclwss/mcp/server.hpp"

#include <array>
#include <istream>
#include <ostream>

#include <nlohmann/json.hpp>

namespace achclwss::mcp {

using nlohmann::json;

namespace {

constexpr std::array kProtocolVersions = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

json rpcResult(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpcError(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json toolResult(const std::string& text, bool isError) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", isError}};
}

std::string valueText(const Value& value) {
    if (auto s = std::get_if<std::string>(&value)) return *s;
    if (auto b = std::get_if<bool>(&value)) return *b ? "true" : "false";
    if (auto i = std::get_if<std::int64_t>(&value)) return std::to_string(*i);
    if (auto d = std::get_if<double>(&value)) return std::to_string(*d);
    return {};
}

json describe(const CommandInfo& info) {
    json properties = json::object();
    json required = json::array();
    for (const auto& p : info.params) {
        properties[p.name] = {{"type", toString(p.type)}, {"description", p.description}};
        if (p.required) required.push_back(p.name);
    }
    return {{"name", Server::toolName(info.id)},
            {"title", info.title},
            {"description", info.description},
            {"inputSchema", {{"type", "object"}, {"properties", properties}, {"required", required}}},
            {"annotations", {{"readOnlyHint", !info.undoable}}}};
}

bool toParams(const json& args, Params& out, std::string& error) {
    if (args.is_null()) return true;
    if (!args.is_object()) {
        error = "arguments must be an object";
        return false;
    }
    for (const auto& [key, v] : args.items()) {
        if (v.is_boolean()) out[key] = v.get<bool>();
        else if (v.is_number_integer()) out[key] = v.get<std::int64_t>();
        else if (v.is_number()) out[key] = v.get<double>();
        else if (v.is_string()) out[key] = v.get<std::string>();
        else {
            error = "argument '" + key + "' must be a boolean, number or string";
            return false;
        }
    }
    return true;
}

}  // namespace

Server::Server(Session& session, std::string name, std::string version)
    : session_(session), name_(std::move(name)), version_(std::move(version)) {}

std::string Server::toolName(const std::string& commandId) {
    std::string out = commandId;
    for (auto& c : out) {
        if (c == '.') c = '_';
    }
    return out;
}

std::optional<std::string> Server::handle(const std::string& line) {
    json msg = json::parse(line, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) return rpcError(nullptr, -32700, "parse error").dump();

    const bool isRequest = msg.contains("id");
    const json id = isRequest ? msg["id"] : json(nullptr);
    if (!msg.contains("method") || !msg["method"].is_string()) {
        return isRequest ? std::optional(rpcError(id, -32600, "invalid request").dump()) : std::nullopt;
    }
    const std::string method = msg["method"];
    const json params = msg.value("params", json::object());

    if (!isRequest) return std::nullopt;  // notifications/initialized and friends need no answer

    if (method == "initialize") {
        std::string requested = params.value("protocolVersion", "");
        std::string version = kProtocolVersions.front();
        for (const char* v : kProtocolVersions) {
            if (requested == v) version = v;
        }
        return rpcResult(id, {{"protocolVersion", version},
                              {"capabilities", {{"tools", {{"listChanged", false}}}}},
                              {"serverInfo", {{"name", name_}, {"version", version_}}}})
            .dump();
    }
    if (method == "ping") return rpcResult(id, json::object()).dump();

    if (method == "tools/list") {
        json tools = json::array();
        for (const auto& info : session_.registry().list()) tools.push_back(describe(info));
        auto noArgs = [](const char* name, const char* description, bool readOnly) {
            return json{{"name", name},
                        {"description", description},
                        {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
                        {"annotations", {{"readOnlyHint", readOnly}}}};
        };
        tools.push_back(noArgs("undo", "Undo the last change", false));
        tools.push_back(noArgs("redo", "Redo the last undone change", false));
        tools.push_back(noArgs("history", "List the undo history, oldest first", true));
        return rpcResult(id, {{"tools", tools}}).dump();
    }

    if (method == "tools/call") {
        const std::string tool = params.value("name", "");
        if (tool == "undo") {
            bool done = session_.history().undo();
            return rpcResult(id, toolResult(done ? "undone" : "nothing to undo", !done)).dump();
        }
        if (tool == "redo") {
            bool done = session_.history().redo();
            return rpcResult(id, toolResult(done ? "redone" : "nothing to redo", !done)).dump();
        }
        if (tool == "history") {
            std::string text;
            for (const auto& label : session_.history().labels()) text += label + "\n";
            return rpcResult(id, toolResult(text.empty() ? "history is empty" : text, false)).dump();
        }

        std::string commandId;
        for (const auto& info : session_.registry().list()) {
            if (toolName(info.id) == tool) commandId = info.id;
        }
        if (commandId.empty()) return rpcError(id, -32602, "unknown tool: " + tool).dump();

        Params args;
        std::string error;
        if (!toParams(params.value("arguments", json::object()), args, error)) {
            return rpcResult(id, toolResult(error, true)).dump();
        }
        Result result = session_.run(commandId, args);
        std::string text = result.message;
        std::string value = valueText(result.value);
        if (!value.empty()) text += text.empty() ? value : "\n" + value;
        return rpcResult(id, toolResult(text, !result.ok)).dump();
    }

    return rpcError(id, -32601, "method not found: " + method).dump();
}

void Server::run(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        if (auto response = handle(line)) out << *response << '\n' << std::flush;
    }
}

}  // namespace achclwss::mcp
