#include <gtest/gtest.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>

#include <string>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

TEST(Scaffold, BeastParsesRequestLine)
{
    std::string raw = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";

    http::request_parser<http::string_body> parser;
    boost::system::error_code ec;
    parser.put(asio::buffer(raw), ec);

    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(parser.get().method(), http::verb::get);
    EXPECT_EQ(parser.get().target(), "/health");
    EXPECT_EQ(parser.get().version(), 11);
}

TEST(Scaffold, AsioIoContextIsUsable)
{
    asio::io_context ioc;
    ioc.restart();
    EXPECT_FALSE(ioc.stopped());

    int counter = 0;
    boost::asio::post(ioc, [&counter] { ++counter; });
    ioc.run();
    EXPECT_EQ(counter, 1);
}
