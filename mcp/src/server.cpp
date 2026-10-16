#include <cmath>
#include "wetwaresuite/mcp/server.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <istream>
#include <ostream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/core/cli.hpp"

namespace wetwaresuite::mcp {

using nlohmann::json;

namespace {

constexpr std::array kProtocolVersions = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
constexpr const char* kDocScheme = "wetwaresuite://doc/";

json rpcResult(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpcError(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json textResult(const std::string& text, bool isError) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", isError}};
}

json toJson(const Value& value) {
    if (auto s = std::get_if<std::string>(&value)) return *s;
    if (auto b = std::get_if<bool>(&value)) return *b;
    if (auto i = std::get_if<std::int64_t>(&value)) return *i;
    if (auto d = std::get_if<double>(&value)) return *d;
    if (auto l = std::get_if<NumberList>(&value)) return *l;
    return nullptr;
}

json toolResult(const Result& result) {
    json content = json::array();
    std::string text = cli::format(result);
    content.push_back({{"type", "text"}, {"text", text.empty() ? (result.ok ? "done" : "failed") : text}});
    for (const auto& a : result.attachments) {
        if (a.mime.rfind("image/", 0) == 0) {
            content.push_back({{"type", "image"}, {"data", base64(a.bytes)}, {"mimeType", a.mime}});
        } else {
            content.push_back({{"type", "resource"},
                               {"resource", {{"uri", "wetwaresuite://attachment/" + a.name},
                                             {"mimeType", a.mime},
                                             {"blob", base64(a.bytes)}}}});
        }
    }
    json out = {{"content", content}, {"isError", !result.ok}};
    if (!result.data.empty()) {
        json structured = json::object();
        for (const auto& [key, v] : result.data) structured[key] = toJson(v);
        out["structuredContent"] = structured;
    }
    return out;
}

json schemaFor(const ParamSpec& p) {
    json s = {{"type", toString(p.type)}, {"description", p.description}};
    if (p.type == ParamType::NumberList) s["items"] = {{"type", "number"}};
    if (!p.choices.empty()) s["enum"] = p.choices;
    if (!std::holds_alternative<std::monostate>(p.defaultValue)) s["default"] = toJson(p.defaultValue);
    return s;
}

json describe(const CommandInfo& info) {
    json properties = json::object();
    json required = json::array();
    for (const auto& p : info.params) {
        properties[p.name] = schemaFor(p);
        if (p.required) required.push_back(p.name);
    }
    return {{"name", Server::toolName(info.id)},
            {"title", info.title},
            {"description", info.description},
            {"inputSchema", {{"type", "object"}, {"properties", properties}, {"required", required}}},
            {"annotations", {{"title", info.title}, {"readOnlyHint", info.readOnly},
                             {"destructiveHint", !info.readOnly && !info.undoable}, {"openWorldHint", false}}}};
}

json noArgsTool(const char* name, const char* description, bool readOnly) {
    return {{"name", name},
            {"description", description},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
            {"annotations", {{"readOnlyHint", readOnly}, {"openWorldHint", false}}}};
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
        else if (v.is_number()) {
            // 300.0 from a client means 300; whole numbers satisfy integer params
            double d = v.get<double>();
            if (std::floor(d) == d && std::abs(d) < 9e15) out[key] = static_cast<std::int64_t>(d);
            else out[key] = d;
        }
        else if (v.is_string()) out[key] = v.get<std::string>();
        else if (v.is_array()) {
            NumberList list;
            for (const auto& item : v) {
                if (!item.is_number()) {
                    error = "argument '" + key + "' must be an array of numbers";
                    return false;
                }
                list.push_back(item.get<double>());
            }
            out[key] = std::move(list);
        } else if (v.is_null()) {
            continue;
        } else {
            error = "argument '" + key + "' must be a boolean, number, string or array of numbers";
            return false;
        }
    }
    return true;
}

}  // namespace

std::string base64(const std::string& bytes) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        unsigned n = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                     static_cast<unsigned char>(bytes[i + 2]);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (i < bytes.size()) {
        unsigned n = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

Server::Server(Session& session, ServerInfo info) : session_(session), info_(std::move(info)) {}

Server::Server(Session& session, std::string name, std::string version)
    : Server(session, ServerInfo{std::move(name), std::move(version), {}}) {}

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
        json result = {{"protocolVersion", version},
                       {"capabilities",
                        {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}}}}},
                       {"serverInfo", {{"name", info_.name}, {"version", info_.version}}}};
        if (!info_.instructions.empty()) result["instructions"] = info_.instructions;
        return rpcResult(id, result).dump();
    }
    if (method == "ping") return rpcResult(id, json::object()).dump();

    if (method == "tools/list") {
        json tools = json::array();
        for (const auto& info : session_.registry().list()) tools.push_back(describe(info));
        tools.push_back(noArgsTool("undo", "Undo the last change to any open document", false));
        tools.push_back(noArgsTool("redo", "Redo the last undone change", false));
        tools.push_back(noArgsTool("history", "List the undo history, oldest first", true));
        tools.push_back(noArgsTool("documents", "List open documents with their ids, kinds and sizes", true));
        return rpcResult(id, {{"tools", tools}}).dump();
    }

    if (method == "tools/call") {
        const std::string tool = params.value("name", "");
        if (tool == "undo") {
            bool done = session_.history().undo();
            return rpcResult(id, textResult(done ? "undone" : "nothing to undo", !done)).dump();
        }
        if (tool == "redo") {
            bool done = session_.history().redo();
            return rpcResult(id, textResult(done ? "redone" : "nothing to redo", !done)).dump();
        }
        if (tool == "history") {
            std::string text;
            for (const auto& label : session_.history().labels()) text += label + "\n";
            return rpcResult(id, textResult(text.empty() ? "history is empty" : text, false)).dump();
        }
        if (tool == "documents") {
            std::string text;
            json docs = json::array();
            for (const auto& docId : session_.workspace().ids()) {
                auto doc = session_.workspace().get(docId);
                text += docId + " (" + doc->kind() + "): " + doc->summary() + "\n";
                docs.push_back({{"id", docId}, {"kind", doc->kind()}, {"summary", doc->summary()}, {"path", doc->path}});
            }
            json out = textResult(text.empty() ? "no open documents" : text, false);
            out["structuredContent"] = {{"documents", docs}};
            return rpcResult(id, out).dump();
        }

        std::string commandId;
        for (const auto& info : session_.registry().list()) {
            if (toolName(info.id) == tool) commandId = info.id;
        }
        if (commandId.empty()) return rpcError(id, -32602, "unknown tool: " + tool).dump();

        Params args;
        std::string error;
        if (!toParams(params.value("arguments", json::object()), args, error)) {
            return rpcResult(id, textResult(error, true)).dump();
        }
        // a number sent for a string param (e.g. a cad expression field) means its text
        if (const auto* info = session_.registry().find(commandId)) {
            for (const auto& spec : info->params) {
                auto it = args.find(spec.name);
                if (spec.type == ParamType::String && it != args.end() &&
                    (std::holds_alternative<std::int64_t>(it->second) || std::holds_alternative<double>(it->second))) {
                    std::string text = toText(it->second);
                    if (auto d = std::get_if<double>(&it->second)) {
                        char buf[40];
                        std::snprintf(buf, sizeof buf, "%.15g", *d);
                        if (std::strtod(buf, nullptr) != *d) std::snprintf(buf, sizeof buf, "%.17g", *d);
                        text = buf;
                    }
                    it->second = text;
                }
            }
        }
        Result result;
        try {
            result = session_.run(commandId, args);
        } catch (const std::exception& e) {
            result = Result::failure(std::string("internal error: ") + e.what());
        }
        return rpcResult(id, toolResult(result)).dump();
    }

    if (method == "resources/list") {
        json resources = json::array();
        for (const auto& docId : session_.workspace().ids()) {
            auto doc = session_.workspace().get(docId);
            resources.push_back({{"uri", kDocScheme + docId},
                                 {"name", docId},
                                 {"description", doc->kind() + ": " + doc->summary()},
                                 {"mimeType", "text/plain"}});
        }
        return rpcResult(id, {{"resources", resources}}).dump();
    }
    if (method == "resources/templates/list") {
        return rpcResult(id, {{"resourceTemplates",
                               json::array({{{"uriTemplate", std::string(kDocScheme) + "{id}"},
                                             {"name", "open document"},
                                             {"description", "summary and preview of an open document"}}})}})
            .dump();
    }
    if (method == "resources/read") {
        const std::string uri = params.value("uri", "");
        const std::string prefix = kDocScheme;
        auto doc = uri.rfind(prefix, 0) == 0 ? session_.workspace().get(uri.substr(prefix.size())) : nullptr;
        if (!doc) return rpcError(id, -32002, "resource not found: " + uri).dump();
        json contents = json::array();
        contents.push_back({{"uri", uri}, {"mimeType", "text/plain"}, {"text", doc->kind() + ": " + doc->summary()}});
        if (auto preview = doc->preview()) {
            contents.push_back({{"uri", uri + "/preview"}, {"mimeType", preview->mime}, {"blob", base64(preview->bytes)}});
        }
        return rpcResult(id, {{"contents", contents}}).dump();
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

}  // namespace wetwaresuite::mcp
