#include <gtest/gtest.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../include/rproxy/Server.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

namespace {

// A socket already listening on an ephemeral port.
struct TCPListener {
    net::io_context ctx;
    net::ip::tcp::acceptor acceptor{ctx};

    TCPListener() {
        acceptor.open(net::ip::tcp::v4());
        acceptor.set_option(net::socket_base::reuse_address(true));
        acceptor.bind(net::ip::tcp::endpoint(net::ip::tcp::v4(), 0));
        acceptor.listen();
    }

    void close() {
        sys::error_code ec;
        acceptor.close(ec);
    }

    std::uint16_t port() const {
        return acceptor.local_endpoint().port();
    }
};

// A looping origin that replies with a fixed body and records the last request it saw.
// A looping origin that replies with a fixed body and records the last request it saw.
//
// The listener is owned by a shared_ptr and closed in the destructor, which makes the
// serving thread's accept() fail and return. Detaching it instead would leave the
// thread writing to m_observed and touching m_acceptor after those were destroyed.
class StubOrigin {
public:
    explicit StubOrigin(std::string body)
        : m_body(std::move(body)),
          m_listener(std::make_shared<TCPListener>()) {

        m_port = m_listener->port();
        m_thread = std::thread([this, listener = m_listener] { serve(*listener); });
    }

    ~StubOrigin() {
        m_listener->close();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    StubOrigin(const StubOrigin&) = delete;
    StubOrigin& operator=(const StubOrigin&) = delete;

    std::uint16_t port() const { return m_port; }

    // @Usage: Returns a copy. The serving thread writes m_observed, and reading the
    // reference directly from the test thread would be an unsynchronized data race.
    std::string observed_request() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_observed;
    }

private:
    void serve(TCPListener& listener) {
        for (;;) {
            net::ip::tcp::socket sock{listener.ctx};
            sys::error_code ec;
            listener.acceptor.accept(sock, ec);
            if (ec) {
                return;
            }

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(sock, buffer, req, ec);

            std::string wire;
            wire += std::string(req.method_string()) + " " + std::string(req.target()) + "\r\n";
            for (auto it = req.begin(); it != req.end(); ++it) {
                wire += std::string(it->name_string()) + ": " + std::string(it->value()) + "\r\n";
            }
            wire += "\r\n";
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_observed = wire;
            }

            http::response<http::string_body> resp;
            resp.result(http::status::ok);
            resp.set(http::field::content_type, "text/plain");
            resp.body() = m_body;
            resp.prepare_payload();
            http::write(sock, resp, ec);
            sock.close(ec);
        }
    }

    std::shared_ptr<TCPListener> m_listener;
    std::uint16_t m_port{0};
    std::string m_body;
    mutable std::mutex m_mutex;
    std::string m_observed;
    std::thread m_thread;
};

