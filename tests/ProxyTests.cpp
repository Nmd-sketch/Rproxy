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
#include <string>
#include <thread>

#include "../include/rproxy/Server.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

namespace {

// A looping origin that replies with a fixed body and records the last request it saw.
class StubOrigin {
public:
    explicit StubOrigin(std::string body)
        : m_body(std::move(body)) {

        m_acceptor.open(net::ip::tcp::v4());
        m_acceptor.set_option(net::socket_base::reuse_address(true));
        m_acceptor.bind(net::ip::tcp::endpoint(net::ip::tcp::v4(), 0));
        m_acceptor.listen();
        m_port = m_acceptor.local_endpoint().port();

        m_thread = std::thread([this] { serve(); });
    }

    ~StubOrigin() {
        sys::error_code ec;
        m_acceptor.close(ec);
        if (m_thread.joinable()) {
            m_thread.detach();
        }
    }

    StubOrigin(const StubOrigin&) = delete;
    StubOrigin& operator=(const StubOrigin&) = delete;

    std::uint16_t port() const { return m_port; }
    const std::string& observed_request() const { return m_observed; }

private:
    void serve() {
        for (;;) {
            net::ip::tcp::socket sock(m_acceptor.get_executor());
            sys::error_code ec;
            m_acceptor.accept(sock, ec);
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
            m_observed = wire;

            http::response<http::string_body> resp;
            resp.result(http::status::ok);
            resp.set(http::field::content_type, "text/plain");
            resp.body() = m_body;
            resp.prepare_payload();
            http::write(sock, resp, ec);
            sock.close(ec);
        }
    }

    net::io_context m_executor_ctx;
    net::ip::tcp::acceptor m_acceptor{m_executor_ctx};
    std::uint16_t m_port{0};
    std::string m_body;
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

std::string make_config(std::uint16_t port, const std::string& upstream_block) {
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
           "read_ms    = 30000\n";
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

// Starts a proxy on a free port over the given upstream block, and returns the port.
std::uint16_t start_proxy(const std::string& workspace,
                         const std::string& upstream_block) {
    const auto dir = make_workspace(workspace);
    copy_certs(dir);

    const auto proxy_port = free_port();
    write_file(dir / "config.toml", make_config(proxy_port, upstream_block));

    // @Usage: Server resolves cert/key relative to the working directory, so the
    // process is moved into the workspace before it is constructed.
    std::filesystem::current_path(dir);
    auto* server = new Server("config.toml");
    std::thread(serve_forever, server).detach();
    return proxy_port;
}

// Sends a plain GET through the proxy and returns the parsed response.
http::response<http::string_body> get_through_proxy(std::uint16_t proxy_port) {
    net::io_context ioc;
    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy_port));

    net::ip::tcp::socket sock(ioc);
    sock.connect(*endpoints.begin());

    beast::flat_buffer buffer;
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "example.com");
    http::write(sock, req);

    http::response<http::string_body> resp;
    http::read(sock, buffer, resp);
    return resp;
}

}  // namespace

TEST(Proxy, RelaysTheOriginBody)
{
    StubOrigin origin("ORIGIN-BODY-X");
    const auto proxy_port = start_proxy("relays", upstream_block_for(origin.port()));

    const auto resp = get_through_proxy(proxy_port);

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-BODY-X");
}

TEST(Proxy, StripsHopByHopHeaders)
{
    StubOrigin origin("BODY");
    const auto proxy_port = start_proxy("hopbyhop", upstream_block_for(origin.port()));

    const auto resp = get_through_proxy(proxy_port);
    ASSERT_EQ(resp.result_int(), 200);

    const std::string& seen = origin.observed_request();
    EXPECT_EQ(seen.find("Connection:"), std::string::npos) << seen;
    EXPECT_EQ(seen.find("Keep-Alive:"), std::string::npos) << seen;
}

TEST(Proxy, AddsForwardedHeadersAndKeepsHost)
{
    StubOrigin origin("BODY");
    const auto proxy_port = start_proxy("forwarded", upstream_block_for(origin.port()));

    const auto resp = get_through_proxy(proxy_port);
    ASSERT_EQ(resp.result_int(), 200);

    const std::string& seen = origin.observed_request();
    EXPECT_NE(seen.find("X-Forwarded-For: 127.0.0.1"), std::string::npos) << seen;
    EXPECT_NE(seen.find("X-Forwarded-Proto: http"), std::string::npos) << seen;
    EXPECT_NE(seen.find("X-Forwarded-Host: example.com"), std::string::npos) << seen;
    EXPECT_NE(seen.find("Host: example.com"), std::string::npos) << seen;
}

TEST(Proxy, RelaysAChunkedResponseWithACorrectContentLength)
{
    // The origin replies chunked. The parser dechunks without writing a
    // Content-Length back, so without prepare_payload() the client would receive
    // a body with no length header at all.
    net::io_context origin_ioc;
    net::ip::tcp::acceptor acceptor(origin_ioc,
        net::ip::tcp::endpoint(net::ip::tcp::v4(), 0));
    const auto origin_port = acceptor.local_endpoint().port();

    std::thread([&acceptor] {
        for (;;) {
            net::ip::tcp::socket sock(acceptor.get_executor());
            sys::error_code ec;
            acceptor.accept(sock, ec);
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

    const auto proxy_port = start_proxy("chunked", upstream_block_for(origin_port));

    const auto resp = get_through_proxy(proxy_port);

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-BODY-X");
    ASSERT_TRUE(resp.has_content_length());
    EXPECT_EQ(resp.base().at("Content-Length"), std::to_string(resp.body().size()));
}

TEST(Proxy, Returns503WhenNoUpstreamIsConfigured)
{
    const auto proxy_port = start_proxy("noupstream", "");

    const auto resp = get_through_proxy(proxy_port);

    EXPECT_EQ(resp.result_int(), 503);
}

TEST(Proxy, Returns502WhenTheOriginRefusesTheConnection)
{
    const auto dead_port = free_port();
    const auto proxy_port = start_proxy("refused", upstream_block_for(dead_port));

    const auto resp = get_through_proxy(proxy_port);

    EXPECT_EQ(resp.result_int(), 502);
}

TEST(Proxy, Returns400OnAMalformedRequestLine)
{
    const auto proxy_port = start_proxy("malformed", "");

    net::io_context ioc;
    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy_port));
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
    const auto proxy_port = start_proxy("tls", upstream_block_for(origin.port()));

    net::io_context ioc;
    net::ssl::context client_ctx{net::ssl::context::tlsv12_client};
    client_ctx.set_verify_mode(net::ssl::verify_none);

    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy_port));

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