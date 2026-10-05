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
// too malformed to read a version, pass 10, which equals to HTTP/1.0.
namespace ErrorResponse {

    // @Usage: Malformed request that could not be parsed at all, for example an
    // unparseable request line or a header that failed validation. Prefer this over
    // make_error_400_framing_conflict whenever the cause is not specifically an
    // ambiguous body length.
    http::response<http::string_body> make_error_400(unsigned version);

    // @Usage: The request declared both Content-Length and Transfer-Encoding, so the
    // body length can be read two different ways. This is a request smuggling
    // attempt, not a syntax error, so log it as a security event rather than a
    // malformed request. Send this instead of make_error_400 so the two causes stay
    // distinguishable in logs.
    http::response<http::string_body> make_error_400_framing_conflict(unsigned version);

    // @Usage: The request body exceeded limits.body_bytes from the configuration.
    // Pass the configured limit so the client knows what to split its upload into;
    // do not pass the size actually received, which leaks nothing useful and stops
    // the client from correcting itself.
    http::response<http::string_body> make_error_413(std::size_t max_payload_size_bytes,
                                                     unsigned version);

    // @Usage: The request headers exceeded limits.header_bytes, either in total or
    // in one field. Pass the offending field name when it is known so the client can
    // find it; pass a placeholder such as "request headers" when it is not, which is
    // the common case.
    http::response<http::string_body> make_error_431(std::string_view header_name,
                                                     unsigned version);

    // @Usage: A bug in the proxy itself, such as an unexpected exception or an
    // internal invariant that did not hold. Never use this for a failure caused by
    // the client or by the origin, since that misleads both of them: a client that
    // retries will not fix its own request, and the origin is not at fault.
    http::response<http::string_body> make_error_500(unsigned version);

    // @Usage: The upstream leg could not be reached or spoke something that was not
    // a valid HTTP response, for example connection refused, a TLS handshake failure
    // against the origin, or a malformed response from it. This is the standard
    // "the origin is down or misbehaving" reply and is the most common error this
    // proxy will produce.
    http::response<http::string_body> make_error_502(unsigned version);

    // @Usage: The proxy is up but cannot serve the request right now. Use it when
    // there is no healthy upstream left to choose from, such as an empty upstream
    // list or every upstream having been marked unhealthy. Retry-After may be added
    // later once backoff policy exists.
    http::response<http::string_body> make_error_503(unsigned version);

    // @Usage: The upstream leg was reached but did not produce a response within
    // timeouts.read_ms. Only usable once timeouts exist; until then a slow origin
    // hangs the connection instead of producing this.
    http::response<http::string_body> make_error_504(unsigned version);

} 
