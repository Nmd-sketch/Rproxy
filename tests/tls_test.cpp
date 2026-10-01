#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>

#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>

#include <type_traits>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;

TEST(Tls, OpenSslCompileTimeAndRuntimeVersionsAgree)
{
    EXPECT_EQ(OPENSSL_VERSION_NUMBER, OpenSSL_version_num());
    EXPECT_STREQ(OPENSSL_VERSION_TEXT, OpenSSL_version(OPENSSL_VERSION));
    EXPECT_GE(OPENSSL_VERSION_MAJOR, 3);
}

TEST(Tls, ContextsConstructForClientAndServer)
{
    ssl::context client{ssl::context::tlsv12_client};
    client.set_default_verify_paths();
    ASSERT_NE(client.native_handle(), nullptr);

    ssl::context server{ssl::context::tlsv12_server};
    ASSERT_NE(server.native_handle(), nullptr);
}

TEST(Tls, SslStreamLayersOverTcpStream)
{
    net::io_context ioc;
    ssl::context ctx{ssl::context::tlsv12_client};

    beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

    ASSERT_NE(stream.native_handle(), nullptr);
    EXPECT_FALSE(beast::get_lowest_layer(stream).socket().is_open());
    EXPECT_EQ(&ioc, &stream.get_executor().context());
}

TEST(Tls, LowestLayerIsTheRawSocket)
{
    using stream_t = beast::ssl_stream<beast::tcp_stream>;

    static_assert(std::is_same_v<stream_t::lowest_layer_type,
                                 boost::asio::ip::tcp::socket>);
    static_assert(std::is_same_v<stream_t::next_layer_type, beast::tcp_stream>);
}
