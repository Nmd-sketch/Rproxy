#include <gtest/gtest.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>
#include <string>
#include "../include/rproxy/RequestTools.hpp"
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

// ---------------------------------------------------------------------------
// make_clean_request
// ---------------------------------------------------------------------------

TEST(MakeCleanRequest, RemovesStandardHopByHopHeaders) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);

    req.set(http::field::host, "example.com");
    req.set(http::field::user_agent, "test/1.0");
    req.set(http::field::accept, "text/html");
    req.set(http::field::authorization, "Bearer tok");
    req.set(http::field::cookie, "a=b");
    req.set("X-Custom-Header", "custom-value");

    req.set(http::field::connection, "keep-alive");
    req.set(http::field::keep_alive, "timeout=5");
    req.set(http::field::proxy_authorization, "Basic xyz");
    req.set(http::field::te, "trailers");
    req.set(http::field::trailer, "X-Trailer");
    req.set(http::field::upgrade, "websocket");

    auto cleaned = RequestTools::make_clean_request(req);

    // End-to-end headers survive
    EXPECT_EQ(cleaned.count(http::field::host), 1);
    EXPECT_EQ(cleaned.count(http::field::user_agent), 1);
    EXPECT_EQ(cleaned.count(http::field::accept), 1);
    EXPECT_EQ(cleaned.count(http::field::authorization), 1);
    EXPECT_EQ(cleaned.count(http::field::cookie), 1);
    EXPECT_EQ(cleaned.count("X-Custom-Header"), 1);

    // Hop-by-hop headers removed
    EXPECT_EQ(cleaned.count(http::field::connection), 0);
    EXPECT_EQ(cleaned.count(http::field::keep_alive), 0);
    EXPECT_EQ(cleaned.count(http::field::proxy_authorization), 0);
    EXPECT_EQ(cleaned.count(http::field::te), 0);
    EXPECT_EQ(cleaned.count(http::field::trailer), 0);
    EXPECT_EQ(cleaned.count(http::field::upgrade), 0);
}

TEST(MakeCleanRequest, RemovesConnectionNamedHeaders) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);
    req.set(http::field::host, "example.com");

    req.set(http::field::connection, "X-Internal-Token, X-Debug");
    req.set("X-Internal-Token", "secret");
    req.set("X-Debug", "1");
    req.set("X-Keep-Me", "yes");

    auto cleaned = RequestTools::make_clean_request(req);

    EXPECT_EQ(cleaned.count("X-Internal-Token"), 0);
    EXPECT_EQ(cleaned.count("X-Debug"), 0);
    EXPECT_EQ(cleaned.count(http::field::connection), 0);
    EXPECT_EQ(cleaned.count("X-Keep-Me"), 1);
    EXPECT_EQ(cleaned.count(http::field::host), 1);
}

TEST(MakeCleanRequest, ConnectionNamedHeadersAreCaseInsensitive) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);
    req.set(http::field::host, "example.com");

    req.set(http::field::connection, "x-internal-token");
    req.set("X-Internal-Token", "secret");

    auto cleaned = RequestTools::make_clean_request(req);

    EXPECT_EQ(cleaned.count("X-Internal-Token"), 0);
    EXPECT_EQ(cleaned.count(http::field::connection), 0);
}

TEST(MakeCleanRequest, PreservesHostHeader) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);
    req.set(http::field::host, "public.example.com");

    auto cleaned = RequestTools::make_clean_request(req);
    EXPECT_EQ(cleaned.count(http::field::host), 1);
    EXPECT_EQ(cleaned[http::field::host], "public.example.com");
}

TEST(MakeCleanRequest, EmptyRequestIsHandled) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);

    auto cleaned = RequestTools::make_clean_request(req);
    EXPECT_EQ(cleaned.count(http::field::connection), 0);
}

// ---------------------------------------------------------------------------
// make_clean_response
// ---------------------------------------------------------------------------

