#pragma once
#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
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
    // - X-Forwarded-For: Set to the client's address. Any client supplied value is overwritten.
    // - X-Forwarded-Proto: Set to https if the client uses TLS, set to http otherwise
    // - X-Forwarded-Host: Set to the original host value from the client request
    // The function does not return any value
    //
    // @Usage: Must be called AFTER make_clean_request, never before it.
    // - headers_lookup_table lists x_forwarded_for among the hop by hop headers, so
    //   make_clean_request erases it. Calling this first means make_clean_request then
    //   deletes the value set here and X-Forwarded-For silently disappears upstream.
    // - Callers must assign to the returned object; this function mutates in place and
    //   returns nothing, so the request passed in is the one that is modified.
    // - client_ip is taken from the downstream socket, not the upstream one.
    // - conn_type is "http" or "https", chosen from whether the client leg negotiated TLS.
    void append_x_headers(http::request<http::string_body>& req, std::string_view client_ip, std::string_view conn_type);


}
