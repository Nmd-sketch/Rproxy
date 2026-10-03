#include "../include/rproxy/TlsSession.hpp"
#include <iostream>

void TlsSession::start() {
    self()->do_tls_handshake();
}

void TlsSession::do_tls_handshake() {
    auto shared_self = shared_from_this();
    
    // Start the async handshake
    m_ssl_stream.async_handshake(net::ssl::stream_base::server, m_buf.data(),
           [this, shared_self] (const sys::error_code& ec, std::size_t bytes) {
                 
                if (ec) {
                    std::cerr << "TLS handshake error: " << ec.message() << '\n';
                    return;
                }

                m_buf.consume(m_buf.size());
                std::cout << "TLS handshake done, reading request\n";

                shared_self->read_request();
            }
    );
}

void TlsSession::read_request() {
    auto shared_self = shared_from_this();

    http::async_read(m_ssl_stream, m_buf, self()->m_req, 
            [this, shared_self] (const beast::error_code ec, std::size_t bytes_read) {
                
                if (ec) {
                    std::cerr << "TLS async_read error: " << ec.message() << " (" << ec.category().name() << ")\n";
                    return;
                }

                std::cout << "The TLS read handler has been called!\n";

                shared_self->send_response();
            }
    );
}

void TlsSession::send_response() {
    auto shared_self = shared_from_this();
	http::response<http::string_body> resp;
	resp.body() = "Ssl request received successfully!";
	resp.set(http::field::content_type, "text/plain");
	resp.set(http::field::server, BOOST_BEAST_VERSION_STRING);
	resp.prepare_payload();

    http::async_write(self()->m_ssl_stream, resp, 
            [shared_self] (const beast::error_code ec, std::size_t bytes) {
                
                if (ec) {
                    std::cerr << "Error sending the http request to the client\n";
                }
            }
    );
}