TEST(MakeCleanResponse, RemovesStandardHopByHopHeaders) {
    http::response<http::string_body> resp;
    resp.result(http::status::ok);
    resp.version(11);

    resp.set(http::field::content_type, "text/html");
    resp.set(http::field::set_cookie, "session=abc");
    resp.set(http::field::location, "https://example.com/next");
    resp.set(http::field::cache_control, "max-age=60");
    resp.set("X-Custom", "v");

    resp.set(http::field::connection, "close");
    resp.set(http::field::keep_alive, "timeout=5");
    resp.set(http::field::transfer_encoding, "chunked");
    resp.set(http::field::upgrade, "h2c");

    auto cleaned = RequestTools::make_clean_response(resp);

    EXPECT_EQ(cleaned.count(http::field::content_type), 1);
    EXPECT_EQ(cleaned.count(http::field::set_cookie), 1);
    EXPECT_EQ(cleaned.count(http::field::location), 1);
    EXPECT_EQ(cleaned.count(http::field::cache_control), 1);
    EXPECT_EQ(cleaned.count("X-Custom"), 1);

    EXPECT_EQ(cleaned.count(http::field::connection), 0);
    EXPECT_EQ(cleaned.count(http::field::keep_alive), 0);
    EXPECT_EQ(cleaned.count(http::field::transfer_encoding), 0);
    EXPECT_EQ(cleaned.count(http::field::upgrade), 0);
}

TEST(MakeCleanResponse, RemovesConnectionNamedHeaders) {
    http::response<http::string_body> resp;
    resp.result(http::status::ok);
    resp.version(11);

    resp.set(http::field::connection, "X-Backend-Node");
    resp.set("X-Backend-Node", "node-7");
    resp.set("X-Survives", "yes");

    auto cleaned = RequestTools::make_clean_response(resp);

    EXPECT_EQ(cleaned.count("X-Backend-Node"), 0);
    EXPECT_EQ(cleaned.count(http::field::connection), 0);
    EXPECT_EQ(cleaned.count("X-Survives"), 1);
}

TEST(MakeCleanResponse, DoesNotAddXForwardedHeaders) {
    http::response<http::string_body> resp;
    resp.result(http::status::ok);
    resp.version(11);
    resp.set(http::field::content_type, "text/plain");

    auto cleaned = RequestTools::make_clean_response(resp);

    EXPECT_EQ(cleaned.count(http::field::x_forwarded_for), 0);
    EXPECT_EQ(cleaned.count(http::field::x_forwarded_proto), 0);
    EXPECT_EQ(cleaned.count(http::field::x_forwarded_host), 0);
}

// ---------------------------------------------------------------------------
// append_x_headers
// ---------------------------------------------------------------------------

TEST(AppendXHeaders, SetsAllThreeHeaders) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);
    req.set(http::field::host, "public.example.com");

    RequestTools::append_x_headers(req, "203.0.113.5", "https");

    EXPECT_EQ(req[http::field::x_forwarded_for], "203.0.113.5");
    EXPECT_EQ(req[http::field::x_forwarded_proto], "https");
    EXPECT_EQ(req[http::field::x_forwarded_host], "public.example.com");
}

TEST(AppendXHeaders, OverwritesClientSuppliedXff) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);
    req.set(http::field::host, "public.example.com");

    // Client sends a spoofed XFF
    req.set(http::field::x_forwarded_for, "10.0.0.1");

    RequestTools::append_x_headers(req, "203.0.113.5", "https");

    // Must be the real client IP, not the spoofed one
    EXPECT_EQ(req[http::field::x_forwarded_for], "203.0.113.5");
}

TEST(AppendXHeaders, OmitsForwardedHostWhenNoHostPresent) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);
    // No Host header set

    RequestTools::append_x_headers(req, "203.0.113.5", "http");

    EXPECT_EQ(req[http::field::x_forwarded_for], "203.0.113.5");
    EXPECT_EQ(req[http::field::x_forwarded_proto], "http");
    EXPECT_EQ(req.count(http::field::x_forwarded_host), 0);
}

TEST(AppendXHeaders, ProtoIsHttpForPlainConnection) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);
    req.set(http::field::host, "public.example.com");

    RequestTools::append_x_headers(req, "203.0.113.5", "http");
    EXPECT_EQ(req[http::field::x_forwarded_proto], "http");
}

TEST(AppendXHeaders, ProtoIsHttpsForTlsConnection) {
    http::request<http::string_body> req;
    req.method(http::verb::get);
    req.target("/");
    req.version(11);
    req.set(http::field::host, "public.example.com");

    RequestTools::append_x_headers(req, "203.0.113.5", "https");
    EXPECT_EQ(req[http::field::x_forwarded_proto], "https");
}
