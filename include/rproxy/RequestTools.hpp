#pragma once
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include <array>
#include <unordered_set>
#include <string_view>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

namespace RequestTools {

    // @Preconditions: A valid http request is passed to the function
    // @Postconditions: A valid http request is returned, with hop-by-hop headers removed
    // and any aditional headers in the connection header's value removed.
    http::request<http::string_body> make_clean_request(http::request<http::string_body> req);

    // @Preconditions: A valid http response is passed to the function
    // @Postconditions: A valid http response is returned, with hop-by-hop headers removed and any
    // aditional headers in the connection header's value are removed.
    http::response<http::string_body> make_clean_response(http::response<http::string_body> resp);

    // @Preconditions: A valid http request is passed to the function
    // @Postconditions: A valid http request is returned with the following headers set:
    // - X-Forwarded-For: Appends the client's address. If the header already exists, the new ip is appended
    // - X-Forwarded-Proto: Set to https if the client uses TLS, set to http otherwise
    // - X-Forwarded-Host: Set to the original host value from the client request
    // The function does not return any value
    void append_x_headers(http::request<http::string_body>& req, std::string_view client_ip, std::string_view conn_type);


}