std::filesystem::path make_workspace(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("rproxy_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// @Usage: RPROXY_TLS_CERT_DIR comes from CMake and is absolute. Resolving the
// certificates relative to the working directory would depend on where ctest
// launched the process from, which differs between a direct run and a ctest run.
void copy_certs(const std::filesystem::path& dir) {
    const std::filesystem::path certs = RPROXY_TLS_CERT_DIR;
    ASSERT_TRUE(std::filesystem::exists(certs / "cert.pem"))
        << certs / "cert.pem missing";
    ASSERT_TRUE(std::filesystem::exists(certs / "key.pem"))
        << certs / "key.pem missing";
    std::filesystem::copy(certs / "cert.pem", dir / "cert.pem");
    std::filesystem::copy(certs / "key.pem", dir / "key.pem");
}

void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::ofstream out(p, std::ios::out | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "could not write " << p;
    out << contents;
}

std::string make_config(std::uint16_t port, const std::string& upstream_block,
                        int read_ms = 30000) {
    return "[server]\n"
           "port = " + std::to_string(port) + "\n"
           "threads = 1\n\n"
           "[server.tls]\n"
           "cert = \"cert.pem\"\n"
           "key  = \"key.pem\"\n\n" +
           upstream_block +
           "[limits]\n"
           "header_bytes = 8192\n"
           "body_bytes   = 8388608\n\n"
           "[timeouts]\n"
           "connect_ms = 5000\n"
           "handshake_ms = 5000\n"
           "read_ms    = " + std::to_string(read_ms) + "\n";
}

// Reserve a port by binding and releasing it, so the proxy can take it.
std::uint16_t free_port() {
    net::io_context ioc;
    net::ip::tcp::acceptor a(ioc, net::ip::tcp::endpoint(net::ip::tcp::v4(), 0));
    return a.local_endpoint().port();
}

void serve_forever(Server* server) {
    server->start();
}

std::string upstream_block_for(std::uint16_t origin_port) {
    return "[[upstream]]\nname = \"stub\"\nhost = \"127.0.0.1\"\nport = " +
           std::to_string(origin_port) + "\ntls = false\n\n";
}

// A running proxy, restored on destruction.
//
// Server resolves cert/key relative to the process working directory, so the
// proxy only works while the process sits inside its workspace. The previous
// working directory is restored on scope exit; without that, running several
// Proxy cases in one process leaves the last workspace in place for the next one.
class RunningProxy {
public:
    RunningProxy(std::string workspace, std::string upstream_block, int read_ms = 30000) {
        const auto dir = make_workspace(workspace);
        copy_certs(dir);

        const auto proxy_port = free_port();
        write_file(dir / "config.toml", make_config(proxy_port, upstream_block, read_ms));

        m_previous_cwd = std::filesystem::current_path();
        std::filesystem::current_path(dir);

        // @Usage: new Server is deliberate. gtest_discover_tests runs each case as
        // its own process, so the serving thread is left parked in io_context::run()
        // and dies with the process rather than being shut down.
        auto* server = new Server("config.toml");
        std::thread(serve_forever, server).detach();

        m_port = proxy_port;
    }

    ~RunningProxy() {
        std::error_code ec;
        std::filesystem::current_path(m_previous_cwd, ec);
    }

    RunningProxy(const RunningProxy&) = delete;
    RunningProxy& operator=(const RunningProxy&) = delete;

    std::uint16_t port() const { return m_port; }

private:
    std::filesystem::path m_previous_cwd;
    std::uint16_t m_port{0};
};

// Sends a request through the proxy and returns the parsed response.
http::response<http::string_body> send_through_proxy(std::uint16_t proxy_port,
                                                     const http::request<http::string_body>& req) {
    net::io_context ioc;
    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy_port));

    net::ip::tcp::socket sock(ioc);
    sock.connect(*endpoints.begin());

    beast::flat_buffer buffer;
    http::write(sock, req);

    http::response<http::string_body> resp;
    http::read(sock, buffer, resp);
    return resp;
}

http::response<http::string_body> get_through_proxy(std::uint16_t proxy_port) {
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "example.com");
    return send_through_proxy(proxy_port, req);
}

// Reads a header value from a captured raw request, matching the field name exactly.
// A substring search would report X-Keep-Alive: as Keep-Alive:, so the line has to be
// matched at the start of the field name.
std::optional<std::string> header_in(const std::string& wire, std::string_view name) {
    std::size_t pos = wire.find("\r\n");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos += 2;

    while (pos < wire.size()) {
        const std::size_t end = wire.find("\r\n", pos);
        if (end == std::string::npos || end == pos) {
            return std::nullopt;
        }
        const std::string line = wire.substr(pos, end - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string field = line.substr(0, colon);
            if (beast::iequals(field, name)) {
                return line.substr(colon + 1);
            }
        }
        pos = end + 2;
    }
    return std::nullopt;
}

}  // namespace

TEST(Proxy, RelaysTheOriginBody)
{
    StubOrigin origin("ORIGIN-BODY-X");
    const RunningProxy proxy("relays", upstream_block_for(origin.port()));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-BODY-X");
}

