#pragma once
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <iostream>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include "ErrorResponse.hpp"
#include "RequestTools.hpp"
#include "Server.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

template <typename T>
struct is_ssl_stream : std::false_type {};

template <typename NextLayer>
struct is_ssl_stream<net::ssl::stream<NextLayer>> : std::true_type {};

template <typename T>
inline constexpr bool is_ssl_stream_v = is_ssl_stream<T>::value;

template <typename Derived, typename DownStream>
class SessionBase : public std::enable_shared_from_this<Derived> {
protected:
    DownStream m_down;
    beast::flat_buffer m_buf;
    http::request<http::string_body> m_req;
    http::response<http::string_body> m_resp;
    Server* m_serv_ref;
    std::string m_client_ip;
    const HostData* m_upstream;

    // @Usage: Second leg. Built closed and opened by async_connect later, because a
    // tcp::socket has no default constructor and the upstream is not known until the
    // request has been read and routed.
    net::ip::tcp::socket m_up;
    beast::flat_buffer m_up_buf;

    // @Usage: Held as a member because async_connect iterates it after the completion
    // handler returns. A local in the handler would dangle.
    net::ip::tcp::resolver::results_type m_endpoints;

    // @Usage: Beast's steady_timer, used as a deadline. A step times out rather than
    // hanging forever, because an unbounded wait is a denial of service that any peer
    // can trigger: a slowloris client on the client leg, a silent origin on the
    // upstream leg.
    net::steady_timer m_deadline;

    void arm_deadline(std::chrono::milliseconds budget) {
        m_deadline.expires_after(budget);
        m_deadline.async_wait(
            [self = this->shared_from_this()] (const beast::error_code ec) {

                if (ec == net::error::operation_aborted) {
                    return;
                }

                if (ec) {
                    std::cerr << "Deadline timer error: " << ec.message() << '\n';
                    return;
                }

                // @Correctness: The session is shared_ptr owned, and the timer handler
                // holds one reference. Cancelling here lets an in-flight operation
                // finish and deliver its own error, so there is no window where a
                // handler runs against a torn-down session.
                self->m_deadline.cancel();
                std::cerr << "Upstream step timed out\n";
                self->send_error(ErrorResponse::make_error_504(self->m_req.version()));
            }
        );
    }

    void disarm_deadline() {
        m_deadline.cancel();
    }

    void read_request() {
        auto self = this->shared_from_this();

        // @Usage: Bounds a client that connects and then stalls without sending a
        // request, or trickles headers forever. M2 has no header limit yet.
        self->arm_deadline(self->m_serv_ref->config().get_read_ms());

        http::async_read(m_down, m_buf, self->m_req,
            [self] (const beast::error_code ec, std::size_t) {

                if (ec) {
                    // @Usage: A Content-Length plus Transfer-Encoding conflict is a
                    // smuggling attempt, so it is reported distinctly from an ordinary
                    // malformed request. Beast rejects both orders before we get here.
                    const bool framing_conflict =
                        ec == http::error::bad_transfer_encoding ||
                        ec == http::error::bad_content_length ||
                        ec == http::error::multiple_content_length;

                    self->disarm_deadline();
                    std::cerr << "Error parsing the http request (" << ec.message()
                              << "), dropping the connection...\n";

                    // @Correctness: The request may not have parsed far enough to know
                    // the client's version, so 10 is the safest floor to reply with.
                    auto error_response = framing_conflict
                        ? ErrorResponse::make_error_400_framing_conflict(10)
                        : ErrorResponse::make_error_400(10);

                    http::async_write(self->m_down, error_response,
                            [self] (const beast::error_code ec, std::size_t) {

                            if (ec) {
                                std::cerr << "Error sending response\n";
                            }
                        }
                    );
                    return;
                }

                self->connect_upstream();
            }
        );
    }

