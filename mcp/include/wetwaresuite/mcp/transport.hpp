// MCP transports: stdio (the default) and Streamable HTTP (optional, for remote
// use). Both feed the same Server::handle, so every tool behaves the same over
// either. The HTTP server is plain sockets (POSIX or winsock), single-threaded,
// POST /mcp with JSON responses; it binds 127.0.0.1 unless told otherwise and
// insists on a bearer token for any address other machines can reach.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "wetwaresuite/mcp/server.hpp"

namespace wetwaresuite::mcp {

struct HttpOptions {
    std::string host = "127.0.0.1";
    int port = 8765;               // 0 picks a free port (see HttpServer::port)
    std::string token;             // required as `Authorization: Bearer <token>` when set
    std::size_t maxRequestBytes = 16u << 20;
    int idleTimeoutMs = 15000;     // a connection that sends nothing for this long is dropped
};

struct TransportOptions {
    bool http = false;
    bool tokenGenerated = false;   // true when parseTransportArgs made one up for a public host
    HttpOptions httpOptions;
};

// Parses `--http`, `--port N`, `--host H` and `--token T` (token also from the
// WETWARESUITE_TOKEN environment variable). With a non-loopback host and no
// token, one is generated. Returns false with `error` set on a bad argument.
bool parseTransportArgs(const std::vector<std::string>& args, TransportOptions& out, std::string& error);

// One line per flag, for a binary's usage text.
const char* transportUsage();

// Runs the server over the chosen transport until stdin closes (stdio) or the
// process is stopped (http). Status lines go to `err`. Returns an exit code.
int serve(Server& server, const TransportOptions& options, std::istream& in, std::ostream& out, std::ostream& err);

bool isLoopbackHost(const std::string& host);
std::string generateToken();

class HttpServer {
public:
    HttpServer(Server& server, HttpOptions options);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds and listens; false with `error` set when the address is taken or invalid.
    bool listen(std::string& error);
    int port() const { return port_; }
    // Accepts and answers connections one at a time until stop() is called.
    void serve();
    // Safe to call from another thread; serve() returns within ~200 ms.
    void stop() { stopping_ = true; }

private:
    void handleConnection(std::intptr_t client);

    Server& server_;
    HttpOptions options_;
    std::intptr_t listener_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
};

// A minimal blocking HTTP/1.1 client for tests and health checks (loopback use).
struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
};
bool httpRequest(const std::string& host, int port, const std::string& method, const std::string& path,
                 const std::map<std::string, std::string>& headers, const std::string& body, HttpResponse& response,
                 std::string& error);

}  // namespace wetwaresuite::mcp
