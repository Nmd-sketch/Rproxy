#pragma once
#include <memory>
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

    void read_request() noexcept {
        auto self = this->shared_from_this();

        http::async_read(m_down, m_buf, self->m_req,
            [self] (const beast::error_code ec, std::size_t) {

                if (ec) {
                    std::cerr << "Error parsing the http request (" << ec.message() << "), dropping the connection...\n";
                    auto error_response = ErrorResponse::make_error_400(10);
                    http::async_write(self->m_down, error_response,
                            [self] (const beast::error_code ec, std::size_t bytes) {

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
                            self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                            return;
                        }

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

        // @Usage: Required because make_clean_request strips Transfer-Encoding. The
        // parser dechunks without writing a Content-Length back, so a chunked request
        // would otherwise reach the origin with a body and no length header.
        self->m_req.prepare_payload();

        http::async_write(self->m_up, self->m_req,
            [self] (const beast::error_code ec, std::size_t) {

                if (ec) {
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

        http::async_read(self->m_up, self->m_up_buf, self->m_resp,
            [self] (const beast::error_code ec, std::size_t) {

                if (ec) {
                    std::cerr << "Error reading the upstream response: " << ec.message() << '\n';
                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                const unsigned result = self->m_resp.result_int();

                // @Usage: A 1xx/204/304 must carry no body. prepare_payload throws
                // std::invalid_argument for one that does, and after the cleaner has
                // stripped its framing header there is nothing sane to relay, so the
                // origin is reported as broken rather than trusted.
                if ((result / 100 == 1 || result == 204 || result == 304) &&
                    !self->m_resp.body().empty()) {
                    std::cerr << "Upstream sent a bodiless status (" << result
                              << ") with a body\n";
                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                self->m_resp = RequestTools::make_clean_response(self->m_resp);

                // @Usage: Required because make_clean_response strips
                // Transfer-Encoding. payload_size() is known for string_body, so this
                // re-derives a correct Content-Length. Throws for the bodiless
                // statuses, which is why these flow functions are not noexcept.
                self->m_resp.prepare_payload();

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

    void send_error(const http::response<http::string_body>& resp) {
        auto self = this->shared_from_this();

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
          m_up(s_ref->io_context()) {}

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