    void connect_upstream() {
        auto self = this->shared_from_this();

        self->arm_deadline(self->m_serv_ref->config().get_connect_ms());

        if (self->m_upstream == nullptr) {
            std::cerr << "No upstream available\n";
            self->send_error(ErrorResponse::make_error_503(self->m_req.version()));
            return;
        }

        self->m_serv_ref->load_balancer().resolver().async_resolve(
            self->m_upstream->host,
            std::to_string(self->m_upstream->port),
            [self] (const sys::error_code& ec, net::ip::tcp::resolver::results_type results) {

                if (ec) {
                    std::cerr << "Upstream resolve failed: " << ec.message() << '\n';
                    self->disarm_deadline();
                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                self->m_endpoints = std::move(results);

                // @Usage: net::async_connect (the free function), not the socket member, because
                // the range overload lives in <boost/asio/connect.hpp>. It takes
                // results_type by const reference and owns the iteration itself, so
                // m_endpoints must outlive this call, which is why it is a member.
                net::async_connect(self->m_up, self->m_endpoints,
                    [self] (const sys::error_code& ec, const net::ip::tcp::endpoint&) {

                        if (ec) {
                            std::cerr << "Upstream connect failed: " << ec.message() << '\n';
                            self->disarm_deadline();
                            self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                            return;
                        }

                        // @Usage: The connect budget is spent; the read budget takes over
                        // for the forward and relay steps.
                        self->disarm_deadline();
                        self->arm_deadline(self->m_serv_ref->config().get_read_ms());

                        self->write_upstream();
                    }
                );
            }
        );
    }

    void write_upstream() {
        auto self = this->shared_from_this();

        // @Usage: append_x_headers must run AFTER make_clean_request. The hop by hop
        // table contains x_forwarded_for, so the cleaner erases it and running these
        // the other way round silently drops X-Forwarded-For.
        self->m_req = RequestTools::make_clean_request(self->m_req);

        const char* proto = "http";
        if constexpr (is_ssl_stream_v<DownStream>) {
            proto = "https";
        }

        RequestTools::append_x_headers(self->m_req, self->m_client_ip, proto);

        // @Usage: Bounds a client that stalls mid-body. Without it a declared
        // Content-Length that never arrives pins the session indefinitely.
        self->arm_deadline(self->m_serv_ref->config().get_read_ms());

        // @Usage: Required because make_clean_request strips Transfer-Encoding. The
        // parser dechunks without writing a Content-Length back, so a chunked request
        // would otherwise reach the origin with a body and no length header.
        //
        // @Correctness: prepare_payload throws std::invalid_argument for a TRACE
        // carrying a body, which any client can send. Without this catch the throw
        // would leave the completion handler, unwind out of io_context::run() and
        // terminate the whole process, so a single request is a remote kill switch.
        try {
            self->m_req.prepare_payload();
        } catch (const std::invalid_argument& e) {
            std::cerr << "Refusing to forward a request Beast rejects: " << e.what() << '\n';
            self->send_error(ErrorResponse::make_error_400(self->m_req.version()));
            return;
        }

        http::async_write(self->m_up, self->m_req,
            [self] (const beast::error_code ec, std::size_t) {

                if (ec) {
                    self->disarm_deadline();
                    std::cerr << "Error forwarding the request upstream: " << ec.message() << '\n';
                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                self->relay_response();
            }
        );
    }

    void relay_response() {
        auto self = this->shared_from_this();

        // @Usage: A HEAD response legitimately has no body, so the parser must be told
        // to stop at the headers. Without skip(true) it waits for body bytes the origin
        // will never send, and the session hangs until the connection times out.
        // @Usage: A HEAD response legitimately has no body, so the parser must be told
        // to stop at the headers. Without skip(true) it waits for body bytes the origin
        // will never send, and the session hangs until the connection times out.
        //
        // @Correctness: The parser is captured by the handler rather than by
        // reference. async_read outlives this scope, so a reference would dangle, and
        // get() must not be called until the read has completed.
        auto parser = std::make_shared<http::response_parser<http::string_body>>();
        if (self->m_req.method() == http::verb::head) {
            parser->skip(true);
        }

        http::async_read(self->m_up, self->m_up_buf, *parser,
            [self, parser] (const beast::error_code ec, std::size_t) {

                if (ec) {
                    self->disarm_deadline();
                    std::cerr << "Error reading the upstream response: " << ec.message() << '\n';
                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                self->disarm_deadline();
                self->m_resp = parser->get();

                const unsigned result = self->m_resp.result_int();

                // @Usage: A 1xx/204/304 must not carry a Content-Length. RFC 9110 15.3.5
                // forbids it on a 204 and 304, and the parser leaves whatever the
                // origin sent in place, so it has to be stripped before relaying.
                if (result / 100 == 1 || result == 204 || result == 304) {
                    self->m_resp.base().erase(http::field::content_length);
                }

                self->m_resp = RequestTools::make_clean_response(self->m_resp);

                // @Usage: Required because make_clean_response strips
                // Transfer-Encoding. payload_size() is known for string_body, so this
                // re-derives a correct Content-Length.
                //
                // @Correctness: Throws std::invalid_argument if the origin sent a
                // 1xx/204/304 with a body. Catching it here keeps a broken origin from
                // terminating the process by throwing out of io_context::run().
                try {
                    self->m_resp.prepare_payload();
                } catch (const std::invalid_argument& e) {
                    std::cerr << "Upstream response rejected by Beast: " << e.what() << '\n';
                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                // @Correctness: prepare_payload re-adds Content-Length: 0 on a bodiless
                // status, which RFC 9110 forbids. Strip it again, this time after the
                // re-derivation rather than before it.
                if (result / 100 == 1 || result == 204 || result == 304) {
                    self->m_resp.base().erase(http::field::content_length);
                }

                // @Usage: The session handles one request and then closes, so the
                // response has to say so. Without this a keep-alive client waits for
                // bytes that never come.
                self->m_resp.keep_alive(false);

                http::async_write(self->m_down, self->m_resp,
                    [self] (const beast::error_code ec, std::size_t) {

                        if (ec) {
                            // @Correctness: A failed write means the client is already gone or the
                            // connection is broken, so there is no channel left to report this
                            // on. Log it and let the session die rather than attempting another
                            // write to the same dead stream, which cannot succeed either.
                            std::cerr << "Error sending the http response to the client\n";
                            return;
                        }

                        self->m_serv_ref->debug_fn();
                    }
                );
            }
        );
    }

    // @Usage: Every path that reports an error is a terminal path for this session, so
    // the deadline is cancelled here rather than at each call site. Left armed, it would
    // fire later and write a second, stale response over the same socket, which the
    // client's next read would pick up instead of the real reply.
    void send_error(const http::response<http::string_body>& resp) {
        auto self = this->shared_from_this();

        self->disarm_deadline();

        http::async_write(self->m_down, resp,
            [self] (const beast::error_code ec, std::size_t) {

                if (ec) {
                    std::cerr << "Error sending the error response to the client\n";
                }
            }
        );
    }

public:
    SessionBase(DownStream&& down, Server* s_ref, beast::flat_buffer buf = {},
                std::string client_ip = {}, const HostData* upstream = nullptr)
        : m_down(std::move(down)), m_buf(std::move(buf)), m_serv_ref(s_ref),
          m_client_ip(std::move(client_ip)), m_upstream(upstream),
          m_up(s_ref->io_context()), m_deadline(s_ref->io_context()) {}

    void start() {
        if constexpr (is_ssl_stream_v<DownStream>) {
            auto self = this->shared_from_this();

            m_down.async_handshake(net::ssl::stream_base::server, m_buf.data(),
                [self] (const sys::error_code& ec, std::size_t bytes) {

                    if (ec) {
                        std::cerr << "TLS handshake error: " << ec.message() << '\n';
                        return;
                    }

                    self->m_buf.consume(self->m_buf.size());
                    std::cout << "TLS handshake done, reading request\n";

                    self->read_request();
                }
            );
        } else {
            this->read_request();
        }
    }

    Derived* self() noexcept { return static_cast<Derived*>(this); };
};