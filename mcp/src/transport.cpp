#include "wetwaresuite/mcp/transport.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace wetwaresuite::mcp {

namespace {

#ifdef _WIN32
using Sock = SOCKET;
const Sock kNoSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
struct NetInit {
    NetInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~NetInit() { WSACleanup(); }
};
void netInit() { static NetInit init; }
#else
using Sock = int;
const Sock kNoSock = -1;
void closeSock(Sock s) { ::close(s); }
void netInit() {}
#endif

Sock toSock(std::intptr_t v) { return static_cast<Sock>(v); }
std::intptr_t fromSock(Sock s) { return static_cast<std::intptr_t>(s); }

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

std::atomic<bool> gSignalled{false};
void onSignal(int) { gSignalled = true; }

std::string envVar(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || !value) return {};
    std::string out = value;
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

void setTimeouts(Sock s, int ms) {
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
#else
    timeval t{};
    t.tv_sec = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
#endif
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

bool sendAll(Sock s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const std::size_t chunk = std::min<std::size_t>(data.size() - sent, 1u << 20);
#ifdef _WIN32
        const int n = ::send(s, data.data() + sent, static_cast<int>(chunk), 0);
#elif defined(MSG_NOSIGNAL)
        const auto n = ::send(s, data.data() + sent, chunk, MSG_NOSIGNAL);
#else
        const auto n = ::send(s, data.data() + sent, chunk, 0);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Bytes arriving on one connection, with a deadline for the whole request.
struct Conn {
    Sock sock;
    std::chrono::steady_clock::time_point deadline;
    std::string buf;
    std::size_t pos = 0;  // start of unread bytes in buf

    // Reads at least one more byte; false on EOF, error, timeout or deadline.
    bool more() {
        if (std::chrono::steady_clock::now() > deadline) return false;
        char chunk[16384];
        const auto n = ::recv(sock, chunk, static_cast<int>(sizeof chunk), 0);
        if (n <= 0) return false;
        buf.append(chunk, static_cast<std::size_t>(n));
        return true;
    }
    bool need(std::size_t bytes) {
        while (buf.size() - pos < bytes) {
            if (!more()) return false;
        }
        return true;
    }
    bool line(std::string& out, std::size_t limit) {
        for (;;) {
            auto end = buf.find("\r\n", pos);
            if (end != std::string::npos) {
                out = buf.substr(pos, end - pos);
                pos = end + 2;
                return true;
            }
            if (buf.size() - pos > limit || !more()) return false;
        }
    }
};

struct Request {
    std::string method, path;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string reasonFor(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        default: return "Error";
    }
}

std::string response(int status, const std::string& type, const std::string& body, const std::string& extraHeaders = {}) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reasonFor(status) + "\r\n";
    if (!type.empty()) out += "Content-Type: " + type + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\n";
    out += extraHeaders;
    out += "\r\n";
    out += body;
    return out;
}

// 0 when the request was read whole, else the HTTP status to answer with.
int readRequest(Conn& c, std::size_t maxBody, Request& req) {
    std::string first;
    if (!c.line(first, kMaxHeaderBytes)) return c.buf.size() > kMaxHeaderBytes ? 431 : 408;
    std::istringstream rl(first);
    std::string target, version;
    rl >> req.method >> target >> version;
    if (req.method.empty() || target.empty() || version.rfind("HTTP/1.", 0) != 0) return 400;
    req.path = target.substr(0, target.find('?'));
    std::size_t headerBytes = first.size();
    for (std::string h;;) {
        if (!c.line(h, kMaxHeaderBytes)) return 408;
        if (h.empty()) break;
        headerBytes += h.size();
        if (headerBytes > kMaxHeaderBytes) return 431;
        auto colon = h.find(':');
        if (colon == std::string::npos) return 400;
        req.headers[lower(trim(h.substr(0, colon)))] = trim(h.substr(colon + 1));
    }
    auto te = req.headers.find("transfer-encoding");
    if (te != req.headers.end() && lower(te->second).find("chunked") != std::string::npos) {
        for (std::string sizeLine;;) {
            if (!c.line(sizeLine, 1024)) return 408;
            std::size_t size = 0;
            try {
                size = std::stoul(sizeLine.substr(0, sizeLine.find(';')), nullptr, 16);
            } catch (const std::exception&) {
                return 400;
            }
            if (req.body.size() + size > maxBody) return 413;
            if (!c.need(size + 2)) return 408;
            if (size == 0) {
                // trailers: skip lines up to the blank one
                std::string trailer;
                while (c.line(trailer, kMaxHeaderBytes) && !trailer.empty()) {
                }
                return 0;
            }
            req.body.append(c.buf, c.pos, size);
            c.pos += size + 2;
        }
    }
    std::size_t length = 0;
    auto cl = req.headers.find("content-length");
    if (cl != req.headers.end()) {
        try {
            length = std::stoull(cl->second);
        } catch (const std::exception&) {
            return 400;
        }
    } else if (req.method == "POST") {
        return 411;
    }
    if (length > maxBody) return 413;
    if (!c.need(length)) return 408;
    req.body = c.buf.substr(c.pos, length);
    return 0;
}

bool sameSecret(const std::string& a, const std::string& b) {
    unsigned diff = a.size() == b.size() ? 0u : 1u;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i % (b.empty() ? 1 : b.size())]);
    return diff == 0 && !b.empty();
}

