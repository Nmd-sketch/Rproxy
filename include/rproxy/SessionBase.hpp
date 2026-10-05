#pragma once
#include <memory>
#include <type_traits>
#include <utility>
#include <iostream>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include "ErrorResponse.hpp"
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
    Server* m_serv_ref;

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

                std::cout << "The read handler has been called!\n";

                self->send_response();
            }
        );
    }

    void send_response() noexcept {
        auto self = this->shared_from_this();

        http::response<http::string_body> resp;
        resp.body() = "Request received successfully!";
        resp.set(http::field::content_type, "text/plain");
        resp.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        resp.prepare_payload();

        http::async_write(m_down, resp,
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

public:
    SessionBase(DownStream&& down, Server* s_ref, beast::flat_buffer buf = {})
        : m_down(std::move(down)), m_buf(std::move(buf)), m_serv_ref(s_ref) {}

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
