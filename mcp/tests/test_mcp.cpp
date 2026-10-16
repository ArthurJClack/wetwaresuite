#include <nlohmann/json.hpp>

#include "wetwaresuite/mcp/server.hpp"
#include "check.hpp"

using namespace wetwaresuite;
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
                             {"params", {{"name", "doc_set_title"}, {"arguments", {{"title", true}}}}}});
    CHECK(bad["result"]["isError"] == true);

    json unknown = call(server, {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "nope"}});
    CHECK(unknown["error"]["code"] == -32601);

    json parse = json::parse(*server.handle("{not json"));
    CHECK(parse["error"]["code"] == -32700);

    // the fast tools/list text is byte for byte what nlohmann builds, edge cases included
    {
        Session odd;
        CommandInfo info{"odd.case", "Quote \" back\\slash \t tab", std::string("ctrl \x01\x1f del \x7f caf\xC3\xA9 \xE2\x9C\x93 / end"),
                         {{"zeta", ParamType::Number, "a number", false, {}, 0.1},
                          {"alpha", ParamType::NumberList, "list\nline", true, {}, NumberList{1, 2.5, -3e-7}},
                          {"mid", ParamType::String, "choices", false, {"a\"b", "\xC3\xA9t\xC3\xA9"}, std::string("a\"b")},
                          {"flag", ParamType::Bool, "", false, {}, true},
                          {"count", ParamType::Int, "count", true, {}, std::int64_t{-42}},
                          {"count", ParamType::Int, "repeated name", false},
                          {"big", ParamType::Number, "big", false, {}, 1e300}},
                         false, true};
        odd.registry().add(info, [](const Params&) { return std::make_unique<FunctionAction>([] { return Result::success("ok"); }); });
        odd.registry().add({"odd.empty", "", "", {}, true}, [](const Params&) { return std::make_unique<FunctionAction>([] { return Result::success("ok"); }); });
        CHECK(mcp::toolsListJson(odd.registry()) == mcp::toolsListJsonReference(odd.registry()));
        CHECK(mcp::toolsListJson(session.registry()) == mcp::toolsListJsonReference(session.registry()));
        mcp::Server oddServer(odd, "odd", "1");
        const json expected = {{"jsonrpc", "2.0"}, {"id", "seven"}, {"result", {{"tools", json::parse(mcp::toolsListJsonReference(odd.registry()))}}}};
        const std::string req = json{{"jsonrpc", "2.0"}, {"id", "seven"}, {"method", "tools/list"}}.dump();
        CHECK(*oddServer.handle(req) == expected.dump());
        CHECK(*oddServer.handle(req) == expected.dump());  // cached
        // a command added later shows up (the cache follows the registry)
        odd.registry().add({"odd.later", "Later", "added after the first list", {}, true},
                           [](const Params&) { return std::make_unique<FunctionAction>([] { return Result::success("later"); }); });
        CHECK(json::parse(*oddServer.handle(req))["result"]["tools"].size() == 7);
        json later = call(oddServer, {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "tools/call"}, {"params", {{"name", "odd_later"}}}});
        CHECK(later["result"]["content"][0]["text"] == "later");
        json missing = call(oddServer, {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"}, {"params", {{"name", "odd_nope"}}}});
        CHECK(missing["error"]["code"] == -32602);
    }

    return check::summary("mcp");
}