// "http://localhost:3000" or "localhost:8765" -> "localhost"
std::string hostPart(std::string s) {
    auto scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    s = s.substr(0, s.find('/'));
    if (!s.empty() && s[0] == '[') return lower(s.substr(1, s.find(']') - 1));
    return lower(s.substr(0, s.find(':')));
}

std::string displayHost(const std::string& host) {
    if (host == "0.0.0.0" || host == "::" || host.empty()) return "<this-computer's-address>";
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

}  // namespace

bool isLoopbackHost(const std::string& host) {
    const std::string h = lower(host);
    return h == "localhost" || h == "::1" || h == "[::1]" || h.rfind("127.", 0) == 0;
}

std::string generateToken() {
    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(),
                       static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
    std::mt19937_64 rng(seed);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 40; ++i) out += hex[rng() & 15];
    return out;
}

const char* transportUsage() {
    return "  --http                serve over HTTP (MCP Streamable HTTP, POST /mcp) instead of stdin/stdout\n"
           "  --port N              HTTP port (default 8765; 0 picks a free one)\n"
           "  --host H              address to listen on (default 127.0.0.1, this computer only)\n"
           "  --token T             require `Authorization: Bearer T` (also read from WETWARESUITE_TOKEN;\n"
           "                        generated and printed when --host is reachable from other machines)\n";
}

bool parseTransportArgs(const std::vector<std::string>& args, TransportOptions& out, std::string& error) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool takesValue = a == "--port" || a == "--host" || a == "--token";
        if (takesValue && i + 1 >= args.size()) {
            error = a + " needs a value";
            return false;
        }
        if (a == "--http") {
            out.http = true;
        } else if (a == "--port") {
            const std::string v = args[++i];
            char* end = nullptr;
            const long port = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end || port < 0 || port > 65535) {
                error = "--port needs a number from 0 to 65535, not '" + v + "'";
                return false;
            }
            out.httpOptions.port = static_cast<int>(port);
            out.http = true;
        } else if (a == "--host") {
            out.httpOptions.host = args[++i];
            if (out.httpOptions.host.size() > 2 && out.httpOptions.host.front() == '[' && out.httpOptions.host.back() == ']')
                out.httpOptions.host = out.httpOptions.host.substr(1, out.httpOptions.host.size() - 2);
            out.http = true;
        } else if (a == "--token") {
            out.httpOptions.token = args[++i];
            out.http = true;
        } else {
            error = "unknown option '" + a + "'";
            return false;
        }
    }
    if (out.httpOptions.token.empty()) out.httpOptions.token = trim(envVar("WETWARESUITE_TOKEN"));
    for (char c : out.httpOptions.token) {
        if (std::isspace(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) < 0x21 || c == 0x7f) {
            error = "the token can't contain spaces or control characters";
            return false;
        }
    }
    if (out.http && out.httpOptions.token.empty() && !isLoopbackHost(out.httpOptions.host)) {
        out.httpOptions.token = generateToken();
        out.tokenGenerated = true;
    }
    return true;
}

HttpServer::HttpServer(Server& server, HttpOptions options) : server_(server), options_(std::move(options)) { netInit(); }

