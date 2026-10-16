// The Streamable HTTP transport, end to end over loopback: a real server on an
// ephemeral port in a thread, a real socket client.
#include <thread>

#include <nlohmann/json.hpp>

#include "wetwaresuite/mcp/transport.hpp"
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

struct Running {
    mcp::HttpServer http;
    std::thread thread;
    Running(mcp::Server& server, mcp::HttpOptions options) : http(server, std::move(options)) {
        std::string error;
        CHECK(http.listen(error));
        CHECK(error.empty());
        CHECK(http.port() > 0);
        thread = std::thread([this] { http.serve(); });
    }
    ~Running() {
        http.stop();
        thread.join();
    }
};

mcp::HttpResponse post(int port, const std::string& body, std::map<std::string, std::string> headers = {}) {
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Accept", "application/json, text/event-stream");
    mcp::HttpResponse r;
    std::string error;
    CHECK(mcp::httpRequest("127.0.0.1", port, "POST", "/mcp", headers, body, r, error));
    return r;
}

json rpc(int id, const std::string& method, json params = json::object()) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

}  // namespace

int main() {
    std::string title = "untitled";
    Session session;
    session.registry().add(
        {"doc.set_title", "Set title", "renames the document", {{"title", ParamType::String, "new title", true}}, true},
        [&title](const Params& p) { return std::make_unique<SetAction>(title, std::get<std::string>(p.at("title"))); });
    mcp::Server server(session, {"test", "0.0.0", "use doc_set_title"});

    // argument parsing
    {
        mcp::TransportOptions o;
        std::string error;
        CHECK(mcp::parseTransportArgs({}, o, error) && !o.http);
        mcp::TransportOptions h;
        CHECK(mcp::parseTransportArgs({"--http", "--port", "0"}, h, error));
        CHECK(h.http && h.httpOptions.port == 0 && h.httpOptions.host == "127.0.0.1");
        mcp::TransportOptions pub;
        CHECK(mcp::parseTransportArgs({"--http", "--host", "0.0.0.0", "--token", "abc123secret"}, pub, error));
        CHECK(pub.httpOptions.token == "abc123secret" && !pub.tokenGenerated);
        mcp::TransportOptions gen;
        CHECK(mcp::parseTransportArgs({"--http", "--host", "0.0.0.0"}, gen, error));
        CHECK(!gen.httpOptions.token.empty());  // generated unless WETWARESUITE_TOKEN is set
        mcp::TransportOptions bad;
        CHECK(!mcp::parseTransportArgs({"--port", "99999"}, bad, error) && !error.empty());
        CHECK(!mcp::parseTransportArgs({"--bogus"}, bad, error));
        CHECK(!mcp::parseTransportArgs({"--host"}, bad, error));
        CHECK(mcp::isLoopbackHost("127.0.0.1") && mcp::isLoopbackHost("localhost") && mcp::isLoopbackHost("::1"));
        CHECK(!mcp::isLoopbackHost("0.0.0.0") && !mcp::isLoopbackHost("192.168.1.4"));
        CHECK(mcp::generateToken() != mcp::generateToken());
    }

    // a public address with no token is refused outright
    {
        mcp::HttpServer open(server, {"0.0.0.0", 0, "", 1u << 20, 2000});
        std::string error;
        CHECK(!open.listen(error) && error.find("token") != std::string::npos);
    }

    // no token on loopback: works, but only for localhost Host/Origin
    {
        mcp::HttpOptions options;
        options.port = 0;
        options.idleTimeoutMs = 2000;
        Running running(server, options);
        const int port = running.http.port();

        auto init = post(port, rpc(1, "initialize", {{"protocolVersion", "2025-06-18"}}).dump());
        CHECK(init.status == 200);
        CHECK(init.headers["content-type"].find("application/json") == 0);
        json ij = json::parse(init.body, nullptr, false);
        CHECK(ij["result"]["serverInfo"]["name"] == "test");
        CHECK(ij["result"]["instructions"] == "use doc_set_title");

        auto note = post(port, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        CHECK(note.status == 202 && note.body.empty());

        auto list = post(port, rpc(2, "tools/list").dump());
        json lj = json::parse(list.body, nullptr, false);
        CHECK(lj["result"]["tools"].size() == 5);
        CHECK(lj["result"]["tools"][0]["name"] == "doc_set_title");

        auto call = post(port, rpc(3, "tools/call", {{"name", "doc_set_title"}, {"arguments", {{"title", "over http"}}}}).dump());
        json cj = json::parse(call.body, nullptr, false);
        CHECK(cj["result"]["isError"] == false);
        CHECK(title == "over http");

        // a batch gets an array back
        json batch = json::array({rpc(4, "ping"), rpc(5, "tools/call", {{"name", "undo"}})});
        auto b = post(port, batch.dump());
        json bj = json::parse(b.body, nullptr, false);
        CHECK(bj.is_array() && bj.size() == 2);
        CHECK(title == "untitled");

        auto garbage = post(port, "{not json");
        CHECK(garbage.status == 200 && json::parse(garbage.body)["error"]["code"] == -32700);

        // a web page on another site can't reach in through the browser
        auto foreign = post(port, rpc(6, "ping").dump(), {{"Origin", "https://evil.example"}});
        CHECK(foreign.status == 403);
        auto local = post(port, rpc(7, "ping").dump(), {{"Origin", "http://localhost:3000"}});
        CHECK(local.status == 200);

        mcp::HttpResponse get;
        std::string error;
        CHECK(mcp::httpRequest("127.0.0.1", port, "GET", "/mcp", {}, "", get, error));
        CHECK(get.status == 405 && get.headers["allow"] == "POST");
        CHECK(mcp::httpRequest("127.0.0.1", port, "GET", "/elsewhere", {}, "", get, error));
        CHECK(get.status == 404);
        CHECK(mcp::httpRequest("127.0.0.1", port, "GET", "/", {}, "", get, error));
        CHECK(get.status == 200 && get.body.find("/mcp") != std::string::npos);
    }

    // with a token: no token, a wrong one, or the wrong scheme are all 401
    {
        mcp::HttpOptions options;
        options.port = 0;
        options.token = "s3cret-token-value";
        options.maxRequestBytes = 4096;
        options.idleTimeoutMs = 2000;
        Running running(server, options);
        const int port = running.http.port();
        const std::string ping = rpc(1, "ping").dump();

        auto none = post(port, ping);
        CHECK(none.status == 401);
        CHECK(none.headers.count("www-authenticate") == 1);
        CHECK(post(port, ping, {{"Authorization", "Bearer wrong"}}).status == 401);
        CHECK(post(port, ping, {{"Authorization", "Basic s3cret-token-value"}}).status == 401);
        CHECK(post(port, ping, {{"Authorization", "Bearer s3cret-token-value-and-more"}}).status == 401);
        auto ok = post(port, ping, {{"Authorization", "Bearer s3cret-token-value"}});
        CHECK(ok.status == 200 && json::parse(ok.body)["id"] == 1);

        // too big a body is refused before it is read
        auto big = post(port, std::string(8192, ' '), {{"Authorization", "Bearer s3cret-token-value"}});
        CHECK(big.status == 413);
    }

    return check::summary("mcp_http");
}