TEST(Proxy, StripsHopByHopHeaders)
{
    StubOrigin origin("BODY");
    const RunningProxy proxy("hopbyhop", upstream_block_for(origin.port()));

    // @Usage: The client must actually send the headers for this to assert anything.
    // A default GET carries neither Connection nor Keep-Alive, so the previous version
    // of this test asserted their absence from a request that never had them.
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "example.com");
    req.set(http::field::connection, "keep-alive, X-Internal-Token");
    req.set(http::field::keep_alive, "timeout=5");
    req.set(http::field::te, "trailers");
    req.set("X-Internal-Token", "secret");
    req.set("X-Keep-Me", "yes");

    const auto resp = send_through_proxy(proxy.port(), req);
    ASSERT_EQ(resp.result_int(), 200);

    const std::string seen = origin.observed_request();
    EXPECT_FALSE(header_in(seen, "Connection").has_value()) << seen;
    EXPECT_FALSE(header_in(seen, "Keep-Alive").has_value()) << seen;
    EXPECT_FALSE(header_in(seen, "TE").has_value()) << seen;
    EXPECT_FALSE(header_in(seen, "X-Internal-Token").has_value())
        << "named by Connection, so it is hop by hop too: " << seen;

    // End to end headers must survive, and X-Keep-Alive must not be mistaken for
    // Keep-Alive by a substring match.
    EXPECT_TRUE(header_in(seen, "X-Keep-Me").has_value()) << seen;
    EXPECT_TRUE(header_in(seen, "Host").has_value()) << seen;
}

TEST(Proxy, AddsForwardedHeadersAndKeepsHost)
{
    StubOrigin origin("BODY");
    const RunningProxy proxy("forwarded", upstream_block_for(origin.port()));

    const auto resp = get_through_proxy(proxy.port());
    ASSERT_EQ(resp.result_int(), 200);

    const std::string seen = origin.observed_request();

    const auto xff = header_in(seen, "X-Forwarded-For");
    ASSERT_TRUE(xff.has_value()) << seen;
    EXPECT_EQ(*xff, " 127.0.0.1");

    const auto proto = header_in(seen, "X-Forwarded-Proto");
    ASSERT_TRUE(proto.has_value()) << seen;
    EXPECT_EQ(*proto, " http");

    const auto fwd_host = header_in(seen, "X-Forwarded-Host");
    ASSERT_TRUE(fwd_host.has_value()) << seen;
    EXPECT_EQ(*fwd_host, " example.com");

    const auto host = header_in(seen, "Host");
    ASSERT_TRUE(host.has_value()) << seen;
    EXPECT_EQ(*host, " example.com");
}

TEST(Proxy, RelaysAChunkedResponseWithACorrectContentLength)
{
    // The origin replies chunked. The parser dechunks without writing a
    // Content-Length back, so without prepare_payload() the client would receive
    // a body with no length header at all.
    //
    // @Correctness: The listener is heap owned and shared into the thread. Capturing a
    // stack acceptor by reference and detaching would dangle at the end of the test.
    auto listener = std::make_shared<TCPListener>();
    const auto origin_port = listener->port();

    std::thread([listener] {
        for (;;) {
            net::ip::tcp::socket sock{listener->ctx};
            sys::error_code ec;
            listener->acceptor.accept(sock, ec);
            if (ec) {
                return;
            }
            static const std::string reply =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "d\r\nORIGIN-BODY-X\r\n"
                "0\r\n\r\n";
            net::write(sock, net::buffer(reply), ec);
            sock.close(ec);
        }
    }).detach();

    const RunningProxy proxy("chunked", upstream_block_for(origin_port));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-BODY-X");
    ASSERT_TRUE(resp.has_content_length());
    EXPECT_EQ(resp.base().at("Content-Length"), std::to_string(resp.body().size()));
}

TEST(Proxy, Returns503WhenNoUpstreamIsConfigured)
{
    const RunningProxy proxy("noupstream", "");

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 503);
}

TEST(Proxy, Returns502WhenTheOriginRefusesTheConnection)
{
    const auto dead_port = free_port();
    const RunningProxy proxy("refused", upstream_block_for(dead_port));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 502);
}

TEST(Proxy, RelaysAHeadResponseWithoutWaitingForABody)
{
    // A HEAD response carries Content-Length but no body, so a parser that does not
    // skip the body waits forever for bytes the origin will never send.
    auto listener = std::make_shared<TCPListener>();
    const auto origin_port = listener->port();

    std::thread([listener] {
        for (;;) {
            net::ip::tcp::socket sock{listener->ctx};
            sys::error_code ec;
            listener->acceptor.accept(sock, ec);
            if (ec) {
                return;
            }
            static const std::string reply =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 42\r\n"
                "\r\n";
            net::write(sock, net::buffer(reply), ec);

            // Hold the connection open, which is what made the proxy hang before.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            sock.close(ec);
        }
    }).detach();

    const RunningProxy proxy("head", upstream_block_for(origin_port));

    http::request<http::string_body> req{http::verb::head, "/", 11};
    req.set(http::field::host, "example.com");

    const auto resp = send_through_proxy(proxy.port(), req);

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_TRUE(resp.body().empty());
}

