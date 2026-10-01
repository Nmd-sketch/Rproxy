#include "../include/rproxy/ClientSession.hpp"
#include <iostream>

void ClientSession::start() {
	auto self = shared_from_this();
	m_socket.async_receive(net::buffer(m_aux_buf), net::ip::tcp::socket::message_peek,
	 [self](const sys::error_code& ec, std::size_t bytes) {
			if (ec) {
				std::cerr << "Error receiving request: " << ec.message() << '\n';
				return;
			}

			// 0x16 indicates TLS handshake
			if (bytes > 0 && self->m_aux_buf[0] == 0x16) {
				self->start_tls_handshake();
			}

			// Normal HTTP request
			else {
				self->read_request();
			}
		}
	);
}

void ClientSession::start_tls_handshake() {
	auto self = shared_from_this();
	net::ssl::stream<net::ip::tcp::socket&> ssl_stream(m_socket, m_ssl_ctx);
	sys::error_code ec;

	// Specific error handling for the handshake later
	auto handshake_ec = ssl_stream.handshake(net::ssl::stream_base::server, ec);

	if (ec) {
		std::cerr << "Error during handshake: " << ec.what() << '\n';
		return;
	}


}
