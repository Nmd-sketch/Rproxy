#pragma once
#include "BaseSession.hpp"

class TlsSession : public BaseSession<TlsSession> {
    net::ssl::stream<net::ip::tcp::socket> m_ssl_stream;
    beast::flat_buffer m_buf;

    void do_tls_handshake();
    void read_request();
    void send_response();

public:
    TlsSession(net::ip::tcp::socket&& socket, net::ssl::context& ssl_ctx, Server* s_ref, beast::flat_buffer buf = {}) : BaseSession<TlsSession>(s_ref), 
    m_ssl_stream(std::move(socket), ssl_ctx),
    m_buf(std::move(buf)) {}

    void start();
};