TEST(Proxy, RefusesATraceRequestCarryingABodyWithoutTerminating)
{
    // Beast's prepare_payload throws std::invalid_argument for TRACE with a body. If
    // that escapes the completion handler it unwinds out of io_context::run() and
    // terminates the process, so one request would kill the proxy for everyone.
    StubOrigin origin("BODY");
    const RunningProxy proxy("trace", upstream_block_for(origin.port()));

    http::request<http::string_body> req{http::verb::trace, "/x", 11};
    req.set(http::field::host, "example.com");
    req.body() = "hello";
    // @Usage: Content-Length is set by hand rather than through prepare_payload(),
    // which is the call under test on the proxy side and would throw here too.
    req.set(http::field::content_length, "5");

    const auto resp = send_through_proxy(proxy.port(), req);

    EXPECT_EQ(resp.result_int(), 400);

    // The proxy must still be serving: a second, ordinary request has to succeed.
    const auto follow_up = get_through_proxy(proxy.port());
    EXPECT_EQ(follow_up.result_int(), 200);
    EXPECT_EQ(follow_up.body(), "BODY");
}

TEST(Proxy, TimesOutWhenTheOriginNeverReplies)
{
    auto listener = std::make_shared<TCPListener>();
    const auto origin_port = listener->port();

    std::thread([listener] {
        for (;;) {
            net::ip::tcp::socket sock{listener->ctx};
            sys::error_code ec;
            listener->acceptor.accept(sock, ec);
            if (ec) {
                return;
            }
            // Accept, read the request, then say nothing at all.
            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(sock, buffer, req, ec);
            std::this_thread::sleep_for(std::chrono::seconds(30));
            sock.close(ec);
        }
    }).detach();

    const RunningProxy proxy("timeout", upstream_block_for(origin_port), 1500);

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 504);
}

TEST(Proxy, TimesOutWhenAClientStallsBeforeSendingARequest)
{
    const RunningProxy proxy("slowloris", "", 1500);

    net::io_context ioc;
    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy.port()));
    net::ip::tcp::socket sock(ioc);
    sock.connect(*endpoints.begin());

    // Connect, then send nothing. The proxy must give up rather than hold the session.
    static const std::string partial = "GET / HTTP/1.1\r\nHost: example.com\r\n";
    net::write(sock, net::buffer(partial));

    beast::flat_buffer buffer;
    http::response<http::string_body> resp;
    beast::error_code ec;
    http::read(sock, buffer, resp, ec);

    ASSERT_FALSE(ec) << "expected a 504 response, got: " << ec.message();
    EXPECT_EQ(resp.result_int(), 504);
}

TEST(Proxy, Returns400OnAMalformedRequestLine)
{
    const RunningProxy proxy("malformed", "");

    net::io_context ioc;
    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy.port()));
    net::ip::tcp::socket sock(ioc);
    sock.connect(*endpoints.begin());

    static const std::string junk = "NOT-A-VALID-REQUEST-LINE\r\n\r\n";
    net::write(sock, net::buffer(junk));

    beast::flat_buffer buffer;
    http::response<http::string_body> resp;
    http::read(sock, buffer, resp);

    EXPECT_EQ(resp.result_int(), 400);
}

TEST(Proxy, RoutesOverTls)
{
    StubOrigin origin("ORIGIN-TLS");
    const RunningProxy proxy("tls", upstream_block_for(origin.port()));

    net::io_context ioc;
    net::ssl::context client_ctx{net::ssl::context::tlsv12_client};
    client_ctx.set_verify_mode(net::ssl::verify_none);

    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy.port()));

    net::ssl::stream<net::ip::tcp::socket> sock{ioc, client_ctx};
    sock.next_layer().connect(*endpoints.begin());
    sock.handshake(net::ssl::stream_base::client);

    beast::flat_buffer buffer;
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "example.com");
    http::write(sock, req);

    http::response<http::string_body> resp;
    http::read(sock, buffer, resp);

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-TLS");
}