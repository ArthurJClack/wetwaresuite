#include <nlohmann/json.hpp>

#include "achclwss/mcp/server.hpp"
#include "check.hpp"

using namespace achclwss;
using nlohmann::json;

namespace {

class SetAction : public Action {
public:
    SetAction(std::string& target, std::string value) : target_(target), value_(std::move(value)) {}
    Result apply() override {
        previous_ = target_;
        target_ = value_;
        return Result::success("set to " + value_);
    }
    void revert() override { target_ = previous_; }

private:
    std::string& target_;
    std::string value_;
    std::string previous_;
};

json call(mcp::Server& server, const json& request) {
    auto response = server.handle(request.dump());
    return response ? json::parse(*response) : json();
}

}  // namespace

int main() {
    std::string title = "untitled";
    Session session;
    session.registry().add(
        {"doc.set_title", "Set title", "renames the document", {{"title", ParamType::String, "new title", true}}, true},
        [&title](const Params& p) { return std::make_unique<SetAction>(title, std::get<std::string>(p.at("title"))); });

    mcp::Server server(session, "test", "0.0.0");

    json init = call(server, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                              {"params", {{"protocolVersion", "2025-06-18"}}}});
    CHECK(init["result"]["protocolVersion"] == "2025-06-18");
    CHECK(init["result"]["serverInfo"]["name"] == "test");

    CHECK(!server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").has_value());

    json list = call(server, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    const auto& tools = list["result"]["tools"];
    CHECK(tools.size() == 5);
    CHECK(tools[0]["name"] == "doc_set_title");
    CHECK(tools[0]["inputSchema"]["required"][0] == "title");

    json set = call(server, {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                             {"params", {{"name", "doc_set_title"}, {"arguments", {{"title", "plan"}}}}}});
    CHECK(set["result"]["isError"] == false);
    CHECK(title == "plan");

    json undo = call(server, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", {{"name", "undo"}}}});
    CHECK(undo["result"]["isError"] == false);
    CHECK(title == "untitled");

    json bad = call(server, {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                             {"params", {{"name", "doc_set_title"}, {"arguments", {{"title", 3}}}}}});
    CHECK(bad["result"]["isError"] == true);

    json unknown = call(server, {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "nope"}});
    CHECK(unknown["error"]["code"] == -32601);

    json parse = json::parse(*server.handle("{not json"));
    CHECK(parse["error"]["code"] == -32700);

    return check::summary("mcp");
}