HttpServer::~HttpServer() {
    if (listener_ != -1) closeSock(toSock(listener_));
}

bool HttpServer::listen(std::string& error) {
    if (options_.token.empty() && !isLoopbackHost(options_.host)) {
        error = "refusing to listen on " + options_.host + " without a token: anyone on the network could use your files";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* found = nullptr;
    const std::string port = std::to_string(options_.port);
    if (int rc = getaddrinfo(options_.host.c_str(), port.c_str(), &hints, &found); rc != 0 || !found) {
        error = "can't use address '" + options_.host + "'";
        return false;
    }
    Sock s = kNoSock;
    for (addrinfo* ai = found; ai; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kNoSock) continue;
#ifndef _WIN32
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 && ::listen(s, 16) == 0) break;
        closeSock(s);
        s = kNoSock;
    }
    freeaddrinfo(found);
    if (s == kNoSock) {
        error = "can't listen on " + options_.host + ":" + port + " (is something else using that port? try --port 0)";
        return false;
    }
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        if (addr.ss_family == AF_INET) port_ = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
        else if (addr.ss_family == AF_INET6) port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    }
    listener_ = fromSock(s);
    return true;
}

void HttpServer::serve() {
    const Sock listener = toSock(listener_);
    while (!stopping_ && !gSignalled) {
#ifdef _WIN32
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener, &ready);
        timeval wait{0, 200000};
        const int n = ::select(0, &ready, nullptr, nullptr, &wait);
#else
        pollfd p{listener, POLLIN, 0};
        const int n = ::poll(&p, 1, 200);
#endif
        if (n <= 0) continue;
        const Sock client = ::accept(listener, nullptr, nullptr);
        if (client == kNoSock) continue;
        setTimeouts(client, options_.idleTimeoutMs);
        handleConnection(fromSock(client));
        // stop sending, then drain what the client is still sending, so closing with
        // unread bytes doesn't reset the connection before it reads our answer
#ifdef _WIN32
        ::shutdown(client, SD_SEND);
#else
        ::shutdown(client, SHUT_WR);
#endif
        setTimeouts(client, 200);
        char sink[16384];
        for (std::size_t drained = 0; drained < options_.maxRequestBytes + kMaxHeaderBytes;) {
            const auto n = ::recv(client, sink, static_cast<int>(sizeof sink), 0);
            if (n <= 0) break;
            drained += static_cast<std::size_t>(n);
        }
        closeSock(client);
    }
}

void HttpServer::handleConnection(std::intptr_t clientHandle) {
    const Sock client = toSock(clientHandle);
    Conn conn{client, std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(4 * options_.idleTimeoutMs, 30000)), {}, 0};
    Request req;
    const std::string jsonType = "application/json";
    auto reply = [&](int status, const std::string& message, const std::string& extra = {}) {
        sendAll(client, response(status, jsonType, nlohmann::json({{"error", message}}).dump(), extra));
    };
    if (int status = readRequest(conn, options_.maxRequestBytes, req); status != 0) {
        if (status != 408) reply(status, reasonFor(status));  // a silent or vanished client gets nothing
        return;
    }
    if (!options_.token.empty()) {
        const std::string auth = req.headers.count("authorization") ? req.headers["authorization"] : "";
        const bool ok = auth.size() > 7 && lower(auth.substr(0, 7)) == "bearer " && sameSecret(trim(auth.substr(7)), options_.token);
        if (!ok) {
            reply(401, "missing or wrong token: send `Authorization: Bearer <token>`", "WWW-Authenticate: Bearer realm=\"wetwaresuite\"\r\n");
            return;
        }
    } else {
        // no token means loopback only: refuse pages on other sites (DNS rebinding) reaching in through the browser
        const std::string host = req.headers.count("host") ? hostPart(req.headers["host"]) : "";
        const std::string origin = req.headers.count("origin") ? req.headers["origin"] : "";
        if (!isLoopbackHost(host) || (!origin.empty() && origin != "null" && !isLoopbackHost(hostPart(origin)))) {
            reply(403, "requests must come from this computer (localhost)");
            return;
        }
    }
    if (req.path == "/" && req.method == "GET") {
        sendAll(client, response(200, "text/plain; charset=utf-8",
                                 "wetwaresuite MCP server. Point your MCP client at /mcp (Streamable HTTP, POST JSON-RPC).\n"));
        return;
    }
    if (req.path != "/mcp") {
        reply(404, "not found: the MCP endpoint is /mcp");
        return;
    }
    if (req.method != "POST") {
        // no server-initiated stream (GET/SSE) and no session to DELETE
        reply(405, "use POST for JSON-RPC messages", "Allow: POST\r\n");
        return;
    }
    auto ct = req.headers.find("content-type");
    if (ct != req.headers.end() && lower(ct->second).find("json") == std::string::npos) {
        reply(415, "send Content-Type: application/json");
        return;
    }
    // A JSON array is a batch (older protocol versions): answer each message in it.
    std::size_t first = req.body.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && req.body[first] == '[') {
        nlohmann::json batch = nlohmann::json::parse(req.body, nullptr, false);
        if (batch.is_array() && !batch.empty()) {
            nlohmann::json answers = nlohmann::json::array();
            for (const auto& message : batch) {
                if (auto r = server_.handle(message.dump())) answers.push_back(nlohmann::json::parse(*r));
            }
            sendAll(client, answers.empty() ? response(202, "", "") : response(200, jsonType, answers.dump()));
            return;
        }
    }
    auto answer = server_.handle(req.body);
    sendAll(client, answer ? response(200, jsonType, *answer) : response(202, "", ""));
}

