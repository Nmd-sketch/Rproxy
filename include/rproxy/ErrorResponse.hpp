#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include <cstddef>
#include <string_view>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

// Every factory takes the client's HTTP version so the reply does not
// claim HTTP/1.1 to a client that spoke HTTP/1.0. When the request was
// too malformed to read a version, pass http::version::11.
namespace ErrorResponse {


http::response<http::string_body> make_error_400(unsigned version);

http::response<http::string_body> make_error_400_framing_conflict(unsigned version);

http::response<http::string_body> make_error_413(std::size_t max_payload_size_bytes,
                                                 unsigned version);

http::response<http::string_body> make_error_431(std::string_view header_name,
                                                 unsigned version);

http::response<http::string_body> make_error_500(unsigned version);

http::response<http::string_body> make_error_502(unsigned version);

http::response<http::string_body> make_error_503(unsigned version);

http::response<http::string_body> make_error_504(unsigned version);

}  // namespace ErrorResponse