int serve(Server& server, const TransportOptions& options, std::istream& in, std::ostream& out, std::ostream& err) {
    if (!options.http) {
        server.run(in, out);
        return 0;
    }
    HttpServer http(server, options.httpOptions);
    std::string error;
    if (!http.listen(error)) {
        err << "error: " << error << "\n";
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const std::string url = "http://" + displayHost(options.httpOptions.host) + ":" + std::to_string(http.port()) + "/mcp";
    err << "MCP server listening on " << url << "\n";
    if (!options.httpOptions.token.empty()) {
        if (options.tokenGenerated) err << "token (made up for this run; pass --token or set WETWARESUITE_TOKEN to keep one): "
                                        << options.httpOptions.token << "\n";
        else err << "clients must send: Authorization: Bearer <your token>\n";
    }
    err << "connect Claude Code with:\n  claude mcp add --transport http wetwaresuite " << url;
    if (!options.httpOptions.token.empty())
        err << " --header \"Authorization: Bearer " << (options.tokenGenerated ? options.httpOptions.token : "<your token>") << "\"";
    err << "\npress Ctrl+C to stop\n" << std::flush;
    http.serve();
    err << "stopped\n";
    return 0;
}

bool httpRequest(const std::string& host, int port, const std::string& method, const std::string& path,
                 const std::map<std::string, std::string>& headers, const std::string& body, HttpResponse& response,
                 std::string& error) {
    netInit();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &found) != 0 || !found) {
        error = "can't resolve " + host;
        return false;
    }
    Sock s = kNoSock;
    for (addrinfo* ai = found; ai; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kNoSock) continue;
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
        closeSock(s);
        s = kNoSock;
    }
    freeaddrinfo(found);
    if (s == kNoSock) {
        error = "can't connect to " + host + ":" + portText;
        return false;
    }
    setTimeouts(s, 10000);
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + (host.find(':') != std::string::npos ? "[" + host + "]" : host) +
                          ":" + portText + "\r\nConnection: close\r\n";
    for (const auto& [k, v] : headers) request += k + ": " + v + "\r\n";
    if (!body.empty() || method == "POST") request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n" + body;
    if (!sendAll(s, request)) {
        closeSock(s);
        error = "send failed";
        return false;
    }
    Conn conn{s, std::chrono::steady_clock::now() + std::chrono::seconds(30), {}, 0};
    while (conn.more()) {
    }
    closeSock(s);
    const auto headEnd = conn.buf.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        error = "no HTTP response";
        return false;
    }
    std::istringstream head(conn.buf.substr(0, headEnd));
    std::string line, version;
    std::getline(head, line);
    std::istringstream status(line);
    status >> version >> response.status;
    while (std::getline(head, line)) {
        auto colon = line.find(':');
        if (colon != std::string::npos) response.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    response.body = conn.buf.substr(headEnd + 4);
    return response.status > 0;
}

}  // namespace wetwaresuite::mcp
